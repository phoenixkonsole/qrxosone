/* QRX OS One 22.5.12 - exact double <-> decimal conversion (printf %f/%e/%g and
 * strtod) that does not depend on the host C library.  The AROS m68k C library
 * formats doubles with limited precision, which changed program-visible
 * results (e.g. calc.exe's display).  Digits are computed exactly with small
 * big integers; rounding is round-half-to-even on the exact value, like glibc. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "qfloat.h"

#define BW 40                 /* 40 x 32 bit = 1280 bits: holds 2^1077 */

typedef struct { uint32_t w[BW]; int n; } big;       /* little-endian words */

static void b_set64(big *b, uint64_t v) { memset(b, 0, sizeof(*b)); b->w[0] = (uint32_t)v; b->w[1] = (uint32_t)(v >> 32); b->n = b->w[1] ? 2 : (b->w[0] ? 1 : 0); }
static void b_norm(big *b) { while (b->n > 0 && !b->w[b->n - 1]) b->n--; }
static int b_zero(const big *b) { return b->n == 0; }
static void b_shl(big *b, int s)
{
    int ws = s / 32, bs = s % 32, i;
    if (b_zero(b)) return;
    for (i = b->n - 1 + ws + 1; i >= 0; i--) {
        uint32_t hi = (i - ws >= 0 && i - ws < b->n) ? b->w[i - ws] : 0;
        uint32_t lo = (i - ws - 1 >= 0 && i - ws - 1 < b->n) ? b->w[i - ws - 1] : 0;
        if (i < BW) b->w[i] = bs ? (hi << bs) | (lo >> (32 - bs)) : hi;
    }
    b->n = b->n + ws + 1 > BW ? BW : b->n + ws + 1;
    b_norm(b);
}
static uint32_t b_mulsmall(big *b, uint32_t m)      /* returns carry */
{
    uint64_t c = 0;
    int i;
    for (i = 0; i < b->n; i++) { c += (uint64_t)b->w[i] * m; b->w[i] = (uint32_t)c; c >>= 32; }
    if (c && b->n < BW) b->w[b->n++] = (uint32_t)c, c = 0;
    return (uint32_t)c;
}
static uint32_t b_divsmall(big *b, uint32_t d)      /* returns remainder */
{
    uint64_t r = 0;
    int i;
    for (i = b->n - 1; i >= 0; i--) { r = (r << 32) | b->w[i]; b->w[i] = (uint32_t)(r / d); r %= d; }
    b_norm(b);
    return (uint32_t)r;
}
static int b_cmp(const big *a, const big *b)
{
    int i;
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--) if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}
static void b_sub(big *a, const big *b)              /* a -= b, a >= b */
{
    int64_t c = 0;
    int i;
    for (i = 0; i < a->n; i++) { c += (int64_t)a->w[i] - (i < b->n ? b->w[i] : 0); a->w[i] = (uint32_t)c; c = c < 0 ? -1 : 0; }
    b_norm(a);
}
static int b_bit(const big *b, int k) { return k / 32 < b->n ? (int)((b->w[k / 32] >> (k % 32)) & 1) : 0; }
static int b_bits(const big *b) { int k; if (!b->n) return 0; for (k = 31; !(b->w[b->n - 1] >> k & 1); k--) ; return (b->n - 1) * 32 + k + 1; }

/* ------------------------------------------------------------------ digit generator */
typedef struct {
    char intd[400]; int nint, ipos;     /* integer part digits (most significant first) */
    big frac; int fshift;               /* fraction = frac / 2^fshift */
} dgen;

