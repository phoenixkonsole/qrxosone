/*
 * libqrxaroscompat, part 2: sockets, poll, and the descriptor functions around them, for
 * m68k64-unknown-aros (QRX OS One 64).
 *
 * QRX OS One 64 has no network stack of its own: the 68100EC64 board passes the guest's socket
 * calls to the host (board host call, network service 4, engine/full68k/qrx_hostnet.c), the same
 * service QRXHostSocket's bsdsocket.library uses. IPv4, TCP and UDP; the host keeps at most 64
 * sockets for the whole system, non-blocking; blocking here is a wait on the host's poll.
 *
 * Socket descriptors are QRX_SOCK_FD + the host's slot, far above AROS's own descriptors, so one
 * descriptor space serves both: close, read, write, fcntl and ioctl here take sockets and pass
 * every other descriptor to AROS's C library (its __<name>_CrtBase_wrapper stubs).
 *
 * poll: sockets ask the host, pipes qrxpipe-handler, consoles WaitForChar; files are always ready.
 * Buffers given to the host must lie below 4 GB (its service takes 32-bit guest addresses);
 * larger ones go through a bounce buffer on the stack.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dosextens.h>

#define QRX_SOCK_FD 0x40000
#define NSOCK 64

#define BOARD_ID_REG 0xFFFFFFFF00000000UL
#define BOARD_HOST_CALL 0xFFFFFFFF00000400UL
#define BOARD_MAGIC 0x5152584543363442UL
#define SVC_NET 4u
enum { NET_SOCKET = 1, NET_BIND, NET_LISTEN, NET_ACCEPT, NET_CONNECT, NET_SEND, NET_RECV, NET_SENDTO, NET_RECVFROM,
       NET_POLL, NET_CLOSE, NET_SHUTDOWN, NET_SETOPT, NET_GETOPT, NET_SOCKNAME, NET_PEERNAME, NET_RESOLVE, NET_HOSTNAME,
       NET_RANDOM };
/* IPv6 (qrx_hostnet.h): the address as a 24-byte record in guest memory, 16 bytes address
 * (IPv4 as ::ffff:a.b.c.d), port, scope id */
enum { NET_SOCKET6 = 23, NET_BIND6, NET_CONNECT6, NET_SENDTO6, NET_RECVFROM6, NET_ACCEPT6, NET_SOCKNAME6,
       NET_PEERNAME6, NET_RESOLVE6 };
#define REC6 24

/* AROS's (BSD) values, as in its headers and the libc crate */
#define AF_INET_ 2
#define AF_INET6_ 28
#define SOCK_STREAM_ 1
#define SOCK_DGRAM_ 2
#define SOL_SOCKET_ 0xFFFF
#define SO_ERROR_ 0x1007
#define SO_TYPE_ 0x1008
#define SO_SNDTIMEO_ 0x1005
#define SO_RCVTIMEO_ 0x1006
#define IPPROTO_TCP_ 6
#define MSG_PEEK_ 2
#define MSG_OOB_ 1
#define MSG_DONTWAIT_ 0x80
#define F_GETFD_ F_GETFD       /* AROS's own numbers (2, 3, 4, 5; O_NONBLOCK 2048), not BSD's */
#define F_SETFD_ F_SETFD
#define F_GETFL_ F_GETFL
#define F_SETFL_ F_SETFL
#define O_NONBLOCK_ O_NONBLOCK
#define FIONBIO_ 0x8008667EUL      /* AROS: _IOW(..., long), and long is 8 bytes here */
#define FIONREAD_ 0x4008667FUL     /* callers pass an int, as elsewhere */
#define FIOCLEX_ 0x20006601UL
#define FIONCLEX_ 0x20006602UL

typedef uint32_t qsocklen_t;
struct qsockaddr_in { uint8_t len, family; uint16_t port; uint32_t addr; uint8_t zero[8]; };
struct qaddrinfo {      /* AROS's netdb.h: the first four are long */
    long ai_flags, ai_family, ai_socktype, ai_protocol;
    size_t ai_addrlen;
    char *ai_canonname;
    void *ai_addr;
    struct qaddrinfo *ai_next;
};
struct qsockaddr_in6 { uint8_t len, family; uint16_t port; uint32_t flowinfo; uint8_t addr[16]; uint32_t scope; };
struct qpollfd { int fd; short events; short revents; };
#define POLLIN_ 0x1
#define POLLPRI_ 0x2
#define POLLOUT_ 0x4
#define POLLERR_ 0x8
#define POLLHUP_ 0x10
#define POLLNVAL_ 0x20
#define POLLRDNORM_ 0x40

/* AROS's C library, for the descriptors that are not sockets */
int __close_CrtBase_wrapper(int);
ssize_t __read_CrtBase_wrapper(int, void *, size_t);
ssize_t __write_CrtBase_wrapper(int, const void *, size_t);
int __fcntl_CrtBase_wrapper(int, int, ...);
int __ioctl_CrtBase_wrapper(int, int, ...);
int __get_default_file(int fd, long *fh);
int qrx_is_shm_fd(int fd);             /* qrxshm.c */
int qrx_shm_close(int fd);
int qrx_shm_fcntl(int fd, int cmd, long arg);

/* pend_err: an error the host reported with a poll. Its poll reads SO_ERROR, which clears it on
 * the host, so it is kept here until getsockopt(SO_ERROR) or connect takes it. */
static struct { uint8_t open, nonblock, dgram, v6; uint32_t rcv_ms, snd_ms; int pend_err; } sk[NSOCK];

static int sock_of(int fd) { int s = fd - QRX_SOCK_FD; return s >= 0 && s < NSOCK && sk[s].open ? s : -1; }
static int is_sockfd(int fd) { return fd >= QRX_SOCK_FD && fd < QRX_SOCK_FD + NSOCK; }

static int errno_of(uint32_t w)
{
    switch (w) {
    case 10035: return EAGAIN;       case 10036: return EINPROGRESS;  case 10037: return EALREADY;
    case 10038: return EBADF;        case 10040: return EMSGSIZE;     case 10043: return EPROTONOSUPPORT;
    case 10045: return EOPNOTSUPP;   case 10048: return EADDRINUSE;   case 10049: return EADDRNOTAVAIL;
    case 10050: return ENETDOWN;     case 10051: return ENETUNREACH;  case 10053: return ECONNABORTED;
    case 10054: return ECONNRESET;   case 10056: return EISCONN;      case 10057: return ENOTCONN;
    case 10060: return ETIMEDOUT;    case 10061: return ECONNREFUSED; case 10065: return EHOSTUNREACH;
    case 10022: return EINVAL;       case 10013: return EACCES;       case 10024: return EMFILE;
    case 10014: return EFAULT;
    default: return EIO;
    }
}

int qrx_host_door(void)
{
    static int state;
    if (!state) state = *(volatile uint64_t *)BOARD_ID_REG == BOARD_MAGIC ? 1 : -1;
    return state > 0;
}

