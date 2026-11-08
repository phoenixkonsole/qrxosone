/*
 * qrx_ipfs.c - HTTP API client for a local IPFS node (QRXIPFSD or Kubo).
 *
 * Everything here is plain AROS C.  Sockets go through qrx_net.c, the
 * QRX host-socket door; the node keeps the content, this file only
 * streams files to /api/v0/add and reads hashes back.
 */
#include "qrx_ipfs.h"

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

#define HTTP_BUF      4096
#define MAX_PARTS     512

struct add_part {
    char rel[220];
    char full[420];
    unsigned long size;
};

struct http_conn {
    int s;
    char buf[HTTP_BUF];
    int pos, len;
};

static char g_host[64] = "127.0.0.1";
static int  g_port = 5001;
static int  g_opened = 0;
static char g_err[200] = "";

static const char *boundary = "----QRXIPFSBoundary7MA4YWxkTrZu0gW";

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void set_err(const char *msg)
{
    if (!msg) msg = "";
    strncpy(g_err, msg, sizeof(g_err) - 1);
    g_err[sizeof(g_err) - 1] = 0;
}

const char *qrx_ipfs_error(void)
{
    return g_err;
}

const char *qrx_ipfs_api(void)
{
    static char api[80];
    snprintf(api, sizeof(api), "%s:%d", g_host, g_port);
    return api;
}

int qrx_ipfs_configured(void)
{
    return g_opened;
}

static void parse_api(const char *value)
{
    const char *colon;
    unsigned long n;

    if (!value || !*value) return;
    colon = strchr(value, ':');
    n = colon ? (unsigned long)(colon - value) : (unsigned long)strlen(value);
    if (n >= sizeof(g_host)) n = sizeof(g_host) - 1;
    memcpy(g_host, value, n);
    g_host[n] = 0;
    if (colon && colon[1]) {
        int p = atoi(colon + 1);
        if (p > 0 && p < 65536) g_port = p;
    }
}

int qrx_ipfs_set_api(const char *host_port)
{
    if (!host_port || !*host_port) return 0;
    parse_api(host_port);
    return 1;
}

int qrx_ipfs_open(void)
{
    char env[80];

    if (g_opened) return 1;
    if (!qrx_net_init()) {
        set_err("QRX Host-Socket-Door nicht verfuegbar - ARMithlon Netzwerk starten.");
        return 0;
    }
    env[0] = 0;
    if (GetVar((CONST_STRPTR)"QRX_IPFS_API", (STRPTR)env, sizeof(env) - 1, GVF_GLOBAL_ONLY) > 0)
        parse_api(env);
    g_opened = 1;
    set_err("");
    return 1;
}

void qrx_ipfs_close(void)
{
    g_opened = 0;
}

int qrx_ipfs_ensure_volume(void)
{
    BPTR lock;

    lock = Lock((CONST_STRPTR)QRX_IPFS_VOLUME, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        return 1;
    }
    lock = CreateDir((CONST_STRPTR)QRX_IPFS_VOLUME_DIR);
    if (lock) UnLock(lock);
    lock = Lock((CONST_STRPTR)QRX_IPFS_VOLUME_DIR, ACCESS_READ);
    if (!lock) {
        set_err("IPFS-Speicher konnte nicht angelegt werden.");
        return 0;
    }
    if (!AssignLock((CONST_STRPTR)"IPFS", lock)) {
        UnLock(lock);
        set_err("IPFS: Assign konnte nicht gesetzt werden.");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

static int tcp_connect(void)
{
    struct sockaddr_in sa;
    struct hostent *he;
    int s;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)g_port);
    he = gethostbyname(g_host);
    if (he && he->h_addrtype == AF_INET && he->h_length == 4)
        memcpy(&sa.sin_addr, he->h_addr, 4);
    else
        sa.sin_addr.s_addr = inet_addr(g_host);
    if (sa.sin_addr.s_addr == 0xffffffffUL) {
        set_err("IPFS-API Host nicht aufloesbar.");
        return -1;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        set_err("Kein Socket - bsdsocket Netzwerk aktiv?");
        return -1;
    }
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        CloseSocket(s);
        set_err("IPFS-Daemon nicht erreichbar (127.0.0.1:5001?).");
        return -1;
    }
    return s;
}

static int send_all(int s, const void *data, unsigned long len)
{
    const char *p = (const char *)data;
    while (len) {
        LONG n = send(s, p, (LONG)len, 0);
        if (n <= 0) return 0;
        p += n;
        len -= (unsigned long)n;
    }
    return 1;
}

static int conn_getc(struct http_conn *c)
{
    if (c->pos >= c->len) {
        LONG n = recv(c->s, c->buf, HTTP_BUF, 0);
        if (n <= 0) return -1;
        c->len = (int)n;
        c->pos = 0;
    }
    return (unsigned char)c->buf[c->pos++];
}

