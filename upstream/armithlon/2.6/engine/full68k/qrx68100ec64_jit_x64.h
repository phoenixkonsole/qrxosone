/* 68100EC64 JIT on x86-64 hosts: the AArch64 code of qrx68100ec64_jit.c, lifted.
 *
 * The translator in qrx68100ec64_jit.c stays as it is: it writes AArch64 code for a block into a
 * buffer that is never run here. This file turns that code, one AArch64 instruction after the
 * other, into x86-64 code that does the same. Only the instructions the translator emits are known;
 * any other makes the lift fail, and the block then runs in the interpreter, as an instruction the
 * translator does not know does.
 *
 * AArch64 registers on the host: x0 rdi, x1 rsi, x2 r10, x3 r11, x4 rbp, x16 r8, x17 r9, x19 rbx,
 * x20 r12, x21 r13, x22 r14, x23 r15; the others in the frame at [rsp + 8 * n]. rax, rcx and rdx are
 * the lifter's own. NZCV lives in the frame slot 32 in AArch64's layout (bits 31..28), written only
 * where a later instruction reads it (a backward liveness pass over the block); a flag reader right
 * after the instruction that set the flags, and the only one, takes the x86 flags directly.
 * Mapping of conditions, with x86 CF = not C (the AArch64 carry of a subtraction is "no borrow"):
 * EQ je, NE jne, HS jae, LO jb, MI js, PL jns, VS jo, VC jno, HI ja, LS jbe, GE jge, LT jl, GT jg,
 * LE jle. Flags from an x86 ADD have CF = C, from a logical operation CF = 0 (C = 0).
 *
 * Every AArch64 B becomes a 5-byte jmp rel32, so the dispatcher can chain a block exit to another
 * block the way it patches the AArch64 branch (xmap: AArch64 word -> x86 offset). */
#include <string.h>

enum { HRAX = 0, HRCX, HRDX, HRBX, HRSP, HRBP, HRSI, HRDI, HR8, HR9, HR10, HR11, HR12, HR13, HR14, HR15 };
static const signed char x64_host[32] = {
    HRDI, HRSI, HR10, HR11, HRBP, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    HR8, HR9, -1, HRBX, HR12, HR13, HR14, HR15, -1, -1, -1, -1, -1, -1, -1, -1 };
#define X64_NZCV_SLOT 32
#define X64_FRAME (35u * 8u)       /* 32 registers, NZCV and a pad: with the return address and the six
                                     * pushes rsp is then 16-aligned for calls */

typedef struct {
    uint8_t *p, *end;
    bool bad;
} X64;
/* The jump cache: pc -> lifted block body. The ret of a block's exit to a computed pc (RTS, JMP (An),
 * the translator's "pc written" exit, ret_index) looks the next block up there and jumps straight
 * into it when the budget allows and no gate is set, as a chained exit does; else it returns to the
 * dispatcher, which fills the cache as it enters blocks. */
typedef struct { uint64_t pc; uint8_t *x; } X64JumpCache;
typedef struct {
    long ret_index;          /* the AArch64 word of that ret, or -1 */
    X64JumpCache *jc;        /* NULL: no lookup */
    unsigned bits, off_pc, off_gate;
} X64Lookup;

