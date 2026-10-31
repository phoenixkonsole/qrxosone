/* QRX OS One 22.5.12 - qrx86core: portable IA-32 user-mode interpreter.
 * See qrx86core.h.  Integer ISA (incl. BT*, SHLD/SHRD, CMPXCHG8B, BCD),
 * x87 FPU (host double precision), lazy arithmetic flags, FS/GS bases.
 * No SSE/MMX: CPUID reports a CMOV-capable i686 without them. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "../qmath/qmath.h"   /* sin/floor/frexp/... -> qm_* (host libm is not trusted) */
#include "qrx86core.h"

#if defined(__GNUC__)
#define X86_INLINE static inline __attribute__((always_inline))
#else
#define X86_INLINE static
#endif
#define FAULT(cc, code, addr, msg) x86_raise((cc), (code), (addr), (msg))

#if defined(__GNUC__)
#define X86_NORETURN __attribute__((noreturn))
#else
#define X86_NORETURN
#endif
static void x86_raise(x86cpu *c, int code, uint32_t addr, const char *msg) X86_NORETURN;
static void x86_raise(x86cpu *c, int code, uint32_t addr, const char *msg)
{
    c->fault = code;
    c->fault_eip = c->cur_eip;
    c->fault_addr = addr;
    strncpy(c->fault_msg, msg ? msg : "", sizeof(c->fault_msg) - 1);
    c->fault_msg[sizeof(c->fault_msg) - 1] = 0;
    if (c->jb) longjmp(*c->jb, 1);
    abort();    /* interpreter entered without x86_run/x86_step: programming error */
}

const char *x86_fault_name(int f)
{
    switch (f) {
    case X86_OK: return "ok";
    case X86_FAULT_PAGE: return "page fault";
    case X86_FAULT_UD: return "invalid/unsupported opcode";
    case X86_FAULT_DE: return "divide error";
    case X86_FAULT_GP: return "general protection";
    case X86_FAULT_BUDGET: return "instruction budget exhausted";
    case X86_FAULT_HOST: return "host hook error";
    case X86_FAULT_INT3: return "breakpoint";
    default: return "fault";
    }
}

/* ======================================================================= memory */
static void tlb_flush(x86cpu *c) {
#if QRX_JIT_PAGE_CACHE
    ++c->memory_serial;
#endif
    memset(c->tlb_rtag, 0, sizeof(c->tlb_rtag)); memset(c->tlb_wtag, 0, sizeof(c->tlb_wtag)); c->f_lim = 0; }

/* little-endian guest words: on big-endian hosts one (unaligned) host load + byte swap;
 * every CPU QRX targets (x86, 68020+, PPC) allows unaligned word/long accesses */
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
X86_INLINE uint16_t ld16(const uint8_t *q) { uint16_t v; memcpy(&v, q, 2); return __builtin_bswap16(v); }
X86_INLINE uint32_t ld32(const uint8_t *q) { uint32_t v; memcpy(&v, q, 4); return __builtin_bswap32(v); }
X86_INLINE void st16(uint8_t *q, uint16_t v) { v = __builtin_bswap16(v); memcpy(q, &v, 2); }
X86_INLINE void st32(uint8_t *q, uint32_t v) { v = __builtin_bswap32(v); memcpy(q, &v, 4); }
#elif defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
X86_INLINE uint16_t ld16(const uint8_t *q) { uint16_t v; memcpy(&v, q, 2); return v; }
X86_INLINE uint32_t ld32(const uint8_t *q) { uint32_t v; memcpy(&v, q, 4); return v; }
X86_INLINE void st16(uint8_t *q, uint16_t v) { memcpy(q, &v, 2); }
X86_INLINE void st32(uint8_t *q, uint32_t v) { memcpy(q, &v, 4); }
#else
X86_INLINE uint16_t ld16(const uint8_t *q) { return (uint16_t)(q[0] | (q[1] << 8)); }
X86_INLINE uint32_t ld32(const uint8_t *q) { return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24); }
X86_INLINE void st16(uint8_t *q, uint16_t v) { q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); }
X86_INLINE void st32(uint8_t *q, uint32_t v) { q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); q[2] = (uint8_t)(v >> 16); q[3] = (uint8_t)(v >> 24); }
#endif

void x86_init(x86cpu *c)
{
    memset(c, 0, sizeof(*c));
    c->eflags = 0x202u;
    c->fcw = 0x037F;
    c->ftw = 0xFFFF;
    c->sreg[X86_CS] = 0x23; c->sreg[X86_DS] = c->sreg[X86_ES] = c->sreg[X86_SS] = 0x2B;
}

void x86_free(x86cpu *c)
{
    int i, j;
    x86_jit_disable(c);
    tlb_flush(c);
    for (i = 0; i < 1024; i++) {
        if (!c->dir[i]) continue;
        for (j = 0; j < 1024; j++) if (!(c->dir[i][j].flags & X86_PG_EXT)) free(c->dir[i][j].mem);
        free(c->dir[i]);
        c->dir[i] = NULL;
    }
}

static x86page *pg(x86cpu *c, uint32_t a)
{
    x86page *t = c->dir[a >> 22];
    if (!t) return NULL;
    t += (a >> 12) & 1023u;
    return t->mem ? t : NULL;
}

int x86_map(x86cpu *c, uint32_t addr, uint32_t size, int writable)
{
    uint32_t a = addr & ~X86_PAGE_MASK;
    uint64_t end = (uint64_t)addr + size;
    tlb_flush(c);
    for (; (uint64_t)a < end; a += X86_PAGE_SIZE) {
        x86page *t = c->dir[a >> 22];
        if (!t) {
            t = (x86page *)calloc(1024, sizeof(x86page));
            if (!t) return 0;
            c->dir[a >> 22] = t;
        }
        t += (a >> 12) & 1023u;
        if (!t->mem) {
            t->mem = (uint8_t *)calloc(1, X86_PAGE_SIZE);
            if (!t->mem) return 0;
        }
        t->flags = (t->flags & X86_PG_EXT) | (writable ? X86_PG_W : 0);
        if (a + X86_PAGE_SIZE < a) break;
    }
    return 1;
}

int x86_map_ext(x86cpu *c, uint32_t addr, uint32_t size, uint8_t *host, int writable)
{
    uint32_t a, off;
    if ((addr | size) & X86_PAGE_MASK) return 0;
    tlb_flush(c);
    for (off = 0; off < size; off += X86_PAGE_SIZE) {
        x86page *t;
        a = addr + off;
        t = c->dir[a >> 22];
        if (!t) {
            t = (x86page *)calloc(1024, sizeof(x86page));
            if (!t) return 0;
            c->dir[a >> 22] = t;
        }
        t += (a >> 12) & 1023u;
        if (t->mem && !(t->flags & X86_PG_EXT)) free(t->mem);
        t->mem = host + off;
        t->flags = X86_PG_EXT | (writable ? X86_PG_W : 0);
    }
    return 1;
}

int x86_protect(x86cpu *c, uint32_t addr, uint32_t size, int writable)
{
    uint32_t a = addr & ~X86_PAGE_MASK;
    uint64_t end = (uint64_t)addr + size;
    tlb_flush(c);
    for (; (uint64_t)a < end; a += X86_PAGE_SIZE) {
        x86page *p = pg(c, a);
        if (p) p->flags = (p->flags & X86_PG_EXT) | (writable ? X86_PG_W : 0);
        if (a + X86_PAGE_SIZE < a) break;
    }
    return 1;
}

void x86_unmap(x86cpu *c, uint32_t addr, uint32_t size)
{
    uint32_t a = addr & ~X86_PAGE_MASK;
    uint64_t end = (uint64_t)addr + size;
    tlb_flush(c);
    for (; (uint64_t)a < end; a += X86_PAGE_SIZE) {
        x86page *p = pg(c, a);
        if (p) { if (!(p->flags & X86_PG_EXT)) free(p->mem); p->mem = NULL; p->flags = 0; }
        if (a + X86_PAGE_SIZE < a) break;
    }
}

int x86_is_mapped(x86cpu *c, uint32_t addr) { return pg(c, addr) != NULL; }
uint8_t *x86_host_ptr(x86cpu *c, uint32_t a) { x86page *p = pg(c, a); return p ? p->mem + (a & X86_PAGE_MASK) : NULL; }

int x86_write_mem(x86cpu *c, uint32_t addr, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        x86page *p = pg(c, addr);
        uint32_t off = addr & X86_PAGE_MASK, k = X86_PAGE_SIZE - off;
        if (!p) return 0;
        if (k > n) k = n;
        memcpy(p->mem + off, s, k);
        s += k; addr += k; n -= k;
    }
    return 1;
}

int x86_read_mem(x86cpu *c, uint32_t addr, void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        x86page *p = pg(c, addr);
        uint32_t off = addr & X86_PAGE_MASK, k = X86_PAGE_SIZE - off;
        if (!p) return 0;
        if (k > n) k = n;
        memcpy(d, p->mem + off, k);
        d += k; addr += k; n -= k;
    }
    return 1;
}

/* faulting accessors used by the interpreter */
static uint8_t *rptr_slow(x86cpu *c, uint32_t a)
{
    x86page *p = pg(c, a);
    unsigned i = (a >> 12) & 63u;
    if (!p) FAULT(c, X86_FAULT_PAGE, a, "read from unmapped memory");
    c->tlb_rtag[i] = (a & ~X86_PAGE_MASK) | 1u;
    c->tlb_rptr[i] = p->mem;
    return p->mem + (a & X86_PAGE_MASK);
}
static uint8_t *wptr_slow(x86cpu *c, uint32_t a)
{
    x86page *p = pg(c, a);
    unsigned i = (a >> 12) & 63u;
    if (!p || !(p->flags & X86_PG_W)) FAULT(c, X86_FAULT_PAGE, a, p ? "write to read-only memory" : "write to unmapped memory");
    c->tlb_wtag[i] = (a & ~X86_PAGE_MASK) | 1u;
    c->tlb_wptr[i] = p->mem;
    return p->mem + (a & X86_PAGE_MASK);
}
X86_INLINE uint8_t *rptr(x86cpu *c, uint32_t a)
{
    unsigned i = (a >> 12) & 63u;
    if (c->tlb_rtag[i] == ((a & ~X86_PAGE_MASK) | 1u)) return c->tlb_rptr[i] + (a & X86_PAGE_MASK);
    return rptr_slow(c, a);
}
X86_INLINE uint8_t *wptr(x86cpu *c, uint32_t a)
{
    unsigned i = (a >> 12) & 63u;
    if (c->tlb_wtag[i] == ((a & ~X86_PAGE_MASK) | 1u)) return c->tlb_wptr[i] + (a & X86_PAGE_MASK);
    return wptr_slow(c, a);
}
X86_INLINE uint8_t m_r8(x86cpu *c, uint32_t a) { return *rptr(c, a); }
X86_INLINE void m_w8(x86cpu *c, uint32_t a, uint8_t v) { *wptr(c, a) = v; }
/* word/long accesses: inline fast path (same page, TLB hit), out-of-line rest */
static uint16_t m_r16_slow(x86cpu *c, uint32_t a)
{
    if ((a & X86_PAGE_MASK) <= X86_PAGE_SIZE - 2) return ld16(rptr_slow(c, a));
    return (uint16_t)(m_r8(c, a) | (m_r8(c, a + 1) << 8));
}
static uint32_t m_r32_slow(x86cpu *c, uint32_t a)
{
    if ((a & X86_PAGE_MASK) <= X86_PAGE_SIZE - 4) return ld32(rptr_slow(c, a));
    return (uint32_t)m_r16_slow(c, a) | ((uint32_t)m_r16_slow(c, a + 2) << 16);
}
static void m_w16_slow(x86cpu *c, uint32_t a, uint16_t v)
{
    if ((a & X86_PAGE_MASK) <= X86_PAGE_SIZE - 2) { st16(wptr_slow(c, a), v); return; }
    m_w8(c, a, (uint8_t)v); m_w8(c, a + 1, (uint8_t)(v >> 8));
}
static void m_w32_slow(x86cpu *c, uint32_t a, uint32_t v)
{
    if ((a & X86_PAGE_MASK) <= X86_PAGE_SIZE - 4) { st32(wptr_slow(c, a), v); return; }
    m_w16_slow(c, a, (uint16_t)v); m_w16_slow(c, a + 2, (uint16_t)(v >> 16));
}
#define TLB_HIT(tag, a, room) (((a) & X86_PAGE_MASK) <= X86_PAGE_SIZE - (room) && (tag)[((a) >> 12) & 63u] == (((a) & ~X86_PAGE_MASK) | 1u))
X86_INLINE uint16_t m_r16(x86cpu *c, uint32_t a)
{
    if (TLB_HIT(c->tlb_rtag, a, 2)) return ld16(c->tlb_rptr[(a >> 12) & 63u] + (a & X86_PAGE_MASK));
    return m_r16_slow(c, a);
}
X86_INLINE uint32_t m_r32(x86cpu *c, uint32_t a)
{
    if (TLB_HIT(c->tlb_rtag, a, 4)) return ld32(c->tlb_rptr[(a >> 12) & 63u] + (a & X86_PAGE_MASK));
    return m_r32_slow(c, a);
}
X86_INLINE void m_w16(x86cpu *c, uint32_t a, uint16_t v)
{
    if (TLB_HIT(c->tlb_wtag, a, 2)) { st16(c->tlb_wptr[(a >> 12) & 63u] + (a & X86_PAGE_MASK), v); return; }
    m_w16_slow(c, a, v);
}
X86_INLINE void m_w32(x86cpu *c, uint32_t a, uint32_t v)
{
    if (TLB_HIT(c->tlb_wtag, a, 4)) { st32(c->tlb_wptr[(a >> 12) & 63u] + (a & X86_PAGE_MASK), v); return; }
    m_w32_slow(c, a, v);
}