static int conn_read(struct http_conn *c, void *out, unsigned long len)
{
    unsigned char *p = (unsigned char *)out;
    while (len) {
        int ch;
        if (c->pos < c->len) {
            int take = (int)(len < (unsigned long)(c->len - c->pos) ? len : (unsigned long)(c->len - c->pos));
            memcpy(p, c->buf + c->pos, take);
            c->pos += take;
            p += take;
            len -= take;
            continue;
        }
        ch = conn_getc(c);
        if (ch < 0) return 0;
        *p++ = (unsigned char)ch;
        --len;
    }
    return 1;
}

static int read_headers(struct http_conn *c, int *status, long *length, int *chunked)
{
    char line[512];
    int n = 0, ch;
    int first = 1;

    *status = 0;
    *length = -1;
    *chunked = 0;
    for (;;) {
        n = 0;
        while ((ch = conn_getc(c)) >= 0 && ch != '\n') {
            if (n < (int)sizeof(line) - 1) line[n++] = (char)ch;
        }
        if (ch < 0) return 0;
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) --n;
        line[n] = 0;
        if (first) {
            char *sp = strchr(line, ' ');
            if (sp) *status = atoi(sp + 1);
            first = 0;
            if (*status != 200) {
                /* still drain the body later, but remember the code */
            }
        } else if (!n) {
            return 1;
        } else {
            char *colon = strchr(line, ':');
            if (colon) {
                char *v = colon + 1;
                while (*v == ' ') ++v;
                *colon = 0;
                if (!strcmp(line, "Content-Length")) *length = atol(v);
                else if (!strcmp(line, "Transfer-Encoding") && strstr(v, "chunked"))
                    *chunked = 1;
            }
        }
    }
}

static int http_begin(const char *method, const char *path,
                      const char *content_type, unsigned long content_length)
{
    char head[640];
    int s = tcp_connect();
    int n;

    if (s < 0) return -1;
    n = snprintf(head, sizeof(head),
                 "%s %s HTTP/1.0\r\n"
                 "Host: %s:%d\r\n"
                 "User-Agent: QRXIPFS/%s (AROS m68k)\r\n"
                 "Connection: close\r\n"
                 "Accept: */*\r\n",
                 method, path, g_host, g_port, QRX_IPFS_VERSION);
    if (content_type) {
        n += snprintf(head + n, sizeof(head) - (unsigned)n,
                      "Content-Type: %s\r\nContent-Length: %lu\r\n",
                      content_type, content_length);
    }
    n += snprintf(head + n, sizeof(head) - (unsigned)n, "\r\n");
    if (!send_all(s, head, (unsigned long)n)) {
        CloseSocket(s);
        set_err("Senden an IPFS-API fehlgeschlagen.");
        return -1;
    }
    return s;
}

static int read_body_to_memory(struct http_conn *c, int status, long length,
                               int chunked, char *out, unsigned long cap)
{
    unsigned long used = 0;

    out[0] = 0;
    if (status != 200) {
        char tmp[512];
        long got = 0;
        while (got < (long)sizeof(tmp) - 1 && conn_getc(c) >= 0) {
            tmp[got++] = (char)c->buf[c->pos - 1];
        }
        tmp[got] = 0;
        snprintf(g_err, sizeof(g_err), "IPFS API HTTP %d: %.140s", status, tmp);
        return 0;
    }
    if (chunked) {
        for (;;) {
            char hex[16];
            int n = 0, ch;
            long size;
            while ((ch = conn_getc(c)) >= 0 && ch != '\n' && n < 15) hex[n++] = (char)ch;
            hex[n] = 0;
            size = strtol(hex, NULL, 16);
            if (size <= 0) break;
            while (size-- > 0) {
                ch = conn_getc(c);
                if (ch < 0) return 0;
                if (used + 1 < cap) out[used++] = (char)ch;
                else if (used > cap / 2) { memmove(out, out + used / 2, used - used / 2); used -= used / 2; out[used++] = (char)ch; }
            }
            conn_getc(c); /* CR */
            conn_getc(c); /* LF */
        }
    } else if (length >= 0) {
        while (length-- > 0) {
            int ch = conn_getc(c);
            if (ch < 0) break;
            if (used + 1 < cap) out[used++] = (char)ch;
            else if (used > cap / 2) { memmove(out, out + used / 2, used - used / 2); used -= used / 2; out[used++] = (char)ch; }
        }
    } else {
        int ch;
        while ((ch = conn_getc(c)) >= 0) {
            if (used + 1 < cap) out[used++] = (char)ch;
            else if (used > cap / 2) { memmove(out, out + used / 2, used - used / 2); used -= used / 2; out[used++] = (char)ch; }
        }
    }
    out[used] = 0;
    return 1;
}

