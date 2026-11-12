/*
 * QRXIPFSD.c - QRX IPFS node core for 68K AROS.
 *
 * This is the local node core: blockstore, CIDv1/UnixFS directories,
 * Kubo-compatible HTTP API and a /ipfs/ gateway.  Public-network
 * transport is provided by the separate QRXIPFSP2P process; missing
 * blocks are requested through its loopback control endpoint and then
 * served from the shared local store.
 *
 * API:      http://127.0.0.1:5001/api/v0/...
 * Gateway:  http://127.0.0.1:5001/ipfs/<cid>[/path]
 * Storage:  IPFS:.qrx-node/blocks, pins, node-id
 */
#include "qrx_crypto.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/datetime.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "qrx_net.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define NODE_DIR        "SYS:Storage/IPFS/.qrx-node"
#define NODE_BLOCKS     NODE_DIR "/blocks"
#define NODE_PINS       NODE_DIR "/pins"
#define NODE_ID         NODE_DIR "/node-id"
#define NODE_CONFIG     "SYS:QRX/Prefs/ipfs.cfg"
#define NODE_MAX_PATH   220
#define NODE_MAX_CID    96
#define NODE_MAX_PARTS  512
#define NODE_MAX_UPLOAD (16UL * 1024UL * 1024UL)
#define NODE_HTTP_MAX   16384
#define NODE_PORT       5001
#define P2P_CONTROL     5002

struct cid {
    unsigned char b[40];
    int len;
    char s[NODE_MAX_CID];
};

struct node_file {
    char path[NODE_MAX_PATH];
    struct cid cid;
    unsigned long size;
};

struct node_dir {
    char path[NODE_MAX_PATH];
    struct cid cid;
    unsigned long size;
};

struct node_link {
    char name[NODE_MAX_PATH];
    struct cid cid;
    unsigned long size;
};

struct pb_link {
    char name[256];
    struct cid cid;
    unsigned long size;
};

struct http_req {
    int s;
    char method[8];
    char target[1024];
    char path[1024];
    char query[512];
    char ctype[256];
    unsigned long content_len;
    unsigned char *body;
};

static char g_err[240] = "";
static char g_node_id[NODE_MAX_CID] = "QRX-LOCAL";
static int g_port = NODE_PORT;
static int g_autofetch = 1;

/* ------------------------------------------------------------------ */
/* basic helpers                                                       */
/* ------------------------------------------------------------------ */

static void set_err(const char *msg)
{
    if (!msg) msg = "";
    strncpy(g_err, msg, sizeof(g_err) - 1);
    g_err[sizeof(g_err) - 1] = 0;
}

static const unsigned char *mem_find(const unsigned char *hay, unsigned long hlen,
                                     const void *needle, unsigned long nlen)
{
    unsigned long i;
    const unsigned char *n = (const unsigned char *)needle;
    if (!hlen || !nlen || nlen > hlen) return NULL;
    for (i = 0; i + nlen <= hlen; ++i)
        if (!memcmp(hay + i, n, nlen)) return hay + i;
    return NULL;
}

static int ci_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        char a = *s++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static int put_varint(unsigned char *out, unsigned long v)
{
    int n = 0;
    while (v >= 0x80UL) {
        out[n++] = (unsigned char)((v & 0x7fUL) | 0x80UL);
        v >>= 7;
    }
    out[n++] = (unsigned char)v;
    return n;
}

static int read_varint(const unsigned char *buf, unsigned long len,
                       unsigned long *pos, unsigned long *out)
{
    unsigned long v = 0;
    int shift = 0;
    while (*pos < len && shift < 35) {
        unsigned char b = buf[(*pos)++];
        v |= (unsigned long)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

static void hex_encode(const unsigned char *in, int len, char *out, int cap)
{
    static const char *hex = "0123456789abcdef";
    int i, n = 0;
    for (i = 0; i < len && n + 2 < cap; ++i) {
        out[n++] = hex[(in[i] >> 4) & 15];
        out[n++] = hex[in[i] & 15];
    }
    out[n] = 0;
}

static void base32_encode(const unsigned char *in, int len, char *out, int cap)
{
    static const char *alpha = "abcdefghijklmnopqrstuvwxyz234567";
    unsigned long acc = 0;
    int bits = 0, i, n = 0;

    for (i = 0; i < len; ++i) {
        acc = (acc << 8) | in[i];
        bits += 8;
        while (bits >= 5 && n + 1 < cap) {
            out[n++] = alpha[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits && n + 1 < cap)
        out[n++] = alpha[(acc << (5 - bits)) & 31];
    out[n] = 0;
}

static int b58_char(int c)
{
    return (c >= '1' && c <= '9') || (c >= 'A' && c <= 'H') ||
           (c >= 'J' && c <= 'N') || (c >= 'P' && c <= 'Z') ||
           (c >= 'a' && c <= 'k') || (c >= 'm' && c <= 'z');
}

static int b58_encode(const unsigned char *in, int len, char *out, int cap)
{
    static const char alpha[] =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    unsigned char d[128];
    int nd = 0, i, j, zeros = 0, n = 0;
    if (!out || cap < 2 || len < 0 || len > (int)sizeof(d)) return 0;
    while (zeros < len && in[zeros] == 0) ++zeros;
    for (i = zeros; i < len; ++i) {
        int carry = in[i];
        for (j = 0; j < nd; ++j) {
            carry += d[j] * 256;
            d[j] = (unsigned char)(carry % 58);
            carry /= 58;
        }
        while (carry) {
            if (nd >= (int)sizeof(d)) return 0;
            d[nd++] = (unsigned char)(carry % 58);
            carry /= 58;
        }
    }
    for (i = 0; i < zeros; ++i) {
        if (n + 1 >= cap) return 0;
        out[n++] = '1';
    }
    for (i = nd - 1; i >= 0; --i) {
        if (n + 1 >= cap) return 0;
        out[n++] = alpha[d[i]];
    }
    out[n] = 0;
    return n;
}

static int cid_from_digest(const unsigned char digest[32], unsigned char codec,
                           struct cid *c)
{
    if (!c) return 0;
    c->b[0] = 1;             /* CIDv1 */
    c->b[1] = codec;
    c->b[2] = 0x12;          /* sha2-256 */
    c->b[3] = 0x20;
    memcpy(c->b + 4, digest, 32);
    c->len = 36;
    c->s[0] = 'b';
    base32_encode(c->b, c->len, c->s + 1, (int)sizeof(c->s) - 1);
    return 1;
}

static int cid_from_bytes(const unsigned char *buf, int len, struct cid *c)
{
    if (!c || len <= 0 || len >= (int)sizeof(c->b)) return 0;
    memcpy(c->b, buf, len);
    c->len = len;
    c->s[0] = 0;
    if (len == 34 && buf[0] == 0x12 && buf[1] == 0x20) {
        if (!b58_encode(buf, len, c->s, (int)sizeof(c->s))) return 0;
    } else {
        c->s[0] = 'b';
        base32_encode(c->b, c->len, c->s + 1, (int)sizeof(c->s) - 1);
    }
    return 1;
}

static int safe_cid(const char *cid)
{
    const char *p;
    int n;
    if (!cid) return 0;
    n = (int)strlen(cid);
    if (n < 2 || n >= NODE_MAX_CID) return 0;
    if (cid[0] == 'b') {
        for (p = cid + 1; *p; ++p) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '2' && *p <= '7')))
                return 0;
        }
        return 1;
    }
    if (cid[0] == 'Q' && cid[1] == 'm' && n == 46) {
        for (p = cid; *p; ++p)
            if (!b58_char((unsigned char)*p)) return 0;
        return 1;
    }
    return 0;
}

