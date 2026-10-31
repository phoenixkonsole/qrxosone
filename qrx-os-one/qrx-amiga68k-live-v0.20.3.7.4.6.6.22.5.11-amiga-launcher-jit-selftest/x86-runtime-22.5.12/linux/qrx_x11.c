/* QRX X11 core subset. Wire integers are decoded explicitly, including on m68k.
 * Per-connection resource namespace; bounded buffers; unknown requests produce
 * BadRequest. Only local display :0 sockets in qrx-i386 reach this server.
 * See docs/QRX_X11.md for the intentionally limited compatibility surface. */
#include "qrx_x11.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define QXX_LIMIT (1024u * 1024u)
#define QXX_WINDOWS 32
#define QXX_GCS 64
#define QXX_ROOT 1u
#define QXX_VISUAL 3u
#define QXX_BASE 0x200000u

typedef struct {
    uint32_t id, mask, bg;
    int x, y, w, h, mapped;
    uint32_t *pixels;
    void *native;
    char title[128];
} qxx_window;
typedef struct {
    uint32_t id, fg, bg;
} qxx_gc;
struct qxx {
    unsigned char *in, *out;
    size_t ni, no;
    int setup, be, failed;
    uint16_t sequence;
    qxx_window windows[QXX_WINDOWS];
    qxx_gc gcs[QXX_GCS];
    char atoms[64][128];
    unsigned natoms;
};
static uint16_t r16(qxx *x, const unsigned char *p) {
    return x->be ? (uint16_t)(p[0] * 256u + p[1]) : (uint16_t)(p[1] * 256u + p[0]);
}
static uint32_t r32(qxx *x, const unsigned char *p) {
    return x->be ? ((uint32_t)r16(x, p) << 16) | r16(x, p + 2)
                 : ((uint32_t)r16(x, p + 2) << 16) | r16(x, p);
}
static void w16(qxx *x, unsigned char *p, uint16_t v) {
    p[x->be ? 1 : 0] = (unsigned char)v;
    p[x->be ? 0 : 1] = (unsigned char)(v >> 8);
}
static void w32(qxx *x, unsigned char *p, uint32_t v) {
    w16(x, p + (x->be ? 2 : 0), (uint16_t)v);
    w16(x, p + (x->be ? 0 : 2), (uint16_t)(v >> 16));
}
static unsigned bits(uint32_t m) {
    unsigned n = 0;
    while (m) {
        n += m & 1;
        m >>= 1;
    }
    return n;
}
static void queue(qxx *x, const void *p, size_t n) {
    if (n > QXX_LIMIT - x->no) {
        x->failed = 1;
        return;
    }
    memcpy(x->out + x->no, p, n);
    x->no += n;
}
static void reply(qxx *x, unsigned char *p, size_t n) {
    p[0] = 1;
    w16(x, p + 2, x->sequence);
    w32(x, p + 4, (uint32_t)((n - 32) / 4));
    queue(x, p, n);
}
static void error(qxx *x, int code, uint32_t id, int op) {
    unsigned char p[32] = {0};
    p[1] = (unsigned char)code;
    w16(x, p + 2, x->sequence);
    w32(x, p + 4, id);
    p[10] = (unsigned char)op;
    queue(x, p, 32);
}
static qxx_window *window(qxx *x, uint32_t id) {
    unsigned i;
    for (i = 0; i < QXX_WINDOWS; i++)
        if (x->windows[i].id == id && id)
            return &x->windows[i];
    return NULL;
}
static qxx_gc *gc(qxx *x, uint32_t id) {
    unsigned i;
    for (i = 0; i < QXX_GCS; i++)
        if (x->gcs[i].id == id && id)
            return &x->gcs[i];
    return NULL;
}
static int newid(qxx *x, uint32_t id) {
    return (id & ~0x1fffffu) == QXX_BASE && !window(x, id) && !gc(x, id);
}
static void event(qxx *x, qxx_window *w, int type, int detail, int px, int py) {
    unsigned char p[32] = {0};
    p[0] = (unsigned char)type;
    p[1] = (unsigned char)detail;
    w16(x, p + 2, x->sequence);
    if (type == 12) {
        w32(x, p + 4, w->id);
        w16(x, p + 12, (uint16_t)w->w);
        w16(x, p + 14, (uint16_t)w->h);
    } else if (type == 19 || type == 17) {
        w32(x, p + 4, w->id);
        w32(x, p + 8, w->id);
    } else {
        w32(x, p + 8, QXX_ROOT);
        w32(x, p + 12, w->id);
        w16(x, p + 20, (uint16_t)(px + w->x));
        w16(x, p + 22, (uint16_t)(py + w->y));
        w16(x, p + 24, (uint16_t)px);
        w16(x, p + 26, (uint16_t)py);
        p[30] = 1;
    }
    queue(x, p, 32);
}
#ifdef __AROS__
#include "qrx_x11_aros.h"
#else
static int native_open(qxx_window *w) {
    (void)w;
    return 1;
}
static void native_close(qxx_window *w) { (void)w; }
static void native_title(qxx_window *w) { (void)w; }
static void native_present(qxx_window *w) {
    const char *dir = getenv("QRX_X11_DUMP_DIR");
    char path[1024];
    FILE *f;
    int i;
    if (!dir || !w->mapped)
        return;
    snprintf(path, sizeof(path), "%s/window-%08x.ppm", dir, w->id);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", w->w, w->h);
    for (i = 0; i < w->w * w->h; i++) {
        unsigned char rgb[3] = {(unsigned char)(w->pixels[i] >> 16),
                                (unsigned char)(w->pixels[i] >> 8), (unsigned char)w->pixels[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
void qxx_pump(qxx *x) { (void)x; }
#endif
static void rect(qxx_window *w, int px, int py, unsigned width, unsigned height, uint32_t color) {
    int x0 = px < 0 ? 0 : px, y0 = py < 0 ? 0 : py, x1 = px + (int)width, y1 = py + (int)height, x,
        y;
    if (x1 > w->w)
        x1 = w->w;
    if (y1 > w->h)
        y1 = w->h;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            w->pixels[y * w->w + x] = color & 0xffffffu;
}
static int attributes(qxx *x, qxx_window *w, const unsigned char *p, uint32_t mask, size_t n) {
    unsigned i;
    if (mask & ~((1u << 1) | (1u << 3) | (1u << 9) | (1u << 11) | (1u << 13)))
        return 2;
    if (n != bits(mask) * 4u)
        return 16;
    for (i = 0; i < 15; i++)
        if (mask & (1u << i)) {
            uint32_t v = r32(x, p);
            p += 4;
            if (i == 1)
                w->bg = v;
            else if (i == 11)
                w->mask = v;
        }
    return 0;
}
static void request(qxx *x, const unsigned char *p, size_t n) {
    unsigned op = p[0], i;
    uint32_t id = n >= 8 ? r32(x, p + 4) : 0;
    unsigned char b[1024] = {0};
    qxx_window *w;
    qxx_gc *g;
#define NEED(N)                                                                                    \
    do {                                                                                           \
        if (n < (N)) {                                                                             \
            error(x, 16, 0, op);                                                                   \
            return;                                                                                \
        }                                                                                          \
    } while (0)
#define WIN(ID)                                                                                    \
    do {                                                                                           \
        w = window(x, (ID));                                                                       \
        if (!w) {                                                                                  \
            error(x, 3, (ID), op);                                                                 \
            return;                                                                                \
        }                                                                                          \
    } while (0)
#define DRAW(ID)                                                                                   \
    do {                                                                                           \
        w = window(x, (ID));                                                                       \
        if (!w) {                                                                                  \
            error(x, 9, (ID), op);                                                                 \
            return;                                                                                \
        }                                                                                          \
    } while (0)
#define GC(ID)                                                                                     \
    do {                                                                                           \
        g = gc(x, (ID));                                                                           \
        if (!g) {                                                                                  \
            error(x, 13, (ID), op);                                                                \
            return;                                                                                \
        }                                                                                          \
    } while (0)
    switch (op) {
    case 1: {
        uint32_t mask;
        int e;
        NEED(32);
        if (!newid(x, id)) {
            error(x, 14, id, op);
            return;
        }
        if (r32(x, p + 8) != QXX_ROOT || (p[1] != 0 && p[1] != 24) ||
            (r16(x, p + 22) != 0 && r16(x, p + 22) != 1) ||
            (r32(x, p + 24) != 0 && r32(x, p + 24) != QXX_VISUAL)) {
            error(x, 8, id, op);
            return;
        }
        if (!r16(x, p + 16) || !r16(x, p + 18) || r16(x, p + 16) > 1024 || r16(x, p + 18) > 768) {
            error(x, 2, id, op);
            return;
        }
        for (i = 0; i < QXX_WINDOWS && x->windows[i].id; i++) {
        }
        if (i == QXX_WINDOWS) {
            error(x, 11, id, op);
            return;
        }
        w = &x->windows[i];
        memset(w, 0, sizeof(*w));
        mask = r32(x, p + 28);
        e = attributes(x, w, p + 32, mask, n - 32);
        if (e) {
            error(x, e, id, op);
            return;
        }
        w->w = r16(x, p + 16);
        w->h = r16(x, p + 18);
        w->x = (int16_t)r16(x, p + 12);
        w->y = (int16_t)r16(x, p + 14);
        w->pixels = calloc((size_t)w->w * w->h, 4);
        if (!w->pixels) {
            error(x, 11, id, op);
            return;
        }
        w->id = id;
        strcpy(w->title, "QRX Linux X11");
        rect(w, 0, 0, w->w, w->h, w->bg);
        break;
    }
    case 2: {
        int e;
        NEED(12);
        WIN(id);
        e = attributes(x, w, p + 12, r32(x, p + 8), n - 12);
        if (e)
            error(x, e, id, op);
        break;
    }
    case 3:
        NEED(8);
        if (id != QXX_ROOT) {
            WIN(id);
        } else
            w = NULL;
        b[1] = 0;
        w32(x, b + 8, QXX_VISUAL);
        w16(x, b + 12, 1);
        b[26] = w && w->mapped ? 2 : 0;
        w32(x, b + 28, 2);
        w32(x, b + 32, w ? w->mask : 0);
        w32(x, b + 36, w ? w->mask : 0);
        reply(x, b, 44);
        break;
    case 4:
        NEED(8);
        WIN(id);
        native_close(w);
        free(w->pixels);
        memset(w, 0, sizeof(*w));
        break;
    case 8:
        NEED(8);
        WIN(id);
        if (!w->mapped) {
            if (!native_open(w)) {
                error(x, 11, id, op);
                break;
            }
            w->mapped = 1;
            native_present(w);
            if (w->mask & (1u << 17))
                event(x, w, 19, 0, 0, 0);
            if (w->mask & (1u << 15))
                event(x, w, 12, 0, 0, 0);
        }
        break;
    case 10:
        NEED(8);
        WIN(id);
        native_close(w);
        w->mapped = 0;
        break;
    case 14:
        NEED(8);
        if (id != QXX_ROOT) {
            WIN(id);
        } else
            w = NULL;
        b[1] = 24;
        w32(x, b + 8, QXX_ROOT);
        w16(x, b + 12, w ? w->x : 0);
        w16(x, b + 14, w ? w->y : 0);
        w16(x, b + 16, w ? w->w : 1024);
        w16(x, b + 18, w ? w->h : 768);
        reply(x, b, 32);
        break;
    case 15:
        NEED(8);
        if (id != QXX_ROOT) {
            WIN(id);
        }
        w32(x, b + 8, QXX_ROOT);
        w32(x, b + 12, id == QXX_ROOT ? 0 : QXX_ROOT);
        if (id == QXX_ROOT) {
            unsigned k = 0;
            for (i = 0; i < QXX_WINDOWS; i++)
                if (x->windows[i].id)
                    w32(x, b + 32 + 4 * k++, x->windows[i].id);
            w16(x, b + 16, k);
            reply(x, b, 32 + 4 * k);
        } else
            reply(x, b, 32);
        break;
    case 16: {
        unsigned len;
        uint32_t atom = 0;
        NEED(8);
        len = r16(x, p + 4);
        if (len >= 128 || n != 8 + ((len + 3u) & ~3u)) {
            error(x, 16, 0, op);
            break;
        }
        if (len == 7 && !memcmp(p + 8, "WM_NAME", 7))
            atom = 39;
        else if (len == 6 && !memcmp(p + 8, "STRING", 6))
            atom = 31;
        else {
            for (i = 0; i < x->natoms; i++)
                if (strlen(x->atoms[i]) == len && !memcmp(x->atoms[i], p + 8, len)) {
                    atom = 256 + i;
                    break;
                }
            if (!atom && !p[1]) {
                if (x->natoms == 64) {
                    error(x, 11, 0, op);
                    break;
                }
                memcpy(x->atoms[x->natoms], p + 8, len);
                atom = 256 + x->natoms++;
            }
        }
        w32(x, b + 8, atom);
        reply(x, b, 32);
        break;
    }
    case 18: {
        unsigned len;
        NEED(24);
        WIN(id);
        len = r32(x, p + 20);
        if (p[16] != 8 || len > n - 24 || n != 24 + ((len + 3u) & ~3u)) {
            error(x, 8, id, op);
            break;
        }
        if (r32(x, p + 8) != 39 || r32(x, p + 12) != 31 || p[1] != 0) {
            error(x, 8, id, op);
            break;
        }
        if (len >= sizeof(w->title))
            len = sizeof(w->title) - 1;
        memcpy(w->title, p + 24, len);
        w->title[len] = 0;
        native_title(w);
        break;
    }
    case 20: {
        size_t len, off, take;
        NEED(24);
        WIN(id);
        if (r32(x, p + 8) != 39) {
            reply(x, b, 32);
            break;
        }
        len = strlen(w->title);
        if (r32(x, p + 16) > len / 4) {
            error(x, 2, 0, op);
            break;
        }
        off = (size_t)r32(x, p + 16) * 4;
        if (off > len) {
            error(x, 2, 0, op);
            break;
        }
        w32(x, b + 8, 31);
        b[1] = 8;
        if (r32(x, p + 12) && r32(x, p + 12) != 31) {
            w32(x, b + 12, (uint32_t)len);
            reply(x, b, 32);
            break;
        }
        take = len - off;
        if (r32(x, p + 20) < (take + 3) / 4)
            take = (size_t)r32(x, p + 20) * 4;
        w32(x, b + 12, (uint32_t)(len - off - take));
        w32(x, b + 16, (uint32_t)take);
        memcpy(b + 32, w->title + off, take);
        reply(x, b, 32 + ((take + 3) & ~(size_t)3));
        if (p[1] && off + take == len) {
            w->title[0] = 0;
            native_title(w);
        }
        break;
    }
    case 38:
        NEED(8);
        if (id != QXX_ROOT) {
            WIN(id);
        }
        b[1] = 1;
        w32(x, b + 8, QXX_ROOT);
        reply(x, b, 32);
        break;
    case 43:
        NEED(4);
        w32(x, b + 8, QXX_ROOT);
        reply(x, b, 32);
        break;
    case 55:
    case 56: {
        uint32_t mask;
        const unsigned char *v;
        NEED(op == 55 ? 16 : 12);
        if (op == 55) {
            if (!newid(x, id)) {
                error(x, 14, id, op);
                break;
            }
            if (r32(x, p + 8) != QXX_ROOT) {
                WIN(r32(x, p + 8));
            }
            for (i = 0; i < QXX_GCS && x->gcs[i].id; i++)
                ;
            if (i == QXX_GCS) {
                error(x, 11, id, op);
                break;
            }
            g = &x->gcs[i];
            mask = r32(x, p + 12);
            v = p + 16;
        } else {
            GC(id);
            mask = r32(x, p + 8);
            v = p + 12;
        }
        if (n != (size_t)(v - p) + 4 * bits(mask)) {
            error(x, 16, id, op);
            break;
        }
        if (mask & ~((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 16))) {
            error(x, 2, id, op);
            break;
        }
        {
            const unsigned char *a = v;
            for (i = 0; i < 23; i++)
                if (mask & (1u << i)) {
                    uint32_t val = r32(x, a);
                    a += 4;
                    if ((i == 0 && val != 3) || (i == 1 && val != 0xffffffffu)) {
                        error(x, 2, val, op);
                        return;
                    }
                }
        }
        if (op == 55) {
            g->id = id;
            g->fg = 0;
            g->bg = 0xffffff;
        }
        for (i = 0; i < 23; i++)
            if (mask & (1u << i)) {
                uint32_t val = r32(x, v);
                v += 4;
                if (i == 2)
                    g->fg = val;
                else if (i == 3)
                    g->bg = val;
            }
        break;
    }
    case 60:
        NEED(8);
        GC(id);
        memset(g, 0, sizeof(*g));
        break;
    case 61:
        NEED(16);
        WIN(id);
        rect(w, (int16_t)r16(x, p + 8), (int16_t)r16(x, p + 10),
             r16(x, p + 12) ? r16(x, p + 12) : w->w, r16(x, p + 14) ? r16(x, p + 14) : w->h, w->bg);
        native_present(w);
        if (p[1] && (w->mask & (1u << 15)))
            event(x, w, 12, 0, 0, 0);
        break;
    case 70:
        NEED(12);
        DRAW(id);
        GC(r32(x, p + 8));
        if ((n - 12) % 8) {
            error(x, 16, id, op);
            break;
        }
        for (i = 12; i < n; i += 8)
            rect(w, (int16_t)r16(x, p + i), (int16_t)r16(x, p + i + 2), r16(x, p + i + 4),
                 r16(x, p + i + 6), g->fg);
        native_present(w);
        break;
    case 72: {
        unsigned width, height, row, col;
        int dx, dy;
        NEED(24);
        DRAW(id);
        GC(r32(x, p + 8));
        width = r16(x, p + 12);
        height = r16(x, p + 14);
        dx = (int16_t)r16(x, p + 16);
        dy = (int16_t)r16(x, p + 18);
        if (p[1] != 2 || p[20] || p[21] != 24) {
            error(x, 8, id, op);
            break;
        }
        if (width > 1024 || height > 768 || n != 24 + (size_t)width * height * 4) {
            error(x, 16, id, op);
            break;
        }
        for (row = 0; row < height; row++)
            for (col = 0; col < width; col++) {
                int xx = dx + (int)col, yy = dy + (int)row;
                if (xx >= 0 && yy >= 0 && xx < w->w && yy < w->h)
                    w->pixels[yy * w->w + xx] = r32(x, p + 24 + 4 * (row * width + col)) & 0xffffff;
            }
        native_present(w);
        break;
    }
    case 98:
        NEED(8);
        if (n != 8 + ((r16(x, p + 4) + 3u) & ~3u)) {
            error(x, 16, 0, op);
            break;
        }
        reply(x, b, 32);
        break; /* no extensions */
    case 99:
        NEED(4);
        reply(x, b, 32);
        break;
    case 101: {
        unsigned first, count;
        NEED(8);
        first = p[4];
        count = p[5];
        if (first < 8 || !count || first + count > 256) {
            error(x, 2, 0, op);
            break;
        }
        b[1] = 1;
        for (i = 0; i < count; i++)
            w32(x, b + 32 + 4 * i, first + i);
        reply(x, b, 32 + 4 * count);
        break;
    }
    case 127:
        break;
    default:
        error(x, 1, 0, op);
        break;
    }
#undef NEED
#undef WIN
#undef DRAW
#undef GC
}
static void setup(qxx *x) {
    unsigned char b[128] = {0};
    b[0] = 1;
    w16(x, b + 2, 11);
    w16(x, b + 6, 30);
    w32(x, b + 8, 1);
    w32(x, b + 12, QXX_BASE);
    w32(x, b + 16, 0x1fffff);
    w16(x, b + 24, 7);
    w16(x, b + 26, 65535);
    b[28] = 1;
    b[29] = 1;
    b[30] = x->be;
    b[31] = x->be;
    b[32] = 32;
    b[33] = 32;
    b[34] = 8;
    b[35] = 255;
    memcpy(b + 40, "QRX X11", 7);
    b[48] = 24;
    b[49] = 32;
    b[50] = 32;
    w32(x, b + 56, QXX_ROOT);
    w32(x, b + 60, 2);
    w32(x, b + 64, 0xffffff);
    w16(x, b + 76, 1024);
    w16(x, b + 78, 768);
    w16(x, b + 80, 270);
    w16(x, b + 82, 203);
    w16(x, b + 84, 1);
    w16(x, b + 86, 1);
    w32(x, b + 88, QXX_VISUAL);
    b[93] = 0;
    b[94] = 24;
    b[95] = 1;
    b[96] = 24;
    w16(x, b + 98, 1);
    w32(x, b + 104, QXX_VISUAL);
    b[108] = 4;
    b[109] = 8;
    w16(x, b + 110, 256);
    w32(x, b + 112, 0xff0000);
    w32(x, b + 116, 0xff00);
    w32(x, b + 120, 0xff);
    queue(x, b, sizeof(b));
    x->setup = 1;
}
qxx *qxx_new(void) {
    qxx *x = calloc(1, sizeof(*x));
    if (!x)
        return NULL;
    x->in = malloc(QXX_LIMIT);
    x->out = malloc(QXX_LIMIT);
    if (!x->in || !x->out) {
        qxx_free(x);
        return NULL;
    }
    return x;
}
void qxx_free(qxx *x) {
    unsigned i;
    if (!x)
        return;
    for (i = 0; i < QXX_WINDOWS; i++) {
        native_close(&x->windows[i]);
        free(x->windows[i].pixels);
    }
    free(x->in);
    free(x->out);
    free(x);
}
int qxx_write(qxx *x, const void *data, size_t n) {
    if (x->failed || n > QXX_LIMIT - x->ni)
        return -1;
    memcpy(x->in + x->ni, data, n);
    x->ni += n;
    for (;;) {
        size_t len;
        if (!x->setup) {
            if (x->ni < 12)
                break;
            if (x->in[0] != 'l' && x->in[0] != 'B') {
                x->failed = 1;
                return -1;
            }
            x->be = x->in[0] == 'B';
            len = 12 + ((r16(x, x->in + 6) + 3u) & ~3u) + ((r16(x, x->in + 8) + 3u) & ~3u);
            if (x->ni < len)
                break;
            if (r16(x, x->in + 2) != 11 || r16(x, x->in + 6) || r16(x, x->in + 8)) {
                x->failed = 1;
                return -1;
            }
            setup(x);
        } else {
            if (x->ni < 4)
                break;
            len = (size_t)r16(x, x->in + 2) * 4;
            if (!len) {
                x->failed = 1;
                return -1;
            }
            if (x->ni < len)
                break;
            x->sequence++;
            request(x, x->in, len);
        }
        memmove(x->in, x->in + len, x->ni - len);
        x->ni -= len;
        if (x->failed)
            return -1;
    }
    return (int)n;
}
int qxx_ready(qxx *x) {
    qxx_pump(x);
    return x->failed ? -1 : (int)x->no;
}
int qxx_read(qxx *x, void *data, size_t n) {
    qxx_pump(x);
    if (x->failed)
        return -1;
    if (n > x->no)
        n = x->no;
    memcpy(data, x->out, n);
    memmove(x->out, x->out + n, x->no - n);
    x->no -= n;
    return (int)n;
}
