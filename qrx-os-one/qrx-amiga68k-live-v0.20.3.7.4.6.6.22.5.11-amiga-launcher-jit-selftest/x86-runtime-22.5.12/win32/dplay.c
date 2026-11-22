/* QRX OS One - qrx-win32: DirectPlay sessions on the host's sockets (net.c).
 *
 * Only what Grand Theft Auto calls: provider enumeration, DirectPlayCreate, QueryInterface for
 * IDirectPlay2A, EnumSessions, Open, CreatePlayer, DestroyPlayer, EnumPlayers, Send, Receive,
 * Close, Release. Any other method stops the program and names itself, like every missing API.
 *
 * The protocol on the wire is QRX86's own and does not talk to real DirectPlay. A search sends UDP
 * probes to ports 47800..47807, on the loopback address, as a broadcast and, if set, to the
 * host named by the environment variable QRX_DPLAY_HOST. The session host holds one TCP connection
 * to each member, hands out the player ids and relays their data, so "guaranteed" and plain
 * messages both arrive complete and in order. Multi-byte fields are big-endian. */
#include "qrxwin.h"
#include <stdlib.h>
#include <string.h>

#define C (&w->cpu)
#define RD32(x) x86_rd32(C, (x))
#define WR32(x, v) x86_wr32(C, (x), (v))
#define WR8(x, v) x86_wr8(C, (x), (uint8_t)(v))

#define DP_OK 0u
#define DPERR(code) (0x88770000u | (code))
#define DPERR_ALREADYINITIALIZED DPERR(5)
#define DPERR_BUFFERTOOSMALL DPERR(30)
#define DPERR_CANTADDPLAYER DPERR(40)
#define DPERR_CANTCREATEPLAYER DPERR(60)
#define DPERR_CANTCREATESESSION DPERR(70)
#define DPERR_INVALIDFLAGS DPERR(120)
#define DPERR_INVALIDPLAYER DPERR(150)
#define DPERR_NOCONNECTION DPERR(170)
#define DPERR_NOMESSAGES DPERR(190)
#define DPERR_NOSESSIONS DPERR(220)
#define DPERR_SENDTOOBIG DPERR(230)
#define DPERR_UNAVAILABLE DPERR(250)
#define DPERR_UNSUPPORTED 0x80004001u
#define DPERR_INVALIDPARAMS 0x80070057u
#define E_NOINTERFACE_ 0x80004002u

#define WSAEWOULDBLOCK 10035u
#define WSAEINPROGRESS_CONNECT 10036u
#define DP_LOOPBACK 0x7F000001u
#define DP_BROADCAST 0xFFFFFFFFu
#define DP_UDP_PORT 47800u
#define DP_UDP_PORTS 8u
#define DP_SESSDESC_SIZE 0x50u
#define DP_OPEN_JOIN 1u
#define DP_OPEN_CREATE 2u
#define DP_ESC_TIMEDOUT 1u
#define DP_ENUM_MS 1500u
#define DP_WAIT_MS 5000u
#define DP_MAXOBJ 8
#define DP_MAXPLAYER 16
#define DP_MAXCONN 15
#define DP_MAXSESS 8
#define DP_MAXMINE 8
#define DP_MAXMSG 8192u
#define DP_NAME 32
#define DP_FRAME (DP_MAXMSG + 64u)
#define DP_INBUF (DP_FRAME + 4u)
#define DP_OUTBUF (64u * 1024u)
#define DP_QUEUE_MAX 2048
#define DP_PROTO 1u
#define DP_FIRST_ID 0x100u
#define DP_SEND_GUARANTEED 1u

enum { PROBE = 1, PROBE_REPLY };
enum { F_HELLO = 1, F_WELCOME, F_ROSTER, F_PLAYER_REQ, F_PLAYER_ACK, F_PLAYER_DEL, F_DATA };
enum { MODE_CLOSED, MODE_HOST, MODE_MEMBER };

typedef struct dpmsg { struct dpmsg *next; uint32_t from, to, len; uint8_t data[]; } dpmsg;
typedef struct dpconn {
    uint32_t sock, inlen, outlen;
    int dead;
    uint8_t in[DP_INBUF], out[DP_OUTBUF];
} dpconn;
typedef struct dpplayer { uint32_t id; int conn, local; char name[DP_NAME]; } dpplayer;
typedef struct dpsess { uint8_t inst[16], app[16]; uint32_t ip, port, maxp, cur; char name[DP_NAME]; } dpsess;
typedef struct qdp {
    int used, refs, mode, npl, nknown, qn, ack_got, welcomed, lost;
    uint32_t face1, face2, udp, lsn, tcp_port, maxp, next_id, ack_id;
    uint8_t app[16], inst[16];
    char name[DP_NAME];
    dpplayer pl[DP_MAXPLAYER];
    uint32_t mine[DP_MAXMINE];
    dpconn *c[DP_MAXCONN];            /* a member uses c[0], its connection to the host */
    dpsess known[DP_MAXSESS];
    dpmsg *qh, *qt;
    uint32_t sent, received, dropped;
} qdp;

static qdp g_dp[DP_MAXOBJ];
static uint32_t g_vt1, g_vt2a;
static int g_service;
static uint32_t g_q[16];              /* door block: q[12] result, q[13] error, q[14] ip, q[15] port */
static uint8_t g_f[DP_FRAME], g_udp[256];