static void xb(X64 *x, unsigned v) { if (x->p < x->end) *x->p++ = (uint8_t)v; else x->bad = true; }
static void x32(X64 *x, uint32_t v) { for (int i = 0; i < 4; i++) xb(x, (v >> (8 * i)) & 0xFF); }
static void x64v(X64 *x, uint64_t v) { x32(x, (uint32_t)v); x32(x, (uint32_t)(v >> 32)); }
static void rex(X64 *x, bool w, unsigned r, unsigned i, unsigned b, bool force)
{
    unsigned v = 0x40 | (w ? 8 : 0) | ((r >> 3) & 1) << 2 | ((i >> 3) & 1) << 1 | ((b >> 3) & 1);
    if (v != 0x40 || force) xb(x, v);
}
/* reg, r/m register */
static void op_rr(X64 *x, bool w, unsigned opc, unsigned reg, unsigned rm)
{
    rex(x, w, reg, 0, rm, false);
    if (opc > 0xFF) xb(x, opc >> 8);
    xb(x, opc & 0xFF);
    xb(x, 0xC0 | (reg & 7) << 3 | (rm & 7));
}
/* reg, [base + index + disp32] (index < 0: none) */
static void op_rm(X64 *x, bool w, unsigned opc, unsigned reg, unsigned base, int index, int32_t disp, bool bytereg, int pfx)
{
    if (pfx) xb(x, (unsigned)pfx);
    rex(x, w, reg, index < 0 ? 0 : (unsigned)index, base, bytereg && (reg & 7) >= 4);
    if (opc > 0xFF) xb(x, opc >> 8);
    xb(x, opc & 0xFF);
    if (index < 0 && (base & 7) != 4) { xb(x, 0x80 | (reg & 7) << 3 | (base & 7)); x32(x, (uint32_t)disp); return; }
    xb(x, 0x80 | (reg & 7) << 3 | 4);
    xb(x, (index < 0 ? 4u : ((unsigned)index & 7)) << 3 | (base & 7));
    x32(x, (uint32_t)disp);
}
static void mov_rr64(X64 *x, unsigned d, unsigned s) { if (d != s) op_rr(x, true, 0x89, s, d); }
static void mov_ri64(X64 *x, unsigned d, uint64_t v)
{
    if (v <= 0xFFFFFFFFu) { rex(x, false, 0, 0, d, false); xb(x, 0xB8 | (d & 7)); x32(x, (uint32_t)v); }   /* zero extends */
    else if ((int64_t)v >= INT32_MIN && (int64_t)v <= INT32_MAX) { rex(x, true, 0, 0, d, false); xb(x, 0xC7); xb(x, 0xC0 | (d & 7)); x32(x, (uint32_t)v); }
    else { rex(x, true, 0, 0, d, false); xb(x, 0xB8 | (d & 7)); x64v(x, v); }
}
static void frame_ld(X64 *x, unsigned d, unsigned slot) { op_rm(x, true, 0x8B, d, HRSP, -1, (int32_t)(slot * 8), false, 0); }
static void frame_st(X64 *x, unsigned slot, unsigned s) { op_rm(x, true, 0x89, s, HRSP, -1, (int32_t)(slot * 8), false, 0); }
static void sh_i(X64 *x, unsigned ext, unsigned r, unsigned n, bool w)   /* ext 4 shl, 5 shr, 7 sar */
{
    if (!n) return;
    rex(x, w, 0, 0, r, false); xb(x, 0xC1); xb(x, 0xC0 | ext << 3 | (r & 7)); xb(x, n);
}
static void alu_rr(X64 *x, bool w, unsigned opc, unsigned d, unsigned s) { op_rr(x, w, opc, s, d); }   /* opc: 01 add 29 sub 21 and 09 or 31 xor 39 cmp 85 test */
static void alu_ri(X64 *x, bool w, unsigned ext, unsigned d, int32_t imm)   /* 81 /ext */
{
    rex(x, w, 0, 0, d, false); xb(x, 0x81); xb(x, 0xC0 | ext << 3 | (d & 7)); x32(x, (uint32_t)imm);
}
static uint8_t *jcc32(X64 *x, unsigned cc) { xb(x, 0x0F); xb(x, 0x80 | cc); uint8_t *s = x->p; x32(x, 0); return s; }
static uint8_t *jmp32(X64 *x) { xb(x, 0xE9); uint8_t *s = x->p; x32(x, 0); return s; }
static void setcc(X64 *x, unsigned cc, unsigned r8) { rex(x, false, 0, 0, r8, r8 >= 4); xb(x, 0x0F); xb(x, 0x90 | cc); xb(x, 0xC0 | (r8 & 7)); }

