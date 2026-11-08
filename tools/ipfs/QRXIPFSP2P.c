/*
 * QRXIPFSP2P.c - the guest-side libp2p peer process.
 *
 * It shares the blockstore of QRXIPFSD (SYS:Storage/IPFS/.qrx-node) but
 * runs its own TCP listener on port 4001.  The control endpoint on
 * 127.0.0.1:5002 is intentionally separate from the Kubo-compatible
 * node API; QRXIPFSD can use it to fetch a missing block without
 * exposing the peer control port to the network.
 */
#include "qrx_p2p.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "qrx_net.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define NODE_DIR       "SYS:Storage/IPFS/.qrx-node"
#define NODE_BLOCKS    NODE_DIR "/blocks"
#define NODE_PINS      NODE_DIR "/pins"
#define P2P_BOOTSTRAP  NODE_DIR "/bootstrap"
#define P2P_STATUS     NODE_DIR "/p2p-status"
#define P2P_CONTROL    5002
#define P2P_MAX_BLOCK  (16UL * 1024UL * 1024UL)

static int ensure_dir(const char *path)
{
    BPTR l = CreateDir((CONST_STRPTR)path);
    if (l) UnLock(l);
    l = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (!l) return 0;
    UnLock(l);
    return 1;
}

/* The desktop cannot speak HTTP from inside the file manager.  Keep a tiny
 * status file beside the node so QRX Drive can show whether the P2P process
 * is alive without linking the network stack into the desktop. */
static void write_p2p_status(struct qrx_p2p *p2p)
{
    BPTR f;
    if (!p2p) return;
    f = Open((CONST_STRPTR)P2P_STATUS, MODE_NEWFILE);
    if (!f) return;
    FPrintf(f, "online=1\npeers=%d\npeer_id=%s\n",
            qrx_p2p_peer_count(p2p), qrx_p2p_peer_id(p2p));
    Close(f);
}

static void block_path(const char *cid, char *out, int cap)
{
    snprintf(out, cap, "%s/%s", NODE_BLOCKS, cid);
}

static int safe_cid(const char *cid)
{
    const char *p;
    if (!cid || !*cid || strlen(cid) > 90) return 0;
    for (p = cid; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return 0;
    }
    return 1;
}

static int be_has(const char *cid)
{
    char path[420];
    BPTR l;
    if (!safe_cid(cid)) return 0;
    block_path(cid, path, sizeof(path));
    l = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (l) UnLock(l);
    return l != 0;
}

static int be_get(const char *cid, unsigned char **data, unsigned long *len)
{
    char path[420];
    BPTR f;
    LONG size;
    unsigned char *buf;
    if (!safe_cid(cid) || !data || !len) return 0;
    block_path(cid, path, sizeof(path));
    f = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!f) return 0;
    Seek(f, 0, OFFSET_END);
    size = Seek(f, 0, OFFSET_CURRENT);
    if (size < 0 || (unsigned long)size > P2P_MAX_BLOCK) { Close(f); return 0; }
    buf = AllocVec((ULONG)(size ? size : 1), MEMF_ANY);
    if (!buf) { Close(f); return 0; }
    Seek(f, 0, OFFSET_BEGINNING);
    if (size && Read(f, buf, size) != size) {
        FreeVec(buf);
        Close(f);
        return 0;
    }
    Close(f);
    *data = buf;
    *len = (unsigned long)size;
    return 1;
}

