/* Arithlon 22.5.13 - 68040 reference interpreter (integer unit, FPU, user mode).
 * See m68k.h.  Every instruction a 68040 executes in user mode is decoded here;
 * supervisor-only instructions, MOVEP, CAS2, CMP2/CHK2, PACK/UNPK and packed-
 * decimal FPU operands fault as "illegal" (none of them appears in gcc output).
 * The FPU computes in IEEE double precision like qrx86core's x87. */
#define _GNU_SOURCE
#include "m68k.h"
#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <fenv.h>

/* ------------------------------------------------------------------ memory (big endian) */
static inline uint8_t rd8(m68k *c, uint32_t a) { return c->mem[a]; }
static inline uint16_t rd16(m68k *c, uint32_t a) { uint16_t v; memcpy(&v, c->mem + a, 2); return __builtin_bswap16(v); }
static inline uint32_t rd32(m68k *c, uint32_t a) { uint32_t v; memcpy(&v, c->mem + a, 4); return __builtin_bswap32(v); }
static inline void wr8(m68k *c, uint32_t a, uint32_t v) { c->mem[a] = (uint8_t)v; }
static inline void wr16(m68k *c, uint32_t a, uint32_t v) { uint16_t t = __builtin_bswap16((uint16_t)v); memcpy(c->mem + a, &t, 2); }
static inline void wr32(m68k *c, uint32_t a, uint32_t v) { uint32_t t = __builtin_bswap32(v); memcpy(c->mem + a, &t, 4); }

void m68k_fault(m68k *c, int code, uint32_t addr, const char *msg)
{
    c->fault = code;
    c->fault_addr = addr;
    snprintf(c->fault_msg, sizeof(c->fault_msg), "%s", msg);
    siglongjmp(*(sigjmp_buf *)c->jb, 1);
}
#define ILLEGAL() m68k_fault(c, M68K_FAULT_ILL, c->insn_pc, "illegal or unsupported instruction")

static inline uint16_t f16(m68k *c) { uint16_t v = rd16(c, c->pc); c->pc += 2; return v; }
static inline uint32_t f32(m68k *c) { uint32_t v = rd32(c, c->pc); c->pc += 4; return v; }

static inline uint32_t MASK(int sz) { return sz == 4 ? 0xFFFFFFFFu : sz == 2 ? 0xFFFFu : 0xFFu; }
static inline uint32_t MSB(int sz) { return sz == 4 ? 0x80000000u : sz == 2 ? 0x8000u : 0x80u; }
static inline int32_t sext(uint32_t v, int sz) { return sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : (int32_t)v; }
static inline void push32(m68k *c, uint32_t v) { c->a[7] -= 4; wr32(c, c->a[7], v); }
static inline uint32_t pop32(m68k *c) { uint32_t v = rd32(c, c->a[7]); c->a[7] += 4; return v; }

void m68k_init(m68k *c, uint8_t *mem)
{
    memset(c, 0, sizeof(*c));
    c->mem = mem;
}
uint32_t m68k_sr(const m68k *c) { return (uint32_t)(c->x << 4 | c->n << 3 | c->z << 2 | c->v << 1 | c->c); }
static void set_ccr(m68k *c, uint32_t v) { c->x = (v >> 4) & 1; c->n = (v >> 3) & 1; c->z = (v >> 2) & 1; c->v = (v >> 1) & 1; c->c = v & 1; }

/* ------------------------------------------------------------------ effective addresses */
enum { EA_D, EA_A, EA_M, EA_I };
typedef struct { int k, r; uint32_t a; } ea_t;

/* (d8,An,Xn) brief format and the 68020 full format with memory indirection */
static uint32_t ea_index(m68k *c, uint32_t base)
{
    uint16_t ext = f16(c);
    int r = (ext >> 12) & 15, bdsz, iis;
    int32_t xi = r < 8 ? (int32_t)c->d[r] : (int32_t)c->a[r - 8], bd = 0, od = 0;
    if (!(ext & 0x800)) xi = (int16_t)xi;
    xi = (int32_t)((uint32_t)xi << ((ext >> 9) & 3));
    if (!(ext & 0x100)) return base + (uint32_t)xi + (uint32_t)(int8_t)ext;
    bdsz = (ext >> 4) & 3; iis = ext & 7;
    if (bdsz == 2) bd = (int16_t)f16(c); else if (bdsz == 3) bd = (int32_t)f32(c);
    if (ext & 0x80) base = 0;
    if (ext & 0x40) xi = 0;
    if ((iis & 3) == 2) od = (int16_t)f16(c); else if ((iis & 3) == 3) od = (int32_t)f32(c);
    if ((iis & 3) == 0) return base + (uint32_t)bd + (uint32_t)xi;
    if (iis & 4) return rd32(c, base + (uint32_t)bd) + (uint32_t)xi + (uint32_t)od;   /* post-indexed */
    return rd32(c, base + (uint32_t)bd + (uint32_t)xi) + (uint32_t)od;                 /* pre-indexed */
}

/* resolve once: (An)+ / -(An) side effects and extension words happen here */
static void ea(m68k *c, int mode, int reg, int sz, ea_t *e)
{
    int step = (sz == 1 && reg == 7) ? 2 : sz;
    e->r = reg;
    switch (mode) {
    case 0: e->k = EA_D; return;
    case 1: e->k = EA_A; return;
    case 2: e->k = EA_M; e->a = c->a[reg]; return;
    case 3: e->k = EA_M; e->a = c->a[reg]; c->a[reg] += (uint32_t)step; return;
    case 4: c->a[reg] -= (uint32_t)step; e->k = EA_M; e->a = c->a[reg]; return;
    case 5: e->k = EA_M; e->a = c->a[reg] + (uint32_t)(int16_t)f16(c); return;
    case 6: e->k = EA_M; e->a = ea_index(c, c->a[reg]); return;
    default:
        switch (reg) {
        case 0: e->k = EA_M; e->a = (uint32_t)(int16_t)f16(c); return;
        case 1: e->k = EA_M; e->a = f32(c); return;
        case 2: { uint32_t b = c->pc; e->k = EA_M; e->a = b + (uint32_t)(int16_t)f16(c); return; }
        case 3: { uint32_t b = c->pc; e->k = EA_M; e->a = ea_index(c, b); return; }
        case 4: e->k = EA_I; e->a = sz == 1 ? (f16(c) & 0xFFu) : sz == 2 ? f16(c) : f32(c); return;
        default: break;
        }
    }
    ILLEGAL();
}
/* control addressing modes (LEA, JMP, MOVEM, bit fields): the address only */
static uint32_t ea_addr(m68k *c, int mode, int reg)
{
    ea_t e;
    if (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg == 4)) ILLEGAL();
    ea(c, mode, reg, 4, &e);
    return e.a;
}
static uint32_t eget(m68k *c, const ea_t *e, int sz)
{
    switch (e->k) {
    case EA_D: return c->d[e->r] & MASK(sz);
    case EA_A: return c->a[e->r] & MASK(sz);
    case EA_I: return e->a;
    default: return sz == 1 ? rd8(c, e->a) : sz == 2 ? rd16(c, e->a) : rd32(c, e->a);
    }
}
static void eset(m68k *c, const ea_t *e, int sz, uint32_t v)
{
    switch (e->k) {
    case EA_D: c->d[e->r] = sz == 4 ? v : (c->d[e->r] & ~MASK(sz)) | (v & MASK(sz)); return;
    case EA_A: c->a[e->r] = v; return;
    case EA_M: if (sz == 1) wr8(c, e->a, v); else if (sz == 2) wr16(c, e->a, v); else wr32(c, e->a, v); return;
    default: ILLEGAL();
    }
}