static const uint8_t GUID_TCPIP[16] = { 0xE0, 0x5E, 0xE9, 0x36, 0x77, 0x85, 0xCF, 0x11, 0x96, 0x0C, 0x00, 0x80, 0xC7, 0x53, 0x4E, 0x82 };
static const uint8_t IID_UNKNOWN[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
static const uint8_t IID_DP1[16] = { 0xA0, 0xE9, 0x54, 0x54, 0x65, 0xDB, 0xCE, 0x11, 0x92, 0x1C, 0x00, 0xAA, 0x00, 0x6C, 0x49, 0x72 };
static const uint8_t IID_DP2A[16] = { 0x80, 0x05, 0x46, 0x9D, 0x22, 0xA8, 0xCF, 0x11, 0x96, 0x0C, 0x00, 0x80, 0xC7, 0x53, 0x4E, 0x82 };

/* ------------------------------------------------------------------ small helpers */
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int net(qwin *w, uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    g_q[4] = a0; g_q[5] = a1; g_q[6] = a2; g_q[7] = a3; g_q[8] = a4; g_q[9] = a5;
    g_q[10] = 0; g_q[11] = 0; g_q[14] = 0; g_q[15] = 0;
    return qw_net_call(w, g_q, op);
}
static uint32_t buf32(const void *p) { return (uint32_t)(uintptr_t)p; }
static qdp *self(qwin *w, uint32_t face)
{
    uint32_t i = RD32(face + 4);
    return i < DP_MAXOBJ && g_dp[i].used ? &g_dp[i] : NULL;
}
static dpplayer *player(qdp *d, uint32_t id)
{
    int i;
    for (i = 0; i < d->npl; i++) if (d->pl[i].id == id) return &d->pl[i];
    return NULL;
}
static int is_mine(qdp *d, uint32_t id)
{
    int i;
    for (i = 0; i < DP_MAXMINE; i++) if (id && d->mine[i] == id) return 1;
    return 0;
}
static void copy_name(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n >= DP_NAME) n = DP_NAME - 1u;
    memmove(dst, src, n);
    dst[n] = 0;
}
static void queue_push(qdp *d, uint32_t from, uint32_t to, uint32_t flags, const uint8_t *data, uint32_t len)
{
    dpmsg *m;
    /* All of it travels over TCP, so only a program that stopped reading can lose plain messages. */
    if (d->qn >= DP_QUEUE_MAX && !(flags & DP_SEND_GUARANTEED)) { d->dropped++; return; }
    m = (dpmsg *)malloc(sizeof(dpmsg) + len);
    if (!m) { d->dropped++; return; }
    m->next = NULL; m->from = from; m->to = to; m->len = len;
    memcpy(m->data, data, len);
    if (d->qt) d->qt->next = m; else d->qh = m;
    d->qt = m;
    d->qn++;
    d->received++;
}

/* ------------------------------------------------------------------ connections */
static dpconn *conn_new(uint32_t sock)
{
    dpconn *c = (dpconn *)malloc(sizeof(dpconn));
    if (c) { c->sock = sock; c->inlen = 0; c->outlen = 0; c->dead = 0; }
    return c;
}
static void conn_put(dpconn *c, const uint8_t *f, uint32_t n)
{
    if (c->dead) return;
    if (c->outlen + 4u + n > DP_OUTBUF) { c->dead = 1; return; }    /* the peer stopped reading long ago */
    put32(c->out + c->outlen, n);
    memcpy(c->out + c->outlen + 4u, f, n);
    c->outlen += 4u + n;
}
static void conn_flush(qwin *w, dpconn *c)
{
    while (c->outlen && !c->dead) {
        if (!net(w, NET_SEND, c->sock, buf32(c->out), c->outlen, 0, 0, 0)) {
            if (g_q[13] != WSAEWOULDBLOCK) c->dead = 1;
            return;
        }
        if (g_q[12] < c->outlen) memmove(c->out, c->out + g_q[12], c->outlen - g_q[12]);
        c->outlen -= g_q[12];
    }
}
static void conn_free(qwin *w, dpconn *c)
{
    /* Whatever the peer still sends must not turn our close into a reset that loses our last data. */
    conn_flush(w, c);
    net(w, NET_SHUTDOWN, c->sock, 1, 0, 0, 0, 0);
    while (net(w, NET_RECV, c->sock, buf32(c->in), DP_INBUF, 0, 0, 0) && g_q[12]) { }
    net(w, NET_CLOSE, c->sock, 0, 0, 0, 0, 0);
    free(c);
}