static int json_last_string(const char *body, const char *key, char *out, unsigned long cap)
{
    char pattern[40];
    const char *p = body, *last = NULL;
    unsigned long n = 0;

    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    while ((p = strstr(p, pattern)) != NULL) {
        last = p + strlen(pattern);
        p = last;
    }
    if (!last) return 0;
    while (last[n] && last[n] != '"' && n + 1 < cap) {
        out[n] = last[n];
        ++n;
    }
    out[n] = 0;
    return n > 0;
}

static const char *json_key(const char *p, const char *key)
{
    char pattern[40];
    if (!p) return NULL;
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    return strstr(p, pattern);
}

static const char *json_string_at(const char *p, const char *key,
                                  char *out, unsigned long cap)
{
    const char *v = json_key(p, key);
    unsigned long n = 0;

    if (!v) return NULL;
    v += strlen(key) + 3;
    while (*v == ' ') ++v;
    if (*v != '"') return NULL;
    ++v;
    while (*v && *v != '"') {
        if (*v == '\\' && v[1]) ++v;
        if (n + 1 < cap) out[n++] = *v;
        ++v;
    }
    out[n] = 0;
    return v;
}

static int json_number_at(const char *p, const char *key, unsigned long *out)
{
    const char *v = json_key(p, key);
    if (!v) return 0;
    v += strlen(key) + 3;
    while (*v == ' ') ++v;
    *out = strtoul(v, NULL, 10);
    return 1;
}

static const char *json_object_end(const char *p)
{
    int in_string = 0, escaped = 0;
    if (!p) return NULL;
    for (; *p; ++p) {
        if (escaped) { escaped = 0; continue; }
        if (*p == '\\') { escaped = 1; continue; }
        if (*p == '"') { in_string = !in_string; continue; }
        if (!in_string && *p == '}') return p;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* recursive file collection                                           */
/* ------------------------------------------------------------------ */

static int collect_parts(const char *full, const char *rel,
                         struct add_part *parts, int *count, int max)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    int ok = 1;

    lock = Lock((CONST_STRPTR)full, ACCESS_READ);
    if (!lock) return 0;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        UnLock(lock);
        return 0;
    }
    if (!Examine(lock, fib)) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        return 0;
    }
    if (fib->fib_DirEntryType > 0) {
        while (ExNext(lock, fib)) {
            char child_full[420];
            char child_rel[220];
            if (fib->fib_FileName[0] == '.') continue;
            if (!strcmp((char *)fib->fib_FileName, "REVISIONS")) continue;
            snprintf(child_full, sizeof(child_full), "%s/%s", full,
                     (char *)fib->fib_FileName);
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel,
                     (char *)fib->fib_FileName);
            if (!collect_parts(child_full, child_rel, parts, count, max)) {
                ok = 0;
                break;
            }
        }
    } else {
        if (*count >= max) {
            set_err("Zu viele Dateien fuer einen Upload (max. 512).");
            ok = 0;
        } else {
            strncpy(parts[*count].rel, rel, sizeof(parts[*count].rel) - 1);
            parts[*count].rel[sizeof(parts[*count].rel) - 1] = 0;
            strncpy(parts[*count].full, full, sizeof(parts[*count].full) - 1);
            parts[*count].full[sizeof(parts[*count].full) - 1] = 0;
            parts[*count].size = (unsigned long)fib->fib_Size;
            ++*count;
        }
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return ok;
}

static unsigned long part_header_len(const char *rel)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                     "Content-Type: application/octet-stream\r\n\r\n",
                     boundary, rel);
    return (unsigned long)n;
}

static int send_part(int s, const struct add_part *p)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                     "Content-Type: application/octet-stream\r\n\r\n",
                     boundary, p->rel);
    BPTR f;
    char buf[1024];
    LONG got;

    if (!send_all(s, hdr, (unsigned long)n)) return 0;
    f = Open((CONST_STRPTR)p->full, MODE_OLDFILE);
    if (!f) {
        snprintf(g_err, sizeof(g_err), "Datei nicht lesbar: %s", p->full);
        return 0;
    }
    while ((got = Read(f, buf, sizeof(buf))) > 0) {
        if (!send_all(s, buf, (unsigned long)got)) {
            Close(f);
            return 0;
        }
    }
    Close(f);
    if (got < 0) {
        set_err("Datei waehrend des Uploads nicht lesbar.");
        return 0;
    }
    return send_all(s, "\r\n", 2);
}