/* non-faulting public accessors (for hooks) */
uint8_t x86_rd8(x86cpu *c, uint32_t a) { x86page *p = pg(c, a); return p ? p->mem[a & X86_PAGE_MASK] : 0; }
uint16_t x86_rd16(x86cpu *c, uint32_t a) { return (uint16_t)(x86_rd8(c, a) | (x86_rd8(c, a + 1) << 8)); }
uint32_t x86_rd32(x86cpu *c, uint32_t a) { return (uint32_t)x86_rd16(c, a) | ((uint32_t)x86_rd16(c, a + 2) << 16); }
int x86_wr8(x86cpu *c, uint32_t a, uint8_t v) { x86page *p = pg(c, a); if (!p) return 0; p->mem[a & X86_PAGE_MASK] = v; return 1; }
int x86_wr16(x86cpu *c, uint32_t a, uint16_t v) { return x86_wr8(c, a, (uint8_t)v) && x86_wr8(c, a + 1, (uint8_t)(v >> 8)); }
int x86_wr32(x86cpu *c, uint32_t a, uint32_t v) { return x86_wr16(c, a, (uint16_t)v) && x86_wr16(c, a + 2, (uint16_t)(v >> 16)); }
int x86_push(x86cpu *c, uint32_t v) { c->r[X86_ESP] -= 4; return x86_wr32(c, c->r[X86_ESP], v); }
uint32_t x86_pop(x86cpu *c) { uint32_t v = x86_rd32(c, c->r[X86_ESP]); c->r[X86_ESP] += 4; return v; }

/* ======================================================================= flags */
enum { LF_NONE = 0, LF_ADD, LF_ADC, LF_SUB, LF_SBB, LF_LOGIC, LF_INC, LF_DEC, LF_NEG, LF_SHL, LF_SHR, LF_SAR };

static const uint8_t parity_tab[256] = {
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
    P6(1), P6(0), P6(0), P6(1)
#undef P2
#undef P4
#undef P6
};

X86_INLINE uint32_t szmask(int sz) { return sz == 8 ? 0xFFu : sz == 16 ? 0xFFFFu : 0xFFFFFFFFu; }
X86_INLINE uint32_t szsign(int sz) { return sz == 8 ? 0x80u : sz == 16 ? 0x8000u : 0x80000000u; }

X86_INLINE void lf_set(x86cpu *c, int op, int sz, uint32_t res, uint32_t a, uint32_t b)
{
    c->lf_op = op; c->lf_sz = sz; c->lf_res = res & szmask(sz); c->lf_a = a; c->lf_b = b;
}

static uint32_t flags_compute(x86cpu *c)
{
    uint32_t f = 0, m, s, r, a, b;
    if (!c->lf_op) return c->eflags & X86_ARITH;
    m = szmask(c->lf_sz); s = szsign(c->lf_sz); r = c->lf_res; a = c->lf_a & m; b = c->lf_b & m;
    if (!r) f |= X86_ZF;
    if (r & s) f |= X86_SF;
    if (parity_tab[r & 0xFF]) f |= X86_PF;
    switch (c->lf_op) {
    case LF_ADD:
        if (r < a) f |= X86_CF;
        if ((a ^ r) & (b ^ r) & s) f |= X86_OF;
        f |= (a ^ b ^ r) & X86_AF;
        break;
    case LF_ADC:
        if (c->lf_cf ? r <= a : r < a) f |= X86_CF;
        if ((a ^ r) & (b ^ r) & s) f |= X86_OF;
        f |= (a ^ b ^ r) & X86_AF;
        break;
    case LF_SUB:
        if (a < b) f |= X86_CF;
        if ((a ^ b) & (a ^ r) & s) f |= X86_OF;
        f |= (a ^ b ^ r) & X86_AF;
        break;
    case LF_SBB:
        if (c->lf_cf ? a <= b : a < b) f |= X86_CF;
        if ((a ^ b) & (a ^ r) & s) f |= X86_OF;
        f |= (a ^ b ^ r) & X86_AF;
        break;
    case LF_LOGIC:
        break;
    case LF_INC:
        f |= c->lf_cf;
        if (r == s) f |= X86_OF;
        if ((r & 0xF) == 0) f |= X86_AF;
        break;
    case LF_DEC:
        f |= c->lf_cf;
        if (r == s - 1) f |= X86_OF;
        if ((r & 0xF) == 0xF) f |= X86_AF;
        break;
    case LF_NEG:
        if (r) f |= X86_CF;
        if (r == s) f |= X86_OF;
        f |= (b ^ r) & X86_AF;
        break;
    case LF_SHL: {
        int n = (int)c->lf_b, w = c->lf_sz;
        if (n <= w && ((a >> (w - n)) & 1u)) f |= X86_CF;
        if (((r & s) ? 1u : 0u) ^ (f & X86_CF)) f |= X86_OF;
        break;
    }
    case LF_SHR: {
        int n = (int)c->lf_b;
        if (n <= c->lf_sz && ((a >> (n - 1)) & 1u)) f |= X86_CF;
        if (a & s) f |= X86_OF;
        break;
    }
    case LF_SAR: {
        int n = (int)c->lf_b, w = c->lf_sz;
        int32_t sa = (int32_t)(a << (32 - w)) >> (32 - w);
        if (n >= w) { if (sa < 0) f |= X86_CF; }
        else if ((sa >> (n - 1)) & 1) f |= X86_CF;
        break;
    }
    }
    return f;
}

uint32_t x86_get_flags(x86cpu *c)
{
    if (c->lf_op) {
        c->eflags = (c->eflags & ~X86_ARITH) | flags_compute(c);
        c->lf_op = LF_NONE;
    }
    return c->eflags | 0x2u;
}

void x86_set_flags(x86cpu *c, uint32_t f)
{
    c->lf_op = LF_NONE;
    c->eflags = (f & 0x00254FD5u) | 0x2u;
}

static int get_cf(x86cpu *c)
{
    switch (c->lf_op) {
    case LF_NONE: return (c->eflags & X86_CF) != 0;
    case LF_SUB: return (c->lf_a & szmask(c->lf_sz)) < (c->lf_b & szmask(c->lf_sz));
    case LF_ADD: return c->lf_res < (c->lf_a & szmask(c->lf_sz));
    case LF_LOGIC: return 0;
    case LF_INC: case LF_DEC: return c->lf_cf != 0;
    default: return (flags_compute(c) & X86_CF) != 0;
    }
}

static int cond(x86cpu *c, int cc)
{
    int r;
    if (c->lf_op == LF_SUB) {
        uint32_t m = szmask(c->lf_sz), a = c->lf_a & m, b = c->lf_b & m;
        int w = c->lf_sz;
        int32_t sa = (int32_t)(a << (32 - w)) >> (32 - w), sb = (int32_t)(b << (32 - w)) >> (32 - w);
        switch (cc >> 1) {
        case 1: r = a < b; return (cc & 1) ? !r : r;
        case 2: r = a == b; return (cc & 1) ? !r : r;
        case 3: r = a <= b; return (cc & 1) ? !r : r;
        case 6: r = sa < sb; return (cc & 1) ? !r : r;
        case 7: r = sa <= sb; return (cc & 1) ? !r : r;
        default: break;
        }
    } else if (c->lf_op == LF_LOGIC) {
        uint32_t s = szsign(c->lf_sz), v = c->lf_res;
        switch (cc >> 1) {
        case 0: r = 0; return (cc & 1) ? !r : r;
        case 1: r = 0; return (cc & 1) ? !r : r;
        case 2: r = v == 0; return (cc & 1) ? !r : r;
        case 3: r = v == 0; return (cc & 1) ? !r : r;
        case 4: r = (v & s) != 0; return (cc & 1) ? !r : r;
        case 6: r = (v & s) != 0; return (cc & 1) ? !r : r;
        case 7: r = v == 0 || (v & s); return (cc & 1) ? !r : r;
        default: break;
        }
    }
    {
        uint32_t f = x86_get_flags(c);
        switch (cc >> 1) {
        case 0: r = (f & X86_OF) != 0; break;
        case 1: r = (f & X86_CF) != 0; break;
        case 2: r = (f & X86_ZF) != 0; break;
        case 3: r = (f & (X86_CF | X86_ZF)) != 0; break;
        case 4: r = (f & X86_SF) != 0; break;
        case 5: r = (f & X86_PF) != 0; break;
        case 6: r = !(f & X86_SF) != !(f & X86_OF); break;
        default: r = (f & X86_ZF) || (!(f & X86_SF) != !(f & X86_OF)); break;
        }
    }
    return (cc & 1) ? !r : r;
}

/* set arithmetic flags eagerly (used by MUL/IMUL/rotates/BT/BCD...) */
static void set_arith(x86cpu *c, uint32_t mask, uint32_t val)
{
    uint32_t f = x86_get_flags(c);
    c->eflags = (f & ~mask) | (val & mask);
}

/* ======================================================================= decode */
typedef struct {    /* bytes: the four initial stores merge into one */
    uint8_t osz;    /* 16 or 32 */
    uint8_t asz;    /* 16 or 32 */
    uint8_t rep;    /* 0, 0xF2, 0xF3 */
    int8_t seg;     /* -1 or segment index */
} prefix_t;

typedef struct { int mod, reg, rm, isreg; uint32_t addr; } modrm_t;

/* instruction fetch through the code-page window; the slow path refills it */
static void fetch_refill(x86cpu *c)
{
    uint8_t *q = rptr(c, c->eip);                  /* faults if unmapped */
    c->f_lo = c->eip & ~X86_PAGE_MASK;
    c->f_base = q - (c->eip & X86_PAGE_MASK);
    c->f_lim = X86_PAGE_SIZE;
}
X86_INLINE uint8_t fetch8(x86cpu *c)
{
    uint32_t o = c->eip - c->f_lo;
    if (o >= c->f_lim) { fetch_refill(c); o = c->eip - c->f_lo; }
    c->eip++;
    return c->f_base[o];
}
X86_INLINE uint16_t fetch16(x86cpu *c)
{
    uint32_t o = c->eip - c->f_lo;
    uint16_t v;
    if (o + 2 <= c->f_lim && o < c->f_lim) { c->eip += 2; return ld16(c->f_base + o); }
    v = fetch8(c);
    return (uint16_t)(v | (fetch8(c) << 8));
}
X86_INLINE uint32_t fetch32(x86cpu *c)
{
    uint32_t o = c->eip - c->f_lo;
    uint32_t v;
    if (o + 4 <= c->f_lim && o < c->f_lim) { c->eip += 4; return ld32(c->f_base + o); }
    v = fetch16(c);
    return v | ((uint32_t)fetch16(c) << 16);
}
static uint32_t fetchimm(x86cpu *c, int sz) { return sz == 8 ? fetch8(c) : sz == 16 ? fetch16(c) : fetch32(c); }