static void dg_init(dgen *g, double v)
{
    uint64_t u, m;
    int e;
    big ip;
    memcpy(&u, &v, 8);
    e = (int)(u >> 52 & 0x7FF);
    m = u & 0xFFFFFFFFFFFFFull;
    if (e) m |= 1ull << 52; else e = 1;
    e -= 1075;                           /* v = m * 2^e */
    g->nint = 0; g->ipos = 0;
    if (e >= 0) {
        char tmp[400]; int k = 0;
        b_set64(&ip, m); b_shl(&ip, e);
        while (!b_zero(&ip)) { uint32_t r = b_divsmall(&ip, 1000000000u); int j; for (j = 0; j < 9; j++) { tmp[k++] = (char)('0' + r % 10); r /= 10; } }
        while (k > 0 && tmp[k - 1] == '0') k--;
        while (k > 0) g->intd[g->nint++] = tmp[--k];
        b_set64(&g->frac, 0); g->fshift = 0;
    } else {
        int s = -e;
        uint64_t ipart = s >= 64 ? 0 : m >> s;
        char tmp[32]; int k = 0;
        while (ipart) { tmp[k++] = (char)('0' + ipart % 10); ipart /= 10; }
        while (k > 0) g->intd[g->nint++] = tmp[--k];
        b_set64(&g->frac, s >= 64 ? m : m & ((1ull << s) - 1));
        g->fshift = s;
    }
}
/* next fraction digit */
static int dg_frac(dgen *g)
{
    int d = 0, k;
    if (b_zero(&g->frac)) return 0;
    b_mulsmall(&g->frac, 10);
    for (k = g->fshift + 3; k >= g->fshift; k--) d = d * 2 + b_bit(&g->frac, k);
    /* clear bits >= fshift */
    {
        int wi = g->fshift / 32, bi = g->fshift % 32, i;
        if (wi < BW) { g->frac.w[wi] &= bi ? ((1u << bi) - 1) : 0; for (i = wi + 1; i < BW; i++) g->frac.w[i] = 0; }
        b_norm(&g->frac);
    }
    return d;
}

/* round digit array d[0..n) given next digit and sticky; returns carry out */
static int round_digits(char *d, int n, int next, int sticky)
{
    int i, up = next > 5 || (next == 5 && (sticky || (n > 0 && ((d[n - 1] - '0') & 1))));
    if (next == 5 && !sticky && n == 0) up = 0;
    if (!up) return 0;
    for (i = n - 1; i >= 0; i--) { if (d[i] == '9') d[i] = '0'; else { d[i]++; return 0; } }
    return 1;
}

/* fixed: integer digits + p fraction digits */
static int fmt_fixed(double v, int p, char *out)
{
    dgen g;
    char d[1500];
    int n = 0, i, next, sticky, carry;
    dg_init(&g, v);
    for (i = 0; i < g.nint; i++) d[n++] = g.intd[i];
    if (!g.nint) d[n++] = '0';
    {
        int ni = n;
        for (i = 0; i < p && n < 1400; i++) d[n++] = (char)('0' + dg_frac(&g));
        next = dg_frac(&g);
        sticky = !b_zero(&g.frac);
        carry = round_digits(d, n, next, sticky);
        if (carry) { memmove(d + 1, d, (size_t)n); d[0] = '1'; n++; ni++; }
        memcpy(out, d, (size_t)ni);
        i = ni;
        if (p > 0) { out[i++] = '.'; memcpy(out + i, d + ni, (size_t)(n - ni)); i += n - ni; }
        out[i] = 0;
        return i;
    }
}

/* scientific: p+1 significant digits; returns decimal exponent */
static int sci_digits(double v, int p, char *d)
{
    dgen g;
    int n = 0, exp10, next, sticky, i;
    dg_init(&g, v);
    if (v == 0) { for (i = 0; i <= p; i++) d[i] = '0'; d[p + 1] = 0; return 0; }
    if (g.nint) {
        exp10 = g.nint - 1;
        for (i = 0; i < g.nint && n <= p; i++) d[n++] = g.intd[i];
        if (n <= p) { while (n <= p) d[n++] = (char)('0' + dg_frac(&g)); next = dg_frac(&g); sticky = !b_zero(&g.frac); }
        else { next = i < g.nint ? g.intd[i++] - '0' : dg_frac(&g); sticky = !b_zero(&g.frac); for (; i < g.nint; i++) if (g.intd[i] != '0') sticky = 1; }
    } else {
        int f;
        exp10 = -1;
        while ((f = dg_frac(&g)) == 0) exp10--;
        d[n++] = (char)('0' + f);
        while (n <= p) d[n++] = (char)('0' + dg_frac(&g));
        next = dg_frac(&g); sticky = !b_zero(&g.frac);
    }
    if (round_digits(d, n, next, sticky)) { memmove(d + 1, d, (size_t)n - 1); d[0] = '1'; exp10++; }
    d[n] = 0;
    return exp10;
}