int qrx_ipfs_add(const char *path, int recursive, int pin,
                 char *cid, unsigned long cid_cap)
{
    struct FileInfoBlock *fib;
    BPTR lock;
    struct add_part *parts;
    int count = 0, max, i, s, status, chunked;
    long length;
    unsigned long total = 0;
    char *body;
    char api_path[300];
    struct http_conn conn;
    const char *base;

    if (!cid || cid_cap < 8) return 0;
    cid[0] = 0;
    if (!qrx_ipfs_open()) return 0;

    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (!lock) {
        snprintf(g_err, sizeof(g_err), "Nicht gefunden: %s", path);
        return 0;
    }
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib || !Examine(lock, fib)) {
        if (fib) FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        set_err("Datei/Drawer nicht lesbar.");
        return 0;
    }
    if (fib->fib_DirEntryType > 0) recursive = 1;
    max = recursive ? MAX_PARTS : 1;
    parts = (struct add_part *)AllocVec(sizeof(struct add_part) * max, MEMF_ANY);
    if (!parts) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        set_err("Kein Speicher fuer Upload-Liste.");
        return 0;
    }

    base = path;
    for (i = (int)strlen(path) - 1; i >= 0; --i) {
        if (path[i] == ':' || path[i] == '/') { base = path + i + 1; break; }
    }
    if (!*base) base = "upload";

    if (recursive) {
        if (!collect_parts(path, base, parts, &count, max)) {
            FreeDosObject(DOS_FIB, fib);
            FreeVec(parts);
            UnLock(lock);
            return 0;
        }
    } else {
        strncpy(parts[0].rel, base, sizeof(parts[0].rel) - 1);
        parts[0].rel[sizeof(parts[0].rel) - 1] = 0;
        strncpy(parts[0].full, path, sizeof(parts[0].full) - 1);
        parts[0].full[sizeof(parts[0].full) - 1] = 0;
        parts[0].size = (unsigned long)fib->fib_Size;
        count = 1;
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);

    if (count == 0) {
        FreeVec(parts);
        set_err("Drawer enthaelt keine Dateien (leere Ordner werden nicht publiziert).");
        return 0;
    }

    for (i = 0; i < count; ++i)
        total += part_header_len(parts[i].rel) + parts[i].size + 2;
    total += (unsigned long)strlen(boundary) + 6;   /* closing boundary */

    snprintf(api_path, sizeof(api_path),
             "/api/v0/add?pin=%d&cid-version=1&progress=false&recursive=%d",
             pin ? 1 : 0, recursive ? 1 : 0);
    s = http_begin("POST", api_path, "multipart/form-data; boundary=----QRXIPFSBoundary7MA4YWxkTrZu0gW", total);
    if (s < 0) {
        FreeVec(parts);
        return 0;
    }
    for (i = 0; i < count; ++i) {
        if (!send_part(s, &parts[i])) {
            CloseSocket(s);
            FreeVec(parts);
            set_err("Upload abgebrochen (Datei oder Netzwerk).");
            return 0;
        }
    }
    {
        char close[160];
        int n = snprintf(close, sizeof(close), "--%s--\r\n", boundary);
        send_all(s, close, (unsigned long)n);
    }
    FreeVec(parts);

    body = (char *)AllocVec(65536, MEMF_ANY);
    if (!body) {
        CloseSocket(s);
        set_err("Kein Speicher fuer API-Antwort.");
        return 0;
    }
    memset(&conn, 0, sizeof(conn));
    conn.s = s;
    if (!read_headers(&conn, &status, &length, &chunked) ||
        !read_body_to_memory(&conn, status, length, chunked, body, 65536)) {
        CloseSocket(s);
        FreeVec(body);
        return 0;
    }
    CloseSocket(s);
    if (!json_last_string(body, "Hash", cid, cid_cap)) {
        snprintf(g_err, sizeof(g_err), "Kein CID in der Antwort: %.120s", body);
        FreeVec(body);
        return 0;
    }
    FreeVec(body);
    set_err("");
    return 1;
}