X86_INLINE uint32_t seg_base(x86cpu *c, const prefix_t *p, int def) { return c->sbase[p->seg >= 0 ? p->seg : def]; }

static void decode_modrm(x86cpu *c, const prefix_t *p, modrm_t *m)
{
    uint8_t b = fetch8(c);
    uint32_t a = 0;
    int def = X86_DS;
    m->mod = b >> 6; m->reg = (b >> 3) & 7; m->rm = b & 7;
#ifdef QRX_EXEC_PROFILE
    c->prof_operands[c->prof_active_op][m->reg+(m->mod==3?0:8)]++;
#endif
    m->isreg = (m->mod == 3);
    if (m->isreg) { m->addr = 0; return; }
    if (p->asz == 16) {
        static const int base1[8] = { X86_EBX, X86_EBX, X86_EBP, X86_EBP, X86_ESI, X86_EDI, X86_EBP, X86_EBX };
        static const int idx1[8] = { X86_ESI, X86_EDI, X86_ESI, X86_EDI, -1, -1, -1, -1 };
        if (m->mod == 0 && m->rm == 6) a = fetch16(c);
        else {
            a = c->r[base1[m->rm]] & 0xFFFF;
            if (idx1[m->rm] >= 0) a += c->r[idx1[m->rm]] & 0xFFFF;
            if (m->rm == 2 || m->rm == 3 || m->rm == 6) def = X86_SS;
            if (m->mod == 1) a += (uint32_t)(int32_t)(int8_t)fetch8(c);
            else if (m->mod == 2) a += fetch16(c);
        }
        a &= 0xFFFF;
    } else {
        if (m->rm == 4) {
            uint8_t sib = fetch8(c);
            int sc = sib >> 6, ix = (sib >> 3) & 7, bs = sib & 7;
            if (bs == 5 && m->mod == 0) a = fetch32(c);
            else { a = c->r[bs]; if (bs == X86_ESP || bs == X86_EBP) def = X86_SS; }
            if (ix != 4) a += c->r[ix] << sc;
        } else if (m->rm == 5 && m->mod == 0) {
            a = fetch32(c);
        } else {
            a = c->r[m->rm];
            if (m->rm == X86_EBP) def = X86_SS;
        }
        if (m->mod == 1) a += (uint32_t)(int32_t)(int8_t)fetch8(c);
        else if (m->mod == 2) a += fetch32(c);
    }
    m->addr = a + seg_base(c, p, def);
}

/* register helpers */
X86_INLINE uint32_t reg_get(x86cpu *c, int r, int sz)
{
    if (sz == 32) return c->r[r];
    if (sz == 16) return c->r[r] & 0xFFFF;
    return r < 4 ? c->r[r] & 0xFF : (c->r[r - 4] >> 8) & 0xFF;
}
X86_INLINE void reg_set(x86cpu *c, int r, int sz, uint32_t v)
{
    if (sz == 32) c->r[r] = v;
    else if (sz == 16) c->r[r] = (c->r[r] & 0xFFFF0000u) | (v & 0xFFFF);
    else if (r < 4) c->r[r] = (c->r[r] & 0xFFFFFF00u) | (v & 0xFF);
    else c->r[r - 4] = (c->r[r - 4] & 0xFFFF00FFu) | ((v & 0xFF) << 8);
}
X86_INLINE uint32_t mem_get(x86cpu *c, uint32_t a, int sz) { return sz == 32 ? m_r32(c, a) : sz == 16 ? m_r16(c, a) : m_r8(c, a); }
X86_INLINE void mem_set(x86cpu *c, uint32_t a, int sz, uint32_t v)
{
    if (sz == 32) m_w32(c, a, v); else if (sz == 16) m_w16(c, a, (uint16_t)v); else m_w8(c, a, (uint8_t)v);
}
X86_INLINE uint32_t rm_get(x86cpu *c, const modrm_t *m, int sz) { return m->isreg ? reg_get(c, m->rm, sz) : mem_get(c, m->addr, sz); }
X86_INLINE void rm_set(x86cpu *c, const modrm_t *m, int sz, uint32_t v) { if (m->isreg) reg_set(c, m->rm, sz, v); else mem_set(c, m->addr, sz, v); }

X86_INLINE void push(x86cpu *c, uint32_t v, int sz)
{
    if (sz == 16) { c->r[X86_ESP] -= 2; m_w16(c, c->r[X86_ESP], (uint16_t)v); }
    else { c->r[X86_ESP] -= 4; m_w32(c, c->r[X86_ESP], v); }
}
X86_INLINE uint32_t pop(x86cpu *c, int sz)
{
    uint32_t v;
    if (sz == 16) { v = m_r16(c, c->r[X86_ESP]); c->r[X86_ESP] += 2; }
    else { v = m_r32(c, c->r[X86_ESP]); c->r[X86_ESP] += 4; }
    return v;
}

/* ======================================================================= ALU */
static uint32_t alu(x86cpu *c, int op, uint32_t a, uint32_t b, int sz)
{
    uint32_t m = szmask(sz), r, cf;
    a &= m; b &= m;
    switch (op) {
    case 0: r = a + b; lf_set(c, LF_ADD, sz, r, a, b); break;
    case 1: r = a | b; lf_set(c, LF_LOGIC, sz, r, a, b); break;
    case 2: cf = (uint32_t)get_cf(c); r = a + b + cf; lf_set(c, LF_ADC, sz, r, a, b); c->lf_cf = cf; break;
    case 3: cf = (uint32_t)get_cf(c); r = a - b - cf; lf_set(c, LF_SBB, sz, r, a, b); c->lf_cf = cf; break;
    case 4: r = a & b; lf_set(c, LF_LOGIC, sz, r, a, b); break;
    case 5: case 7: r = a - b; lf_set(c, LF_SUB, sz, r, a, b); break;
    default: r = a ^ b; lf_set(c, LF_LOGIC, sz, r, a, b); break;
    }
    return r & m;
}

/* 32-bit operands: no size masks; ADC/SBB keep the generic path */
X86_INLINE uint32_t alu32(x86cpu *c, int op, uint32_t a, uint32_t b)
{
    uint32_t r;
    switch (op) {
    case 0: r = a + b; c->lf_op = LF_ADD; break;
    case 1: r = a | b; c->lf_op = LF_LOGIC; break;
    case 4: r = a & b; c->lf_op = LF_LOGIC; break;
    case 5: case 7: r = a - b; c->lf_op = LF_SUB; break;
    case 6: r = a ^ b; c->lf_op = LF_LOGIC; break;
    default: return alu(c, op, a, b, 32);
    }
    c->lf_sz = 32; c->lf_res = r; c->lf_a = a; c->lf_b = b;
    return r;
}

static uint32_t incdec(x86cpu *c, uint32_t a, int sz, int dec)
{
    uint32_t cf = get_cf(c) ? X86_CF : 0, r = dec ? a - 1 : a + 1;
    lf_set(c, dec ? LF_DEC : LF_INC, sz, r, a, 1);
    c->lf_cf = cf;
    return r & szmask(sz);
}

/* shifts and rotates: op = modrm.reg (0 ROL 1 ROR 2 RCL 3 RCR 4 SHL 5 SHR 6 SAL 7 SAR) */
static uint32_t shift(x86cpu *c, int op, uint32_t a, unsigned n, int sz)
{
    uint32_t m = szmask(sz), s = szsign(sz), r;
    n &= 31;
    a &= m;
    if (!n) return a;
    switch (op) {
    case 0: case 1: {
        unsigned k = n % (unsigned)sz;
        uint32_t cfv, of;
        r = k ? (op == 0 ? ((a << k) | (a >> (sz - k))) : ((a >> k) | (a << (sz - k)))) & m : a;
        if (op == 0) { cfv = r & 1u; of = ((r & s) ? 1u : 0u) ^ cfv; }
        else { cfv = (r & s) ? 1u : 0u; of = ((r & s) ? 1u : 0u) ^ ((r & (s >> 1)) ? 1u : 0u); }
        set_arith(c, X86_CF | X86_OF, (cfv ? X86_CF : 0) | (of ? X86_OF : 0));
        return r;
    }
    case 2: case 3: {
        unsigned k = n % (unsigned)(sz + 1), i;
        uint32_t cf = (uint32_t)get_cf(c), of;
        r = a;
        for (i = 0; i < k; i++) {
            if (op == 2) { uint32_t nc = (r & s) ? 1u : 0u; r = ((r << 1) | cf) & m; cf = nc; }
            else { uint32_t nc = r & 1u; r = (r >> 1) | (cf ? s : 0); cf = nc; }
        }
        if (op == 2) of = ((r & s) ? 1u : 0u) ^ cf;
        else of = ((r & s) ? 1u : 0u) ^ ((r & (s >> 1)) ? 1u : 0u);
        if (k) set_arith(c, X86_CF | X86_OF, (cf ? X86_CF : 0) | (of ? X86_OF : 0));
        return r;
    }
    case 4: case 6:
        r = n < 32 ? (a << n) & m : 0;
        if (n > (unsigned)sz) r = 0;
        lf_set(c, LF_SHL, sz, r, a, n);
        return r;
    case 5:
        r = n < 32 ? a >> n : 0;
        lf_set(c, LF_SHR, sz, r, a, n);
        return r & m;
    default: {
        int32_t sa = (int32_t)(a << (32 - sz)) >> (32 - sz);
        r = (uint32_t)(sa >> (n >= (unsigned)sz ? sz - 1 : (int)n)) & m;
        lf_set(c, LF_SAR, sz, r, a, n);
        return r;
    }
    }
}

static uint32_t shld(x86cpu *c, uint32_t dst, uint32_t src, unsigned n, int sz, int right)
{
    uint32_t m = szmask(sz), s = szsign(sz), r;
    uint64_t wide;
    n &= 31;
    if (!n) return dst & m;
    dst &= m; src &= m;
    if (sz == 16 && n > 16) {   /* undefined; emulate Intel-like by 32-bit funnel */
        n &= 15;
        if (!n) return dst;
    }
    if (!right) {
        wide = ((uint64_t)dst << sz) | src;
        r = (uint32_t)((wide << n) >> sz) & m;
        lf_set(c, LF_SHL, sz, r, dst, n);
        {
            uint32_t f = x86_get_flags(c);
            uint32_t cf = (uint32_t)((dst >> (sz - n)) & 1u);
            uint32_t of = ((r ^ dst) & s) ? X86_OF : 0;
            c->eflags = (f & ~(X86_CF | X86_OF)) | (cf ? X86_CF : 0) | of;
        }
    } else {
        wide = ((uint64_t)src << sz) | dst;
        r = (uint32_t)(wide >> n) & m;
        lf_set(c, LF_SHR, sz, r, dst, n);
        {
            uint32_t f = x86_get_flags(c);
            uint32_t cf = (dst >> (n - 1)) & 1u;
            uint32_t of = ((r ^ dst) & s) ? X86_OF : 0;
            c->eflags = (f & ~(X86_CF | X86_OF)) | (cf ? X86_CF : 0) | of;
        }
    }
    return r;
}

