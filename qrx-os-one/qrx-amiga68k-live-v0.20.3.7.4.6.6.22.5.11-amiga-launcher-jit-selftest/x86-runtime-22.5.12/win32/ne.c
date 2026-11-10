/*
 * 16-bit Windows starters.
 *
 * There is no Win16 here: the CPU core runs flat 32-bit code only. Many 16-bit programs of the
 * time are only a starter for a 32-bit one, and those are recognised and replaced:
 *
 *  - NAME.EXE next to NAME.W32 (Humongous games): the .w32 is the program.
 *  - An InstallShield 3 SETUP.EXE next to _INST32I.EX_: the archive holds the 32-bit engine
 *    (INSTALL.EXE, stored as _INSnnnn._MP) and its DLLs. They are unpacked beside SETUP.EXE,
 *    where the engine then finds SETUP.INS and the cabinets, and the engine starts instead.
 *
 * The archive is PKWARE DCL "implode" data; the decoder follows zlib's contrib/blast.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qrxwin.h"

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* "MZ" with a header at e_lfanew: 1 PE, 2 NE, 0 anything else. */
static int image_kind(const char *path)
{
    uint8_t h[64], s[2];
    FILE *f = fopen(path, "rb");
    int k = 0;
    if (!f) return 0;
    if (fread(h, 1, 64, f) == 64 && h[0] == 'M' && h[1] == 'Z' && !fseek(f, (long)le32(h + 60), SEEK_SET) && fread(s, 1, 2, f) == 2)
        k = s[0] == 'P' && s[1] == 'E' ? 1 : s[0] == 'N' && s[1] == 'E' ? 2 : 0;
    fclose(f);
    return k;
}

/* The folder part of an AmigaDOS path, and a file in it found without regard to case. */
static void dir_of(const char *path, char *dir, size_t max)
{
    const char *sl = strrchr(path, '/'), *co = strrchr(path, ':');
    size_t L = sl ? (size_t)(sl - path) : co ? (size_t)(co - path) + 1 : 0;
    if (L >= max) L = max - 1;
    memcpy(dir, path, L);
    dir[L] = 0;
}
static int find_in(const char *dir, const char *name, char *out, size_t max)
{
    DIR *d = opendir(dir[0] ? dir : "");
    struct dirent *e;
    size_t L = strlen(dir);
    if (!d) return 0;
    while ((e = readdir(d))) {
        if (strcasecmp(e->d_name, name)) continue;
        snprintf(out, max, "%s%s%s", dir, L && dir[L - 1] != '/' && dir[L - 1] != ':' ? "/" : "", e->d_name);
        closedir(d);
        return 1;
    }
    closedir(d);
    return 0;
}