/* AArch64 register -> host register holding its value (scratch for the frame ones and XZR) */
static unsigned src(X64 *x, unsigned r, unsigned scratch)
{
    if (r == 31) { mov_ri64(x, scratch, 0); return scratch; }
    if (x64_host[r] >= 0) return (unsigned)x64_host[r];
    frame_ld(x, scratch, r);
    return scratch;
}
static void wb(X64 *x, unsigned r, unsigned h)
{
    if (r == 31) return;
    if (x64_host[r] >= 0) mov_rr64(x, (unsigned)x64_host[r], h);
    else frame_st(x, r, h);
}
/* x86 condition code for an AArch64 one with x86 CF = not C; -1: always, -2: never */
static int x64_cc_natural(unsigned c)
{
    static const signed char m[16] = { 4, 5, 3, 2, 8, 9, 0, 1, 7, 6, 13, 12, 15, 14, -1, -1 };
    return m[c & 15];
}
/* kinds of x86 flags left by the instruction before: 0 none (use the slot), 1 subtraction, 2 addition, 3 logical */
enum { XF_SLOT = 0, XF_SUB, XF_ADD, XF_LOGIC };
/* the x86 condition for condition c with flags of kind k; -3 when it cannot be taken from those flags */
static int x64_cc(unsigned c, int k)
{
    c &= 15;
    if (c >= 14) return -1;
    if (k == XF_SUB || k == XF_SLOT) return x64_cc_natural(c);
    if (k == XF_ADD) {
        if (c == 2) return 2;             /* HS: C set, x86 CF set */
        if (c == 3) return 3;             /* LO */
        if (c == 8 || c == 9) return -3;  /* HI, LS need not C */
        return x64_cc_natural(c);
    }
    /* logical: C = V = 0 */
    if (c == 2 || c == 8) return -2;      /* HS, HI never */
    if (c == 3 || c == 9) return -1;      /* LO, LS always */
    if (c == 6) return -2;                /* VS */
    if (c == 7) return -1;                /* VC */
    return x64_cc_natural(c);
}
/* The x86 flags of kind k as AArch64 NZCV into host register d (rax, rcx, rdx used) */
static void nzcv_from_flags(X64 *x, int k, unsigned d)
{
    setcc(x, 8, HRAX);                           /* sets al: N */
    xb(x, 0x0F); xb(x, 0x94); xb(x, 0xC4);       /* sete ah: Z */
    if (k == XF_LOGIC) { mov_ri64(x, HRCX, 0); }
    else {
        setcc(x, k == XF_SUB ? 3 : 2, HRCX);     /* setae / setb cl: C */
        xb(x, 0x0F); xb(x, 0x90); xb(x, 0xC5);   /* seto ch: V */
    }
    /* d = al << 31 | ah << 30 | cl << 29 | ch << 28 */
    xb(x, 0x0F); xb(x, 0xB6); xb(x, 0xD0);       /* movzx edx, al */
    sh_i(x, 4, HRDX, 31, false);
    xb(x, 0x0F); xb(x, 0xB6); xb(x, 0xC4);       /* movzx eax, ah */
    sh_i(x, 4, HRAX, 30, false); alu_rr(x, false, 0x09, HRDX, HRAX);
    xb(x, 0x0F); xb(x, 0xB6); xb(x, 0xC1);       /* movzx eax, cl */
    sh_i(x, 4, HRAX, 29, false); alu_rr(x, false, 0x09, HRDX, HRAX);
    xb(x, 0x0F); xb(x, 0xB6); xb(x, 0xC5);       /* movzx eax, ch */
    sh_i(x, 4, HRAX, 28, false); alu_rr(x, false, 0x09, HRDX, HRAX);
    mov_rr64(x, d, HRDX);
}
/* x86 flags from the NZCV slot, mapped the natural way (CF = not C) */
static void flags_from_slot(X64 *x)
{
    frame_ld(x, HRAX, X64_NZCV_SLOT);
    sh_i(x, 5, HRAX, 24, false);                 /* N Z C V at 7 6 5 4 */
    mov_rr64(x, HRDX, HRAX);
    alu_ri(x, false, 4, HRDX, 0xC0);             /* N Z */
    mov_rr64(x, HRCX, HRAX);
    sh_i(x, 5, HRCX, 5, false); alu_ri(x, false, 4, HRCX, 1); alu_ri(x, false, 6, HRCX, 1);   /* not C */
    alu_rr(x, false, 0x09, HRDX, HRCX);
    sh_i(x, 5, HRAX, 4, false); alu_ri(x, false, 4, HRAX, 1);   /* V in al */
    xb(x, 0x88); xb(x, 0xD4);                    /* mov ah, dl */
    xb(x, 0x04); xb(x, 0x7F);                    /* add al, 0x7f: OF = V */
    xb(x, 0x9E);                                 /* sahf */
}

/* AArch64 bitmask immediates (DecodeBitMasks, 64-bit) */
static bool x64_bitmask(unsigned n, unsigned imms, unsigned immr, uint64_t *out)
{
    unsigned len = 0;
    unsigned v = (n << 6) | (~imms & 0x3F);
    for (int i = 6; i >= 0; i--) if (v & (1u << i)) { len = (unsigned)i; break; }
    if (len < 1) return false;
    unsigned esize = 1u << len, levels = esize - 1, s = imms & levels, r = immr & levels;
    if (s == levels) return false;
    uint64_t welem = (s + 1 == 64) ? UINT64_MAX : ((UINT64_C(1) << (s + 1)) - 1);
    uint64_t emask = esize == 64 ? UINT64_MAX : ((UINT64_C(1) << esize) - 1);
    uint64_t e = r ? (((welem >> r) | (welem << (esize - r))) & emask) : welem;
    uint64_t m = 0;
    for (unsigned i = 0; i < 64; i += esize) m |= e << i;
    *out = m;
    return true;
}

/* What an instruction does with NZCV: 1 reads, 2 writes, 4 kills (a call) */
static unsigned x64_nzcv_use(uint32_t w)
{
    if ((w & 0xFF000010u) == 0x54000000u) return (w & 15) >= 14 ? 0 : 1;             /* B.cond */
    if ((w & 0x7FE00C00u) == 0x1A800000u || (w & 0x7FE00C00u) == 0x1A800400u)      /* CSEL, CSINC */
        return ((w >> 12) & 15) >= 14 ? 0 : 1;
    if ((w & 0xFFFFFFE0u) == 0xD53B4200u) return 1;                                /* MRS NZCV */
    if ((w & 0xFFFFFFE0u) == 0xD51B4200u) return 2;                                /* MSR NZCV */
    if ((w & 0x7F200000u) == 0x2B000000u || (w & 0x7F200000u) == 0x6B000000u) return 2;   /* ADDS, SUBS reg */
    if ((w & 0x7F200000u) == 0x6A000000u) return 2;                                /* ANDS reg */
    if ((w & 0x7F800000u) == 0x31000000u || (w & 0x7F800000u) == 0x71000000u) return 2;   /* ADDS, SUBS imm */
    if ((w & 0x7F800000u) == 0x72000000u) return 2;                                /* ANDS imm */
    if ((w & 0xFFFFFC1Fu) == 0xD63F0000u) return 4;                                /* BLR */
    return 0;
}
/* branch target index inside the block for word i, or -1 */
static long x64_target(uint32_t w, long i)
{
    if ((w & 0xFC000000u) == 0x14000000u) return i + (long)((int32_t)(w << 6) >> 6);
    if ((w & 0xFF000010u) == 0x54000000u || (w & 0x7E000000u) == 0x34000000u)
        return i + (long)((int32_t)(w << 8) >> 13);
    return -1;
}