/* ======================================================================= x87 */
#define ST(i) c->st[(c->top + (i)) & 7]
static void fpush(x86cpu *c, double v)
{
    c->top = (c->top - 1) & 7;
    c->st[c->top] = v;
    c->ftw &= (uint16_t)~(3u << (c->top * 2));
}
static void fpop(x86cpu *c)
{
    c->ftw |= (uint16_t)(3u << (c->top * 2));
    c->top = (c->top + 1) & 7;
}
static void fset_cc(x86cpu *c, int c3, int c2, int c1, int c0)
{
    c->fsw = (uint16_t)((c->fsw & ~0x4700u) | (c3 ? 0x4000u : 0) | (c2 ? 0x400u : 0) | (c1 ? 0x200u : 0) | (c0 ? 0x100u : 0));
}
static void fcompare(x86cpu *c, double a, double b)
{
    if (a != a || b != b) fset_cc(c, 1, 1, 0, 1);
    else if (a > b) fset_cc(c, 0, 0, 0, 0);
    else if (a < b) fset_cc(c, 0, 0, 0, 1);
    else fset_cc(c, 1, 0, 0, 0);
}
static void fcomi(x86cpu *c, double a, double b)
{
    uint32_t v;
    if (a != a || b != b) v = X86_ZF | X86_PF | X86_CF;
    else if (a > b) v = 0;
    else if (a < b) v = X86_CF;
    else v = X86_ZF;
    set_arith(c, X86_ZF | X86_PF | X86_CF | X86_OF | X86_SF | X86_AF, v);
}
static double fround(x86cpu *c, double v)
{
    double f;
    switch ((c->fcw >> 10) & 3) {
    case 1: return floor(v);
    case 2: return ceil(v);
    case 3: return v < 0 ? ceil(v) : floor(v);
    default:
        f = floor(v);
        if (v - f > 0.5) return f + 1.0;
        if (v - f < 0.5) return f;
        return fmod(f, 2.0) == 0.0 ? f : f + 1.0;
    }
}
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static uint64_t m_r64(x86cpu *c, uint32_t a) { return (uint64_t)m_r32(c, a) | ((uint64_t)m_r32(c, a + 4) << 32); }
static void m_w64(x86cpu *c, uint32_t a, uint64_t v) { m_w32(c, a, (uint32_t)v); m_w32(c, a + 4, (uint32_t)(v >> 32)); }

static double bits2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
#define X87_INDEFINITE 0xFFF8000000000000ull
static double ld80(x86cpu *c, uint32_t a)
{
    uint64_t man = m_r64(c, a);
    uint16_t se = m_r16(c, a + 8);
    int e = se & 0x7FFF, neg = se >> 15;
    double v;
    if (e == 0 && man == 0) v = 0.0;
    else if (e == 0x7FFF) v = (man << 1) ? bits2d(0x7FF8000000000000ull | ((man >> 11) & 0xFFFFFFFFFFFFFull)) : bits2d(0x7FF0000000000000ull);
    else v = ldexp((double)man, e - 16383 - 63);
    return neg ? -v : v;
}
static void st80(x86cpu *c, uint32_t a, double v)
{
    uint64_t man = 0;
    int e = 0, neg = signbit(v) ? 1 : 0;
    if (neg) v = -v;
    if (v != v) { e = 0x7FFF; man = 0xC000000000000000ull; }
    else if (isinf(v)) { e = 0x7FFF; man = 0x8000000000000000ull; }
    else if (v != 0.0) {
        int ex;
        double fr = frexp(v, &ex);            /* v = fr * 2^ex, 0.5 <= fr < 1 */
        man = x86_d2u64(ldexp(fr, 64));
        e = ex - 1 + 16383;
    }
    m_w64(c, a, man);
    m_w16(c, a + 8, (uint16_t)(e | (neg << 15)));
}
static int64_t fto_int(x86cpu *c, double v, int bits, int trunc)
{
    double r = trunc ? (v < 0 ? ceil(v) : floor(v)) : fround(c, v);
    double lim = bits == 16 ? 32768.0 : bits == 32 ? 2147483648.0 : 9223372036854775808.0;
    if (r != r || r >= lim || r < -lim) return bits == 16 ? -32768 : bits == 32 ? (int64_t)INT32_MIN : INT64_MIN;
    return x86_d2i64(r);
}
/* x87 NaN semantics, independent of the host FPU (the 68k FPU's default NaN is
 * 7FFF...FFFF, x86 uses the negative "real indefinite" FFF8000000000000) */
static double nanfix(double r, double a, double b)
{
    uint64_t ua, ub;
    int na, nb;
    if (r == r) return r;
    memcpy(&ua, &a, 8); memcpy(&ub, &b, 8);
    na = a != a; nb = b != b;
    if (na && nb) return bits2d(((ua & 0xFFFFFFFFFFFFFull) >= (ub & 0xFFFFFFFFFFFFFull) ? ua : ub) | 0x0008000000000000ull);
    if (na) return bits2d(ua | 0x0008000000000000ull);
    if (nb) return bits2d(ub | 0x0008000000000000ull);
    return bits2d(X87_INDEFINITE);
}
static void fpu_arith(double *dst, double src, int op)
{
    double a = *dst;
    switch (op) {
    case 0: *dst = *dst + src; break;
    case 1: *dst = *dst * src; break;
    case 4: *dst = *dst - src; break;
    case 5: *dst = src - *dst; break;
    case 6: *dst = *dst / src; break;
    case 7: *dst = src / *dst; break;
    }
    *dst = nanfix(*dst, a, src);
}
static void fxam(x86cpu *c)
{
    /* classify by bit pattern only (host FPU compares on NaN/inf are not trusted) */
    uint64_t u = d2u(ST(0));
    uint64_t ex = (u >> 52) & 0x7FF, mant = u & 0xFFFFFFFFFFFFFull;
    int empty = ((c->ftw >> (c->top * 2)) & 3) == 3;
    int sign = (int)(u >> 63);
    if (empty) fset_cc(c, 1, 0, sign, 1);
    else if (ex == 0x7FF && mant) fset_cc(c, 0, 0, sign, 1);          /* NaN */
    else if (ex == 0x7FF) fset_cc(c, 0, 1, sign, 1);                  /* infinity */
    else if (!ex && !mant) fset_cc(c, 1, 0, sign, 0);                 /* zero */
    else if (!ex) fset_cc(c, 1, 1, sign, 0);                          /* denormal */
    else fset_cc(c, 0, 1, sign, 0);
}

/* FSIN/FCOS/FPTAN/FSINCOS: |x| >= 2^63 leaves ST(0) unchanged with C2 = 1; inf -> default NaN */
static int trig_range(x86cpu *c)
{
    double v = ST(0);
    if (isinf(v)) { ST(0) = bits2d(X87_INDEFINITE); fset_cc(c, 0, 0, 0, 0); return 1; }
    if (fabs(v) >= 9223372036854775808.0) { fset_cc(c, 0, 1, 0, 0); return 1; }
    return 0;
}

static void fpu_exec(x86cpu *c, const prefix_t *p, uint8_t op)
{
    modrm_t m;
    int grp;
    decode_modrm(c, p, &m);
    grp = m.reg;
    if (!m.isreg) {
        uint32_t a = m.addr;
        switch (op) {
        case 0xD8: case 0xDC: case 0xDA: case 0xDE: {
            double v;
            if (op == 0xD8) v = u2f(m_r32(c, a));
            else if (op == 0xDC) v = u2d(m_r64(c, a));
            else if (op == 0xDA) v = (double)(int32_t)m_r32(c, a);
            else v = (double)(int16_t)m_r16(c, a);
            if (grp == 2 || grp == 3) { fcompare(c, ST(0), v); if (grp == 3) fpop(c); }
            else fpu_arith(&ST(0), v, grp);
            return;
        }
        case 0xD9:
            switch (grp) {
            case 0: fpush(c, u2f(m_r32(c, a))); return;
            case 2: m_w32(c, a, f2u((float)ST(0))); return;
            case 3: m_w32(c, a, f2u((float)ST(0))); fpop(c); return;
            case 4: { /* FLDENV: 28 bytes, protected 32-bit format */
                c->fcw = m_r16(c, a); c->fsw = m_r16(c, a + 4); c->top = (c->fsw >> 11) & 7; c->ftw = m_r16(c, a + 8); return; }
            case 5: c->fcw = m_r16(c, a); return;
            case 6: { int i; uint16_t sw = (uint16_t)((c->fsw & ~0x3800u) | (c->top << 11));
                m_w32(c, a, c->fcw | 0xFFFF0000u); m_w32(c, a + 4, sw | 0xFFFF0000u); m_w32(c, a + 8, c->ftw | 0xFFFF0000u);
                for (i = 12; i < 28; i += 4) m_w32(c, a + i, 0);
                return; }
            case 7: m_w16(c, a, c->fcw); return;
            }
            break;
        case 0xDB:
            switch (grp) {
            case 0: fpush(c, (double)(int32_t)m_r32(c, a)); return;
            case 1: m_w32(c, a, (uint32_t)(int32_t)fto_int(c, ST(0), 32, 1)); fpop(c); return;
            case 2: m_w32(c, a, (uint32_t)(int32_t)fto_int(c, ST(0), 32, 0)); return;
            case 3: m_w32(c, a, (uint32_t)(int32_t)fto_int(c, ST(0), 32, 0)); fpop(c); return;
            case 5: fpush(c, ld80(c, a)); return;
            case 7: st80(c, a, ST(0)); fpop(c); return;
            }
            break;
        case 0xDD:
            switch (grp) {
            case 0: fpush(c, u2d(m_r64(c, a))); return;
            case 1: m_w64(c, a, (uint64_t)fto_int(c, ST(0), 64, 1)); fpop(c); return;
            case 2: m_w64(c, a, d2u(ST(0))); return;
            case 3: m_w64(c, a, d2u(ST(0))); fpop(c); return;
            case 4: { int i; /* FRSTOR (108 bytes) */
                c->fcw = m_r16(c, a); c->fsw = m_r16(c, a + 4); c->top = (c->fsw >> 11) & 7; c->ftw = m_r16(c, a + 8);
                for (i = 0; i < 8; i++) c->st[(c->top + i) & 7] = ld80(c, a + 28 + i * 10);
                return; }
            case 6: { int i; uint16_t sw = (uint16_t)((c->fsw & ~0x3800u) | (c->top << 11));
                m_w32(c, a, c->fcw | 0xFFFF0000u); m_w32(c, a + 4, sw | 0xFFFF0000u); m_w32(c, a + 8, c->ftw | 0xFFFF0000u);
                for (i = 12; i < 28; i += 4) m_w32(c, a + i, 0);
                for (i = 0; i < 8; i++) st80(c, a + 28 + i * 10, c->st[(c->top + i) & 7]);
                c->fcw = 0x037F; c->fsw = 0; c->ftw = 0xFFFF; c->top = 0; return; }
            case 7: m_w16(c, a, (uint16_t)((c->fsw & ~0x3800u) | (c->top << 11))); return;
            }
            break;
        case 0xDF:
            switch (grp) {
            case 0: fpush(c, (double)(int16_t)m_r16(c, a)); return;
            case 1: m_w16(c, a, (uint16_t)(int16_t)fto_int(c, ST(0), 16, 1)); fpop(c); return;
            case 2: m_w16(c, a, (uint16_t)(int16_t)fto_int(c, ST(0), 16, 0)); return;
            case 3: m_w16(c, a, (uint16_t)(int16_t)fto_int(c, ST(0), 16, 0)); fpop(c); return;
            case 5: fpush(c, (double)(int64_t)m_r64(c, a)); return;
            case 7: m_w64(c, a, (uint64_t)fto_int(c, ST(0), 64, 0)); fpop(c); return;
            }
            break;
        }
        FAULT(c, X86_FAULT_UD, c->cur_eip, "unsupported x87 memory form");
    }
    /* register forms */
    {
        int i = m.rm;
        uint8_t sub = (uint8_t)(0xC0 | (m.reg << 3) | m.rm);
        switch (op) {
        case 0xD8:
            if (grp == 2 || grp == 3) { fcompare(c, ST(0), ST(i)); if (grp == 3) fpop(c); }
            else fpu_arith(&ST(0), ST(i), grp);
            return;
        case 0xDC:
            if (grp == 2 || grp == 3) { fcompare(c, ST(0), ST(i)); if (grp == 3) fpop(c); return; }
            { static const int map[8] = { 0, 1, 2, 3, 5, 4, 7, 6 }; fpu_arith(&ST(i), ST(0), map[grp]); }
            return;
        case 0xDE:
            if (grp == 3 && i == 1) { fcompare(c, ST(0), ST(1)); fpop(c); fpop(c); return; }
            { static const int map[8] = { 0, 1, 2, 3, 5, 4, 7, 6 }; fpu_arith(&ST(i), ST(0), map[grp]); fpop(c); }
            return;
        case 0xD9:
            if (grp == 0) { double v = ST(i); fpush(c, v); return; }
            if (grp == 1) { double t = ST(0); ST(0) = ST(i); ST(i) = t; return; }
            switch (sub) {
            case 0xD0: return;                                         /* FNOP */
            case 0xE0: ST(0) = -ST(0); return;
            case 0xE1: ST(0) = fabs(ST(0)); return;
            case 0xE4: fcompare(c, ST(0), 0.0); return;
            case 0xE5: fxam(c); return;
            case 0xE8: fpush(c, 1.0); return;
            case 0xE9: fpush(c, 3.321928094887362347870); return;     /* log2(10) */
            case 0xEA: fpush(c, 1.442695040888963407360); return;     /* log2(e) */
            case 0xEB: fpush(c, 3.141592653589793238463); return;
            case 0xEC: fpush(c, 0.301029995663981195214); return;     /* log10(2) */
            case 0xED: fpush(c, 0.693147180559945309417); return;     /* ln(2) */
            case 0xEE: fpush(c, 0.0); return;
            case 0xF0: { double a = ST(0); ST(0) = nanfix(expm1(a * 0.693147180559945309417), a, a); return; }
            case 0xF1: { double a = ST(0), b = ST(1); ST(1) = nanfix(b * (log(a) / 0.693147180559945309417), a, b); fpop(c); return; }
            case 0xF2: if (trig_range(c)) return; { double a = ST(0); ST(0) = nanfix(tan(a), a, a); } fpush(c, 1.0); fset_cc(c, 0, 0, 0, 0); return;
            case 0xF3: { double a = ST(0), b = ST(1); ST(1) = nanfix(atan2(b, a), a, b); fpop(c); return; }
            case 0xF4: { int e; double fr = frexp(ST(0), &e); ST(0) = (double)(e - 1); fpush(c, fr * 2.0); return; }
            case 0xF5: case 0xF8: {
                double a = ST(0), b = ST(1), r, q;
                r = sub == 0xF8 ? fmod(a, b) : remainder(a, b);
                q = (a - r) / b;
                ST(0) = r;
                { int64_t qi = fabs(q) < 9.2e18 ? x86_d2i64(fabs(q)) : 0; fset_cc(c, (qi >> 1) & 1, 0, qi & 1, (qi >> 2) & 1); }
                return;
            }
            case 0xF6: c->top = (c->top - 1) & 7; return;
            case 0xF7: c->top = (c->top + 1) & 7; return;
            case 0xF9: { double a = ST(0), b = ST(1); ST(1) = nanfix(b * (log1p(a) / 0.693147180559945309417), a, b); fpop(c); return; }
            case 0xFA: { double a = ST(0); ST(0) = nanfix(a < 0 ? bits2d(X87_INDEFINITE) : qm_sqrt(a), a, a); return; }
            case 0xFB: { double v = ST(0); if (trig_range(c)) return; ST(0) = nanfix(sin(v), v, v); fpush(c, nanfix(cos(v), v, v)); fset_cc(c, 0, 0, 0, 0); return; }
            case 0xFC: ST(0) = fround(c, ST(0)); return;
            case 0xFD: { double s = ST(1); s = s < 0 ? ceil(s) : floor(s); ST(0) = ldexp(ST(0), (int)s); return; }
            case 0xFE: if (trig_range(c)) return; { double a = ST(0); ST(0) = nanfix(sin(a), a, a); } fset_cc(c, 0, 0, 0, 0); return;
            case 0xFF: if (trig_range(c)) return; { double a = ST(0); ST(0) = nanfix(cos(a), a, a); } fset_cc(c, 0, 0, 0, 0); return;
            }
            break;
        case 0xDA:
            if (grp <= 3) {     /* FCMOVB/E/BE/U */
                static const int cc[4] = { 2, 4, 6, 10 };
                if (cond(c, cc[grp])) ST(0) = ST(i);
                return;
            }
            if (sub == 0xE9) { fcompare(c, ST(0), ST(1)); fpop(c); fpop(c); return; }   /* FUCOMPP */
            break;
        case 0xDB:
            if (grp <= 3) {     /* FCMOVNB/NE/NBE/NU */
                static const int cc[4] = { 3, 5, 7, 11 };
                if (cond(c, cc[grp])) ST(0) = ST(i);
                return;
            }
            if (sub == 0xE2) { c->fsw &= 0x7F00u; return; }                              /* FNCLEX */
            if (sub == 0xE3) { c->fcw = 0x037F; c->fsw = 0; c->ftw = 0xFFFF; c->top = 0; return; } /* FNINIT */
            if (grp == 5 || grp == 6) { fcomi(c, ST(0), ST(i)); return; }               /* FUCOMI/FCOMI */
            if (sub == 0xE0 || sub == 0xE1 || sub == 0xE4) return;                       /* FENI/FDISI/FSETPM */
            break;
        case 0xDD:
            if (grp == 0) { c->ftw |= (uint16_t)(3u << (((c->top + i) & 7) * 2)); return; }  /* FFREE */
            if (grp == 2) { ST(i) = ST(0); return; }
            if (grp == 3) { ST(i) = ST(0); fpop(c); return; }
            if (grp == 4 || grp == 5) { fcompare(c, ST(0), ST(i)); if (grp == 5) fpop(c); return; }
            break;
        case 0xDF:
            if (sub == 0xE0) { reg_set(c, X86_EAX, 16, (uint16_t)((c->fsw & ~0x3800u) | (c->top << 11))); return; }
            if (grp == 5 || grp == 6) { fcomi(c, ST(0), ST(i)); fpop(c); return; }      /* FUCOMIP/FCOMIP */
            if (grp == 0) { c->ftw |= (uint16_t)(3u << (((c->top + i) & 7) * 2)); fpop(c); return; }
            break;
        }
    }
    FAULT(c, X86_FAULT_UD, c->cur_eip, "unsupported x87 register form");
}