/* ------------------------------------------------------------------ host side */
static void roster_send(qwin *w, qdp *d)
{
    uint32_t n = 5;
    int i;
    g_f[0] = F_ROSTER;
    put32(g_f + 1, (uint32_t)d->npl);
    for (i = 0; i < d->npl; i++) {
        uint32_t len = (uint32_t)strlen(d->pl[i].name);
        put32(g_f + n, d->pl[i].id);
        g_f[n + 4] = (uint8_t)len;
        memcpy(g_f + n + 5, d->pl[i].name, len);
        n += 5u + len;
    }
    for (i = 0; i < DP_MAXCONN; i++) if (d->c[i]) { conn_put(d->c[i], g_f, n); conn_flush(w, d->c[i]); }
}
static uint32_t host_add_player(qdp *d, int conn, const char *name)
{
    dpplayer *p;
    if (d->npl >= DP_MAXPLAYER || (d->maxp && (uint32_t)d->npl >= d->maxp)) return 0;
    p = &d->pl[d->npl++];
    p->id = d->next_id++;
    p->conn = conn;
    p->local = conn < 0;
    copy_name(p->name, name);
    return p->id;
}
static void host_remove_player(qdp *d, int index)
{
    int i;
    for (i = index; i + 1 < d->npl; i++) d->pl[i] = d->pl[i + 1];
    d->npl--;
}
static void host_frame(qwin *w, qdp *d, int ci, const uint8_t *f, uint32_t n)
{
    dpconn *c = d->c[ci];
    char name[DP_NAME];
    uint32_t len;
    int i;
    switch (f[0]) {
    case F_HELLO:
        if (n >= 9 && !memcmp(f + 1, "QRXD", 4) && get32(f + 5) == DP_PROTO) {
            g_f[0] = F_WELCOME;
            conn_put(c, g_f, 1);
            roster_send(w, d);
        } else c->dead = 1;
        break;
    case F_PLAYER_REQ: {
        uint32_t id;
        if (n < 2) break;
        len = n - 1u < DP_NAME - 1u ? n - 1u : DP_NAME - 1u;
        memcpy(name, f + 1, len);
        name[len] = 0;
        id = host_add_player(d, ci, name);
        if (id) roster_send(w, d);       /* first, so the new player's own EnumPlayers finds itself */
        g_f[0] = F_PLAYER_ACK;
        put32(g_f + 1, id);
        conn_put(c, g_f, 5);
        break;
    }
    case F_PLAYER_DEL:
        if (n < 5) break;
        for (i = 0; i < d->npl; i++) if (d->pl[i].id == get32(f + 1) && d->pl[i].conn == ci) {
            host_remove_player(d, i);
            roster_send(w, d);
            break;
        }
        break;
    case F_DATA: {
        dpplayer *from, *to;
        if (n < 13) break;
        from = player(d, get32(f + 1));
        to = player(d, get32(f + 5));
        if (!from || !to || from->conn != ci) break;     /* the target may just have left */
        if (to->conn < 0) queue_push(d, from->id, to->id, get32(f + 9), f + 13, n - 13u);
        else if (d->c[to->conn]) conn_put(d->c[to->conn], f, n);
        break;
    }
    default: c->dead = 1; break;
    }
}
/* ------------------------------------------------------------------ member side */
static void member_frame(qdp *d, const uint8_t *f, uint32_t n)
{
    switch (f[0]) {
    case F_WELCOME: d->welcomed = 1; break;
    case F_ROSTER: {
        uint32_t count, pos = 5, i;
        if (n < 5) break;
        count = get32(f + 1);
        if (count > DP_MAXPLAYER) break;
        d->npl = 0;
        for (i = 0; i < count && pos + 5u <= n; i++) {
            dpplayer *p = &d->pl[d->npl++];
            uint32_t len = f[pos + 4] < DP_NAME ? f[pos + 4] : DP_NAME - 1u;
            p->id = get32(f + pos);
            p->conn = 0;
            p->local = is_mine(d, p->id);
            memcpy(p->name, f + pos + 5, len);
            p->name[len] = 0;
            pos += 5u + f[pos + 4];
        }
        break;
    }
    case F_PLAYER_ACK:
        if (n >= 5) { d->ack_id = get32(f + 1); d->ack_got = 1; }
        break;
    case F_DATA:
        if (n >= 13) queue_push(d, get32(f + 1), get32(f + 5), get32(f + 9), f + 13, n - 13u);
        break;
    default: break;
    }
}
/* ------------------------------------------------------------------ pumping */
static void conn_pump(qwin *w, qdp *d, int ci)
{
    dpconn *c = d->c[ci];
    conn_flush(w, c);
    while (!c->dead) {
        uint32_t pos = 0;
        if (!net(w, NET_RECV, c->sock, buf32(c->in + c->inlen), DP_INBUF - c->inlen, 0, 0, 0)) {
            if (g_q[13] != WSAEWOULDBLOCK) c->dead = 1;
            break;
        }
        if (!g_q[12]) { c->dead = 1; break; }
        c->inlen += g_q[12];
        while (c->inlen - pos >= 4u && !c->dead) {
            uint32_t n = get32(c->in + pos);
            if (!n || n > DP_FRAME) { c->dead = 1; break; }
            if (c->inlen - pos - 4u < n) break;
            if (d->mode == MODE_HOST) host_frame(w, d, ci, c->in + pos + 4u, n);
            else member_frame(d, c->in + pos + 4u, n);
            pos += 4u + n;
        }
        if (pos && pos < c->inlen) memmove(c->in, c->in + pos, c->inlen - pos);
        c->inlen -= pos;
    }
    conn_flush(w, c);
}
static void host_answer_probes(qwin *w, qdp *d)
{
    while (net(w, NET_RECVFROM, d->udp, buf32(g_udp), sizeof(g_udp), 0, 0, 0)) {
        uint32_t ip = g_q[14], port = g_q[15], len;
        if (g_q[12] < 22u || memcmp(g_udp, "QRXD", 4) || g_udp[4] != DP_PROTO || g_udp[5] != PROBE) continue;
        if (memcmp(g_udp + 6, d->app, 16)) {
            static const uint8_t none[16];
            if (memcmp(g_udp + 6, none, 16)) continue;       /* a search for another program */
        }
        len = (uint32_t)strlen(d->name);
        g_udp[5] = PROBE_REPLY;
        memcpy(g_udp + 6, d->inst, 16);
        memcpy(g_udp + 22, d->app, 16);
        put32(g_udp + 38, d->tcp_port);
        put32(g_udp + 42, d->maxp);
        put32(g_udp + 46, (uint32_t)d->npl);
        g_udp[50] = (uint8_t)len;
        memcpy(g_udp + 51, d->name, len);
        net(w, NET_SENDTO, d->udp, buf32(g_udp), 51u + len, 0, ip, port);
    }
}
static void host_accept(qwin *w, qdp *d)
{
    while (net(w, NET_ACCEPT, d->lsn, 0, 0, 0, 0, 0)) {
        uint32_t s = g_q[12];
        int i;
        for (i = 0; i < DP_MAXCONN; i++) if (!d->c[i]) break;
        net(w, NET_SETOPT, s, 6, 1, 1, 0, 0);       /* TCP_NODELAY: a game sends small packets */
        if (i >= DP_MAXCONN || !(d->c[i] = conn_new(s))) net(w, NET_CLOSE, s, 0, 0, 0, 0, 0);
    }
}
static void obj_pump(qwin *w, qdp *d)
{
    int i;
    if (d->mode == MODE_HOST) {
        int changed = 0;
        host_answer_probes(w, d);
        host_accept(w, d);
        for (i = 0; i < DP_MAXCONN; i++) if (d->c[i]) conn_pump(w, d, i);
        for (i = 0; i < DP_MAXCONN; i++) if (d->c[i]) {
            conn_flush(w, d->c[i]);
            if (d->c[i]->dead) {
                int k = 0;
                conn_free(w, d->c[i]);
                d->c[i] = NULL;
                while (k < d->npl) {
                    if (d->pl[k].conn == i) { host_remove_player(d, k); changed = 1; }
                    else k++;
                }
            }
        }
        if (changed) roster_send(w, d);
    } else if (d->mode == MODE_MEMBER && d->c[0]) {
        conn_pump(w, d, 0);
        if (d->c[0]->dead) {
            conn_free(w, d->c[0]);
            d->c[0] = NULL;
            d->lost = 1;
            d->npl = 0;
            if (w->verbose) qw_log(w, "DirectPlay: connection to the session host lost");
        }
    }
}
/* Keeps a session answering while the program is busy: loading a level, say. */
static void service_main(qwin *w)
{
    uint64_t last = w->be->now(w);
    while (!w->exiting) {
        int i;
        for (i = 0; i < DP_MAXOBJ; i++) if (g_dp[i].used && g_dp[i].mode != MODE_CLOSED) obj_pump(w, &g_dp[i]);
        if (w->verbose && w->be->now(w) - last >= 5000u) {
            last = w->be->now(w);
            for (i = 0; i < DP_MAXOBJ; i++) if (g_dp[i].used && g_dp[i].mode != MODE_CLOSED)
                qw_log(w, "DirectPlay: %s, %d players, sent %u, received %u, dropped %u", g_dp[i].mode == MODE_HOST ? "host" : "member",
                       g_dp[i].npl, g_dp[i].sent, g_dp[i].received, g_dp[i].dropped);
        }
        qw_thread_sleep(w, 2);
    }
}
static void session_close(qwin *w, qdp *d)
{
    int i;
    dpmsg *m = d->qh;
    for (i = 0; i < DP_MAXCONN; i++) if (d->c[i]) { conn_free(w, d->c[i]); d->c[i] = NULL; }
    if (d->mode == MODE_HOST) {
        net(w, NET_CLOSE, d->udp, 0, 0, 0, 0, 0);
        net(w, NET_CLOSE, d->lsn, 0, 0, 0, 0, 0);
    }
    while (m) { dpmsg *n = m->next; free(m); m = n; }
    d->qh = d->qt = NULL;
    d->qn = 0;
    d->mode = MODE_CLOSED;
    d->npl = 0;
    d->lost = 0;
    memset(d->mine, 0, sizeof(d->mine));
}