/* one call of the network service: a[] arguments, then q[12] result, q[13] error, q[14..16] extra */
static int net(uint32_t *q, uint32_t op)
{
    if (!qrx_host_door()) { errno = ENETDOWN; return -1; }
    q[0] = SVC_NET; q[1] = 0; q[2] = 0; q[3] = op;
    q[12] = UINT32_MAX; q[13] = 10050;
    __asm__ volatile("" ::: "memory");
    *(volatile uint64_t *)BOARD_HOST_CALL = (uint64_t)(uintptr_t)q;
    __asm__ volatile("" ::: "memory");
    if (q[12] == UINT32_MAX) { errno = errno_of(q[13]); return -1; }
    return (int)q[12];
}
#define BLOCK(q) uint32_t q[32] __attribute__((aligned(8))); memset(q, 0, sizeof q)

/* the host's random bytes (getentropy on the host): for std's random and getentropy here */
int qrx_host_random(void *buf, size_t n)
{
    uint8_t tmp[256] __attribute__((aligned(8)));
    uint8_t *p = buf;
    while (n) {
        size_t k = n < sizeof tmp ? n : sizeof tmp;
        BLOCK(q);
        q[5] = (uint32_t)(uintptr_t)tmp; q[6] = (uint32_t)k;
        if ((uintptr_t)tmp >> 32 || net(q, NET_RANDOM) != (int)k) return -1;
        memcpy(p, tmp, k);
        memset(tmp, 0, k);
        p += k; n -= k;
    }
    return 0;
}

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
/* a short sleep that grows while nothing happens: 1, 2, 4 ... 16 ms */
static void backoff(unsigned *step)
{
    usleep(*step * 1000u);
    if (*step < 16) *step *= 2;
}
static int host_poll(int s, uint32_t mask, uint32_t *avail)
{
    BLOCK(q);
    q[4] = (uint32_t)s; q[5] = mask | (avail ? 8u : 0u);
    int r = net(q, NET_POLL);
    if (r >= 0 && q[13]) sk[s].pend_err = errno_of(q[13]);
    if (r >= 0 && avail) *avail = q[14];
    return r;
}
/* waits until socket s is ready for mask (1 read, 2 write); 0 ready, -1 timeout or error */
static int wait_sock(int s, uint32_t mask, uint32_t timeout_ms)
{
    uint64_t end = timeout_ms ? now_ms() + timeout_ms : 0;
    unsigned step = 1;
    for (;;) {
        int r = host_poll(s, mask, NULL);
        if (r < 0) return -1;
        if (r & (int)mask) return 0;
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) { errno = EINTR; return -1; }
        if (end && now_ms() >= end) { errno = EAGAIN; return -1; }
        backoff(&step);
    }
}
static int get_in(const void *sa, qsocklen_t l, uint32_t *ip, uint32_t *port)
{
    const struct qsockaddr_in *in = sa;
    if (!sa || l < 8) { errno = EINVAL; return -1; }
    if (in->family != AF_INET_) { errno = EAFNOSUPPORT; return -1; }
    *ip = in->addr; *port = in->port;      /* network order is the 68k's own: no swap */
    return 0;
}
static void put_in(void *sa, qsocklen_t *l, uint32_t ip, uint32_t port)
{
    struct qsockaddr_in in;
    if (!sa || !l) return;
    memset(&in, 0, sizeof in);
    in.len = sizeof in; in.family = AF_INET_; in.port = (uint16_t)port; in.addr = ip;
    memcpy(sa, &in, *l < sizeof in ? *l : sizeof in);
    *l = sizeof in;
}
/* a socket address of either family as a door record (an IPv4 one v4-mapped) */
static int get_rec(const void *sa, qsocklen_t l, uint8_t *rec)
{
    memset(rec, 0, REC6);
    if (!sa || l < 2) { errno = EINVAL; return -1; }
    uint8_t fam = ((const uint8_t *)sa)[1];
    if (fam == AF_INET_) {
        uint32_t ip, port;
        if (get_in(sa, l, &ip, &port)) return -1;
        rec[10] = rec[11] = 0xFF;
        memcpy(rec + 12, &ip, 4);
        uint32_t p32 = port; memcpy(rec + 16, &p32, 4);
        return 0;
    }
    if (fam != AF_INET6_ || l < sizeof(struct qsockaddr_in6)) { errno = EAFNOSUPPORT; return -1; }
    const struct qsockaddr_in6 *in6 = sa;
    memcpy(rec, in6->addr, 16);
    uint32_t p32 = in6->port; memcpy(rec + 16, &p32, 4);
    memcpy(rec + 20, &in6->scope, 4);
    return 0;
}
/* a record as the socket address the caller gets: IPv6 for an IPv6 socket, else IPv4 */
static void put_rec(void *sa, qsocklen_t *l, const uint8_t *rec, int v6)
{
    if (!sa || !l) return;
    uint32_t port, ip;
    memcpy(&port, rec + 16, 4);
    if (!v6) { memcpy(&ip, rec + 12, 4); put_in(sa, l, ip, port); return; }
    struct qsockaddr_in6 in6;
    memset(&in6, 0, sizeof in6);
    in6.len = sizeof in6; in6.family = AF_INET6_; in6.port = (uint16_t)port;
    memcpy(in6.addr, rec, 16);
    memcpy(&in6.scope, rec + 20, 4);
    memcpy(sa, &in6, *l < sizeof in6 ? *l : sizeof in6);
    *l = sizeof in6;
}
static int new_slot(int r, int dgram)
{
    if (r < 0) return -1;
    if (r >= NSOCK) { BLOCK(c); c[4] = (uint32_t)r; net(c, NET_CLOSE); errno = EMFILE; return -1; }
    memset(&sk[r], 0, sizeof sk[r]);
    sk[r].open = 1; sk[r].dgram = (uint8_t)dgram;
    return QRX_SOCK_FD + r;
}

int socket(int domain, int type, int protocol)
{
    (void)protocol;
    if (domain != AF_INET_ && domain != AF_INET6_) { errno = EAFNOSUPPORT; return -1; }
    type &= 0xFF;
    if (type != SOCK_STREAM_ && type != SOCK_DGRAM_) { errno = EPROTONOSUPPORT; return -1; }
    BLOCK(q);
    q[4] = type == SOCK_DGRAM_ ? 2 : 1;
    int fd = new_slot(net(q, domain == AF_INET6_ ? NET_SOCKET6 : NET_SOCKET), type == SOCK_DGRAM_);
    if (fd >= 0) sk[fd - QRX_SOCK_FD].v6 = domain == AF_INET6_;
    return fd;
}

