/* Winsock and threads under QRX86: a server thread echoes what a client sends over real host
 * TCP on the loopback address, while the main thread also talks to itself over UDP. */
#include <winsock.h>
#include <stdio.h>
#include <string.h>

#define PORT 47123
#define BYTES (512 * 1024)
#define CHUNK 8192
static unsigned char sendbuf[CHUNK], recvbuf[CHUNK];
static volatile LONG server_ready;
static DWORD server_total;

static DWORD WINAPI server(void *arg)
{
    (void)arg;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0), c;
    struct sockaddr_in sa;
    char buf[CHUNK];
    int n;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ls == INVALID_SOCKET || bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 1)) {
        printf("server setup failed: %d\n", WSAGetLastError());
        server_ready = -1;
        return 1;
    }
    server_ready = 1;
    c = accept(ls, NULL, NULL);
    if (c == INVALID_SOCKET) { printf("accept failed: %d\n", WSAGetLastError()); return 1; }
    while ((n = recv(c, buf, sizeof buf, 0)) > 0) {
        int off = 0;
        server_total += (DWORD)n;
        while (off < n) {
            int s = send(c, buf + off, n - off, 0);
            if (s <= 0) return 1;
            off += s;
        }
    }
    closesocket(c);
    closesocket(ls);
    return 0;
}

/* Two threads send at the same time, each on its own connection, while nobody reads: both
 * buffers fill up and both calls wait inside send. The host side then reads slowly, so they keep
 * waiting. Every byte depends on the sender and its position, so data from the other thread,
 * lost or repeated data all show up as wrong bytes. */
#define RACE_PORT (PORT + 2)
#define RACE_BYTES (2048 * 1024)      /* macOS takes about 400 KB on loopback, even with RACE_SOCKBUF */
#define RACE_SOCKBUF 4096
#define RACE_READ 4096
typedef struct racer {
    SOCKET out, in;
    unsigned seed;
    volatile DWORD sent;
    DWORD got, wrong, first_wrong;
    int error, closed;
    HANDLE thread;
} racer;

static unsigned char race_byte(unsigned seed, DWORD off)
{
    return (unsigned char)(off * 7u + (off >> 10) + seed);
}
static DWORD WINAPI race_sender(void *arg)
{
    racer *r = arg;
    char buf[CHUNK];
    while (r->sent < RACE_BYTES) {
        DWORD left = RACE_BYTES - r->sent;
        int i, n = left < CHUNK ? (int)left : CHUNK, s;
        for (i = 0; i < n; i++) buf[i] = (char)race_byte(r->seed, r->sent + (DWORD)i);
        s = send(r->out, buf, n, 0);
        if (s <= 0) { r->error = WSAGetLastError(); return 1; }
        r->sent += (DWORD)s;
    }
    closesocket(r->out);
    return 0;
}
/* Nonzero when the test failed. */
static int two_senders_on_full_buffers(void)
{
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    racer r[2];
    DWORD id, start, last[2] = { 0, 0 }, code[2] = { 99, 99 };
    int i, quiet = 0, size = RACE_SOCKBUF, bad = 0;
    u_long on = 1;
    memset(&sa, 0, sizeof sa);
    memset(r, 0, sizeof r);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(RACE_PORT);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    /* An accepted socket takes its receive buffer from the listening one. */
    setsockopt(ls, SOL_SOCKET, SO_RCVBUF, (char *)&size, sizeof size);
    if (ls == INVALID_SOCKET || bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 2)) {
        printf("race: setup failed: %d\n", WSAGetLastError());
        return 1;
    }
    for (i = 0; i < 2; i++) {
        r[i].seed = i ? 0x80u : 0x01u;
        r[i].out = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(r[i].out, SOL_SOCKET, SO_SNDBUF, (char *)&size, sizeof size);
        if (connect(r[i].out, (struct sockaddr *)&sa, sizeof sa)) { printf("race: connect failed: %d\n", WSAGetLastError()); return 1; }
        r[i].in = accept(ls, NULL, NULL);
        if (r[i].in == INVALID_SOCKET) { printf("race: accept failed: %d\n", WSAGetLastError()); return 1; }
        ioctlsocket(r[i].in, FIONBIO, &on);
    }
    closesocket(ls);
    for (i = 0; i < 2; i++) r[i].thread = CreateThread(NULL, 0, race_sender, &r[i], 0, &id);
    /* The test proves nothing unless both really wait with data still to send. */
    start = GetTickCount();
    while (quiet < 10) {
        DWORD a, b;
        Sleep(10);
        a = r[0].sent; b = r[1].sent;
        quiet = a == last[0] && b == last[1] ? quiet + 1 : 0;
        last[0] = a; last[1] = b;
        if (a >= RACE_BYTES || b >= RACE_BYTES || GetTickCount() - start > 20000) {
            printf("race: the buffers did not fill up, sent %lu and %lu\n", (unsigned long)a, (unsigned long)b);
            return 1;
        }
    }
    printf("race: both senders wait, %lu and %lu of %d bytes sent\n", (unsigned long)last[0], (unsigned long)last[1], RACE_BYTES);
    start = GetTickCount();
    while (!r[0].closed || !r[1].closed) {
        static unsigned char buf[RACE_READ];
        for (i = 0; i < 2; i++) {
            int n, k;
            if (r[i].closed) continue;
            n = recv(r[i].in, (char *)buf, sizeof buf, 0);
            if (n == 0) { r[i].closed = 1; continue; }
            if (n < 0) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
                printf("race: recv on connection %d failed: %d\n", i, WSAGetLastError());
                r[i].closed = 1;
                bad = 1;
                continue;
            }
            for (k = 0; k < n; k++) {
                if (buf[k] != race_byte(r[i].seed, r[i].got + (DWORD)k)) {
                    if (!r[i].wrong) r[i].first_wrong = r[i].got + (DWORD)k;
                    r[i].wrong++;
                }
            }
            r[i].got += (DWORD)n;
        }
        Sleep(1);                     /* time for the senders to fill the buffers again */
        if (GetTickCount() - start > 60000) { puts("race: timed out"); bad = 1; break; }
    }
    for (i = 0; i < 2; i++) {
        WaitForSingleObject(r[i].thread, bad ? 0 : INFINITE);
        GetExitCodeThread(r[i].thread, &code[i]);
        closesocket(r[i].in);
        printf("race: connection %d: sent %lu, received %lu, %lu wrong", i, (unsigned long)r[i].sent, (unsigned long)r[i].got, (unsigned long)r[i].wrong);
        if (r[i].wrong) printf(", first at %lu", (unsigned long)r[i].first_wrong);
        printf(", thread exit %lu, error %d\n", (unsigned long)code[i], r[i].error);
        if (r[i].wrong || r[i].got != RACE_BYTES || r[i].sent != RACE_BYTES || code[i]) bad = 1;
    }
    return bad;
}