/* ---------------------------------------------------------------- PKWARE DCL implode */
typedef struct { const uint8_t *in; size_t n, pos; uint32_t buf; int cnt; int bad; } bits_t;
typedef struct { short count[14]; short sym[256]; } huff_t;
static int getbits(bits_t *s, int need)
{
    uint32_t v;
    while (s->cnt < need) {
        if (s->pos >= s->n) { s->bad = 1; return 0; }
        s->buf |= (uint32_t)s->in[s->pos++] << s->cnt;
        s->cnt += 8;
    }
    v = s->buf & ((1u << need) - 1u);
    s->buf >>= need;
    s->cnt -= need;
    return (int)v;
}
/* The tables are stored as run lengths: low nibble the code length, high nibble repeats - 1. */
static void construct(huff_t *h, const uint8_t *rep, int n)
{
    short length[256], offs[15];
    int symbol = 0, len, i;
    while (n--) { int l = *rep & 15, k = (*rep++ >> 4) + 1; while (k--) length[symbol++] = (short)l; }
    memset(h->count, 0, sizeof(h->count));
    for (i = 0; i < symbol; i++) h->count[length[i]]++;
    offs[1] = 0;
    for (len = 1; len < 13; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (i = 0; i < symbol; i++) if (length[i]) h->sym[offs[length[i]]++] = (short)i;
}
/* Codes are stored bit-inverted, so each bit read is flipped. */
static int decode(bits_t *s, const huff_t *h)
{
    int len, code = 0, first = 0, index = 0, count;
    for (len = 1; len < 14; len++) {
        code |= getbits(s, 1) ^ 1;
        count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
        if (s->bad) return -1;
    }
    return -1;
}
static long blast(const uint8_t *in, size_t n, uint8_t *out, size_t max)
{
    static const uint8_t litlen[] = { 11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8, 9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11,
        7, 9, 11, 12, 6, 7, 22, 5, 8, 11, 8, 3, 7, 8, 6, 7, 6, 7, 8, 5, 8, 7, 9, 6, 8, 5, 7, 6, 7, 6, 5, 6, 7, 4, 5, 6, 5, 6, 7, 5, 6, 5, 4, 8, 5, 7, 4, 6,
        4, 5, 6, 5, 4, 3, 4, 4, 4, 5, 5, 4, 5, 4, 3, 4, 4, 3, 2, 3, 2, 2 };
    static const uint8_t lenlen[] = { 2, 35, 36, 53, 38, 23 };
    static const uint8_t distlen[] = { 2, 20, 53, 230, 247, 151, 248 };
    static const short base[16] = { 3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264 };
    static const uint8_t extra[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8 };
    static huff_t lit, len, dist;
    static int built;
    bits_t s;
    int lit_coded, dict, symbol;
    size_t at = 0;
    if (!built) { construct(&lit, litlen, sizeof(litlen)); construct(&len, lenlen, sizeof(lenlen)); construct(&dist, distlen, sizeof(distlen)); built = 1; }
    memset(&s, 0, sizeof(s));
    s.in = in; s.n = n;
    lit_coded = getbits(&s, 8);
    dict = getbits(&s, 8);
    if (s.bad || lit_coded > 1 || dict < 4 || dict > 6) return -1;
    for (;;) {
        if (getbits(&s, 1)) {
            int l, shift;
            size_t d;
            if ((symbol = decode(&s, &len)) < 0) return -1;
            l = base[symbol] + getbits(&s, extra[symbol]);
            if (l == 519) return (long)at;                         /* end of stream */
            shift = l == 2 ? 2 : dict;
            if ((symbol = decode(&s, &dist)) < 0) return -1;
            d = ((size_t)symbol << shift) + (size_t)getbits(&s, shift) + 1u;
            if (s.bad || d > at || at + (size_t)l > max) return -1;
            while (l--) { out[at] = out[at - d]; at++; }
        } else {
            symbol = lit_coded ? decode(&s, &lit) : getbits(&s, 8);
            if (s.bad || symbol < 0 || at >= max) return -1;
            out[at++] = (uint8_t)symbol;
        }
    }
}

/* ---------------------------------------------------------------- InstallShield 3 */
/* Header 0x4E: number of entries; from 0x52: offset, packed size, size, 8 bytes, then the
   name the setup uses and the name it is stored under, each as a 16-bit length and text. */
static int is3_unpack(qwin *w, const char *archive, const char *dir, char *engine, size_t max)
{
    FILE *f = fopen(archive, "rb");
    uint8_t *a = NULL, *out;
    long n;
    uint32_t count, i, p = 0x52;
    int ok = 0;
    engine[0] = 0;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0x60 && n < (32L << 20) && (a = (uint8_t *)malloc((size_t)n)) && fread(a, 1, (size_t)n, f) == (size_t)n) ok = 1;
    fclose(f);
    if (!ok || le32(a) != 0xD879AB2Au) { free(a); return 0; }   /* 2A AB 79 D8 */
    count = le16(a + 0x4E);
    for (i = 0; i < count && ok; i++) {
        uint32_t off, csize, usize, l1, l2;
        char name[64], stored[64], path[1200], have[1200];
        if (p + 22u > (uint32_t)n) { ok = 0; break; }
        off = le32(a + p); csize = le32(a + p + 4); usize = le32(a + p + 8);
        p += 20;
        l1 = le16(a + p); p += 2;
        if (l1 >= sizeof(name) || p + l1 + 2u > (uint32_t)n) { ok = 0; break; }
        memcpy(name, a + p, l1); name[l1] = 0; p += l1;
        l2 = le16(a + p); p += 2;
        if (l2 >= sizeof(stored) || p + l2 > (uint32_t)n) { ok = 0; break; }
        memcpy(stored, a + p, l2); stored[l2] = 0; p += l2;
        if (off + csize > (uint32_t)n || usize > (16u << 20)) { ok = 0; break; }
        snprintf(path, sizeof(path), "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != '/' && dir[strlen(dir) - 1] != ':' ? "/" : "", stored);
        if (!strcasecmp(name, "INSTALL.EXE")) snprintf(engine, max, "%s", path);
        if (find_in(dir, stored, have, sizeof(have))) continue;     /* unpacked by an earlier start */
        out = (uint8_t *)malloc(usize + 16u);
        if (!out) { ok = 0; break; }
        if (blast(a + off, csize, out, usize + 16u) != (long)usize) {
            qw_log(w, "InstallShield: %s in %s does not unpack", stored, archive);
            ok = 0;
        } else if ((f = fopen(path, "wb"))) {
            ok = fwrite(out, 1, usize, f) == usize;
            fclose(f);
        } else { qw_log(w, "InstallShield: cannot write %s", path); ok = 0; }
        free(out);
    }
    free(a);
    return ok && engine[0];
}

/* path: an AmigaDOS path of a 16-bit program. Fills out with the 32-bit program to run. */
int qw_ne_redirect(qwin *w, const char *path, char *out, size_t max)
{
    char dir[1024], name[256], alt[1200];
    const char *base = strrchr(path, '/');
    char *dot;
    if (image_kind(path) != 2) return 0;
    if (!base) base = strrchr(path, ':');
    base = base ? base + 1 : path;
    dir_of(path, dir, sizeof(dir));
    snprintf(name, sizeof(name), "%s", base);
    if ((dot = strrchr(name, '.'))) *dot = 0;
    if (strlen(name) + 5 < sizeof(name)) strcat(name, ".w32");
    if (find_in(dir, name, alt, sizeof(alt)) && image_kind(alt) == 1) {
        qw_log(w, "%s is a 16-bit starter: running %s", base, alt);
        snprintf(out, max, "%s", alt);
        return 1;
    }
    if (find_in(dir, "_INST32I.EX_", alt, sizeof(alt))) {
        char engine[1200];
        if (is3_unpack(w, alt, dir, engine, sizeof(engine)) && image_kind(engine) == 1) {
            qw_log(w, "%s is InstallShield's 16-bit starter: running its engine %s", base, engine);
            snprintf(out, max, "%s", engine);
            return 1;
        }
    }
    return 0;
}