int bind(int fd, const void *sa, qsocklen_t l)
{
    int s = sock_of(fd);
    if (s < 0) { errno = ENOTSOCK; return -1; }
    BLOCK(q);
    uint8_t rec[REC6] __attribute__((aligned(8)));
    if (sk[s].v6) {
        if (get_rec(sa, l, rec)) return -1;
        q[5] = (uint32_t)(uintptr_t)rec;
    } else if (get_in(sa, l, &q[5], &q[6])) return -1;
    q[4] = (uint32_t)s;
    return net(q, sk[s].v6 ? NET_BIND6 : NET_BIND) < 0 ? -1 : 0;
}
int listen(int fd, int backlog)
{
    int s = sock_of(fd);
    if (s < 0) { errno = ENOTSOCK; return -1; }
    BLOCK(q);
    q[4] = (uint32_t)s; q[5] = (uint32_t)backlog;
    return net(q, NET_LISTEN) < 0 ? -1 : 0;
}
int accept(int fd, void *sa, qsocklen_t *l)
{
    int s = sock_of(fd);
    if (s < 0) { errno = ENOTSOCK; return -1; }
    for (;;) {
        BLOCK(q);
        uint8_t rec[REC6] __attribute__((aligned(8)));
        q[4] = (uint32_t)s; q[5] = (uint32_t)(uintptr_t)rec;
        int r = net(q, sk[s].v6 ? NET_ACCEPT6 : NET_ACCEPT);
        if (r >= 0) {
            int nfd = new_slot(r, 0);
            if (nfd >= 0) {
                sk[nfd - QRX_SOCK_FD].v6 = sk[s].v6;
                if (sk[s].v6) put_rec(sa, l, rec, 1); else put_in(sa, l, q[14], q[15]);
            }
            return nfd;
        }
        if (errno != EAGAIN || sk[s].nonblock) return -1;
        if (wait_sock(s, 1, sk[s].rcv_ms)) return -1;
    }
}
int accept4(int fd, void *sa, qsocklen_t *l, int flags)
{
    int r = accept(fd, sa, l);
    if (r >= 0 && (flags & O_NONBLOCK_)) sk[r - QRX_SOCK_FD].nonblock = 1;
    return r;
}
static int sock_error(int s)
{
    if (sk[s].pend_err) { int e = sk[s].pend_err; sk[s].pend_err = 0; return e; }
    BLOCK(q);
    q[4] = (uint32_t)s; q[5] = SOL_SOCKET_; q[6] = SO_ERROR_;
    if (net(q, NET_GETOPT) < 0) return errno;
    return q[14] ? errno_of(q[14]) : 0;
}
int connect(int fd, const void *sa, qsocklen_t l)
{
    int s = sock_of(fd);
    if (s < 0) { errno = ENOTSOCK; return -1; }
    BLOCK(q);
    uint8_t rec[REC6] __attribute__((aligned(8)));
    if (sk[s].v6) {
        if (get_rec(sa, l, rec)) return -1;
        q[5] = (uint32_t)(uintptr_t)rec;
    } else if (get_in(sa, l, &q[5], &q[6])) return -1;
    q[4] = (uint32_t)s;
    if (net(q, sk[s].v6 ? NET_CONNECT6 : NET_CONNECT) >= 0) return 0;
    if (errno != EINPROGRESS && errno != EAGAIN) return -1;
    if (sk[s].nonblock) { errno = EINPROGRESS; return -1; }
    if (wait_sock(s, 2, 0)) return -1;
    int e = sock_error(s);
    if (e) { errno = e; return -1; }
    return 0;
}
int shutdown(int fd, int how)
{
    int s = sock_of(fd);
    if (s < 0) { errno = ENOTSOCK; return -1; }
    BLOCK(q);
    q[4] = (uint32_t)s; q[5] = (uint32_t)how;
    return net(q, NET_SHUTDOWN) < 0 ? -1 : 0;
}
static int name_of(int fd, void *sa, qsocklen_t *l, uint32_t op)
{
    int s = sock_of(fd);
    if (s < 0) { errno = ENOTSOCK; return -1; }
    BLOCK(q);
    uint8_t rec[REC6] __attribute__((aligned(8)));
    q[4] = (uint32_t)s; q[5] = (uint32_t)(uintptr_t)rec;
    if (sk[s].v6) op = op == NET_SOCKNAME ? NET_SOCKNAME6 : NET_PEERNAME6;
    if (net(q, op) < 0) return -1;
    if (sk[s].v6) put_rec(sa, l, rec, 1); else put_in(sa, l, q[14], q[15]);
    return 0;
}
int getsockname(int fd, void *sa, qsocklen_t *l) { return name_of(fd, sa, l, NET_SOCKNAME); }
int getpeername(int fd, void *sa, qsocklen_t *l) { return name_of(fd, sa, l, NET_PEERNAME); }