/* The last error belongs to the thread. Two threads fail over and over, each with a Winsock error
 * and a SetLastError value of its own, and each must read back its own. hle_hook switches threads
 * only after every 32nd API call, so a loop of 2 or 4 calls could always switch at the same place
 * and never hit the gap: this one makes 5. A switch right after the failing call shows up as
 * err_runner changing there; the test proves nothing without one, so it needs at least one. */
#define ERR_RUN_MS 1500u
#define WSAENOTSOCK_EXPECTED 10038
typedef struct errthread {
    SOCKET s;
    int want_wsa;
    DWORD want_win;
    DWORD rounds, wrong_wsa, wrong_win, gap_wsa, gap_win;
    int seen_wsa;
    DWORD seen_win, start_win;
    int start_wsa;
    HANDLE thread;
} errthread;
static volatile errthread *err_runner;
static DWORD err_end;

static DWORD WINAPI err_worker(void *arg)
{
    errthread *t = arg;
    char c;
    t->start_wsa = WSAGetLastError();
    t->start_win = GetLastError();
    do {
        volatile errthread *after;
        int wsa;
        DWORD win;
        err_runner = t;
        recv(t->s, &c, 1, 0);
        after = err_runner;
        wsa = WSAGetLastError();
        if (after != t) t->gap_wsa++;
        if (wsa != t->want_wsa) { if (!t->wrong_wsa) t->seen_wsa = wsa; t->wrong_wsa++; }
        err_runner = t;
        SetLastError(t->want_win);
        after = err_runner;
        win = GetLastError();
        if (after != t) t->gap_win++;
        if (win != t->want_win) { if (!t->wrong_win) t->seen_win = win; t->wrong_win++; }
        t->rounds++;
    } while (GetTickCount() - err_end > 0x80000000u);
    return 0;
}
/* Nonzero when the test failed. */
static int last_error_per_thread(void)
{
    errthread t[2];
    struct sockaddr_in sa;
    u_long on = 1;
    DWORD id;
    int i, bad = 0;
    memset(t, 0, sizeof t);
    /* Bound, without data and nonblocking: recv always says WSAEWOULDBLOCK. */
    t[0].s = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (t[0].s == INVALID_SOCKET || bind(t[0].s, (struct sockaddr *)&sa, sizeof sa) || ioctlsocket(t[0].s, FIONBIO, &on)) {
        printf("lasterror: setup failed: %d\n", WSAGetLastError());
        return 1;
    }
    t[0].want_wsa = WSAEWOULDBLOCK;
    t[0].want_win = 1111;
    /* A socket that was never opened: recv always says WSAENOTSOCK. */
    t[1].s = 63;
    t[1].want_wsa = WSAENOTSOCK_EXPECTED;
    t[1].want_win = 2222;
    err_end = GetTickCount() + ERR_RUN_MS;
    for (i = 0; i < 2; i++) t[i].thread = CreateThread(NULL, 0, err_worker, &t[i], 0, &id);
    for (i = 0; i < 2; i++) WaitForSingleObject(t[i].thread, INFINITE);
    closesocket(t[0].s);
    for (i = 0; i < 2; i++) {
        printf("lasterror: thread %d: %lu rounds, Winsock error %d: %lu wrong", i, (unsigned long)t[i].rounds, t[i].want_wsa, (unsigned long)t[i].wrong_wsa);
        if (t[i].wrong_wsa) printf(" (first read %d)", t[i].seen_wsa);
        printf(", %lu switches in the gap; SetLastError %lu: %lu wrong", (unsigned long)t[i].gap_wsa, (unsigned long)t[i].want_win, (unsigned long)t[i].wrong_win);
        if (t[i].wrong_win) printf(" (first read %lu)", (unsigned long)t[i].seen_win);
        printf(", %lu switches in the gap; started with %d and %lu\n", (unsigned long)t[i].gap_win, t[i].start_wsa, (unsigned long)t[i].start_win);
        if (t[i].wrong_wsa || t[i].wrong_win || t[i].start_wsa || t[i].start_win) bad = 1;
        if (!t[i].gap_wsa || !t[i].gap_win) { puts("lasterror: no thread switch in the gap, so this run proves nothing"); bad = 1; }
    }
    return bad;
}
int main(void)
{
    WSADATA d;
    struct sockaddr_in sa;
    SOCKET s;
    HANDLE th;
    DWORD id, t0, t1, code = 99, got = 0, sent = 0;
    int i, bad = 0;
    if (WSAStartup(MAKEWORD(1, 1), &d)) { puts("WSAStartup failed"); return 1; }
    th = CreateThread(NULL, 0, server, NULL, 0, &id);
    while (!server_ready) Sleep(1);
    if (server_ready < 0) return 2;
    s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr *)&sa, sizeof sa)) { printf("connect failed: %d\n", WSAGetLastError()); return 3; }
    t0 = GetTickCount();
    while (got < BYTES) {
        for (i = 0; i < CHUNK; i++) sendbuf[i] = (unsigned char)((sent + (DWORD)i) * 7u);
        int n = send(s, (char *)sendbuf, CHUNK, 0);
        if (n != CHUNK) { printf("send: %d err %d\n", n, WSAGetLastError()); return 4; }
        sent += (DWORD)n;
        DWORD want = sent;
        while (got < want) {
            int r = recv(s, (char *)recvbuf, CHUNK, 0);
            if (r <= 0) { printf("recv: %d err %d\n", r, WSAGetLastError()); return 5; }
            for (i = 0; i < r; i++) if (recvbuf[i] != (unsigned char)((got + (DWORD)i) * 7u)) bad++;
            got += (DWORD)r;
        }
    }
    t1 = GetTickCount();
    closesocket(s);
    WaitForSingleObject(th, INFINITE);
    GetExitCodeThread(th, &code);
    printf("TCP: %lu bytes echoed, %d wrong, server saw %lu, thread exit %lu, %lu ms\n",
           (unsigned long)got, bad, (unsigned long)server_total, (unsigned long)code, (unsigned long)(t1 - t0));
    /* UDP to itself */
    {
        SOCKET u = socket(AF_INET, SOCK_DGRAM, 0);
        char b[16];
        int sl = sizeof sa, r;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(PORT + 1);
        sa.sin_addr.s_addr = inet_addr("127.0.0.1");
        bind(u, (struct sockaddr *)&sa, sizeof sa);
        sendto(u, "qrx", 4, 0, (struct sockaddr *)&sa, sizeof sa);
        fd_set rs;
        struct timeval tv = { 2, 0 };
        FD_ZERO(&rs);
        FD_SET(u, &rs);
        r = select(0, &rs, NULL, NULL, &tv);
        printf("UDP: select %d, ", r);
        r = recvfrom(u, b, sizeof b, 0, (struct sockaddr *)&sa, &sl);
        printf("received %d bytes '%s' from %s\n", r, r > 0 ? b : "", inet_ntoa(sa.sin_addr));
        closesocket(u);
        if (r != 4 || strcmp(b, "qrx")) bad++;
    }
    /* A refused connection must report the right error. */
    {
        SOCKET r = socket(AF_INET, SOCK_STREAM, 0);
        sa.sin_port = htons(47199);
        int e = connect(r, (struct sockaddr *)&sa, sizeof sa);
        printf("refused: connect %d, error %d\n", e, WSAGetLastError());
        if (e == 0 || WSAGetLastError() != 10061) bad++;
        closesocket(r);
    }
    bad += two_senders_on_full_buffers();
    bad += last_error_per_thread();
    WSACleanup();
    puts(bad || got != BYTES || server_total != BYTES || code ? "NETTEST FAILED" : "NETTEST PASSED");
    return bad != 0;
}
