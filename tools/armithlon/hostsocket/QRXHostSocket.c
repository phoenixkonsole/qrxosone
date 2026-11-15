/* QRXHostSocket: a bsdsocket.library for AROS 68k on ARMithlon, on the Mac's own sockets
 * through the host door (network service 4), the same way Windows programs get Winsock.
 * ARMithlon emulates no network card, so AROSTCP has nothing to talk to; with this, native
 * programs such as QRXWeb reach the network. Started once from the Startup-Sequence, it builds
 * the library in memory, adds it to the system and stays loaded.
 *
 * IPv4, TCP and UDP, at most 64 sockets for the whole system. Blocking calls poll the host.
 * Not here: SANA-II, routing and interface configuration, IPv6, sendmsg/recvmsg, select on
 * anything but sockets. The register stubs (stubs.S) come from gen-stubs.py. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <utility/tagitem.h>
#include <bsdsocket/socketbasetags.h>
#include <stdint.h>
#include <string.h>

#if defined(__m68k64__)
/* AROS 64 on the 68100EC64: the board's host call register, see qrx_ec64_board.h */
#define BOARD_ID_REG 0xFFFFFFFF00000000UL
#define BOARD_HOST_CALL 0xFFFFFFFF00000400UL
#define BOARD_MAGIC 0x5152584543363442UL
typedef void *RPTR;          /* pointer results come back as pointers */
#else
typedef ULONG RPTR;          /* the stubs copy D0 to A0 */
#endif
#define DOOR_CALL_REG 0xFF00088Cu
#define DOOR_MAGIC 0x51383644u
#define DOOR_FEATURE_NATIVE_NET 1u
#define SVC_NET 4u
enum { NET_SOCKET = 1, NET_BIND, NET_LISTEN, NET_ACCEPT, NET_CONNECT, NET_SEND, NET_RECV, NET_SENDTO, NET_RECVFROM,
       NET_POLL, NET_CLOSE, NET_SHUTDOWN, NET_SETOPT, NET_GETOPT, NET_SOCKNAME, NET_PEERNAME, NET_RESOLVE, NET_HOSTNAME,
       NET_RANDOM, NET_TIME };
#define MAXSOCK 64
#define AF_INET_ 2
#define SOCK_STREAM_ 1
#define SOCK_DGRAM_ 2
#define FIONBIO_ 0x8004667Eu
#define FIONREAD_ 0x4004667Fu
#define MSG_PEEK_ 2
#define SOL_SOCKET_ 0xFFFF
#define SO_ERROR_ 0x1007
/* errno values of AROS (BSD) */
enum { E_BADF = 9, E_INVAL = 22, E_MFILE = 24, E_AGAIN = 35, E_INPROGRESS = 36, E_ALREADY = 37, E_NOTSOCK = 38,
       E_MSGSIZE = 40, E_PROTONOSUPPORT = 43, E_OPNOTSUPP = 45, E_AFNOSUPPORT = 47, E_ADDRINUSE = 48,
       E_ADDRNOTAVAIL = 49, E_NETDOWN = 50, E_NETUNREACH = 51, E_CONNABORTED = 53, E_CONNRESET = 54,
       E_ISCONN = 56, E_NOTCONN = 57, E_TIMEDOUT = 60, E_CONNREFUSED = 61, E_HOSTUNREACH = 65, E_NOSYS = 78,
       E_INTR = 4, E_IO = 5, E_ACCES = 13 };

struct sockaddr_in_ { uint8_t len, family; uint16_t port; uint32_t addr; uint8_t zero[8]; };
struct hostent_ { char *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list; };

static int g_errno, g_herrno;
static void *g_errno_ptr;
static int g_errno_size;
static ULONG g_breakmask = SIGBREAKF_CTRL_C;
static uint8_t g_open[MAXSOCK], g_nonblock[MAXSOCK];