/* ------------------------------------------------------------------ flags */
static inline void nz(m68k *c, uint32_t r, int sz) { c->n = (r & MSB(sz)) != 0; c->z = (r & MASK(sz)) == 0; }
static inline void logic(m68k *c, uint32_t r, int sz) { nz(c, r, sz); c->v = c->c = 0; }
/* mode: 0 ADD (sets X), 1 ADDX (Z only cleared) */
static uint32_t add(m68k *c, uint32_t d, uint32_t s, int sz, int mode)
{
    uint32_t m = MASK(sz), r;
    uint64_t r64;
    int x = mode ? c->x : 0;
    d &= m; s &= m;
    r64 = (uint64_t)d + s + (uint32_t)x;
    r = (uint32_t)r64 & m;
    c->c = c->x = r64 > m;
    c->v = ((~(d ^ s)) & (d ^ r) & MSB(sz)) != 0;
    c->n = (r & MSB(sz)) != 0;
    if (mode) { if (r) c->z = 0; } else c->z = r == 0;
    return r;
}
/* mode: 0 SUB (sets X), 1 SUBX, 2 CMP (X unchanged) */
static uint32_t sub(m68k *c, uint32_t d, uint32_t s, int sz, int mode)
{
    uint32_t m = MASK(sz), r, x = mode == 1 ? c->x : 0;
    d &= m; s &= m;
    r = (d - s - x) & m;
    c->c = (uint64_t)s + x > d;
    if (mode != 2) c->x = c->c;
    c->v = ((d ^ s) & (d ^ r) & MSB(sz)) != 0;
    c->n = (r & MSB(sz)) != 0;
    if (mode == 1) { if (r) c->z = 0; } else c->z = r == 0;
    return r;
}
static int cond(const m68k *c, int cc)
{
    switch (cc & 15) {
    case 0: return 1;
    case 1: return 0;
    case 2: return !c->c && !c->z;
    case 3: return c->c || c->z;
    case 4: return !c->c;
    case 5: return c->c;
    case 6: return !c->z;
    case 7: return c->z;
    case 8: return !c->v;
    case 9: return c->v;
    case 10: return !c->n;
    case 11: return c->n;
    case 12: return c->n == c->v;
    case 13: return c->n != c->v;
    case 14: return !c->z && c->n == c->v;
    default: return c->z || c->n != c->v;
    }
}

/* type: 0 AS, 1 LS, 2 ROX, 3 RO */
static uint32_t shift(m68k *c, int type, int left, uint32_t v, unsigned cnt, int sz)
{
    uint32_t m = MASK(sz), sb = MSB(sz), r = v & m;
    unsigned i;
    int cf = 0, vf = 0;
    if (!cnt) { c->c = type == 2 ? c->x : 0; c->v = 0; nz(c, r, sz); return r; }
    switch (type) {
    case 0:
        for (i = 0; i < cnt; i++) {
            if (left) { uint32_t nr = (r << 1) & m; cf = (r & sb) != 0; if ((nr ^ r) & sb) vf = 1; r = nr; }
            else { cf = r & 1; r = (r >> 1) | (r & sb); }
        }
        c->x = c->c = (uint8_t)cf; c->v = (uint8_t)vf; break;
    case 1:
        for (i = 0; i < cnt; i++) {
            if (left) { cf = (r & sb) != 0; r = (r << 1) & m; }
            else { cf = r & 1; r >>= 1; }
        }
        c->x = c->c = (uint8_t)cf; c->v = 0; break;
    case 2:
        for (i = 0; i < cnt; i++) {
            if (left) { cf = (r & sb) != 0; r = ((r << 1) | c->x) & m; }
            else { cf = r & 1; r = (r >> 1) | (c->x ? sb : 0); }
            c->x = (uint8_t)cf;
        }
        c->c = c->x; c->v = 0; break;
    default: {
        unsigned bits = (unsigned)sz * 8, k = cnt % bits;
        if (k) r = left ? ((r << k) | (r >> (bits - k))) & m : ((r >> k) | (r << (bits - k))) & m;
        c->c = left ? (r & 1) : ((r & sb) != 0); c->v = 0; break; }
    }
    nz(c, r, sz);
    return r;
}

/* ------------------------------------------------------------------ BCD */
static uint32_t bcd_add(m68k *c, uint32_t d, uint32_t s)
{
    unsigned lo = (s & 15) + (d & 15) + c->x, r, cy;
    unsigned hi = (s & 0xF0) + (d & 0xF0);
    r = lo + hi;
    if (lo > 9) r += 6;
    cy = (r & 0x3F0) > 0x90;
    if (cy) r += 0x60;
    c->c = c->x = (uint8_t)cy;
    if (r & 0xFF) c->z = 0;
    c->n = (r & 0x80) != 0;
    c->v = 0;
    return r & 0xFF;
}
static uint32_t bcd_sub(m68k *c, uint32_t d, uint32_t s)
{
    int lo = (int)(d & 15) - (int)(s & 15) - c->x, hi = (int)(d & 0xF0) - (int)(s & 0xF0), r, bw;
    if (lo < 0) { lo += 10; hi -= 0x10; }
    bw = hi < 0;
    if (bw) hi += 0xA0;
    r = (hi + lo) & 0xFF;
    c->c = c->x = (uint8_t)bw;
    if (r) c->z = 0;
    c->n = (r & 0x80) != 0;
    c->v = 0;
    return (uint32_t)r;
}