static uint32_t tv_ms(const void *v, qsocklen_t l)
{
    struct timeval tv;
    if (l < sizeof tv) return 0;
    memcpy(&tv, v, sizeof tv);
    uint64_t ms = (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
    if (!ms && (tv.tv_sec || tv.tv_usec)) ms = 1;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}
int setsockopt(int fd, int level, int name, const void *v, qsocklen_t l)
{
    int s = sock_of(fd);
    if (s < 0) { errno = ENOTSOCK; return -1; }
    if (level == SOL_SOCKET_ && name == SO_RCVTIMEO_) { sk[s].rcv_ms = tv_ms(v, l); return 0; }
    if (level == SOL_SOCKET_ && name == SO_SNDTIMEO_) { sk[s].snd_ms = tv_ms(v, l); return 0; }
    int val = 0;
    if (v && l >= sizeof(int)) memcpy(&val, v, sizeof val);
    BLOCK(q);
    q[4] = (uint32_t)s; q[5] = (uint32_t)level; q[6] = (uint32_t)name; q[7] = (uint32_t)val;
    return net(q, NET_SETOPT) < 0 ? -1 : 0;     /* the host ignores options it does not know */
}
int getsockopt(int fd, int level, int name, void *v, qsocklen_t *l)
{
    int s = sock_of(fd);
    if (s < 0) { errno = ENOTSOCK; return -1; }
    if (!v || !l) { errno = EFAULT; return -1; }
    if (level == SOL_SOCKET_ && (name == SO_RCVTIMEO_ || name == SO_SNDTIMEO_)) {
        uint32_t ms = name == SO_RCVTIMEO_ ? sk[s].rcv_ms : sk[s].snd_ms;
        struct timeval tv;
        memset(&tv, 0, sizeof tv);
        tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
        memcpy(v, &tv, *l < sizeof tv ? *l : sizeof tv);
        *l = sizeof tv;
        return 0;
    }
    int val = 0;
    if (level == SOL_SOCKET_ && name == SO_ERROR_) val = sock_error(s);
    else if (level == SOL_SOCKET_ && name == SO_TYPE_) val = sk[s].dgram ? SOCK_DGRAM_ : SOCK_STREAM_;
    else {
        BLOCK(q);
        q[4] = (uint32_t)s; q[5] = (uint32_t)level; q[6] = (uint32_t)name;
        if (net(q, NET_GETOPT) < 0) return -1;
        val = (int)q[14];
    }
    memcpy(v, &val, *l < sizeof val ? *l : sizeof val);
    *l = sizeof val;
    return 0;
}

/* send/recv with the host: through a bounce buffer when buf lies above 4 GB */
static ssize_t xfer(int s, void *buf, size_t n, int flags, uint32_t op, const void *to, qsocklen_t tl,
                    void *from, qsocklen_t *fl)
{
    int is_send = op == NET_SEND || op == NET_SENDTO;
    uint32_t tmo = is_send ? sk[s].snd_ms : sk[s].rcv_ms;
    uint8_t bounce[4096] __attribute__((aligned(8)));
    for (;;) {
        BLOCK(q);
        void *p = buf;
        size_t k = n > 0x7FFFFFFF ? 0x7FFFFFFF : n;
        if ((uintptr_t)buf + k > UINT32_MAX) {
            if (k > sizeof bounce) k = sizeof bounce;
            p = bounce;
            if (is_send) memcpy(bounce, buf, k);
        }
        q[4] = (uint32_t)s; q[5] = (uint32_t)(uintptr_t)p; q[6] = (uint32_t)k;
        q[7] = (uint32_t)(((flags & MSG_PEEK_) ? 2u : 0u) | ((flags & MSG_OOB_) ? 1u : 0u));
        uint8_t rec[REC6] __attribute__((aligned(8)));
        uint32_t o = op == NET_SENDTO && !to ? NET_SEND : op;
        if (sk[s].v6 && (o == NET_SENDTO || o == NET_RECVFROM)) {
            if (o == NET_SENDTO && get_rec(to, tl, rec)) return -1;
            q[8] = (uint32_t)(uintptr_t)rec;
            o = o == NET_SENDTO ? NET_SENDTO6 : NET_RECVFROM6;
        } else if (op == NET_SENDTO && to) { if (get_in(to, tl, &q[8], &q[9])) return -1; }
        int r = net(q, o);
        if (r >= 0) {
            if (!is_send && p == bounce) memcpy(buf, bounce, (size_t)r);
            if (o == NET_RECVFROM6) put_rec(from, fl, rec, 1);
            else if (op == NET_RECVFROM) put_in(from, fl, q[14], q[15]);
            return r;
        }
        if (errno != EAGAIN || sk[s].nonblock || (flags & MSG_DONTWAIT_)) return -1;
        if (wait_sock(s, is_send ? 2 : 1, tmo)) return -1;
    }
}
ssize_t send(int fd, const void *b, size_t n, int f)
{
    int s = sock_of(fd);
    if (s < 0) { errno = ENOTSOCK; return -1; }
    return xfer(s, (void *)b, n, f, NET_SEND, NULL, 0, NULL, NULL);
}
ssize_t recv(int fd, void *b, size_t n, int f)
{
    int s = sock_of(fd);
    if (s < 0) { errno = ENOTSOCK; return -1; }
    return xfer(s, b, n, f, NET_RECV, NULL, 0, NULL, NULL);
}
ssize_t sendto(int fd, const void *b, size_t n, int f, const void *to, qsocklen_t tl)
{
    int s = sock_of(fd);
    if (s < 0) { errno = ENOTSOCK; return -1; }
    return xfer(s, (void *)b, n, f, NET_SENDTO, to, tl, NULL, NULL);
}
ssize_t recvfrom(int fd, void *b, size_t n, int f, void *from, qsocklen_t *fl)
{
    int s = sock_of(fd);
    if (s < 0) { errno = ENOTSOCK; return -1; }
    return xfer(s, b, n, f, NET_RECVFROM, NULL, 0, from, fl);
}

/* ---- the descriptor functions: sockets here, the rest to AROS */
/* O_NONBLOCK for AROS's own descriptors (pipes of PIPE:, consoles): AROS's fcntl does not know it
 * (the handlers answer "action unknown"), so it is kept here and read() checks first whether
 * anything is waiting (file_ready, as poll() does) and else fails with EAGAIN. Writes do not block. */
#define NB_FDS 1024
static unsigned char file_nonblock[NB_FDS];
static short file_ready(int fd, short ev);
static int file_is_nonblock(int fd) { return fd >= 0 && fd < NB_FDS && file_nonblock[fd]; }
int close(int fd)
{
    if (fd >= 0 && fd < NB_FDS) file_nonblock[fd] = 0;
    if (qrx_is_shm_fd(fd)) return qrx_shm_close(fd);
    if (!is_sockfd(fd)) return __close_CrtBase_wrapper(fd);
    int s = sock_of(fd);
    if (s < 0) { errno = EBADF; return -1; }
    BLOCK(q);
    q[4] = (uint32_t)s;
    sk[s].open = 0;
    return net(q, NET_CLOSE) < 0 ? -1 : 0;
}
ssize_t read(int fd, void *b, size_t n)
{
    if (!is_sockfd(fd)) {
        if (n && file_is_nonblock(fd) && !(file_ready(fd, POLLIN_) & (POLLIN_ | POLLHUP_ | POLLNVAL_))) {
            errno = EAGAIN;
            return -1;
        }
        return __read_CrtBase_wrapper(fd, b, n);
    }
    return recv(fd, b, n, 0);
}
ssize_t write(int fd, const void *b, size_t n)
{
    if (!is_sockfd(fd)) return __write_CrtBase_wrapper(fd, b, n);
    return send(fd, b, n, 0);
}
/* readv/writev (declared by AROS's sys/uio.h, not implemented): one buffer at a time. A socket or
 * pipe may take less than asked; then the call returns what went through, as a short transfer. */
struct qiovec { void *base; size_t len; };
ssize_t writev(int fd, const struct qiovec *iov, int cnt)
{
    ssize_t total = 0;
    if (cnt < 0) { errno = EINVAL; return -1; }
    for (int i = 0; i < cnt; i++) {
        if (!iov[i].len) continue;
        ssize_t r = write(fd, iov[i].base, iov[i].len);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].len) break;
    }
    return total;
}
ssize_t readv(int fd, const struct qiovec *iov, int cnt)
{
    ssize_t total = 0;
    if (cnt < 0) { errno = EINVAL; return -1; }
    for (int i = 0; i < cnt; i++) {
        if (!iov[i].len) continue;
        ssize_t r = read(fd, iov[i].base, iov[i].len);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].len) break;
    }
    return total;
}
int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    if (qrx_is_shm_fd(fd)) return qrx_shm_fcntl(fd, cmd, arg);
    if (!is_sockfd(fd)) {
        /* close-on-exec: AROS has no such flag; qrx_spawn closes the parent's other descriptors */
        if (cmd == F_GETFD_) return __fcntl_CrtBase_wrapper(fd, F_GETFL_) < 0 ? -1 : 0;
        if (cmd == F_SETFD_) return __fcntl_CrtBase_wrapper(fd, F_GETFL_) < 0 ? -1 : 0;
        if ((cmd == F_GETFL_ || cmd == F_SETFL_) && fd >= 0 && fd < NB_FDS) {
            long fh = 0;
            if (__get_default_file(fd, &fh) || !fh) { errno = EBADF; return -1; }
            int e = errno;
            int fl = __fcntl_CrtBase_wrapper(fd, F_GETFL_);
            if (fl < 0) { fl = O_RDWR; errno = e; }      /* the handler does not answer: read/write */
            if (cmd == F_GETFL_) return (fl & ~O_NONBLOCK_) | (file_nonblock[fd] ? O_NONBLOCK_ : 0);
            file_nonblock[fd] = (arg & O_NONBLOCK_) != 0;
            /* the other status flags (O_APPEND) still go to AROS where it takes them */
            if ((arg & ~O_NONBLOCK_) != (fl & ~O_NONBLOCK_) && __fcntl_CrtBase_wrapper(fd, F_SETFL_, arg & ~O_NONBLOCK_) < 0)
                return -1;
            return 0;
        }
        return __fcntl_CrtBase_wrapper(fd, cmd, arg);
    }
    int s = sock_of(fd);
    if (s < 0) { errno = EBADF; return -1; }
    switch (cmd) {
    case F_GETFD_: case F_SETFD_: return 0;
    case F_GETFL_: return O_RDWR | (sk[s].nonblock ? O_NONBLOCK_ : 0);
    case F_SETFL_: sk[s].nonblock = (arg & O_NONBLOCK_) != 0; return 0;
    default: errno = EINVAL; return -1;
    }
}
int ioctl(int fd, int ireq, ...)
{
    va_list ap;
    va_start(ap, ireq);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    unsigned long req = (uint32_t)ireq;     /* AROS declares the request as int, like the libc crate */
    if (req == FIOCLEX_ || req == FIONCLEX_) {
        /* see fcntl: accepted for every descriptor that exists */
        if (is_sockfd(fd)) return sock_of(fd) < 0 ? (errno = EBADF, -1) : 0;
        return __fcntl_CrtBase_wrapper(fd, F_GETFL_) < 0 ? -1 : 0;
    }
    if (!is_sockfd(fd)) return __ioctl_CrtBase_wrapper(fd, ireq, arg);
    int s = sock_of(fd);
    if (s < 0) { errno = EBADF; return -1; }
    if (req == FIONBIO_) { sk[s].nonblock = arg && *(int *)arg; return 0; }
    if (req == FIONREAD_) {
        uint32_t avail = 0;
        if (host_poll(s, 1, &avail) < 0) return -1;
        if (arg) *(int *)arg = (int)avail;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

/* ---- poll */
/* qrxpipe-handler's private packets (aros-qrx/qrxpipe): other handlers do not know them */
#define ACTION_QRX_PIPE_AVAIL 0x51525001
#define ACTION_QRX_PIPE_NAME 0x51525002
static int pipe_packet(BPTR bfh, LONG action, SIPTR a2, SIPTR a3, SIPTR *res)
{
    struct FileHandle *fh = BADDR(bfh);
    if (!fh || !fh->fh_Type) return -1;
    SIPTR r = DoPkt(fh->fh_Type, action, fh->fh_Arg1, a2, a3, 0, 0);
    if (IoErr() == ERROR_ACTION_NOT_KNOWN) return -1;     /* not a pipe */
    *res = r;
    return 0;
}
/* the name of the pipe behind fh (without PIPE:), 0; -1 when fh is no pipe */
int qrx_pipe_name(BPTR fh, char *buf, size_t n)
{
    SIPTR r;
    return pipe_packet(fh, ACTION_QRX_PIPE_NAME, (SIPTR)buf, (SIPTR)n, &r) == 0 && r ? 0 : -1;
}
static short file_ready(int fd, short ev)
{
    long fh = 0;
    SIPTR avail;
    if (__get_default_file(fd, &fh) || !fh) return POLLNVAL_;
    short r = ev & POLLOUT_;           /* writes never block on AROS's handlers here */
    if (ev & (POLLIN_ | POLLRDNORM_)) {
        if (pipe_packet((BPTR)fh, ACTION_QRX_PIPE_AVAIL, 0, 0, &avail) == 0) {
            if (avail != 0) r |= ev & (POLLIN_ | POLLRDNORM_);   /* data, or -1: end of file */
            if (avail < 0) r |= POLLHUP_;
        } else if (IsInteractive((BPTR)fh)) {
            if (WaitForChar((BPTR)fh, 0)) r |= ev & (POLLIN_ | POLLRDNORM_);
        } else r |= ev & (POLLIN_ | POLLRDNORM_);     /* a file: read never blocks */
    }
    return r;
}
int poll(struct qpollfd *fds, unsigned int nfds, int timeout)
{
    uint64_t end = timeout > 0 ? now_ms() + (uint64_t)timeout : 0;
    unsigned step = 1;
    for (;;) {
        int ready = 0;
        for (unsigned i = 0; i < nfds; i++) {
            short ev = fds[i].events, r = 0;
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            if (is_sockfd(fds[i].fd)) {
                int s = sock_of(fds[i].fd);
                if (s < 0) r = POLLNVAL_;
                else {
                    uint32_t m = (ev & (POLLIN_ | POLLRDNORM_) ? 1u : 0u) | (ev & POLLOUT_ ? 2u : 0u) | (ev & POLLPRI_ ? 4u : 0u);
                    int h = host_poll(s, m, NULL);
                    if (h < 0) r = POLLERR_;
                    else {
                        if (h & 1) r |= ev & (POLLIN_ | POLLRDNORM_);
                        if (h & 2) r |= ev & POLLOUT_;
                        if (h & 4) r |= ev & POLLPRI_;
                        if (sk[s].pend_err) r |= POLLERR_;
                    }
                }
            } else r = file_ready(fds[i].fd, ev);
            fds[i].revents = r;
            if (r) ready++;
        }
        if (ready || timeout == 0) return ready;
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) { errno = EINTR; return -1; }
        if (timeout > 0 && now_ms() >= end) return 0;
        backoff(&step);
    }
}

/* ---- names */
static int parse_ipv4(const char *s, uint32_t *ip)
{
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t x = 0;
        while (*s >= '0' && *s <= '9') { x = x * 10 + (uint32_t)(*s++ - '0'); if (x > 255) return -1; }
        v = v << 8 | x;
        if (part < 3 && *s++ != '.') return -1;
    }
    if (*s) return -1;
    *ip = v;
    return 0;
}
int qrx_resolve4(const char *name, uint32_t *ip)
{
    if (!parse_ipv4(name, ip)) return 0;
    if (!strcmp(name, "localhost")) { *ip = 0x7F000001u; return 0; }
    size_t n = strlen(name);
    char tmp[256] __attribute__((aligned(8)));
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, name, n + 1);
    BLOCK(q);
    q[5] = (uint32_t)(uintptr_t)tmp;
    if ((uintptr_t)tmp >> 32 || net(q, NET_RESOLVE) < 0) return -1;
    *ip = q[14];
    return 0;
}
#define EAI_FAMILY_ 5
#define EAI_MEMORY_ 6
#define EAI_NONAME_ 8
#define EAI_SERVICE_ 9
#define EAI_SOCKTYPE_ 10
#define AI_PASSIVE_ 1
#define AI_NUMERICHOST_ 4
/* inet_pton/inet_ntop for both families (AROS's headers declare them, its C library has none) */
static int pton6(const char *src, uint8_t *dst)
{
    uint16_t w[8] = { 0 };
    int n = 0, gap = -1;
    const char *p = src;
    if (*p == ':') { if (p[1] != ':') return 0; p++; }
    while (*p) {
        if (*p == ':') {
            if (gap >= 0) return 0;
            gap = n; p++;
            if (!*p) break;
            continue;
        }
        if (n == 8) return 0;
        /* an IPv4 tail: a.b.c.d */
        const char *dot = strchr(p, '.');
        if (dot && !strchr(p, ':')) {
            uint32_t ip;
            if (n > 6 || parse_ipv4(p, &ip)) return 0;
            w[n++] = (uint16_t)(ip >> 16); w[n++] = (uint16_t)ip;
            p += strlen(p);
            break;
        }
        unsigned v = 0, k = 0;
        while (k < 4 && ((*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'f'))) {
            v = v * 16 + (unsigned)(*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10); p++; k++;
        }
        if (!k) return 0;
        w[n++] = (uint16_t)v;
        if (*p == ':') { if (p[1] == ':') continue; p++; if (!*p) return 0; }
        else if (*p) return 0;
    }
    if (gap >= 0) {
        int tail = n - gap;
        if (n == 8) return 0;
        memmove(w + 8 - tail, w + gap, (size_t)tail * 2);
        memset(w + gap, 0, (size_t)(8 - n) * 2);
    } else if (n != 8) return 0;
    for (int k = 0; k < 8; k++) { dst[2 * k] = (uint8_t)(w[k] >> 8); dst[2 * k + 1] = (uint8_t)w[k]; }
    return 1;
}
int inet_pton(int af, const char *src, void *dst)
{
    if (af == AF_INET_) { uint32_t ip; if (parse_ipv4(src, &ip)) return 0; memcpy(dst, &ip, 4); return 1; }
    if (af == AF_INET6_) return pton6(src, dst);
    errno = EAFNOSUPPORT;
    return -1;
}
const char *inet_ntop(int af, const void *src, char *dst, qsocklen_t size)
{
    char b[48];
    const uint8_t *a = src;
    if (af == AF_INET_) snprintf(b, sizeof b, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    else if (af == AF_INET6_) {
        static const uint8_t mapped[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xFF,0xFF };
        if (!memcmp(a, mapped, 12)) snprintf(b, sizeof b, "::ffff:%u.%u.%u.%u", a[12], a[13], a[14], a[15]);
        else {
            /* the longest run of zero words (two or more) becomes :: (RFC 5952) */
            int best = -1, bestlen = 0;
            for (int k = 0; k < 8;) {
                if (a[2 * k] | a[2 * k + 1]) { k++; continue; }
                int e = k; while (e < 8 && !(a[2 * e] | a[2 * e + 1])) e++;
                if (e - k > bestlen && e - k >= 2) { best = k; bestlen = e - k; }
                k = e;
            }
            char *o = b;
            for (int k = 0; k < 8; k++) {
                if (k == best) { *o++ = ':'; if (k == 0) *o++ = ':'; k += bestlen - 1; continue; }
                o += sprintf(o, "%x", (unsigned)(a[2 * k] << 8 | a[2 * k + 1]));
                if (k < 7) *o++ = ':';
            }
            *o = 0;
        }
    } else { errno = EAFNOSUPPORT; return NULL; }
    if (strlen(b) >= size) { errno = ENOSPC; return NULL; }
    strcpy(dst, b);
    return dst;
}

void freeaddrinfo(struct qaddrinfo *ai);
static struct qaddrinfo *ai_new(long family, long socktype, const uint8_t *addr, uint32_t port)
{
    size_t sl = family == AF_INET6_ ? sizeof(struct qsockaddr_in6) : sizeof(struct qsockaddr_in);
    struct qaddrinfo *ai = calloc(1, sizeof *ai + sl);
    if (!ai) return NULL;
    if (family == AF_INET6_) {
        struct qsockaddr_in6 *in6 = (struct qsockaddr_in6 *)(ai + 1);
        in6->len = sizeof *in6; in6->family = AF_INET6_; in6->port = (uint16_t)port; memcpy(in6->addr, addr, 16);
        ai->ai_addr = in6;
    } else {
        struct qsockaddr_in *in = (struct qsockaddr_in *)(ai + 1);
        in->len = sizeof *in; in->family = AF_INET_; in->port = (uint16_t)port; memcpy(&in->addr, addr, 4);
        ai->ai_addr = in;
    }
    ai->ai_family = family;
    ai->ai_socktype = socktype ? socktype : SOCK_STREAM_;
    ai->ai_protocol = ai->ai_socktype == SOCK_DGRAM_ ? 17 : 6;
    ai->ai_addrlen = sl;
    return ai;
}
/* getaddrinfo: IPv4 and IPv6 (the host's resolver through NET_RESOLVE6, in the order it gives;
 * through Tor or WireGuard only IPv4). AF_UNSPEC gives both, IPv4 as AF_INET entries. */
int getaddrinfo(const char *node, const char *service, const struct qaddrinfo *hints, struct qaddrinfo **res)
{
    uint32_t port = 0;
    long family = hints ? hints->ai_family : 0, socktype = hints ? hints->ai_socktype : 0;
    long flags = hints ? hints->ai_flags : 0;
    *res = NULL;
    if (family != 0 && family != AF_INET_ && family != AF_INET6_) return EAI_FAMILY_;
    if (socktype != 0 && socktype != SOCK_STREAM_ && socktype != SOCK_DGRAM_) return EAI_SOCKTYPE_;
    if (!node && !service) return EAI_NONAME_;
    if (service) {
        char *e;
        unsigned long p = strtoul(service, &e, 10);
        if (*e || p > 65535) return EAI_SERVICE_;
        port = (uint32_t)p;
    }
    static const uint8_t any6[16], lo6[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
    uint8_t a6[16];
    uint32_t ip = 0;
    struct qaddrinfo *head = NULL, **tail = &head;
#define ADD(fam, addr) do { struct qaddrinfo *n_ = ai_new(fam, socktype, addr, port); \
        if (!n_) { freeaddrinfo(head); return EAI_MEMORY_; } *tail = n_; tail = &n_->ai_next; } while (0)
    if (!node) {                          /* the local addresses: any or loopback */
        int passive = (flags & AI_PASSIVE_) != 0;
        if (family == AF_INET6_ || family == 0) ADD(AF_INET6_, passive ? any6 : lo6);
        ip = passive ? 0 : 0x7F000001u;
        if (family == AF_INET_ || family == 0) ADD(AF_INET_, (uint8_t *)&ip);
        *res = head;
        return 0;
    }
    if (pton6(node, a6)) {                /* a numeric IPv6 address */
        if (family == AF_INET_) return EAI_FAMILY_;
        ADD(AF_INET6_, a6);
        *res = head;
        return 0;
    }
    if (!parse_ipv4(node, &ip) || family == AF_INET_) {   /* numeric IPv4, or IPv4 asked for */
        if (family == AF_INET6_) return EAI_FAMILY_;
        if (parse_ipv4(node, &ip) && (flags & AI_NUMERICHOST_)) return EAI_NONAME_;
        if (parse_ipv4(node, &ip) && qrx_resolve4(node, &ip)) return EAI_NONAME_;
        ADD(AF_INET_, (uint8_t *)&ip);
        *res = head;
        return 0;
    }
    if (!strcmp(node, "localhost")) {     /* IPv4 first: a server here may listen on 127.0.0.1 only */
        ip = 0x7F000001u;
        if (family != AF_INET6_) ADD(AF_INET_, (uint8_t *)&ip);
        if (family != AF_INET_) ADD(AF_INET6_, lo6);
        *res = head;
        return 0;
    }
    if (flags & AI_NUMERICHOST_) return EAI_NONAME_;   /* a name, but only numbers were asked for */
    size_t n = strlen(node);
    char name[256] __attribute__((aligned(8)));
    uint8_t recs[16 * REC6] __attribute__((aligned(8)));
    if (n >= sizeof name) return EAI_NONAME_;
    memcpy(name, node, n + 1);
    BLOCK(q);
    q[5] = (uint32_t)(uintptr_t)name; q[6] = (uint32_t)(uintptr_t)recs; q[7] = 16;
    int k = net(q, NET_RESOLVE6);
    if (k <= 0) return EAI_NONAME_;
    static const uint8_t mapped[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xFF,0xFF };
    for (int i = 0; i < k; i++) {
        const uint8_t *r = recs + i * REC6;
        if (!memcmp(r, mapped, 12)) { if (family != AF_INET6_) ADD(AF_INET_, r + 12); }
        else if (family != AF_INET_) ADD(AF_INET6_, r);
    }
#undef ADD
    if (!head) return EAI_NONAME_;
    *res = head;
    return 0;
}
/* getnameinfo: numeric only (no reverse lookup through the door; NI_NAMEREQD fails) */
#define NI_NAMEREQD_ 4
int getnameinfo(const void *sa, qsocklen_t sl, char *host, qsocklen_t hl, char *serv, qsocklen_t svl, int flags)
{
    if (!sa || sl < 2) return EAI_FAMILY_;
    uint8_t fam = ((const uint8_t *)sa)[1];
    unsigned port;
    if (flags & NI_NAMEREQD_) return EAI_NONAME_;
    if (fam == AF_INET_ && sl >= sizeof(struct qsockaddr_in)) {
        const struct qsockaddr_in *in = sa;
        if (host && hl && !inet_ntop(AF_INET_, &in->addr, host, hl)) return EAI_MEMORY_;
        port = in->port;
    } else if (fam == AF_INET6_ && sl >= sizeof(struct qsockaddr_in6)) {
        const struct qsockaddr_in6 *in6 = sa;
        if (host && hl && !inet_ntop(AF_INET6_, in6->addr, host, hl)) return EAI_MEMORY_;
        port = in6->port;
    } else return EAI_FAMILY_;
    if (serv && svl && (qsocklen_t)snprintf(serv, svl, "%u", port) >= svl) return EAI_MEMORY_;
    return 0;
}
void freeaddrinfo(struct qaddrinfo *ai)
{
    while (ai) { struct qaddrinfo *n = ai->ai_next; free(ai); ai = n; }
}
const char *gai_strerror(int e)
{
    switch (e) {
    case EAI_FAMILY_: return "address family not supported";
    case EAI_MEMORY_: return "out of memory";
    case EAI_NONAME_: return "name not known";
    case EAI_SERVICE_: return "service not supported (numeric ports only)";
    case EAI_SOCKTYPE_: return "socket type not supported";
    default: return "name resolution failed";
    }
}
int gethostname(char *name, size_t len)
{
    char tmp[256] __attribute__((aligned(8)));
    BLOCK(q);
    q[5] = (uint32_t)(uintptr_t)tmp; q[6] = sizeof tmp;
    if ((uintptr_t)tmp >> 32 || net(q, NET_HOSTNAME) < 0) return -1;
    tmp[sizeof tmp - 1] = 0;
    if (strlen(tmp) >= len) { errno = ENAMETOOLONG; return -1; }
    strcpy(name, tmp);
    return 0;
}

/* gethostbyname/gethostbyaddr (NSPR's PR_GetHostByName, the IPv4 path): through qrx_resolve4, one
 * address. As in the classic interface the result lives in static storage, one per program; NSPR
 * calls it under its own lock (_PR_HAVE_GETHOST_R is not set). */
struct qhostent { char *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list; };
int h_errno;
static struct { struct qhostent he; char name[256]; char *aliases[1]; uint32_t addr; char *list[2]; } hostbuf;
struct qhostent *gethostbyname(const char *name)
{
    uint32_t ip;
    if (!name || qrx_resolve4(name, &ip)) { h_errno = 1; /* HOST_NOT_FOUND */ return NULL; }
    snprintf(hostbuf.name, sizeof hostbuf.name, "%s", name);
    hostbuf.aliases[0] = NULL;
    hostbuf.addr = ip;                    /* network order, as qrx_resolve4 gives it */
    hostbuf.list[0] = (char *)&hostbuf.addr; hostbuf.list[1] = NULL;
    hostbuf.he.h_name = hostbuf.name; hostbuf.he.h_aliases = hostbuf.aliases;
    hostbuf.he.h_addrtype = AF_INET_; hostbuf.he.h_length = 4; hostbuf.he.h_addr_list = hostbuf.list;
    return &hostbuf.he;
}
struct qhostent *gethostbyaddr(const void *addr, qsocklen_t len, int type)
{
    (void)addr; (void)len; (void)type;
    h_errno = 1;                          /* no reverse lookup through the host door */
    return NULL;
}

/* select (AROS's sys/select.h leaves it out; bsdsocket's headers make it WaitSelect): on poll()
 * above. AROS's fd_set holds descriptors below FD_SETSIZE (64), so the door's sockets (from
 * QRX_SOCK_FD up) cannot be selected; NSPR and Gecko poll. The signal mask of WaitSelect is not
 * waited on. */
#include <sys/select.h>
int WaitSelect(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv, unsigned long *sigs)
{
    struct qpollfd p[FD_SETSIZE];
    unsigned n = 0;
    if (sigs) *sigs = 0;
    if (nfds > FD_SETSIZE) { errno = EINVAL; return -1; }
    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (r && FD_ISSET(fd, r)) ev |= POLLIN_;
        if (w && FD_ISSET(fd, w)) ev |= POLLOUT_;
        if (e && FD_ISSET(fd, e)) ev |= POLLPRI_;
        if (ev) { p[n].fd = fd; p[n].events = ev; p[n].revents = 0; n++; }
    }
    int ms = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
    int k = poll(p, n, ms);
    if (k < 0) return -1;
    if (r) FD_ZERO(r);
    if (w) FD_ZERO(w);
    if (e) FD_ZERO(e);
    int ready = 0;
    for (unsigned i = 0; i < n; i++) {
        short rv = p[i].revents;
        if (r && (rv & (POLLIN_ | POLLHUP_ | POLLERR_))) { FD_SET(p[i].fd, r); ready++; }
        if (w && (rv & (POLLOUT_ | POLLERR_))) { FD_SET(p[i].fd, w); ready++; }
        if (e && (rv & POLLPRI_)) { FD_SET(p[i].fd, e); ready++; }
    }
    return ready;
}
int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv) { return WaitSelect(nfds, r, w, e, tv, NULL); }