static void set_errno(int e)
{
    g_errno = e;
    if (g_errno_ptr) {
        if (g_errno_size == 1) *(int8_t *)g_errno_ptr = (int8_t)e;
        else if (g_errno_size == 2) *(int16_t *)g_errno_ptr = (int16_t)e;
        else *(int32_t *)g_errno_ptr = e;
    }
}
static int errno_of(uint32_t w)
{
    switch (w) {
    case 10035: return E_AGAIN;       case 10036: return E_INPROGRESS;  case 10037: return E_ALREADY;
    case 10038: return E_BADF;        case 10040: return E_MSGSIZE;     case 10043: return E_PROTONOSUPPORT;
    case 10045: return E_OPNOTSUPP;   case 10048: return E_ADDRINUSE;   case 10049: return E_ADDRNOTAVAIL;
    case 10050: return E_NETDOWN;     case 10051: return E_NETUNREACH;  case 10053: return E_CONNABORTED;
    case 10054: return E_CONNRESET;   case 10056: return E_ISCONN;      case 10057: return E_NOTCONN;
    case 10060: return E_TIMEDOUT;    case 10061: return E_CONNREFUSED; case 10065: return E_HOSTUNREACH;
    case 10022: return E_INVAL;       case 10013: return E_ACCES;       case 10024: return E_MFILE;
    default: return E_IO;
    }
}
static int door_ok(void)
{
    static int state;
#if defined(__m68k64__)
    if (!state) state = *(volatile uint64_t *)BOARD_ID_REG == BOARD_MAGIC ? 1 : -1;
    return state > 0;
#endif
    if (!state) {
        uint32_t magic, f;
        /* Whole 32-bit reads: a byte access to the door faults the emulator. */
        __asm__ volatile("move.l 0xFF000880,%0" : "=d"(magic));
        state = -1;
        if (magic == DOOR_MAGIC) {
            __asm__ volatile("move.l 0xFF000890,%0" : "=d"(f));
            if (f & DOOR_FEATURE_NATIVE_NET) state = 1;
        }
    }
    return state > 0;
}
/* One door call: q[4..11] arguments, then q[12] result, q[13] error, q[14] ip, q[15] port */
static int net(uint32_t *q, uint32_t op)
{
    if (!door_ok()) { set_errno(E_NETDOWN); return -1; }
    q[0] = SVC_NET; q[1] = 0; q[2] = 0; q[3] = op;
    q[12] = UINT32_MAX; q[13] = 10050;
#if defined(__m68k64__)
    *(volatile uint64_t *)BOARD_HOST_CALL = (uint64_t)(uintptr_t)q;
#else
    *(volatile uint32_t *)DOOR_CALL_REG = (uint32_t)(uintptr_t)q;
#endif
    __asm__ volatile("" ::: "memory");
    if (q[12] == UINT32_MAX) { set_errno(errno_of(q[13])); return -1; }
    return (int)q[12];
}
#define BLOCK(q) uint32_t q[32] __attribute__((aligned(4))); memset(q, 0, sizeof q)
static int bad(int s) { return s < 0 || s >= MAXSOCK || !g_open[s]; }
#define NEED(s) do { if (bad(s)) { set_errno(E_NOTSOCK); return -1; } } while (0)
static int interrupted(void)
{
    if (SetSignal(0, 0) & g_breakmask) { SetSignal(0, g_breakmask & SIGBREAKF_CTRL_C); set_errno(E_INTR); return 1; }
    return 0;
}
static int wait_ready(int s, uint32_t mask)
{
    for (;;) {
        BLOCK(q);
        q[4] = (uint32_t)s; q[5] = mask;
        int r = net(q, NET_POLL);
        if (r < 0) return -1;
        if (r & (int)mask) return 0;
        if (interrupted()) return -1;
        Delay(1);
    }
}
static int get_in(const void *sa, uint32_t l, uint32_t *ip, uint32_t *port)
{
    const struct sockaddr_in_ *in = sa;
    if (!sa || l < 8 || in->family != AF_INET_) { set_errno(E_AFNOSUPPORT); return -1; }
    *ip = in->addr; *port = in->port;   /* big endian like the network: no swap on 68k */
    return 0;
}
static void put_in(void *sa, int32_t *l, uint32_t ip, uint32_t port)
{
    struct sockaddr_in_ in;
    if (!sa || !l) return;
    memset(&in, 0, sizeof in);
    in.len = sizeof in; in.family = AF_INET_; in.port = (uint16_t)port; in.addr = ip;
    memcpy(sa, &in, (uint32_t)*l < sizeof in ? (uint32_t)*l : sizeof in);
    *l = sizeof in;
}