static void parent_path(const char *path, char *out, int cap)
{
    const char *slash = strrchr(path, '/');
    int n;
    out[0] = 0;
    if (!slash) return;
    n = (int)(slash - path);
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int clean_path(const char *src, char *dst, int cap)
{
    char tmp[NODE_MAX_PATH * 2];
    char *p;
    int n = 0;

    if (!src || !*src) return 0;
    strncpy(tmp, src, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    for (p = tmp; *p; ++p) {
        if ((unsigned char)*p < 32) return 0;
        if (*p == '\\') *p = '/';
        if (*p == ':') return 0;
    }
    p = tmp;
    while (*p) {
        char *start, *end;
        int len;
        while (*p == '/') ++p;
        if (!*p) break;
        start = p;
        end = strchr(p, '/');
        len = end ? (int)(end - p) : (int)strlen(p);
        p = end ? end + 1 : p + len;
        if (!len || (len == 1 && start[0] == '.')) continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') return 0;
        if (n && n + 1 < cap) dst[n++] = '/';
        if (n + len >= cap) return 0;
        memcpy(dst + n, start, len);
        n += len;
    }
    dst[n] = 0;
    return n > 0;
}

static int ensure_dir(const char *path)
{
    BPTR lock = CreateDir((CONST_STRPTR)path);
    if (lock) UnLock(lock);
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (!lock) return 0;
    UnLock(lock);
    return 1;
}

static void block_path(const char *cid, char *out, int cap)
{
    snprintf(out, cap, "%s/%s", NODE_BLOCKS, cid);
}

static int block_exists(const char *cid)
{
    char path[420];
    BPTR lock;
    block_path(cid, path, sizeof(path));
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock) UnLock(lock);
    return lock != 0;
}

static int block_write(const char *cid, const unsigned char *data, unsigned long len)
{
    char path[420], tmp[440];
    BPTR f;
    int ok = 1;

    if (block_exists(cid)) return 1;
    block_path(cid, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s/.tmp", NODE_BLOCKS);
    f = Open((CONST_STRPTR)tmp, MODE_NEWFILE);
    if (!f) return 0;
    if (len && Write(f, (APTR)data, (LONG)len) != (LONG)len) ok = 0;
    Close(f);
    if (!ok) {
        DeleteFile((CONST_STRPTR)tmp);
        return 0;
    }
    if (!Rename((CONST_STRPTR)tmp, (CONST_STRPTR)path)) {
        DeleteFile((CONST_STRPTR)tmp);
        return 0;
    }
    return 1;
}

static int block_read_local(const char *cid, unsigned char **out, unsigned long *outlen)
{
    char path[420];
    BPTR f;
    LONG size;
    unsigned char *buf;

    if (!safe_cid(cid)) return 0;
    block_path(cid, path, sizeof(path));
    f = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!f) return 0;
    Seek(f, 0, OFFSET_END);
    size = Seek(f, 0, OFFSET_CURRENT);
    if (size < 0 || (unsigned long)size > NODE_MAX_UPLOAD) {
        Close(f);
        return 0;
    }
    Seek(f, 0, OFFSET_BEGINNING);
    buf = (unsigned char *)AllocVec((unsigned long)size ? (unsigned long)size : 1, MEMF_ANY);
    if (!buf) {
        Close(f);
        return 0;
    }
    if (size && Read(f, buf, size) != size) {
        FreeVec(buf);
        Close(f);
        return 0;
    }
    Close(f);
    *out = buf;
    *outlen = (unsigned long)size;
    return 1;
}

static int p2p_fetch_block(const char *cid)
{
    struct sockaddr_in sa;
    char req[256], hdr[512];
    int s, n, i;

    if (!safe_cid(cid)) return 0;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(P2P_CONTROL);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        CloseSocket(s);
        return 0;
    }
    n = snprintf(req, sizeof(req),
                 "GET /fetch?arg=%s HTTP/1.0\r\nHost: 127.0.0.1:%d\r\n"
                 "Connection: close\r\n\r\n", cid, P2P_CONTROL);
    for (i = 0; i < n; ) {
        LONG w = send(s, req + i, n - i, 0);
        if (w <= 0) { CloseSocket(s); return 0; }
        i += (int)w;
    }
    n = 0;
    while (n < (int)sizeof(hdr) - 1) {
        unsigned char ch;
        LONG r = recv(s, &ch, 1, 0);
        if (r != 1) break;
        hdr[n++] = (char)ch;
        if (n >= 4 && !memcmp(hdr + n - 4, "\r\n\r\n", 4)) break;
    }
    hdr[n] = 0;
    CloseSocket(s);
    return !strncmp(hdr, "HTTP/1.0 200", 12) ||
           !strncmp(hdr, "HTTP/1.1 200", 12);
}

static int block_read(const char *cid, unsigned char **out, unsigned long *outlen)
{
    if (block_read_local(cid, out, outlen)) return 1;
    if (!g_autofetch) return 0;
    if (!p2p_fetch_block(cid)) return 0;
    return block_read_local(cid, out, outlen);
}

static int block_size(const char *cid, unsigned long *size)
{
    char path[420];
    BPTR f;
    LONG n;
    if (!safe_cid(cid)) return 0;
    block_path(cid, path, sizeof(path));
    f = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!f) return 0;
    Seek(f, 0, OFFSET_END);
    n = Seek(f, 0, OFFSET_CURRENT);
    Close(f);
    if (n < 0) return 0;
    *size = (unsigned long)n;
    return 1;
}

static int store_bytes(const unsigned char *data, unsigned long len,
                       unsigned char codec, struct cid *cid)
{
    unsigned char digest[32];
    qrx_sha256(data, len, digest);
    if (!cid_from_digest(digest, codec, cid)) return 0;
    return block_write(cid->s, data, len);
}

static int read_text(const char *path, char *out, int cap)
{
    BPTR f = Open((CONST_STRPTR)path, MODE_OLDFILE);
    LONG n;
    if (!f) return 0;
    n = Read(f, out, cap - 1);
    Close(f);
    if (n <= 0) return 0;
    out[n] = 0;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
        out[--n] = 0;
    return n > 0;
}

static void load_node_config(void)
{
    BPTR f = Open((CONST_STRPTR)NODE_CONFIG, MODE_OLDFILE);
    char line[160];
    if (!f) return;
    while (FGets(f, line, sizeof(line)))
    {
        if (!strncmp(line, "autofetch=", 10))
        {
            int v = atoi(line + 10);
            g_autofetch = v != 0;
        }
    }
    Close(f);
}

static int write_text(const char *path, const char *text)
{
    BPTR f = Open((CONST_STRPTR)path, MODE_NEWFILE);
    LONG n = (LONG)strlen(text);
    if (!f) return 0;
    if (Write(f, (APTR)text, n) != n) {
        Close(f);
        return 0;
    }
    Close(f);
    return 1;
}

static int node_init(void)
{
    unsigned char rnd[32];
    char id[NODE_MAX_CID];
    BPTR f;

    if (!ensure_dir("SYS:Storage") ||
        !ensure_dir("SYS:Storage/IPFS") ||
        !ensure_dir(NODE_DIR) ||
        !ensure_dir(NODE_BLOCKS)) {
        set_err("Node-Verzeichnisse nicht anlegbar.");
        return 0;
    }

    if (read_text(NODE_ID, id, sizeof(id))) {
        strncpy(g_node_id, id, sizeof(g_node_id) - 1);
        g_node_id[sizeof(g_node_id) - 1] = 0;
    } else {
        qrx_random_bytes(rnd, sizeof(rnd));
        g_node_id[0] = 'Q'; g_node_id[1] = 'R'; g_node_id[2] = 'X'; g_node_id[3] = '-';
        hex_encode(rnd, sizeof(rnd), g_node_id + 4, (int)sizeof(g_node_id) - 4);
        write_text(NODE_ID, g_node_id);
    }

    f = Open((CONST_STRPTR)NODE_PINS, MODE_OLDFILE);
    if (f) Close(f);
    else {
        f = Open((CONST_STRPTR)NODE_PINS, MODE_NEWFILE);
        if (f) Close(f);
    }
    set_err("");
    return 1;
}

