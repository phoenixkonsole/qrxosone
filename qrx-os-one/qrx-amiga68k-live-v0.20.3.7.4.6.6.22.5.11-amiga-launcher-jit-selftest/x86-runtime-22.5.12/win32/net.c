/* QRX OS One - qrx-win32: Winsock on the host's TCP/IP through the ARMithlon door.
 * Host sockets never block the emulator; a blocking guest call polls and lets other guest
 * threads run. Without the door every call fails with WSAENETDOWN. */
#include "qrxwin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FN(name) static uint32_t name(qwin *w, uint32_t *a)
#define C (&w->cpu)
#define RD32(x) x86_rd32(C, (x))
#define WR32(x, v) x86_wr32(C, (x), (v))
#define RD16(x) x86_rd16(C, (x))
#define WR16(x, v) x86_wr16(C, (x), (v))
#define RD8(x) x86_rd8(C, (x))
#define WR8(x, v) x86_wr8(C, (x), (v))
#define UNUSED (void)w; (void)a
#define DOOR_SVC_NET 4u
#define SOCKET_ERROR 0xFFFFFFFFu
#define WSAEWOULDBLOCK 10035u
#define WSAENETDOWN 10050u
#define MAXSOCK 64

static int g_nonblock[MAXSOCK];
/* q[3]=op, q[4..11]=arguments, q[12]=result, q[13]=error, q[14]=ip out, q[15]=port out */
static int net_call(qwin *w, uint32_t *q, uint32_t op)
{
    q[0] = DOOR_SVC_NET;
    q[3] = op;
    q[12] = SOCKET_ERROR;
    q[13] = WSAENETDOWN;
    if (!C->door || !x86_door_call(C, q)) { w->net_error = WSAENETDOWN; return 0; }
    if (q[12] == SOCKET_ERROR) { w->net_error = q[13]; return 0; }
    return 1;
}
int qw_net_call(qwin *w, uint32_t *q, uint32_t op) { return net_call(w, q, op); }
static int blocking(uint32_t s) { return s < MAXSOCK && !g_nonblock[s]; }

/* sockaddr_in in guest memory: u16 family, be16 port, be32 addr */
static void rd_addr(qwin *w, uint32_t p, uint32_t *ip, uint32_t *port)
{
    *port = (uint32_t)RD8(p + 2) << 8 | RD8(p + 3);
    *ip = (uint32_t)RD8(p + 4) << 24 | (uint32_t)RD8(p + 5) << 16 | (uint32_t)RD8(p + 6) << 8 | RD8(p + 7);
}
static void wr_addr(qwin *w, uint32_t p, uint32_t lenp, uint32_t ip, uint32_t port)
{
    uint32_t i;
    if (!p) return;
    WR16(p, 2);
    WR8(p + 2, (uint8_t)(port >> 8)); WR8(p + 3, (uint8_t)port);
    WR8(p + 4, (uint8_t)(ip >> 24)); WR8(p + 5, (uint8_t)(ip >> 16)); WR8(p + 6, (uint8_t)(ip >> 8)); WR8(p + 7, (uint8_t)ip);
    for (i = 8; i < 16; i++) WR8(p + i, 0);
    if (lenp) WR32(lenp, 16);
}
FN(n_WSAStartup) {
    uint32_t i;
    if (a[1]) {
        for (i = 0; i < 400; i++) WR8(a[1] + i, 0);
        WR16(a[1], 0x0101u); WR16(a[1] + 2, 0x0202u);
        for (i = 0; "QRX host sockets"[i]; i++) WR8(a[1] + 4 + i, (uint8_t)"QRX host sockets"[i]);
        WR16(a[1] + 390, 64);
        WR16(a[1] + 392, 1500);
    }
    return 0;
}
FN(n_ok0) { UNUSED; return 0; }
FN(n_WSAGetLastError) { UNUSED; return w->net_error; }
FN(n_WSASetLastError) { w->net_error = a[0]; return 0; }
FN(n_socket) {
    static uint32_t q[16];
    q[4] = a[1]; q[5] = a[2];
    if (!net_call(w, q, NET_SOCKET)) return SOCKET_ERROR;
    if (q[12] < MAXSOCK) g_nonblock[q[12]] = 0;
    return q[12];
}
FN(n_closesocket) { static uint32_t q[16]; q[4] = a[0]; return net_call(w, q, NET_CLOSE) ? 0 : SOCKET_ERROR; }
FN(n_shutdown) { static uint32_t q[16]; q[4] = a[0]; q[5] = a[1]; return net_call(w, q, NET_SHUTDOWN) ? 0 : SOCKET_ERROR; }
FN(n_bind) {
    static uint32_t q[16];
    q[4] = a[0];
    rd_addr(w, a[1], &q[5], &q[6]);
    return net_call(w, q, NET_BIND) ? 0 : SOCKET_ERROR;
}
FN(n_listen) { static uint32_t q[16]; q[4] = a[0]; q[5] = a[1]; return net_call(w, q, NET_LISTEN) ? 0 : SOCKET_ERROR; }
FN(n_accept) {
    static uint32_t q[16];
    uint32_t s = a[0];
    for (;;) {
        q[4] = s;
        if (net_call(w, q, NET_ACCEPT)) {
            wr_addr(w, a[1], a[2], q[14], q[15]);
            if (q[12] < MAXSOCK) g_nonblock[q[12]] = 0;
            return q[12];
        }
        if (w->net_error != WSAEWOULDBLOCK || !blocking(s) || w->exiting) return SOCKET_ERROR;
        qw_thread_block(w, 2);
    }
}
FN(n_connect) {
    static uint32_t q[16];
    uint32_t s = a[0], ip, port;
    rd_addr(w, a[1], &ip, &port);
    q[4] = s; q[5] = ip; q[6] = port;
    if (net_call(w, q, NET_CONNECT)) return 0;
    if (w->net_error != 10036u && w->net_error != WSAEWOULDBLOCK) return SOCKET_ERROR;   /* in progress */
    if (!blocking(s)) { w->net_error = WSAEWOULDBLOCK; return SOCKET_ERROR; }
    for (;;) {
        q[4] = s; q[5] = 2;       /* writable means finished; q[12] holds the ready mask, q[13] SO_ERROR */
        if (!net_call(w, q, NET_POLL)) return SOCKET_ERROR;
        if (q[12] & 2u) {
            if (q[13]) { w->net_error = q[13]; return SOCKET_ERROR; }
            return 0;
        }
        if (w->exiting) return SOCKET_ERROR;
        qw_thread_block(w, 2);
    }
}
/* One chunk through a buffer in 68k RAM that the host can reach without x86 page walks.
 * All threads share it and it is valid only until the next switch: qw_thread_block lets others
 * run, so fill it right before the door call and empty it right after, as q is refilled too. */