/* ---- the library's functions, called from the stubs with their register arguments on the stack */
ULONG c_socket(int domain, int type, int protocol)
{
    (void)protocol;
    if (domain != AF_INET_) { set_errno(E_AFNOSUPPORT); return (ULONG)-1; }
    if (type != SOCK_STREAM_ && type != SOCK_DGRAM_) { set_errno(E_PROTONOSUPPORT); return (ULONG)-1; }
    BLOCK(q);
    q[4] = type == SOCK_DGRAM_ ? 2 : 1;
    int r = net(q, NET_SOCKET);
    if (r < 0) return (ULONG)-1;
    if (r >= MAXSOCK) { BLOCK(c); c[4] = (uint32_t)r; net(c, NET_CLOSE); set_errno(E_MFILE); return (ULONG)-1; }
    g_open[r] = 1; g_nonblock[r] = 0;
    return (ULONG)r;
}
ULONG c_bind(int s, const void *sa, int l)
{
    BLOCK(q); NEED(s);
    if (get_in(sa, (uint32_t)l, &q[5], &q[6])) return (ULONG)-1;
    q[4] = (uint32_t)s;
    return net(q, NET_BIND) < 0 ? (ULONG)-1 : 0;
}
ULONG c_listen(int s, int backlog)
{
    BLOCK(q); NEED(s);
    q[4] = (uint32_t)s; q[5] = (uint32_t)backlog;
    return net(q, NET_LISTEN) < 0 ? (ULONG)-1 : 0;
}
ULONG c_accept(int s, void *sa, int32_t *l)
{
    NEED(s);
    for (;;) {
        BLOCK(q);
        q[4] = (uint32_t)s;
        int r = net(q, NET_ACCEPT);
        if (r >= 0) {
            if (r >= MAXSOCK) { BLOCK(c); c[4] = (uint32_t)r; net(c, NET_CLOSE); set_errno(E_MFILE); return (ULONG)-1; }
            g_open[r] = 1; g_nonblock[r] = 0;
            put_in(sa, l, q[14], q[15]);
            return (ULONG)r;
        }
        if (g_errno != E_AGAIN || g_nonblock[s]) return (ULONG)-1;
        if (wait_ready(s, 1)) return (ULONG)-1;
    }
}
ULONG c_getsockopt(int s, int level, int name, void *val, int32_t *l);
ULONG c_connect(int s, const void *sa, int l)
{
    BLOCK(q); NEED(s);
    if (get_in(sa, (uint32_t)l, &q[5], &q[6])) return (ULONG)-1;
    q[4] = (uint32_t)s;
    if (net(q, NET_CONNECT) >= 0) return 0;
    if (g_errno != E_INPROGRESS && g_errno != E_AGAIN) return (ULONG)-1;
    if (g_nonblock[s]) { set_errno(E_INPROGRESS); return (ULONG)-1; }
    if (wait_ready(s, 2)) return (ULONG)-1;
    int err = 0; int32_t el = sizeof err;
    if (c_getsockopt(s, SOL_SOCKET_, SO_ERROR_, &err, &el) == (ULONG)-1) return (ULONG)-1;
    if (err) { set_errno(err); return (ULONG)-1; }
    return 0;
}
ULONG c_send(int s, const void *buf, int n, int flags)
{
    NEED(s);
    for (;;) {
        BLOCK(q);
        q[4] = (uint32_t)s; q[5] = (uint32_t)(uintptr_t)buf; q[6] = (uint32_t)n; q[7] = (flags & 1) ? 1 : 0;
        int r = net(q, NET_SEND);
        if (r >= 0) return (ULONG)r;
        if (g_errno != E_AGAIN || g_nonblock[s]) return (ULONG)-1;
        if (wait_ready(s, 2)) return (ULONG)-1;
    }
}
ULONG c_recv(int s, void *buf, int n, int flags)
{
    NEED(s);
    for (;;) {
        BLOCK(q);
        q[4] = (uint32_t)s; q[5] = (uint32_t)(uintptr_t)buf; q[6] = (uint32_t)n; q[7] = (flags & MSG_PEEK_) ? 2 : 0;
        int r = net(q, NET_RECV);
        if (r >= 0) return (ULONG)r;
        if (g_errno != E_AGAIN || g_nonblock[s]) return (ULONG)-1;
        if (wait_ready(s, 1)) return (ULONG)-1;
    }
}
ULONG c_sendto(int s, const void *buf, int n, int flags, const void *to, int tl)
{
    if (!to) return c_send(s, buf, n, flags);
    BLOCK(q); NEED(s);
    if (get_in(to, (uint32_t)tl, &q[8], &q[9])) return (ULONG)-1;
    q[4] = (uint32_t)s; q[5] = (uint32_t)(uintptr_t)buf; q[6] = (uint32_t)n;
    int r = net(q, NET_SENDTO);
    return r < 0 ? (ULONG)-1 : (ULONG)r;
}
ULONG c_recvfrom(int s, void *buf, int n, int flags, void *from, int32_t *fl)
{
    NEED(s);
    for (;;) {
        BLOCK(q);
        q[4] = (uint32_t)s; q[5] = (uint32_t)(uintptr_t)buf; q[6] = (uint32_t)n; q[7] = (flags & MSG_PEEK_) ? 2 : 0;
        int r = net(q, NET_RECVFROM);
        if (r >= 0) { put_in(from, fl, q[14], q[15]); return (ULONG)r; }
        if (g_errno != E_AGAIN || g_nonblock[s]) return (ULONG)-1;
        if (wait_ready(s, 1)) return (ULONG)-1;
    }
}
ULONG c_shutdown(int s, int how)
{
    BLOCK(q); NEED(s);
    q[4] = (uint32_t)s; q[5] = (uint32_t)how;
    return net(q, NET_SHUTDOWN) < 0 ? (ULONG)-1 : 0;
}
ULONG c_setsockopt(int s, int level, int name, const void *val, int l)
{
    BLOCK(q); NEED(s);
    q[4] = (uint32_t)s; q[5] = (uint32_t)level; q[6] = (uint32_t)name;
    if (val && l >= 4) q[7] = *(const uint32_t *)val;
    return net(q, NET_SETOPT) < 0 ? (ULONG)-1 : 0;
}
ULONG c_getsockopt(int s, int level, int name, void *val, int32_t *l)
{
    BLOCK(q); NEED(s);
    q[4] = (uint32_t)s; q[5] = (uint32_t)level; q[6] = (uint32_t)name;
    if (net(q, NET_GETOPT) < 0) return (ULONG)-1;
    if (!val || !l || *l < 4) { set_errno(E_INVAL); return (ULONG)-1; }
    /* The door speaks Winsock numbers; SO_ERROR comes back as one. */
    *(int32_t *)val = (level == SOL_SOCKET_ && name == SO_ERROR_ && q[14]) ? errno_of(q[14]) : (int32_t)q[14];
    *l = 4;
    return 0;
}
ULONG c_getsockname(int s, void *sa, int32_t *l)
{
    BLOCK(q); NEED(s);
    q[4] = (uint32_t)s;
    if (net(q, NET_SOCKNAME) < 0) return (ULONG)-1;
    put_in(sa, l, q[14], q[15]);
    return 0;
}
ULONG c_getpeername(int s, void *sa, int32_t *l)
{
    BLOCK(q); NEED(s);
    q[4] = (uint32_t)s;
    if (net(q, NET_PEERNAME) < 0) return (ULONG)-1;
    put_in(sa, l, q[14], q[15]);
    return 0;
}
ULONG c_IoctlSocket(int s, ULONG req, char *argp)
{
    NEED(s);
    if (req == FIONBIO_) { g_nonblock[s] = argp && *(int32_t *)argp; return 0; }
    if (req == FIONREAD_) {
        BLOCK(q);
        q[4] = (uint32_t)s; q[5] = 1;
        int r = net(q, NET_POLL);
        if (r < 0) return (ULONG)-1;
        if (argp) *(int32_t *)argp = (r & 1) ? 1 : 0;   /* the door says ready, not how much */
        return 0;
    }
    set_errno(E_INVAL);
    return (ULONG)-1;
}
ULONG c_CloseSocket(int s)
{
    BLOCK(q); NEED(s);
    q[4] = (uint32_t)s;
    net(q, NET_CLOSE);
    g_open[s] = 0;
    return 0;
}
/* fd_set: words of a long (fd_mask), bit n % NB of word n / NB, as on AROS */
#define NB ((int)(sizeof(unsigned long) * 8))
#define WORD(n) ((n) / NB)
#define BIT(n) (1UL << ((n) % NB))
static int fd_isset(unsigned long *set, int n) { return set && (set[WORD(n)] & BIT(n)) != 0; }
struct timeval_ { unsigned long secs, micro; };   /* AROS's timeval: two longs */
ULONG c_WaitSelect(int nfds, unsigned long *rd, unsigned long *wr, unsigned long *ex, struct timeval_ *tv, ULONG *sigmask)
{
    uint8_t r_in[MAXSOCK], w_in[MAXSOCK];
    if (nfds > MAXSOCK) nfds = MAXSOCK;
    for (int i = 0; i < nfds; i++) { r_in[i] = fd_isset(rd, i); w_in[i] = fd_isset(wr, i); }
    ULONG waitmask = sigmask ? *sigmask : 0;
    ULONG limit = tv ? (ULONG)(tv->secs * 50 + tv->micro / 20000) : 0xFFFFFFFFu;   /* in ticks */
    for (ULONG ticks = 0;; ticks++) {
        int count = 0;
        uint8_t r_out[MAXSOCK], w_out[MAXSOCK];
        for (int i = 0; i < nfds; i++) {
            uint32_t m = (uint32_t)r_in[i] | ((uint32_t)w_in[i] << 1);
            r_out[i] = w_out[i] = 0;
            if (!m) continue;
            if (bad(i)) { set_errno(E_BADF); return (ULONG)-1; }
            BLOCK(q);
            q[4] = (uint32_t)i; q[5] = m;
            int got = net(q, NET_POLL);
            if (got < 0) got = (int)m;   /* an error: let the call find it */
            if (got & (int)m & 1) { r_out[i] = 1; count++; }
            if (got & (int)m & 2) { w_out[i] = 1; count++; }
        }
        ULONG sig = waitmask ? SetSignal(0, 0) & waitmask : 0;
        if (count || sig || ticks >= limit) {
            for (int i = 0; i < nfds; i++) {
                if (rd) { if (r_out[i]) rd[WORD(i)] |= BIT(i); else rd[WORD(i)] &= ~BIT(i); }
                if (wr) { if (w_out[i]) wr[WORD(i)] |= BIT(i); else wr[WORD(i)] &= ~BIT(i); }
                if (ex) ex[WORD(i)] &= ~BIT(i);
            }
            if (sigmask) { *sigmask = sig; if (sig) SetSignal(0, sig); }
            return (ULONG)count;
        }
        if (SetSignal(0, 0) & g_breakmask & ~waitmask) { set_errno(E_INTR); return (ULONG)-1; }
        Delay(1);
    }
}
ULONG c_SetSocketSignals(ULONG intr, ULONG io, ULONG urg) { (void)io; (void)urg; g_breakmask = intr; return 0; }
ULONG c_getdtablesize(void) { return MAXSOCK; }
ULONG c_Errno(void) { return (ULONG)g_errno; }
ULONG c_SetErrnoPtr(void *p, int size) { g_errno_ptr = p; g_errno_size = size; return 0; }
static char g_ntoa[16];
RPTR c_Inet_NtoA(ULONG in)
{
    char *p = g_ntoa;
    for (int i = 3; i >= 0; i--) {
        unsigned v = (in >> (i * 8)) & 0xFF;
        if (v >= 100) *p++ = (char)('0' + v / 100);
        if (v >= 10) *p++ = (char)('0' + v / 10 % 10);
        *p++ = (char)('0' + v % 10);
        if (i) *p++ = '.';
    }
    *p = 0;
    return (RPTR)(uintptr_t)g_ntoa;
}
static int parse_ip(const char *cp, uint32_t *out)
{
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        unsigned n = 0, digits = 0;
        while (*cp >= '0' && *cp <= '9') { n = n * 10 + (unsigned)(*cp++ - '0'); digits++; if (n > 255) return 0; }
        if (!digits) return 0;
        v = v << 8 | n;
        if (part < 3) { if (*cp++ != '.') return 0; }
    }
    if (*cp) return 0;
    *out = v;
    return 1;
}
ULONG c_inet_addr(const char *cp) { uint32_t v; return cp && parse_ip(cp, &v) ? v : 0xFFFFFFFFu; }
ULONG c_inet_aton(const char *cp, uint32_t *out) { uint32_t v; if (!cp || !parse_ip(cp, &v)) return 0; if (out) *out = v; return 1; }
ULONG c_Inet_LnaOf(ULONG in) { return (in >> 24) < 128 ? in & 0xFFFFFF : (in >> 24) < 192 ? in & 0xFFFF : in & 0xFF; }
ULONG c_Inet_NetOf(ULONG in) { return (in >> 24) < 128 ? in >> 24 : (in >> 24) < 192 ? in >> 16 : in >> 8; }
ULONG c_Inet_MakeAddr(int netw, int lna)
{
    uint32_t n = (uint32_t)netw, l = (uint32_t)lna;
    return n < 128 ? n << 24 | (l & 0xFFFFFF) : n < 65536 ? n << 16 | (l & 0xFFFF) : n << 8 | (l & 0xFF);
}
ULONG c_inet_network(const char *cp) { return c_inet_addr(cp); }
static char g_hname[256];
static uint32_t g_haddr;
static char *g_hlist[2], *g_halias[1];
static struct hostent_ g_host;
RPTR c_gethostbyname(const char *name)
{
    uint32_t ip;
    if (!name) return 0;
    strncpy(g_hname, name, sizeof g_hname - 1);
    g_hname[sizeof g_hname - 1] = 0;
    if (parse_ip(g_hname, &ip)) g_haddr = ip;
    else {
        BLOCK(q);
        q[5] = (uint32_t)(uintptr_t)g_hname;
        if (net(q, NET_RESOLVE) < 0) { g_herrno = 1; return (RPTR)0; }   /* HOST_NOT_FOUND */
        g_haddr = q[14];
    }
    g_hlist[0] = (char *)&g_haddr; g_hlist[1] = NULL; g_halias[0] = NULL;
    g_host.h_name = g_hname; g_host.h_aliases = g_halias; g_host.h_addrtype = AF_INET_; g_host.h_length = 4;
    g_host.h_addr_list = g_hlist;
    return (RPTR)(uintptr_t)&g_host;
}
ULONG c_gethostname(char *name, int len)
{
    BLOCK(q);
    if (!name || len <= 0) { set_errno(E_INVAL); return (ULONG)-1; }
    q[5] = (uint32_t)(uintptr_t)name; q[6] = (uint32_t)len;
    if (net(q, NET_HOSTNAME) < 0) { strncpy(name, "qrx-os-one", (size_t)len); name[len - 1] = 0; }
    return 0;
}
ULONG c_gethostid(void) { return 0x7F000001u; }
ULONG c_SocketBaseTagList(struct TagItem *tags)
{
    struct TagItem *t;
    if (!tags) return 0;
    for (t = tags; t->ti_Tag != TAG_END; t++) {
        if (t->ti_Tag == TAG_IGNORE) continue;
        if (t->ti_Tag == TAG_MORE) { t = (struct TagItem *)t->ti_Data - 1; continue; }
        if (!(t->ti_Tag & TAG_USER)) continue;
        ULONG code = (t->ti_Tag >> SBTB_CODE) & SBTS_CODE;
        int set = t->ti_Tag & SBTF_SET, ref = t->ti_Tag & SBTF_REF;
        IPTR *where = ref ? (IPTR *)t->ti_Data : (IPTR *)&t->ti_Data;
        switch (code) {
        case SBTC_BREAKMASK: if (set) g_breakmask = *where; else *where = g_breakmask; break;
        case SBTC_ERRNO: if (set) g_errno = (int)*where; else *where = (ULONG)g_errno; break;
        case SBTC_HERRNO: if (set) g_herrno = (int)*where; else *where = (ULONG)g_herrno; break;
        case SBTC_DTABLESIZE: if (!set) *where = MAXSOCK; break;
        case SBTC_ERRNOBYTEPTR: if (set) { g_errno_ptr = (void *)*where; g_errno_size = 1; } break;
        case SBTC_ERRNOWORDPTR: if (set) { g_errno_ptr = (void *)*where; g_errno_size = 2; } break;
        case SBTC_ERRNOLONGPTR: if (set) { g_errno_ptr = (void *)*where; g_errno_size = 4; } break;
        default: break;   /* syslog, callbacks and friends: accepted, nothing to do */
        }
    }
    return 0;
}
ULONG c_unimplemented(void) { set_errno(E_NOSYS); return (ULONG)-1; }
ULONG c_nothing(void) { return 0; }

