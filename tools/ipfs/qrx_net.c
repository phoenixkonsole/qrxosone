/*
 * qrx_net.c - synchronous 68k client for the QRXHostFS host-socket door.
 *
 * The host side keeps sockets non-blocking (qrx_hostnet.c).  This file
 * supplies the blocking send/recv/accept/connect semantics the existing
 * IPFS code expects, using the host's poll operation and AROS Delay().
 */
#include "qrx_net.h"

#include <proto/dos.h>

#include <string.h>
#include <stdio.h>

static int g_error;
static int g_ok;

#if defined(__m68k__)
static inline uint32_t net_rd(unsigned reg)
{
    uint32_t value;
    __asm__ volatile("move.l (%1),%0" : "=d"(value)
                     : "a"(QRX_NET_HFS_BASE + reg) : "memory");
    return value;
}
static inline void net_wr(unsigned reg, uint32_t value)
{
    *(volatile uint32_t *)(QRX_NET_HFS_BASE + reg) = value;
}
#else
static inline uint32_t net_rd(unsigned reg)
{
    return *(volatile uint32_t *)(QRX_NET_HFS_BASE + reg);
}
static inline void net_wr(unsigned reg, uint32_t value)
{
    *(volatile uint32_t *)(QRX_NET_HFS_BASE + reg) = value;
}
#endif

static uint32_t net_call(uint32_t op, const uint32_t *args, uint32_t *out)
{
    static uint32_t req[QRX_NET_WORDS];
    unsigned i;

    memset(req, 0, sizeof(req));
    req[0] = op;
    for (i = 0; i < 12; ++i) req[i + 1] = args ? args[i] : 0;
    net_wr(QRX_NET_HFS_DATA, (uint32_t)(uintptr_t)req);
    net_wr(QRX_NET_HFS_LENGTH, QRX_NET_WORDS * 4u);
    __asm__ volatile("" ::: "memory");
    net_wr(QRX_NET_HFS_COMMAND, QRX_NET_HFS_NET);
    __asm__ volatile("" ::: "memory");
    if (net_rd(QRX_NET_HFS_STATUS) != QRX_NET_HFS_OK) {
        g_error = 10045;
        if (out) memset(out, 0, 12 * sizeof(uint32_t));
        return 0;
    }
    if (out) {
        for (i = 0; i < 12; ++i) out[i] = req[i + 1];
    }
    g_error = out ? (int)out[9] : 0;
    return 1;
}

int qrx_net_init(void)
{
    if (g_ok) return 1;
    if (net_rd(0) != QRX_NET_HFS_MAGIC ||
        !(net_rd(QRX_NET_HFS_FEAT) & QRX_NET_HFS_FEAT)) {
        g_error = 10045;
        return 0;
    }
    g_ok = 1;
    return 1;
}

int qrx_net_available(void)
{
    return qrx_net_init();
}

int qrx_net_last_error(void)
{
    return g_error;
}

static int net_poll_once(int fd, int mask)
{
    uint32_t a[12], r[12];
    memset(a, 0, sizeof(a));
    a[0] = (uint32_t)fd;
    a[1] = (uint32_t)mask;
    if (!qrx_net_init() || !net_call(QRX_NET_OP_POLL, a, r)) return 0;
    g_error = (int)r[9];
    return (int)r[8];
}

int qrx_net_poll(int fd, int mask, unsigned *avail)
{
    uint32_t a[12], r[12];
    memset(a, 0, sizeof(a));
    a[0] = (uint32_t)fd;
    a[1] = (uint32_t)mask;
    if (!qrx_net_init() || !net_call(QRX_NET_OP_POLL, a, r)) return 0;
    g_error = (int)r[9];
    if (avail) *avail = r[10];
    return (int)r[8];
}

static int wait_ready(int fd, int mask)
{
    for (;;) {
        if (net_poll_once(fd, mask)) return 1;
        if (g_error && g_error != QRX_NET_EWOULDBLOCK &&
            g_error != QRX_NET_EINPROGRESS && g_error != QRX_NET_EALREADY)
            return 0;
        Delay(1);
    }
}

