/* 68100EC64 reference interpreter. Long mode follows docs/armithlon/68100ec64.md; with long
 * mode off it matches Musashi's 68040 integer instructions (tests/ec64_diff_tests.c).
 * Anything not listed there stops with QEC64_UNIMPLEMENTED rather than guessing. */
#include "qrx68100ec64.h"
#include <setjmp.h>
#include <string.h>

#define XF 0x10
#define NF 0x08
#define ZF 0x04
#define VF 0x02
#define CF 0x01
#define PFX_W 8
#define PFX_R 4
#define PFX_X 2
#define PFX_B 1
#define PFX_N 0x10   /* MOVE leaves the flags alone */
#define CR_USP 0x800
#define CR_VBR 0x801
#define CR_M64 0x80F
#define CR_COREID 0x80E
#define CR_IPI 0x80D
#define CR_TIMER 0x80C
#define CR_NCORES 0x80B
#define CR_START_SP 0x80A
#define CR_CONSOLE 0x8F0
#define CR_HALT 0x8F1

typedef struct {
    qec64_cpu *c;
    jmp_buf out;
    uint16_t op;
    unsigned pfx;
    uint64_t start_pc;
} ctx;

static inline bool vectored(const qec64_cpu *c) { return c->m64 && c->vbr_set; }

/* Where an effective address leads. */
enum { LOC_D, LOC_A, LOC_MEM, LOC_IMM };
typedef struct { int kind; unsigned reg; uint64_t addr; uint64_t imm; } loc;

static inline uint64_t amask(const ctx *x) { return x->c->m64 ? UINT64_MAX : 0xFFFFFFFFu; }
static inline uint64_t size_mask(unsigned s) { return s == 8 ? UINT64_MAX : (UINT64_C(1) << (s * 8)) - 1; }
static inline uint64_t sign_bit(unsigned s) { return UINT64_C(1) << (s * 8 - 1); }
static inline int64_t sext(uint64_t v, unsigned s)
{
    return s == 8 ? (int64_t)v : (int64_t)((v & size_mask(s)) ^ sign_bit(s)) - (int64_t)sign_bit(s);
}
static _Noreturn void stop_with(ctx *x, enum qec64_status s) { x->c->status = s; x->c->fault_pc = x->start_pc; longjmp(x->out, 1); }
static _Noreturn void unimpl(ctx *x) { stop_with(x, QEC64_UNIMPLEMENTED); }