/* ------------------------------------------------------------------ bit fields */
static void bitfield(m68k *c, uint16_t op)
{
    uint16_t ext = f16(c);
    int type = (op >> 8) & 7, mode = (op >> 3) & 7, reg = op & 7, dr = (ext >> 12) & 7;
    int32_t off = (ext & 0x800) ? (int32_t)c->d[(ext >> 6) & 7] : (ext >> 6) & 31;
    int w = (ext & 0x20) ? (int)(c->d[ext & 7] & 31) : ext & 31;
    uint32_t field, fmask, nf = 0;
    int write = type == 2 || type == 4 || type == 6 || type == 7;
    if (!w) w = 32;
    fmask = w == 32 ? 0xFFFFFFFFu : ((1u << w) - 1);
    if (mode == 0) {
        unsigned o = (unsigned)off & 31;
        uint32_t v = c->d[reg], rot = o ? (v << o) | (v >> (32 - o)) : v;
        field = (rot >> (32 - w)) & fmask;
        if (w == 32) field = rot;
        if (write) {
            uint32_t top = fmask << (32 - w);
            if (w == 32) top = 0xFFFFFFFFu;
            switch (type) {
            case 2: nf = ~field; break;
            case 4: nf = 0; break;
            case 6: nf = 0xFFFFFFFFu; break;
            default: nf = c->d[dr]; break;
            }
            rot = (rot & ~top) | (((nf & fmask) << (32 - w)) & top);
            if (w == 32) rot = nf;
            c->d[reg] = o ? (rot >> o) | (rot << (32 - o)) : rot;
        }
    } else {
        uint32_t a = ea_addr(c, mode, reg) + (uint32_t)(off >> 3);
        unsigned bo = (unsigned)off & 7;
        uint64_t big = ((uint64_t)rd32(c, a) << 32) | ((uint64_t)rd8(c, a + 4) << 24), m64;
        field = (uint32_t)((big << bo) >> (64 - w));
        if (write) {
            switch (type) {
            case 2: nf = ~field; break;
            case 4: nf = 0; break;
            case 6: nf = 0xFFFFFFFFu; break;
            default: nf = c->d[dr]; break;
            }
            m64 = ((uint64_t)fmask << (64 - w)) >> bo;
            big = (big & ~m64) | ((((uint64_t)(nf & fmask)) << (64 - w)) >> bo);
            wr32(c, a, (uint32_t)(big >> 32));
            if (bo + (unsigned)w > 32) wr8(c, a + 4, (uint32_t)(big >> 24));
        }
    }
    if (type == 7) { uint32_t ins = c->d[dr] & fmask; c->n = (ins >> (w - 1)) & 1; c->z = ins == 0; }
    else { c->n = (field >> (w - 1)) & 1; c->z = field == 0; }
    c->v = c->c = 0;
    switch (type) {
    case 1: c->d[dr] = field; break;                                     /* BFEXTU */
    case 3: c->d[dr] = w == 32 ? field : (uint32_t)(((int32_t)(field << (32 - w))) >> (32 - w)); break;   /* BFEXTS */
    case 5: { int i = 0; while (i < w && !((field >> (w - 1 - i)) & 1)) i++; c->d[dr] = (uint32_t)(off + i); break; }  /* BFFFO */
    default: break;
    }
}