/* ======================================================================= helpers for string ops */
static int str_should_continue(x86cpu *c, const prefix_t *p)
{
    uint32_t cnt = p->asz == 16 ? (c->r[X86_ECX] & 0xFFFF) : c->r[X86_ECX];
    return cnt != 0;
}
static void str_dec_count(x86cpu *c, const prefix_t *p)
{
    if (p->asz == 16) reg_set(c, X86_ECX, 16, (c->r[X86_ECX] - 1) & 0xFFFF);
    else c->r[X86_ECX]--;
}
static uint32_t si_addr(x86cpu *c, const prefix_t *p) { return seg_base(c, p, X86_DS) + (p->asz == 16 ? (c->r[X86_ESI] & 0xFFFF) : c->r[X86_ESI]); }
static uint32_t di_addr(x86cpu *c, const prefix_t *p) { return c->sbase[X86_ES] + (p->asz == 16 ? (c->r[X86_EDI] & 0xFFFF) : c->r[X86_EDI]); }
static void adv(x86cpu *c, const prefix_t *p, int r, int n)
{
    int d = (c->eflags & X86_DF) ? -n : n;
    if (p->asz == 16) reg_set(c, r, 16, (c->r[r] + (uint32_t)d) & 0xFFFF);
    else c->r[r] += (uint32_t)d;
}

/* string instruction with REP: executes at most 'chunk' iterations, then re-executes
 * itself (eip rewound) so watchdog/hooks stay responsive */
static void string_op(x86cpu *c, const prefix_t *p, uint8_t op, uint32_t start_eip)
{
    int sz = (op & 1) ? p->osz : 8, n = sz / 8;
    int rep = p->rep, iter = 0;
    for (;;) {
        if (rep && !str_should_continue(c, p)) return;
        switch (op) {
        case 0xA4: case 0xA5: mem_set(c, di_addr(c, p), sz, mem_get(c, si_addr(c, p), sz)); adv(c, p, X86_ESI, n); adv(c, p, X86_EDI, n); break;
        case 0xAA: case 0xAB: mem_set(c, di_addr(c, p), sz, reg_get(c, X86_EAX, sz)); adv(c, p, X86_EDI, n); break;
        case 0xAC: case 0xAD: reg_set(c, X86_EAX, sz, mem_get(c, si_addr(c, p), sz)); adv(c, p, X86_ESI, n); break;
        case 0xA6: case 0xA7: { uint32_t a = mem_get(c, si_addr(c, p), sz), b = mem_get(c, di_addr(c, p), sz);
            alu(c, 7, a, b, sz); adv(c, p, X86_ESI, n); adv(c, p, X86_EDI, n); break; }
        case 0xAE: case 0xAF: { uint32_t b = mem_get(c, di_addr(c, p), sz);
            alu(c, 7, reg_get(c, X86_EAX, sz), b, sz); adv(c, p, X86_EDI, n); break; }
        }
        if (!rep) return;
        str_dec_count(c, p);
        if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
            int zf = cond(c, 4);
            if (rep == 0xF3 && !zf) return;
            if (rep == 0xF2 && zf) return;
        }
        if (++iter >= 4096) {
            if (str_should_continue(c, p)) c->eip = start_eip;    /* resume later */
            if (c->left >= (uint32_t)iter) c->left -= (uint32_t)iter;
            else { c->icount += (uint32_t)iter - c->left; c->left = 0; }
            return;
        }
    }
}

/* ======================================================================= main step */
static void bad(x86cpu *c, const char *msg) { FAULT(c, X86_FAULT_UD, c->cur_eip, msg); }

static const uint8_t prefix_tab[256] = {
    [0x26] = 1, [0x2E] = 1, [0x36] = 1, [0x3E] = 1, [0x64] = 1, [0x65] = 1,
    [0x66] = 1, [0x67] = 1, [0xF0] = 1, [0xF2] = 1, [0xF3] = 1
};

#include "qrx86jit.h"

