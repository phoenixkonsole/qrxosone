/* BSD sockets for 68k programs on QRX OS One, on the host's sockets through door service 4 (engine
 * full68k/qrx_hostnet.c), so no AROSTCP and no network card are involved. Taken from the Tor port's
 * layer (tools/armithlon/tor/shim). All sockets are non-blocking on the host; a call that has to wait
 * polls from here. IPv4 only, 64 sockets, as the door has. Unlike Tor's, names are resolved by the host. */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <devices/timer.h>
#define DOOR_MAGIC_REG (*(volatile uint32_t *)0xFF000880u)
#define DOOR_CALL_REG (*(volatile uint32_t *)0xFF00088Cu)
#define DOOR_MAGIC 0x51383644u
#define DOOR_FEATURE_NATIVE_NET 1u
#define SVC_NET 4u
enum { NET_SOCKET = 1, NET_BIND, NET_LISTEN, NET_ACCEPT, NET_CONNECT, NET_SEND, NET_RECV, NET_SENDTO, NET_RECVFROM,
       NET_POLL, NET_CLOSE, NET_SHUTDOWN, NET_SETOPT, NET_GETOPT, NET_SOCKNAME, NET_PEERNAME, NET_RESOLVE, NET_HOSTNAME,
       NET_RANDOM, NET_TIME };
#define MAXSOCK 64
#define SOCK_BASE 32
#define POLL_US 2000u
static unsigned char sock_open[MAXSOCK], sock_nonblock[MAXSOCK], sock_shut_atexit;

static int is_sock(int fd) { return fd >= SOCK_BASE && fd < SOCK_BASE + MAXSOCK && sock_open[fd - SOCK_BASE]; }
static int errno_of(uint32_t w)
{
    switch (w) {
    case 10035: return EWOULDBLOCK;
    case 10036: return EINPROGRESS;
    case 10037: return EALREADY;
    case 10038: return EBADF;
    case 10040: return EMSGSIZE;
    case 10043: return EPROTONOSUPPORT;
    case 10045: return EOPNOTSUPP;
    case 10048: return EADDRINUSE;
    case 10049: return EADDRNOTAVAIL;
    case 10050: return ENETDOWN;
    case 10051: return ENETUNREACH;
    case 10053: return ECONNABORTED;
    case 10054: return ECONNRESET;
    case 10056: return EISCONN;
    case 10057: return ENOTCONN;
    case 10060: return ETIMEDOUT;
    case 10061: return ECONNREFUSED;
    case 10065: return EHOSTUNREACH;
    case 10022: return EINVAL;
    case 10013: return EACCES;
    case 10024: return EMFILE;
    default: return EIO;
    }
}
int qrx_door_ready(void)
{
    static int state;
    if (!state) {
        /* Whole 32-bit reads: gcc turns a tested bit into a byte access, which the door faults. */
        uint32_t magic = DOOR_MAGIC_REG;
        __asm__ volatile("" : "+m"(magic));
        state = -1;
        if (magic == DOOR_MAGIC) {
            uint32_t f;
            __asm__ volatile("move.l 0xFF000890,%0" : "=d"(f));
            if (f & DOOR_FEATURE_NATIVE_NET) state = 1;
        }
    }
    return state > 0;
}
/* One call. On return q[12] is the result, q[13] the error, q[14] ip, q[15] port. */
static int net_call(uint32_t *q, uint32_t op)
{
    if (!qrx_door_ready()) { errno = ENETDOWN; return -1; }
    q[0] = SVC_NET; q[1] = 0; q[2] = 0; q[3] = op;
    q[12] = UINT32_MAX; q[13] = 10050;
    DOOR_CALL_REG = (uint32_t)(uintptr_t)q;
    __asm__ volatile("" ::: "memory");
    if (q[12] == UINT32_MAX) { errno = errno_of(q[13]); return -1; }
    return (int)q[12];
}
#define BLOCK(q) uint32_t q[32] __attribute__((aligned(4))); memset(q, 0, sizeof q)