static int be_put(const char *cid, const unsigned char *data, unsigned long len)
{
    char path[420], tmp[440];
    BPTR f;
    if (!safe_cid(cid) || len > P2P_MAX_BLOCK) return 0;
    if (be_has(cid)) return 1;
    block_path(cid, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s/.tmp", NODE_BLOCKS);
    f = Open((CONST_STRPTR)tmp, MODE_NEWFILE);
    if (!f) return 0;
    if (len && Write(f, (APTR)data, (LONG)len) != (LONG)len) {
        Close(f);
        DeleteFile((CONST_STRPTR)tmp);
        return 0;
    }
    Close(f);
    if (!Rename((CONST_STRPTR)tmp, (CONST_STRPTR)path)) {
        DeleteFile((CONST_STRPTR)tmp);
        return 0;
    }
    return 1;
}

static int be_pins(char *cids, int max)
{
    BPTR f;
    char line[128];
    int n = 0, i;
    f = Open((CONST_STRPTR)NODE_PINS, MODE_OLDFILE);
    if (!f) return 0;
    while (FGets(f, (STRPTR)line, sizeof(line))) {
        char *e = line;
        while (*e && *e != '\n' && *e != '\r') ++e;
        *e = 0;
        if (line[0] == '-') {
            for (i = 0; i < n; ++i) {
                if (!strcmp(cids + (i * QRX_P2P_MAX_CID), line + 1)) {
                    if (i != n - 1)
                        strcpy(cids + (i * QRX_P2P_MAX_CID),
                               cids + ((n - 1) * QRX_P2P_MAX_CID));
                    --n;
                    break;
                }
            }
            continue;
        }
        if (!safe_cid(line)) continue;
        for (i = 0; i < n; ++i)
            if (!strcmp(cids + (i * QRX_P2P_MAX_CID), line)) break;
        if (i != n || n >= max) continue;
        strncpy(cids + (n * QRX_P2P_MAX_CID), line, QRX_P2P_MAX_CID - 1);
        cids[n * QRX_P2P_MAX_CID + QRX_P2P_MAX_CID - 1] = 0;
        ++n;
    }
    Close(f);
    return n;
}

static int send_all(int fd, const void *data, unsigned long len)
{
    const unsigned char *p = (const unsigned char *)data;
    while (len) {
        LONG n = send(fd, p, (LONG)len, 0);
        if (n <= 0) return 0;
        p += n;
        len -= (unsigned long)n;
    }
    return 1;
}

static void http_headers(int fd, const char *status, const char *type,
                         unsigned long len)
{
    char h[320];
    snprintf(h, sizeof(h),
             "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
             "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
             status, type, len);
    send_all(fd, h, (unsigned long)strlen(h));
}

static int url_decode(const char *in, char *out, int cap)
{
    int n = 0;
    while (*in && n + 1 < cap) {
        if (*in == '%' && in[1] && in[2]) {
            int a = in[1], b = in[2], hi, lo;
            if (a >= '0' && a <= '9') hi = a - '0';
            else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
            else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
            else return 0;
            if (b >= '0' && b <= '9') lo = b - '0';
            else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
            else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
            else return 0;
            out[n++] = (char)((hi << 4) | lo);
            in += 3;
        } else {
            out[n++] = *in++;
        }
    }
    out[n] = 0;
    return n > 0;
}

static void control_request(struct qrx_p2p *p2p, int fd)
{
    char req[2048], target[1024], arg[256], decoded[256];
    int n;
    char *p, *q;
    n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) return;
    req[n] = 0;
    if (strncmp(req, "GET ", 4)) {
        http_headers(fd, "405 Method Not Allowed", "text/plain", 0);
        return;
    }
    p = req + 4;
    q = strchr(p, ' ');
    if (!q) return;
    *q = 0;
    strncpy(target, p, sizeof(target) - 1);
    target[sizeof(target) - 1] = 0;
    printf("control: %s\n", target);
    fflush(stdout);
    if (!strncmp(target, "/status", 7)) {
        char json[512];
        qrx_p2p_status_json(p2p, json, sizeof(json));
        http_headers(fd, "200 OK", "application/json", (unsigned long)strlen(json));
        send_all(fd, json, (unsigned long)strlen(json));
        return;
    }
    if (!strncmp(target, "/fetch?arg=", 11)) {
        unsigned char *data = NULL;
        unsigned long len = 0;
        char cid[96];
        if (!url_decode(target + 11, cid, sizeof(cid)) || !safe_cid(cid)) {
            http_headers(fd, "400 Bad Request", "text/plain", 0);
            return;
        }
        if (!qrx_p2p_fetch(p2p, cid, &data, &len)) {
            http_headers(fd, "404 Not Found", "text/plain", 0);
            return;
        }
        write_p2p_status(p2p);
        http_headers(fd, "200 OK", "application/octet-stream", len);
        send_all(fd, data, len);
        FreeVec(data);
        return;
    }
    if (!strncmp(target, "/provide?arg=", 13)) {
        char cid[96];
        if (!url_decode(target + 13, cid, sizeof(cid)) || !safe_cid(cid)) {
            http_headers(fd, "400 Bad Request", "text/plain", 0);
            return;
        }
        qrx_p2p_provide(p2p, cid);
        write_p2p_status(p2p);
        http_headers(fd, "200 OK", "text/plain", 2);
        send_all(fd, "ok", 2);
        return;
    }
    http_headers(fd, "404 Not Found", "text/plain", 0);
    (void)arg;
    (void)decoded;
}