/* inlined into run_loop only: keeps the CPU pointer and the loop state in registers */
X86_INLINE void exec_one_mode(x86cpu *c,uint32_t stop_eip,int jit,int *did_native)
{
    prefix_t p;
    uint8_t op;
    uint32_t start = c->eip;
    modrm_t m;
    int sz;

    c->cur_eip = start;
    if (start - c->hle_base < c->hle_count) {     /* hle_count 0: never */
        if (!c->hle_hook || !c->hle_hook(c, start - c->hle_base)) {
            if (!c->fault) FAULT(c, X86_FAULT_HOST, start, "HLE hook failed");
        }
        if (c->fault || c->stop) longjmp(*c->jb, 1);     /* the run loop tests neither */
        return;
    }
    p.osz = 32; p.asz = 32; p.rep = 0; p.seg = -1;
    op = fetch8(c);
#if XJ_NATIVE && QRX_JIT_FUSED_FETCH
    /* Reuse the already charged/fetched opcode. A failed probe leaves the
     * original decoder immediately after its opcode, with no state rollback. */
    if(jit && c->jit && !prefix_tab[op] && xj_candidate_op(c,start,op) && xj_try(c,stop_eip,start,1)) {
        if(did_native)*did_native=1;
        return;
    }
#endif
    while (prefix_tab[op]) {                /* one table test for the common no-prefix case */
        switch (op) {
        case 0x66: p.osz = 16; break;
        case 0x67: p.asz = 16; break;
        case 0xF2: case 0xF3: p.rep = op; break;
        case 0x26: p.seg = X86_ES; break;
        case 0x2E: p.seg = X86_CS; break;
        case 0x36: p.seg = X86_SS; break;
        case 0x3E: p.seg = X86_DS; break;
        case 0x64: p.seg = X86_FS; break;
        case 0x65: p.seg = X86_GS; break;
        default: break;                      /* 0xF0 LOCK */
        }
        op = fetch8(c);
    }
#ifdef QRX_EXEC_PROFILE
    c->prof_ops[op]++;c->prof_active_op=op;
    if (!c->prof_sample_left--) {
        unsigned slot = ((start >> 4) ^ (start >> 16)) & 4095u;
        uint32_t group = start & ~15u;
        c->prof_sample_left = 1023;
        if (!c->prof_pc_hits[slot]) c->prof_pc[slot] = group;
        if (c->prof_pc[slot] == group) c->prof_pc_hits[slot]++;
        else c->prof_pc_collisions++;
    }
#endif
    sz = p.osz;

    /* one switch for every one-byte opcode: a single jump table instead of range tests */
    switch (op) {
    case 0x00 ... 0x05: case 0x08 ... 0x0D: case 0x10 ... 0x15: case 0x18 ... 0x1D:
    case 0x20 ... 0x25: case 0x28 ... 0x2D: case 0x30 ... 0x35: case 0x38 ... 0x3D: {   /* ALU group */
        int aop = op >> 3, form = op & 7;
        uint32_t r;
        if (sz == 32 && (form == 1 || form == 3)) {
            uint32_t v;
            decode_modrm(c, &p, &m);
            v = m.isreg ? c->r[m.rm] : m_r32(c, m.addr);
            if (form == 3) { r = alu32(c, aop, c->r[m.reg], v); if (aop != 7) c->r[m.reg] = r; return; }
            r = alu32(c, aop, v, c->r[m.reg]);
            if (aop != 7) { if (m.isreg) c->r[m.rm] = r; else m_w32(c, m.addr, r); }
            return;
        }
        switch (form) {
        case 0: case 1: { int s = form ? sz : 8; decode_modrm(c, &p, &m);
            r = alu(c, aop, rm_get(c, &m, s), reg_get(c, m.reg, s), s); if (aop != 7) rm_set(c, &m, s, r); return; }
        case 2: case 3: { int s = form == 3 ? sz : 8; decode_modrm(c, &p, &m);
            r = alu(c, aop, reg_get(c, m.reg, s), rm_get(c, &m, s), s); if (aop != 7) reg_set(c, m.reg, s, r); return; }
        case 4: r = alu(c, aop, reg_get(c, X86_EAX, 8), fetch8(c), 8); if (aop != 7) reg_set(c, X86_EAX, 8, r); return;
        default: r = alu(c, aop, reg_get(c, X86_EAX, sz), fetchimm(c, sz), sz); if (aop != 7) reg_set(c, X86_EAX, sz, r); return;
        }
    }
    case 0x06: push(c, c->sreg[X86_ES], sz); return;
    case 0x07: c->sreg[X86_ES] = (uint16_t)pop(c, sz); return;
    case 0x0E: push(c, c->sreg[X86_CS], sz); return;
    case 0x16: push(c, c->sreg[X86_SS], sz); return;
    case 0x17: c->sreg[X86_SS] = (uint16_t)pop(c, sz); return;
    case 0x1E: push(c, c->sreg[X86_DS], sz); return;
    case 0x1F: c->sreg[X86_DS] = (uint16_t)pop(c, sz); return;
    case 0x27: case 0x2F: { /* DAA / DAS */
        uint32_t f = x86_get_flags(c), al = c->r[X86_EAX] & 0xFF, oal = al, cf = 0;
        int af = (f & X86_AF) != 0, ocf = (f & X86_CF) != 0;
        if ((al & 0xF) > 9 || af) { al = op == 0x27 ? al + 6 : al - 6; cf = ocf || (op == 0x27 ? (oal > 0xF9) : (oal < 6)); af = 1; } else af = 0;
        if (oal > 0x99 || ocf) { al = op == 0x27 ? al + 0x60 : al - 0x60; cf = 1; }
        al &= 0xFF; reg_set(c, X86_EAX, 8, al);
        c->eflags = (f & ~X86_ARITH) | (cf ? X86_CF : 0) | (af ? X86_AF : 0) | (al ? 0 : X86_ZF) | (al & 0x80 ? X86_SF : 0) | (parity_tab[al] ? X86_PF : 0);
        return; }
    case 0x37: case 0x3F: { /* AAA / AAS */
        uint32_t f = x86_get_flags(c), ax = c->r[X86_EAX] & 0xFFFF;
        if ((ax & 0xF) > 9 || (f & X86_AF)) {
            ax = op == 0x37 ? ax + 0x106 : ax - 0x106;
            f |= X86_AF | X86_CF;
        } else f &= ~(X86_AF | X86_CF);
        reg_set(c, X86_EAX, 16, ax & 0xFF0F); c->eflags = f; return; }
    case 0x40 ... 0x4F: { int r = op & 7; reg_set(c, r, sz, incdec(c, reg_get(c, r, sz), sz, op >= 0x48)); return; }
    case 0x50 ... 0x57: if (sz == 32) push(c, c->r[op & 7], 32); else push(c, reg_get(c, op & 7, 16), 16); return;
    case 0x58 ... 0x5F: { uint32_t v; if (sz == 32) { v = pop(c, 32); c->r[op & 7] = v; } else { v = pop(c, 16); reg_set(c, op & 7, 16, v); } return; }
    case 0x70 ... 0x7F: { int8_t d = (int8_t)fetch8(c); if (cond(c, op & 15)) c->eip += (uint32_t)(int32_t)d; return; }
    case 0x91 ... 0x97: { uint32_t t = reg_get(c, X86_EAX, sz); reg_set(c, X86_EAX, sz, reg_get(c, op & 7, sz)); reg_set(c, op & 7, sz, t); return; }
    case 0xB0 ... 0xB7: reg_set(c, op & 7, 8, fetch8(c)); return;
    case 0xB8 ... 0xBF: reg_set(c, op & 7, sz, fetchimm(c, sz)); return;
    case 0xD8 ... 0xDF: fpu_exec(c, &p, op); return;
    case 0x60: { uint32_t sp = c->r[X86_ESP]; int i; for (i = 0; i < 8; i++) push(c, i == 4 ? sp : reg_get(c, i, sz), sz); return; }
    case 0x61: { int i; for (i = 7; i >= 0; i--) { uint32_t v = pop(c, sz); if (i != 4) reg_set(c, i, sz, v); } return; }
    case 0x68: push(c, fetchimm(c, sz), sz); return;
    case 0x6A: push(c, (uint32_t)(int32_t)(int8_t)fetch8(c), sz); return;
    case 0x69: case 0x6B: {
        int64_t a, b, r; int bad16;
        decode_modrm(c, &p, &m);
        a = sz == 16 ? (int16_t)rm_get(c, &m, 16) : (int32_t)rm_get(c, &m, 32);
        b = op == 0x6B ? (int8_t)fetch8(c) : (sz == 16 ? (int16_t)fetch16(c) : (int32_t)fetch32(c));
        r = a * b;
        bad16 = sz == 16 ? (r != (int16_t)r) : (r != (int32_t)r);
        reg_set(c, m.reg, sz, (uint32_t)r);
        lf_set(c, LF_LOGIC, sz, (uint32_t)r, 0, 0); x86_get_flags(c);
        set_arith(c, X86_CF | X86_OF, bad16 ? (X86_CF | X86_OF) : 0);
        return; }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        int s = (op == 0x81 || op == 0x83) ? sz : 8;
        uint32_t imm, r;
        decode_modrm(c, &p, &m);
        imm = op == 0x81 ? fetchimm(c, s) : (op == 0x83 ? (uint32_t)(int32_t)(int8_t)fetch8(c) : fetch8(c));
        if (s == 32) {
            r = alu32(c, m.reg, m.isreg ? c->r[m.rm] : m_r32(c, m.addr), imm);
            if (m.reg != 7) { if (m.isreg) c->r[m.rm] = r; else m_w32(c, m.addr, r); }
            return;
        }
        r = alu(c, m.reg, rm_get(c, &m, s), imm, s);
        if (m.reg != 7) rm_set(c, &m, s, r);
        return; }
    case 0x84: decode_modrm(c, &p, &m); alu(c, 4, rm_get(c, &m, 8), reg_get(c, m.reg, 8), 8); return;
    case 0x85: decode_modrm(c, &p, &m);
        if (sz == 32) alu32(c, 4, m.isreg ? c->r[m.rm] : m_r32(c, m.addr), c->r[m.reg]);
        else alu(c, 4, rm_get(c, &m, 16), reg_get(c, m.reg, 16), 16);
        return;
    case 0x86: case 0x87: { int s = op & 1 ? sz : 8; uint32_t t; decode_modrm(c, &p, &m);
        t = rm_get(c, &m, s); rm_set(c, &m, s, reg_get(c, m.reg, s)); reg_set(c, m.reg, s, t); return; }
    case 0x88: decode_modrm(c, &p, &m); rm_set(c, &m, 8, reg_get(c, m.reg, 8)); return;
    case 0x8A: decode_modrm(c, &p, &m); reg_set(c, m.reg, 8, rm_get(c, &m, 8)); return;
    case 0x89: decode_modrm(c, &p, &m);
        if (sz != 32) rm_set(c, &m, 16, reg_get(c, m.reg, 16));
        else if (m.isreg) c->r[m.rm] = c->r[m.reg];
        else m_w32(c, m.addr, c->r[m.reg]);
        return;
    case 0x8B: decode_modrm(c, &p, &m);
        if (sz != 32) reg_set(c, m.reg, 16, rm_get(c, &m, 16));
        else c->r[m.reg] = m.isreg ? c->r[m.rm] : m_r32(c, m.addr);
        return;
    case 0x8C: decode_modrm(c, &p, &m); if (m.reg > 5) bad(c, "bad sreg"); rm_set(c, &m, m.isreg ? sz : 16, c->sreg[m.reg]); return;
    case 0x8D: { prefix_t q = p; q.seg = X86_DS; decode_modrm(c, &q, &m); if (m.isreg) bad(c, "LEA reg"); reg_set(c, m.reg, sz, m.addr - c->sbase[X86_DS]); return; }
    case 0x8E: {
        uint16_t sel; decode_modrm(c, &p, &m); sel = (uint16_t)rm_get(c, &m, 16);
        if (m.reg > 5 || m.reg == X86_CS) bad(c, "bad sreg load");
        c->sreg[m.reg] = sel;
        if (m.reg == X86_FS || m.reg == X86_GS) {
            int idx = sel >> 3;
            if (idx >= 6 && idx <= 8 && c->tls[idx - 6].used) c->sbase[m.reg] = c->tls[idx - 6].base;
            else if (sel == 0) c->sbase[m.reg] = 0;
        }
        return; }
    case 0x8F: { uint32_t v = pop(c, sz); decode_modrm(c, &p, &m); rm_set(c, &m, sz, v); return; }
    case 0x90: if (p.rep == 0xF3) return; return;
    case 0x98: if (sz == 16) reg_set(c, X86_EAX, 16, (uint32_t)(int32_t)(int8_t)c->r[X86_EAX]); else c->r[X86_EAX] = (uint32_t)(int32_t)(int16_t)c->r[X86_EAX]; return;
    case 0x99: if (sz == 16) reg_set(c, X86_EDX, 16, (c->r[X86_EAX] & 0x8000) ? 0xFFFF : 0); else c->r[X86_EDX] = (c->r[X86_EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0; return;
    case 0x9B: return;
    case 0x9C: push(c, (x86_get_flags(c) & 0x00FCFFFFu), sz); return;
    case 0x9D: { uint32_t f = pop(c, sz); if (sz == 16) f = (x86_get_flags(c) & 0xFFFF0000u) | f; x86_set_flags(c, (f & 0x00244DD5u) | (c->eflags & X86_IF)); return; }
    case 0x9E: { uint32_t f = x86_get_flags(c); c->eflags = (f & ~0xD5u) | ((c->r[X86_EAX] >> 8) & 0xD5u); return; }
    case 0x9F: reg_set(c, X86_EAX + 4, 8, (x86_get_flags(c) & 0xD7u) | 2u); return;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        int s = op & 1 ? sz : 8;
        uint32_t a = (p.asz == 16 ? fetch16(c) : fetch32(c)) + seg_base(c, &p, X86_DS);
        if (op < 0xA2) reg_set(c, X86_EAX, s, mem_get(c, a, s)); else mem_set(c, a, s, reg_get(c, X86_EAX, s));
        return; }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, &p, op, start); return;
    case 0xA8: alu(c, 4, reg_get(c, X86_EAX, 8), fetch8(c), 8); return;
    case 0xA9: alu(c, 4, reg_get(c, X86_EAX, sz), fetchimm(c, sz), sz); return;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int s = op & 1 ? sz : 8; unsigned n;
        decode_modrm(c, &p, &m);
        { uint32_t v = rm_get(c, &m, s);
          n = (op <= 0xC1) ? fetch8(c) : (op <= 0xD1 ? 1u : (c->r[X86_ECX] & 0xFF));
          rm_set(c, &m, s, shift(c, m.reg, v, n, s)); }
        return; }
    case 0xC2: { uint16_t n = fetch16(c); c->eip = pop(c, sz); c->r[X86_ESP] += n; return; }
    case 0xC3: c->eip = pop(c, sz); return;
    case 0xC6: case 0xC7: { int s = op & 1 ? sz : 8; decode_modrm(c, &p, &m); rm_set(c, &m, s, fetchimm(c, s)); return; }
    case 0xC8: {
        uint16_t frame = fetch16(c); uint8_t lvl = (uint8_t)(fetch8(c) & 31); uint32_t fp;
        push(c, c->r[X86_EBP], sz); fp = c->r[X86_ESP];
        if (lvl) { int i; for (i = 1; i < lvl; i++) { c->r[X86_EBP] -= (uint32_t)(sz / 8); push(c, mem_get(c, c->r[X86_EBP], sz), sz); } push(c, fp, sz); }
        c->r[X86_EBP] = fp; c->r[X86_ESP] -= frame; return; }
    case 0xC9: c->r[X86_ESP] = c->r[X86_EBP]; reg_set(c, X86_EBP, sz, pop(c, sz)); return;
    case 0xCC: FAULT(c, X86_FAULT_INT3, start, "INT3");
    case 0xCD: { uint8_t v = fetch8(c);
        if (!c->int_hook || !c->int_hook(c, v)) { if (!c->fault) FAULT(c, X86_FAULT_GP, start, "unhandled software interrupt"); }
        if (c->fault || c->stop) longjmp(*c->jb, 1);
        return; }
    case 0xD4: { uint8_t b = fetch8(c), al = (uint8_t)c->r[X86_EAX]; if (!b) FAULT(c, X86_FAULT_DE, start, "AAM 0");
        reg_set(c, X86_EAX, 16, ((al / b) << 8) | (al % b)); lf_set(c, LF_LOGIC, 8, al % b, 0, 0); return; }
    case 0xD5: { uint8_t b = fetch8(c); uint32_t al = ((c->r[X86_EAX] >> 8) & 0xFF) * b + (c->r[X86_EAX] & 0xFF);
        reg_set(c, X86_EAX, 16, al & 0xFF); lf_set(c, LF_LOGIC, 8, al & 0xFF, 0, 0); return; }
    case 0xD6: reg_set(c, X86_EAX, 8, get_cf(c) ? 0xFF : 0); return;
    case 0xD7: reg_set(c, X86_EAX, 8, m_r8(c, seg_base(c, &p, X86_DS) + (p.asz == 16 ? (c->r[X86_EBX] & 0xFFFF) : c->r[X86_EBX]) + (c->r[X86_EAX] & 0xFF))); return;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
        int8_t d = (int8_t)fetch8(c); int take;
        if (op == 0xE3) take = p.asz == 16 ? !(c->r[X86_ECX] & 0xFFFF) : !c->r[X86_ECX];
        else {
            uint32_t cnt;
            if (p.asz == 16) { reg_set(c, X86_ECX, 16, (c->r[X86_ECX] - 1) & 0xFFFF); cnt = c->r[X86_ECX] & 0xFFFF; }
            else cnt = --c->r[X86_ECX];
            take = cnt != 0;
            if (op == 0xE0) take = take && !cond(c, 4);
            if (op == 0xE1) take = take && cond(c, 4);
        }
        if (take) c->eip += (uint32_t)(int32_t)d;
        return; }
    case 0xE8: { uint32_t d = fetchimm(c, sz); push(c, c->eip, sz); c->eip += d; if (sz == 16) c->eip &= 0xFFFF; return; }
    case 0xE9: { uint32_t d = fetchimm(c, sz); c->eip += d; if (sz == 16) c->eip &= 0xFFFF; return; }
    case 0xEB: { int8_t d = (int8_t)fetch8(c); c->eip += (uint32_t)(int32_t)d; return; }
    case 0xF4: FAULT(c, X86_FAULT_GP, start, "HLT in user mode");
    case 0xF5: { uint32_t f = x86_get_flags(c); c->eflags = f ^ X86_CF; return; }
    case 0xF6: case 0xF7: {
        int s = op & 1 ? sz : 8;
        decode_modrm(c, &p, &m);
        switch (m.reg) {
        case 0: case 1: alu(c, 4, rm_get(c, &m, s), fetchimm(c, s), s); return;
        case 2: rm_set(c, &m, s, ~rm_get(c, &m, s)); return;
        case 3: { uint32_t v = rm_get(c, &m, s), r = (0u - v) & szmask(s); rm_set(c, &m, s, r); lf_set(c, LF_NEG, s, r, 0, v); return; }
        case 4: case 5: {
            uint32_t v = rm_get(c, &m, s), hi = 0, lo; int ovf;
            if (s == 8) {
                uint32_t al = c->r[X86_EAX] & 0xFF; int32_t r16;
                if (m.reg == 4) { r16 = (int32_t)(al * v); ovf = (r16 & 0xFF00) != 0; }
                else { r16 = (int8_t)al * (int8_t)v; ovf = r16 != (int8_t)r16; }
                reg_set(c, X86_EAX, 16, (uint32_t)r16 & 0xFFFF); lo = (uint32_t)r16 & 0xFF;
            } else if (s == 16) {
                uint32_t ax = c->r[X86_EAX] & 0xFFFF; int32_t r32;
                if (m.reg == 4) { uint32_t u = ax * v; r32 = (int32_t)u; ovf = (u >> 16) != 0; }
                else { r32 = (int16_t)ax * (int16_t)v; ovf = r32 != (int16_t)r32; }
                reg_set(c, X86_EAX, 16, (uint32_t)r32 & 0xFFFF); reg_set(c, X86_EDX, 16, ((uint32_t)r32 >> 16) & 0xFFFF); lo = (uint32_t)r32 & 0xFFFF;
            } else {
                if (m.reg == 4) { uint64_t u = (uint64_t)c->r[X86_EAX] * v; lo = (uint32_t)u; hi = (uint32_t)(u >> 32); ovf = hi != 0; }
                else { int64_t r = (int64_t)(int32_t)c->r[X86_EAX] * (int32_t)v; lo = (uint32_t)r; hi = (uint32_t)((uint64_t)r >> 32); ovf = r != (int32_t)r; }
                c->r[X86_EAX] = lo; c->r[X86_EDX] = hi;
            }
            lf_set(c, LF_LOGIC, s, lo, 0, 0); x86_get_flags(c);
            set_arith(c, X86_CF | X86_OF, ovf ? (X86_CF | X86_OF) : 0);
            return; }
        default: {
            uint32_t v = rm_get(c, &m, s);
            if (!v) FAULT(c, X86_FAULT_DE, start, "divide by zero");
            if (s == 8) {
                uint32_t ax = c->r[X86_EAX] & 0xFFFF;
                if (m.reg == 6) { uint32_t q = ax / v; if (q > 0xFF) FAULT(c, X86_FAULT_DE, start, "divide overflow"); reg_set(c, X86_EAX, 16, ((ax % v) << 8) | q); }
                else { int32_t n = (int16_t)ax, d = (int8_t)v, q = n / d; if (q > 127 || q < -128) FAULT(c, X86_FAULT_DE, start, "divide overflow");
                    reg_set(c, X86_EAX, 16, (((uint32_t)(n % d) & 0xFF) << 8) | ((uint32_t)q & 0xFF)); }
            } else if (s == 16) {
                uint32_t n = ((c->r[X86_EDX] & 0xFFFF) << 16) | (c->r[X86_EAX] & 0xFFFF);
                if (m.reg == 6) { uint32_t q = n / v; if (q > 0xFFFF) FAULT(c, X86_FAULT_DE, start, "divide overflow"); reg_set(c, X86_EAX, 16, q); reg_set(c, X86_EDX, 16, n % v); }
                else { int32_t sn = (int32_t)n, d = (int16_t)v, q; if (sn == INT32_MIN && d == -1) FAULT(c, X86_FAULT_DE, start, "divide overflow");
                    q = sn / d; if (q > 32767 || q < -32768) FAULT(c, X86_FAULT_DE, start, "divide overflow");
                    reg_set(c, X86_EAX, 16, (uint32_t)q); reg_set(c, X86_EDX, 16, (uint32_t)(sn % d)); }
            } else {
                uint64_t n = ((uint64_t)c->r[X86_EDX] << 32) | c->r[X86_EAX];
                if (m.reg == 6) { uint64_t q = n / v; if (q > 0xFFFFFFFFull) FAULT(c, X86_FAULT_DE, start, "divide overflow"); c->r[X86_EAX] = (uint32_t)q; c->r[X86_EDX] = (uint32_t)(n % v); }
                else { int64_t sn = (int64_t)n, d = (int32_t)v, q; if (sn == INT64_MIN && d == -1) FAULT(c, X86_FAULT_DE, start, "divide overflow");
                    q = sn / d; if (q > INT32_MAX || q < INT32_MIN) FAULT(c, X86_FAULT_DE, start, "divide overflow");
                    c->r[X86_EAX] = (uint32_t)q; c->r[X86_EDX] = (uint32_t)(sn % d); }
            }
            return; }
        }
    }
    case 0xF8: x86_get_flags(c); c->eflags &= ~X86_CF; return;
    case 0xF9: x86_get_flags(c); c->eflags |= X86_CF; return;
    case 0xFA: case 0xFB: return;                 /* CLI/STI: ignored in user mode emulation */
    case 0xFC: c->eflags &= ~X86_DF; return;
    case 0xFD: c->eflags |= X86_DF; return;
    case 0xFE: decode_modrm(c, &p, &m);
        if (m.reg > 1) bad(c, "FE /r");
        rm_set(c, &m, 8, incdec(c, rm_get(c, &m, 8), 8, m.reg == 1)); return;
    case 0xFF: decode_modrm(c, &p, &m);
        switch (m.reg) {
        case 0: case 1: rm_set(c, &m, sz, incdec(c, rm_get(c, &m, sz), sz, m.reg == 1)); return;
        case 2: { uint32_t t = rm_get(c, &m, sz); push(c, c->eip, sz); c->eip = t; return; }
        case 4: c->eip = rm_get(c, &m, sz); return;
        case 6: push(c, rm_get(c, &m, sz), sz); return;
        default: bad(c, "far call/jmp");
        }
    case 0x0F: break;
    default: bad(c, "unsupported one-byte opcode");
    }

    /* ------------------------------------------------------------- 0F map */
    op = fetch8(c);