/* ---- the library */
extern APTR qrx_hostsocket_vectors[];
#if defined(__m68k64__)
/* Vector functions are plain C functions; the base comes in A1, and there is only one */
static struct Library *g_lib;
struct Library *qrx_lib_open(void) { g_lib->lib_OpenCnt++; g_lib->lib_Flags &= ~LIBF_DELEXP; return g_lib; }
ULONG qrx_lib_close(void) { if (g_lib->lib_OpenCnt) g_lib->lib_OpenCnt--; return 0; }
ULONG qrx_lib_zero(void) { return 0; }
#else
struct Library *qrx_lib_open(struct Library *lib) { lib->lib_OpenCnt++; lib->lib_Flags &= ~LIBF_DELEXP; return lib; }
ULONG qrx_lib_close(struct Library *lib) { if (lib->lib_OpenCnt) lib->lib_OpenCnt--; return 0; }
#endif

int main(void)
{
    if (FindName(&SysBase->LibList, (CONST_STRPTR)"bsdsocket.library")) {
        PutStr((CONST_STRPTR)"QRXHostSocket: there is a bsdsocket.library already\n");
        return 5;
    }
    if (!door_ok()) { PutStr((CONST_STRPTR)"QRXHostSocket: no ARMithlon host network door\n"); return 5; }
    struct Library *lib = MakeLibrary(qrx_hostsocket_vectors, NULL, NULL, sizeof(struct Library), 0);
    if (!lib) { PutStr((CONST_STRPTR)"QRXHostSocket: no memory\n"); return 20; }
#if defined(__m68k64__)
    g_lib = lib;
#endif
    lib->lib_Node.ln_Type = NT_LIBRARY;
    lib->lib_Node.ln_Name = (char *)"bsdsocket.library";
    lib->lib_IdString = (APTR)"bsdsocket.library 4.1 (QRX host sockets through ARMithlon)";
    lib->lib_Version = 4;
    lib->lib_Revision = 1;
    lib->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    AddLibrary(lib);
    /* The code must stay: this process never ends. */
    Wait(0);
    return 0;
}