int qf_format(char *out, size_t outn, double v, int conv, int prec, int flags, int width)
{
    char body[1600], num[1600];
    int neg, n = 0, upper = conv == 'E' || conv == 'G' || conv == 'F', alt = flags & QF_ALT;
    uint64_t u;
    memcpy(&u, &v, 8);
    neg = (int)(u >> 63);
    if (neg) v = -v;
    if (prec < 0) prec = 6;
    if (prec > 1100) prec = 1100;
    if ((u >> 52 & 0x7FF) == 0x7FF) {
        strcpy(body, (u << 12) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf"));
        flags &= ~QF_ZERO;
    } else if (conv == 'f' || conv == 'F') {
        n = fmt_fixed(v, prec, body);
        if (alt && !prec) { body[n++] = '.'; body[n] = 0; }
    } else if (conv == 'e' || conv == 'E') {
        int e = sci_digits(v, prec, num), k = 0;
        body[k++] = num[0];
        if (prec || alt) body[k++] = '.';
        memcpy(body + k, num + 1, (size_t)prec); k += prec;
        k += sprintf(body + k, "%c%c%02d", upper ? 'E' : 'e', e < 0 ? '-' : '+', e < 0 ? -e : e);
        body[k] = 0;
    } else {                                      /* g / G */
        int P = prec ? prec : 1, X = v == 0 ? 0 : sci_digits(v, P - 1, num);
        if (P > X && X >= -4) {
            n = fmt_fixed(v, P - 1 - X, body);
        } else {
            int k = 0;
            body[k++] = num[0];
            body[k++] = '.';
            memcpy(body + k, num + 1, (size_t)(P - 1)); k += P - 1;
            body[k] = 0;
            n = k;
            if (!alt) { while (n > 0 && body[n - 1] == '0') n--; if (n > 0 && body[n - 1] == '.') n--; body[n] = 0; }
            sprintf(body + n, "%c%c%02d", upper ? 'E' : 'e', X < 0 ? '-' : '+', X < 0 ? -X : X);
            goto pad;
        }
        if (!alt && strchr(body, '.')) { while (n > 0 && body[n - 1] == '0') n--; if (n > 0 && body[n - 1] == '.') n--; body[n] = 0; }
        else if (alt && !strchr(body, '.')) { body[n++] = '.'; body[n] = 0; }
    }
pad:
    {
        char sign = neg ? '-' : (flags & QF_PLUS) ? '+' : (flags & QF_SPACE) ? ' ' : 0;
        int bl = (int)strlen(body), tot = bl + (sign ? 1 : 0), padn = width > tot ? width - tot : 0, k = 0, i;
        char tmp[1700];
        if (!(flags & QF_LEFT) && !(flags & QF_ZERO)) for (i = 0; i < padn; i++) tmp[k++] = ' ';
        if (sign) tmp[k++] = sign;
        if (!(flags & QF_LEFT) && (flags & QF_ZERO)) for (i = 0; i < padn; i++) tmp[k++] = '0';
        memcpy(tmp + k, body, (size_t)bl); k += bl;
        if (flags & QF_LEFT) for (i = 0; i < padn; i++) tmp[k++] = ' ';
        tmp[k] = 0;
        if (outn) { size_t c = (size_t)k < outn - 1 ? (size_t)k : outn - 1; memcpy(out, tmp, c); out[c] = 0; }
        return k;
    }
}

/* ------------------------------------------------------------------ parsing */
static double make_double(int neg, uint64_t mant, int e2)   /* value = mant * 2^e2, mant < 2^63, already rounded */
{
    uint64_t u;
    double d;
    if (!mant) { u = (uint64_t)neg << 63; memcpy(&d, &u, 8); return d; }
    while (mant >= (1ull << 53)) { mant >>= 1; e2++; }
    while (mant < (1ull << 52)) { mant <<= 1; e2--; }
    e2 += 52 + 1023;
    if (e2 >= 0x7FF) u = 0x7FF0000000000000ull;
    else if (e2 <= 0) u = mant;      /* caller already produced the denormal mantissa with e2 adjusted */
    else u = ((uint64_t)e2 << 52) | (mant & 0xFFFFFFFFFFFFFull);
    u |= (uint64_t)neg << 63;
    memcpy(&d, &u, 8);
    return d;
}

double qf_strtod(const char *s, char **end)
{
    const char *p = s;
    int neg = 0, nd = 0, dexp = 0, any = 0, sticky = 0;
    big N;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if ((p[0] | 32) == 'i' && (p[1] | 32) == 'n' && (p[2] | 32) == 'f') { if (end) *end = (char *)p + 3; return make_double(neg, 0, 0) + (neg ? -1.0 : 1.0) * __builtin_inf(); }
    if ((p[0] | 32) == 'n' && (p[1] | 32) == 'a' && (p[2] | 32) == 'n') { uint64_t u = 0x7FF8000000000000ull; double d; memcpy(&d, &u, 8); if (end) *end = (char *)p + 3; return d; }
    b_set64(&N, 0);
    for (; *p >= '0' && *p <= '9'; p++) { any = 1; if (nd < 760) { b_mulsmall(&N, 10); if (*p != '0') { big t; b_set64(&t, (uint64_t)(*p - '0')); /* add */ { uint64_t c = (uint64_t)(*p - '0'); int i; for (i = 0; c && i < BW; i++) { c += N.w[i]; N.w[i] = (uint32_t)c; c >>= 32; if (i >= N.n) N.n = i + 1; } } (void)t; } if (N.n) nd++; } else { dexp++; if (*p != '0') sticky = 1; } }
    if (*p == '.') {
        p++;
        for (; *p >= '0' && *p <= '9'; p++) { any = 1; if (nd < 760) { b_mulsmall(&N, 10); { uint64_t c = (uint64_t)(*p - '0'); int i; for (i = 0; c && i < BW; i++) { c += N.w[i]; N.w[i] = (uint32_t)c; c >>= 32; if (i >= N.n) N.n = i + 1; } } if (N.n) nd++; dexp--; } else if (*p != '0') sticky = 1; }
    }
    if (!any) { if (end) *end = (char *)s; return 0.0; }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int en = 0, eneg = 0, ok = 0;
        if (*q == '-' || *q == '+') eneg = *q++ == '-';
        for (; *q >= '0' && *q <= '9'; q++) { ok = 1; if (en < 100000) en = en * 10 + (*q - '0'); }
        if (ok) { dexp += eneg ? -en : en; p = q; }
    }
    if (end) *end = (char *)p;
    b_norm(&N);
    if (b_zero(&N)) return make_double(neg, 0, 0);
    if (dexp > 330) return neg ? -__builtin_inf() : __builtin_inf();
    if (dexp < -1100) return make_double(neg, 0, 0);
    {
        /* value = N * 10^dexp.  Compute q = floor(value * 2^k) with 55..56 significant bits. */
        big num = N, den;
        int k, i, e2, bits;
        uint64_t q = 0;
        b_set64(&den, 1);
        if (dexp >= 0) for (i = 0; i < dexp; i++) b_mulsmall(&num, 10);
        else for (i = 0; i < -dexp; i++) b_mulsmall(&den, 10);
        /* choose k so that num*2^k / den is in [2^55, 2^56) */
        k = 55 - (b_bits(&num) - b_bits(&den));
        if (k > 0) b_shl(&num, k); else if (k < 0) b_shl(&den, -k);
        while (b_cmp(&num, &den) < 0) { b_shl(&num, 1); k++; }
        {   /* num/den in [1, 2^60): long division producing 56 quotient bits */
            big dd = den;
            int sh = 0;
            while (b_cmp(&dd, &num) <= 0) { b_shl(&dd, 1); sh++; }
            for (i = sh - 1; i >= 0; i--) {
                big t = den;
                b_shl(&t, i);
                q <<= 1;
                if (b_cmp(&num, &t) >= 0) { b_sub(&num, &t); q |= 1; }
            }
            if (!b_zero(&num)) sticky = 1;
        }
        e2 = -k;                       /* value ~= q * 2^e2 */
        bits = 64 - __builtin_clzll(q);
        /* target precision: 53 bits, or fewer for denormals */
        {
            int exp_unb = e2 + bits - 1;          /* binary exponent of the leading bit */
            int keep = 53;
            uint64_t half, rem, mant;
            int drop;
            if (exp_unb < -1022) keep = 53 - (-1022 - exp_unb);
            if (keep < 0) keep = 0;
            drop = bits - keep;
            if (drop <= 0) { mant = q << -drop; e2 += drop; drop = 0; rem = 0; half = 1; }
            else {
                rem = q & ((1ull << drop) - 1);
                half = 1ull << (drop - 1);
                mant = q >> drop;
                e2 += drop;
                if (rem > half || (rem == half && (sticky || (mant & 1)))) mant++;
            }
            if (exp_unb < -1022) {             /* denormal: mantissa field directly */
                uint64_t u; double d;
                if (mant >> 52) { u = (1ull << 52) | (mant & 0xFFFFFFFFFFFFFull); u = (mant >= (1ull << 52)) ? (1ull << 52) : mant; }
                u = mant | ((uint64_t)neg << 63);
                if (mant >= (1ull << 52)) u = (1ull << 52) | ((uint64_t)neg << 63);
                memcpy(&d, &u, 8);
                return d;
            }
            return make_double(neg, mant, e2);
        }
    }
}
