/* Windows icons for QRXDesktop: reads the first icon group of a PE file, or an
 * .ico file, and converts the best image to 32x32 ARGB. Plain C with no AROS
 * dependency, so tests/test-wicon.c can run it on the build host. */
#ifndef QRX_WICON_H
#define QRX_WICON_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define QRX_WICON_SIZE 32
#define QRX_WICON_MAX_IMAGE (1024u * 1024u)

/* Reads n bytes at off; returns 0 on a short read. */
typedef int (*qrx_wicon_read)(void *ctx, uint32_t off, void *buf, uint32_t n);

static uint16_t qwi_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t qwi_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A higher score is a better match for a 32x32 display. */
static long qwi_score(unsigned w, unsigned h, unsigned bits) {
    long dist;
    if (!w) w = 256;
    if (!h) h = 256;
    dist = (long)w - QRX_WICON_SIZE;
    if (dist < 0) dist = -dist * 3;      /* enlarging looks worse than shrinking */
    if (w != h) dist += 64;
    return (long)bits * 2 - dist * 4;
}

/* One DIB icon image (BITMAPINFOHEADER, XOR bitmap, AND mask). PNG images are
 * refused; the caller then tries nothing else and draws a generic icon. */
static int qwi_decode_dib(const uint8_t *d, uint32_t n, uint32_t *out) {
    uint32_t hs, w, h, bits, used, colors, xrow, arow, off_xor, off_and, x, y;
    const uint8_t *pal;
    if (n < 40) return 0;
    hs = qwi_u32(d);
    if (hs < 40 || hs > n) return 0;
    w = qwi_u32(d + 4);
    h = qwi_u32(d + 8) / 2;
    bits = qwi_u16(d + 14);
    used = qwi_u32(d + 32);
    if (!w || !h || w > 512 || h > 512 || qwi_u32(d + 16) != 0) return 0;
    if (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32) return 0;
    colors = bits <= 8 ? (used ? used : 1u << bits) : 0;
    if (colors > 256) return 0;
    if ((uint64_t)hs + colors * 4u > n) return 0;
    pal = d + hs;
    xrow = ((w * bits + 31) / 32) * 4;
    arow = ((w + 31) / 32) * 4;
    off_xor = hs + colors * 4u;
    off_and = off_xor + xrow * h;
    if ((uint64_t)off_xor + (uint64_t)xrow * h > n) return 0;
    for (y = 0; y < QRX_WICON_SIZE; y++) {
        uint32_t sy = y * h / QRX_WICON_SIZE;
        const uint8_t *row = d + off_xor + (h - 1 - sy) * xrow;
        const uint8_t *mask = d + off_and + (h - 1 - sy) * arow;
        int has_mask = (uint64_t)off_and + (uint64_t)arow * h <= n;
        for (x = 0; x < QRX_WICON_SIZE; x++) {
            uint32_t sx = x * w / QRX_WICON_SIZE, a = 255, r, g, b, idx;
            if (bits == 32) {
                b = row[sx * 4]; g = row[sx * 4 + 1]; r = row[sx * 4 + 2]; a = row[sx * 4 + 3];
            } else if (bits == 24) {
                b = row[sx * 3]; g = row[sx * 3 + 1]; r = row[sx * 3 + 2];
            } else {
                if (bits == 8) idx = row[sx];
                else if (bits == 4) idx = (row[sx / 2] >> ((sx & 1) ? 0 : 4)) & 15u;
                else idx = (row[sx / 8] >> (7 - (sx & 7))) & 1u;
                if (idx >= colors) idx = 0;
                b = pal[idx * 4]; g = pal[idx * 4 + 1]; r = pal[idx * 4 + 2];
            }
            /* Old 32-bit icons have an all-zero alpha channel and rely on the mask. */
            if (bits != 32 && has_mask && ((mask[sx / 8] >> (7 - (sx & 7))) & 1u)) a = 0;
            out[y * QRX_WICON_SIZE + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    if (bits == 32) {
        uint32_t i, any = 0;
        for (i = 0; i < QRX_WICON_SIZE * QRX_WICON_SIZE; i++) any |= out[i] >> 24;
        if (!any) for (i = 0; i < QRX_WICON_SIZE * QRX_WICON_SIZE; i++) out[i] |= 0xFF000000u;
    }
    return 1;
}

static int qwi_load_image(qrx_wicon_read rd, void *ctx, uint32_t off, uint32_t size, uint32_t *out) {
    uint8_t *buf;
    int ok;
    if (!size || size > QRX_WICON_MAX_IMAGE) return 0;
    buf = (uint8_t *)malloc(size);
    if (!buf) return 0;
    ok = rd(ctx, off, buf, size) && qwi_decode_dib(buf, size, out);
    free(buf);
    return ok;
}

/* An .ico file: ICONDIR followed by 16-byte entries. */
static int qrx_wicon_from_ico(qrx_wicon_read rd, void *ctx, uint32_t *out) {
    uint8_t head[6], e[16];
    unsigned i, count;
    long best = -2147483647L;
    uint32_t off = 0, size = 0;
    if (!rd(ctx, 0, head, 6) || qwi_u16(head) != 0 || qwi_u16(head + 2) != 1) return 0;
    count = qwi_u16(head + 4);
    if (!count || count > 256) return 0;
    for (i = 0; i < count; i++) {
        long s;
        if (!rd(ctx, 6 + i * 16, e, 16)) return 0;
        s = qwi_score(e[0], e[1], qwi_u16(e + 6));
        if (s > best) { best = s; size = qwi_u32(e + 8); off = qwi_u32(e + 12); }
    }
    return qwi_load_image(rd, ctx, off, size, out);
}

/* ---- PE resources ---- */
typedef struct { qrx_wicon_read rd; void *ctx; uint32_t base, rva, nsec, sec_off; } qwi_pe;

static int qwi_rva_to_off(const qwi_pe *p, uint32_t rva, uint32_t *off) {
    uint32_t i;
    uint8_t s[40];
    for (i = 0; i < p->nsec; i++) {
        uint32_t va, vs, raw, ptr;
        if (!p->rd(p->ctx, p->sec_off + i * 40, s, 40)) return 0;
        vs = qwi_u32(s + 8); va = qwi_u32(s + 12); raw = qwi_u32(s + 16); ptr = qwi_u32(s + 20);
        if (vs < raw) vs = raw;
        if (rva >= va && rva - va < vs) { *off = ptr + (rva - va); return 1; }
    }
    return 0;
}

/* Finds the entry `id` of a resource directory, or with id < 0 the first one.
 * Returns the raw offset field, with bit 31 set for a subdirectory. */
static int qwi_res_find(const qwi_pe *p, uint32_t dir, long id, uint32_t *value, uint32_t *found_id) {
    uint8_t h[16], e[8];
    unsigned i, named, ids;
    if (!p->rd(p->ctx, p->base + dir, h, 16)) return 0;
    named = qwi_u16(h + 12); ids = qwi_u16(h + 14);
    if (named + ids > 4096) return 0;
    for (i = 0; i < named + ids; i++) {
        uint32_t name;
        if (!p->rd(p->ctx, p->base + dir + 16 + i * 8, e, 8)) return 0;
        name = qwi_u32(e);
        if (id < 0 || (!(name & 0x80000000u) && name == (uint32_t)id)) {
            *value = qwi_u32(e + 4);
            if (found_id) *found_id = name;
            return 1;
        }
    }
    return 0;
}

/* Walks type/name/language down to the data entry; returns file offset and size. */
static int qwi_res_data(const qwi_pe *p, uint32_t type, long name, uint32_t *off, uint32_t *size) {
    uint32_t v;
    uint8_t d[16];
    if (!qwi_res_find(p, 0, (long)type, &v, NULL) || !(v & 0x80000000u)) return 0;
    if (!qwi_res_find(p, v & 0x7FFFFFFFu, name, &v, NULL) || !(v & 0x80000000u)) return 0;
    if (!qwi_res_find(p, v & 0x7FFFFFFFu, -1, &v, NULL) || (v & 0x80000000u)) return 0;
    if (!p->rd(p->ctx, p->base + v, d, 16)) return 0;
    *size = qwi_u32(d + 4);
    return qwi_rva_to_off(p, qwi_u32(d), off);
}

static int qrx_wicon_from_pe(qrx_wicon_read rd, void *ctx, uint32_t *out) {
    uint8_t mz[64], pe[24], dd[8], *grp;
    uint32_t peoff, opt, rva, off, size, i, count, best_id = 0;
    long best = -2147483647L;
    qwi_pe p;
    int ok = 0;
    if (!rd(ctx, 0, mz, 64) || mz[0] != 'M' || mz[1] != 'Z') return 0;
    peoff = qwi_u32(mz + 60);
    if (!rd(ctx, peoff, pe, 24) || memcmp(pe, "PE\0\0", 4)) return 0;
    opt = qwi_u16(pe + 20);
    /* DataDirectory[2] of a PE32 optional header; PE32+ is 16 bytes further on. */
    if (!rd(ctx, peoff + 24, dd, 2)) return 0;
    if (!rd(ctx, peoff + 24 + (qwi_u16(dd) == 0x20B ? 112 : 96) + 16, dd, 8)) return 0;
    rva = qwi_u32(dd);
    if (!rva) return 0;
    p.rd = rd; p.ctx = ctx; p.base = 0; p.rva = rva;
    p.nsec = qwi_u16(pe + 6);
    p.sec_off = peoff + 24 + opt;
    if (p.nsec > 96 || !qwi_rva_to_off(&p, rva, &p.base)) return 0;
    if (!qwi_res_data(&p, 14, -1, &off, &size) || size < 6 || size > 65536) return 0;
    grp = (uint8_t *)malloc(size);
    if (!grp) return 0;
    if (rd(ctx, off, grp, size)) {
        count = qwi_u16(grp + 4);
        for (i = 0; i < count && 6 + (i + 1) * 14 <= size; i++) {
            const uint8_t *e = grp + 6 + i * 14;
            long s = qwi_score(e[0], e[1], qwi_u16(e + 6));
            if (s > best) { best = s; best_id = qwi_u16(e + 12); }
        }
        if (best_id && qwi_res_data(&p, 3, (long)best_id, &off, &size))
            ok = qwi_load_image(rd, ctx, off, size, out);
    }
    free(grp);
    return ok;
}
#endif