int qrx_ipfs_cat(const char *cid, const char *dest)
{
    char path[400];
    int s, status, chunked;
    long length, left;
    struct http_conn conn;
    BPTR out;
    char buf[2048];

    if (!cid || !dest) return 0;
    if (!qrx_ipfs_open()) return 0;
    snprintf(path, sizeof(path), "/api/v0/cat?arg=%s", cid);
    s = http_begin("POST", path, NULL, 0);
    if (s < 0) return 0;
    memset(&conn, 0, sizeof(conn));
    conn.s = s;
    if (!read_headers(&conn, &status, &length, &chunked)) {
        CloseSocket(s);
        set_err("Keine Antwort vom IPFS-Daemon.");
        return 0;
    }
    if (status != 200) {
        CloseSocket(s);
        snprintf(g_err, sizeof(g_err), "IPFS cat: HTTP %d", status);
        return 0;
    }
    out = Open((CONST_STRPTR)dest, MODE_NEWFILE);
    if (!out) {
        CloseSocket(s);
        set_err("Ziel nicht schreibbar.");
        return 0;
    }
    if (chunked) {
        for (;;) {
            char hex[16];
            int n = 0, ch;
            long size;
            while ((ch = conn_getc(&conn)) >= 0 && ch != '\n' && n < 15) hex[n++] = (char)ch;
            hex[n] = 0;
            size = strtol(hex, NULL, 16);
            if (size <= 0) break;
            while (size > 0) {
                long take = size < (long)sizeof(buf) ? size : (long)sizeof(buf);
                if (!conn_read(&conn, buf, (unsigned long)take)) break;
                Write(out, buf, take);
                size -= take;
            }
            conn_getc(&conn); conn_getc(&conn);
        }
    } else {
        left = length;
        while (left != 0) {
            long take = left < 0 ? (long)sizeof(buf) : (left < (long)sizeof(buf) ? left : (long)sizeof(buf));
            if (!conn_read(&conn, buf, (unsigned long)take)) break;
            Write(out, buf, take);
            if (left > 0) left -= take;
        }
    }
    Close(out);
    CloseSocket(s);
    set_err("");
    return 1;
}

int qrx_ipfs_ls(const char *cid, struct qrx_ipfs_link *links, int max)
{
    char path[400], *body;
    int s, status, chunked, n = 0;
    long length;
    unsigned long cap, body_cap;
    const char *p, *end;
    struct http_conn conn;

    if (!cid || !links || max < 1) return -1;
    if (!qrx_ipfs_open()) return -1;
    snprintf(path, sizeof(path), "/api/v0/ls?arg=%s", cid);
    s = http_begin("POST", path, NULL, 0);
    if (s < 0) return -1;
    memset(&conn, 0, sizeof(conn));
    conn.s = s;
    if (!read_headers(&conn, &status, &length, &chunked)) {
        CloseSocket(s);
        set_err("Keine Antwort vom IPFS-Daemon.");
        return -1;
    }
    body_cap = 65536UL + (unsigned long)max * 1024UL;
    body = (char *)AllocVec(body_cap, MEMF_ANY);
    if (!body) {
        CloseSocket(s);
        set_err("Kein Speicher fuer IPFS-Listing.");
        return -1;
    }
    if (!read_body_to_memory(&conn, status, length, chunked, body, body_cap)) {
        CloseSocket(s);
        FreeVec(body);
        return -1;
    }
    CloseSocket(s);

    p = strstr(body, "\"Links\":[");
    if (!p) {
        set_err("IPFS-Listing ohne Links.");
        FreeVec(body);
        return -1;
    }
    p += 9;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\r' || *p == '\n' || *p == ',') ++p;
        if (*p == ']') break;
        if (*p != '{') { ++p; continue; }
        end = json_object_end(p);
        if (!end) break;
        if (json_string_at(p, "Name", links[n].name, sizeof(links[n].name)) &&
            json_string_at(p, "Hash", links[n].cid, sizeof(links[n].cid))) {
            unsigned long value = 0;
            links[n].size = json_number_at(p, "Size", &value) ? value : 0;
            links[n].is_dir = json_number_at(p, "Type", &value) && value == 1;
            ++n;
        }
        p = end + 1;
    }
    FreeVec(body);
    set_err("");
    return n;
}