/* After an instruction that set the x86 flags of kind k: when the NZCV it made is read later, either
 * the next instruction alone reads it and can take the x86 flags (returns k for it), or NZCV goes to
 * its slot now (returns XF_SLOT). */
static int x64_after_flags(X64 *x, const uint32_t *a, unsigned n, unsigned i, const uint8_t *live, const uint8_t *target, int k)
{
    if (!live[i]) return XF_SLOT;
    if (i + 1 < n && !target[i + 1] && !live[i + 1]) {
        uint32_t nw = a[i + 1];
        if (x64_nzcv_use(nw) & 1) {
            if ((nw & 0xFFFFFFE0u) == 0xD53B4200u) return k;
            unsigned c = (nw & 0xFF000010u) == 0x54000000u ? (nw & 15) : (nw >> 12) & 15;
            if (x64_cc(c, k) != -3) return k;
        }
    }
    nzcv_from_flags(x, k, HRDX);
    frame_st(x, X64_NZCV_SLOT, HRDX);
    return XF_SLOT;
}

/* Lifts the AArch64 words [a, a + n) into x86 code at *xp (up to xend). xmap gets the x86 offset
 * (from xbase) of each word. Returns false for anything not known; *xp is then unchanged. */
static bool x64_lift(const uint32_t *a, unsigned n, uint8_t **xp, uint8_t *xend, uint8_t *xbase, uint32_t *xmap, const X64Lookup *lk)
{
    typedef struct { uint8_t *site; long to; } Fix;
    static __thread uint8_t *tls_live, *tls_target;
    static __thread unsigned tls_cap;
    static __thread Fix *tls_fix;
    if (n > tls_cap) {
        free(tls_live); free(tls_target); free(tls_fix);
        tls_cap = n + 4096;
        tls_live = malloc(tls_cap); tls_target = malloc(tls_cap); tls_fix = malloc(tls_cap * sizeof *tls_fix);
        if (!tls_live || !tls_target || !tls_fix) { tls_cap = 0; return false; }
    }
    /* plain locals from here: thread-local accesses in the loops cost a call each in PIC code */
    uint8_t *live = tls_live, *target = tls_target;
    Fix *fix = tls_fix;
    memset(target, 0, n);
    for (unsigned i = 0; i < n; i++) {
        long to = x64_target(a[i], i);
        if (to >= 0 && to < (long)n) target[to] = 1;
        else if (to >= 0) return false;
    }
    /* NZCV live after each instruction (live[i]: live out); dead at the block's exits */
    memset(live, 0, n);
    for (int pass = 0, changed = 1; changed && pass < 8; pass++) {
        changed = 0;
        for (long i = (long)n - 1; i >= 0; i--) {
            uint32_t w = a[i];
            bool uncond = (w & 0xFC000000u) == 0x14000000u || w == 0xD65F03C0u;
            unsigned out = 0;
            if (!uncond && i + 1 < (long)n) {
                unsigned u = x64_nzcv_use(a[i + 1]);
                out |= (u & 1) || (live[i + 1] && !(u & 6));
            }
            long to = x64_target(w, i);
            if (to >= 0) { unsigned u = x64_nzcv_use(a[to]); out |= (u & 1) || (live[to] && !(u & 6)); }
            if (out != live[i]) { live[i] = (uint8_t)out; changed = 1; }
        }
    }
    X64 xx = { *xp, xend, false }, *x = &xx;
    unsigned nfix = 0;
    int kind_for_next = XF_SLOT;     /* the x86 flags the next instruction may read */
    for (unsigned i = 0; i < n && !x->bad; i++) {
        uint32_t w = a[i];
        int kind = kind_for_next;
        kind_for_next = XF_SLOT;
        xmap[i] = (uint32_t)(x->p - xbase);
        if (target[i]) kind = XF_SLOT;
        unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
        bool sf = w >> 31;
        /* ---- prologue, epilogue */
        if (w == 0xA9BB7BFDu) {                               /* stp x29, x30, [sp, #-80]! */
            static const unsigned pushes[6] = { HRBX, HRBP, HR12, HR13, HR14, HR15 };
            for (int k = 0; k < 6; k++) { rex(x, false, 0, 0, pushes[k], false); xb(x, 0x50 | (pushes[k] & 7)); }
            alu_ri(x, true, 5, HRSP, (int32_t)X64_FRAME);
            continue;
        }
        if (w == 0xA90153F3u || w == 0xA9025BF5u || w == 0xA90363F7u || w == 0xA9046BF9u ||
            w == 0xA9446BF9u || w == 0xA94363F7u || w == 0xA9425BF5u || w == 0xA94153F3u || w == 0xA8C57BFDu)
            continue;                                         /* the other saves and restores */
        if (w == 0xD65F03C0u) {                               /* ret: x0 is the result */
            uint8_t *miss[4];
            unsigned nmiss = 0;
            if (lk && lk->jc && (long)i == lk->ret_index) {
                mov_rr64(x, HR15, HRDI);                          /* RDONE = what the block has done */
                frame_ld(x, HRAX, 25);                            /* RLIMIT */
                alu_rr(x, true, 0x39, HR15, HRAX);                /* cmp r15, rax */
                miss[nmiss++] = jcc32(x, 3);                      /* jae: budget used up */
                op_rm(x, true, 0x8B, HRAX, HRBX, -1, (int32_t)lk->off_gate, false, 0);
                alu_rr(x, true, 0x85, HRAX, HRAX);
                miss[nmiss++] = jcc32(x, 5);                      /* jnz: the global gate */
                op_rm(x, true, 0x8B, HRAX, HRBX, -1, (int32_t)lk->off_pc, false, 0);   /* the next pc */
                mov_rr64(x, HRCX, HRAX);
                sh_i(x, 5, HRCX, 1, true);
                alu_ri(x, false, 4, HRCX, (int32_t)((1u << lk->bits) - 1));
                sh_i(x, 4, HRCX, 4, true);                        /* 16 bytes an entry */
                mov_ri64(x, HRDX, (uint64_t)(uintptr_t)lk->jc);
                alu_rr(x, true, 0x01, HRDX, HRCX);
                op_rm(x, true, 0x3B, HRAX, HRDX, -1, 0, false, 0);   /* cmp rax, [rdx] */
                miss[nmiss++] = jcc32(x, 5);
                op_rm(x, false, 0xFF, 4, HRDX, -1, 8, false, 0);    /* jmp [rdx + 8] */
                for (unsigned k = 0; k < nmiss; k++) { int32_t rel = (int32_t)(x->p - (miss[k] + 4)); memcpy(miss[k], &rel, 4); }
            }
            mov_rr64(x, HRAX, HRDI);
            alu_ri(x, true, 0, HRSP, (int32_t)X64_FRAME);
            static const unsigned pops[6] = { HR15, HR14, HR13, HR12, HRBP, HRBX };
            for (int k = 0; k < 6; k++) { rex(x, false, 0, 0, pops[k], false); xb(x, 0x58 | (pops[k] & 7)); }
            xb(x, 0xC3);
            continue;
        }
        /* ---- moves of immediates */
        if ((w & 0xFF800000u) == 0xD2800000u || (w & 0xFF800000u) == 0x92800000u) {   /* MOVZ, MOVN */
            unsigned hw = (w >> 21) & 3;
            uint64_t v = (uint64_t)((w >> 5) & 0xFFFF) << (16 * hw);
            if ((w & 0xFF800000u) == 0x92800000u) v = ~v;
            unsigned h = rd != 31 && x64_host[rd] >= 0 ? (unsigned)x64_host[rd] : HRAX;
            mov_ri64(x, h, v); if (h == HRAX) wb(x, rd, HRAX);
            continue;
        }
        if ((w & 0xFF800000u) == 0xF2800000u) {               /* MOVK */
            unsigned hw = (w >> 21) & 3;
            uint64_t v = (uint64_t)((w >> 5) & 0xFFFF) << (16 * hw);
            mov_rr64(x, HRAX, src(x, rd, HRAX));
            mov_ri64(x, HRCX, ~(UINT64_C(0xFFFF) << (16 * hw)));
            alu_rr(x, true, 0x21, HRAX, HRCX);
            mov_ri64(x, HRCX, v);
            alu_rr(x, true, 0x09, HRAX, HRCX);
            wb(x, rd, HRAX);
            continue;
        }
        /* ---- loads and stores */
        if ((w & 0x3FC00000u) == 0x39400000u || (w & 0x3FC00000u) == 0x39000000u) {   /* LDR/STR unsigned offset */
            unsigned size = w >> 30, s = 1u << size;
            int32_t off = (int32_t)(((w >> 10) & 0xFFF) * s);
            bool isload = (w >> 22) & 1;
            if (rn == 31) return false;
            unsigned base = src(x, rn, HRCX);
            if (isload) {
                static const unsigned opc[4] = { 0x0FB6, 0x0FB7, 0x8B, 0x8B };
                op_rm(x, size == 3, opc[size], HRAX, base, -1, off, false, 0);
                wb(x, rd, HRAX);
            } else {
                unsigned v = src(x, rd, HRAX);
                op_rm(x, size == 3, size == 0 ? 0x88 : 0x89, v, base, -1, off, size == 0, size == 1 ? 0x66 : 0);
            }
            continue;
        }
        if ((w & 0x3FE0FC00u) == 0x38606800u || (w & 0x3FE0FC00u) == 0x38206800u) {   /* LDR/STR register offset */
            unsigned size = w >> 30;
            bool isload = (w >> 22) & 1;
            if (rn == 31) return false;
            unsigned base = src(x, rn, HRCX), idx = src(x, rm, HRDX);
            if (idx == HRSP) return false;
            if (isload) {
                static const unsigned opc[4] = { 0x0FB6, 0x0FB7, 0x8B, 0x8B };
                op_rm(x, size == 3, opc[size], HRAX, base, (int)idx, 0, false, 0);
                wb(x, rd, HRAX);
            } else {
                unsigned v = src(x, rd, HRAX);
                op_rm(x, size == 3, size == 0 ? 0x88 : 0x89, v, base, (int)idx, 0, size == 0, size == 1 ? 0x66 : 0);
            }
            continue;
        }
        if ((w & 0x3FFFFC00u) == 0x08DFFC00u) {               /* LDAR: a plain load on x86 */
            unsigned size = w >> 30;
            unsigned base = src(x, rn, HRCX);
            static const unsigned opc[4] = { 0x0FB6, 0x0FB7, 0x8B, 0x8B };
            op_rm(x, size == 3, opc[size], HRAX, base, -1, 0, false, 0);
            wb(x, rd, HRAX);
            continue;
        }
        if ((w & 0x3FFFFC00u) == 0x089FFC00u) {               /* STLR: xchg, so later loads wait for it as after STLR+LDAR */
            unsigned size = w >> 30;
            unsigned base = src(x, rn, HRCX);
            mov_rr64(x, HRAX, src(x, rd, HRAX));
            op_rm(x, size == 3, size == 0 ? 0x86 : 0x87, HRAX, base, -1, 0, size == 0, size == 1 ? 0x66 : 0);
            continue;
        }
        /* ---- arithmetic and logic, register forms */
        if ((w & 0x7F200000u) == 0x0B000000u || (w & 0x7F200000u) == 0x4B000000u ||
            (w & 0x7F200000u) == 0x2B000000u || (w & 0x7F200000u) == 0x6B000000u) {   /* ADD, SUB, ADDS, SUBS (shifted reg) */
            unsigned sh = (w >> 22) & 3, amt = (w >> 10) & 63;
            bool sub = (w >> 30) & 1, setf = (w >> 29) & 1;
            if (sh != 0 || (!sf && amt > 31)) return false;
            mov_rr64(x, HRAX, src(x, rn, HRAX));
            unsigned b = src(x, rm, HRCX);
            if (amt) { mov_rr64(x, HRCX, b); sh_i(x, 4, HRCX, amt, sf); b = HRCX; }
            alu_rr(x, sf, sub ? 0x29 : 0x01, HRAX, b);
            wb(x, rd, HRAX);
            if (setf) kind_for_next = x64_after_flags(x, a, n, i, live, target, sub ? XF_SUB : XF_ADD);
            continue;
        }
        if ((w & 0x7F000000u) == 0x0A000000u || (w & 0x7F000000u) == 0x2A000000u ||
            (w & 0x7F000000u) == 0x4A000000u || (w & 0x7F000000u) == 0x6A000000u) {   /* AND, ORR, EOR, ANDS (+ BIC, ORN) */
            unsigned opc = (w >> 29) & 3, sh = (w >> 22) & 3, amt = (w >> 10) & 63;
            bool neg = (w >> 21) & 1;
            if (sh != 0 || amt != 0) return false;
            if (neg && opc == 2) return false;               /* EON: not emitted */
            mov_rr64(x, HRAX, src(x, rn, HRAX));
            unsigned b = src(x, rm, HRCX);
            if (neg) { mov_rr64(x, HRCX, b); op_rr(x, sf, 0xF7, 2, HRCX); b = HRCX; }   /* not */
            static const unsigned ops[4] = { 0x21, 0x09, 0x31, 0x21 };
            alu_rr(x, sf, ops[opc], HRAX, b);
            wb(x, rd, HRAX);
            if (opc == 3) kind_for_next = x64_after_flags(x, a, n, i, live, target, XF_LOGIC);
            continue;
        }
        /* ---- immediates */
        if ((w & 0x7F800000u) == 0x11000000u || (w & 0x7F800000u) == 0x51000000u ||
            (w & 0x7F800000u) == 0x31000000u || (w & 0x7F800000u) == 0x71000000u) {   /* ADD, SUB, ADDS, SUBS imm */
            bool sub = (w >> 30) & 1, setf = (w >> 29) & 1;
            uint32_t imm = ((w >> 10) & 0xFFF) << (((w >> 22) & 1) ? 12 : 0);
            if (rn == 31 || (!setf && rd == 31)) return false;    /* SP: not emitted */
            mov_rr64(x, HRAX, src(x, rn, HRAX));
            alu_ri(x, sf, sub ? 5 : 0, HRAX, (int32_t)imm);
            wb(x, rd, HRAX);
            if (setf) kind_for_next = x64_after_flags(x, a, n, i, live, target, sub ? XF_SUB : XF_ADD);
            continue;
        }
        if ((w & 0x1F800000u) == 0x12000000u) {              /* AND, ORR, EOR, ANDS imm */
            unsigned opc = (w >> 29) & 3, nbit = (w >> 22) & 1, immr = (w >> 16) & 63, imms = (w >> 10) & 63;
            uint64_t m;
            if (!sf || !x64_bitmask(nbit, imms, immr, &m)) return false;
            if (rd == 31 && opc != 3) return false;          /* SP */
            mov_rr64(x, HRAX, src(x, rn, HRAX));
            mov_ri64(x, HRCX, m);
            static const unsigned ops[4] = { 0x21, 0x09, 0x31, 0x21 };
            alu_rr(x, true, ops[opc], HRAX, HRCX);
            wb(x, rd, HRAX);
            if (opc == 3) kind_for_next = x64_after_flags(x, a, n, i, live, target, XF_LOGIC);
            continue;
        }
        /* ---- bit fields (64-bit) */
        if ((w & 0xFFC00000u) == 0xD3400000u || (w & 0xFFC00000u) == 0x93400000u || (w & 0xFFC00000u) == 0xB3400000u) {
            unsigned immr = (w >> 16) & 63, imms = (w >> 10) & 63, opc = (w >> 29) & 3;
            mov_rr64(x, HRAX, src(x, rn, HRAX));
            if (opc == 1) {                                   /* BFM */
                unsigned lo, hi, lsb;
                if (imms >= immr) { lo = immr; hi = imms; lsb = 0; } else { lo = 0; hi = imms; lsb = 64 - immr; }
                unsigned width = hi - lo + 1;
                sh_i(x, 4, HRAX, 63 - hi, true);
                sh_i(x, 5, HRAX, 63 - hi + lo, true);
                sh_i(x, 4, HRAX, lsb, true);
                uint64_t fm = (width == 64 ? UINT64_MAX : ((UINT64_C(1) << width) - 1)) << lsb;
                mov_rr64(x, HRCX, src(x, rd, HRCX));
                mov_ri64(x, HRDX, ~fm);
                alu_rr(x, true, 0x21, HRCX, HRDX);
                alu_rr(x, true, 0x09, HRAX, HRCX);
            } else {
                unsigned right = opc == 0 ? 7 : 5;           /* SBFM: sar, UBFM: shr */
                if (imms >= immr) {
                    sh_i(x, 4, HRAX, 63 - imms, true);
                    sh_i(x, right, HRAX, 63 - imms + immr, true);
                } else {
                    sh_i(x, 4, HRAX, 63 - imms, true);
                    sh_i(x, right, HRAX, 63 - imms, true);
                    sh_i(x, 4, HRAX, 64 - immr, true);
                }
            }
            wb(x, rd, HRAX);
            continue;
        }
        /* ---- NZCV */
        if ((w & 0xFFFFFFE0u) == 0xD53B4200u) {               /* MRS rd, NZCV */
            if (kind != XF_SLOT) { nzcv_from_flags(x, kind, HRDX); wb(x, rd, HRDX); }
            else { frame_ld(x, HRAX, X64_NZCV_SLOT); wb(x, rd, HRAX); }
            continue;
        }
        if ((w & 0xFFFFFFE0u) == 0xD51B4200u) {               /* MSR NZCV, rd */
            unsigned v = src(x, rd, HRAX);
            frame_st(x, X64_NZCV_SLOT, v);
            continue;
        }
        /* ---- conditional select */
        if ((w & 0xFFE00C00u) == 0x9A800000u || (w & 0xFFE00C00u) == 0x9A800400u) {   /* CSEL, CSINC (64-bit) */
            unsigned cond = (w >> 12) & 15;
            bool inc = (w >> 10) & 1;
            int cc = x64_cc(cond, kind);
            if (cc == -3) return false;
            if (kind == XF_SLOT && cc >= 0) flags_from_slot(x);   /* first: it uses rax, rcx, rdx */
            /* the values with mov and lea only, so the x86 flags stay */
            unsigned vn = src(x, rn, HRAX); mov_rr64(x, HRAX, vn);
            unsigned vm = src(x, rm, HRCX); mov_rr64(x, HRCX, vm);
            if (inc) op_rm(x, true, 0x8D, HRCX, HRCX, -1, 1, false, 0);   /* lea rcx, [rcx + 1] */
            if (cc == -2) mov_rr64(x, HRAX, HRCX);              /* never: the second value */
            else if (cc >= 0) op_rr(x, true, 0x0F40 | (unsigned)(cc ^ 1), HRAX, HRCX);   /* cmov rax, rcx when not cond */
            wb(x, rd, HRAX);
            continue;
        }
        /* ---- multiplication, variable shift, byte reversal */
        if ((w & 0xFFE0FC00u) == 0x9B007C00u) {               /* MUL */
            mov_rr64(x, HRAX, src(x, rn, HRAX));
            unsigned b = src(x, rm, HRCX);
            op_rr(x, true, 0x0FAF, HRAX, b);
            wb(x, rd, HRAX);
            continue;
        }
        if ((w & 0xFFE0FC00u) == 0x9B407C00u || (w & 0xFFE0FC00u) == 0x9BC07C00u) {   /* SMULH, UMULH */
            bool sg = (w & 0xFFE0FC00u) == 0x9B407C00u;
            unsigned b = src(x, rm, HRCX); mov_rr64(x, HRCX, b);
            mov_rr64(x, HRAX, src(x, rn, HRAX));
            op_rr(x, true, 0xF7, sg ? 5 : 4, HRCX);        /* imul/mul rcx: rdx:rax */
            wb(x, rd, HRDX);
            continue;
        }
        if ((w & 0xFFE0FC00u) == 0x9B207C00u || (w & 0xFFE0FC00u) == 0x9BA07C00u) {   /* SMULL, UMULL */
            bool sg = (w & 0xFFE0FC00u) == 0x9B207C00u;
            unsigned b = src(x, rm, HRCX);
            if (sg) op_rr(x, true, 0x63, HRCX, b); else op_rr(x, false, 0x8B, HRCX, b);   /* movsxd / mov r32 */
            unsigned c = src(x, rn, HRAX);
            if (sg) op_rr(x, true, 0x63, HRAX, c); else op_rr(x, false, 0x8B, HRAX, c);
            op_rr(x, true, 0x0FAF, HRAX, HRCX);
            wb(x, rd, HRAX);
            continue;
        }
        if ((w & 0xFFE0FC00u) == 0x9AC02000u) {               /* LSLV */
            unsigned b = src(x, rm, HRCX); mov_rr64(x, HRCX, b);
            mov_rr64(x, HRAX, src(x, rn, HRAX));
            rex(x, true, 0, 0, HRAX, false); xb(x, 0xD3); xb(x, 0xE0);   /* shl rax, cl */
            wb(x, rd, HRAX);
            continue;
        }
        if (w >> 10 == 0xDAC00C00u >> 10) {                   /* REV x */
            mov_rr64(x, HRAX, src(x, rn, HRAX));
            xb(x, 0x48); xb(x, 0x0F); xb(x, 0xC8);           /* bswap rax */
            wb(x, rd, HRAX);
            continue;
        }
        if (w >> 10 == 0x5AC00800u >> 10) {                   /* REV w */
            mov_rr64(x, HRAX, src(x, rn, HRAX));
            xb(x, 0x0F); xb(x, 0xC8);                        /* bswap eax (clears the upper half) */
            wb(x, rd, HRAX);
            continue;
        }
        if (w >> 10 == 0x5AC00400u >> 10) {                   /* REV16 w */
            mov_rr64(x, HRAX, src(x, rn, HRAX));
            op_rr(x, false, 0x8B, HRAX, HRAX);               /* mov eax, eax */
            mov_rr64(x, HRCX, HRAX);
            sh_i(x, 4, HRAX, 8, false); alu_ri(x, false, 4, HRAX, (int32_t)0xFF00FF00u);
            sh_i(x, 5, HRCX, 8, false); alu_ri(x, false, 4, HRCX, 0x00FF00FF);
            alu_rr(x, false, 0x09, HRAX, HRCX);
            wb(x, rd, HRAX);
            continue;
        }
        /* ---- calls and branches */
        if ((w & 0xFFFFFC1Fu) == 0xD63F0000u) {               /* BLR: x0..x3 to rdi, rsi, rdx, rcx; x0, x1 from rax, rdx */
            unsigned f = src(x, rn, HRAX);
            if (f != HRAX) mov_rr64(x, HRAX, f);
            mov_rr64(x, HRDX, HR10);
            mov_rr64(x, HRCX, HR11);
            xb(x, 0xFF); xb(x, 0xD0);                        /* call rax */
            mov_rr64(x, HRDI, HRAX);
            mov_rr64(x, HRSI, HRDX);
            continue;
        }
        if ((w & 0xFC000000u) == 0x14000000u) {               /* B: always a jmp rel32 (the chain site) */
            uint8_t *s = jmp32(x);
            fix[nfix].site = s; fix[nfix].to = x64_target(w, i); nfix++;
            continue;
        }
        if ((w & 0xFF000010u) == 0x54000000u) {               /* B.cond */
            int cc = x64_cc(w & 15, kind);
            if (cc == -3) return false;
            if (kind == XF_SLOT && cc >= 0) flags_from_slot(x);
            if (cc == -2) continue;
            uint8_t *s = cc == -1 ? jmp32(x) : jcc32(x, (unsigned)cc);
            fix[nfix].site = s; fix[nfix].to = x64_target(w, i); nfix++;
            continue;
        }
        if ((w & 0x7E000000u) == 0x34000000u) {               /* CBZ, CBNZ */
            bool nz = (w >> 24) & 1;
            unsigned v = src(x, rd, HRAX);
            alu_rr(x, sf, 0x85, v, v);                        /* test */
            uint8_t *s = jcc32(x, nz ? 5 : 4);
            fix[nfix].site = s; fix[nfix].to = x64_target(w, i); nfix++;
            continue;
        }
        {   /* not known: say which, once per word, with QRX_EC64_JIT_STATS */
            static uint32_t said[64]; static unsigned nsaid;
            if (getenv("QRX_EC64_JIT_STATS")) {
                unsigned k = 0;
                while (k < nsaid && said[k] != w) k++;
                if (k == nsaid && nsaid < 64) { said[nsaid++] = w; fprintf(stderr, "[EC64 JIT] x86-64 lifter: AArch64 word %08x not known\n", w); }
            }
        }
        return false;
    }
    if (x->bad) return false;
    for (unsigned k = 0; k < nfix; k++) {
        uint8_t *to = xbase + xmap[fix[k].to];
        int32_t rel = (int32_t)(to - (fix[k].site + 4));
        memcpy(fix[k].site, &rel, 4);
    }
    *xp = x->p;
    return true;
}
