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
    WSACleanup();
    puts(bad || got != BYTES || server_total != BYTES || code ? "NETTEST FAILED" : "NETTEST PASSED");
    return bad != 0;
}