/* ---- memory: big endian. Aligned accesses keep the 68040's order between cores. */
static uint8_t *mem_at(ctx *x, uint64_t a, unsigned n)
{
    qec64_cpu *c = x->c;
    if (a >= c->ram_size || n > c->ram_size - a) { c->fault_addr = a; stop_with(x, QEC64_BUS_FAULT); }
    return c->ram + a;
}
static uint64_t rd(ctx *x, uint64_t a, unsigned n)
{
    uint8_t *p = mem_at(x, a, n);
    switch (n) {
    case 1: return __atomic_load_n(p, __ATOMIC_ACQUIRE);
    case 2: if (!(a & 1)) return __builtin_bswap16(__atomic_load_n((uint16_t *)p, __ATOMIC_ACQUIRE)); break;
    case 4: if (!(a & 3)) return __builtin_bswap32(__atomic_load_n((uint32_t *)p, __ATOMIC_ACQUIRE)); break;
    default: if (!(a & 7)) return __builtin_bswap64(__atomic_load_n((uint64_t *)p, __ATOMIC_ACQUIRE)); break;
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}
static void wr(ctx *x, uint64_t a, unsigned n, uint64_t v)
{
    uint8_t *p = mem_at(x, a, n);
    switch (n) {
    case 1: __atomic_store_n(p, (uint8_t)v, __ATOMIC_RELEASE); return;
    case 2: if (!(a & 1)) { __atomic_store_n((uint16_t *)p, __builtin_bswap16((uint16_t)v), __ATOMIC_RELEASE); return; } break;
    case 4: if (!(a & 3)) { __atomic_store_n((uint32_t *)p, __builtin_bswap32((uint32_t)v), __ATOMIC_RELEASE); return; } break;
    default: if (!(a & 7)) { __atomic_store_n((uint64_t *)p, __builtin_bswap64(v), __ATOMIC_RELEASE); return; } break;
    }
    for (unsigned i = 0; i < n; i++) p[i] = (uint8_t)(v >> ((n - 1 - i) * 8));
}
static uint16_t fetch16(ctx *x) { uint16_t v = (uint16_t)rd(x, x->c->pc, 2); x->c->pc = (x->c->pc + 2) & amask(x); return v; }
static uint32_t fetch32(ctx *x) { uint32_t h = fetch16(x); return h << 16 | fetch16(x); }
static uint64_t fetch64(ctx *x) { uint64_t h = fetch32(x); return h << 32 | fetch32(x); }

/* ---- registers */
static inline void dset(ctx *x, unsigned r, unsigned s, uint64_t v)
{
    uint64_t *d = &x->c->d[r];
    if (s == 1) *d = (*d & ~UINT64_C(0xFF)) | (v & 0xFF);
    else if (s == 2) *d = (*d & ~UINT64_C(0xFFFF)) | (v & 0xFFFF);
    else if (s == 4) *d = (uint32_t)v;
    else *d = v;
}
static inline void aset(ctx *x, unsigned r, uint64_t v) { x->c->a[r] = v & amask(x); }
static inline unsigned regR(const ctx *x, unsigned r) { return r | ((x->pfx & PFX_R) ? 8 : 0); }
static inline unsigned regB(const ctx *x, unsigned r) { return r | ((x->pfx & PFX_B) ? 8 : 0); }
static inline unsigned ptr_size(const ctx *x) { return x->c->m64 ? 8 : 4; }
/* .L becomes .Q under the W prefix. */
static inline unsigned wide(const ctx *x, unsigned s) { return s == 4 && (x->pfx & PFX_W) ? 8 : s; }
static inline unsigned size_field(const ctx *x, unsigned f)
{
    if (f == 3) unimpl((ctx *)x);
    return wide(x, 1u << f);
}
static void push(ctx *x, uint64_t v) { unsigned n = ptr_size(x); aset(x, 7, x->c->a[7] - n); wr(x, x->c->a[7], n, v); }
static uint64_t pop(ctx *x) { unsigned n = ptr_size(x); uint64_t v = rd(x, x->c->a[7], n); aset(x, 7, x->c->a[7] + n); return v; }

/* ---- effective addresses */
static uint64_t index_ext(ctx *x, uint64_t base, uint16_t ext)
{
    unsigned r = (ext >> 12) & 7;
    if (x->pfx & PFX_X) r |= 8;
    uint64_t idx = (ext & 0x8000) ? x->c->a[r] : x->c->d[r];
    int64_t v = (ext & 0x800) ? (x->c->m64 ? (int64_t)idx : (int32_t)idx) : (int16_t)idx;
    v *= 1 << ((ext >> 9) & 3);
    if (!(ext & 0x100)) return base + (uint64_t)v + (uint64_t)(int64_t)(int8_t)ext;
    /* 68020 full format, without the memory indirect forms */
    if (ext & 0x8) unimpl(x);
    if (ext & 7) unimpl(x);
    if (ext & 0x40) v = 0;
    if (ext & 0x80) base = 0;
    int64_t bd = 0;
    switch ((ext >> 4) & 3) {
    case 0: unimpl(x);
    case 2: bd = (int16_t)fetch16(x); break;
    case 3: bd = (int32_t)fetch32(x); break;
    default: break;
    }
    return base + (uint64_t)v + (uint64_t)bd;
}
static loc ea(ctx *x, unsigned mode, unsigned reg, unsigned s)
{
    qec64_cpu *c = x->c;
    loc l = { LOC_MEM, 0, 0, 0 };
    unsigned r = regB(x, reg);
    unsigned step = (s == 1 && reg == 7 && !(x->pfx & PFX_B)) ? 2 : s;
    switch (mode) {
    case 0: l.kind = LOC_D; l.reg = r; return l;
    case 1: l.kind = LOC_A; l.reg = r; return l;
    case 2: l.addr = c->a[r]; return l;
    case 3: l.addr = c->a[r]; aset(x, r, c->a[r] + step); return l;
    case 4: aset(x, r, c->a[r] - step); l.addr = c->a[r]; return l;
    case 5: l.addr = c->a[r] + (uint64_t)(int64_t)(int16_t)fetch16(x); break;
    case 6: { uint16_t e = fetch16(x); l.addr = index_ext(x, c->a[r], e); break; }
    default:
        switch (reg) {
        case 0: l.addr = (uint64_t)(int64_t)(int16_t)fetch16(x); break;
        case 1: l.addr = (uint64_t)(int64_t)(int32_t)fetch32(x); break;
        case 2: { uint64_t b = c->pc; l.addr = b + (uint64_t)(int64_t)(int16_t)fetch16(x); break; }
        case 3: { uint64_t b = c->pc; uint16_t e = fetch16(x); l.addr = index_ext(x, b, e); break; }
        case 4:
            l.kind = LOC_IMM;
            if (s == 1) l.imm = fetch16(x) & 0xFF;
            else if (s == 2) l.imm = fetch16(x);
            else if (s == 4) l.imm = fetch32(x);
            else l.imm = fetch64(x);
            return l;
        default: unimpl(x);
        }
    }
    l.addr &= amask(x);
    return l;
}
static uint64_t ld(ctx *x, const loc *l, unsigned s)
{
    switch (l->kind) {
    case LOC_D: return x->c->d[l->reg] & size_mask(s);
    case LOC_A: return x->c->a[l->reg] & size_mask(s);
    case LOC_IMM: return l->imm;
    default: return rd(x, l->addr, s);
    }
}
static void st(ctx *x, const loc *l, unsigned s, uint64_t v)
{
    if (l->kind == LOC_D) dset(x, l->reg, s, v);
    else if (l->kind == LOC_MEM) wr(x, l->addr, s, v);
    else if (l->kind == LOC_A) aset(x, l->reg, v);
    else unimpl(x);
}
/* PC relative destinations exist in long mode only. */
static inline unsigned last_alterable(const ctx *x) { return x->c->m64 ? 3 : 1; }
static void need_alterable_mem(ctx *x, unsigned mode, unsigned reg)
{
    if (mode < 2 || (mode == 7 && reg > last_alterable(x))) unimpl(x);
}
static void need_data_alterable(ctx *x, unsigned mode, unsigned reg)
{
    if (mode == 1 || (mode == 7 && reg > last_alterable(x))) unimpl(x);
}

/* ---- flags */
static inline void set_nz(qec64_cpu *c, uint64_t v, unsigned s)
{
    v &= size_mask(s);
    c->ccr = (uint8_t)((c->ccr & (XF | VF | CF)) | (v == 0 ? ZF : 0) | ((v & sign_bit(s)) ? NF : 0));
}
static inline void logic_flags(qec64_cpu *c, uint64_t v, unsigned s)
{
    c->ccr &= XF;
    set_nz(c, v, s);
}
static uint64_t do_add(qec64_cpu *c, uint64_t a, uint64_t b, unsigned s, unsigned carry, bool keep_z)
{
    uint64_t m = size_mask(s), r = (a + b + carry) & m, sb = sign_bit(s);
    bool cy = (((a & b) | ((a | b) & ~r)) & sb) != 0;
    bool ov = ((a ^ r) & (b ^ r) & sb) != 0;
    uint8_t z = keep_z ? (r ? 0 : (c->ccr & ZF)) : (r ? 0 : ZF);
    c->ccr = (uint8_t)((cy ? XF | CF : 0) | (ov ? VF : 0) | z | ((r & sb) ? NF : 0));
    return r;
}
/* a - b. keepx is for CMP, which leaves X alone. */
static uint64_t do_sub(qec64_cpu *c, uint64_t a, uint64_t b, unsigned s, unsigned borrow, bool keep_z, bool keepx)
{
    uint64_t m = size_mask(s), r = (a - b - borrow) & m, sb = sign_bit(s);
    bool cy = (((~a & b) | ((~a | b) & r)) & sb) != 0;
    bool ov = ((a ^ b) & (a ^ r) & sb) != 0;
    uint8_t z = keep_z ? (r ? 0 : (c->ccr & ZF)) : (r ? 0 : ZF);
    uint8_t xf = keepx ? (c->ccr & XF) : (cy ? XF : 0);
    c->ccr = (uint8_t)(xf | (cy ? CF : 0) | (ov ? VF : 0) | z | ((r & sb) ? NF : 0));
    return r;
}
static bool cond(const qec64_cpu *c, unsigned cc)
{
    bool n = c->ccr & NF, z = c->ccr & ZF, v = c->ccr & VF, cy = c->ccr & CF;
    switch (cc) {
    case 0: return true;
    case 1: return false;
    case 2: return !cy && !z;
    case 3: return cy || z;
    case 4: return !cy;
    case 5: return cy;
    case 6: return !z;
    case 7: return z;
    case 8: return !v;
    case 9: return v;
    case 10: return !n;
    case 11: return n;
    case 12: return n == v;
    case 13: return n != v;
    case 14: return !z && n == v;
    default: return z || n != v;
    }
}

/* ---- status register */
static uint16_t get_sr(const qec64_cpu *c)
{
    return (uint16_t)((c->supervisor ? 0x2000u : 0) | (uint16_t)(c->ipl << 8) | c->ccr);
}
static void set_sr(ctx *x, uint16_t v)
{
    qec64_cpu *c = x->c;
    bool sup = (v & 0x2000) != 0;
    if (sup != c->supervisor) {
        uint64_t t = c->a[7];
        c->a[7] = c->other_sp;
        c->other_sp = t;
        c->supervisor = sup;
    }
    c->ipl = (v >> 8) & 7;
    c->ccr = v & 0x1F;
}

/* ---- instruction groups */
static void op_movep(ctx *x)
{
    qec64_cpu *c = x->c;
    unsigned op = x->op, d = regR(x, (op >> 9) & 7), an = regB(x, op & 7), n = (op & 0x40) ? 4 : 2;
    uint64_t a = (c->a[an] + (uint64_t)(int64_t)(int16_t)fetch16(x)) & amask(x);
    if (op & 0x80) {
        for (unsigned i = 0; i < n; i++) wr(x, (a + i * 2) & amask(x), 1, (c->d[d] >> ((n - 1 - i) * 8)) & 0xFF);
    } else {
        uint64_t v = 0;
        for (unsigned i = 0; i < n; i++) v = v << 8 | rd(x, (a + i * 2) & amask(x), 1);
        dset(x, d, n, v);
    }
}
static void op_cmp2(ctx *x)
{
    qec64_cpu *c = x->c;
    unsigned op = x->op, f = (op >> 9) & 3, mode = (op >> 3) & 7, reg = op & 7;
    if (f == 3 || mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg == 4)) unimpl(x);
    unsigned s = wide(x, 1u << f);
    uint16_t e = fetch16(x);
    unsigned r = (e >> 12) & 7;
    if (x->pfx & PFX_R) r |= 8;
    bool is_a = e & 0x8000, trap = e & 0x800;
    loc l = ea(x, mode, reg, s);
    /* The signed compare and the register rules are Musashi's: an address register is only
     * masked to the size, a data register is sign extended. */
    int64_t lo = sext(rd(x, l.addr, s), s), hi = sext(rd(x, (l.addr + s) & amask(x), s), s);
    int64_t v = is_a ? (s == 8 ? (int64_t)c->a[r] : (int64_t)(c->a[r] & size_mask(s))) : sext(c->d[r], s);
    if (s == 4) v = (int32_t)v;
    bool eq = v == lo || v == hi;
    bool out = v < lo || v > hi;
    c->ccr = (uint8_t)((c->ccr & ~(ZF | CF)) | (eq ? ZF : 0) | (out ? CF : 0));
    if (out && trap) stop_with(x, QEC64_CHK);
}
static void op_cas2(ctx *x)
{
    qec64_cpu *c = x->c;
    unsigned s = wide(x, (x->op & 0x200) ? 4 : 2);
    uint16_t e1 = fetch16(x), e2 = fetch16(x);
    unsigned dc1 = e1 & 7, du1 = (e1 >> 6) & 7, dc2 = e2 & 7, du2 = (e2 >> 6) & 7;
    uint64_t a1 = (e1 & 0x8000) ? c->a[(e1 >> 12) & 7] : c->d[(e1 >> 12) & 7];
    uint64_t a2 = (e2 & 0x8000) ? c->a[(e2 >> 12) & 7] : c->d[(e2 >> 12) & 7];
    if (!c->m64) { a1 = (uint32_t)a1; a2 = (uint32_t)a2; }
    /* Not atomic, see the specification. */
    uint64_t v1 = rd(x, a1, s), v2 = rd(x, a2, s);
    do_sub(c, v1, c->d[dc1] & size_mask(s), s, 0, false, true);
    if (c->ccr & ZF) {
        do_sub(c, v2, c->d[dc2] & size_mask(s), s, 0, false, true);
        if (c->ccr & ZF) {
            wr(x, a1, s, c->d[du1]);
            wr(x, a2, s, c->d[du2]);
            return;
        }
    }
    /* Both registers take the memory values: the second first, as Musashi orders them. */
    dset(x, dc1, s, v1);
    dset(x, dc2, s, v2);
}
static uint64_t bcd_add(qec64_cpu *c, uint64_t dst, uint64_t src)
{
    unsigned xb = (c->ccr & XF) ? 1 : 0;
    unsigned res = (unsigned)((src & 0xF) + (dst & 0xF) + xb), v = ~res;
    if (res > 9) res += 6;
    res += (unsigned)((src & 0xF0) + (dst & 0xF0));
    bool cy = res > 0x99;
    if (cy) res -= 0xA0;
    v &= res;
    res &= 0xFF;
    c->ccr = (uint8_t)((cy ? XF | CF : 0) | ((v & 0x80) ? VF : 0) | ((res & 0x80) ? NF : 0) | (res ? 0 : (c->ccr & ZF)));
    return res;
}
static uint64_t bcd_sub(qec64_cpu *c, uint64_t dst, uint64_t src)
{
    unsigned xb = (c->ccr & XF) ? 1 : 0;
    unsigned res = (unsigned)((dst & 0xF) - (src & 0xF) - xb), v = ~res;
    if (res > 9) res -= 6;
    res += (unsigned)((dst & 0xF0) - (src & 0xF0));
    bool cy = res > 0x99;
    if (cy) res += 0xA0;
    res &= 0xFF;
    v &= res;
    c->ccr = (uint8_t)((cy ? XF | CF : 0) | ((v & 0x80) ? VF : 0) | ((res & 0x80) ? NF : 0) | (res ? 0 : (c->ccr & ZF)));
    return res;
}
/* ABCD (line C), SBCD, PACK, UNPK (line 8), register or -(An) forms */
static void op_bcd(ctx *x)
{
    qec64_cpu *c = x->c;
    unsigned op = x->op, line = op >> 12, kind = (op >> 4) & 0x1F, ry = op & 7, rx = (op >> 9) & 7;
    bool mem = op & 8;
    if (kind == 0x10) {   /* ABCD, SBCD */
        if (!mem) { dset(x, rx, 1, line == 12 ? bcd_add(c, c->d[rx] & 0xFF, c->d[ry] & 0xFF) : bcd_sub(c, c->d[rx] & 0xFF, c->d[ry] & 0xFF)); return; }
        loc a = ea(x, 4, ry, 1), b = ea(x, 4, rx, 1);
        uint64_t sv = rd(x, a.addr, 1), dv = rd(x, b.addr, 1);
        wr(x, b.addr, 1, line == 12 ? bcd_add(c, dv, sv) : bcd_sub(c, dv, sv));
        return;
    }
    if (line != 8) unimpl(x);
    uint16_t adj = fetch16(x);
    if (kind == 0x14) {   /* PACK */
        unsigned v;
        if (!mem) { v = ((uint16_t)c->d[ry] + adj) & 0xFFFF; dset(x, rx, 1, ((v >> 4) & 0xF0) | (v & 0x0F)); return; }
        loc a = ea(x, 4, ry, 1); unsigned hi = (unsigned)rd(x, a.addr, 1);
        loc a2 = ea(x, 4, ry, 1); unsigned lo = (unsigned)rd(x, a2.addr, 1);
        v = (((hi << 8) | lo) + adj) & 0xFFFF;
        loc b = ea(x, 4, rx, 1);
        wr(x, b.addr, 1, ((v >> 4) & 0xF0) | (v & 0x0F));
        return;
    }
    if (kind == 0x18) {   /* UNPK */
        unsigned v;
        if (!mem) { v = (unsigned)c->d[ry] & 0xFF; dset(x, rx, 2, (((v << 4) & 0x0F00) | (v & 0x0F)) + adj); return; }
        loc a = ea(x, 4, ry, 1); v = (unsigned)rd(x, a.addr, 1);
        v = (((v << 4) & 0x0F00) | (v & 0x0F)) + adj;
        loc b = ea(x, 4, rx, 1); wr(x, b.addr, 1, (v >> 8) & 0xFF);
        loc b2 = ea(x, 4, rx, 1); wr(x, b2.addr, 1, v & 0xFF);
        return;
    }
    unimpl(x);
}
/* Bit fields, up to 32 bits wide also in long mode. */
static void op_bitfield(ctx *x)
{
    qec64_cpu *c = x->c;
    unsigned op = x->op, kind = (op >> 8) & 7, mode = (op >> 3) & 7, reg = op & 7;
    uint16_t e = fetch16(x);
    int64_t off = (e & 0x800) ? (int64_t)(int32_t)c->d[(e >> 6) & 7] : (int64_t)((e >> 6) & 31);
    unsigned w = ((e & 0x20) ? (unsigned)c->d[e & 7] & 31 : e & 31);
    if (!w) w = 32;
    unsigned dr = (e >> 12) & 7;
    bool reads_only = kind == 0 || kind == 1 || kind == 3 || kind == 5;
    if (mode == 1 || mode == 3 || mode == 4) unimpl(x);
    if (reads_only ? (mode == 7 && reg == 4) : (mode == 7 && reg > last_alterable(x))) unimpl(x);
    uint64_t field;
    uint32_t reg_val = 0;
    unsigned r0 = 0;
    uint64_t addr = 0;
    unsigned bytes = 0, shift_low = 0;
    uint64_t mem_val = 0;
    if (mode == 0) {
        r0 = regB(x, reg);
        reg_val = (uint32_t)c->d[r0];
        unsigned o = (unsigned)off & 31;
        /* rotate so the field sits at the top */
        uint32_t rot = o ? (reg_val << o) | (reg_val >> (32 - o)) : reg_val;
        field = w == 32 ? rot : rot >> (32 - w);
        off = o;
    } else {
        loc l = ea(x, mode, reg, 1);
        addr = (l.addr + (uint64_t)(off >> 3)) & amask(x);
        unsigned o = (unsigned)(off & 7);
        bytes = (o + w + 7) / 8;
        for (unsigned i = 0; i < bytes; i++) mem_val = mem_val << 8 | rd(x, (addr + i) & amask(x), 1);
        shift_low = bytes * 8 - o - w;
        field = (mem_val >> shift_low) & size_mask(1) * 0 + ((mem_val >> shift_low) & ((w == 32) ? UINT64_C(0xFFFFFFFF) : ((UINT64_C(1) << w) - 1)));
    }
    uint64_t mask = w == 32 ? UINT64_C(0xFFFFFFFF) : (UINT64_C(1) << w) - 1;
    field &= mask;
    bool top = (field >> (w - 1)) & 1;
    uint64_t out = field;
    bool write = false;
    if (kind == 7) {   /* BFINS takes its flags from the inserted value */
        out = c->d[dr] & mask;
        top = (out >> (w - 1)) & 1;
        c->ccr = (uint8_t)((c->ccr & XF) | (top ? NF : 0) | (out ? 0 : ZF));
        write = true;
    } else {
        c->ccr = (uint8_t)((c->ccr & XF) | (top ? NF : 0) | (field ? 0 : ZF));
    }
    switch (kind) {
    case 0: break;   /* BFTST */
    case 1: dset(x, dr, 4, field); break;   /* BFEXTU */
    case 2: out = ~field & mask; write = true; break;   /* BFCHG */
    case 3: dset(x, dr, 4, top ? (field | ~mask) : field); break;   /* BFEXTS */
    case 4: out = 0; write = true; break;   /* BFCLR */
    case 5: {   /* BFFFO */
        unsigned n = 0;
        while (n < w && !((field >> (w - 1 - n)) & 1)) n++;
        dset(x, dr, 4, (uint32_t)((e & 0x800 ? (uint32_t)c->d[(e >> 6) & 7] : (uint32_t)((e >> 6) & 31)) + n));
        break;
    }
    case 6: out = mask; write = true; break;   /* BFSET */
    default: break;
    }
    if (!write) return;
    if (mode == 0) {
        unsigned o = (unsigned)off;
        uint32_t m32 = (uint32_t)(mask << (32 - w)), v32 = (uint32_t)(out << (32 - w));
        if (o) { m32 = m32 >> o | m32 << (32 - o); v32 = v32 >> o | v32 << (32 - o); }
        dset(x, r0, 4, (reg_val & ~m32) | v32);
    } else {
        mem_val = (mem_val & ~(mask << shift_low)) | (out << shift_low);
        for (unsigned i = 0; i < bytes; i++) wr(x, (addr + i) & amask(x), 1, (mem_val >> ((bytes - 1 - i) * 8)) & 0xFF);
    }
}
static void op_immediate(ctx *x)
{
    qec64_cpu *c = x->c;
    unsigned op = x->op, kind = (op >> 9) & 7, f = (op >> 6) & 3, mode = (op >> 3) & 7, reg = op & 7;
    /* ORI, ANDI, EORI to CCR and SR */
    if (mode == 7 && reg == 4 && f < 2 && !(op & 0x100) && (kind == 0 || kind == 1 || kind == 5)) {
        uint16_t imm = fetch16(x);
        unsigned old = f == 0 ? c->ccr : get_sr(c), v = kind == 0 ? old | imm : kind == 1 ? old & imm : old ^ imm;
        if (f == 0) { c->ccr = (uint8_t)(v & 0x1F); return; }
        if (!c->supervisor) stop_with(x, QEC64_PRIVILEGE);
        set_sr(x, (uint16_t)v);
        return;
    }
    if ((op & 0x138) == 0x108) { op_movep(x); return; }
    /* bit operations with a static number, and CAS, share this line */
    if (kind == 4 || (op & 0x100)) {
        unsigned how = (op >> 6) & 3, bit;
        if (op & 0x100) { if (mode == 1) unimpl(x); bit = (unsigned)c->d[regR(x, (op >> 9) & 7)]; }
        else { bit = fetch16(x) & 0xFF; }
        unsigned s = mode == 0 ? wide(x, 4) : 1;
        loc l = ea(x, mode, reg, 1);
        if (how && l.kind != LOC_D && l.kind != LOC_MEM) unimpl(x);
        if (l.kind == LOC_A) unimpl(x);
        bit &= s * 8 - 1;
        uint64_t v = l.kind == LOC_IMM ? l.imm : ld(x, &l, s), m = UINT64_C(1) << bit;
        c->ccr = (uint8_t)((c->ccr & ~ZF) | ((v & m) ? 0 : ZF));
        if (how == 1) st(x, &l, s, v ^ m);
        else if (how == 2) st(x, &l, s, v & ~m);
        else if (how == 3) st(x, &l, s, v | m);
        return;
    }
    if (f == 3) {
        if (!(op & 0x800)) { op_cmp2(x); return; }
        if ((op & 0xFDFF) == 0x0CFC) { op_cas2(x); return; }
        /* CAS Dc,Du,<ea>: $0AC0 .B, $0CC0 .W, $0EC0 .L (.Q under W) */
        unsigned sz = (op >> 9) & 3;
        if (sz == 0) unimpl(x);
        unsigned s = wide(x, 1u << (sz - 1));
        uint16_t e = fetch16(x);
        loc l = ea(x, mode, reg, s);
        need_alterable_mem(x, mode, reg);
        unsigned dc = e & 7, du = (e >> 6) & 7;
        uint8_t *p = mem_at(x, l.addr, s);
        if (s > 1 && (l.addr & (s - 1))) {
            /* Unaligned: not atomic, as CAS across a bus boundary was not on the 68040. */
            uint64_t old = rd(x, l.addr, s);
            do_sub(c, old, c->d[dc] & size_mask(s), s, 0, false, true);
            if (c->ccr & ZF) wr(x, l.addr, s, c->d[du]); else dset(x, dc, s, old);
            return;
        }
        uint64_t want = c->d[dc] & size_mask(s), put = c->d[du] & size_mask(s), old;
        bool ok;
        switch (s) {
        case 1: { uint8_t w = (uint8_t)want; ok = __atomic_compare_exchange_n(p, &w, (uint8_t)put, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); old = w; break; }
        case 2: { uint16_t w = __builtin_bswap16((uint16_t)want); ok = __atomic_compare_exchange_n((uint16_t *)p, &w, __builtin_bswap16((uint16_t)put), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); old = __builtin_bswap16(w); break; }
        case 4: { uint32_t w = __builtin_bswap32((uint32_t)want); ok = __atomic_compare_exchange_n((uint32_t *)p, &w, __builtin_bswap32((uint32_t)put), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); old = __builtin_bswap32(w); break; }
        default: { uint64_t w = __builtin_bswap64(want); ok = __atomic_compare_exchange_n((uint64_t *)p, &w, __builtin_bswap64(put), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); old = __builtin_bswap64(w); break; }
        }
        do_sub(c, old, want, s, 0, false, true);
        if (!ok) dset(x, dc, s, old);
        return;
    }
    if (kind == 7) unimpl(x);
    /* xxI #imm,CCR and SR */
    if (mode == 7 && reg == 4) unimpl(x);
    unsigned s = size_field(x, f);
    uint64_t imm = s == 1 ? (fetch16(x) & 0xFF) : s == 2 ? fetch16(x) : s == 4 ? fetch32(x) : fetch64(x);
    if (mode == 1) unimpl(x);
    if (kind != 6) need_data_alterable(x, mode, reg);
    else if (mode == 7 && reg > 3) unimpl(x);
    loc l = ea(x, mode, reg, s);
    uint64_t v = ld(x, &l, s), r;
    switch (kind) {
    case 0: r = v | imm; logic_flags(c, r, s); break;
    case 1: r = v & imm; logic_flags(c, r, s); break;
    case 2: r = do_sub(c, v, imm, s, 0, false, false); break;
    case 3: r = do_add(c, v, imm, s, 0, false); break;
    case 5: r = v ^ imm; logic_flags(c, r, s); break;
    default: do_sub(c, v, imm, s, 0, false, true); return;
    }
    st(x, &l, s, r);
}
static void op_move(ctx *x)
{
    qec64_cpu *c = x->c;
    unsigned op = x->op, line = op >> 12, s = line == 1 ? 1 : line == 3 ? 2 : wide(x, 4);
    unsigned smode = (op >> 3) & 7, sreg = op & 7, dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
    if (s == 1 && smode == 1) unimpl(x);
    if (dmode == 1 && s == 1) unimpl(x);
    if (dmode == 7 && dreg > last_alterable(x)) unimpl(x);
    loc from = ea(x, smode, sreg, s);
    uint64_t v = ld(x, &from, s);
    unsigned sv = x->pfx;
    if (dmode == 1) {   /* MOVEA: the address register's width, sign extended */
        unsigned r = regR(x, dreg);
        aset(x, r, s == 8 ? v : (uint64_t)sext(v, s));
        return;
    }
    /* The destination's register field is the R one here. */
    x->pfx = (sv & ~PFX_B) | ((sv & PFX_R) ? PFX_B : 0);
    loc to = ea(x, dmode, dreg, s);
    x->pfx = sv;
    if (!(x->pfx & PFX_N)) logic_flags(c, v, s);
    st(x, &to, s, v);
}
static void op_movem(ctx *x)
{
    qec64_cpu *c = x->c;
    unsigned op = x->op, mode = (op >> 3) & 7, reg = op & 7, s = (op & 0x40) ? wide(x, 4) : 2;
    bool to_regs = op & 0x400;
    uint16_t mask = fetch16(x);
    if (mode < 2 || (to_regs && mode == 4) || (!to_regs && mode == 3)) unimpl(x);
    unsigned r = regB(x, reg);
    if (mode == 4) {
        uint64_t a = c->a[r];
        /* Mask order is reversed: bit 0 is A7. Musashi stores the register's old value. */
        for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
            a = (a - s) & amask(x);
            wr(x, a, s, i < 8 ? c->a[7 - i] : c->d[15 - i]);
        }
        aset(x, r, a);
        return;
    }
    uint64_t a;
    if (mode == 3) a = c->a[r];
    else {
        loc l = ea(x, mode, reg, s);
        if (l.kind != LOC_MEM) unimpl(x);
        a = l.addr;
    }
    for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
        if (to_regs) {
            uint64_t v = s == 8 ? rd(x, a, s) : (uint64_t)sext(rd(x, a, s), s);
            if (i < 8) { if (x->c->m64) c->d[i] = v; else c->d[i] = (uint32_t)v; }
            else aset(x, i - 8, v);
        } else {
            wr(x, a, s, i < 8 ? c->d[i] : c->a[i - 8]);
        }
        a = (a + s) & amask(x);
    }
    if (mode == 3) aset(x, r, a);
}
static void op_muldiv_long(ctx *x)
{
    qec64_cpu *c = x->c;
    unsigned op = x->op, mode = (op >> 3) & 7, reg = op & 7;
    uint16_t e = fetch16(x);
    bool is_signed = e & 0x800, dual = e & 0x400;
    unsigned s = wide(x, 4), dl = (e >> 12) & 7, dh = e & 7;
    if (mode == 1) unimpl(x);
    loc l = ea(x, mode, reg, s);
    uint64_t src = ld(x, &l, s);
    if (!(op & 0x40)) {   /* MUL */
        unsigned __int128 r;
        if (s == 8) {
            r = is_signed ? (unsigned __int128)((__int128)(int64_t)src * (int64_t)c->d[dl])
                          : (unsigned __int128)src * c->d[dl];
        } else {
            uint64_t a = c->d[dl] & 0xFFFFFFFFu;
            r = is_signed ? (uint64_t)((int64_t)(int32_t)src * (int64_t)(int32_t)a) : src * a;
        }
        uint64_t lo = s == 8 ? (uint64_t)r : (uint32_t)r, hi = s == 8 ? (uint64_t)(r >> 64) : (uint32_t)((uint64_t)r >> 32);
        c->ccr &= XF;
        if (dual) {
            dset(x, dh, s, hi); dset(x, dl, s, lo);
            if (hi == 0 && lo == 0) c->ccr |= ZF;
            if (hi & sign_bit(s)) c->ccr |= NF;
        } else {
            dset(x, dl, s, lo);
            set_nz(c, lo, s);
            bool over = is_signed ? (hi != ((lo & sign_bit(s)) ? size_mask(s) : 0)) : hi != 0;
            if (over) c->ccr |= VF;
        }
        return;
    }
    /* DIV: dual 32/32 and 64/32 are the 68040's; under W only the single dividend form */
    if (src == 0) stop_with(x, QEC64_DIVIDE_ZERO);
    if (s == 8) {
        if (dual) unimpl(x);
        uint64_t n = c->d[dl];
        c->ccr &= XF;
        if (is_signed) {
            if ((int64_t)n == INT64_MIN && (int64_t)src == -1) { c->ccr |= VF; return; }
            int64_t q = (int64_t)n / (int64_t)src, rem = (int64_t)n % (int64_t)src;
            if (dh != dl) c->d[dh] = (uint64_t)rem;
            c->d[dl] = (uint64_t)q;
            set_nz(c, (uint64_t)q, 8);
        } else {
            uint64_t q = n / src, rem = n % src;
            if (dh != dl) c->d[dh] = rem;
            c->d[dl] = q;
            set_nz(c, q, 8);
        }
        return;
    }
    if (dual) {
        uint64_t n = (c->d[dh] & 0xFFFFFFFFu) << 32 | (c->d[dl] & 0xFFFFFFFFu);
        if (is_signed) {
            if ((int64_t)n == INT64_MIN && (int32_t)src == -1) { c->ccr |= VF; return; }
            int64_t q = (int64_t)n / (int32_t)src, rem = (int64_t)n % (int32_t)src;
            if (q > INT32_MAX || q < INT32_MIN) { c->ccr |= VF; return; }
            dset(x, dh, 4, (uint64_t)rem); dset(x, dl, 4, (uint64_t)q);
            c->ccr &= XF;
            set_nz(c, (uint64_t)q, 4);
        } else {
            uint64_t d = (uint32_t)src, q = n / d, rem = n % d;
            if (q > UINT32_MAX) { c->ccr |= VF; return; }
            dset(x, dh, 4, rem); dset(x, dl, 4, q);
            c->ccr &= XF;
            set_nz(c, q, 4);
        }
        return;
    }
    uint32_t n = (uint32_t)c->d[dl], d = (uint32_t)src;
    if (is_signed) {
        if ((int32_t)n == INT32_MIN && (int32_t)d == -1) { c->ccr |= VF; return; }
        int32_t q = (int32_t)n / (int32_t)d, rem = (int32_t)n % (int32_t)d;
        if (dh != dl) dset(x, dh, 4, (uint32_t)rem);
        dset(x, dl, 4, (uint32_t)q);
        c->ccr &= XF;
        set_nz(c, (uint32_t)q, 4);
    } else {
        uint32_t q = n / d, rem = n % d;
        if (dh != dl) dset(x, dh, 4, rem);
        dset(x, dl, 4, q);
        c->ccr &= XF;
        set_nz(c, q, 4);
    }
}
static void op_line4(ctx *x)
{
    qec64_cpu *c = x->c;
    unsigned op = x->op, mode = (op >> 3) & 7, reg = op & 7;
    switch (op) {
    case 0x4E71: return;                 /* NOP */
    case 0x4E75: c->pc = pop(x) & amask(x); return;   /* RTS */
    case 0x4AFC: stop_with(x, vectored(c) ? QEC64_UNIMPLEMENTED : QEC64_STOPPED);
    case 0x4E70: if (!c->supervisor) stop_with(x, QEC64_PRIVILEGE); return;   /* RESET: no devices to reset */
    case 0x4E72: {                       /* STOP */
        if (!c->supervisor) stop_with(x, QEC64_PRIVILEGE);
        uint16_t v = fetch16(x);
        if (!vectored(c)) stop_with(x, QEC64_STOPPED);
        set_sr(x, v);
        c->waiting = true;
        return;
    }
    case 0x4E73: {                       /* RTE, see the frame in the specification */
        if (!c->supervisor) stop_with(x, QEC64_PRIVILEGE);
        if (!vectored(c)) unimpl(x);
        uint64_t sp = c->a[7];
        uint16_t sr = (uint16_t)rd(x, sp, 2), fv = (uint16_t)rd(x, sp + 2, 2);
        uint64_t pc = rd(x, sp + 8, 8);
        aset(x, 7, sp + ((fv >> 12) == 1 ? 24 : 16));
        set_sr(x, sr);
        c->pc = pc;
        return;
    }
    case 0x4E74: {                       /* RTD */
        int64_t d = (int16_t)fetch16(x);
        c->pc = pop(x) & amask(x);
        aset(x, 7, c->a[7] + (uint64_t)d);
        return;
    }
    case 0x4E76: if (c->ccr & VF) stop_with(x, QEC64_TRAPV); return;
    case 0x4E77: {                       /* RTR */
        c->ccr = (uint8_t)(rd(x, c->a[7], 2) & 0x1F);
        aset(x, 7, c->a[7] + 2);
        c->pc = pop(x) & amask(x);
        return;
    }
    default: break;
    }
    if ((op & 0xFFF0) == 0x4E40) { c->trap_number = op & 15; stop_with(x, QEC64_TRAP); }
    if ((op & 0xFFF0) == 0x4E60) {       /* MOVE USP */
        if (!c->supervisor) stop_with(x, QEC64_PRIVILEGE);
        unsigned r = regB(x, reg);
        if (op & 8) aset(x, r, c->other_sp); else c->other_sp = c->a[r] & amask(x);
        return;
    }
    if ((op & 0xFFC0) == 0x40C0) {       /* MOVE from SR */
        if (!c->supervisor) stop_with(x, QEC64_PRIVILEGE);
        need_data_alterable(x, mode, reg);
        loc l = ea(x, mode, reg, 2);
        st(x, &l, 2, get_sr(c));
        return;
    }
    if ((op & 0xFFC0) == 0x46C0) {       /* MOVE to SR */
        if (!c->supervisor) stop_with(x, QEC64_PRIVILEGE);
        if (mode == 1) unimpl(x);
        loc l = ea(x, mode, reg, 2);
        set_sr(x, (uint16_t)ld(x, &l, 2));
        return;
    }
    if ((op & 0xFFC0) == 0x4800 && mode != 1) {   /* NBCD */
        need_data_alterable(x, mode, reg);
        loc l = ea(x, mode, reg, 1);
        unsigned dst = (unsigned)ld(x, &l, 1), xb = (c->ccr & XF) ? 1 : 0;
        unsigned res = (0x9A - dst - xb) & 0xFF;
        if (res != 0x9A) {
            unsigned v = ~res;
            if ((res & 0x0F) == 0x0A) res = (res & 0xF0) + 0x10;
            res &= 0xFF;
            v &= res;
            st(x, &l, 1, res);
            c->ccr = (uint8_t)(XF | CF | ((v & 0x80) ? VF : 0) | ((res & 0x80) ? NF : 0) | (res ? 0 : (c->ccr & ZF)));
        } else {
            c->ccr = (uint8_t)(((res & 0x80) ? NF : 0) | (c->ccr & ZF));
        }
        return;
    }
    if ((op & 0xF140) == 0x4100) {       /* CHK.W, CHK.L */
        if (mode == 1) unimpl(x);
        unsigned s = (op & 0x80) ? 2 : wide(x, 4);
        if (!(op & 0x80) && (op & 0x100) && (op & 0x40)) unimpl(x);
        loc l = ea(x, mode, reg, s);
        int64_t bound = sext(ld(x, &l, s), s), v = sext(c->d[regR(x, (op >> 9) & 7)], s);
        c->ccr = (uint8_t)((c->ccr & (XF | NF)) | ((v & (int64_t)size_mask(s)) ? 0 : ZF));
        if (v >= 0 && v <= bound) return;
        c->ccr = (uint8_t)((c->ccr & ~NF) | (v < 0 ? NF : 0));
        stop_with(x, QEC64_CHK);
    }
    if ((op & 0xFFFE) == 0x4E7A) {
        /* MOVEC: the registers this core adds, see the specification */
        if (!c->supervisor) stop_with(x, QEC64_PRIVILEGE);
        uint16_t e = fetch16(x);
        unsigned rn = (e >> 12) & 15, cr = e & 0xFFF;
        uint64_t *rp = rn < 8 ? &c->d[rn] : &c->a[rn - 8];
        if (op & 1) {
            switch (cr) {
            case CR_M64: c->m64 = *rp & 1; return;
            case CR_VBR: c->vbr = *rp; c->vbr_set = true; return;
            case CR_USP: c->other_sp = *rp; return;
            case CR_TIMER: c->timer_us = *rp; if (c->on_timer) c->on_timer(c); return;
            case CR_START_SP: c->start_sp = *rp; return;
            case CR_IPI: if (c->on_ipi) c->on_ipi(c, *rp); return;
            case CR_CONSOLE: if (c->on_console) c->on_console(c, (unsigned)*rp & 0xFF); return;
            case CR_HALT: c->exit_code = *rp; stop_with(x, QEC64_HALTED);
            default: unimpl(x);
            }
        }
        switch (cr) {
        case CR_M64: *rp = c->m64; return;
        case CR_COREID: *rp = c->core_id; return;
        case CR_VBR: *rp = c->vbr; return;
        case CR_USP: *rp = c->other_sp; return;
        case CR_TIMER: *rp = c->timer_us; return;
        case CR_NCORES: *rp = c->num_cores ? c->num_cores : 1; return;
        case CR_CONSOLE: *rp = c->on_console_in ? (uint64_t)(int64_t)c->on_console_in(c) : ~UINT64_C(0); return;
        default: unimpl(x);
        }
    }
    if ((op & 0xFFF8) == 0x4E50 || (op & 0xFFF8) == 0x4808) {   /* LINK.W, LINK.L */
        unsigned r = regB(x, reg);
        int64_t disp = (op & 0x4000 && (op & 0xFFF8) == 0x4E50) ? (int16_t)fetch16(x) : (int32_t)fetch32(x);
        /* LINK A7 pushes the already decremented stack pointer. */
        push(x, r == 7 ? c->a[7] - ptr_size(x) : c->a[r]);
        aset(x, r, c->a[7]);
        aset(x, 7, c->a[7] + (uint64_t)disp);
        return;
    }
    if ((op & 0xFFF8) == 0x4E58) {       /* UNLK */
        unsigned r = regB(x, reg);
        aset(x, 7, c->a[r]);
        aset(x, r, pop(x));
        return;
    }
    if ((op & 0xFF80) == 0x4E80) {       /* JSR, JMP */
        if (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg == 4)) unimpl(x);
        loc l = ea(x, mode, reg, 2);
        if (!(op & 0x40)) push(x, c->pc);
        c->pc = l.addr;
        return;
    }
    if ((op & 0xF1C0) == 0x41C0) {       /* LEA */
        if (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg == 4)) unimpl(x);
        loc l = ea(x, mode, reg, 4);
        aset(x, regR(x, (op >> 9) & 7), l.addr);
        return;
    }
    if ((op & 0xFFC0) == 0x4840) {       /* SWAP, PEA */
        if (mode == 0) {
            unsigned r = regB(x, reg), s = wide(x, 4);
            uint64_t v = c->d[r], res = s == 8 ? (v << 32 | v >> 32) : (uint32_t)(v << 16 | (v & 0xFFFFFFFFu) >> 16);
            dset(x, r, s, res);
            logic_flags(c, res, s);
            return;
        }
        if (mode == 1 || mode == 3 || mode == 4 || (mode == 7 && reg == 4)) unimpl(x);
        loc l = ea(x, mode, reg, 4);
        push(x, l.addr);
        return;
    }
    if ((op & 0xFFB8) == 0x4880) {       /* EXT.W, EXT.L (from .L to .Q under W) */
        unsigned r = regB(x, reg);
        if (op & 0x40) {
            if (x->pfx & PFX_W) { uint64_t v = (uint64_t)(int64_t)(int32_t)c->d[r]; c->d[r] = v; logic_flags(c, v, 8); }
            else { uint64_t v = (uint64_t)(int64_t)(int16_t)c->d[r]; dset(x, r, 4, v); logic_flags(c, v, 4); }
        } else {
            uint64_t v = (uint64_t)(int64_t)(int8_t)c->d[r];
            dset(x, r, 2, v); logic_flags(c, v, 2);
        }
        return;
    }
    if ((op & 0xFFF8) == 0x49C0) {       /* EXTB.L */
        unsigned r = regB(x, reg), s = wide(x, 4);
        uint64_t v = (uint64_t)(int64_t)(int8_t)c->d[r];
        dset(x, r, s, v); logic_flags(c, v, s);
        return;
    }
    if ((op & 0xFB80) == 0x4880) { op_movem(x); return; }
    if ((op & 0xFF80) == 0x4C00) { op_muldiv_long(x); return; }
    if ((op & 0xFFC0) == 0x4AC0) {       /* TAS */
        need_data_alterable(x, mode, reg);
        loc l = ea(x, mode, reg, 1);
        if (l.kind == LOC_D) { uint64_t v = ld(x, &l, 1); logic_flags(c, v, 1); st(x, &l, 1, v | 0x80); return; }
        uint8_t old = __atomic_fetch_or(mem_at(x, l.addr, 1), 0x80, __ATOMIC_SEQ_CST);
        logic_flags(c, old, 1);
        return;
    }
    if ((op & 0xFFC0) == 0x44C0) {       /* MOVE to CCR */
        if (mode == 1) unimpl(x);
        loc l = ea(x, mode, reg, 2);
        c->ccr = (uint8_t)(ld(x, &l, 2) & 0x1F);
        return;
    }
    if ((op & 0xFFC0) == 0x42C0) {       /* MOVE from CCR */
        need_data_alterable(x, mode, reg);
        loc l = ea(x, mode, reg, 2);
        st(x, &l, 2, c->ccr);
        return;
    }
    unsigned f = (op >> 6) & 3;
    if (f == 3) unimpl(x);               /* MOVE from and to SR, CHK and others */
    unsigned s = size_field(x, f);
    unsigned k = (op >> 8) & 15;
    if (k == 10) {                       /* TST */
        if (mode == 1 && s == 1) unimpl(x);
        loc l = ea(x, mode, reg, s);
        logic_flags(c, ld(x, &l, s), s);
        return;
    }
    if (k != 0 && k != 2 && k != 4 && k != 6) unimpl(x);
    need_data_alterable(x, mode, reg);
    loc l = ea(x, mode, reg, s);
    if (k == 2) { c->ccr = (uint8_t)((c->ccr & XF) | ZF); st(x, &l, s, 0); return; }  /* CLR, no read on a real part either way */
    uint64_t v = ld(x, &l, s), r;
    if (k == 0) r = do_sub(c, 0, v, s, (c->ccr & XF) ? 1 : 0, true, false);   /* NEGX */
    else if (k == 4) r = do_sub(c, 0, v, s, 0, false, false);                 /* NEG */
    else { r = ~v & size_mask(s); logic_flags(c, r, s); }                     /* NOT */
    st(x, &l, s, r);
}
static void op_line5(ctx *x)
{
    qec64_cpu *c = x->c;
    unsigned op = x->op, mode = (op >> 3) & 7, reg = op & 7, f = (op >> 6) & 3;
    if (f == 3) {
        unsigned cc = (op >> 8) & 15;
        if (mode == 1) {                 /* DBcc */
            uint64_t base = c->pc;
            int64_t disp = (int16_t)fetch16(x);
            if (!cond(c, cc)) {
                unsigned r = regB(x, reg);
                uint16_t n = (uint16_t)c->d[r] - 1;
                dset(x, r, 2, n);
                if (n != 0xFFFF) c->pc = (base + (uint64_t)disp) & amask(x);
            }
            return;
        }
        if (mode == 7 && reg >= 2 && reg <= 4) {   /* TRAPcc */
            if (reg == 2) fetch16(x);
            else if (reg == 3) fetch32(x);
            if (cond(c, cc)) stop_with(x, QEC64_TRAPV);
            return;
        }
        if (mode == 7 && reg > last_alterable(x)) unimpl(x);
        loc l = ea(x, mode, reg, 1);
        st(x, &l, 1, cond(c, cc) ? 0xFF : 0);
        return;
    }
    uint64_t n = (op >> 9) & 7;
    if (!n) n = 8;
    unsigned s = size_field(x, f);
    if (mode == 1) {
        if (s == 1) unimpl(x);
        unsigned r = regB(x, reg);
        aset(x, r, (op & 0x100) ? c->a[r] - n : c->a[r] + n);
        return;
    }
    if (mode == 7 && reg > last_alterable(x)) unimpl(x);
    loc l = ea(x, mode, reg, s);
    uint64_t v = ld(x, &l, s);
    st(x, &l, s, (op & 0x100) ? do_sub(c, v, n, s, 0, false, false) : do_add(c, v, n, s, 0, false));
}
static void op_branch(ctx *x)
{
    qec64_cpu *c = x->c;
    unsigned op = x->op, cc = (op >> 8) & 15;
    uint64_t base = c->pc;
    int64_t d = (int8_t)op;
    if (d == 0) d = (int16_t)fetch16(x);
    else if ((op & 0xFF) == 0xFF) d = (int32_t)fetch32(x);
    if (cc == 1) { push(x, c->pc); c->pc = (base + (uint64_t)d) & amask(x); return; }  /* BSR */
    if (cond(c, cc)) c->pc = (base + (uint64_t)d) & amask(x);
}
static void op_div_word(ctx *x, bool is_signed)
{
    qec64_cpu *c = x->c;
    unsigned op = x->op, mode = (op >> 3) & 7;
    unsigned r = regR(x, (op >> 9) & 7);
    if (mode == 1) unimpl(x);
    loc l = ea(x, mode, op & 7, 2);
    uint16_t d = (uint16_t)ld(x, &l, 2);
    if (!d) stop_with(x, QEC64_DIVIDE_ZERO);
    uint32_t n = (uint32_t)c->d[r];
    /* On overflow only V changes, as in Musashi; N and Z are undefined on the real part. */
    if (is_signed) {
        if (n == 0x80000000u && (int16_t)d == -1) { dset(x, r, 4, 0); c->ccr = (c->ccr & XF) | ZF; return; }
        int32_t q = (int32_t)n / (int16_t)d, rem = (int32_t)n % (int16_t)d;
        if (q != (int16_t)q) { c->ccr |= VF; return; }
        dset(x, r, 4, ((uint32_t)rem << 16) | (uint16_t)q);
        c->ccr &= XF;
        set_nz(c, (uint16_t)q, 2);
    } else {
        uint32_t q = n / d, rem = n % d;
        if (q > 0xFFFF) { c->ccr |= VF; return; }
        dset(x, r, 4, rem << 16 | q);
        c->ccr &= XF;
        set_nz(c, q, 2);
    }
}
static void op_alu(ctx *x)
{
    /* OR $8, SUB $9, EOR and CMP $B, AND $C, ADD $D */
    qec64_cpu *c = x->c;
    unsigned op = x->op, line = op >> 12, mode = (op >> 3) & 7, reg = op & 7, om = (op >> 6) & 7;
    unsigned r = regR(x, (op >> 9) & 7);
    if (line == 8 && (om == 3 || om == 7)) { op_div_word(x, om == 7); return; }
    if (line == 12 && (om == 3 || om == 7)) {   /* MULU.W, MULS.W */
        if (mode == 1) unimpl(x);
        loc l = ea(x, mode, reg, 2);
        uint32_t a = (uint32_t)ld(x, &l, 2), b = (uint16_t)c->d[r], v;
        v = om == 7 ? (uint32_t)((int32_t)(int16_t)a * (int32_t)(int16_t)b) : a * b;
        dset(x, r, 4, v);
        logic_flags(c, v, 4);
        return;
    }
    if (om == 3 || om == 7) {                   /* ADDA, SUBA, CMPA */
        if (line == 8 || line == 12) unimpl(x);
        unsigned s = om == 3 ? 2 : wide(x, 4);
        loc l = ea(x, mode, reg, s);
        uint64_t v = ld(x, &l, s);
        v = s == 8 ? v : (uint64_t)sext(v, s);
        if (line == 0xD) aset(x, r, c->a[r] + v);
        else if (line == 9) aset(x, r, c->a[r] - v);
        else do_sub(c, c->a[r], v & amask(x), x->c->m64 ? 8 : 4, 0, false, true);
        return;
    }
    unsigned s = size_field(x, om & 3);
    bool to_ea = om & 4;
    if (to_ea && mode <= 1) {
        unsigned rb = regB(x, reg);
        if (line == 0xB && mode == 1) {         /* CMPM */
            loc a = ea(x, 3, reg, s);
            uint64_t sv = ld(x, &a, s);
            unsigned p = x->pfx;
            x->pfx = (p & ~PFX_B) | ((p & PFX_R) ? PFX_B : 0);
            loc b = ea(x, 3, (op >> 9) & 7, s);
            x->pfx = p;
            do_sub(c, ld(x, &b, s), sv, s, 0, false, true);
            return;
        }
        if (line == 9 || line == 0xD) {         /* SUBX, ADDX */
            unsigned xb = (c->ccr & XF) ? 1 : 0;
            if (mode == 0) {
                uint64_t a = c->d[r] & size_mask(s), b = c->d[rb] & size_mask(s);
                dset(x, r, s, line == 9 ? do_sub(c, a, b, s, xb, true, false) : do_add(c, a, b, s, xb, true));
            } else {
                loc sa = ea(x, 4, reg, s);
                uint64_t b = ld(x, &sa, s);
                unsigned p = x->pfx;
                x->pfx = (p & ~PFX_B) | ((p & PFX_R) ? PFX_B : 0);
                loc da = ea(x, 4, (op >> 9) & 7, s);
                x->pfx = p;
                uint64_t a = ld(x, &da, s);
                st(x, &da, s, line == 9 ? do_sub(c, a, b, s, xb, true, false) : do_add(c, a, b, s, xb, true));
            }
            return;
        }
        if (line == 12 && om == 4) { op_bcd(x); return; }   /* ABCD */
        if (line == 12) {                       /* EXG */
            uint64_t t;
            if (om == 5 && mode == 0) { t = c->d[r]; c->d[r] = c->d[rb]; c->d[rb] = t; return; }
            if (om == 5 && mode == 1) { t = c->a[r]; c->a[r] = c->a[rb]; c->a[rb] = t; return; }
            if (om == 6 && mode == 1) { t = c->d[r]; c->d[r] = c->a[rb]; c->a[rb] = t; if (!c->m64) c->d[r] &= 0xFFFFFFFFu; return; }
            unimpl(x);
        }
        if (line == 8) { op_bcd(x); return; }   /* SBCD, PACK, UNPK */
        if (line != 0xB || mode == 1) unimpl(x);
    }
    if (line == 0xB && !to_ea) {                /* CMP */
        if (s == 1 && mode == 1) unimpl(x);
        loc l = ea(x, mode, reg, s);
        do_sub(c, c->d[r] & size_mask(s), ld(x, &l, s), s, 0, false, true);
        return;
    }
    if (!to_ea && s == 1 && mode == 1) unimpl(x);
    if (!to_ea && mode == 1 && (line == 8 || line == 12)) unimpl(x);
    if (to_ea && mode == 7 && reg > last_alterable(x)) unimpl(x);
    loc l = ea(x, mode, reg, s);
    uint64_t e = ld(x, &l, s), d = c->d[r] & size_mask(s), res;
    uint64_t a = to_ea ? e : d, b = to_ea ? d : e;
    switch (line) {
    case 8: res = a | b; logic_flags(c, res, s); break;
    case 12: res = a & b; logic_flags(c, res, s); break;
    case 0xB: res = a ^ b; logic_flags(c, res, s); break;
    case 9: res = do_sub(c, a, b, s, 0, false, false); break;
    default: res = do_add(c, a, b, s, 0, false); break;
    }
    if (to_ea) st(x, &l, s, res); else dset(x, r, s, res);
}
/* One shift or rotate, with 68000 flag rules. kind: 0 AS, 1 LS, 2 ROX, 3 RO. */
static uint64_t shift(qec64_cpu *c, uint64_t v, unsigned s, unsigned kind, bool left, unsigned n)
{
    unsigned bits = s * 8;
    uint64_t m = size_mask(s), sb = sign_bit(s);
    bool cy = false, ov = false, xb = c->ccr & XF;
    v &= m;
    if (kind == 2) {
        for (unsigned i = 0; i < n; i++) {
            bool out = left ? (v & sb) != 0 : (v & 1) != 0;
            v = left ? ((v << 1) & m) | xb : (v >> 1) | (xb ? sb : 0);
            xb = out;
        }
        cy = xb;
    } else if (kind == 3) {
        if (n) {
            unsigned k = n % bits;
            if (k) v = left ? ((v << k) | (v >> (bits - k))) & m : ((v >> k) | (v << (bits - k))) & m;
            cy = left ? (v & 1) != 0 : (v & sb) != 0;
        }
    } else {
        for (unsigned i = 0; i < n; i++) {
            uint64_t before = v;
            if (left) { cy = (v & sb) != 0; v = (v << 1) & m; if (kind == 0 && ((before ^ v) & sb)) ov = true; }
            else { cy = v & 1; v = kind == 0 ? (v >> 1) | (v & sb) : v >> 1; }
        }
        if (n) xb = cy;
    }
    uint8_t f = 0;
    if (kind < 2 ? n != 0 : true) f |= (kind == 3) ? (c->ccr & XF) : (xb ? XF : 0);
    else f |= c->ccr & XF;
    if (cy) f |= CF;
    if (ov) f |= VF;
    if (!v) f |= ZF;
    if (v & sb) f |= NF;
    c->ccr = f;
    return v;
}
static void op_shift(ctx *x)
{
    qec64_cpu *c = x->c;
    unsigned op = x->op, f = (op >> 6) & 3;
    bool left = op & 0x100;
    if (f == 3) {
        unsigned mode = (op >> 3) & 7, reg = op & 7, kind = (op >> 9) & 3;
        if (op & 0x800) { op_bitfield(x); return; }
        need_alterable_mem(x, mode, reg);
        loc l = ea(x, mode, reg, 2);
        st(x, &l, 2, shift(c, ld(x, &l, 2), 2, kind, left, 1));
        return;
    }
    unsigned s = size_field(x, f), r = regB(x, op & 7), kind = (op >> 3) & 3, cnt = (op >> 9) & 7, n;
    if (op & 0x20) n = (unsigned)(c->d[regR(x, cnt)] & 63);
    else n = cnt ? cnt : 8;
    dset(x, r, s, shift(c, c->d[r], s, kind, left, n));
}
static void execute(ctx *x)
{
    qec64_cpu *c = x->c;
    x->pfx = 0;
    x->start_pc = c->pc;
    x->op = fetch16(x);
    if ((x->op >> 12) == 0xA) {
        if (!c->m64 || (x->op & 0xFE0)) unimpl(x);
        x->pfx = x->op & 0x1F;
        x->op = fetch16(x);
        if ((x->op >> 12) == 0xA) unimpl(x);
    }
    if ((x->pfx & PFX_N) && ((x->op >> 12) < 1 || (x->op >> 12) > 3)) unimpl(x);
    switch (x->op >> 12) {
    case 0: op_immediate(x); break;
    case 1: case 2: case 3: op_move(x); break;
    case 4: op_line4(x); break;
    case 5: op_line5(x); break;
    case 6: op_branch(x); break;
    case 7: {
        if (x->op & 0x100) unimpl(x);
        uint64_t v = (uint64_t)(int64_t)(int8_t)x->op;
        unsigned r = regR(x, (x->op >> 9) & 7);
        /* Sign extends to the register's width. */
        if (c->m64) c->d[r] = v; else dset(x, r, 4, v);
        logic_flags(c, v, 4);
        break;
    }
    case 8: case 9: case 11: case 12: case 13: op_alu(x); break;
    case 14: op_shift(x); break;
    default: unimpl(x);
    }
}
void qec64_init(qec64_cpu *c, uint8_t *ram, uint64_t ram_size, uint32_t core_id)
{
    memset(c, 0, sizeof *c);
    c->ram = ram; c->ram_size = ram_size; c->core_id = core_id;
    c->supervisor = true;
    c->ipl = 7;
}
/* Long mode frame, 16 bytes: SR, format and vector, 4 reserved, PC. Format 1 adds the fault address. */
static bool take_exception(ctx *x, unsigned vector, uint64_t frame_pc, bool has_addr, uint64_t addr)
{
    qec64_cpu *c = x->c;
    uint16_t sr = get_sr(c);
    if (!c->supervisor) set_sr(x, (uint16_t)(sr | 0x2000));
    uint64_t size = has_addr ? 24 : 16, sp = c->a[7] - size;
    uint64_t entry = c->vbr + vector * UINT64_C(8);
    if (sp >= c->ram_size || size > c->ram_size - sp || entry >= c->ram_size || c->ram_size - entry < 8 || (entry & 7)) {
        c->status = QEC64_DOUBLE_FAULT;
        c->fault_pc = frame_pc;
        c->fault_addr = sp;
        return false;
    }
    c->a[7] = sp;
    /* These cannot fault after the check above. */
    wr(x, sp, 2, sr);
    wr(x, sp + 2, 2, (uint64_t)((has_addr ? 0x1000u : 0) | (vector << 2)));
    wr(x, sp + 4, 4, 0);
    wr(x, sp + 8, 8, frame_pc);
    if (has_addr) wr(x, sp + 16, 8, addr);
    c->pc = rd(x, entry, 8);
    return true;
}
/* After a stop_with: turn it into an exception if the core has a vector table. */
static bool to_exception(ctx *x)
{
    qec64_cpu *c = x->c;
    if (!vectored(c)) return false;
    uint64_t pc_here = c->pc;
    enum qec64_status st = c->status;
    c->status = QEC64_RUNNING;
    switch (st) {
    case QEC64_BUS_FAULT: return take_exception(x, 2, x->start_pc, true, c->fault_addr);
    case QEC64_UNIMPLEMENTED:
        return take_exception(x, (x->op >> 12) == 0xA ? 10 : (x->op >> 12) == 0xF ? 11 : 4, x->start_pc, false, 0);
    case QEC64_DIVIDE_ZERO: return take_exception(x, 5, pc_here, false, 0);
    case QEC64_CHK: return take_exception(x, 6, pc_here, false, 0);
    case QEC64_TRAPV: return take_exception(x, 7, pc_here, false, 0);
    case QEC64_PRIVILEGE: return take_exception(x, 8, x->start_pc, false, 0);
    case QEC64_TRAP: return take_exception(x, 32 + c->trap_number, pc_here, false, 0);
    default: c->status = st; return false;
    }
}
void qec64_raise(qec64_cpu *c, unsigned level)
{
    __atomic_fetch_or(&c->pending, UINT32_C(1) << level, __ATOMIC_SEQ_CST);
}
enum qec64_status qec64_run(qec64_cpu *c, uint64_t max)
{
    ctx x;
    x.c = c;
    x.op = 0; x.pfx = 0; x.start_pc = c->pc;
    c->status = QEC64_RUNNING;
    c->budget = max;
    if (setjmp(x.out)) {
        c->instructions++;
        if (!to_exception(&x)) return c->status;
    }
    while (c->budget) {
        uint32_t p = __atomic_load_n(&c->pending, __ATOMIC_RELAXED);
        if (p && vectored(c)) {
            unsigned level = 31u - (unsigned)__builtin_clz(p);
            if (level > c->ipl || level == 7) {
                __atomic_fetch_and(&c->pending, ~(UINT32_C(1) << level), __ATOMIC_SEQ_CST);
                c->waiting = false;
                uint64_t pc = c->pc;
                x.start_pc = pc;
                if (!take_exception(&x, 24 + level, pc, false, 0)) return c->status;
                c->ipl = level;
            }
        }
        if (c->waiting) return QEC64_WAITING;
        c->budget--;
        execute(&x);
        c->instructions++;
    }
    return QEC64_RUNNING;
}