#ifdef QRX_EXEC_PROFILE
    c->prof_ops[256u + op]++;c->prof_active_op=256u+op;
#endif
    if (op >= 0x80 && op <= 0x8F) { uint32_t d = fetchimm(c, sz); if (cond(c, op & 15)) { c->eip += d; if (sz == 16) c->eip &= 0xFFFF; } return; }
    if (op >= 0x90 && op <= 0x9F) { decode_modrm(c, &p, &m); rm_set(c, &m, 8, (uint32_t)cond(c, op & 15)); return; }
    if (op >= 0x40 && op <= 0x4F) { uint32_t v; decode_modrm(c, &p, &m); v = rm_get(c, &m, sz); if (cond(c, op & 15)) reg_set(c, m.reg, sz, v); else if (sz == 32) reg_set(c, m.reg, 32, c->r[m.reg]); return; }
    if (op >= 0xC8 && op <= 0xCF) { uint32_t v = c->r[op & 7]; c->r[op & 7] = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); return; }
    switch (op) {
    case 0x0B: FAULT(c, X86_FAULT_UD, start, "UD2");
    case 0x0D: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: decode_modrm(c, &p, &m); return;  /* prefetch / hint NOP */
    case 0x31: { uint64_t t = x86_icount(c) * 4; c->r[X86_EAX] = (uint32_t)t; c->r[X86_EDX] = (uint32_t)(t >> 32); return; }
    case 0xA2: {
        uint32_t leaf = c->r[X86_EAX];
        if (leaf == 0) { c->r[X86_EAX] = 1; c->r[X86_EBX] = 0x756E6547u; c->r[X86_EDX] = 0x49656E69u; c->r[X86_ECX] = 0x6C65746Eu; }
        else if (leaf == 1) { c->r[X86_EAX] = 0x00000633u; c->r[X86_EBX] = 0; c->r[X86_ECX] = 0; c->r[X86_EDX] = 0x00008111u; } /* FPU TSC CX8 CMOV */
        else { c->r[X86_EAX] = c->r[X86_EBX] = c->r[X86_ECX] = c->r[X86_EDX] = 0; }
        return; }
    case 0xA0: push(c, c->sreg[X86_FS], sz); return;
    case 0xA1: c->sreg[X86_FS] = (uint16_t)pop(c, sz); return;
    case 0xA8: push(c, c->sreg[X86_GS], sz); return;
    case 0xA9: c->sreg[X86_GS] = (uint16_t)pop(c, sz); return;
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: case 0xBA: {
        int kind; uint32_t bit, v, a;
        decode_modrm(c, &p, &m);
        if (op == 0xBA) { kind = m.reg - 4; if (kind < 0) bad(c, "0F BA /r"); bit = fetch8(c) & (uint32_t)(sz - 1); a = m.addr; }
        else {
            int32_t off = (int32_t)reg_get(c, m.reg, sz);
            if (sz == 16) off = (int16_t)off;
            kind = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
            if (m.isreg) { bit = (uint32_t)off & (uint32_t)(sz - 1); a = 0; }
            else { a = m.addr + (uint32_t)((off >> (sz == 16 ? 4 : 5)) * (sz / 8)); bit = (uint32_t)off & (uint32_t)(sz - 1); }
        }
        v = m.isreg ? reg_get(c, m.rm, sz) : mem_get(c, a, sz);
        set_arith(c, X86_CF, ((v >> bit) & 1u) ? X86_CF : 0);
        if (kind) {
            if (kind == 1) v |= 1u << bit; else if (kind == 2) v &= ~(1u << bit); else v ^= 1u << bit;
            if (m.isreg) reg_set(c, m.rm, sz, v); else mem_set(c, a, sz, v);
        }
        return; }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: {
        unsigned n; uint32_t dst, src;
        decode_modrm(c, &p, &m);
        dst = rm_get(c, &m, sz); src = reg_get(c, m.reg, sz);
        n = (op == 0xA4 || op == 0xAC) ? fetch8(c) : (c->r[X86_ECX] & 0xFF);
        rm_set(c, &m, sz, shld(c, dst, src, n, sz, op >= 0xAC));
        return; }
    case 0xAF: {
        int64_t a, b, r; int ovf;
        decode_modrm(c, &p, &m);
        a = sz == 16 ? (int16_t)reg_get(c, m.reg, 16) : (int32_t)c->r[m.reg];
        b = sz == 16 ? (int16_t)rm_get(c, &m, 16) : (int32_t)rm_get(c, &m, 32);
        r = a * b; ovf = sz == 16 ? r != (int16_t)r : r != (int32_t)r;
        reg_set(c, m.reg, sz, (uint32_t)r);
        lf_set(c, LF_LOGIC, sz, (uint32_t)r, 0, 0); x86_get_flags(c);
        set_arith(c, X86_CF | X86_OF, ovf ? (X86_CF | X86_OF) : 0);
        return; }
    case 0xB0: case 0xB1: {
        int s = op & 1 ? sz : 8; uint32_t v, acc;
        decode_modrm(c, &p, &m);
        v = rm_get(c, &m, s); acc = reg_get(c, X86_EAX, s);
        alu(c, 7, acc, v, s);
        if (acc == v) rm_set(c, &m, s, reg_get(c, m.reg, s));
        else { reg_set(c, X86_EAX, s, v); if (!m.isreg) rm_set(c, &m, s, v); }
        return; }
    case 0xB6: case 0xB7: { int s = op & 1 ? 16 : 8; decode_modrm(c, &p, &m); reg_set(c, m.reg, sz, rm_get(c, &m, s)); return; }
    case 0xBE: case 0xBF: { int s = op & 1 ? 16 : 8; uint32_t v; decode_modrm(c, &p, &m); v = rm_get(c, &m, s);
        v = s == 8 ? (uint32_t)(int32_t)(int8_t)v : (uint32_t)(int32_t)(int16_t)v; reg_set(c, m.reg, sz, v); return; }
    case 0xBC: case 0xBD: {
        uint32_t v; int i;
        decode_modrm(c, &p, &m); v = rm_get(c, &m, sz);
        if (!v) { set_arith(c, X86_ZF, X86_ZF); return; }
        if (op == 0xBC) { for (i = 0; !((v >> i) & 1u); i++) ; }
        else { for (i = sz - 1; !((v >> i) & 1u); i--) ; }
        reg_set(c, m.reg, sz, (uint32_t)i);
        set_arith(c, X86_ZF, 0);
        return; }
    case 0xC0: case 0xC1: {
        int s = op & 1 ? sz : 8; uint32_t d, sv, r;
        decode_modrm(c, &p, &m);
        d = rm_get(c, &m, s); sv = reg_get(c, m.reg, s);
        r = alu(c, 0, d, sv, s);
        reg_set(c, m.reg, s, d);
        rm_set(c, &m, s, r);
        return; }
    case 0xC7: {
        uint32_t lo, hi;
        decode_modrm(c, &p, &m);
        if (m.reg != 1 || m.isreg) bad(c, "0F C7");
        lo = m_r32(c, m.addr); hi = m_r32(c, m.addr + 4);
        if (lo == c->r[X86_EAX] && hi == c->r[X86_EDX]) { m_w32(c, m.addr, c->r[X86_EBX]); m_w32(c, m.addr + 4, c->r[X86_ECX]); set_arith(c, X86_ZF, X86_ZF); }
        else { c->r[X86_EAX] = lo; c->r[X86_EDX] = hi; set_arith(c, X86_ZF, 0); }
        return; }
    }
    bad(c, "unsupported 0F opcode");
}