#define NET_CHUNK 16384u
static uint8_t g_buf[NET_CHUNK];
FN(n_send) {
    static uint32_t q[16];
    uint32_t s = a[0], n = a[2] < NET_CHUNK ? a[2] : NET_CHUNK;
    for (;;) {
        x86_read_mem(C, a[1], g_buf, n);
        q[4] = s; q[5] = (uint32_t)(uintptr_t)g_buf; q[6] = n; q[7] = a[3];
        if (net_call(w, q, NET_SEND)) return q[12];
        if (w->net_error != WSAEWOULDBLOCK || !blocking(s) || w->exiting) return SOCKET_ERROR;
        qw_thread_block(w, 2);
    }
}
FN(n_recv) {
    static uint32_t q[16];
    uint32_t s = a[0], n = a[2] < NET_CHUNK ? a[2] : NET_CHUNK;
    for (;;) {
        q[4] = s; q[5] = (uint32_t)(uintptr_t)g_buf; q[6] = n; q[7] = a[3];
        if (net_call(w, q, NET_RECV)) { x86_write_mem(C, a[1], g_buf, q[12]); return q[12]; }
        if (w->net_error != WSAEWOULDBLOCK || !blocking(s) || w->exiting) return SOCKET_ERROR;
        qw_thread_block(w, 2);
    }
}
FN(n_sendto) {
    static uint32_t q[16];
    uint32_t s = a[0], n = a[2] < NET_CHUNK ? a[2] : NET_CHUNK;
    x86_read_mem(C, a[1], g_buf, n);
    q[4] = s; q[5] = (uint32_t)(uintptr_t)g_buf; q[6] = n; q[7] = a[3];
    rd_addr(w, a[4], &q[8], &q[9]);
    return net_call(w, q, NET_SENDTO) ? q[12] : SOCKET_ERROR;
}
FN(n_recvfrom) {
    static uint32_t q[16];
    uint32_t s = a[0], n = a[2] < NET_CHUNK ? a[2] : NET_CHUNK;
    for (;;) {
        q[4] = s; q[5] = (uint32_t)(uintptr_t)g_buf; q[6] = n; q[7] = a[3];
        if (net_call(w, q, NET_RECVFROM)) {
            x86_write_mem(C, a[1], g_buf, q[12]);
            wr_addr(w, a[4], a[5], q[14], q[15]);
            return q[12];
        }
        if (w->net_error != WSAEWOULDBLOCK || !blocking(s) || w->exiting) return SOCKET_ERROR;
        qw_thread_block(w, 2);
    }
}
/* fd_set: u32 count, then the sockets. */
static uint32_t set_poll(qwin *w, uint32_t set, uint32_t mask, uint32_t *out, uint32_t *nout)
{
    static uint32_t q[16];
    uint32_t i, n, ready = 0;
    if (!set) return 0;
    n = RD32(set);
    if (n > 64) n = 64;
    for (i = 0; i < n; i++) {
        uint32_t s = RD32(set + 4 + i * 4);
        q[4] = s; q[5] = mask;
        if (net_call(w, q, NET_POLL) && (q[12] & mask)) { out[ready++] = s; }
    }
    *nout = ready;
    return ready;
}
static void set_store(qwin *w, uint32_t set, const uint32_t *v, uint32_t n)
{
    uint32_t i;
    if (!set) return;
    WR32(set, n);
    for (i = 0; i < n; i++) WR32(set + 4 + i * 4, v[i]);
}
FN(n_select) {
    uint32_t rv[64], wv[64], ev[64], rn = 0, wn = 0, en = 0;
    uint64_t end = 0;
    int forever = !a[4];
    if (a[4]) end = w->be->now(w) + (uint64_t)RD32(a[4]) * 1000u + RD32(a[4] + 4) / 1000u;
    for (;;) {
        uint32_t total = set_poll(w, a[1], 1, rv, &rn) + set_poll(w, a[2], 2, wv, &wn) + set_poll(w, a[3], 4, ev, &en);
        if (total || w->exiting || (!forever && w->be->now(w) >= end)) {
            set_store(w, a[1], rv, rn); set_store(w, a[2], wv, wn); set_store(w, a[3], ev, en);
            return total;
        }
        qw_thread_block(w, 2);
    }
}
FN(n_WSAFDIsSet) {
    uint32_t n = RD32(a[1]), i;
    for (i = 0; i < n && i < 64; i++) if (RD32(a[1] + 4 + i * 4) == a[0]) return 1;
    return 0;
}
FN(n_ioctlsocket) {
    if (a[1] == 0x8004667Eu) {       /* FIONBIO */
        if (a[0] < MAXSOCK) g_nonblock[a[0]] = RD32(a[2]) != 0;
        return 0;
    }
    if (a[1] == 0x4004667Fu) {       /* FIONREAD */
        static uint32_t q[16];
        q[4] = a[0]; q[5] = 8;
        if (!net_call(w, q, NET_POLL)) return SOCKET_ERROR;
        WR32(a[2], q[14]);
        return 0;
    }
    w->net_error = 10045;
    return SOCKET_ERROR;
}
FN(n_setsockopt) {
    static uint32_t q[16];
    q[4] = a[0]; q[5] = a[1]; q[6] = a[2]; q[7] = a[3] && a[4] >= 4 ? RD32(a[3]) : (a[3] ? RD8(a[3]) : 0);
    return net_call(w, q, NET_SETOPT) ? 0 : SOCKET_ERROR;
}
FN(n_getsockopt) {
    static uint32_t q[16];
    q[4] = a[0]; q[5] = a[1]; q[6] = a[2];
    if (!net_call(w, q, NET_GETOPT)) return SOCKET_ERROR;
    if (a[3]) WR32(a[3], q[14]);
    if (a[4]) WR32(a[4], 4);
    return 0;
}
FN(n_getsockname) { static uint32_t q[16]; q[4] = a[0]; if (!net_call(w, q, NET_SOCKNAME)) return SOCKET_ERROR; wr_addr(w, a[1], a[2], q[14], q[15]); return 0; }
FN(n_getpeername) { static uint32_t q[16]; q[4] = a[0]; if (!net_call(w, q, NET_PEERNAME)) return SOCKET_ERROR; wr_addr(w, a[1], a[2], q[14], q[15]); return 0; }
FN(n_htons) { (void)w; return (a[0] & 0xFFu) << 8 | (a[0] >> 8 & 0xFFu); }
FN(n_htonl) { (void)w; return a[0] << 24 | (a[0] & 0xFF00u) << 8 | (a[0] >> 8 & 0xFF00u) | a[0] >> 24; }
FN(n_inet_addr) {
    unsigned b[4];
    char s[32];
    int i;
    for (i = 0; i < 31; i++) { s[i] = (char)RD8(a[0] + (uint32_t)i); if (!s[i]) break; }
    s[i] = 0;
    if (sscanf(s, "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) != 4) return 0xFFFFFFFFu;
    return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
}
FN(n_inet_ntoa) {
    static uint32_t p;
    char s[20];
    int i;
    if (!p) p = qw_alloc(w, 20);
    snprintf(s, sizeof(s), "%u.%u.%u.%u", a[0] & 255, a[0] >> 8 & 255, a[0] >> 16 & 255, a[0] >> 24 & 255);
    for (i = 0; ; i++) { WR8(p + (uint32_t)i, (uint8_t)s[i]); if (!s[i]) break; }
    return p;
}
FN(n_gethostname) {
    static uint32_t q[16];
    int i;
    q[5] = (uint32_t)(uintptr_t)g_buf; q[6] = 128;
    if (!net_call(w, q, NET_HOSTNAME)) return SOCKET_ERROR;
    for (i = 0; (uint32_t)i + 1 < a[1] && g_buf[i]; i++) WR8(a[0] + (uint32_t)i, g_buf[i]);
    WR8(a[0] + (uint32_t)i, 0);
    return 0;
}
FN(n_gethostbyname) {
    /* struct hostent { char *name; char **aliases; short type, length; char **addrs; } */
    static uint32_t he;
    static uint32_t q[16];
    int i;
    for (i = 0; i < 255; i++) { g_buf[i] = (uint8_t)RD8(a[0] + (uint32_t)i); if (!g_buf[i]) break; }
    g_buf[i] = 0;
    q[5] = (uint32_t)(uintptr_t)g_buf;
    if (!net_call(w, q, NET_RESOLVE)) return 0;
    if (!he) he = qw_alloc(w, 64 + 256);
    WR32(he, he + 64); WR32(he + 4, he + 24); WR16(he + 8, 2); WR16(he + 10, 4); WR32(he + 12, he + 28);
    WR32(he + 24, 0);                 /* no aliases */
    WR32(he + 28, he + 40); WR32(he + 32, 0);
    WR8(he + 40, (uint8_t)(q[14] >> 24)); WR8(he + 41, (uint8_t)(q[14] >> 16)); WR8(he + 42, (uint8_t)(q[14] >> 8)); WR8(he + 43, (uint8_t)q[14]);
    for (i = 0; i < 255 && g_buf[i]; i++) WR8(he + 64 + (uint32_t)i, g_buf[i]);
    WR8(he + 64 + (uint32_t)i, 0);
    return he;
}
FN(n_null) { (void)a; w->net_error = 11004; return 0; }

#define N(name, ord, fn, args) { "wsock32", name, args, fn }, { "wsock32", "#" #ord, args, fn }, { "ws2_32", name, args, fn }, { "ws2_32", "#" #ord, args, fn }
const qhle qw_net[] = {
    N("accept", 1, n_accept, 3), N("bind", 2, n_bind, 3), N("closesocket", 3, n_closesocket, 1), N("connect", 4, n_connect, 3),
    N("getpeername", 5, n_getpeername, 3), N("getsockname", 6, n_getsockname, 3), N("getsockopt", 7, n_getsockopt, 5),
    N("htonl", 8, n_htonl, 1), N("htons", 9, n_htons, 1), N("inet_addr", 10, n_inet_addr, 1), N("inet_ntoa", 11, n_inet_ntoa, 1),
    N("ioctlsocket", 12, n_ioctlsocket, 3), N("listen", 13, n_listen, 2), N("ntohl", 14, n_htonl, 1), N("ntohs", 15, n_htons, 1),
    N("recv", 16, n_recv, 4), N("recvfrom", 17, n_recvfrom, 6), N("select", 18, n_select, 5), N("send", 19, n_send, 4),
    N("sendto", 20, n_sendto, 6), N("setsockopt", 21, n_setsockopt, 5), N("shutdown", 22, n_shutdown, 2), N("socket", 23, n_socket, 3),
    N("gethostbyaddr", 51, n_null, 3), N("gethostbyname", 52, n_gethostbyname, 1), N("getprotobyname", 53, n_null, 1),
    N("getservbyname", 55, n_null, 2), N("gethostname", 57, n_gethostname, 2),
    /* Asynchronous notification is accepted but delivers no events yet: programs start, network play would wait. */
    N("WSAAsyncSelect", 101, n_ok0, 4), N("WSAAsyncGetHostByName", 103, n_null, 5), N("WSACancelAsyncRequest", 108, n_ok0, 1),
    N("WSAGetLastError", 111, n_WSAGetLastError, 0), N("WSASetLastError", 112, n_WSASetLastError, 1), N("WSACancelBlockingCall", 113, n_ok0, 0),
    N("WSAIsBlocking", 114, n_ok0, 0), N("WSAStartup", 115, n_WSAStartup, 2), N("WSACleanup", 116, n_ok0, 0), N("__WSAFDIsSet", 151, n_WSAFDIsSet, 2),
    { NULL, NULL, 0, NULL }
};