int main(void)
{
    struct qrx_p2p_backend be;
    struct qrx_p2p *p2p;
    struct sockaddr_in sa;
    char err[160];
    int cs, ls, one = 1, ticks = 0;

    if (!ensure_dir("SYS:Storage") || !ensure_dir("SYS:Storage/IPFS") ||
        !ensure_dir(NODE_DIR) || !ensure_dir(NODE_BLOCKS)) {
        printf("QRXIPFSP2P: cannot create node directories\n");
        return 10;
    }
    DeleteFile((CONST_STRPTR)P2P_STATUS);
    while (!qrx_net_init()) {
        printf("QRXIPFSP2P: waiting for QRX host-socket door...\n");
        Delay(250);
    }

    be.block_get = be_get;
    be.block_put = be_put;
    be.block_has = be_has;
    be.pin_list = be_pins;
    p2p = qrx_p2p_new(NODE_DIR, 4001, &be, err, sizeof(err));
    if (!p2p) {
        printf("QRXIPFSP2P: %s\n", err);
        return 11;
    }
    qrx_p2p_load_bootstrap(p2p, P2P_BOOTSTRAP);
    if (!qrx_p2p_listen(p2p)) {
        printf("QRXIPFSP2P: cannot bind TCP port 4001\n");
        qrx_p2p_free(p2p);
        return 12;
    }

    cs = socket(AF_INET, SOCK_STREAM, 0);
    if (cs < 0) {
        printf("QRXIPFSP2P: no control socket\n");
        qrx_p2p_free(p2p);
        return 13;
    }
    setsockopt(cs, SOL_SOCKET, SO_REUSEADDR, (void *)&one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(P2P_CONTROL);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(cs, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(cs, 8) < 0) {
        printf("QRXIPFSP2P: cannot bind control port 5002\n");
        CloseSocket(cs);
        qrx_p2p_free(p2p);
        return 14;
    }

    printf("QRXIPFSP2P %s (trace-b)\n", qrx_p2p_peer_id(p2p));
    printf("P2P:     tcp/%d\n", qrx_p2p_listen_port(p2p));
    printf("Control: http://127.0.0.1:%d/status\n", P2P_CONTROL);
    fflush(stdout);
    write_p2p_status(p2p);

    for (;;) {
        int peer_fd = qrx_p2p_listen_fd(p2p);
        if (qrx_net_poll(peer_fd, 1, NULL)) {
            int peer = qrx_net_accept_nb(peer_fd, NULL);
            if (peer >= 0) qrx_p2p_serve_fd(p2p, peer);
        }
        if (qrx_net_poll(cs, 1, NULL)) {
            int ctrl = qrx_net_accept_nb(cs, NULL);
            if (ctrl >= 0) {
                control_request(p2p, ctrl);
                CloseSocket(ctrl);
            }
        }
        Delay(20);
        if (++ticks >= 300) {
            ticks = 0;
            write_p2p_status(p2p);
            qrx_p2p_tick(p2p);
        }
    }
    return 0;
}