/* ------------------------------------------------------------------ FPU */
#define FPCC_N 0x08000000u
#define FPCC_Z 0x04000000u
#define FPCC_I 0x02000000u
#define FPCC_NAN 0x01000000u
static void fp_cc(m68k *c, double r)
{
    uint32_t cc = 0;
    if (isnan(r)) cc |= FPCC_NAN;
    else {
        if (r == 0) cc |= FPCC_Z;
        if (isinf(r)) cc |= FPCC_I;
    }
    if (signbit(r)) cc |= FPCC_N;
    c->fpsr = (c->fpsr & 0x00FFFFFFu) | cc;
}
static int fcond(const m68k *c, int p)
{
    int n = (c->fpsr & FPCC_N) != 0, z = (c->fpsr & FPCC_Z) != 0, nan = (c->fpsr & FPCC_NAN) != 0;
    switch (p & 15) {
    case 0: return 0;
    case 1: return z;
    case 2: return !(nan || z || n);
    case 3: return z || !(nan || n);
    case 4: return n && !(nan || z);
    case 5: return z || (n && !nan);
    case 6: return !(nan || z);
    case 7: return !nan;
    case 8: return nan;
    case 9: return nan || z;
    case 10: return nan || !(n || z);
    case 11: return nan || z || !n;
    case 12: return nan || (n && !z);
    case 13: return nan || z || n;
    case 14: return !z;
    default: return 1;
    }
}
static double rd_ext(m68k *c, uint32_t a)
{
    uint32_t se = rd16(c, a), e = se & 0x7FFF;
    uint64_t m = ((uint64_t)rd32(c, a + 4) << 32) | rd32(c, a + 8);
    double v;
    if (e == 0x7FFF) v = (m << 1) ? NAN : INFINITY;
    else if (!m) v = 0.0;
    else v = ldexp((double)m, (int)e - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}
static void wr_ext(m68k *c, uint32_t a, double v)
{
    uint32_t se = signbit(v) ? 0x8000u : 0;
    uint64_t m = 0;
    if (isnan(v)) { se |= 0x7FFF; m = 0xC000000000000000ull; }
    else if (isinf(v)) se |= 0x7FFF;
    else if (v != 0) {
        int e;
        double f = frexp(fabs(v), &e);                     /* f in [0.5,1) */
        m = (uint64_t)ldexp(f, 64);
        se |= (uint32_t)(e - 1 + 16383) & 0x7FFF;
    }
    wr16(c, a, se); wr16(c, a + 2, 0);
    wr32(c, a + 4, (uint32_t)(m >> 32)); wr32(c, a + 8, (uint32_t)m);
}
static double rnd_mode(const m68k *c, double v)
{
    switch ((c->fpcr >> 4) & 3) {
    case 0: return nearbyint(v);
    case 1: return trunc(v);
    case 2: return floor(v);
    default: return ceil(v);
    }
}
static int32_t to_int(const m68k *c, double v, int32_t lo, int32_t hi)
{
    if (isnan(v)) return hi;
    v = rnd_mode(c, v);
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (int32_t)v;
}
static const int fmt_size[8] = { 4, 4, 12, 12, 2, 8, 1, 0 };
/* source operand of format fmt (0 L, 1 S, 2 X, 3 P, 4 W, 5 D, 6 B) */
static double fp_src(m68k *c, int mode, int reg, int fmt)
{
    uint32_t a;
    int sz = fmt_size[fmt];
    if (fmt == 3 || fmt == 7) ILLEGAL();
    if (mode == 0) {
        uint32_t v = c->d[reg];
        switch (fmt) {
        case 0: return (double)(int32_t)v;
        case 1: { float f; memcpy(&f, &v, 4); return f; }
        case 4: return (double)(int16_t)v;
        case 6: return (double)(int8_t)v;
        default: ILLEGAL();
        }
    }
    if (mode == 1) ILLEGAL();
    if (mode == 7 && reg == 4) { a = c->pc + (fmt == 6 ? 1u : 0u); c->pc += (uint32_t)(sz < 2 ? 2 : sz); }
    else { ea_t e; ea(c, mode, reg, sz, &e); a = e.a; }
    switch (fmt) {
    case 0: return (double)(int32_t)rd32(c, a);
    case 1: { uint32_t v = rd32(c, a); float f; memcpy(&f, &v, 4); return f; }
    case 2: return rd_ext(c, a);
    case 4: return (double)(int16_t)rd16(c, a);
    case 5: { uint64_t v = ((uint64_t)rd32(c, a) << 32) | rd32(c, a + 4); double d; memcpy(&d, &v, 8); return d; }
    default: return (double)(int8_t)rd8(c, a);
    }
}
static void fp_dst(m68k *c, int mode, int reg, int fmt, double v)
{
    int sz = fmt_size[fmt];
    uint32_t bits = 0;
    ea_t e;
    if (fmt == 3 || fmt == 7) ILLEGAL();
    switch (fmt) {
    case 0: bits = (uint32_t)to_int(c, v, INT32_MIN, INT32_MAX); break;
    case 1: { float f = (float)v; memcpy(&bits, &f, 4); break; }
    case 4: bits = (uint32_t)to_int(c, v, -32768, 32767) & 0xFFFF; break;
    case 6: bits = (uint32_t)to_int(c, v, -128, 127) & 0xFF; break;
    default: break;
    }
    if (mode == 0) {
        if (fmt == 2 || fmt == 5) ILLEGAL();
        e.k = EA_D; e.r = reg;
        eset(c, &e, sz, bits);
        return;
    }
    if (mode == 1 || (mode == 7 && reg >= 2)) ILLEGAL();
    ea(c, mode, reg, sz, &e);
    if (fmt == 2) wr_ext(c, e.a, v);
    else if (fmt == 5) { uint64_t q; memcpy(&q, &v, 8); wr32(c, e.a, (uint32_t)(q >> 32)); wr32(c, e.a + 4, (uint32_t)q); }
    else eset(c, &e, sz, bits);
}
static double fmovecr(int k)
{
    static const double p10[] = { 1e0, 1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256 };
    switch (k) {
    case 0x00: return M_PI;
    case 0x0B: return 0.30102999566398119521;     /* log10(2) */
    case 0x0C: return M_E;
    case 0x0D: return M_LOG2E;
    case 0x0E: return M_LOG10E;
    case 0x30: return M_LN2;
    case 0x31: return M_LN10;
    default:
        if (k >= 0x32 && k <= 0x3B) return p10[k - 0x32];
        if (k > 0x3B && k <= 0x3F) return INFINITY;     /* 1e512.. exceed double */
        return 0.0;
    }
}
static void fpu_op(m68k *c, int opm, double s, int dst, int sincos_c)
{
    double d = c->fp[dst], r;
    int store = 1, single = 0;
    switch (opm) {
    case 0x00: case 0x40: case 0x44: r = s; break;
    case 0x01: r = rnd_mode(c, s); break;
    case 0x02: r = sinh(s); break;
    case 0x03: r = trunc(s); break;
    case 0x04: case 0x41: case 0x45: r = sqrt(s); break;
    case 0x06: r = log1p(s); break;
    case 0x08: r = expm1(s); break;
    case 0x09: r = tanh(s); break;
    case 0x0A: r = atan(s); break;
    case 0x0C: r = asin(s); break;
    case 0x0D: r = atanh(s); break;
    case 0x0E: r = sin(s); break;
    case 0x0F: r = tan(s); break;
    case 0x10: r = exp(s); break;
    case 0x11: r = exp2(s); break;
    case 0x12: r = pow(10.0, s); break;
    case 0x14: r = log(s); break;
    case 0x15: r = log10(s); break;
    case 0x16: r = log2(s); break;
    case 0x18: case 0x58: case 0x5C: r = fabs(s); break;
    case 0x19: r = cosh(s); break;
    case 0x1A: case 0x5A: case 0x5E: r = -s; break;
    case 0x1C: r = acos(s); break;
    case 0x1D: r = cos(s); break;
    case 0x1E: r = (s == 0 || isinf(s) || isnan(s)) ? (isinf(s) ? NAN : s) : (double)ilogb(s); break;
    case 0x1F: { int e; r = (s == 0 || isinf(s) || isnan(s)) ? (isinf(s) ? NAN : s) : frexp(s, &e) * 2.0; break; }
    case 0x20: case 0x60: case 0x64: r = d / s; break;
    case 0x21: r = fmod(d, s); break;
    case 0x22: case 0x62: case 0x66: r = d + s; break;
    case 0x23: case 0x63: case 0x67: r = d * s; break;
    case 0x24: r = (double)(float)d / (double)(float)s; single = 1; break;
    case 0x25: r = remainder(d, s); break;
    case 0x26: r = ldexp(d, (int)trunc(s)); break;
    case 0x27: r = (double)(float)d * (double)(float)s; single = 1; break;
    case 0x28: case 0x68: case 0x6C: r = d - s; break;
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
        c->fp[sincos_c] = cos(s); r = sin(s); break;
    case 0x38: {                                    /* FCMP: condition codes of dst - src */
        uint32_t cc = 0;
        if (isnan(d) || isnan(s)) cc = FPCC_NAN;
        else if (d == s) { cc = FPCC_Z; if (signbit(d) && (isinf(d) || signbit(s))) cc |= FPCC_N; }
        else if (d < s) cc = FPCC_N;
        c->fpsr = (c->fpsr & 0x00FFFFFFu) | cc;
        return; }
    case 0x3A: fp_cc(c, s); return;                /* FTST */
    default: ILLEGAL();
    }
    if (opm >= 0x40 && !(opm & 4)) single = 1;      /* FSxxx: round to single */
    if (opm < 0x40 && ((c->fpcr >> 6) & 3) == 1) single = 1;
    if (single) r = (double)(float)r;
    if (store) c->fp[dst] = r;
    fp_cc(c, r);
}
static void fmovem(m68k *c, uint16_t op, uint16_t ext)
{
    int mode = (op >> 3) & 7, reg = op & 7, tomem = (ext & 0x2000) != 0, i;
    unsigned mask = (ext & 0x800) ? c->d[(ext >> 4) & 7] & 0xFF : ext & 0xFF;
    uint32_t a;
    if (mode == 4) {                                /* predecrement: bit i = FPi, FP7 stored first (highest address) */
        if (!tomem) ILLEGAL();
        a = c->a[reg];
        for (i = 7; i >= 0; i--) if (mask & (1u << i)) { a -= 12; wr_ext(c, a, c->fp[i]); }
        c->a[reg] = a;
        return;
    }
    a = mode == 3 ? c->a[reg] : ea_addr(c, mode, reg);
    for (i = 0; i < 8; i++) if (mask & (0x80u >> i)) {
        if (tomem) wr_ext(c, a, c->fp[i]); else c->fp[i] = rd_ext(c, a);
        a += 12;
    }
    if (mode == 3) c->a[reg] = a;
}
static void fmove_ctl(m68k *c, uint16_t op, uint16_t ext)
{
    int mode = (op >> 3) & 7, reg = op & 7, tomem = (ext & 0x2000) != 0, n = 0, i;
    uint32_t *r[3], a;
    if (ext & 0x1000) r[n++] = &c->fpcr;
    if (ext & 0x0800) r[n++] = &c->fpsr;
    if (ext & 0x0400) r[n++] = &c->fpiar;
    if (!n) ILLEGAL();
    if (mode == 0 || mode == 1) {
        if (n != 1) ILLEGAL();
        if (tomem) { if (mode == 0) c->d[reg] = *r[0]; else c->a[reg] = *r[0]; }
        else *r[0] = mode == 0 ? c->d[reg] : c->a[reg];
        return;
    }
    if (mode == 7 && reg == 4) { if (tomem) ILLEGAL(); for (i = 0; i < n; i++) *r[i] = f32(c); return; }
    if (mode == 4) { c->a[reg] -= 4u * (uint32_t)n; a = c->a[reg]; }
    else if (mode == 3) { a = c->a[reg]; c->a[reg] += 4u * (uint32_t)n; }
    else a = ea_addr(c, mode, reg);
    for (i = 0; i < n; i++, a += 4) { if (tomem) wr32(c, a, *r[i]); else *r[i] = rd32(c, a); }
}
static void fpu(m68k *c, uint16_t op)
{
    int type = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7;
    if (((op >> 9) & 7) != 1) {
        if ((op & 0xFFF8) == 0xF620) {              /* MOVE16 (Ax)+,(Ay)+ */
            uint16_t ext = f16(c);
            int ay = (ext >> 12) & 7;
            uint32_t s = c->a[reg] & ~15u, d = c->a[ay] & ~15u;
            memmove(c->mem + d, c->mem + s, 16);
            c->a[reg] += 16; if (ay != reg) c->a[ay] += 16;
            return;
        }
        ILLEGAL();
    }
    switch (type) {
    case 0: {
        uint16_t ext = f16(c);
        int opc = ext >> 13, fmt = (ext >> 10) & 7, dst = (ext >> 7) & 7;
        c->fpiar = c->insn_pc;
        switch (opc) {
        case 0: fpu_op(c, ext & 0x7F, c->fp[fmt], dst, ext & 7); return;
        case 2:
            if (fmt == 7) { c->fp[dst] = fmovecr(ext & 0x7F); fp_cc(c, c->fp[dst]); return; }
            { double s = fp_src(c, mode, reg, fmt); fpu_op(c, ext & 0x7F, s, dst, ext & 7); }
            return;
        case 3: fp_dst(c, mode, reg, fmt, c->fp[dst]); return;
        case 4: case 5: fmove_ctl(c, op, ext); return;
        case 6: case 7: fmovem(c, op, ext); return;
        default: ILLEGAL();
        }
    }
    case 1: {
        uint16_t ext = f16(c);
        int t = fcond(c, ext & 63);
        if (mode == 1) {                            /* FDBcc */
            uint32_t base = c->pc;
            int16_t disp = (int16_t)f16(c);
            if (!t) { uint16_t w = (uint16_t)(c->d[reg] - 1); c->d[reg] = (c->d[reg] & 0xFFFF0000u) | w; if (w != 0xFFFF) c->pc = base + (uint32_t)disp; }
            return;
        }
        if (mode == 7 && reg >= 2) {                /* FTRAPcc */
            if (reg == 2) c->pc += 2; else if (reg == 3) c->pc += 4;
            if (t) m68k_fault(c, M68K_FAULT_TRAP, c->insn_pc, "FTRAPcc");
            return;
        }
        { ea_t e; ea(c, mode, reg, 1, &e); eset(c, &e, 1, t ? 0xFF : 0); }
        return;
    }
    case 2: case 3: {                               /* FBcc */
        uint32_t base = c->pc;
        int32_t disp = type == 2 ? (int16_t)f16(c) : (int32_t)f32(c);
        if (fcond(c, op & 63)) c->pc = base + (uint32_t)disp;
        return;
    }
    default: ILLEGAL();                             /* FSAVE/FRESTORE: supervisor */
    }
}

/* ------------------------------------------------------------------ instruction groups */
static void bitop(m68k *c, int type, uint32_t bit, int mode, int reg)
{
    ea_t e;
    uint32_t v;
    int sz = mode == 0 ? 4 : 1;
    bit &= sz == 4 ? 31 : 7;
    ea(c, mode, reg, sz, &e);
    v = eget(c, &e, sz);
    c->z = !((v >> bit) & 1);
    switch (type) {
    case 1: eset(c, &e, sz, v ^ (1u << bit)); break;
    case 2: eset(c, &e, sz, v & ~(1u << bit)); break;
    case 3: eset(c, &e, sz, v | (1u << bit)); break;
    default: break;
    }
}
static const int size_code[4] = { 1, 2, 4, 0 };

static void group0(m68k *c, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7, sz = size_code[(op >> 6) & 3];
    ea_t e;
    uint32_t imm, v, r;
    if (op & 0x0100) {
        if (mode == 1) ILLEGAL();                   /* MOVEP */
        bitop(c, (op >> 6) & 3, c->d[(op >> 9) & 7], mode, reg);
        return;
    }
    if (op == 0x003C) { set_ccr(c, m68k_sr(c) | (f16(c) & 0x1F)); return; }
    if (op == 0x023C) { set_ccr(c, m68k_sr(c) & f16(c)); return; }
    if (op == 0x0A3C) { set_ccr(c, (m68k_sr(c) ^ f16(c)) & 0x1F); return; }
    if (!sz) {
        if ((op & 0xF9C0) == 0x08C0 && (op & 0x0600)) {   /* CAS .b/.w/.l */
            int csz = size_code[((op >> 9) & 3) - 1];
            uint16_t ext = f16(c);
            int du = (ext >> 6) & 7, dc = ext & 7;
            uint32_t dest;
            if ((op & 0x3F) == 0x3C) ILLEGAL();     /* CAS2 */
            ea(c, mode, reg, csz, &e);
            dest = eget(c, &e, csz);
            sub(c, dest, c->d[dc], csz, 2);
            if (c->z) eset(c, &e, csz, c->d[du]);
            else c->d[dc] = csz == 4 ? dest : (c->d[dc] & ~MASK(csz)) | dest;
            return;
        }
        if (((op >> 9) & 7) == 4) { uint32_t b = f16(c) & 0xFF; bitop(c, (op >> 6) & 3, b, mode, reg); return; }
        ILLEGAL();                                  /* CMP2/CHK2/CALLM/RTM */
    }
    switch ((op >> 9) & 7) {
    case 4: { uint32_t b = f16(c) & 0xFF; bitop(c, (op >> 6) & 3, b, mode, reg); return; }
    case 7: ILLEGAL();                              /* MOVES */
    default: break;
    }
    imm = sz == 1 ? (f16(c) & 0xFF) : sz == 2 ? f16(c) : f32(c);
    ea(c, mode, reg, sz, &e);
    if (e.k == EA_A || e.k == EA_I) ILLEGAL();
    v = eget(c, &e, sz);
    switch ((op >> 9) & 7) {
    case 0: r = v | imm; logic(c, r, sz); eset(c, &e, sz, r); return;          /* ORI */
    case 1: r = v & imm; logic(c, r, sz); eset(c, &e, sz, r); return;          /* ANDI */
    case 2: r = sub(c, v, imm, sz, 0); eset(c, &e, sz, r); return;             /* SUBI */
    case 3: r = add(c, v, imm, sz, 0); eset(c, &e, sz, r); return;             /* ADDI */
    case 5: r = v ^ imm; logic(c, r, sz); eset(c, &e, sz, r); return;          /* EORI */
    default: sub(c, v, imm, sz, 2); return;                                     /* CMPI */
    }
}

static void move(m68k *c, uint16_t op)
{
    static const int msz[4] = { 0, 1, 4, 2 };
    int sz = msz[(op >> 12) & 3], dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
    ea_t s, d;
    uint32_t v;
    ea(c, (op >> 3) & 7, op & 7, sz, &s);
    v = eget(c, &s, sz);
    if (dmode == 1) {                               /* MOVEA */
        if (sz == 1) ILLEGAL();
        c->a[dreg] = (uint32_t)sext(v, sz);
        return;
    }
    ea(c, dmode, dreg, sz, &d);
    if (d.k == EA_I) ILLEGAL();
    eset(c, &d, sz, v);
    logic(c, v, sz);
}

static void movem(m68k *c, uint16_t op)
{
    uint16_t mask = f16(c);
    int mode = (op >> 3) & 7, reg = op & 7, sz = (op & 0x40) ? 4 : 2, i;
    uint32_t a;
    if (!(op & 0x400)) {                            /* registers -> memory */
        if (mode == 4) {
            a = c->a[reg];
            for (i = 0; i < 16; i++) if (mask & (1u << i)) {
                int r = 15 - i;
                uint32_t v = r < 8 ? c->d[r] : c->a[r - 8];
                a -= (uint32_t)sz;
                if (sz == 4) wr32(c, a, v); else wr16(c, a, v);
            }
            c->a[reg] = a;
            return;
        }
        a = ea_addr(c, mode, reg);
        for (i = 0; i < 16; i++) if (mask & (1u << i)) {
            uint32_t v = i < 8 ? c->d[i] : c->a[i - 8];
            if (sz == 4) wr32(c, a, v); else wr16(c, a, v);
            a += (uint32_t)sz;
        }
        return;
    }
    a = mode == 3 ? c->a[reg] : ea_addr(c, mode, reg);
    for (i = 0; i < 16; i++) if (mask & (1u << i)) {
        uint32_t v = sz == 4 ? rd32(c, a) : (uint32_t)(int16_t)rd16(c, a);
        if (i < 8) c->d[i] = v; else c->a[i - 8] = v;
        a += (uint32_t)sz;
    }
    if (mode == 3) c->a[reg] = a;
}

static void muldiv_l(m68k *c, uint16_t op)
{
    uint16_t ext = f16(c);
    int dl = (ext >> 12) & 7, dh = ext & 7, sgn = (ext & 0x800) != 0, wide = (ext & 0x400) != 0;
    ea_t e;
    uint32_t s;
    ea(c, (op >> 3) & 7, op & 7, 4, &e);
    s = eget(c, &e, 4);
    c->c = 0;
    if (!(op & 0x40)) {                             /* MULU.L / MULS.L */
        uint64_t r = sgn ? (uint64_t)((int64_t)(int32_t)c->d[dl] * (int32_t)s) : (uint64_t)c->d[dl] * s;
        if (wide) {
            c->d[dh] = (uint32_t)(r >> 32); c->d[dl] = (uint32_t)r;
            c->n = (r >> 63) & 1; c->z = r == 0; c->v = 0;
        } else {
            c->d[dl] = (uint32_t)r;
            c->n = (r >> 31) & 1; c->z = (uint32_t)r == 0;
            c->v = sgn ? ((int64_t)r != (int32_t)(uint32_t)r) : (r >> 32) != 0;
        }
        return;
    }
    if (!s) m68k_fault(c, M68K_FAULT_DIV0, c->insn_pc, "division by zero");
    if (!sgn) {
        uint64_t n = wide ? ((uint64_t)c->d[dh] << 32) | c->d[dl] : c->d[dl], q = n / s, r = n % s;
        if (q > 0xFFFFFFFFu) { c->v = 1; return; }
        if (dh != dl) c->d[dh] = (uint32_t)r;
        c->d[dl] = (uint32_t)q;
        c->n = (q >> 31) & 1; c->z = q == 0; c->v = 0;
    } else {
        int64_t n = wide ? (int64_t)(((uint64_t)c->d[dh] << 32) | c->d[dl]) : (int64_t)(int32_t)c->d[dl], q, r;
        int32_t sv = (int32_t)s;
        if (n == INT64_MIN && sv == -1) { c->v = 1; return; }
        q = n / sv; r = n % sv;
        if (q != (int32_t)q) { c->v = 1; return; }
        if (dh != dl) c->d[dh] = (uint32_t)r;
        c->d[dl] = (uint32_t)q;
        c->n = q < 0; c->z = q == 0; c->v = 0;
    }
}

static void group4(m68k *c, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7, sz = size_code[(op >> 6) & 3];
    ea_t e;
    uint32_t v, r;
    if (op == 0x4AFC) ILLEGAL();
    if ((op & 0xFFF0) == 0x4E40) {                  /* TRAP #n */
        if (!c->trap || !c->trap(c, op & 15)) m68k_fault(c, M68K_FAULT_TRAP, c->insn_pc, "unhandled TRAP");
        return;
    }
    if ((op & 0xFFF8) == 0x4E50) { int16_t d = (int16_t)f16(c); push32(c, c->a[reg]); c->a[reg] = c->a[7]; c->a[7] += (uint32_t)d; return; }  /* LINK.W */
    if ((op & 0xFFF8) == 0x4808) { int32_t d = (int32_t)f32(c); push32(c, c->a[reg]); c->a[reg] = c->a[7]; c->a[7] += (uint32_t)d; return; }  /* LINK.L */
    if ((op & 0xFFF8) == 0x4E58) { c->a[7] = c->a[reg]; c->a[reg] = pop32(c); return; }    /* UNLK */
    switch (op) {
    case 0x4E71: return;                                                                    /* NOP */
    case 0x4E74: { int16_t d = (int16_t)f16(c); c->pc = pop32(c); c->a[7] += (uint32_t)d; return; }   /* RTD */
    case 0x4E75: c->pc = pop32(c); return;                                                  /* RTS */
    case 0x4E76: if (c->v) m68k_fault(c, M68K_FAULT_TRAP, c->insn_pc, "TRAPV"); return;
    case 0x4E77: { uint32_t s = rd16(c, c->a[7]); c->a[7] += 2; set_ccr(c, s); c->pc = pop32(c); return; }   /* RTR */
    default: break;
    }
    if ((op & 0xFFC0) == 0x4E80) { uint32_t t = ea_addr(c, mode, reg); push32(c, c->pc); c->pc = t; return; }   /* JSR */
    if ((op & 0xFFC0) == 0x4EC0) { c->pc = ea_addr(c, mode, reg); return; }                                      /* JMP */
    if ((op & 0xF1C0) == 0x41C0 && mode) { c->a[(op >> 9) & 7] = ea_addr(c, mode, reg); return; }                /* LEA (mode 0: EXTB.L) */
    if ((op & 0xF140) == 0x4100) {                                                                                /* CHK */
        int csz = (op & 0x80) ? 2 : 4;
        int32_t dn, bound;
        ea(c, mode, reg, csz, &e);
        bound = sext(eget(c, &e, csz), csz);
        dn = sext(c->d[(op >> 9) & 7], csz);
        if (dn < 0) { c->n = 1; m68k_fault(c, M68K_FAULT_TRAP, c->insn_pc, "CHK"); }
        if (dn > bound) { c->n = 0; m68k_fault(c, M68K_FAULT_TRAP, c->insn_pc, "CHK"); }
        return;
    }
    if ((op & 0xFFF8) == 0x4840) { r = (c->d[reg] >> 16) | (c->d[reg] << 16); c->d[reg] = r; logic(c, r, 4); return; }  /* SWAP */
    if ((op & 0xFFC0) == 0x4840) { uint32_t t = ea_addr(c, mode, reg); push32(c, t); return; }                     /* PEA */
    if ((op & 0xFFF8) == 0x4880) { r = (uint32_t)(int8_t)c->d[reg]; c->d[reg] = (c->d[reg] & 0xFFFF0000u) | (r & 0xFFFF); logic(c, r, 2); return; }   /* EXT.W */
    if ((op & 0xFFF8) == 0x48C0) { r = (uint32_t)(int16_t)c->d[reg]; c->d[reg] = r; logic(c, r, 4); return; }      /* EXT.L */
    if ((op & 0xFFF8) == 0x49C0) { r = (uint32_t)(int8_t)c->d[reg]; c->d[reg] = r; logic(c, r, 4); return; }       /* EXTB.L */
    if ((op & 0xFB80) == 0x4880) { movem(c, op); return; }
    if ((op & 0xFF80) == 0x4C00) { muldiv_l(c, op); return; }
    switch ((op >> 8) & 15) {
    case 0x0:                                                                                 /* NEGX */
        if (!sz) ILLEGAL();                                                                   /* MOVE from SR: supervisor */
        ea(c, mode, reg, sz, &e); v = eget(c, &e, sz); r = sub(c, 0, v, sz, 1); eset(c, &e, sz, r); return;
    case 0x2:
        if (!sz) { ea(c, mode, reg, 2, &e); eset(c, &e, 2, m68k_sr(c)); return; }             /* MOVE from CCR */
        ea(c, mode, reg, sz, &e); eset(c, &e, sz, 0); c->n = c->v = c->c = 0; c->z = 1; return;   /* CLR */
    case 0x4:
        if (!sz) { ea(c, mode, reg, 2, &e); set_ccr(c, eget(c, &e, 2)); return; }             /* MOVE to CCR */
        ea(c, mode, reg, sz, &e); v = eget(c, &e, sz); r = sub(c, 0, v, sz, 0); eset(c, &e, sz, r); return;   /* NEG */
    case 0x6:
        if (!sz) ILLEGAL();                                                                   /* MOVE to SR */
        ea(c, mode, reg, sz, &e); r = ~eget(c, &e, sz); logic(c, r, sz); eset(c, &e, sz, r); return;   /* NOT */
    case 0x8:
        if ((op & 0xFFC0) == 0x4800) { ea(c, mode, reg, 1, &e); v = eget(c, &e, 1); r = bcd_sub(c, 0, v); eset(c, &e, 1, r); return; }   /* NBCD */
        break;
    case 0xA:
        if (!sz) { ea(c, mode, reg, 1, &e); v = eget(c, &e, 1); logic(c, v, 1); eset(c, &e, 1, v | 0x80); return; }   /* TAS */
        ea(c, mode, reg, sz, &e); logic(c, eget(c, &e, sz), sz); return;                      /* TST */
    default: break;
    }
    ILLEGAL();
}

static void group5(m68k *c, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7, sz = size_code[(op >> 6) & 3];
    ea_t e;
    if (!sz) {
        int t = cond(c, (op >> 8) & 15);
        if (mode == 1) {                                                   /* DBcc */
            uint32_t base = c->pc;
            int16_t disp = (int16_t)f16(c);
            if (!t) {
                uint16_t w = (uint16_t)(c->d[reg] - 1);
                c->d[reg] = (c->d[reg] & 0xFFFF0000u) | w;
                if (w != 0xFFFF) c->pc = base + (uint32_t)disp;
            }
            return;
        }
        if (mode == 7 && reg >= 2) {                                       /* TRAPcc */
            if (reg == 2) c->pc += 2; else if (reg == 3) c->pc += 4;
            if (t) m68k_fault(c, M68K_FAULT_TRAP, c->insn_pc, "TRAPcc");
            return;
        }
        ea(c, mode, reg, 1, &e); eset(c, &e, 1, t ? 0xFF : 0);           /* Scc */
        return;
    }
    {
        uint32_t q = (op >> 9) & 7, v, r;
        if (!q) q = 8;
        ea(c, mode, reg, sz, &e);
        if (e.k == EA_A) { if (op & 0x100) c->a[reg] -= q; else c->a[reg] += q; return; }   /* ADDQ/SUBQ to An: no flags */
        v = eget(c, &e, sz);
        r = (op & 0x100) ? sub(c, v, q, sz, 0) : add(c, v, q, sz, 0);
        eset(c, &e, sz, r);
    }
}

static void branch(m68k *c, uint16_t op)
{
    uint32_t base = c->pc;
    int32_t disp = (int8_t)op;
    int cc = (op >> 8) & 15;
    if ((op & 0xFF) == 0) disp = (int16_t)f16(c);
    else if ((op & 0xFF) == 0xFF) disp = (int32_t)f32(c);
    if (cc == 1) { push32(c, c->pc); c->pc = base + (uint32_t)disp; return; }          /* BSR */
    if (cond(c, cc)) c->pc = base + (uint32_t)disp;
}

/* OR / AND / SUB / ADD / CMP / EOR register forms; op: 0 or, 1 and, 2 sub, 3 add */
static uint32_t alu(m68k *c, int kind, uint32_t d, uint32_t s, int sz)
{
    uint32_t r;
    switch (kind) {
    case 0: r = d | s; logic(c, r, sz); return r;
    case 1: r = d & s; logic(c, r, sz); return r;
    case 2: return sub(c, d, s, sz, 0);
    default: return add(c, d, s, sz, 0);
    }
}
static void alu_group(m68k *c, uint16_t op, int kind)
{
    int mode = (op >> 3) & 7, reg = op & 7, dn = (op >> 9) & 7, sz = size_code[(op >> 6) & 3];
    ea_t e;
    uint32_t v, r;
    if (!(op & 0x100)) {                            /* <ea> op Dn -> Dn */
        ea(c, mode, reg, sz, &e);
        v = eget(c, &e, sz);
        r = alu(c, kind, c->d[dn], v, sz);
        c->d[dn] = sz == 4 ? r : (c->d[dn] & ~MASK(sz)) | r;
    } else {                                        /* Dn op <ea> -> <ea> */
        ea(c, mode, reg, sz, &e);
        if (e.k != EA_M) ILLEGAL();
        v = eget(c, &e, sz);
        r = alu(c, kind, v, c->d[dn], sz);
        eset(c, &e, sz, r);
    }
}
static void addsub(m68k *c, uint16_t op, int isadd)
{
    int opm = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7, rx = (op >> 9) & 7;
    ea_t e;
    if (opm == 3 || opm == 7) {                     /* ADDA / SUBA */
        int sz = opm == 3 ? 2 : 4;
        uint32_t s;
        ea(c, mode, reg, sz, &e);
        s = (uint32_t)sext(eget(c, &e, sz), sz);
        c->a[rx] = isadd ? c->a[rx] + s : c->a[rx] - s;
        return;
    }
    if (opm >= 4 && mode < 2) {                     /* ADDX / SUBX */
        int sz = size_code[opm - 4];
        uint32_t s, d, r;
        if (mode == 0) {
            r = isadd ? add(c, c->d[rx], c->d[reg], sz, 1) : sub(c, c->d[rx], c->d[reg], sz, 1);
            c->d[rx] = sz == 4 ? r : (c->d[rx] & ~MASK(sz)) | r;
        } else {
            ea_t es, ed;
            ea(c, 4, reg, sz, &es); s = eget(c, &es, sz);
            ea(c, 4, rx, sz, &ed); d = eget(c, &ed, sz);
            r = isadd ? add(c, d, s, sz, 1) : sub(c, d, s, sz, 1);
            eset(c, &ed, sz, r);
        }
        return;
    }
    if (mode == 1 && size_code[opm & 3] == 1) ILLEGAL();
    alu_group(c, op, isadd ? 3 : 2);
}

static void group8(m68k *c, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7, dn = (op >> 9) & 7;
    ea_t e;
    if ((op & 0x1F0) == 0x100) {                    /* SBCD */
        if (mode == 0) { c->d[dn] = (c->d[dn] & ~0xFFu) | bcd_sub(c, c->d[dn] & 0xFF, c->d[reg] & 0xFF); return; }
        { ea_t es, ed; uint32_t s, d; ea(c, 4, reg, 1, &es); s = eget(c, &es, 1); ea(c, 4, dn, 1, &ed); d = eget(c, &ed, 1); eset(c, &ed, 1, bcd_sub(c, d, s)); }
        return;
    }
    if ((op & 0x1F0) == 0x140 || (op & 0x1F0) == 0x180) ILLEGAL();   /* PACK / UNPK */
    if ((op & 0xC0) == 0xC0) {                      /* DIVU.W / DIVS.W */
        uint32_t s;
        ea(c, mode, reg, 2, &e);
        s = eget(c, &e, 2);
        if (!s) m68k_fault(c, M68K_FAULT_DIV0, c->insn_pc, "division by zero");
        c->c = 0;
        if (!(op & 0x100)) {
            uint32_t q = c->d[dn] / s, r = c->d[dn] % s;
            if (q > 0xFFFF) { c->v = 1; return; }
            c->d[dn] = (r << 16) | q;
            c->n = (q >> 15) & 1; c->z = q == 0; c->v = 0;
        } else {
            int32_t n = (int32_t)c->d[dn], sv = (int16_t)s, q, r;
            if (n == INT32_MIN && sv == -1) { c->v = 1; return; }
            q = n / sv; r = n % sv;
            if (q != (int16_t)q) { c->v = 1; return; }
            c->d[dn] = ((uint32_t)r << 16) | ((uint32_t)q & 0xFFFF);
            c->n = q < 0; c->z = q == 0; c->v = 0;
        }
        return;
    }
    alu_group(c, op, 0);
}

static void groupB(m68k *c, uint16_t op)
{
    int opm = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7, dn = (op >> 9) & 7;
    ea_t e;
    if (opm == 3 || opm == 7) {                     /* CMPA */
        int sz = opm == 3 ? 2 : 4;
        uint32_t s;
        ea(c, mode, reg, sz, &e);
        s = (uint32_t)sext(eget(c, &e, sz), sz);
        sub(c, c->a[dn], s, 4, 2);
        return;
    }
    if (opm < 3) {                                  /* CMP */
        int sz = size_code[opm];
        uint32_t s;
        ea(c, mode, reg, sz, &e);
        s = eget(c, &e, sz);
        sub(c, c->d[dn], s, sz, 2);
        return;
    }
    {
        int sz = size_code[opm - 4];
        uint32_t s, d;
        if (mode == 1) {                            /* CMPM (Ay)+,(Ax)+ */
            ea_t es, ed;
            ea(c, 3, reg, sz, &es); s = eget(c, &es, sz);
            ea(c, 3, dn, sz, &ed); d = eget(c, &ed, sz);
            sub(c, d, s, sz, 2);
            return;
        }
        ea(c, mode, reg, sz, &e);                   /* EOR Dn,<ea> */
        if (e.k == EA_A || e.k == EA_I) ILLEGAL();
        d = eget(c, &e, sz) ^ c->d[dn];
        logic(c, d, sz);
        eset(c, &e, sz, d);
    }
}

static void groupC(m68k *c, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7, dn = (op >> 9) & 7;
    ea_t e;
    if ((op & 0x1F0) == 0x100) {                    /* ABCD */
        if (mode == 0) { c->d[dn] = (c->d[dn] & ~0xFFu) | bcd_add(c, c->d[dn] & 0xFF, c->d[reg] & 0xFF); return; }
        { ea_t es, ed; uint32_t s, d; ea(c, 4, reg, 1, &es); s = eget(c, &es, 1); ea(c, 4, dn, 1, &ed); d = eget(c, &ed, 1); eset(c, &ed, 1, bcd_add(c, d, s)); }
        return;
    }
    if ((op & 0x1F8) == 0x140) { uint32_t t = c->d[dn]; c->d[dn] = c->d[reg]; c->d[reg] = t; return; }   /* EXG Dx,Dy */
    if ((op & 0x1F8) == 0x148) { uint32_t t = c->a[dn]; c->a[dn] = c->a[reg]; c->a[reg] = t; return; }   /* EXG Ax,Ay */
    if ((op & 0x1F8) == 0x188) { uint32_t t = c->d[dn]; c->d[dn] = c->a[reg]; c->a[reg] = t; return; }   /* EXG Dx,Ay */
    if ((op & 0xC0) == 0xC0) {                      /* MULU.W / MULS.W */
        uint32_t s, r;
        ea(c, mode, reg, 2, &e);
        s = eget(c, &e, 2);
        r = (op & 0x100) ? (uint32_t)((int32_t)(int16_t)c->d[dn] * (int32_t)(int16_t)s) : (c->d[dn] & 0xFFFF) * s;
        c->d[dn] = r;
        logic(c, r, 4);
        return;
    }
    alu_group(c, op, 1);
}

static void groupE(m68k *c, uint16_t op)
{
    if ((op & 0xF8C0) == 0xE8C0) { bitfield(c, op); return; }
    if ((op & 0xC0) == 0xC0) {                      /* memory shift by one, word */
        ea_t e;
        uint32_t v;
        ea(c, (op >> 3) & 7, op & 7, 2, &e);
        if (e.k != EA_M) ILLEGAL();
        v = eget(c, &e, 2);
        eset(c, &e, 2, shift(c, (op >> 9) & 3, (op >> 8) & 1, v, 1, 2));
        return;
    }
    {
        int sz = size_code[(op >> 6) & 3], reg = op & 7, cr = (op >> 9) & 7;
        unsigned cnt = (op & 0x20) ? (c->d[cr] & 63) : (cr ? (unsigned)cr : 8u);
        uint32_t r = shift(c, (op >> 3) & 3, (op >> 8) & 1, c->d[reg], cnt, sz);
        c->d[reg] = sz == 4 ? r : (c->d[reg] & ~MASK(sz)) | r;
    }
}

void m68k_step(m68k *c)
{
    uint16_t op;
    c->insn_pc = c->pc;
    op = f16(c);
    c->icount++;
    switch (op >> 12) {
    case 0x0: group0(c, op); return;
    case 0x1: case 0x2: case 0x3: move(c, op); return;
    case 0x4: group4(c, op); return;
    case 0x5: group5(c, op); return;
    case 0x6: branch(c, op); return;
    case 0x7:
        if (op & 0x100) ILLEGAL();
        c->d[(op >> 9) & 7] = (uint32_t)(int8_t)op; logic(c, (uint32_t)(int8_t)op, 4); return;   /* MOVEQ */
    case 0x8: group8(c, op); return;
    case 0x9: addsub(c, op, 0); return;
    case 0xB: groupB(c, op); return;
    case 0xC: groupC(c, op); return;
    case 0xD: addsub(c, op, 1); return;
    case 0xE: groupE(c, op); return;
    case 0xF: fpu(c, op); return;
    default: ILLEGAL();                             /* line A */
    }
}

int m68k_run(m68k *c)
{
    sigjmp_buf jb;
    void *old = c->jb;
    c->jb = &jb;
    if (sigsetjmp(jb, 0)) { c->jb = old; return c->fault; }
    while (!c->stop) m68k_step(c);
    c->jb = old;
    return c->fault;
}

void m68k_describe(const m68k *c, char *b, size_t n)
{
    snprintf(b, n, "%s at pc=%08x (addr %08x) d0-7=%08x %08x %08x %08x %08x %08x %08x %08x a0-7=%08x %08x %08x %08x %08x %08x %08x %08x ccr=%02x",
             c->fault_msg, c->insn_pc, c->fault_addr, c->d[0], c->d[1], c->d[2], c->d[3], c->d[4], c->d[5], c->d[6], c->d[7],
             c->a[0], c->a[1], c->a[2], c->a[3], c->a[4], c->a[5], c->a[6], c->a[7], m68k_sr(c));
}