X86_INLINE void exec_one(x86cpu *c)
{ exec_one_mode(c,0,0,NULL); }

/* bring icount up to date, then reload left with the instructions until the budget is
 * reached, capped to 32 bits (refreshed when it runs out) */
static uint32_t budget_refill(x86cpu *c)
{
    uint64_t n;
    c->icount = x86_icount(c);
    if (!c->budget) n = 0xFFFFFFFFu;
    else if (c->icount >= c->budget) n = 0;
    else { n = c->budget - c->icount; if (n > 0xFFFFFFFFu) n = 0xFFFFFFFFu; }
    c->left = c->lbase = (uint32_t)n;
    return c->left;
}

/* single = 1: exactly one instruction, no budget accounting (x86_step).
 * Preserve the established interpreter path independently of JIT tuning.  The loop tests only eip: faults longjmp out, and the
 * hook call sites in exec_one longjmp out when a hook set fault or stop. */
static int run_loop(x86cpu *c, uint32_t stop_eip, int single)
{
    jmp_buf jb, *old = c->jb;
    if (c->fault || c->stop) return c->fault;
    c->jb = &jb;
    if (setjmp(jb)) { c->jb = old; return c->fault; }
    if (single) {
        exec_one(c);
        c->icount++;
        if (c->left) { c->left--; c->lbase--; }
    } else {
        budget_refill(c);                     /* nested runs share and decrement the same counter */
        while (c->eip != stop_eip) {
            if (!c->left && !budget_refill(c))
                FAULT(c, X86_FAULT_BUDGET, c->eip, "instruction budget exhausted");
            c->left--;                        /* counts the instruction: before exec_one, never wraps */
            exec_one(c);
        }
    }
    c->jb = old;
    return c->fault;
}


#if XJ_NATIVE
/* Separate hot loops: interpreter-only mode keeps the original decoder loop
 * without per-instruction cache checks, counters or profiling branches. */
/* Global -Os regressed the AROS interpreter. Restrict size optimization to
 * this dispatch loop; normal interpretation keeps the established -O2 build. */
#if QRX_JIT_SIZE_LOOP && defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("Os")))
#endif
static int run_loop_jit(x86cpu *c, uint32_t stop_eip)
{
    jmp_buf jb, *old = c->jb;
    if (c->fault || c->stop) return c->fault;
    c->jb = &jb;
    if (setjmp(jb)) { c->jb = old; return c->fault; }
    budget_refill(c);
    if(!c->jit->stats.timing) {
        while(c->eip!=stop_eip) {
            if(!c->left && !budget_refill(c))
                FAULT(c,X86_FAULT_BUDGET,c->eip,"instruction budget exhausted");
            #if QRX_JIT_FUSED_FETCH
            c->left--;exec_one_mode(c,stop_eip,1,NULL);
#else
            if(c->jit && xj_candidate(c) && xj_try(c,stop_eip,c->eip,0))continue;
            c->left--;exec_one(c);
#endif
        }
        c->jb=old;return c->fault;
    }
    while (c->eip != stop_eip) {
        clock_t t = 0;
        if (!c->left && !budget_refill(c))
            FAULT(c, X86_FAULT_BUDGET, c->eip, "instruction budget exhausted");
        #if !QRX_JIT_FUSED_FETCH
        if (c->jit && xj_candidate(c) && xj_try(c, stop_eip,c->eip,0)) continue;
#endif
        c->left--;
        if (c->jit) {
            if (c->jit->stats.timing) t = clock();
        }
#if QRX_JIT_FUSED_FETCH
        int native_done=0;
        exec_one_mode(c,stop_eip,1,&native_done);
#else
        exec_one(c);
#endif
        /* execve may destroy the old CPU/JIT inside a hook. */
        if (c->jit && c->jit->stats.timing
#if QRX_JIT_FUSED_FETCH
            && !native_done
#endif
            )
            c->jit->stats.interpreter_ticks += xj_elapsed(&c->jit->stats,t);
    }
    c->jb = old;
    return c->fault;
}
#endif


int x86_step(x86cpu *c) { c->fault = X86_OK; c->stop = 0; return run_loop(c, c->eip + 1u, 1); }
int x86_run(x86cpu *c, uint32_t stop_eip)
{
#if XJ_NATIVE
    if (c->jit) return run_loop_jit(c, stop_eip);
#endif
    return run_loop(c, stop_eip, 0);
}

uint32_t x86_call(x86cpu *c, uint32_t addr, int nargs, const uint32_t *args, int *ok)
{
    uint32_t saved_eip = c->eip, saved_cur = c->cur_eip, saved_sp = c->r[X86_ESP];
    int i, f;
    for (i = nargs - 1; i >= 0; i--) x86_push(c, args[i]);
    x86_push(c, X86_CALL_SENTINEL);
    c->eip = addr;
    c->depth++;
    f = x86_run(c, X86_CALL_SENTINEL);
    c->depth--;
    c->r[X86_ESP] = saved_sp;
    if (ok) *ok = (f == X86_OK && !c->stop);
    c->eip = saved_eip;
    c->cur_eip = saved_cur;
    return c->r[X86_EAX];
}

void x86_fpu_push(x86cpu *c, double v) { fpush(c, v); }
double x86_fpu_pop(x86cpu *c) { double v = ST(0); fpop(c); return v; }

void x86_describe_fault(x86cpu *c, char *buf, size_t n)
{
    uint8_t b[8];
    int i, k = 0;
    char hex[40];
    for (i = 0; i < 8; i++) b[i] = x86_rd8(c, c->fault_eip + (uint32_t)i);
    for (i = 0; i < 8; i++) k += snprintf(hex + k, sizeof(hex) - (size_t)k, "%02x", b[i]);
    snprintf(buf, n, "%s at eip=%08lx addr=%08lx bytes=%s (%s) eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
             x86_fault_name(c->fault), (unsigned long)c->fault_eip, (unsigned long)c->fault_addr, hex, c->fault_msg,
             (unsigned long)c->r[0], (unsigned long)c->r[3], (unsigned long)c->r[1], (unsigned long)c->r[2],
             (unsigned long)c->r[6], (unsigned long)c->r[7], (unsigned long)c->r[5], (unsigned long)c->r[4]);
}