/* ------------------------------------------------------------------ session search */
static void probe_one(qwin *w, uint32_t sock, uint32_t n, uint32_t ip)
{
    uint32_t i;
    for (i = 0; i < DP_UDP_PORTS; i++) net(w, NET_SENDTO, sock, buf32(g_udp), n, 0, ip, DP_UDP_PORT + i);
}
static void probe_send(qwin *w, qdp *d, uint32_t sock, uint32_t named_ip)
{
    memcpy(g_udp, "QRXD", 4);
    g_udp[4] = DP_PROTO;
    g_udp[5] = PROBE;
    memcpy(g_udp + 6, d->app, 16);
    probe_one(w, sock, 22, DP_LOOPBACK);
    probe_one(w, sock, 22, DP_BROADCAST);
    if (named_ip) probe_one(w, sock, 22, named_ip);
}
static void probe_read(qwin *w, qdp *d, uint32_t sock)
{
    while (net(w, NET_RECVFROM, sock, buf32(g_udp), sizeof(g_udp), 0, 0, 0)) {
        uint32_t ip = g_q[14], n = g_q[12], len;
        dpsess *s = NULL;
        int i;
        if (n < 51u || memcmp(g_udp, "QRXD", 4) || g_udp[4] != DP_PROTO || g_udp[5] != PROBE_REPLY) continue;
        for (i = 0; i < d->nknown; i++) if (!memcmp(d->known[i].inst, g_udp + 6, 16)) s = &d->known[i];
        if (!s) {
            if (d->nknown >= DP_MAXSESS) continue;
            s = &d->known[d->nknown++];
        }
        memcpy(s->inst, g_udp + 6, 16);
        memcpy(s->app, g_udp + 22, 16);
        s->ip = ip;
        s->port = get32(g_udp + 38);
        s->maxp = get32(g_udp + 42);
        s->cur = get32(g_udp + 46);
        len = g_udp[50];
        if (len > n - 51u) len = n - 51u;
        if (len >= DP_NAME) len = DP_NAME - 1u;
        memcpy(s->name, g_udp + 51, len);
        s->name[len] = 0;
    }
}
static uint32_t named_host(qwin *w)
{
    static char name[128];
    const char *e = getenv("QRX_DPLAY_HOST");
    if (!e || !*e) return 0;
    strncpy(name, e, sizeof(name) - 1);
    if (!net(w, NET_RESOLVE, 0, buf32(name), 0, 0, 0, 0)) {
        qw_log(w, "DirectPlay: QRX_DPLAY_HOST %s not found", name);
        return 0;
    }
    return g_q[14];
}
/* One search: replies replace what an earlier search knew. */
static uint32_t search(qwin *w, qdp *d, uint32_t ms)
{
    uint64_t end = w->be->now(w) + ms, next = 0;
    uint32_t sock, named;
    if (!net(w, NET_SOCKET, 2, 0, 0, 0, 0, 0)) return DPERR_UNAVAILABLE;
    sock = g_q[12];
    net(w, NET_SETOPT, sock, 0xFFFFu, 0x20, 1, 0, 0);       /* SO_BROADCAST */
    named = named_host(w);
    d->nknown = 0;
    while (w->be->now(w) < end && !w->exiting) {
        if (w->be->now(w) >= next) {
            probe_send(w, d, sock, named);
            next = w->be->now(w) + 250u;
        }
        probe_read(w, d, sock);
        qw_thread_block(w, 2);
    }
    probe_read(w, d, sock);
    net(w, NET_CLOSE, sock, 0, 0, 0, 0, 0);
    return DP_OK;
}

/* ------------------------------------------------------------------ IUnknown */
static uint32_t dp_QueryInterface(qwin *w, uint32_t *a)
{
    qdp *d = self(w, a[0]);
    uint8_t iid[16];
    if (!d || !a[2]) return DPERR_INVALIDPARAMS;
    x86_read_mem(C, a[1], iid, 16);
    if (!memcmp(iid, IID_DP2A, 16)) {
        if (!d->face2) {
            d->face2 = qw_alloc(w, 16);
            WR32(d->face2, g_vt2a);
            WR32(d->face2 + 4, RD32(a[0] + 4));
        }
        WR32(a[2], d->face2);
    } else if (!memcmp(iid, IID_DP1, 16) || !memcmp(iid, IID_UNKNOWN, 16)) {
        WR32(a[2], d->face1);
    } else {
        char s[80];
        snprintf(s, sizeof(s), "%02x%02x%02x%02x", iid[3], iid[2], iid[1], iid[0]);
        qw_log(w, "DirectPlay: interface %s... not implemented", s);
        WR32(a[2], 0);
        return E_NOINTERFACE_;
    }
    d->refs++;
    return DP_OK;
}
static uint32_t dp_AddRef(qwin *w, uint32_t *a)
{
    qdp *d = self(w, a[0]);
    return d ? (uint32_t)++d->refs : 0;
}
static uint32_t dp_Release(qwin *w, uint32_t *a)
{
    qdp *d = self(w, a[0]);
    if (!d) return 0;
    if (--d->refs > 0) return (uint32_t)d->refs;
    session_close(w, d);
    if (d->face1) qw_free(w, d->face1);
    if (d->face2) qw_free(w, d->face2);
    memset(d, 0, sizeof(*d));
    return 0;
}