int qrx_net_socket(int type)
{
    uint32_t a[12], r[12];
    memset(a, 0, sizeof(a));
    /* qrx_hostnet's NET_SOCKET uses 1=TCP, 2=UDP. */
    a[0] = (type == SOCK_DGRAM) ? 2u : 1u;
    if (!qrx_net_init() || !net_call(QRX_NET_OP_SOCKET, a, r) ||
        (int32_t)r[8] < 0) {
        if (!g_error) g_error = (int)r[9];
        return -1;
    }
    return (int)r[8];
}

int qrx_net_bind(int fd, const struct sockaddr_in *sa)
{
    uint32_t a[12], r[12];
    if (!sa) { g_error = 10022; return -1; }
    memset(a, 0, sizeof(a));
    a[0] = (uint32_t)fd;
    a[1] = (uint32_t)ntohl(sa->sin_addr.s_addr);
    a[2] = (uint32_t)ntohs(sa->sin_port);
    if (!qrx_net_init() || !net_call(QRX_NET_OP_BIND, a, r) ||
        (int32_t)r[8] < 0) {
        if (!g_error) g_error = (int)r[9];
        return -1;
    }
    g_error = 0;
    return 0;
}

int qrx_net_listen(int fd, int backlog)
{
    uint32_t a[12], r[12];
    memset(a, 0, sizeof(a));
    a[0] = (uint32_t)fd;
    a[1] = (uint32_t)backlog;
    if (!qrx_net_init() || !net_call(QRX_NET_OP_LISTEN, a, r) ||
        (int32_t)r[8] < 0) {
        if (!g_error) g_error = (int)r[9];
        return -1;
    }
    g_error = 0;
    return 0;
}

static void fill_sockaddr(struct sockaddr_in *sa, uint32_t ip, uint32_t port)
{
    if (!sa) return;
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((unsigned short)port);
    sa->sin_addr.s_addr = htonl(ip);
}

int qrx_net_accept_nb(int fd, struct sockaddr_in *sa)
{
    uint32_t a[12], r[12];
    memset(a, 0, sizeof(a));
    a[0] = (uint32_t)fd;
    if (!qrx_net_init() || !net_call(QRX_NET_OP_ACCEPT, a, r)) return -1;
    if ((int32_t)r[8] < 0) {
        g_error = (int)r[9];
        return -1;
    }
    fill_sockaddr(sa, r[10], r[11]);
    g_error = 0;
    return (int)r[8];
}

int qrx_net_accept(int fd, struct sockaddr_in *sa, socklen_t *len)
{
    int n;
    for (;;) {
        n = qrx_net_accept_nb(fd, sa);
        if (n >= 0) {
            if (len) *len = sizeof(*sa);
            return n;
        }
        if (g_error != QRX_NET_EWOULDBLOCK &&
            g_error != QRX_NET_EINPROGRESS &&
            g_error != QRX_NET_EALREADY)
            return -1;
        if (!wait_ready(fd, 1)) return -1;
    }
}

static int net_so_error(int fd)
{
    uint32_t a[12], r[12];
    memset(a, 0, sizeof(a));
    a[0] = (uint32_t)fd;
    a[1] = 0xffffu;
    a[2] = 0x1007u;
    if (!qrx_net_init() || !net_call(QRX_NET_OP_GETOPT, a, r)) return 10004;
    return (int)r[10];
}

int qrx_net_connect(int fd, const struct sockaddr_in *sa)
{
    uint32_t a[12], r[12];
    if (!sa) { g_error = 10022; return -1; }
    memset(a, 0, sizeof(a));
    a[0] = (uint32_t)fd;
    a[1] = (uint32_t)ntohl(sa->sin_addr.s_addr);
    a[2] = (uint32_t)ntohs(sa->sin_port);
    if (!qrx_net_init() || !net_call(QRX_NET_OP_CONNECT, a, r)) return -1;
    if ((int32_t)r[8] >= 0) { g_error = 0; return 0; }
    g_error = (int)r[9];
    if (g_error == QRX_NET_EWOULDBLOCK || g_error == QRX_NET_EINPROGRESS ||
        g_error == QRX_NET_EALREADY) {
        if (wait_ready(fd, 2)) {
            int e = net_so_error(fd);
            if (e) { g_error = e; return -1; }
            g_error = 0;
            return 0;
        }
    }
    return -1;
}