static int pin_has(const char *cid)
{
    BPTR f = Open((CONST_STRPTR)NODE_PINS, MODE_OLDFILE);
    char line[NODE_MAX_CID + 8];
    int active = 0;
    if (!f) return 0;
    while (FGets(f, (STRPTR)line, sizeof(line))) {
        char *p = line;
        while (*p && *p != '\r' && *p != '\n' && *p != ' ') ++p;
        *p = 0;
        if (!strcmp(line, cid))
            active = 1;
        else if (line[0] == '-' && !strcmp(line + 1, cid))
            active = 0;
    }
    Close(f);
    return active;
}

static int pin_add(const char *cid)
{
    BPTR f;
    if (!safe_cid(cid) || pin_has(cid)) return 1;
    f = Open((CONST_STRPTR)NODE_PINS, MODE_READWRITE);
    if (!f) {
        f = Open((CONST_STRPTR)NODE_PINS, MODE_NEWFILE);
        if (!f) return 0;
        Close(f);
        f = Open((CONST_STRPTR)NODE_PINS, MODE_READWRITE);
        if (!f) return 0;
    }
    Seek(f, 0, OFFSET_END);
    FPuts(f, (CONST_STRPTR)cid);
    FPuts(f, (CONST_STRPTR)"\n");
    Close(f);
    return 1;
}

static int pin_remove(const char *cid)
{
    BPTR f;

    if (!safe_cid(cid)) return 0;
    f = Open((CONST_STRPTR)NODE_PINS, MODE_READWRITE);
    if (!f) f = Open((CONST_STRPTR)NODE_PINS, MODE_NEWFILE);
    if (!f) return 0;
    Seek(f, 0, OFFSET_END);
    FPrintf(f, (CONST_STRPTR)"-%s\n", cid);
    Close(f);
    return 1;
}

static void repo_stat(unsigned long *count, unsigned long *bytes)
{
    BPTR lock = Lock((CONST_STRPTR)NODE_BLOCKS, ACCESS_READ);
    struct FileInfoBlock *fib;
    *count = 0;
    *bytes = 0;
    if (!lock) return;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        UnLock(lock);
        return;
    }
    if (Examine(lock, fib)) {
        while (ExNext(lock, fib)) {
            if (fib->fib_FileName[0] == '.') continue;
            if (fib->fib_DirEntryType > 0) continue;
            ++*count;
            *bytes += (unsigned long)fib->fib_Size;
        }
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
}

/* ------------------------------------------------------------------ */
/* multipart add and UnixFS directories                                */
/* ------------------------------------------------------------------ */

static int boundary_from_type(const char *ctype, char *out, int cap)
{
    const char *p;
    int n = 0;
    if (!ctype) return 0;
    p = strstr(ctype, "boundary=");
    if (!p) return 0;
    p += 9;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    } else {
        while (*p && *p != ';' && *p != ' ' && n + 1 < cap) out[n++] = *p++;
    }
    out[n] = 0;
    return n > 0;
}

static int filename_from_header(const unsigned char *hdr, unsigned long len,
                                char *out, int cap)
{
    const unsigned char *p = mem_find(hdr, len, "filename=", 9);
    int n = 0;
    char end = ';';

    if (!p) return 0;
    p += 9;
    if (p >= hdr + len) return 0;
    if (*p == '"') {
        ++p;
        end = '"';
    }
    while (p < hdr + len && *p != end && *p != '\r' && *p != '\n' && n + 1 < cap) {
        if (*p == '\\' && p + 1 < hdr + len) ++p;
        out[n++] = (char)*p++;
    }
    out[n] = 0;
    return n > 0;
}

static int cmp_links(const void *a, const void *b)
{
    return strcmp(((const struct node_link *)a)->name,
                  ((const struct node_link *)b)->name);
}

static int dir_depth(const char *path)
{
    int n = 0;
    while (*path) if (*path++ == '/') ++n;
    return n;
}

static int cmp_dirs(const void *a, const void *b)
{
    const struct node_dir *da = (const struct node_dir *)a;
    const struct node_dir *db = (const struct node_dir *)b;
    int da_depth = dir_depth(da->path);
    int db_depth = dir_depth(db->path);
    if (da_depth != db_depth) return db_depth - da_depth;
    return strcmp(da->path, db->path);
}

static int add_dir_path(struct node_dir *dirs, int *ndirs, const char *path)
{
    int i;
    if (!path || !*path) return 1;
    for (i = 0; i < *ndirs; ++i)
        if (!strcmp(dirs[i].path, path)) return 1;
    if (*ndirs >= NODE_MAX_PARTS) return 0;
    strncpy(dirs[*ndirs].path, path, sizeof(dirs[*ndirs].path) - 1);
    dirs[*ndirs].path[sizeof(dirs[*ndirs].path) - 1] = 0;
    dirs[*ndirs].cid.len = 0;
    dirs[*ndirs].size = 0;
    ++*ndirs;
    return 1;
}