/* ------------------------------------------------------------------ IDirectPlay2A */
static uint32_t dp_EnumSessions(qwin *w, uint32_t *a)
{
    /* this, desc, timeout, callback, context, flags */
    qdp *d = self(w, a[0]);
    uint32_t timeout = a[2] ? a[2] : DP_ENUM_MS, r, sd, tp;
    uint32_t args[4];
    int i, ok, go;
    if (w->verbose) qw_log(w, "DirectPlay: EnumSessions timeout=%u flags=%x", a[2], a[5]);
    if (!d || !a[1] || !a[3] || RD32(a[1]) != DP_SESSDESC_SIZE) return DPERR_INVALIDPARAMS;
    if (a[5] & ~3u) return DPERR_INVALIDFLAGS;
    x86_read_mem(C, a[1] + 0x18, d->app, 16);
    sd = qw_alloc(w, DP_SESSDESC_SIZE + DP_NAME + 4u);
    if (!sd) return DPERR_UNAVAILABLE;
    tp = sd + DP_SESSDESC_SIZE + DP_NAME;
    do {
        dpsess found[DP_MAXSESS];
        int count;
        r = search(w, d, timeout);
        if (r) break;
        memcpy(found, d->known, sizeof(found));
        count = d->nknown;
        go = 1;
        if (w->verbose) qw_log(w, "DirectPlay: found %d sessions", count);
        for (i = 0; i < count && go; i++) {
            uint32_t n;
            for (n = 0; n < DP_SESSDESC_SIZE; n++) WR8(sd + n, 0);
            WR32(sd, DP_SESSDESC_SIZE);
            x86_write_mem(C, sd + 8, found[i].inst, 16);
            x86_write_mem(C, sd + 0x18, found[i].app, 16);
            WR32(sd + 0x28, found[i].maxp);
            WR32(sd + 0x2C, found[i].cur);
            WR32(sd + 0x30, sd + DP_SESSDESC_SIZE);
            x86_write_mem(C, sd + DP_SESSDESC_SIZE, found[i].name, DP_NAME);
            WR32(tp, timeout);
            args[0] = sd; args[1] = tp; args[2] = 0; args[3] = a[4];
            go = x86_call(C, a[3], 4, args, &ok) != 0;
            if (C->fault || !ok) { qw_free(w, sd); return 0; }
        }
        if (!go) break;
        WR32(tp, timeout);
        args[0] = 0; args[1] = tp; args[2] = DP_ESC_TIMEDOUT; args[3] = a[4];
        go = x86_call(C, a[3], 4, args, &ok) != 0;
        if (C->fault || !ok) { qw_free(w, sd); return 0; }
        if (go) timeout = RD32(tp) ? RD32(tp) : DP_ENUM_MS;
    } while (go && !w->exiting);
    qw_free(w, sd);
    return r;
}
static void service_up(qwin *w)
{
    if (!g_service) { g_service = 1; qw_thread_spawn_service(w, service_main); }
}
static uint32_t open_create(qwin *w, qdp *d, uint32_t desc)
{
    static uint32_t counter;
    uint32_t i, stamp;
    char name[DP_NAME] = "";
    if (!net(w, NET_SOCKET, 1, 0, 0, 0, 0, 0)) return DPERR_CANTCREATESESSION;
    d->lsn = g_q[12];
    if (!net(w, NET_BIND, d->lsn, 0, 0, 0, 0, 0) || !net(w, NET_LISTEN, d->lsn, 8, 0, 0, 0, 0) || !net(w, NET_SOCKNAME, d->lsn, 0, 0, 0, 0, 0)) {
        net(w, NET_CLOSE, d->lsn, 0, 0, 0, 0, 0);
        return DPERR_CANTCREATESESSION;
    }
    d->tcp_port = g_q[15];
    if (!net(w, NET_SOCKET, 2, 0, 0, 0, 0, 0)) {
        net(w, NET_CLOSE, d->lsn, 0, 0, 0, 0, 0);
        return DPERR_CANTCREATESESSION;
    }
    d->udp = g_q[12];
    /* Several hosts on one computer, as with two emulators: each takes the next free port. */
    for (i = 0; i < DP_UDP_PORTS; i++) if (net(w, NET_BIND, d->udp, 0, DP_UDP_PORT + i, 0, 0, 0)) break;
    if (i >= DP_UDP_PORTS) {
        net(w, NET_CLOSE, d->udp, 0, 0, 0, 0, 0);
        net(w, NET_CLOSE, d->lsn, 0, 0, 0, 0, 0);
        qw_log(w, "DirectPlay: all %u search ports are in use", DP_UDP_PORTS);
        return DPERR_CANTCREATESESSION;
    }
    x86_read_mem(C, desc + 0x18, d->app, 16);
    d->maxp = RD32(desc + 0x28);
    if (RD32(desc + 0x30)) qw_rd_str(w, RD32(desc + 0x30), name, DP_NAME);
    copy_name(d->name, name);
    stamp = (uint32_t)w->be->now(w);
    put32(d->inst, stamp);
    put32(d->inst + 4, d->tcp_port);
    put32(d->inst + 8, DP_UDP_PORT + i);
    put32(d->inst + 12, ++counter * 2654435761u ^ stamp);
    d->next_id = DP_FIRST_ID;
    d->npl = 0;
    d->mode = MODE_HOST;
    if (w->verbose) qw_log(w, "DirectPlay: hosting '%s' for %u players, TCP port %u, search port %u", d->name, d->maxp, d->tcp_port, DP_UDP_PORT + i);
    return DP_OK;
}
static uint32_t open_join(qwin *w, qdp *d, uint32_t desc)
{
    uint8_t inst[16], hello[9];
    dpsess *s = NULL;
    uint64_t end;
    uint32_t sock;
    int i;
    x86_read_mem(C, desc + 8, inst, 16);
    for (i = 0; i < d->nknown; i++) if (!memcmp(d->known[i].inst, inst, 16)) s = &d->known[i];
    if (!s) return DPERR_INVALIDPARAMS;
    if (!net(w, NET_SOCKET, 1, 0, 0, 0, 0, 0)) return DPERR_NOCONNECTION;
    sock = g_q[12];
    net(w, NET_SETOPT, sock, 6, 1, 1, 0, 0);
    end = w->be->now(w) + DP_WAIT_MS;
    if (!net(w, NET_CONNECT, sock, s->ip, s->port, 0, 0, 0)) {
        int done = 0;
        if (g_q[13] != WSAEINPROGRESS_CONNECT && g_q[13] != WSAEWOULDBLOCK) {
            net(w, NET_CLOSE, sock, 0, 0, 0, 0, 0);
            return DPERR_NOCONNECTION;
        }
        /* Sending before it is finished would fail, and that would look like a lost connection. */
        while (!done && !w->exiting && w->be->now(w) < end) {
            if (!net(w, NET_POLL, sock, 2, 0, 0, 0, 0)) break;
            if (g_q[12] & 2u) { done = g_q[13] ? -1 : 1; break; }
            qw_thread_block(w, 2);
        }
        if (done != 1) {
            net(w, NET_CLOSE, sock, 0, 0, 0, 0, 0);
            return DPERR_NOCONNECTION;
        }
    }
    d->c[0] = conn_new(sock);
    if (!d->c[0]) { net(w, NET_CLOSE, sock, 0, 0, 0, 0, 0); return DPERR_NOCONNECTION; }
    memcpy(d->app, s->app, 16);
    memcpy(d->inst, s->inst, 16);
    copy_name(d->name, s->name);
    d->maxp = s->maxp;
    d->npl = 0;
    d->lost = 0;
    d->welcomed = 0;
    hello[0] = F_HELLO;
    memcpy(hello + 1, "QRXD", 4);
    put32(hello + 5, DP_PROTO);
    conn_put(d->c[0], hello, sizeof(hello));
    d->mode = MODE_MEMBER;
    service_up(w);
    while (!d->welcomed && !d->lost && !w->exiting && w->be->now(w) < end) {
        obj_pump(w, d);
        if (!d->welcomed) qw_thread_block(w, 2);
    }
    if (!d->welcomed) {
        session_close(w, d);
        return DPERR_NOCONNECTION;
    }
    if (w->verbose) qw_log(w, "DirectPlay: joined '%s'", d->name);
    return DP_OK;
}
static uint32_t dp_Open(qwin *w, uint32_t *a)
{
    qdp *d = self(w, a[0]);
    uint32_t r;
    if (w->verbose) qw_log(w, "DirectPlay: Open flags=%x", a[2]);
    if (!d || !a[1] || RD32(a[1]) != DP_SESSDESC_SIZE) return DPERR_INVALIDPARAMS;
    if (d->mode != MODE_CLOSED) return DPERR_ALREADYINITIALIZED;
    if (a[2] == DP_OPEN_CREATE) {
        r = open_create(w, d, a[1]);
        if (!r) service_up(w);
        return r;
    }
    if (a[2] == DP_OPEN_JOIN) return open_join(w, d, a[1]);
    return DPERR_INVALIDFLAGS;
}
static uint32_t dp_Close(qwin *w, uint32_t *a)
{
    qdp *d = self(w, a[0]);
    if (w->verbose) qw_log(w, "DirectPlay: Close");
    if (d) {
        if (w->verbose && d->mode != MODE_CLOSED) qw_log(w, "DirectPlay: closing, sent %u, received %u, dropped %u", d->sent, d->received, d->dropped);
        session_close(w, d);
    }
    return DP_OK;
}
static uint32_t dp_CreatePlayer(qwin *w, uint32_t *a)
{
    /* this, *id, name, event, data, size, flags */
    qdp *d = self(w, a[0]);
    char name[DP_NAME] = "";
    uint32_t id = 0;
    if (!d || !a[1]) return DPERR_INVALIDPARAMS;
    if (a[2] && RD32(a[2] + 8)) qw_rd_str(w, RD32(a[2] + 8), name, DP_NAME);
    if (w->verbose) qw_log(w, "DirectPlay: CreatePlayer '%s'", name);
    if (d->mode == MODE_HOST) {
        id = host_add_player(d, -1, name);
        if (id) roster_send(w, d);
    } else if (d->mode == MODE_MEMBER && d->c[0]) {
        uint64_t end = w->be->now(w) + DP_WAIT_MS;
        int i;
        uint32_t n = (uint32_t)strlen(name);
        g_f[0] = F_PLAYER_REQ;
        memcpy(g_f + 1, name, n);
        conn_put(d->c[0], g_f, 1u + n);
        d->ack_got = 0;
        while (!d->ack_got && !d->lost && !w->exiting && w->be->now(w) < end) {
            obj_pump(w, d);
            if (!d->ack_got) qw_thread_block(w, 2);
        }
        if (d->ack_got && d->ack_id) {
            dpplayer *p = player(d, d->ack_id);
            id = d->ack_id;
            for (i = 0; i < DP_MAXMINE; i++) if (!d->mine[i]) { d->mine[i] = id; break; }
            if (p) p->local = 1;
        }
    } else return DPERR_CANTCREATEPLAYER;
    if (!id) return DPERR_CANTADDPLAYER;
    WR32(a[1], id);
    if (w->verbose) qw_log(w, "DirectPlay: player '%s' has id %x", name, id);
    return DP_OK;
}
static uint32_t dp_DestroyPlayer(qwin *w, uint32_t *a)
{
    qdp *d = self(w, a[0]);
    int i;
    if (w->verbose) qw_log(w, "DirectPlay: DestroyPlayer %x", a[1]);
    if (!d) return DPERR_INVALIDPARAMS;
    if (d->mode == MODE_HOST) {
        for (i = 0; i < d->npl; i++) if (d->pl[i].id == a[1] && d->pl[i].local) {
            host_remove_player(d, i);
            roster_send(w, d);
            return DP_OK;
        }
    } else if (d->mode == MODE_MEMBER && is_mine(d, a[1])) {
        for (i = 0; i < DP_MAXMINE; i++) if (d->mine[i] == a[1]) d->mine[i] = 0;
        for (i = 0; i < d->npl; i++) if (d->pl[i].id == a[1]) { host_remove_player(d, i); break; }
        if (d->c[0]) {
            g_f[0] = F_PLAYER_DEL;
            put32(g_f + 1, a[1]);
            conn_put(d->c[0], g_f, 5);
            conn_flush(w, d->c[0]);
        }
        return DP_OK;
    }
    return DPERR_INVALIDPLAYER;
}
static uint32_t dp_EnumPlayers(qwin *w, uint32_t *a)
{
    /* this, session, callback, context, flags */
    qdp *d = self(w, a[0]);
    static int logged;
    dpplayer list[DP_MAXPLAYER];
    uint32_t name, args[5];
    int i, n, ok;
    if (w->verbose && !logged) { logged = 1; qw_log(w, "DirectPlay: EnumPlayers (logged once)"); }
    if (!d || !a[2]) return DPERR_INVALIDPARAMS;
    if (a[1]) return DPERR_UNSUPPORTED;          /* players of a session not joined */
    if (d->mode == MODE_CLOSED) return DPERR_NOSESSIONS;
    obj_pump(w, d);
    n = d->npl;
    memcpy(list, d->pl, sizeof(dpplayer) * (size_t)n);
    name = qw_alloc(w, 16u + DP_NAME);
    if (!name) return DPERR_UNAVAILABLE;
    for (i = 0; i < n; i++) {
        WR32(name, 16); WR32(name + 4, 0); WR32(name + 8, name + 16); WR32(name + 12, 0);
        x86_write_mem(C, name + 16, list[i].name, DP_NAME);
        args[0] = list[i].id; args[1] = 1 /* DPPLAYERTYPE_PLAYER */; args[2] = name; args[3] = 0; args[4] = a[3];
        if (!x86_call(C, a[2], 5, args, &ok)) break;
        if (C->fault || !ok) break;
    }
    qw_free(w, name);
    return DP_OK;
}
static uint32_t dp_Send(qwin *w, uint32_t *a)
{
    /* this, from, to, flags, data, size */
    qdp *d = self(w, a[0]);
    static int logged;
    dpplayer *to;
    if (w->verbose && !logged) { logged = 1; qw_log(w, "DirectPlay: Send from=%x to=%x flags=%x size=%u (logged once)", a[1], a[2], a[3], a[5]); }
    if (!d || !a[4] || !a[5]) return DPERR_INVALIDPARAMS;
    if (a[5] > DP_MAXMSG) return DPERR_SENDTOOBIG;
    if (!a[2]) return DPERR_UNSUPPORTED;         /* DPID_ALLPLAYERS: GTA sends to each one itself */
    if (d->mode == MODE_CLOSED) return DPERR_INVALIDPLAYER;
    if (d->mode == MODE_MEMBER && (d->lost || !d->c[0])) return DPERR_NOCONNECTION;
    to = player(d, a[2]);
    if (!to) return DPERR_INVALIDPLAYER;
    if (d->mode == MODE_HOST ? !player(d, a[1]) || !player(d, a[1])->local : !is_mine(d, a[1])) return DPERR_INVALIDPLAYER;
    g_f[0] = F_DATA;
    put32(g_f + 1, a[1]);
    put32(g_f + 5, a[2]);
    put32(g_f + 9, a[3]);
    x86_read_mem(C, a[4], g_f + 13, a[5]);
    d->sent++;
    if (d->mode == MODE_HOST && to->conn < 0) { queue_push(d, a[1], a[2], a[3], g_f + 13, a[5]); return DP_OK; }
    {
        dpconn *c = d->mode == MODE_HOST ? d->c[to->conn] : d->c[0];
        if (!c) return DPERR_INVALIDPLAYER;
        conn_put(c, g_f, 13u + a[5]);
        conn_flush(w, c);
    }
    return DP_OK;
}
static uint32_t dp_Receive(qwin *w, uint32_t *a)
{
    /* this, *from, *to, flags, buffer, *size */
    qdp *d = self(w, a[0]);
    static int logged;
    dpmsg *m;
    uint32_t room;
    if (w->verbose && !logged) { logged = 1; qw_log(w, "DirectPlay: Receive flags=%x (logged once)", a[3]); }
    if (!d || !a[5]) return DPERR_INVALIDPARAMS;
    if (a[3] & ~1u) return DPERR_INVALIDFLAGS;   /* only DPRECEIVE_ALL */
    if (!d->qh && d->mode != MODE_CLOSED) obj_pump(w, d);
    m = d->qh;
    if (!m) return DPERR_NOMESSAGES;
    room = RD32(a[5]);
    WR32(a[5], m->len);
    if (!a[4] || room < m->len) return DPERR_BUFFERTOOSMALL;
    if (a[1]) WR32(a[1], m->from);
    if (a[2]) WR32(a[2], m->to);
    x86_write_mem(C, a[4], m->data, m->len);
    d->qh = m->next;
    if (!d->qh) d->qt = NULL;
    d->qn--;
    free(m);
    return DP_OK;
}