/* getprotobyname/getprotobynumber: the two protocols there are */
struct qprotoent { char *p_name; char **p_aliases; int p_proto; };
static char *no_aliases[1];
static struct qprotoent protos[] = { { "tcp", no_aliases, 6 }, { "udp", no_aliases, 17 }, { "ip", no_aliases, 0 } };
struct qprotoent *getprotobyname(const char *name)
{
    for (unsigned i = 0; i < sizeof protos / sizeof protos[0]; i++) if (name && !strcmp(name, protos[i].p_name)) return &protos[i];
    return NULL;
}
struct qprotoent *getprotobynumber(int proto)
{
    for (unsigned i = 0; i < sizeof protos / sizeof protos[0]; i++) if (protos[i].p_proto == proto) return &protos[i];
    return NULL;
}

/* gethostbyname2 (NSPR with IPv6): AF_INET as gethostbyname, AF_INET6 the IPv6 addresses of
 * NET_RESOLVE6 (up to 8), in the same static storage */
static struct { struct qhostent he; char name[256]; char *aliases[1]; uint8_t addr[8][16]; char *list[9]; } host6buf;
struct qhostent *gethostbyname2(const char *name, int af)
{
    if (af == AF_INET_) return gethostbyname(name);
    if (af != AF_INET6_ || !name) { h_errno = 1; return NULL; }
    uint8_t a6[16];
    int k = 0;
    if (pton6(name, a6)) { memcpy(host6buf.addr[0], a6, 16); k = 1; }
    else {
        size_t n = strlen(name);
        char tmp[256] __attribute__((aligned(8)));
        uint8_t recs[8 * REC6] __attribute__((aligned(8)));
        if (n >= sizeof tmp) { h_errno = 1; return NULL; }
        memcpy(tmp, name, n + 1);
        BLOCK(q);
        q[5] = (uint32_t)(uintptr_t)tmp; q[6] = (uint32_t)(uintptr_t)recs; q[7] = 8;
        int r = net(q, NET_RESOLVE6);
        static const uint8_t mapped[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xFF,0xFF };
        for (int i = 0; i < r; i++) if (memcmp(recs + i * REC6, mapped, 12)) memcpy(host6buf.addr[k++], recs + i * REC6, 16);
    }
    if (!k) { h_errno = 4; /* NO_DATA: the name has no IPv6 address */ return NULL; }
    snprintf(host6buf.name, sizeof host6buf.name, "%s", name);
    host6buf.aliases[0] = NULL;
    for (int i = 0; i < k; i++) host6buf.list[i] = (char *)host6buf.addr[i];
    host6buf.list[k] = NULL;
    host6buf.he.h_name = host6buf.name; host6buf.he.h_aliases = host6buf.aliases;
    host6buf.he.h_addrtype = AF_INET6_; host6buf.he.h_length = 16; host6buf.he.h_addr_list = host6buf.list;
    return &host6buf.he;
}
const uint8_t in6addr_any[16] = { 0 };
const uint8_t in6addr_loopback[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };

