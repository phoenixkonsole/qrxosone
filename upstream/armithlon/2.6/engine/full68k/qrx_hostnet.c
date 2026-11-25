/* TCP and UDP for guest programs, on the host's sockets. Everything is non-blocking; a guest
 * call that has to wait polls from the guest side. Reached through door service 4. */
#include "qrx_hostnet.h"
#include "qrx_netpolicy.h"
#include "qrx_vault.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAXSOCK 64
enum { NET_SOCKET = 1, NET_BIND, NET_LISTEN, NET_ACCEPT, NET_CONNECT, NET_SEND, NET_RECV, NET_SENDTO, NET_RECVFROM,
       NET_POLL, NET_CLOSE, NET_SHUTDOWN, NET_SETOPT, NET_GETOPT, NET_SOCKNAME, NET_PEERNAME, NET_RESOLVE, NET_HOSTNAME };
static int fds[MAXSOCK];
static bool used[MAXSOCK];
/* Tor/WireGuard (qrx_netpolicy.h): a stream socket whose SOCKS reply is still coming, its reply so
 * far, the error it ended with, where it was meant to go; datagram sockets, which may not leave */
static bool socks_wait[MAXSOCK], dgram[MAXSOCK];
static uint8_t socks_buf[MAXSOCK][QRX_SOCKS_REPLY];
static uint8_t socks_have[MAXSOCK];
static int sock_err[MAXSOCK];
static uint32_t peer_ip[MAXSOCK], peer_port[MAXSOCK];
/* an IPv6 socket of the guest that is an IPv4 socket here, because this host has no IPv6 */
static bool v4only[MAXSOCK];
static bool loopback(uint32_t ip) { return (ip >> 24) == 127; }
/* Advances a pending SOCKS connection: 1 connected, 0 still waiting, -1 failed (sock_err set) */
static int socks_step(int s, int fd)
{
    if (!socks_wait[s]) return 1;
    while (socks_have[s] < QRX_SOCKS_REPLY) {
        ssize_t n = read(fd, socks_buf[s] + socks_have[s], QRX_SOCKS_REPLY - socks_have[s]);
        if (n > 0) { socks_have[s] += (uint8_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        socks_wait[s] = false;
        sock_err[s] = ECONNREFUSED;
        return -1;
    }
    int r = qrx_net_socks_reply(socks_buf[s], socks_have[s]);
    socks_wait[s] = false;
    if (r == 1) return 1;
    sock_err[s] = -r;
    return -1;
}

static uint32_t wsa_of(int e)
{
    switch (e) {
    case EWOULDBLOCK: return 10035;
    case EINPROGRESS: return 10036;
    case EALREADY: return 10037;
    case ENOTSOCK: case EBADF: return 10038;
    case EMSGSIZE: return 10040;
    case EPROTONOSUPPORT: return 10043;
    case EOPNOTSUPP: return 10045;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;
    case ECONNABORTED: return 10053;
    case ECONNRESET: case EPIPE: return 10054;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;
    case EINVAL: return 10022;
    case EACCES: return 10013;
    case EMFILE: return 10024;
    default: return 10004;
    }
}
static int guest_slot(int fd)
{
    for (int i = 0; i < MAXSOCK; i++) if (!used[i]) {
        used[i] = true; fds[i] = fd;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        return i;
    }
    close(fd);
    errno = EMFILE;
    return -1;
}
static int fd_of(uint32_t s) { return s < MAXSOCK && used[s] ? fds[s] : -1; }
static void addr_make(struct sockaddr_in *sa, uint32_t ip, uint32_t port)
{
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    sa->sin_addr.s_addr = htonl(ip);
}
static void result(uint32_t *a, long r)
{
    if (r < 0) { a[8] = UINT32_MAX; a[9] = wsa_of(errno); }
    else a[8] = (uint32_t)r;
}
/* ---- IPv6: door address records (qrx_hostnet.h), big endian in guest memory */
static uint32_t be32_get(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void be32_put(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void rec_to_sa6(const uint8_t *r, struct sockaddr_in6 *sa)
{
    memset(sa, 0, sizeof *sa);
    sa->sin6_family = AF_INET6;
    memcpy(&sa->sin6_addr, r, 16);
    sa->sin6_port = htons((uint16_t)be32_get(r + 16));
    sa->sin6_scope_id = be32_get(r + 20);
}
/* any family the host gives back, as a record (IPv4 as ::ffff:a.b.c.d) */
static void sa_to_rec(const struct sockaddr *sa, uint8_t *r)
{
    memset(r, 0, QRX_NET_ADDR6_SIZE);
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
        memcpy(r, &s6->sin6_addr, 16);
        be32_put(r + 16, ntohs(s6->sin6_port));
        be32_put(r + 20, s6->sin6_scope_id);
    } else if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;
        r[10] = r[11] = 0xFF;
        memcpy(r + 12, &s4->sin_addr, 4);
        be32_put(r + 16, ntohs(s4->sin_port));
    }
}
/* the IPv4 address of a v4-mapped record, or 0 with *v4 false */
static uint32_t rec_v4(const uint8_t *r, bool *v4)
{
    static const uint8_t pre[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xFF,0xFF };
    *v4 = !memcmp(r, pre, 12);
    return *v4 ? be32_get(r + 12) : 0;
}
/* for a v4only socket: the IPv4 form of a record (v4-mapped, ::1, ::); false for real IPv6 */
static bool rec_to_sa4(const uint8_t *r, struct sockaddr_in *sa)
{
    static const uint8_t z[16], lo[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
    bool v4;
    uint32_t ip = rec_v4(r, &v4);
    if (!v4) {
        if (!memcmp(r, z, 16)) ip = 0;
        else if (!memcmp(r, lo, 16)) ip = 0x7F000001u;
        else return false;
    }
    addr_make(sa, ip, be32_get(r + 16));
    return true;
}
static bool rec_loopback(const uint8_t *r)
{
    static const uint8_t lo[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
    bool v4;
    uint32_t ip = rec_v4(r, &v4);
    return v4 ? loopback(ip) : !memcmp(r, lo, 16);
}

void qrx_hostnet_call(uint32_t op, uint32_t *a, uint8_t *ram)
{
    /* a[0..7] are the arguments, a[8] result, a[9] error, a[10] ip, a[11] port */
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int fd = fd_of(a[0]);
    signal(SIGPIPE, SIG_IGN);
    a[9] = 0;
    switch (op) {
    case NET_SOCKET: {
        int type = a[0 + 0] == 2 ? SOCK_DGRAM : SOCK_STREAM;
        int nfd = socket(AF_INET, type, 0);
        if (nfd < 0) { result(a, -1); return; }
        int s = guest_slot(nfd);
        if (s >= 0) { dgram[s] = type == SOCK_DGRAM; socks_wait[s] = false; sock_err[s] = 0; peer_ip[s] = peer_port[s] = 0; v4only[s] = false; }
        result(a, s);
        return;
    }
    case QRX_NET_CONFIG:
        /* the whole system's network: only an admin, once there are users */
        if (!qrx_vault_admin_or_unset()) { a[8] = UINT32_MAX; a[9] = 10013; return; }   /* WSAEACCES */
        result(a, qrx_net_configure((const char *)ram + a[1], a[2]));
        return;
    case QRX_NET_STATUS:
        a[10] = (uint32_t)qrx_net_mode();
        a[11] = (uint32_t)qrx_net_ready();
        a[8] = 0;
        return;
    case NET_RESOLVE: {
        if (qrx_net_mode() > 0) {         /* the name through the tunnel too: no DNS from the Mac */
            uint32_t ip;
            if (qrx_net_resolve((const char *)ram + a[1], &ip)) { a[8] = UINT32_MAX; a[9] = 11001; return; }
            a[10] = ip;
            a[8] = 0;
            return;
        }
        struct hostent *h = gethostbyname((const char *)ram + a[1]);
        if (!h || h->h_length != 4) { a[8] = UINT32_MAX; a[9] = 11001; return; }
        uint32_t ip;
        memcpy(&ip, h->h_addr_list[0], 4);
        a[10] = ntohl(ip);
        a[8] = 0;
        return;
    }
    case QRX_NET_TIME: {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        a[10] = (uint32_t)tv.tv_sec;
        a[11] = (uint32_t)tv.tv_usec;
        {   /* the Mac's offset from UTC in seconds, east positive: guests set a local clock with it */
            struct tm lt;
            time_t now = tv.tv_sec;
            localtime_r(&now, &lt);
            a[12] = (uint32_t)(int32_t)lt.tm_gmtoff;
        }
        a[8] = 0;
        return;
    }
    case QRX_NET_RANDOM:
        if (a[2] == 0 || a[2] > 256 || getentropy(ram + a[1], a[2])) { a[8] = UINT32_MAX; a[9] = 10022; return; }
        a[8] = a[2];
        return;
    case NET_HOSTNAME: {
        char *dst = (char *)ram + a[1];
        result(a, gethostname(dst, a[2]));
        return;
    }
    case QRX_NET_SOCKET6: {
        int type = a[0] == 2 ? SOCK_DGRAM : SOCK_STREAM;
        int nfd = socket(AF_INET6, type, 0), only4 = 0;
        if (nfd < 0 && errno == EAFNOSUPPORT) { nfd = socket(AF_INET, type, 0); only4 = 1; }   /* no IPv6 on this host */
        if (nfd < 0) { result(a, -1); return; }
        int off = 0;
        if (!only4) setsockopt(nfd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
        int s = guest_slot(nfd);
        if (s >= 0) { dgram[s] = type == SOCK_DGRAM; socks_wait[s] = false; sock_err[s] = 0; peer_ip[s] = peer_port[s] = 0; v4only[s] = only4; }
        result(a, s);
        return;
    }
    case QRX_NET_RESOLVE6: {
        uint8_t *out = ram + a[2];
        uint32_t max = a[3], n = 0;
        if (qrx_net_mode() > 0) {         /* through the tunnel: IPv4 only, as NET_RESOLVE */
            uint32_t ip;
            if (max == 0 || qrx_net_resolve((const char *)ram + a[1], &ip)) { a[8] = UINT32_MAX; a[9] = 11001; return; }
            memset(out, 0, QRX_NET_ADDR6_SIZE);
            out[10] = out[11] = 0xFF;
            be32_put(out + 12, ip);
            a[8] = 1;
            return;
        }
        struct addrinfo hints, *res = NULL, *ai;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo((const char *)ram + a[1], NULL, &hints, &res) || !res) { a[8] = UINT32_MAX; a[9] = 11001; return; }
        for (ai = res; ai && n < max; ai = ai->ai_next)
            if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) sa_to_rec(ai->ai_addr, out + QRX_NET_ADDR6_SIZE * n++);
        freeaddrinfo(res);
        if (!n) { a[8] = UINT32_MAX; a[9] = 11001; return; }
        a[8] = n;
        return;
    }
    default: break;
    }
    if (fd < 0) { a[8] = UINT32_MAX; a[9] = 10038; return; }
    switch (op) {
    case NET_BIND: {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        /* through a tunnel nothing listens on the Mac's own addresses */
        addr_make(&sa, qrx_net_mode() > 0 ? 0x7F000001u : a[1], a[2]);
        result(a, bind(fd, (struct sockaddr *)&sa, sizeof sa));
        break;
    }
    case NET_LISTEN: result(a, listen(fd, (int)a[1])); break;
    case NET_ACCEPT: {
        int nfd = accept(fd, (struct sockaddr *)&sa, &sl);
        if (nfd < 0) { result(a, -1); break; }
        a[10] = ntohl(sa.sin_addr.s_addr);
        a[11] = ntohs(sa.sin_port);
        {
            int s = guest_slot(nfd);
            if (s >= 0) { dgram[s] = false; socks_wait[s] = false; sock_err[s] = 0; peer_ip[s] = a[10]; peer_port[s] = a[11]; v4only[s] = false; }
            result(a, s);
        }
        break;
    }
    case NET_CONNECT:
        if (qrx_net_mode() > 0 && !loopback(a[1])) {
            /* through qrxnetd: the socket becomes the Unix stream with the SOCKS request sent */
            if (dgram[a[0]]) { errno = ENETUNREACH; result(a, -1); break; }
            int nfd = qrx_net_socks_connect(a[1], (uint16_t)a[2]);
            if (nfd < 0) { result(a, -1); break; }
            dup2(nfd, fd);
            close(nfd);
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
            { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
            socks_wait[a[0]] = true;
            socks_have[a[0]] = 0;
            sock_err[a[0]] = 0;
            peer_ip[a[0]] = a[1]; peer_port[a[0]] = a[2];
            errno = EINPROGRESS;
            result(a, -1);
            break;
        }
        addr_make(&sa, a[1], a[2]);
        peer_ip[a[0]] = a[1]; peer_port[a[0]] = a[2];
        result(a, connect(fd, (struct sockaddr *)&sa, sizeof sa));
        break;
    case NET_SEND:
        if (socks_step((int)a[0], fd) <= 0) { errno = sock_err[a[0]] ? ENOTCONN : EWOULDBLOCK; result(a, -1); break; }
        result(a, send(fd, ram + a[1], a[2], (a[3] & 1u) ? MSG_OOB : 0));
        break;
    case NET_RECV: {
        if (socks_step((int)a[0], fd) <= 0) { errno = sock_err[a[0]] ? ENOTCONN : EWOULDBLOCK; result(a, -1); break; }
        ssize_t n = recv(fd, ram + a[1], a[2], (a[3] & 2u) ? MSG_PEEK : 0);
        result(a, n);
        break;
    }
    case NET_SENDTO:
        /* UDP does not go through Tor, nor through this tunnel: it does not leave at all */
        if (qrx_net_mode() > 0 && !loopback(a[4])) { errno = ENETUNREACH; result(a, -1); break; }
        addr_make(&sa, a[4], a[5]);
        result(a, sendto(fd, ram + a[1], a[2], 0, (struct sockaddr *)&sa, sizeof sa));
        break;
    case NET_RECVFROM: {
        ssize_t n = recvfrom(fd, ram + a[1], a[2], (a[3] & 2u) ? MSG_PEEK : 0, (struct sockaddr *)&sa, &sl);
        if (n >= 0) { a[10] = ntohl(sa.sin_addr.s_addr); a[11] = ntohs(sa.sin_port); }
        result(a, n);
        break;
    }
    case NET_POLL: {
        /* mask: 1 readable, 2 writable, 4 exception, 8 report bytes readable in a[10] */
        if (socks_wait[a[0]]) {
            int st = socks_step((int)a[0], fd);
            if (st == 0) { a[8] = 0; a[9] = 0; if (a[1] & 8u) a[10] = 0; break; }   /* still connecting */
            if (st < 0) { a[8] = (a[1] & 2u) ? 2u : 0u; a[9] = wsa_of(sock_err[a[0]]); break; }
        }
        struct pollfd p = { fd, 0, 0 };
        if (a[1] & 1u) p.events |= POLLIN;
        if (a[1] & 2u) p.events |= POLLOUT;
        if (a[1] & 4u) p.events |= POLLPRI;
        int r = poll(&p, 1, 0);
        uint32_t ready = 0;
        if (r > 0) {
            if (p.revents & (POLLIN | POLLHUP)) ready |= 1u;
            if (p.revents & (POLLOUT | POLLERR | POLLHUP)) ready |= 2u;
            if (p.revents & POLLPRI) ready |= 4u;
        }
        int err = 0;
        socklen_t el = sizeof err;
        if (ready & 2u) getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        a[8] = ready;
        a[9] = err ? wsa_of(err) : 0;
        if (a[1] & 8u) { int avail = 0; ioctl(fd, FIONREAD, &avail); a[10] = (uint32_t)avail; }
        break;
    }
    case NET_CLOSE:
        close(fd);
        used[a[0]] = false;
        socks_wait[a[0]] = false;
        sock_err[a[0]] = 0;
        a[8] = 0;
        break;
    case NET_SHUTDOWN: result(a, shutdown(fd, a[1] == 0 ? SHUT_RD : a[1] == 1 ? SHUT_WR : SHUT_RDWR)); break;
    case NET_SETOPT: {
        int v = (int)a[3], r = 0;
        if (a[1] == 0xFFFFu) {
            int name = a[2] == 4 ? SO_REUSEADDR : a[2] == 8 ? SO_KEEPALIVE : a[2] == 0x20 ? SO_BROADCAST :
                       a[2] == 0x1001 ? SO_SNDBUF : a[2] == 0x1002 ? SO_RCVBUF : -1;
            if (name >= 0) r = setsockopt(fd, SOL_SOCKET, name, &v, sizeof v);
        } else if (a[1] == 6 && a[2] == 1) r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v);
        result(a, r);
        break;
    }
    case NET_GETOPT: {
        int v = 0;
        socklen_t vl = sizeof v;
        if (a[1] == 0xFFFFu && a[2] == 0x1007) {     /* SO_ERROR */
            if (sock_err[a[0]]) { v = (int)wsa_of(sock_err[a[0]]); sock_err[a[0]] = 0; }
            else getsockopt(fd, SOL_SOCKET, SO_ERROR, &v, &vl);
            if (v && v < 10000) v = (int)wsa_of(v);
        } else if (a[1] == 0xFFFFu && a[2] == 0x1008) {   /* SO_TYPE */
            getsockopt(fd, SOL_SOCKET, SO_TYPE, &v, &vl);
            v = v == SOCK_DGRAM ? 2 : 1;
        }
        a[10] = (uint32_t)v;
        a[8] = 0;
        break;
    }
    case NET_SOCKNAME: case NET_PEERNAME: {
        if (qrx_net_mode() > 0) {         /* a tunnel's socket is a Unix stream: report the guest's view */
            a[10] = op == NET_PEERNAME ? peer_ip[a[0]] : 0x7F000001u;
            a[11] = op == NET_PEERNAME ? peer_port[a[0]] : 0;
            a[8] = 0;
            break;
        }
        int r = op == NET_SOCKNAME ? getsockname(fd, (struct sockaddr *)&sa, &sl) : getpeername(fd, (struct sockaddr *)&sa, &sl);
        if (r == 0) { a[10] = ntohl(sa.sin_addr.s_addr); a[11] = ntohs(sa.sin_port); }
        result(a, r);
        break;
    }
    case QRX_NET_BIND6: {
        struct sockaddr_in6 s6;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (v4only[a[0]]) {
            if (!rec_to_sa4(ram + a[1], &sa)) { errno = EADDRNOTAVAIL; result(a, -1); break; }
            if (qrx_net_mode() > 0) sa.sin_addr.s_addr = htonl(0x7F000001u);
            result(a, bind(fd, (struct sockaddr *)&sa, sizeof sa));
            break;
        }
        rec_to_sa6(ram + a[1], &s6);
        if (qrx_net_mode() > 0) s6.sin6_addr = in6addr_loopback;   /* through a tunnel: nothing on the Mac's addresses */
        result(a, bind(fd, (struct sockaddr *)&s6, sizeof s6));
        break;
    }
    case QRX_NET_CONNECT6: {
        const uint8_t *r = ram + a[1];
        bool v4;
        uint32_t ip = rec_v4(r, &v4);
        if (qrx_net_mode() > 0 && !rec_loopback(r)) {
            /* an IPv4 address goes through the tunnel as with NET_CONNECT; IPv6 does not leave */
            if (!v4 || dgram[a[0]]) { errno = ENETUNREACH; result(a, -1); break; }
            int nfd = qrx_net_socks_connect(ip, (uint16_t)be32_get(r + 16));
            if (nfd < 0) { result(a, -1); break; }
            dup2(nfd, fd);
            close(nfd);
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
            { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
            socks_wait[a[0]] = true;
            socks_have[a[0]] = 0;
            sock_err[a[0]] = 0;
            peer_ip[a[0]] = ip; peer_port[a[0]] = be32_get(r + 16);
            errno = EINPROGRESS;
            result(a, -1);
            break;
        }
        peer_ip[a[0]] = ip; peer_port[a[0]] = be32_get(r + 16);
        if (v4only[a[0]]) {
            if (!rec_to_sa4(r, &sa)) { errno = ENETUNREACH; result(a, -1); break; }
            result(a, connect(fd, (struct sockaddr *)&sa, sizeof sa));
            break;
        }
        struct sockaddr_in6 s6;
        rec_to_sa6(r, &s6);
        result(a, connect(fd, (struct sockaddr *)&s6, sizeof s6));
        break;
    }
    case QRX_NET_SENDTO6: {
        const uint8_t *r = ram + a[4];
        if (qrx_net_mode() > 0 && !rec_loopback(r)) { errno = ENETUNREACH; result(a, -1); break; }
        if (v4only[a[0]]) {
            if (!rec_to_sa4(r, &sa)) { errno = ENETUNREACH; result(a, -1); break; }
            result(a, sendto(fd, ram + a[1], a[2], 0, (struct sockaddr *)&sa, sizeof sa));
            break;
        }
        struct sockaddr_in6 s6;
        rec_to_sa6(r, &s6);
        result(a, sendto(fd, ram + a[1], a[2], 0, (struct sockaddr *)&s6, sizeof s6));
        break;
    }
    case QRX_NET_RECVFROM6: {
        struct sockaddr_storage ss;
        socklen_t l = sizeof ss;
        ssize_t n = recvfrom(fd, ram + a[1], a[2], (a[3] & 2u) ? MSG_PEEK : 0, (struct sockaddr *)&ss, &l);
        if (n >= 0) sa_to_rec((struct sockaddr *)&ss, ram + a[4]);
        result(a, n);
        break;
    }
    case QRX_NET_ACCEPT6: {
        struct sockaddr_storage ss;
        socklen_t l = sizeof ss;
        int nfd = accept(fd, (struct sockaddr *)&ss, &l);
        if (nfd < 0) { result(a, -1); break; }
        sa_to_rec((struct sockaddr *)&ss, ram + a[1]);
        int s = guest_slot(nfd);
        if (s >= 0) { dgram[s] = false; socks_wait[s] = false; sock_err[s] = 0; peer_ip[s] = peer_port[s] = 0; v4only[s] = v4only[a[0]]; }
        result(a, s);
        break;
    }
    case QRX_NET_SOCKNAME6: case QRX_NET_PEERNAME6: {
        uint8_t *r = ram + a[1];
        if (qrx_net_mode() > 0) {         /* a tunnel's socket is a Unix stream: the guest's view */
            memset(r, 0, QRX_NET_ADDR6_SIZE);
            if (op == QRX_NET_PEERNAME6) { r[10] = r[11] = 0xFF; be32_put(r + 12, peer_ip[a[0]]); be32_put(r + 16, peer_port[a[0]]); }
            else r[15] = 1;
            a[8] = 0;
            break;
        }
        struct sockaddr_storage ss;
        socklen_t l = sizeof ss;
        int rv = op == QRX_NET_SOCKNAME6 ? getsockname(fd, (struct sockaddr *)&ss, &l) : getpeername(fd, (struct sockaddr *)&ss, &l);
        if (rv == 0) sa_to_rec((struct sockaddr *)&ss, r);
        result(a, rv);
        break;
    }
    default: a[8] = UINT32_MAX; a[9] = 10045; break;
    }
}