/* ------------------------------------------------------------------ creation */
static const char *const g_names2a[32] = {
    "IDirectPlay2A::QueryInterface", "IDirectPlay2A::AddRef", "IDirectPlay2A::Release", "IDirectPlay2A::AddPlayerToGroup", "IDirectPlay2A::Close",
    "IDirectPlay2A::CreateGroup", "IDirectPlay2A::CreatePlayer", "IDirectPlay2A::DeletePlayerFromGroup", "IDirectPlay2A::DestroyGroup",
    "IDirectPlay2A::DestroyPlayer", "IDirectPlay2A::EnumGroupPlayers", "IDirectPlay2A::EnumGroups", "IDirectPlay2A::EnumPlayers",
    "IDirectPlay2A::EnumSessions", "IDirectPlay2A::GetCaps", "IDirectPlay2A::GetGroupData", "IDirectPlay2A::GetGroupName",
    "IDirectPlay2A::GetMessageCount", "IDirectPlay2A::GetPlayerAddress", "IDirectPlay2A::GetPlayerCaps", "IDirectPlay2A::GetPlayerData",
    "IDirectPlay2A::GetPlayerName", "IDirectPlay2A::GetSessionDesc", "IDirectPlay2A::Initialize", "IDirectPlay2A::Open", "IDirectPlay2A::Receive",
    "IDirectPlay2A::Send", "IDirectPlay2A::SetGroupData", "IDirectPlay2A::SetGroupName", "IDirectPlay2A::SetPlayerData", "IDirectPlay2A::SetPlayerName",
    "IDirectPlay2A::SetSessionDesc"
};
static const char *const g_names1[18] = {
    "IDirectPlay::QueryInterface", "IDirectPlay::AddRef", "IDirectPlay::Release", "IDirectPlay::AddPlayerToGroup", "IDirectPlay::Close",
    "IDirectPlay::CreatePlayer", "IDirectPlay::CreateGroup", "IDirectPlay::DeletePlayerFromGroup", "IDirectPlay::DestroyPlayer",
    "IDirectPlay::DestroyGroup", "IDirectPlay::EnableNewPlayers", "IDirectPlay::EnumGroupPlayers", "IDirectPlay::EnumGroups", "IDirectPlay::EnumPlayers",
    "IDirectPlay::EnumSessions", "IDirectPlay::GetCaps", "IDirectPlay::GetMessageCount", "IDirectPlay::Open"
};
static void vtables(qwin *w)
{
    int i;
    if (g_vt2a) return;
    g_vt1 = qw_alloc(w, 18u * 4u);
    g_vt2a = qw_alloc(w, 32u * 4u);
    for (i = 0; i < 18; i++) WR32(g_vt1 + (uint32_t)i * 4u, qw_thunk(w, "dplayx", g_names1[i]));
    for (i = 0; i < 32; i++) WR32(g_vt2a + (uint32_t)i * 4u, qw_thunk(w, "dplayx", g_names2a[i]));
}
static uint32_t x_DirectPlayCreate(qwin *w, uint32_t *a)
{
    uint8_t guid[16];
    int i;
    if (a[1]) WR32(a[1], 0);
    if (!a[0] || !a[1]) return DPERR_INVALIDPARAMS;
    x86_read_mem(C, a[0], guid, 16);
    if (memcmp(guid, GUID_TCPIP, 16) || !C->door) return DPERR_UNAVAILABLE;
    vtables(w);
    for (i = 0; i < DP_MAXOBJ; i++) if (!g_dp[i].used) break;
    if (i >= DP_MAXOBJ) return DPERR_UNAVAILABLE;
    memset(&g_dp[i], 0, sizeof(g_dp[i]));
    g_dp[i].used = 1;
    g_dp[i].refs = 1;
    g_dp[i].face1 = qw_alloc(w, 16);
    WR32(g_dp[i].face1, g_vt1);
    WR32(g_dp[i].face1 + 4, (uint32_t)i);
    WR32(a[1], g_dp[i].face1);
    if (w->verbose) qw_log(w, "DirectPlay: DirectPlayCreate");
    return DP_OK;
}
static uint32_t x_DirectPlayEnumerateA(qwin *w, uint32_t *a)
{
    static const char name[] = "Internet TCP/IP Connection For DirectPlay";
    uint32_t mem, args[5];
    int ok;
    if (w->verbose) qw_log(w, "DirectPlay: DirectPlayEnumerateA");
    if (!a[0]) return DPERR_INVALIDPARAMS;
    if (!C->door) return DP_OK;                /* no network without the emulator's door */
    mem = qw_alloc(w, 16u + sizeof(name));
    if (!mem) return DPERR_UNAVAILABLE;
    x86_write_mem(C, mem, GUID_TCPIP, 16);
    x86_write_mem(C, mem + 16, name, sizeof(name));
    args[0] = mem; args[1] = mem + 16; args[2] = 6; args[3] = 0; args[4] = a[1];
    x86_call(C, a[0], 5, args, &ok);
    qw_free(w, mem);
    return DP_OK;
}
static uint32_t x_unavailable(qwin *w, uint32_t *a)
{
    (void)w; (void)a;
    return DPERR_UNAVAILABLE;
}