/* ---- the wait between polls. With libcrt's usleep the main loop stopped for good shortly
 * after tor's worker threads had started, sitting in select; the cause was not established.
 * The first task that polls keeps its own timer here; any other one uses Delay. */
static struct MsgPort *poll_port;
static struct timerequest *poll_req;
static void poll_sleep_exit(void)
{
	/* The timer's signal and port go back, or AROS warns about an unfreed signal. */
	if (poll_req) {
		CloseDevice((struct IORequest *)poll_req);
		DeleteIORequest((struct IORequest *)poll_req);
	}
	if (poll_port) DeleteMsgPort(poll_port);
}
static void poll_sleep(void)
{
    static struct Task *owner;
    static struct MsgPort *port;
    static struct timerequest *req;
    static int ready;
    struct Task *me = FindTask(NULL);
    if (!owner) {
        owner = me;
        port = CreateMsgPort();
        if (port) req = (struct timerequest *)CreateIORequest(port, sizeof *req);
        poll_port = port;
        poll_port = port;
        if (req && !OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ, (struct IORequest *)req, 0)) {
            ready = 1;
            poll_req = req;
        }
        atexit(poll_sleep_exit);
    }
    if (me != owner || !ready) { Delay(1); return; }
    req->tr_node.io_Command = TR_ADDREQUEST;
    req->tr_time.tv_secs = 0;
    req->tr_time.tv_micro = POLL_US;
    DoIO((struct IORequest *)req);
}
/* ---- break: AROS's Ctrl-C has no way into a task waiting here but this one */
static void check_break(void)
{
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) raise(SIGINT);
}
static int wait_ready(int fd, uint32_t mask)
{
    for (;;) {
        BLOCK(q);
        q[4] = (uint32_t)(fd - SOCK_BASE); q[5] = mask;
        int r = net_call(q, NET_POLL);
        if (r < 0) return -1;
        if (r & (int)mask) return 0;
        check_break();
        poll_sleep();
    }
}
static void close_all(void)
{
    for (int i = 0; i < MAXSOCK; i++) if (sock_open[i]) {
        BLOCK(q);
        q[4] = (uint32_t)i;
        net_call(q, NET_CLOSE);
        sock_open[i] = 0;
    }
}
static int adopt(int slot)
{
    if (slot >= MAXSOCK) { errno = EMFILE; return -1; }
    sock_open[slot] = 1; sock_nonblock[slot] = 0;
    if (!sock_shut_atexit) { sock_shut_atexit = 1; atexit(close_all); }
    return SOCK_BASE + slot;
}
int socket(int domain, int type, int protocol)
{
    (void)protocol;
    if (domain != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    int nb = (type & SOCK_NONBLOCK) != 0;
    type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (type != SOCK_STREAM && type != SOCK_DGRAM) { errno = EPROTONOSUPPORT; return -1; }
    BLOCK(q);
    q[4] = type == SOCK_DGRAM ? 2 : 1;
    int r = net_call(q, NET_SOCKET);
    if (r < 0) return -1;
    r = adopt(r);
    if (r >= 0) sock_nonblock[r - SOCK_BASE] = nb;
    return r;
}
static int get_in(const struct sockaddr *sa, socklen_t l, uint32_t *ip, uint32_t *port)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
    if (!sa || l < sizeof *in || in->sin_family != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    *ip = ntohl(in->sin_addr.s_addr); *port = ntohs(in->sin_port);
    return 0;
}
static void put_in(struct sockaddr *sa, socklen_t *l, uint32_t ip, uint32_t port)
{
    struct sockaddr_in in;
    socklen_t n;
    if (!sa || !l) return;
    memset(&in, 0, sizeof in);
    in.sin_len = sizeof in;
    in.sin_family = AF_INET; in.sin_port = htons((uint16_t)port); in.sin_addr.s_addr = htonl(ip);
    n = *l < sizeof in ? *l : sizeof in;
    memcpy(sa, &in, n);
    *l = sizeof in;
}
#define NEED_SOCK(fd) do { if (!is_sock(fd)) { errno = ENOTSOCK; return -1; } } while (0)
#define SLOT(fd) ((uint32_t)((fd) - SOCK_BASE))
int bind(int fd, const struct sockaddr *sa, socklen_t l)
{
    BLOCK(q);
    NEED_SOCK(fd);
    if (get_in(sa, l, &q[5], &q[6])) return -1;
    q[4] = SLOT(fd);
    return net_call(q, NET_BIND) < 0 ? -1 : 0;
}
int listen(int fd, int backlog)
{
    BLOCK(q);
    NEED_SOCK(fd);
    q[4] = SLOT(fd); q[5] = (uint32_t)backlog;
    return net_call(q, NET_LISTEN) < 0 ? -1 : 0;
}
int accept(int fd, struct sockaddr *sa, socklen_t *l)
{
    NEED_SOCK(fd);
    for (;;) {
        BLOCK(q);
        q[4] = SLOT(fd);
        int r = net_call(q, NET_ACCEPT);
        if (r >= 0) { put_in(sa, l, q[14], q[15]); return adopt(r); }
        if (errno != EWOULDBLOCK || sock_nonblock[SLOT(fd)]) return -1;
        if (wait_ready(fd, 1)) return -1;
    }
}
int connect(int fd, const struct sockaddr *sa, socklen_t l)
{
    BLOCK(q);
    NEED_SOCK(fd);
    if (get_in(sa, l, &q[5], &q[6])) return -1;
    q[4] = SLOT(fd);
    if (net_call(q, NET_CONNECT) >= 0) return 0;
    if (errno != EINPROGRESS && errno != EWOULDBLOCK) return -1;
    if (sock_nonblock[SLOT(fd)]) { errno = EINPROGRESS; return -1; }
    if (wait_ready(fd, 2)) return -1;
    int err = 0;
    socklen_t el = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el)) return -1;
    if (err) { errno = err; return -1; }
    return 0;
}
ssize_t send(int fd, const void *buf, size_t n, int flags)
{
    NEED_SOCK(fd);
    for (;;) {
        BLOCK(q);
        q[4] = SLOT(fd); q[5] = (uint32_t)(uintptr_t)buf; q[6] = (uint32_t)n; q[7] = (flags & MSG_OOB) ? 1 : 0;
        int r = net_call(q, NET_SEND);
        if (r >= 0) return r;
        if (errno != EWOULDBLOCK || sock_nonblock[SLOT(fd)] || (flags & MSG_DONTWAIT)) return -1;
        if (wait_ready(fd, 2)) return -1;
    }
}
ssize_t recv(int fd, void *buf, size_t n, int flags)
{
    NEED_SOCK(fd);
    for (;;) {
        BLOCK(q);
        q[4] = SLOT(fd); q[5] = (uint32_t)(uintptr_t)buf; q[6] = (uint32_t)n; q[7] = (flags & MSG_PEEK) ? 2 : 0;
        int r = net_call(q, NET_RECV);
        if (r >= 0) return r;
        if (errno != EWOULDBLOCK || sock_nonblock[SLOT(fd)] || (flags & MSG_DONTWAIT)) return -1;
        if (wait_ready(fd, 1)) return -1;
    }
}
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *sa, socklen_t l)
{
    (void)flags;
    if (!sa) return send(fd, buf, n, flags);
    BLOCK(q);
    NEED_SOCK(fd);
    if (get_in(sa, l, &q[8], &q[9])) return -1;
    q[4] = SLOT(fd); q[5] = (uint32_t)(uintptr_t)buf; q[6] = (uint32_t)n;
    return net_call(q, NET_SENDTO);
}
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *sa, socklen_t *l)
{
    NEED_SOCK(fd);
    for (;;) {
        BLOCK(q);
        q[4] = SLOT(fd); q[5] = (uint32_t)(uintptr_t)buf; q[6] = (uint32_t)n; q[7] = (flags & MSG_PEEK) ? 2 : 0;
        int r = net_call(q, NET_RECVFROM);
        if (r >= 0) { put_in(sa, l, q[14], q[15]); return r; }
        if (errno != EWOULDBLOCK || sock_nonblock[SLOT(fd)]) return -1;
        if (wait_ready(fd, 1)) return -1;
    }
}
int shutdown(int fd, int how)
{
    BLOCK(q);
    NEED_SOCK(fd);
    q[4] = SLOT(fd); q[5] = (uint32_t)how;
    return net_call(q, NET_SHUTDOWN) < 0 ? -1 : 0;
}
int setsockopt(int fd, int level, int name, const void *val, socklen_t l)
{
    BLOCK(q);
    NEED_SOCK(fd);
    q[4] = SLOT(fd); q[5] = (uint32_t)level; q[6] = (uint32_t)name;
    if (l >= sizeof(int)) q[7] = (uint32_t)*(const int *)val;
    return net_call(q, NET_SETOPT) < 0 ? -1 : 0;
}
int getsockopt(int fd, int level, int name, void *val, socklen_t *l)
{
    BLOCK(q);
    NEED_SOCK(fd);
    q[4] = SLOT(fd); q[5] = (uint32_t)level; q[6] = (uint32_t)name;
    if (net_call(q, NET_GETOPT) < 0) return -1;
    if (*l < sizeof(int)) { errno = EINVAL; return -1; }
    /* The door speaks Winsock numbers; SO_ERROR comes back as one. */
    *(int *)val = (level == SOL_SOCKET && name == SO_ERROR && q[14]) ? errno_of(q[14]) : (int)q[14];
    *l = sizeof(int);
    return 0;
}
int getsockname(int fd, struct sockaddr *sa, socklen_t *l)
{
    BLOCK(q);
    NEED_SOCK(fd);
    q[4] = SLOT(fd);
    if (net_call(q, NET_SOCKNAME) < 0) return -1;
    put_in(sa, l, q[14], q[15]);
    return 0;
}
int getpeername(int fd, struct sockaddr *sa, socklen_t *l)
{
    BLOCK(q);
    NEED_SOCK(fd);
    q[4] = SLOT(fd);
    if (net_call(q, NET_PEERNAME) < 0) return -1;
    put_in(sa, l, q[14], q[15]);
    return 0;
}
/* Over host loopback, with the peer checked, since any host program can connect there. */
int socketpair(int domain, int type, int protocol, int sv[2])
{
    struct sockaddr_in a, b, c;
    socklen_t l = sizeof a;
    int ls, s0 = -1, s1 = -1, e;
    (void)domain; (void)protocol;
    if ((type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) != SOCK_STREAM) { errno = EPROTONOSUPPORT; return -1; }
    if ((ls = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1) || getsockname(ls, (struct sockaddr *)&a, &l)) goto fail;
    if ((s0 = socket(AF_INET, SOCK_STREAM, 0)) < 0 || connect(s0, (struct sockaddr *)&a, sizeof a)) goto fail;
    l = sizeof b;
    if (getsockname(s0, (struct sockaddr *)&b, &l)) goto fail;
    l = sizeof c;
    if ((s1 = accept(ls, (struct sockaddr *)&c, &l)) < 0) goto fail;
    if (c.sin_port != b.sin_port || c.sin_addr.s_addr != b.sin_addr.s_addr) { errno = EACCES; goto fail; }
    close(ls);
    if (type & SOCK_NONBLOCK) { sock_nonblock[SLOT(s0)] = 1; sock_nonblock[SLOT(s1)] = 1; }
    sv[0] = s0; sv[1] = s1;
    return 0;
fail:
    e = errno;
    if (s1 >= 0) close(s1);
    if (s0 >= 0) close(s0);
    close(ls);
    errno = e;
    return -1;
}
int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    struct timeval start, now;
    long limit_us = -1;
    fd_set r0, w0, e0;
    if (nfds > FD_SETSIZE) nfds = FD_SETSIZE;
    if (tv) {
        limit_us = tv->tv_sec * 1000000L + tv->tv_usec;
        gettimeofday(&start, NULL);
    }
    FD_ZERO(&r0); FD_ZERO(&w0); FD_ZERO(&e0);
    if (rd) r0 = *rd;
    if (wr) w0 = *wr;
    if (ex) e0 = *ex;
    for (;;) {
        int hits = 0;
        if (rd) FD_ZERO(rd);
        if (wr) FD_ZERO(wr);
        if (ex) FD_ZERO(ex);
        for (int fd = SOCK_BASE; fd < nfds; fd++) {
            uint32_t mask = (FD_ISSET(fd, &r0) ? 1u : 0u) | (FD_ISSET(fd, &w0) ? 2u : 0u) | (FD_ISSET(fd, &e0) ? 4u : 0u);
            if (!mask) continue;
            if (!is_sock(fd)) { errno = EBADF; return -1; }
            BLOCK(q);
            q[4] = SLOT(fd); q[5] = mask;
            int got = net_call(q, NET_POLL);
            if (got < 0) return -1;
            got &= (int)mask;
            if ((got & 1) && rd) { FD_SET(fd, rd); hits++; }
            if ((got & 2) && wr) { FD_SET(fd, wr); hits++; }
            if ((got & 4) && ex) { FD_SET(fd, ex); hits++; }
        }
        if (hits) return hits;
        if (limit_us == 0) return 0;
        if (limit_us > 0) {
            gettimeofday(&now, NULL);
            long gone = (now.tv_sec - start.tv_sec) * 1000000L + (now.tv_usec - start.tv_usec);
            if (gone >= limit_us) return 0;
        }
        check_break();
        poll_sleep();
    }
}
int poll(struct pollfd *p, nfds_t n, int timeout_ms)
{
    fd_set r, w;
    struct timeval tv;
    int top = 0, hits;
    FD_ZERO(&r); FD_ZERO(&w);
    for (nfds_t i = 0; i < n; i++) {
        if (p[i].fd < 0) continue;
        if (p[i].events & POLLIN) FD_SET(p[i].fd, &r);
        if (p[i].events & POLLOUT) FD_SET(p[i].fd, &w);
        if (p[i].fd >= top) top = p[i].fd + 1;
    }
    tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(top, &r, &w, NULL, timeout_ms < 0 ? NULL : &tv) < 0) return -1;
    hits = 0;
    for (nfds_t i = 0; i < n; i++) {
        p[i].revents = 0;
        if (p[i].fd < 0) continue;
        if (FD_ISSET(p[i].fd, &r)) p[i].revents |= POLLIN;
        if (FD_ISSET(p[i].fd, &w)) p[i].revents |= POLLOUT;
        if (p[i].revents) hits++;
    }
    return hits;
}
/* ---- the file calls that also take sockets */
extern int __real_close(int);
extern ssize_t __real_read(int, void *, size_t);
extern ssize_t __real_write(int, const void *, size_t);
extern int __real_fcntl(int, int, ...);
extern int __real_ioctl(int, unsigned long, ...);
int __wrap_close(int fd)
{
    if (fd < SOCK_BASE || fd >= SOCK_BASE + MAXSOCK) return __real_close(fd);
    if (!is_sock(fd)) { errno = EBADF; return -1; }
    BLOCK(q);
    q[4] = SLOT(fd);
    sock_open[SLOT(fd)] = 0;
    return net_call(q, NET_CLOSE) < 0 ? -1 : 0;
}
ssize_t __wrap_read(int fd, void *buf, size_t n)
{
    return is_sock(fd) ? recv(fd, buf, n, 0) : __real_read(fd, buf, n);
}
ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    return is_sock(fd) ? send(fd, buf, n, 0) : __real_write(fd, buf, n);
}
int __wrap_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    long arg;
    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);
    if (!is_sock(fd)) return __real_fcntl(fd, cmd, arg);
    switch (cmd) {
    case F_GETFL: return O_RDWR | (sock_nonblock[SLOT(fd)] ? O_NONBLOCK : 0);
    case F_SETFL: sock_nonblock[SLOT(fd)] = (arg & O_NONBLOCK) != 0; return 0;
    case F_GETFD: return 0;
    case F_SETFD: return 0;
    default: errno = EINVAL; return -1;
    }
}
int __wrap_ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg;
    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (!is_sock(fd)) return __real_ioctl(fd, req, arg);
    if (req == FIONBIO) { sock_nonblock[SLOT(fd)] = *(int *)arg != 0; return 0; }
    if (req == FIONREAD) {
        BLOCK(q);
        q[4] = SLOT(fd); q[5] = 8;
        if (net_call(q, NET_POLL) < 0) return -1;
        *(int *)arg = (int)q[14];
        return 0;
    }
    errno = EINVAL;
    return -1;
}