long qrx_net_send(int fd, const void *data, unsigned long len, int flags)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned long done = 0;
    uint32_t a[12], r[12];

    while (done < len) {
        memset(a, 0, sizeof(a));
        a[0] = (uint32_t)fd;
        a[1] = (uint32_t)(uintptr_t)(p + done);
        a[2] = (uint32_t)(len - done);
        a[3] = (uint32_t)flags;
        if (!qrx_net_init() || !net_call(QRX_NET_OP_SEND, a, r)) return -1;
        if ((int32_t)r[8] > 0) {
            done += (unsigned long)r[8];
            continue;
        }
        g_error = (int)r[9];
        if (g_error == QRX_NET_EWOULDBLOCK || g_error == QRX_NET_EINPROGRESS ||
            g_error == QRX_NET_EALREADY) {
            if (wait_ready(fd, 2)) continue;
        }
        return done ? (long)done : -1;
    }
    g_error = 0;
    return (long)done;
}

long qrx_net_recv(int fd, void *data, unsigned long len, int flags)
{
    uint32_t a[12], r[12];
    for (;;) {
        memset(a, 0, sizeof(a));
        a[0] = (uint32_t)fd;
        a[1] = (uint32_t)(uintptr_t)data;
        a[2] = (uint32_t)len;
        a[3] = (uint32_t)flags;
        if (!qrx_net_init() || !net_call(QRX_NET_OP_RECV, a, r)) return -1;
        if ((int32_t)r[8] >= 0) {
            g_error = 0;
            return (long)(int32_t)r[8];
        }
        g_error = (int)r[9];
        if (g_error == QRX_NET_EWOULDBLOCK || g_error == QRX_NET_EINPROGRESS ||
            g_error == QRX_NET_EALREADY) {
            if (wait_ready(fd, 1)) continue;
        }
        return -1;
    }
}

int qrx_net_close(int fd)
{
    uint32_t a[12], r[12];
    memset(a, 0, sizeof(a));
    a[0] = (uint32_t)fd;
    if (!qrx_net_init() || !net_call(QRX_NET_OP_CLOSE, a, r)) return -1;
    g_error = 0;
    return 0;
}

int qrx_net_resolve(const char *host, uint32_t *ip)
{
    uint32_t a[12], r[12];
    if (!host || !ip) { g_error = 10022; return 0; }
    memset(a, 0, sizeof(a));
    a[1] = (uint32_t)(uintptr_t)host;
    if (!qrx_net_init() || !net_call(QRX_NET_OP_RESOLVE, a, r) ||
        (int32_t)r[8] < 0) {
        g_error = (int)r[9];
        return 0;
    }
    *ip = r[10];
    g_error = 0;
    return 1;
}

unsigned long qrx_net_inet_addr(const char *text)
{
    unsigned long parts[4] = {0, 0, 0, 0};
    int n = 0;
    const char *p = text;

    if (!text) return 0xffffffffUL;
    while (*p && n < 4) {
        unsigned long v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10UL + (unsigned long)(*p - '0');
            if (v > 255UL) return 0xffffffffUL;
            ++p;
            digits = 1;
        }
        if (!digits) return 0xffffffffUL;
        parts[n++] = v;
        if (*p == '.') { ++p; continue; }
        break;
    }
    if (*p || n != 4) return 0xffffffffUL;
    return htonl((uint32_t)((parts[0] << 24) | (parts[1] << 16) |
                            (parts[2] << 8) | parts[3]));
}

struct hostent *qrx_net_gethostbyname(const char *host)
{
    static struct hostent he;
    static char name[256];
    static char *list[2];
    static struct in_addr addr;
    uint32_t ip;

    if (!host || !qrx_net_resolve(host, &ip)) return NULL;
    strncpy(name, host, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    addr.s_addr = htonl(ip);
    list[0] = (char *)&addr;
    list[1] = NULL;
    memset(&he, 0, sizeof(he));
    he.h_name = name;
    he.h_aliases = NULL;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = list;
    return &he;
}
