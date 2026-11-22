/* TCP and UDP behind the guest's Winsock: loopback echo, error codes, non-blocking behaviour. */
#include "qrx_hostnet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
enum { NET_SOCKET = 1, NET_BIND, NET_LISTEN, NET_ACCEPT, NET_CONNECT, NET_SEND, NET_RECV, NET_SENDTO, NET_RECVFROM,
       NET_POLL, NET_CLOSE, NET_SHUTDOWN, NET_SETOPT, NET_GETOPT, NET_SOCKNAME, NET_PEERNAME, NET_RESOLVE, NET_HOSTNAME };
#define LOOPBACK 0x7F000001u
static uint8_t ram[65536];
static unsigned failures;
#define CHECK(x, ...) do { if (!(x)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); failures++; } } while (0)
static uint32_t call(uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t *err)
{
    uint32_t a[13] = { a0, a1, a2, a3, a4, a5, 0, 0, 0, 0, 0, 0, 0 };
    qrx_hostnet_call(op, a, ram);
    if (err) *err = a[9];
    return a[8];
}
int main(void)
{
    uint32_t e, ls = call(NET_SOCKET, 1, 0, 0, 0, 0, 0, &e);
    CHECK(ls != UINT32_MAX, "socket");
    uint32_t a[13] = { ls, 0, 0 };
    /* port 0: the host picks one */
    CHECK(call(NET_BIND, ls, LOOPBACK, 0, 0, 0, 0, &e) == 0, "bind %u", e);
    CHECK(call(NET_LISTEN, ls, 4, 0, 0, 0, 0, &e) == 0, "listen %u", e);
    memset(a, 0, sizeof a); a[0] = ls;
    qrx_hostnet_call(NET_SOCKNAME, a, ram);
    uint32_t port = a[11];
    CHECK(port != 0, "port");
    CHECK(call(NET_ACCEPT, ls, 0, 0, 0, 0, 0, &e) == UINT32_MAX && e == 10035, "accept must not block: %u", e);
    uint32_t cs = call(NET_SOCKET, 1, 0, 0, 0, 0, 0, &e);
    uint32_t r = call(NET_CONNECT, cs, LOOPBACK, port, 0, 0, 0, &e);
    CHECK(r == 0 || e == 10036, "connect %u", e);
    uint32_t ss = UINT32_MAX;
    for (int i = 0; i < 1000 && ss == UINT32_MAX; i++) { ss = call(NET_ACCEPT, ls, 0, 0, 0, 0, 0, &e); usleep(1000); }
    CHECK(ss != UINT32_MAX, "accept %u", e);
    memcpy(ram + 100, "hello qrx", 9);
    CHECK(call(NET_SEND, cs, 100, 9, 0, 0, 0, &e) == 9, "send %u", e);
    uint32_t got = UINT32_MAX;
    for (int i = 0; i < 1000 && got == UINT32_MAX; i++) { got = call(NET_RECV, ss, 200, 64, 0, 0, 0, &e); usleep(1000); }
    CHECK(got == 9 && !memcmp(ram + 200, "hello qrx", 9), "recv %u", got);
    CHECK(call(NET_RECV, ss, 200, 64, 0, 0, 0, &e) == UINT32_MAX && e == 10035, "empty recv %u", e);
    CHECK(call(NET_POLL, ss, 2, 0, 0, 0, 0, &e) == 2, "writable");
    CHECK(call(NET_CLOSE, cs, 0, 0, 0, 0, 0, &e) == 0, "close");
    got = UINT32_MAX;
    for (int i = 0; i < 1000 && got == UINT32_MAX; i++) { got = call(NET_RECV, ss, 200, 64, 0, 0, 0, &e); usleep(1000); }
    CHECK(got == 0, "EOF after close: %u", got);
    CHECK(call(NET_SEND, 60, 100, 9, 0, 0, 0, &e) == UINT32_MAX && e == 10038, "bad socket %u", e);
    /* refused */
    uint32_t rs = call(NET_SOCKET, 1, 0, 0, 0, 0, 0, &e);
    memset(a, 0, sizeof a); a[0] = ss; qrx_hostnet_call(NET_CLOSE, a, ram);
    a[0] = ls; qrx_hostnet_call(NET_CLOSE, a, ram);
    r = call(NET_CONNECT, rs, LOOPBACK, port, 0, 0, 0, &e);
    for (int i = 0; i < 1000 && r == UINT32_MAX && e == 10036; i++) {
        uint32_t ready = call(NET_POLL, rs, 2, 0, 0, 0, 0, &e);
        if (ready & 2u) { r = UINT32_MAX; break; }
        e = 10036;
        usleep(1000);
    }
    CHECK(e == 10061, "refused: %u", e);
    /* UDP */
    uint32_t u = call(NET_SOCKET, 2, 0, 0, 0, 0, 0, &e);
    CHECK(call(NET_BIND, u, LOOPBACK, 0, 0, 0, 0, &e) == 0, "udp bind");
    memset(a, 0, sizeof a); a[0] = u; qrx_hostnet_call(NET_SOCKNAME, a, ram);
    uint32_t up = a[11];
    memcpy(ram + 300, "ping", 4);
    CHECK(call(NET_SENDTO, u, 300, 4, 0, LOOPBACK, up, &e) == 4, "sendto %u", e);
    memset(a, 0, sizeof a);
    a[8] = UINT32_MAX;
    for (int i = 0; i < 1000 && a[8] == UINT32_MAX; i++) {
        a[0] = u; a[1] = 400; a[2] = 64; a[3] = 0;
        qrx_hostnet_call(NET_RECVFROM, a, ram);
        usleep(1000);
    }
    CHECK(a[8] == 4 && a[10] == LOOPBACK && a[11] == up, "recvfrom %u from %x:%u", a[8], a[10], a[11]);
    /* random bytes and clock for native 68k programs */
    memset(ram + 500, 0, 64);
    CHECK(call(QRX_NET_RANDOM, 0, 500, 32, 0, 0, 0, &e) == 32, "random %u", e);
    memcpy(ram + 600, ram + 500, 32);
    CHECK(call(QRX_NET_RANDOM, 0, 500, 32, 0, 0, 0, &e) == 32, "random again %u", e);
    CHECK(memcmp(ram + 500, ram + 600, 32) != 0, "two random blocks are the same");
    CHECK(ram[532] == 0, "random wrote past its length");
    CHECK(call(QRX_NET_RANDOM, 0, 500, 0, 0, 0, 0, &e) == UINT32_MAX, "random of no bytes");
    CHECK(call(QRX_NET_RANDOM, 0, 500, 257, 0, 0, 0, &e) == UINT32_MAX, "random over the limit");
    memset(a, 0, sizeof a);
    qrx_hostnet_call(QRX_NET_TIME, a, ram);
    CHECK(a[8] == 0 && a[10] > 1700000000u && a[11] < 1000000u, "time %u.%u", a[10], a[11]);
    if (failures) { printf("%u failures\n", failures); return 1; }
    puts("host sockets: all checks passed");
    return 0;
}