/* sendmsg/recvmsg: the data of the iovecs; no ancillary data (descriptors cannot be passed between
 * AROS processes: sendmsg with control data fails with EOPNOTSUPP, recvmsg returns none) */
struct qmsghdr { void *name; qsocklen_t namelen; struct qiovec *iov; int iovlen; void *control; qsocklen_t controllen; int flags; };
ssize_t sendmsg(int fd, const struct qmsghdr *m, int flags)
{
    if (m->controllen) { errno = EOPNOTSUPP; return -1; }
    ssize_t total = 0;
    for (int i = 0; i < m->iovlen; i++) {
        if (!m->iov[i].len) continue;
        ssize_t r = m->name ? sendto(fd, m->iov[i].base, m->iov[i].len, flags, m->name, m->namelen)
                            : send(fd, m->iov[i].base, m->iov[i].len, flags);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((size_t)r < m->iov[i].len) break;
    }
    return total;
}
ssize_t recvmsg(int fd, struct qmsghdr *m, int flags)
{
    ssize_t total = 0;
    m->controllen = 0;
    m->flags = 0;
    for (int i = 0; i < m->iovlen; i++) {
        if (!m->iov[i].len) continue;
        ssize_t r = (i == 0 && m->name) ? recvfrom(fd, m->iov[i].base, m->iov[i].len, flags, m->name, &m->namelen)
                                        : recv(fd, m->iov[i].base, m->iov[i].len, total ? (flags | MSG_DONTWAIT_) : flags);
        if (r < 0) { if (total) break; return -1; }
        total += r;
        if ((size_t)r < m->iov[i].len) break;
    }
    return total;
}