static int add_parent_dirs(struct node_dir *dirs, int *ndirs, const char *file)
{
    char path[NODE_MAX_PATH];
    strncpy(path, file, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    for (;;) {
        char *slash = strrchr(path, '/');
        if (!slash) break;
        *slash = 0;
        if (!*path) break;
        if (!add_dir_path(dirs, ndirs, path)) return 0;
    }
    return 1;
}

static int add_link(struct node_link *links, int *n, const char *name,
                    const struct cid *cid, unsigned long size)
{
    if (*n >= NODE_MAX_PARTS) return 0;
    strncpy(links[*n].name, name, sizeof(links[*n].name) - 1);
    links[*n].name[sizeof(links[*n].name) - 1] = 0;
    links[*n].cid = *cid;
    links[*n].size = size;
    ++*n;
    return 1;
}

static int build_dir_block(struct node_link *links, int nlinks,
                           unsigned char **out, unsigned long *outlen)
{
    unsigned long cap = 65536UL + (unsigned long)nlinks * 512UL;
    unsigned char *buf = (unsigned char *)AllocVec(cap, MEMF_ANY);
    unsigned char linkbuf[512];
    unsigned long pos = 0;
    int i;

    if (!buf) return 0;
    buf[pos++] = 0x0a;          /* PBNode.Data */
    buf[pos++] = 0x02;
    buf[pos++] = 0x08;          /* UnixFS Type = Directory */
    buf[pos++] = 0x01;

    for (i = 0; i < nlinks; ++i) {
        int lp = 0;
        int nlen = (int)strlen(links[i].name);
        int clen = links[i].cid.len;

        linkbuf[lp++] = 0x0a;   /* PBLink.Hash */
        lp += put_varint(linkbuf + lp, (unsigned long)clen);
        memcpy(linkbuf + lp, links[i].cid.b, clen); lp += clen;
        linkbuf[lp++] = 0x12;   /* PBLink.Name */
        lp += put_varint(linkbuf + lp, (unsigned long)nlen);
        memcpy(linkbuf + lp, links[i].name, nlen); lp += nlen;
        linkbuf[lp++] = 0x18;   /* PBLink.Tsize */
        lp += put_varint(linkbuf + lp, links[i].size);

        /* 0x12 tag + up to two varint bytes + link payload. */
        if (pos + 3 + (unsigned long)lp > cap) {
            FreeVec(buf);
            return 0;
        }
        buf[pos++] = 0x12;      /* PBNode.Links */
        pos += (unsigned long)put_varint(buf + pos, (unsigned long)lp);
        memcpy(buf + pos, linkbuf, lp);
        pos += (unsigned long)lp;
    }
    *out = buf;
    *outlen = pos;
    return 1;
}

static int build_one_dir(struct node_file *files, int nfiles,
                         struct node_dir *dirs, int ndirs, struct node_dir *dir)
{
    struct node_link *links;
    unsigned char *block = NULL;
    unsigned long blocklen = 0;
    unsigned long total = 0;
    unsigned char digest[32];
    int nlinks = 0, i;

    links = (struct node_link *)AllocVec(sizeof(struct node_link) * NODE_MAX_PARTS,
                                         MEMF_ANY);
    if (!links) return 0;

    for (i = 0; i < nfiles; ++i) {
        char parent[NODE_MAX_PATH];
        parent_path(files[i].path, parent, sizeof(parent));
        if (!strcmp(parent, dir->path) &&
            !add_link(links, &nlinks, base_name(files[i].path),
                      &files[i].cid, files[i].size)) {
            FreeVec(links);
            return 0;
        }
    }
    for (i = 0; i < ndirs; ++i) {
        char parent[NODE_MAX_PATH];
        if (&dirs[i] == dir) continue;
        parent_path(dirs[i].path, parent, sizeof(parent));
        if (!strcmp(parent, dir->path) &&
            !add_link(links, &nlinks, base_name(dirs[i].path),
                      &dirs[i].cid, dirs[i].size)) {
            FreeVec(links);
            return 0;
        }
    }

    qsort(links, (size_t)nlinks, sizeof(struct node_link), cmp_links);
    if (!build_dir_block(links, nlinks, &block, &blocklen)) {
        FreeVec(links);
        return 0;
    }
    qrx_sha256(block, blocklen, digest);
    cid_from_digest(digest, 0x70, &dir->cid);
    if (!block_write(dir->cid.s, block, blocklen)) {
        FreeVec(block);
        FreeVec(links);
        return 0;
    }

    total = blocklen;
    for (i = 0; i < nlinks; ++i) total += links[i].size;
    dir->size = total;
    FreeVec(block);
    FreeVec(links);
    return 1;
}

static int common_root(struct node_file *files, int nfiles, char *root, int cap)
{
    int any = 0, no_slash = 0, i;
    root[0] = 0;
    for (i = 0; i < nfiles; ++i) {
        const char *slash = strchr(files[i].path, '/');
        if (!slash) {
            no_slash = 1;
            continue;
        }
        if (!any) {
            int n = (int)(slash - files[i].path);
            if (n >= cap) n = cap - 1;
            memcpy(root, files[i].path, n);
            root[n] = 0;
            any = 1;
        } else {
            int n = (int)(slash - files[i].path);
            if ((int)strlen(root) != n || strncmp(root, files[i].path, n))
                return 0;
        }
    }
    return any && !no_slash;
}

static int node_add(const unsigned char *body, unsigned long len,
                    const char *ctype, int pin,
                    char *cid_out, unsigned long cid_cap,
                    char *name_out, unsigned long name_cap,
                    unsigned long *size_out)
{
    char boundary[128];
    char marker[160], delim[180];
    const unsigned char *p, *end;
    struct node_file *files;
    struct node_dir *dirs;
    int nfiles = 0, ndirs = 0, i;
    int bmark_len, dmark_len;
    char root[NODE_MAX_PATH];
    struct cid root_cid;
    unsigned long root_size = 0;
    char root_name[NODE_MAX_PATH];

    if (!body || !len) {
        set_err("Leerer Upload.");
        return 0;
    }
    if (!boundary_from_type(ctype, boundary, sizeof(boundary))) {
        set_err("Multipart-Boundary fehlt.");
        return 0;
    }
    files = (struct node_file *)AllocVec(sizeof(struct node_file) * NODE_MAX_PARTS, MEMF_ANY);
    dirs = (struct node_dir *)AllocVec(sizeof(struct node_dir) * NODE_MAX_PARTS, MEMF_ANY);
    if (!files || !dirs) {
        if (files) FreeVec(files);
        if (dirs) FreeVec(dirs);
        set_err("Kein Speicher fuer Upload.");
        return 0;
    }

    bmark_len = snprintf(marker, sizeof(marker), "--%s", boundary);
    dmark_len = snprintf(delim, sizeof(delim), "\r\n--%s", boundary);
    p = mem_find(body, len, marker, (unsigned long)bmark_len);
    if (!p) {
        set_err("Multipart-Anfang fehlt.");
        goto fail;
    }
    p += bmark_len;
    end = body + len;
    if (p + 2 > end || p[0] != '\r' || p[1] != '\n') {
        set_err("Multipart-Anfang beschaedigt.");
        goto fail;
    }
    p += 2;

    for (;;) {
        const unsigned char *hdr_end, *content, *next;
        unsigned long hdr_len, content_len;
        char fname[256], clean[NODE_MAX_PATH];
        struct cid cid;

        if (p >= end) break;
        hdr_end = mem_find(p, (unsigned long)(end - p), "\r\n\r\n", 4);
        if (!hdr_end) {
            set_err("Multipart-Header unvollstaendig.");
            goto fail;
        }
        hdr_len = (unsigned long)(hdr_end - p);
        if (!filename_from_header(p, hdr_len, fname, sizeof(fname)))
            strcpy(fname, "file");
        if (!clean_path(fname, clean, sizeof(clean))) {
            set_err("Ungueltiger Dateiname im Upload.");
            goto fail;
        }
        content = hdr_end + 4;
        next = mem_find(content, (unsigned long)(end - content), delim,
                        (unsigned long)dmark_len);
        if (!next) {
            set_err("Multipart-Ende fehlt.");
            goto fail;
        }
        content_len = (unsigned long)(next - content);
        if (nfiles >= NODE_MAX_PARTS) {
            set_err("Zu viele Dateien im Upload.");
            goto fail;
        }
        for (i = 0; i < nfiles; ++i) {
            if (!strcmp(files[i].path, clean)) {
                set_err("Doppelter Dateiname im Upload.");
                goto fail;
            }
        }
        if (!store_bytes(content, content_len, 0x55, &cid)) {
            set_err("Block konnte nicht gespeichert werden.");
            goto fail;
        }
        strncpy(files[nfiles].path, clean, sizeof(files[nfiles].path) - 1);
        files[nfiles].path[sizeof(files[nfiles].path) - 1] = 0;
        files[nfiles].cid = cid;
        files[nfiles].size = content_len;
        ++nfiles;

        p = next + dmark_len;
        if (p + 2 <= end && p[0] == '-' && p[1] == '-') break;
        if (p + 2 > end || p[0] != '\r' || p[1] != '\n') {
            set_err("Multipart-Trenner beschaedigt.");
            goto fail;
        }
        p += 2;
    }

    if (!nfiles) {
        set_err("Upload enthaelt keine Datei.");
        goto fail;
    }

    if (common_root(files, nfiles, root, sizeof(root))) {
        for (i = 0; i < nfiles; ++i)
            if (!add_parent_dirs(dirs, &ndirs, files[i].path)) {
                set_err("Zu viele Verzeichnisse.");
                goto fail;
            }
        qsort(dirs, (size_t)ndirs, sizeof(struct node_dir), cmp_dirs);
        for (i = 0; i < ndirs; ++i) {
            if (!build_one_dir(files, nfiles, dirs, ndirs, &dirs[i])) {
                set_err("UnixFS-Verzeichnis konnte nicht gebaut werden.");
                goto fail;
            }
        }
        root_cid.len = 0;
        for (i = 0; i < ndirs; ++i) {
            if (!strcmp(dirs[i].path, root)) {
                root_cid = dirs[i].cid;
                root_size = dirs[i].size;
                break;
            }
        }
        if (!root_cid.len) {
            set_err("Wurzelverzeichnis fehlt.");
            goto fail;
        }
        strncpy(root_name, root, sizeof(root_name) - 1);
        root_name[sizeof(root_name) - 1] = 0;
    } else {
        if (nfiles != 1) {
            set_err("Mehrere Dateien ohne gemeinsamen Drawer.");
            goto fail;
        }
        root_cid = files[0].cid;
        root_size = files[0].size;
        strncpy(root_name, files[0].path, sizeof(root_name) - 1);
        root_name[sizeof(root_name) - 1] = 0;
    }

    if (pin && !pin_add(root_cid.s)) {
        set_err("Root-CID konnte nicht gepinnt werden.");
        goto fail;
    }
    strncpy(cid_out, root_cid.s, cid_cap - 1);
    cid_out[cid_cap - 1] = 0;
    strncpy(name_out, root_name, name_cap - 1);
    name_out[name_cap - 1] = 0;
    *size_out = root_size;
    FreeVec(files);
    FreeVec(dirs);
    set_err("");
    return 1;

fail:
    FreeVec(files);
    FreeVec(dirs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* DAG-PB / gateway                                                    */
/* ------------------------------------------------------------------ */

static int pb_parse_link(const unsigned char *buf, unsigned long len,
                         struct pb_link *link)
{
    unsigned long pos = 0;
    link->name[0] = 0;
    link->cid.len = 0;
    link->size = 0;
    while (pos < len) {
        unsigned long tag, field, wire, v, sublen;
        if (!read_varint(buf, len, &pos, &tag)) return 0;
        field = tag >> 3;
        wire = tag & 7;
        if (wire == 0) {
            if (!read_varint(buf, len, &pos, &v)) return 0;
            if (field == 3) link->size = v;
        } else if (wire == 2) {
            if (!read_varint(buf, len, &pos, &sublen) || pos + sublen > len) return 0;
            if (field == 1 && sublen < sizeof(link->cid.b))
                cid_from_bytes(buf + pos, (int)sublen, &link->cid);
            else if (field == 2) {
                unsigned long n = sublen;
                if (n >= sizeof(link->name)) n = sizeof(link->name) - 1;
                memcpy(link->name, buf + pos, n);
                link->name[n] = 0;
            }
            pos += sublen;
        } else {
            return 0;
        }
    }
    return link->cid.len > 0;
}

static int pb_parse(const unsigned char *buf, unsigned long len,
                    struct pb_link *links, int max, int *is_dir, int *nlinks)
{
    unsigned long pos = 0;
    *is_dir = 0;
    *nlinks = 0;
    while (pos < len) {
        unsigned long tag, field, wire, v, sublen;
        if (!read_varint(buf, len, &pos, &tag)) return 0;
        field = tag >> 3;
        wire = tag & 7;
        if (wire == 0) {
            if (!read_varint(buf, len, &pos, &v)) return 0;
        } else if (wire == 2) {
            if (!read_varint(buf, len, &pos, &sublen) || pos + sublen > len) return 0;
            if (field == 1) {
                if (sublen >= 2 && buf[pos] == 0x08 && buf[pos + 1] == 0x01)
                    *is_dir = 1;
            } else if (field == 2 && *nlinks < max) {
                if (pb_parse_link(buf + pos, sublen, &links[*nlinks]))
                    ++*nlinks;
            }
            pos += sublen;
        } else {
            return 0;
        }
    }
    return 1;
}

struct file_out {
    unsigned char *p;
    unsigned long len, cap;
};

static int file_append(struct file_out *o, const unsigned char *data,
                       unsigned long len)
{
    unsigned long cap;
    unsigned char *p;
    if (!len) return 1;
    if (o->len + len > NODE_MAX_UPLOAD) return 0;
    if (o->len + len > o->cap) {
        cap = o->cap ? o->cap : 4096UL;
        while (cap < o->len + len) {
            if (cap > NODE_MAX_UPLOAD / 2) { cap = NODE_MAX_UPLOAD; break; }
            cap <<= 1;
        }
        if (cap < o->len + len) return 0;
        p = (unsigned char *)AllocVec(cap, MEMF_ANY);
        if (!p) return 0;
        if (o->p) {
            memcpy(p, o->p, o->len);
            FreeVec(o->p);
        }
        o->p = p;
        o->cap = cap;
    }
    memcpy(o->p + o->len, data, len);
    o->len += len;
    return 1;
}

static int pb_get_data(const unsigned char *buf, unsigned long len,
                       const unsigned char **data, unsigned long *dlen)
{
    unsigned long pos = 0;
    *data = NULL;
    *dlen = 0;
    while (pos < len) {
        unsigned long tag, field, wire, v, sublen;
        if (!read_varint(buf, len, &pos, &tag)) return 0;
        field = tag >> 3;
        wire = tag & 7;
        if (wire == 0) {
            if (!read_varint(buf, len, &pos, &v)) return 0;
        } else if (wire == 2) {
            if (!read_varint(buf, len, &pos, &sublen) || pos + sublen > len)
                return 0;
            if (field == 1) {
                *data = buf + pos;
                *dlen = sublen;
            }
            pos += sublen;
        } else if (wire == 1) {
            if (pos + 8 > len) return 0;
            pos += 8;
        } else if (wire == 5) {
            if (pos + 4 > len) return 0;
            pos += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

static int unixfs_info(const unsigned char *buf, unsigned long len,
                       int *type, const unsigned char **payload,
                       unsigned long *plen)
{
    unsigned long pos = 0;
    *type = 0;
    *payload = NULL;
    *plen = 0;
    if (!buf) return 1;
    while (pos < len) {
        unsigned long tag, field, wire, v, sublen;
        if (!read_varint(buf, len, &pos, &tag)) return 0;
        field = tag >> 3;
        wire = tag & 7;
        if (wire == 0) {
            if (!read_varint(buf, len, &pos, &v)) return 0;
            if (field == 1) *type = (int)v;
        } else if (wire == 2) {
            if (!read_varint(buf, len, &pos, &sublen) || pos + sublen > len)
                return 0;
            if (field == 2) {
                *payload = buf + pos;
                *plen = sublen;
            }
            pos += sublen;
        } else if (wire == 1) {
            if (pos + 8 > len) return 0;
            pos += 8;
        } else if (wire == 5) {
            if (pos + 4 > len) return 0;
            pos += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

static int read_unixfs_collect(const char *cid, struct file_out *out,
                               int depth, int *is_file)
{
    unsigned char *block = NULL;
    unsigned long len = 0, ulen = 0, plen = 0;
    const unsigned char *udata = NULL, *payload = NULL;
    struct pb_link *links = NULL;
    int is_dir = 0, nlinks = 0, utype = 0, i, ok = 0;
    *is_file = 0;
    if (depth > 32 || !block_read(cid, &block, &len)) return 0;
    links = (struct pb_link *)AllocVec(sizeof(struct pb_link) * NODE_MAX_PARTS,
                                       MEMF_ANY);
    if (!links) {
        FreeVec(block);
        return 0;
    }
    if (!pb_parse(block, len, links, NODE_MAX_PARTS, &is_dir, &nlinks) ||
        !pb_get_data(block, len, &udata, &ulen) ||
        !unixfs_info(udata, ulen, &utype, &payload, &plen)) {
        ok = file_append(out, block, len);
        *is_file = ok;
        goto done;
    }
    if (utype == 1) goto done;             /* UnixFS directory */
    if (utype == 2) {                      /* UnixFS file or chunk */
        *is_file = 1;
        if (!file_append(out, payload, plen)) goto done;
        for (i = 0; i < nlinks; ++i) {
            int child_file = 0;
            if (!read_unixfs_collect(links[i].cid.s, out, depth + 1,
                                     &child_file) || !child_file)
                goto done;
        }
        ok = 1;
        goto done;
    }
    ok = file_append(out, block, len);     /* raw or unknown node */
    *is_file = ok;
done:
    FreeVec(links);
    FreeVec(block);
    return ok;
}

static int read_unixfs(const char *cid, unsigned char **data,
                       unsigned long *len)
{
    struct file_out o;
    int is_file = 0;
    o.p = NULL;
    o.len = 0;
    o.cap = 0;
    if (!read_unixfs_collect(cid, &o, 0, &is_file) || !is_file) {
        FreeVec(o.p);
        return 0;
    }
    *data = o.p;
    *len = o.len;
    return 1;
}

static void html_escape(const char *in, char *out, int cap)
{
    int n = 0;
    while (*in && n + 8 < cap) {
        if (*in == '&') { memcpy(out + n, "&amp;", 5); n += 5; }
        else if (*in == '<') { memcpy(out + n, "&lt;", 4); n += 4; }
        else if (*in == '>') { memcpy(out + n, "&gt;", 4); n += 4; }
        else if (*in == '"') { memcpy(out + n, "&quot;", 6); n += 6; }
        else out[n++] = *in;
        ++in;
    }
    out[n] = 0;
}

static void url_encode(const char *in, char *out, int cap)
{
    static const char *hex = "0123456789ABCDEF";
    int n = 0;
    while (*in && n + 4 < cap) {
        unsigned char c = (unsigned char)*in++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[(c >> 4) & 15];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
}

static const char *content_type(const char *name)
{
    const char *dot = strrchr(name ? name : "", '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css")) return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js")) return "application/javascript";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".txt")) return "text/plain; charset=utf-8";
    if (!strcmp(dot, ".svg")) return "image/svg+xml";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcmp(dot, ".gif")) return "image/gif";
    return "application/octet-stream";
}

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                        */
/* ------------------------------------------------------------------ */

static int send_all(int s, const void *data, unsigned long len)
{
    const unsigned char *p = (const unsigned char *)data;
    while (len) {
        LONG n = send(s, p, (LONG)len, 0);
        if (n <= 0) return 0;
        p += n;
        len -= (unsigned long)n;
    }
    return 1;
}

static void http_send(int s, int status, const char *type,
                      const unsigned char *body, unsigned long len)
{
    char hdr[512];
    const char *reason = status == 200 ? "OK" :
                         status == 404 ? "Not Found" :
                         status == 400 ? "Bad Request" :
                         status == 413 ? "Payload Too Large" :
                         status == 500 ? "Internal Server Error" : "Error";
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Connection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n",
                     status, reason, type ? type : "text/plain; charset=utf-8", len);
    send_all(s, hdr, (unsigned long)n);
    if (body && len) send_all(s, body, len);
}

static void http_error(int s, int status, const char *msg)
{
    char body[512];
    int n = snprintf(body, sizeof(body), "%s\n", msg ? msg : "error");
    http_send(s, status, "text/plain; charset=utf-8", (const unsigned char *)body, (unsigned long)n);
}

static int recv_byte(int s)
{
    unsigned char c;
    LONG n = recv(s, &c, 1, 0);
    if (n != 1) return -1;
    return c;
}

static int read_request(struct http_req *r)
{
    char *header;
    int used = 0, ch, sock = r->s, result = 0;
    char *line_end, *p;
    unsigned long body_len = 0;

    memset(r, 0, sizeof(*r));
    r->s = sock;

    /* The 68K CLI stack is small; keep the 16 KiB header buffer on the heap. */
    header = (char *)AllocVec(NODE_HTTP_MAX, MEMF_ANY);
    if (!header) return -1;
    while (used < NODE_HTTP_MAX - 1) {
        ch = recv_byte(r->s);
        if (ch < 0) goto out;
        header[used++] = (char)ch;
        if (used >= 4 && !memcmp(header + used - 4, "\r\n\r\n", 4)) break;
    }
    header[used] = 0;
    line_end = strstr(header, "\r\n");
    if (!line_end) goto out;
    *line_end = 0;
    if (sscanf(header, "%7s %1023s", r->method, r->target) != 2) goto out;
    p = line_end + 2;
    while (*p) {
        char *e = strstr(p, "\r\n");
        if (!e) break;
        *e = 0;
        if (ci_prefix(p, "Content-Length:")) {
            body_len = strtoul(p + 15, NULL, 10);
        } else if (ci_prefix(p, "Content-Type:")) {
            const char *v = p + 13;
            while (*v == ' ' || *v == '\t') ++v;
            strncpy(r->ctype, v, sizeof(r->ctype) - 1);
            r->ctype[sizeof(r->ctype) - 1] = 0;
        }
        p = e + 2;
    }
    strncpy(r->path, r->target, sizeof(r->path) - 1);
    r->path[sizeof(r->path) - 1] = 0;
    p = strchr(r->path, '?');
    if (p) {
        *p++ = 0;
        strncpy(r->query, p, sizeof(r->query) - 1);
        r->query[sizeof(r->query) - 1] = 0;
    }
    r->content_len = body_len;
    if (body_len > NODE_MAX_UPLOAD) {
        result = -1;
        goto out;
    }
    if (body_len) {
        unsigned long left = body_len;
        r->body = (unsigned char *)AllocVec(body_len, MEMF_ANY);
        if (!r->body) {
            result = -1;
            goto out;
        }
        while (left) {
            LONG n = recv(r->s, r->body + (body_len - left), (LONG)left, 0);
            if (n <= 0) {
                FreeVec(r->body);
                r->body = NULL;
                goto out;
            }
            left -= (unsigned long)n;
        }
    }
    result = 1;
out:
    FreeVec(header);
    return result;
}

static void free_request(struct http_req *r)
{
    if (r->body) FreeVec(r->body);
    r->body = NULL;
}

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            int hi = s[1] >= '0' && s[1] <= '9' ? s[1] - '0' :
                     s[1] >= 'a' && s[1] <= 'f' ? s[1] - 'a' + 10 :
                     s[1] >= 'A' && s[1] <= 'F' ? s[1] - 'A' + 10 : -1;
            int lo = s[2] >= '0' && s[2] <= '9' ? s[2] - '0' :
                     s[2] >= 'a' && s[2] <= 'f' ? s[2] - 'a' + 10 :
                     s[2] >= 'A' && s[2] <= 'F' ? s[2] - 'A' + 10 : -1;
            if (hi >= 0 && lo >= 0) {
                *o++ = (char)((hi << 4) | lo);
                s += 3;
                continue;
            }
        }
        if (*s == '+') *o++ = ' ';
        else *o++ = *s;
        ++s;
    }
    *o = 0;
}

static int query_get(const char *query, const char *key, char *out, int cap)
{
    const char *p = query;
    int klen = (int)strlen(key);
    out[0] = 0;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        int n = amp ? (int)(amp - p) : (int)strlen(p);
        if (n > klen && !strncmp(p, key, klen) && p[klen] == '=') {
            int vlen = n - klen - 1;
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = 0;
            url_decode(out);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

static void json_escape(const char *in, char *out, int cap)
{
    int n = 0;
    while (*in && n + 7 < cap) {
        if (*in == '"' || *in == '\\') {
            out[n++] = '\\';
            out[n++] = *in++;
        } else if ((unsigned char)*in < 32) {
            ++in;
        } else {
            out[n++] = *in++;
        }
    }
    out[n] = 0;
}

/* ------------------------------------------------------------------ */
/* API handlers                                                        */
/* ------------------------------------------------------------------ */

static void api_id(int s)
{
    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"ID\":\"%s\",\"PublicKey\":\"\",\"Addresses\":[],"
                     "\"AgentVersion\":\"qrx-ipfsd/0.1\","
                     "\"ProtocolVersion\":\"ipfs/0.1.0\",\"Protocols\":[]}\n",
                     g_node_id);
    http_send(s, 200, "application/json", (const unsigned char *)body, (unsigned long)n);
}

static void api_version(int s)
{
    const char *body = "{\"Version\":\"0.1.0-qrx\",\"Commit\":\"\",\"Repo\":\"1\","
                       "\"System\":\"aros/m68k\",\"Golang\":\"\"}\n";
    http_send(s, 200, "application/json", (const unsigned char *)body,
              (unsigned long)strlen(body));
}

static void api_add(int s, struct http_req *r)
{
    char pinbuf[8], cid[NODE_MAX_CID], name[NODE_MAX_PATH], jname[2 * NODE_MAX_PATH];
    char body[640];
    unsigned long size = 0;
    int pin = 0, n;

    query_get(r->query, "pin", pinbuf, sizeof(pinbuf));
    pin = pinbuf[0] == '1' || pinbuf[0] == 't' || pinbuf[0] == 'y';
    if (!node_add(r->body, r->content_len, r->ctype, pin,
                  cid, sizeof(cid), name, sizeof(name), &size)) {
        http_error(s, 500, g_err);
        return;
    }
    json_escape(name, jname, sizeof(jname));
    n = snprintf(body, sizeof(body),
                 "{\"Name\":\"%s\",\"Hash\":\"%s\",\"Size\":\"%lu\"}\n",
                 jname, cid, size);
    http_send(s, 200, "application/json", (const unsigned char *)body, (unsigned long)n);
}

static void api_cat(int s, struct http_req *r)
{
    char cid[NODE_MAX_CID];
    unsigned char *data = NULL;
    unsigned long len = 0;
    if (!query_get(r->query, "arg", cid, sizeof(cid)) || !safe_cid(cid)) {
        http_error(s, 400, "CID fehlt oder ist ungueltig.");
        return;
    }
    if (!read_unixfs(cid, &data, &len)) {
        http_error(s, 404, "Block nicht im lokalen Node.");
        return;
    }
    http_send(s, 200, "application/octet-stream", data, len);
    FreeVec(data);
}

static void api_block_get(int s, struct http_req *r)
{
    char cid[NODE_MAX_CID];
    unsigned char *data = NULL;
    unsigned long len = 0;
    if (!query_get(r->query, "arg", cid, sizeof(cid)) || !safe_cid(cid)) {
        http_error(s, 400, "CID fehlt oder ist ungueltig.");
        return;
    }
    if (!block_read(cid, &data, &len)) {
        http_error(s, 404, "Block nicht im lokalen Node.");
        return;
    }
    http_send(s, 200, "application/octet-stream", data, len);
    FreeVec(data);
}

static void api_pin_add(int s, struct http_req *r)
{
    char cid[NODE_MAX_CID], body[512];
    int n;
    if (!query_get(r->query, "arg", cid, sizeof(cid)) || !safe_cid(cid)) {
        http_error(s, 400, "CID fehlt oder ist ungueltig.");
        return;
    }
    if (!pin_add(cid)) {
        http_error(s, 500, "Pinning fehlgeschlagen.");
        return;
    }
    n = snprintf(body, sizeof(body), "{\"Pins\":[\"%s\"]}\n", cid);
    http_send(s, 200, "application/json", (const unsigned char *)body, (unsigned long)n);
}

static void api_pin_ls(int s, struct http_req *r)
{
    char cid[NODE_MAX_CID], body[512];
    int n;
    if (!query_get(r->query, "arg", cid, sizeof(cid)) || !safe_cid(cid)) {
        http_error(s, 400, "CID fehlt oder ist ungueltig.");
        return;
    }
    if (pin_has(cid))
        n = snprintf(body, sizeof(body), "{\"Keys\":{\"%s\":{\"Type\":\"recursive\"}}}\n", cid);
    else
        n = snprintf(body, sizeof(body), "{\"Keys\":{}}\n");
    http_send(s, 200, "application/json", (const unsigned char *)body, (unsigned long)n);
}

static void api_pin_rm(int s, struct http_req *r)
{
    char cid[NODE_MAX_CID], body[512];
    int n;
    if (!query_get(r->query, "arg", cid, sizeof(cid)) || !safe_cid(cid)) {
        http_error(s, 400, "CID fehlt oder ist ungueltig.");
        return;
    }
    if (!pin_remove(cid)) {
        http_error(s, 500, "Unpin fehlgeschlagen.");
        return;
    }
    n = snprintf(body, sizeof(body), "{\"Pins\":[\"%s\"]}\n", cid);
    http_send(s, 200, "application/json", (const unsigned char *)body, (unsigned long)n);
}

static int cid_is_dir(const struct cid *c)
{
    return c && c->len >= 2 && c->b[1] == 0x70;
}

static void api_ls(int s, struct http_req *r)
{
    char cid[NODE_MAX_CID];
    struct pb_link *links;
    unsigned char *block = NULL;
    unsigned long len = 0, cap, pos = 0;
    char *body;
    int nlinks = 0, is_dir = 0, i;

    if (!query_get(r->query, "arg", cid, sizeof(cid)) || !safe_cid(cid)) {
        http_error(s, 400, "CID fehlt oder ist ungueltig.");
        return;
    }
    links = (struct pb_link *)AllocVec(sizeof(struct pb_link) * NODE_MAX_PARTS, MEMF_ANY);
    if (!links) {
        http_error(s, 500, "Kein Speicher fuer Listing.");
        return;
    }
    if (!block_read(cid, &block, &len)) {
        FreeVec(links);
        http_error(s, 404, "CID nicht im lokalen Node.");
        return;
    }
    if (!pb_parse(block, len, links, NODE_MAX_PARTS, &is_dir, &nlinks) || !is_dir) {
        FreeVec(block);
        FreeVec(links);
        http_error(s, 400, "CID ist kein Verzeichnis.");
        return;
    }
    FreeVec(block);

    cap = 4096UL + (unsigned long)nlinks * 1024UL;
    body = (char *)AllocVec(cap, MEMF_ANY);
    if (!body) {
        FreeVec(links);
        http_error(s, 500, "Kein Speicher fuer Listing.");
        return;
    }
    pos = (unsigned long)snprintf(body, cap,
        "{\"Objects\":[{\"Hash\":\"%s\",\"Links\":[", cid);
    for (i = 0; i < nlinks; ++i) {
        char jname[512];
        json_escape(links[i].name, jname, sizeof(jname));
        if (pos + 1024 >= cap) break;
        pos += (unsigned long)snprintf(body + pos, cap - pos,
            "%s{\"Name\":\"%s\",\"Hash\":\"%s\",\"Size\":%lu,\"Type\":%d}",
            i ? "," : "", jname, links[i].cid.s, links[i].size,
            cid_is_dir(&links[i].cid) ? 1 : 2);
    }
    pos += (unsigned long)snprintf(body + pos, cap - pos, "]}]}\n");
    http_send(s, 200, "application/json", (const unsigned char *)body, pos);
    FreeVec(body);
    FreeVec(links);
}

static void api_block_stat(int s, struct http_req *r)
{
    char cid[NODE_MAX_CID], body[512];
    unsigned long size;
    int n;
    if (!query_get(r->query, "arg", cid, sizeof(cid)) || !safe_cid(cid) ||
        !block_size(cid, &size)) {
        http_error(s, 404, "Block nicht im lokalen Node.");
        return;
    }
    n = snprintf(body, sizeof(body), "{\"Key\":\"%s\",\"Size\":%lu}\n", cid, size);
    http_send(s, 200, "application/json", (const unsigned char *)body, (unsigned long)n);
}

static void api_repo_stat(int s)
{
    unsigned long count = 0, bytes = 0;
    char body[256];
    int n;
    repo_stat(&count, &bytes);
    n = snprintf(body, sizeof(body),
                 "{\"RepoSize\":%lu,\"StorageMax\":%lu,\"NumObjects\":%lu,"
                 "\"RepoPath\":\"%s\"}\n",
                 bytes, NODE_MAX_UPLOAD * 64UL, count, NODE_DIR);
    http_send(s, 200, "application/json", (const unsigned char *)body, (unsigned long)n);
}

/* ------------------------------------------------------------------ */
/* gateway                                                             */
/* ------------------------------------------------------------------ */

static int gateway_listing(int s, const char *cid, struct pb_link *links, int n)
{
    unsigned long cap = 8192UL + (unsigned long)n * 2048UL;
    char *html = (char *)AllocVec(cap, MEMF_ANY);
    unsigned long pos = 0;
    int i;
    if (!html) return 0;
    pos += (unsigned long)snprintf(html + pos, cap - pos,
        "<!doctype html><meta charset=\"utf-8\"><title>IPFS %s</title>"
        "<h1>IPFS-Verzeichnis</h1><p><code>%s</code></p><ul>", cid, cid);
    for (i = 0; i < n; ++i) {
        char esc[1024], url[1024];
        html_escape(links[i].name, esc, sizeof(esc));
        url_encode(links[i].name, url, sizeof(url));
        pos += (unsigned long)snprintf(html + pos, cap - pos,
            "<li><a href=\"/ipfs/%s/%s\">%s</a></li>", cid, url, esc);
    }
    pos += (unsigned long)snprintf(html + pos, cap - pos,
        "</ul><hr><small>QRXIPFSD: lokaler Node ohne Peers.</small>");
    http_send(s, 200, "text/html; charset=utf-8", (const unsigned char *)html, pos);
    FreeVec(html);
    return 1;
}

static int gateway_serve(int s, const char *cid, const char *subpath)
{
    char cur[NODE_MAX_CID];
    const char *p = subpath;
    struct pb_link *links;
    int is_dir = 0, nlinks = 0, i, path_ok = 1;
    unsigned char *block = NULL;
    unsigned long len = 0;
    char last[NODE_MAX_PATH];

    strncpy(cur, cid, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = 0;
    last[0] = 0;
    links = (struct pb_link *)AllocVec(sizeof(struct pb_link) * NODE_MAX_PARTS, MEMF_ANY);
    if (!links) {
        http_error(s, 500, "Kein Speicher fuer Gateway.");
        return 1;
    }

    while (p && *p) {
        char seg[256];
        int n = 0;
        while (*p == '/') ++p;
        if (!*p) break;
        while (*p && *p != '/' && n + 1 < (int)sizeof(seg)) seg[n++] = *p++;
        seg[n] = 0;
        url_decode(seg);
        if (!block_read(cur, &block, &len)) {
            path_ok = 0;
            break;
        }
        if (!pb_parse(block, len, links, NODE_MAX_PARTS, &is_dir, &nlinks) || !is_dir) {
            FreeVec(block);
            block = NULL;
            path_ok = 0;
            break;
        }
        FreeVec(block);
        block = NULL;
        for (i = 0; i < nlinks; ++i) {
            if (!strcmp(links[i].name, seg)) {
                strncpy(cur, links[i].cid.s, sizeof(cur) - 1);
                cur[sizeof(cur) - 1] = 0;
                strncpy(last, seg, sizeof(last) - 1);
                last[sizeof(last) - 1] = 0;
                break;
            }
        }
        if (i >= nlinks) {
            path_ok = 0;
            break;
        }
    }
    if (!path_ok) {
        FreeVec(links);
        http_error(s, 404, "Pfad nicht gefunden.");
        return 1;
    }

    if (!block_read(cur, &block, &len)) {
        FreeVec(links);
        http_error(s, 404, "CID nicht im lokalen Node.");
        return 1;
    }
    if (!pb_parse(block, len, links, NODE_MAX_PARTS, &is_dir, &nlinks)) {
        is_dir = 0;
        nlinks = 0;
    }
    if (is_dir) {
        for (i = 0; i < nlinks; ++i) {
            if (!strcmp(links[i].name, "index.html")) {
                unsigned char *idx = NULL;
                unsigned long ilen = 0;
                if (read_unixfs(links[i].cid.s, &idx, &ilen)) {
                    http_send(s, 200, "text/html; charset=utf-8", idx, ilen);
                    FreeVec(idx);
                    FreeVec(block);
                    FreeVec(links);
                    return 1;
                }
            }
        }
        if (!gateway_listing(s, cur, links, nlinks))
            http_error(s, 500, "Kein Speicher fuer Verzeichnisliste.");
        FreeVec(block);
        FreeVec(links);
        return 1;
    }
    {
        unsigned char *file = NULL;
        unsigned long flen = 0;
        if (read_unixfs(cur, &file, &flen)) {
            http_send(s, 200, content_type(last[0] ? last : ""), file, flen);
            FreeVec(file);
            FreeVec(block);
            FreeVec(links);
            return 1;
        }
    }
    http_send(s, 200, content_type(last[0] ? last : ""), block, len);
    FreeVec(block);
    FreeVec(links);
    return 1;
}

static void gateway(int s, const char *path)
{
    char cid[NODE_MAX_CID];
    const char *p;
    int n = 0;
    if (strncmp(path, "/ipfs/", 6)) {
        http_error(s, 404, "Nicht gefunden.");
        return;
    }
    p = path + 6;
    while (*p && *p != '/' && n + 1 < (int)sizeof(cid)) cid[n++] = *p++;
    cid[n] = 0;
    if (!safe_cid(cid)) {
        http_error(s, 400, "Ungueltige CID.");
        return;
    }
    gateway_serve(s, cid, p);
}

/* ------------------------------------------------------------------ */
/* routing and server                                                  */
/* ------------------------------------------------------------------ */

static void handle_request(int s)
{
    struct http_req req;
    int rc;
    req.s = s;
    rc = read_request(&req);
    if (rc == 0) {
        free_request(&req);
        return;
    }
    if (rc < 0) {
        http_error(s, 413, "Upload zu gross (maximal 16 MiB).");
        free_request(&req);
        return;
    }

    if (!strncmp(req.path, "/ipfs/", 6)) {
        gateway(s, req.path);
    } else if (!strcmp(req.path, "/api/v0/id")) {
        api_id(s);
    } else if (!strcmp(req.path, "/api/v0/version")) {
        api_version(s);
    } else if (!strcmp(req.path, "/api/v0/add")) {
        api_add(s, &req);
    } else if (!strcmp(req.path, "/api/v0/cat")) {
        api_cat(s, &req);
    } else if (!strcmp(req.path, "/api/v0/pin/add")) {
        api_pin_add(s, &req);
    } else if (!strcmp(req.path, "/api/v0/pin/ls")) {
        api_pin_ls(s, &req);
    } else if (!strcmp(req.path, "/api/v0/pin/rm")) {
        api_pin_rm(s, &req);
    } else if (!strcmp(req.path, "/api/v0/ls")) {
        api_ls(s, &req);
    } else if (!strcmp(req.path, "/api/v0/block/stat")) {
        api_block_stat(s, &req);
    } else if (!strcmp(req.path, "/api/v0/block/get")) {
        api_block_get(s, &req);
    } else if (!strcmp(req.path, "/api/v0/repo/stat")) {
        api_repo_stat(s);
    } else if (!strcmp(req.path, "/")) {
        char body[512];
        int n = snprintf(body, sizeof(body),
            "QRXIPFSD %s\nlocal node: %s\nAPI: 127.0.0.1:%d/api/v0/\n"
            "Gateway: http://127.0.0.1:%d/ipfs/<cid>\n"
            "P2P: C:QRXIPFSP2P (control 127.0.0.1:5002)\n",
            "0.1", g_node_id, g_port, g_port);
        http_send(s, 200, "text/plain; charset=utf-8", (const unsigned char *)body, (unsigned long)n);
    } else {
        http_error(s, 404, "Unbekannter Endpunkt.");
    }
    free_request(&req);
}

static int env_port(void)
{
    char buf[32];
    if (GetVar((CONST_STRPTR)"QRX_IPFS_NODE_PORT", (STRPTR)buf, sizeof(buf) - 1,
               GVF_GLOBAL_ONLY) > 0) {
        int p = atoi(buf);
        if (p > 0 && p < 65536) return p;
    }
    return NODE_PORT;
}

int main(void)
{
    struct sockaddr_in sa;
    int ls;

    g_port = env_port();
    load_node_config();
    if (!node_init()) {
        printf("QRXIPFSD: %s\n", g_err);
        return 10;
    }

    while (!qrx_net_init()) {
        printf("QRXIPFSD: warte auf QRX Host-Socket-Door...\n");
        Delay(250);
    }

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        printf("QRXIPFSD: kein Socket.\n");
        return 11;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)g_port);
    /* Bind on the host, not on guest loopback: Safari and other LAN devices
     * must be able to reach the gateway. */
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("QRXIPFSD: Port %d belegt.\n", g_port);
        CloseSocket(ls);
        return 12;
    }
    if (listen(ls, SOMAXCONN) < 0) {
        printf("QRXIPFSD: listen fehlgeschlagen.\n");
        CloseSocket(ls);
        return 13;
    }

    printf("QRXIPFSD %s\n", g_node_id);
    printf("API:     http://127.0.0.1:%d/api/v0/\n", g_port);
    printf("Gateway: http://<host-ip>:%d/ipfs/<cid>\n", g_port);
    printf("Autofetch: %s\n", g_autofetch ? "an" : "aus");
    printf("Hinweis: lokaler Node; P2P-Peers ueber QRXIPFSP2P.\n");
    fflush(stdout);

    for (;;) {
        int cs = accept(ls, NULL, NULL);
        if (cs < 0) {
            Delay(25);
            continue;
        }
        handle_request(cs);
        CloseSocket(cs);
    }
    return 0;
}