/* ---- names, resolved by the host */
int h_errno;
struct hostent *gethostbyname(const char *name)
{
	static struct hostent he;
	static struct in_addr addr;
	static char *addrs[2], *aliases[1], namebuf[256];
	BLOCK(q);
	char host[256];
	strncpy(host, name, sizeof host - 1);
	host[sizeof host - 1] = 0;
	q[4] = 0;
	q[5] = (uint32_t)(uintptr_t)host;
	if (net_call(q, NET_RESOLVE) < 0) {
		h_errno = HOST_NOT_FOUND;
		return NULL;
	}
	addr.s_addr = htonl(q[14]);
	strcpy(namebuf, host);
	addrs[0] = (char *)&addr; addrs[1] = NULL; aliases[0] = NULL;
	he.h_name = namebuf; he.h_aliases = aliases; he.h_addrtype = AF_INET; he.h_length = 4; he.h_addr_list = addrs;
	return &he;
}
struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type)
{
	(void)addr; (void)len; (void)type;
	h_errno = HOST_NOT_FOUND;
	return NULL;
}
int gethostname(char *name, size_t len)
{
	BLOCK(q);
	q[4] = 0;
	q[5] = (uint32_t)(uintptr_t)name;
	q[6] = (uint32_t)len;
	return net_call(q, NET_HOSTNAME) < 0 ? -1 : 0;
}
struct servent *getservbyname(const char *name, const char *proto)
{
	(void)name; (void)proto;
	return NULL;
}
struct protoent *getprotobyname(const char *name)
{
	static char *aliases[1];
	static struct protoent p;
	static const struct { const char *name; int proto; } known[] = { { "ip", 0 }, { "icmp", 1 }, { "tcp", 6 }, { "udp", 17 } };
	for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++) {
		if (!strcmp(name, known[i].name)) {
			p.p_name = (char *)known[i].name; p.p_aliases = aliases; p.p_proto = known[i].proto;
			return &p;
		}
	}
	return NULL;
}
struct servent *getservbyport(int port, const char *proto)
{
	(void)port; (void)proto;
	return NULL;
}
char *inet_ntoa(struct in_addr a)
{
    static char buf[16];
    uint32_t v = ntohl(a.s_addr);
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", (unsigned)(v >> 24), (unsigned)(v >> 16 & 255), (unsigned)(v >> 8 & 255), (unsigned)(v & 255));
    return buf;
}
int inet_aton(const char *s, struct in_addr *a)
{
    unsigned b[4];
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &b[0], &b[1], &b[2], &b[3], &tail) != 4 || (b[0] | b[1] | b[2] | b[3]) > 255) return 0;
    a->s_addr = htonl(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
    return 1;
}
in_addr_t inet_addr(const char *s)
{
    struct in_addr a;
    return inet_aton(s, &a) ? a.s_addr : INADDR_NONE;
}
/* ---- random bytes from the host */
/* Random bytes from the host. Returns -1 where the door cannot give them. */
int getentropy(void *buf, size_t n)
{
	unsigned char *p = buf;
	while (n) {
		unsigned char chunk[256];
		size_t k = n < sizeof chunk ? n : sizeof chunk;
		BLOCK(q);
		q[4] = 0;
		q[5] = (uint32_t)(uintptr_t)chunk;
		q[6] = (uint32_t)k;
		if (net_call(q, NET_RANDOM) < 0) return -1;
		memcpy(p, chunk, k);
		p += k;
		n -= k;
	}
	return 0;
}