#define M(name, fn, args) { "dplayx", name, args, fn }
const qhle qw_dplay[] = {
    M("DirectPlayCreate", x_DirectPlayCreate, 3), M("#1", x_DirectPlayCreate, 3),
    M("DirectPlayEnumerateA", x_DirectPlayEnumerateA, 2), M("#2", x_DirectPlayEnumerateA, 2),
    M("DirectPlayEnumerate", x_DirectPlayEnumerateA, 2), M("#9", x_DirectPlayEnumerateA, 2),
    M("DirectPlayLobbyCreateA", x_unavailable, 5), M("#4", x_unavailable, 5),
    { "dplay", "DirectPlayCreate", 3, x_DirectPlayCreate }, { "dplay", "DirectPlayEnumerateA", 2, x_DirectPlayEnumerateA },
    { "dplay", "DirectPlayEnumerate", 2, x_DirectPlayEnumerateA },
    M("IDirectPlay::QueryInterface", dp_QueryInterface, 3), M("IDirectPlay::AddRef", dp_AddRef, 1), M("IDirectPlay::Release", dp_Release, 1),
    M("IDirectPlay2A::QueryInterface", dp_QueryInterface, 3), M("IDirectPlay2A::AddRef", dp_AddRef, 1), M("IDirectPlay2A::Release", dp_Release, 1),
    M("IDirectPlay2A::Close", dp_Close, 1), M("IDirectPlay2A::CreatePlayer", dp_CreatePlayer, 7), M("IDirectPlay2A::DestroyPlayer", dp_DestroyPlayer, 2),
    M("IDirectPlay2A::EnumPlayers", dp_EnumPlayers, 5), M("IDirectPlay2A::EnumSessions", dp_EnumSessions, 6), M("IDirectPlay2A::Open", dp_Open, 3),
    M("IDirectPlay2A::Receive", dp_Receive, 6), M("IDirectPlay2A::Send", dp_Send, 6),
    { NULL, NULL, 0, NULL }
};