/* socketpair: a connected TCP pair on 127.0.0.1 through the door (AROS has no AF_UNIX sockets);
 * stream only */
int socketpair(int d, int t, int p, int *sv)
{
    (void)d; (void)p;
    if ((t & 0xFF) != SOCK_STREAM_) { errno = EOPNOTSUPP; return -1; }
    struct qsockaddr_in a;
    qsocklen_t al = sizeof a;
    int l = socket(AF_INET_, SOCK_STREAM_, 0);
    if (l < 0) return -1;
    memset(&a, 0, sizeof a);
    a.len = sizeof a; a.family = AF_INET_; a.addr = 0x7F000001u; a.port = 0;
    if (bind(l, &a, sizeof a) || listen(l, 1) || getsockname(l, &a, &al)) { int e = errno; close(l); errno = e; return -1; }
    int c = socket(AF_INET_, SOCK_STREAM_, 0);
    if (c < 0 || connect(c, &a, sizeof a)) { int e = errno; if (c >= 0) close(c); close(l); errno = e; return -1; }
    int s2 = accept(l, NULL, NULL);
    int e = errno;
    close(l);
    if (s2 < 0) { close(c); errno = e; return -1; }
    sv[0] = c; sv[1] = s2;
    return 0;
}

/* inet_addr/inet_ntoa (IPv4 only, as POSIX): through inet_pton/inet_ntop above */
uint32_t inet_addr(const char *cp)
{
    uint32_t a;
    return inet_pton(AF_INET_, cp, &a) == 1 ? a : 0xffffffffu;   /* INADDR_NONE */
}
struct qin_addr_ntoa { uint32_t s_addr; };
char *inet_ntoa(struct qin_addr_ntoa in)
{
    static char b[16];
    inet_ntop(AF_INET_, &in, b, sizeof b);
    return b;
}

