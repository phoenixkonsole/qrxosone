/* TCP and UDP for guest programs, on the host's sockets. Everything is non-blocking; a guest
 * call that has to wait polls from the guest side. Reached through door service 4. */
#include "qrx_hostnet.h"
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
        result(a, guest_slot(nfd));
        return;
    }
    case NET_RESOLVE: {
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
    default: break;
    }
    if (fd < 0) { a[8] = UINT32_MAX; a[9] = 10038; return; }
    switch (op) {
    case NET_BIND: {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        addr_make(&sa, a[1], a[2]);
        result(a, bind(fd, (struct sockaddr *)&sa, sizeof sa));
        break;
    }
    case NET_LISTEN: result(a, listen(fd, (int)a[1])); break;
    case NET_ACCEPT: {
        int nfd = accept(fd, (struct sockaddr *)&sa, &sl);
        if (nfd < 0) { result(a, -1); break; }
        a[10] = ntohl(sa.sin_addr.s_addr);
        a[11] = ntohs(sa.sin_port);
        result(a, guest_slot(nfd));
        break;
    }
    case NET_CONNECT:
        addr_make(&sa, a[1], a[2]);
        result(a, connect(fd, (struct sockaddr *)&sa, sizeof sa));
        break;
    case NET_SEND: result(a, send(fd, ram + a[1], a[2], (a[3] & 1u) ? MSG_OOB : 0)); break;
    case NET_RECV: {
        ssize_t n = recv(fd, ram + a[1], a[2], (a[3] & 2u) ? MSG_PEEK : 0);
        result(a, n);
        break;
    }
    case NET_SENDTO:
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
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &v, &vl);
            if (v) v = (int)wsa_of(v);
        } else if (a[1] == 0xFFFFu && a[2] == 0x1008) {   /* SO_TYPE */
            getsockopt(fd, SOL_SOCKET, SO_TYPE, &v, &vl);
            v = v == SOCK_DGRAM ? 2 : 1;
        }
        a[10] = (uint32_t)v;
        a[8] = 0;
        break;
    }
    case NET_SOCKNAME: case NET_PEERNAME: {
        int r = op == NET_SOCKNAME ? getsockname(fd, (struct sockaddr *)&sa, &sl) : getpeername(fd, (struct sockaddr *)&sa, &sl);
        if (r == 0) { a[10] = ntohl(sa.sin_addr.s_addr); a[11] = ntohs(sa.sin_port); }
        result(a, r);
        break;
    }
    default: a[8] = UINT32_MAX; a[9] = 10045; break;
    }
}