static int safe_entry_name(const char *name)
{
    const char *p;
    unsigned long n;
    if (!name || !*name || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    n = (unsigned long)strlen(name);
    if (n >= 200) return 0;
    for (p = name; *p; ++p)
        if (*p == '/' || *p == '\\' || *p == ':' || (unsigned char)*p < 32)
            return 0;
    return 1;
}

static int get_recursive(const char *cid, const char *dest, int depth)
{
    struct qrx_ipfs_link *links;
    int n, i, ok = 1;
    BPTR lock;

    if (!cid || !dest) return 0;
    if (depth > 32) {
        set_err("IPFS-Abruf zu tief verschachtelt.");
        return 0;
    }
    links = (struct qrx_ipfs_link *)AllocVec(
        sizeof(struct qrx_ipfs_link) * QRX_IPFS_MAX_LINKS, MEMF_ANY);
    if (!links) {
        set_err("Kein Speicher fuer IPFS-Abruf.");
        return 0;
    }
    n = qrx_ipfs_ls(cid, links, QRX_IPFS_MAX_LINKS);
    if (n < 0) {
        /* A raw file has no links; cat is the fallback. */
        FreeVec(links);
        return qrx_ipfs_cat(cid, dest);
    }

    lock = CreateDir((CONST_STRPTR)dest);
    if (lock) UnLock(lock);
    lock = Lock((CONST_STRPTR)dest, ACCESS_READ);
    if (!lock) {
        FreeVec(links);
        set_err("Zielordner nicht anlegbar.");
        return 0;
    }
    UnLock(lock);

    for (i = 0; i < n; ++i) {
        char child[420];
        int len;
        if (!safe_entry_name(links[i].name)) {
            set_err("Ungueltiger Name im IPFS-Verzeichnis.");
            ok = 0;
            break;
        }
        len = snprintf(child, sizeof(child), "%s/%s", dest, links[i].name);
        if (len < 0 || (unsigned long)len >= sizeof(child)) {
            set_err("IPFS-Zielpfad zu lang.");
            ok = 0;
            break;
        }
        if (links[i].is_dir) {
            if (!get_recursive(links[i].cid, child, depth + 1)) { ok = 0; break; }
        } else if (!qrx_ipfs_cat(links[i].cid, child)) {
            ok = 0;
            break;
        }
    }
    FreeVec(links);
    return ok;
}

int qrx_ipfs_get(const char *cid, const char *dest)
{
    return get_recursive(cid, dest, 0);
}

int qrx_ipfs_pin_add(const char *cid)
{
    char path[400], body[4096];
    int s, status, chunked;
    long length;
    struct http_conn conn;

    if (!cid) return 0;
    if (!qrx_ipfs_open()) return 0;
    snprintf(path, sizeof(path), "/api/v0/pin/add?arg=%s&recursive=true", cid);
    s = http_begin("POST", path, NULL, 0);
    if (s < 0) return 0;
    memset(&conn, 0, sizeof(conn));
    conn.s = s;
    if (!read_headers(&conn, &status, &length, &chunked)) { CloseSocket(s); return 0; }
    read_body_to_memory(&conn, status, length, chunked, body, sizeof(body));
    CloseSocket(s);
    return status == 200;
}

int qrx_ipfs_pin_ls(const char *cid)
{
    char path[400], body[8192];
    int s, status, chunked;
    long length;
    struct http_conn conn;

    if (!cid) return 0;
    if (!qrx_ipfs_open()) return 0;
    snprintf(path, sizeof(path), "/api/v0/pin/ls?arg=%s&type=recursive", cid);
    s = http_begin("POST", path, NULL, 0);
    if (s < 0) return 0;
    memset(&conn, 0, sizeof(conn));
    conn.s = s;
    if (!read_headers(&conn, &status, &length, &chunked)) { CloseSocket(s); return 0; }
    read_body_to_memory(&conn, status, length, chunked, body, sizeof(body));
    CloseSocket(s);
    return status == 200 && strstr(body, "\"Type\"") != NULL;
}

int qrx_ipfs_unpin(const char *cid)
{
    char path[400], body[2048];
    int s, status, chunked;
    long length;
    struct http_conn conn;

    if (!cid) return 0;
    if (!qrx_ipfs_open()) return 0;
    snprintf(path, sizeof(path), "/api/v0/pin/rm?arg=%s&recursive=true", cid);
    s = http_begin("POST", path, NULL, 0);
    if (s < 0) return 0;
    memset(&conn, 0, sizeof(conn));
    conn.s = s;
    if (!read_headers(&conn, &status, &length, &chunked)) { CloseSocket(s); return 0; }
    read_body_to_memory(&conn, status, length, chunked, body, sizeof(body));
    CloseSocket(s);
    return status == 200;
}

int qrx_ipfs_version(char *out, unsigned long cap)
{
    char body[4096];
    int s, status, chunked;
    long length;
    struct http_conn conn;

    if (!out || !cap || !qrx_ipfs_open()) return 0;
    out[0] = 0;
    s = http_begin("POST", "/api/v0/version", NULL, 0);
    if (s < 0) return 0;
    memset(&conn, 0, sizeof(conn));
    conn.s = s;
    if (!read_headers(&conn, &status, &length, &chunked)) { CloseSocket(s); return 0; }
    if (!read_body_to_memory(&conn, status, length, chunked, body, sizeof(body))) {
        CloseSocket(s);
        return 0;
    }
    CloseSocket(s);
    return json_last_string(body, "Version", out, cap);
}

int qrx_ipfs_repo(unsigned long *bytes, unsigned long *objects)
{
    char body[4096];
    int s, status, chunked;
    long length;
    struct http_conn conn;

    if (!qrx_ipfs_open()) return 0;
    s = http_begin("POST", "/api/v0/repo/stat", NULL, 0);
    if (s < 0) return 0;
    memset(&conn, 0, sizeof(conn));
    conn.s = s;
    if (!read_headers(&conn, &status, &length, &chunked)) { CloseSocket(s); return 0; }
    if (!read_body_to_memory(&conn, status, length, chunked, body, sizeof(body))) {
        CloseSocket(s);
        return 0;
    }
    CloseSocket(s);
    if (bytes) json_number_at(body, "RepoSize", bytes);
    if (objects) json_number_at(body, "NumObjects", objects);
    return status == 200;
}

int qrx_ipfs_status(char *agent, unsigned long agent_cap,
                    char *peer, unsigned long peer_cap)
{
    char body[8192];
    int s, status, chunked;
    long length;
    struct http_conn conn;

    if (!qrx_ipfs_open()) return 0;
    s = http_begin("POST", "/api/v0/id", NULL, 0);
    if (s < 0) return 0;
    memset(&conn, 0, sizeof(conn));
    conn.s = s;
    if (!read_headers(&conn, &status, &length, &chunked)) {
        CloseSocket(s);
        set_err("IPFS-Daemon antwortet nicht.");
        return 0;
    }
    if (!read_body_to_memory(&conn, status, length, chunked, body, sizeof(body))) {
        CloseSocket(s);
        return 0;
    }
    CloseSocket(s);
    if (agent && agent_cap) {
        if (!json_last_string(body, "AgentVersion", agent, agent_cap))
            strncpy(agent, "IPFS", agent_cap - 1), agent[agent_cap - 1] = 0;
    }
    if (peer && peer_cap) {
        if (!json_last_string(body, "ID", peer, peer_cap))
            strncpy(peer, "?", peer_cap - 1), peer[peer_cap - 1] = 0;
    }
    set_err("");
    return 1;
}

/* ------------------------------------------------------------------ */
/* revisions and volume sync                                           */
/* ------------------------------------------------------------------ */

static void stamp_now(char *out, unsigned long cap)
{
    struct DateTime dt;
    struct DateStamp ds;
    char d[LEN_DATSTRING], t[LEN_DATSTRING];

    DateStamp(&ds);
    memset(&dt, 0, sizeof(dt));
    dt.dat_Stamp = ds;
    dt.dat_Format = FORMAT_DOS;
    dt.dat_StrDate = (STRPTR)d;
    dt.dat_StrTime = (STRPTR)t;
    if (DateToStr(&dt))
        snprintf(out, cap, "%s %s", d, t);
    else
        snprintf(out, cap, "%ld.%ld", ds.ds_Days, ds.ds_Minute);
}

int qrx_ipfs_revision_append(const char *log_path, const char *object,
                             const char *cid, unsigned long bytes)
{
    BPTR f;
    char stamp[64];
    char line[420];

    if (!log_path || !object || !cid) return 0;
    stamp_now(stamp, sizeof(stamp));
    f = Open((CONST_STRPTR)log_path, MODE_OLDFILE);
    if (f) Close(f);
    else {
        f = Open((CONST_STRPTR)log_path, MODE_NEWFILE);
        if (f) Close(f);
    }
    f = Open((CONST_STRPTR)log_path, MODE_READWRITE);
    if (!f) return 0;
    Seek(f, 0, OFFSET_END);
    snprintf(line, sizeof(line), "%s  %s  %s  %lu\n", stamp, object, cid, bytes);
    FPuts(f, (CONST_STRPTR)line);
    Close(f);
    return 1;
}

long qrx_ipfs_revision_tail(const char *log_path, char *out, unsigned long cap)
{
    BPTR f;
    long size, start, got;

    if (!out || cap < 2) return 0;
    out[0] = 0;
    f = Open((CONST_STRPTR)log_path, MODE_OLDFILE);
    if (!f) return 0;
    Seek(f, 0, OFFSET_END);
    size = Seek(f, 0, OFFSET_CURRENT);
    start = size > (long)cap - 1 ? size - (long)cap + 1 : 0;
    Seek(f, start, OFFSET_BEGINNING);
    got = Read(f, out, (LONG)cap - 1);
    Close(f);
    if (got < 0) got = 0;
    out[got] = 0;
    return got;
}

struct sync_entry {
    char path[180];
    unsigned long size;
    long days, mins, ticks;
};

static int state_load(const char *file, struct sync_entry *tab, int max)
{
    BPTR f;
    char line[300];
    int n = 0;

    f = Open((CONST_STRPTR)file, MODE_OLDFILE);
    if (!f) return 0;
    while (n < max && FGets(f, (STRPTR)line, sizeof(line))) {
        char *p = line;
        char *tab1, *tab2, *tab3, *tab4;
        if (!*p || *p == '#') continue;
        tab1 = strchr(p, '\t'); if (!tab1) continue;
        *tab1++ = 0;
        tab2 = strchr(tab1, '\t'); if (!tab2) continue;
        *tab2++ = 0;
        tab3 = strchr(tab2, '\t'); if (!tab3) continue;
        *tab3++ = 0;
        tab4 = strchr(tab3, '\t');
        if (tab4) *tab4++ = 0;
        strncpy(tab[n].path, p, sizeof(tab[n].path) - 1);
        tab[n].path[sizeof(tab[n].path) - 1] = 0;
        tab[n].days = atol(tab1);
        tab[n].mins = atol(tab2);
        tab[n].ticks = atol(tab3);
        tab[n].size = tab4 ? (unsigned long)atol(tab4) : 0;
        ++n;
    }
    Close(f);
    return n;
}

static struct sync_entry *state_find(struct sync_entry *tab, int n, const char *path)
{
    int i;
    for (i = 0; i < n; ++i)
        if (!strcmp(tab[i].path, path)) return &tab[i];
    return NULL;
}

static int sync_walk(const char *root, const char *rel,
                     struct sync_entry *tab, int *ntab, int max,
                     const char *log, int publish_all, int *published)
{
    char full[420];
    BPTR lock;
    struct FileInfoBlock *fib;
    int ok = 1;

    {
        unsigned long rl = (unsigned long)strlen(root);
        int sep = *rel && rl && root[rl - 1] != ':' && root[rl - 1] != '/';
        snprintf(full, sizeof(full), "%s%s%s", root, sep ? "/" : "", rel);
    }
    lock = Lock((CONST_STRPTR)full, ACCESS_READ);
    if (!lock) return 0;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) { UnLock(lock); return 0; }
    if (!Examine(lock, fib)) { FreeDosObject(DOS_FIB, fib); UnLock(lock); return 0; }

    if (fib->fib_DirEntryType > 0) {
        while (ExNext(lock, fib)) {
            char child[180];
            if (fib->fib_FileName[0] == '.') continue;
            if (!strcmp((char *)fib->fib_FileName, "REVISIONS")) continue;
            snprintf(child, sizeof(child), "%s%s%s", rel, *rel ? "/" : "",
                     (char *)fib->fib_FileName);
            if (!sync_walk(root, child, tab, ntab, max, log, publish_all, published)) {
                ok = 0;
                break;
            }
        }
    } else {
        struct sync_entry *e = state_find(tab, *ntab, rel);
        int changed = publish_all || !e ||
                      e->days != fib->fib_Date.ds_Days ||
                      e->mins != fib->fib_Date.ds_Minute ||
                      e->ticks != fib->fib_Date.ds_Tick ||
                      e->size != (unsigned long)fib->fib_Size;
        if (changed) {
            char cid[128];
            if (qrx_ipfs_add(full, 0, 1, cid, sizeof(cid))) {
                qrx_ipfs_revision_append(log, rel, cid, (unsigned long)fib->fib_Size);
                if (!e) {
                    if (*ntab < max) {
                        e = &tab[*ntab];
                        strncpy(e->path, rel, sizeof(e->path) - 1);
                        e->path[sizeof(e->path) - 1] = 0;
                        ++*ntab;
                    }
                }
                if (e) {
                    e->days = fib->fib_Date.ds_Days;
                    e->mins = fib->fib_Date.ds_Minute;
                    e->ticks = fib->fib_Date.ds_Tick;
                    e->size = (unsigned long)fib->fib_Size;
                }
                ++*published;
            } else {
                ok = 0;
            }
        }
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return ok;
}

int qrx_ipfs_volume_sync(const char *root, const char *log_path, int publish_all)
{
    struct sync_entry *tab;
    int ntab = 0, published = 0;
    BPTR f;

    if (!root || !log_path) return -1;
    if (!qrx_ipfs_open()) return -1;
    tab = (struct sync_entry *)AllocVec(sizeof(struct sync_entry) * 400, MEMF_ANY | MEMF_CLEAR);
    if (!tab) { set_err("Kein Speicher fuer Sync-Status."); return -1; }

    ntab = state_load(QRX_IPFS_STATE_FILE, tab, 400);
    if (!sync_walk(root, "", tab, &ntab, 400, log_path, publish_all, &published)) {
        FreeVec(tab);
        return -1;
    }
    f = Open((CONST_STRPTR)QRX_IPFS_STATE_FILE, MODE_NEWFILE);
    if (f) {
        int i;
        for (i = 0; i < ntab; ++i)
            FPrintf(f, (CONST_STRPTR)"%s\t%ld\t%ld\t%ld\t%lu\n", tab[i].path,
                    tab[i].days, tab[i].mins, tab[i].ticks, tab[i].size);
        Close(f);
    }
    FreeVec(tab);

    if (publish_all) {
        char cid[128];
        if (qrx_ipfs_add(root, 1, 1, cid, sizeof(cid))) {
            char label[200];
            snprintf(label, sizeof(label), "(volume) %s", root);
            qrx_ipfs_revision_append(log_path, label, cid, 0);
        }
    }
    set_err("");
    return published;
}