/* getservbyname/getservbyport: AROS has no services database; the services Gecko and libevent ask
 * for by name, in network byte order */
struct qservent { char *s_name; char **s_aliases; int s_port; char *s_proto; };
static char *serv_no_aliases[1];
static struct { const char *name; int port; } servs[] = {
    { "http", 80 }, { "https", 443 }, { "ftp", 21 }, { "ssh", 22 }, { "domain", 53 }, { "socks", 1080 },
};
static struct qservent servbuf;
static struct qservent *serv_entry(unsigned i, const char *proto)
{
    servbuf.s_name = (char *)servs[i].name; servbuf.s_aliases = serv_no_aliases;
    servbuf.s_port = (int)(uint16_t)((servs[i].port >> 8) | ((servs[i].port & 0xff) << 8));
    servbuf.s_proto = (char *)(proto ? proto : "tcp");
    return &servbuf;
}
struct qservent *getservbyname(const char *name, const char *proto)
{
    for (unsigned i = 0; name && i < sizeof servs / sizeof servs[0]; i++)
        if (!strcmp(name, servs[i].name)) return serv_entry(i, proto);
    return NULL;
}
struct qservent *getservbyport(int port, const char *proto)
{
    int p = (int)(uint16_t)(((unsigned)port >> 8 & 0xff) | (((unsigned)port & 0xff) << 8));
    for (unsigned i = 0; i < sizeof servs / sizeof servs[0]; i++)
        if (servs[i].port == p) return serv_entry(i, proto);
    return NULL;
}

/* if_nametoindex/if_indextoname: the host door has one interface name for programs, "lo" */
unsigned int if_nametoindex(const char *name) { return name && !strcmp(name, "lo") ? 1 : 0; }
char *if_indextoname(unsigned int index, char *buf)
{
    if (index != 1) { errno = ENXIO; return NULL; }
    strcpy(buf, "lo");
    return buf;
}
