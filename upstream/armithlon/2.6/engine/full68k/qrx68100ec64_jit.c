/* 68100EC64 long mode -> AArch64 block JIT. See qec64_jit_run in qrx68100ec64.h.
 *
 * The reference interpreter (qrx68100ec64.c) stays the authority: a block covers the
 * instructions this file knows, and anything else, including every exception, interrupt and
 * privileged instruction, runs there one instruction at a time. A translated instruction first
 * computes its addresses and checks them (inside RAM, aligned, not into translated code), and
 * only then changes state. A failed check leaves the block before the instruction, so the
 * interpreter runs it from the start with its own rules (bus faults, MMIO, unaligned bytes,
 * stale code).
 *
 * Host registers in a block: x19 the core, x20 RAM, x21 RAM size, x22 the code map,
 * x0-x15 temporaries of one instruction, x16/x17 scratch of memory accesses.
 * Guest registers stay in qec64_cpu between instructions. Memory accesses are LDAR/STLR, as the
 * interpreter's are acquire and release.
 *
 * Translations die when RAM they came from is written (code_map, code_gen): the writer clears
 * the map bytes and bumps the generation, and each core drops all its blocks when it sees a
 * new generation. Stores in translated code leave the block when they hit marked RAM.
 */
#include "qrx68100ec64.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

bool qec64_jit_map_new(uint64_t ram_size, uint8_t **code_map, uint32_t **code_gen)
{
    *code_map = calloc((size_t)(ram_size >> QEC64_CODE_SHIFT) + 2, 1);
    *code_gen = calloc(1, 64);
    if (!*code_map || !*code_gen) { free(*code_map); free(*code_gen); *code_map = NULL; *code_gen = NULL; return false; }
    return true;
}

#if defined(__aarch64__)
#include <sys/mman.h>
#include "qrx_perfmap.h"
#include <pthread.h>
#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif

#define CODE_BYTES (32u * 1024u * 1024u)
#define HASH_BITS 16u
#define MAX_INSNS 48u
/* For tests: fewer instructions per block (1 steps instruction by instruction). */
unsigned qec64_jit_block_limit = MAX_INSNS;
#define MAX_STUBS 160u

#define XF 0x10
#define NF 0x08
#define ZF 0x04
#define VF 0x02
#define CF 0x01
#define PFX_W 8
#define PFX_R 4
#define PFX_X 2
#define PFX_B 1
#define PFX_N 0x10

typedef struct Block {
    uint64_t pc;
    uint32_t *body;          /* after the prologue: where chained blocks jump in */
    unsigned count;          /* instructions, when all of them run */
    uint32_t *code;          /* NULL: the first instruction is not translated */
    struct Block *next;
} Block;

typedef struct {
    uint32_t *buf, *pos, *end;
    Block *hash[1u << HASH_BITS];
    Block *blocks;           /* bump allocated, as many as fit */
    unsigned nblocks, maxblocks;
    uint32_t gen;
    uint64_t translated, interpreted, entered, flushes, chained;
    uint32_t *last_exit;     /* a block's exit to a fixed pc that returned here, to patch */
    uint64_t last_exit_pc;   /* that pc: only a block there may be chained in */
    uint32_t *single;        /* with QRX_EC64_JIT_STATS: interpreted instructions by prefix and opcode */
} Jit;

/* ------------------------------------------------------------------ AArch64 encoding */
/* x23: instructions done in this call, x25: chain while x23 is below it */
enum { RCPU = 19, RRAM = 20, RSIZE = 21, RMAP = 22, RDONE = 23, RFLAGS = 24, RLIMIT = 25, S0 = 16, S1 = 17, ZR = 31, SP_ = 31 };
/* Flags not yet in the CCR: host NZCV of the last flag setting instruction is in RFLAGS.
 * X is always written at once, so the CCR in memory has the right X. */
enum { PEND_NONE, PEND_ADD, PEND_SUB, PEND_LOGIC };
enum { C_EQ = 0, C_NE, C_HS, C_LO, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE, C_AL };
#define OFF_D(r) ((unsigned)offsetof(qec64_cpu, d) + (r) * 8u)
#define OFF_A(r) ((unsigned)offsetof(qec64_cpu, a) + (r) * 8u)
#define OFF_PC ((unsigned)offsetof(qec64_cpu, pc))
#define OFF_CCR ((unsigned)offsetof(qec64_cpu, ccr))

typedef struct {
    uint32_t *at;            /* the branch to patch */
    uint64_t pc;             /* the instruction to leave before */
    unsigned done;           /* instructions completed before it */
    int pend;                /* flags pending at that point */
} Stub;

typedef struct {
    qec64_cpu *c;
    Jit *j;
    uint32_t *p;
    uint64_t pc, start;      /* decode position, start of the instruction */
    unsigned op, pfx;
    unsigned ntmp;
    int an[16];              /* host register with the new value of An in this instruction, or -1 */
    int dn[16];              /* host register with the new value of Dn (already merged), or -1 */
    int ccr;                 /* host register with the new CCR, or -1 */
    int pend, pend_start;    /* pending flags now, and when this instruction began */
    int x_cond;              /* a branch at the end: condition, taken target, fall through */
    uint64_t x_taken, x_next;
    unsigned done;
    Stub stub[MAX_STUBS];
    unsigned nstub;
    bool bad;
} T;

static void emit(T *t, uint32_t w) { if (t->p < t->j->end) *t->p++ = w; else t->bad = true; }
static unsigned tmp(T *t) { if (t->ntmp >= 16) { t->bad = true; return 15; } return t->ntmp++; }

static void mov_imm(T *t, unsigned rd, uint64_t v)
{
    if ((int64_t)v < 0 && (int64_t)v >= -65536) { emit(t, 0x92800000u | ((uint32_t)(~v & 0xFFFF) << 5) | rd); return; }
    emit(t, 0xD2800000u | ((uint32_t)(v & 0xFFFF) << 5) | rd);
    for (unsigned h = 1; h < 4; h++) {
        uint32_t part = (uint32_t)(v >> (16 * h)) & 0xFFFF;
        if (part) emit(t, 0xF2800000u | (h << 21) | (part << 5) | rd);
    }
}
static void ldr_x(T *t, unsigned rt, unsigned rn, unsigned off) { emit(t, 0xF9400000u | ((off / 8) << 10) | (rn << 5) | rt); }
static void str_x(T *t, unsigned rt, unsigned rn, unsigned off) { emit(t, 0xF9000000u | ((off / 8) << 10) | (rn << 5) | rt); }
static void ldrb_(T *t, unsigned rt, unsigned rn, unsigned off) { emit(t, 0x39400000u | (off << 10) | (rn << 5) | rt); }
static void strb_(T *t, unsigned rt, unsigned rn, unsigned off) { emit(t, 0x39000000u | (off << 10) | (rn << 5) | rt); }
static void ldrb_reg(T *t, unsigned rt, unsigned rn, unsigned rm) { emit(t, 0x38606800u | (rm << 16) | (rn << 5) | rt); }
static void add_r(T *t, unsigned rd, unsigned rn, unsigned rm) { emit(t, 0x8B000000u | (rm << 16) | (rn << 5) | rd); }
static void sub_r(T *t, unsigned rd, unsigned rn, unsigned rm) { emit(t, 0xCB000000u | (rm << 16) | (rn << 5) | rd); }
static void and_r(T *t, unsigned rd, unsigned rn, unsigned rm) { emit(t, 0x8A000000u | (rm << 16) | (rn << 5) | rd); }
static void orr_r(T *t, unsigned rd, unsigned rn, unsigned rm) { emit(t, 0xAA000000u | (rm << 16) | (rn << 5) | rd); }
static void eor_r(T *t, unsigned rd, unsigned rn, unsigned rm) { emit(t, 0xCA000000u | (rm << 16) | (rn << 5) | rd); }
static void mov_r(T *t, unsigned rd, unsigned rm) { orr_r(t, rd, ZR, rm); }
static void mvn_r(T *t, unsigned rd, unsigned rm) { emit(t, 0xAA200000u | (rm << 16) | (ZR << 5) | rd); }
static void add_i(T *t, unsigned rd, unsigned rn, unsigned imm) { emit(t, 0x91000000u | (imm << 10) | (rn << 5) | rd); }
static void sub_i(T *t, unsigned rd, unsigned rn, unsigned imm) { emit(t, 0xD1000000u | (imm << 10) | (rn << 5) | rd); }
/* x: 64 bit; w: 32 bit */
static void adds_r(T *t, bool x, unsigned rd, unsigned rn, unsigned rm) { emit(t, (x ? 0xAB000000u : 0x2B000000u) | (rm << 16) | (rn << 5) | rd); }
static void subs_r(T *t, bool x, unsigned rd, unsigned rn, unsigned rm) { emit(t, (x ? 0xEB000000u : 0x6B000000u) | (rm << 16) | (rn << 5) | rd); }
static void ands_r(T *t, bool x, unsigned rd, unsigned rn, unsigned rm) { emit(t, (x ? 0xEA000000u : 0x6A000000u) | (rm << 16) | (rn << 5) | rd); }
static void ubfm(T *t, unsigned rd, unsigned rn, unsigned immr, unsigned imms) { emit(t, 0xD3400000u | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
static void sbfm(T *t, unsigned rd, unsigned rn, unsigned immr, unsigned imms) { emit(t, 0x93400000u | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
static void bfm(T *t, unsigned rd, unsigned rn, unsigned immr, unsigned imms) { emit(t, 0xB3400000u | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
static void lsl_i(T *t, unsigned rd, unsigned rn, unsigned n) { if (n) ubfm(t, rd, rn, (64 - n) & 63, 63 - n); else mov_r(t, rd, rn); }
static void lsr_i(T *t, unsigned rd, unsigned rn, unsigned n) { ubfm(t, rd, rn, n, 63); }
static void asr_i(T *t, unsigned rd, unsigned rn, unsigned n) { sbfm(t, rd, rn, n, 63); }
static void ubfx(T *t, unsigned rd, unsigned rn, unsigned lsb, unsigned w) { ubfm(t, rd, rn, lsb, lsb + w - 1); }
static void bfi(T *t, unsigned rd, unsigned rn, unsigned lsb, unsigned w) { bfm(t, rd, rn, (64 - lsb) & 63, w - 1); }
/* zero or sign extend the low s bytes */
static void zext(T *t, unsigned rd, unsigned rn, unsigned s) { if (s == 8) { if (rd != rn) mov_r(t, rd, rn); } else ubfm(t, rd, rn, 0, s * 8 - 1); }
static void sext(T *t, unsigned rd, unsigned rn, unsigned s) { if (s == 8) { if (rd != rn) mov_r(t, rd, rn); } else sbfm(t, rd, rn, 0, s * 8 - 1); }
static void mrs_nzcv(T *t, unsigned rt) { emit(t, 0xD53B4200u | rt); }
static void msr_nzcv(T *t, unsigned rt) { emit(t, 0xD51B4200u | rt); }
static void cset(T *t, unsigned rd, unsigned cond) { emit(t, 0x9A9F07E0u | ((cond ^ 1) << 12) | rd); }
static void csel(T *t, unsigned rd, unsigned rn, unsigned rm, unsigned cond) { emit(t, 0x9A800000u | (rm << 16) | (cond << 12) | (rn << 5) | rd); }
static void cmp_r(T *t, unsigned rn, unsigned rm) { subs_r(t, true, ZR, rn, rm); }
static void cmp_i(T *t, unsigned rn, unsigned imm) { emit(t, 0xF1000000u | (imm << 10) | (rn << 5) | ZR); }
static void tst_i_low(T *t, unsigned rn, unsigned bits) /* tst rn, #(1<<bits)-1 */
{
    emit(t, 0xF2400000u | ((bits - 1) << 10) | (rn << 5) | ZR);
}
static uint32_t *bcond_site(T *t, unsigned cond) { uint32_t *s = t->p; emit(t, 0x54000000u | cond); return s; }
static uint32_t *b_site(T *t) { uint32_t *s = t->p; emit(t, 0x14000000u); return s; }
static uint32_t *cbnz_site(T *t, unsigned rt) { uint32_t *s = t->p; emit(t, 0x35000000u | rt); return s; }
static void patch(uint32_t *site, uint32_t *to)
{
    int64_t d = to - site;
    if ((*site & 0xFC000000u) == 0x14000000u) *site |= (uint32_t)(d & 0x3FFFFFF);
    else *site |= (uint32_t)((d & 0x7FFFF) << 5);
}
static void rev(T *t, unsigned rd, unsigned rn, unsigned s)
{
    if (s == 2) emit(t, 0x5AC00400u | (rn << 5) | rd);        /* rev16 w */
    else if (s == 4) emit(t, 0x5AC00800u | (rn << 5) | rd);   /* rev w */
    else if (s == 8) emit(t, 0xDAC00C00u | (rn << 5) | rd);   /* rev x */
    else if (rd != rn) mov_r(t, rd, rn);
}

/* ------------------------------------------------------------------ leaving the block */
static void leave_before(T *t, uint32_t *site)
{
    if (t->nstub >= MAX_STUBS) { t->bad = true; return; }
    t->stub[t->nstub++] = (Stub){ site, t->start, t->done, t->pend_start };
}
/* The address in ra must be inside RAM for s bytes, aligned, and for a write not in code. */
static void check(T *t, unsigned ra, unsigned s, bool write)
{
    sub_i(t, S0, RSIZE, s);
    cmp_r(t, ra, S0);
    leave_before(t, bcond_site(t, C_HI));
    if (s > 1) { tst_i_low(t, ra, s == 2 ? 1 : s == 4 ? 2 : 3); leave_before(t, bcond_site(t, C_NE)); }
    if (write) {
        lsr_i(t, S0, ra, QEC64_CODE_SHIFT);
        ldrb_reg(t, S0, RMAP, S0);
        leave_before(t, cbnz_site(t, S0));
    }
}
/* A range [ra, ra + n) of s-byte accesses, n <= 128, ra aligned to s */
static void check_range(T *t, unsigned ra, unsigned n, unsigned s, bool write)
{
    mov_imm(t, S0, n);
    sub_r(t, S0, RSIZE, S0);
    cmp_r(t, ra, S0);
    leave_before(t, bcond_site(t, C_HI));
    if (s > 1) { tst_i_low(t, ra, s == 2 ? 1 : s == 4 ? 2 : 3); leave_before(t, bcond_site(t, C_NE)); }
    if (write) {
        lsr_i(t, S0, ra, QEC64_CODE_SHIFT);
        ldrb_reg(t, S0, RMAP, S0);
        leave_before(t, cbnz_site(t, S0));
        add_i(t, S0, ra, n - 1);
        lsr_i(t, S0, S0, QEC64_CODE_SHIFT);
        ldrb_reg(t, S0, RMAP, S0);
        leave_before(t, cbnz_site(t, S0));
    }
}
/* Ordered accesses (LDAR/STLR) are only needed while other cores share the RAM: with one core,
 * as in the 32-bit JIT, plain LDR/STR with a register offset do, which on in-order-retiring
 * cores like the Pi 4's Cortex-A72 cost far less. The devices on other host threads synchronise
 * through the board's own locks and atomics, not through these accesses.
 * QRX_EC64_JIT_ORDERED=1 keeps the ordered form on one core too. */
static bool ordered(const T *t)
{
    static int forced = -1;
    if (forced < 0) forced = getenv("QRX_EC64_JIT_ORDERED") != NULL;
    return forced || t->c->num_cores > 1;
}
static void load(T *t, unsigned rd, unsigned ra, unsigned s)
{
    if (!ordered(t)) {
        static const uint32_t ld[9] = { 0, 0x38606800u, 0x78606800u, 0, 0xB8606800u, 0, 0, 0, 0xF8606800u };
        emit(t, ld[s] | (ra << 16) | (RRAM << 5) | rd);          /* ldr rd, [x20, ra] */
        rev(t, rd, rd, s);
        return;
    }
    add_r(t, S1, RRAM, ra);
    static const uint32_t ld[9] = { 0, 0x08DFFC00u, 0x48DFFC00u, 0, 0x88DFFC00u, 0, 0, 0, 0xC8DFFC00u };
    emit(t, ld[s] | (S1 << 5) | rd);
    rev(t, rd, rd, s);
}
static void store(T *t, unsigned rv, unsigned ra, unsigned s)
{
    rev(t, S0, rv, s);
    if (!ordered(t)) {
        static const uint32_t st[9] = { 0, 0x38206800u, 0x78206800u, 0, 0xB8206800u, 0, 0, 0, 0xF8206800u };
        emit(t, st[s] | (ra << 16) | (RRAM << 5) | S0);          /* str s0, [x20, ra] */
        return;
    }
    add_r(t, S1, RRAM, ra);
    static const uint32_t st[9] = { 0, 0x089FFC00u, 0x489FFC00u, 0, 0x889FFC00u, 0, 0, 0, 0xC89FFC00u };
    emit(t, st[s] | (S1 << 5) | S0);
}

/* ------------------------------------------------------------------ decoding */
static bool code16(T *t, uint16_t *v)
{
    qec64_cpu *c = t->c;
    if (t->pc >= c->ram_size || c->ram_size - t->pc < 2 || (t->pc & 1)) return false;
    *v = (uint16_t)(c->ram[t->pc] << 8 | c->ram[t->pc + 1]);
    t->pc += 2;
    return true;
}
static uint16_t f16(T *t) { uint16_t v = 0; if (!code16(t, &v)) t->bad = true; return v; }
static uint32_t f32(T *t) { uint32_t h = f16(t); return h << 16 | f16(t); }
static uint64_t f64(T *t) { uint64_t h = f32(t); return h << 32 | f32(t); }

static unsigned regR(T *t, unsigned r) { return r | ((t->pfx & PFX_R) ? 8 : 0); }
static unsigned regB(T *t, unsigned r) { return r | ((t->pfx & PFX_B) ? 8 : 0); }
static unsigned wide(T *t, unsigned s) { return s == 4 && (t->pfx & PFX_W) ? 8 : s; }

/* Guest registers as this instruction sees them so far */
static unsigned get_a(T *t, unsigned r)
{
    if (t->an[r] >= 0) return (unsigned)t->an[r];
    unsigned h = tmp(t); ldr_x(t, h, RCPU, OFF_A(r)); return h;
}
static unsigned get_d(T *t, unsigned r)
{
    if (t->dn[r] >= 0) return (unsigned)t->dn[r];
    unsigned h = tmp(t); ldr_x(t, h, RCPU, OFF_D(r)); return h;
}
static void set_a(T *t, unsigned r, unsigned h) { t->an[r] = (int)h; }
/* dset: .B and .W keep the upper bits, .L clears them, .Q writes all */
static void set_d(T *t, unsigned r, unsigned s, unsigned h)
{
    unsigned n = tmp(t);
    if (s == 1 || s == 2) { unsigned old = get_d(t, r); mov_r(t, n, old); bfi(t, n, h, 0, s * 8); }
    else zext(t, n, h, s);
    t->dn[r] = (int)n;
}

enum { L_D, L_A, L_MEM, L_IMM };
typedef struct { int kind; unsigned reg; unsigned addr; uint64_t imm; } Loc;

/* index_ext() of the interpreter: the brief format, and the full one without memory indirection */
static bool index_brief(T *t, unsigned base, unsigned *out)
{
    uint16_t e = f16(t);
    unsigned r = (e >> 12) & 7;
    if (t->pfx & PFX_X) r |= 8;
    bool full = e & 0x100, no_index = full && (e & 0x40), no_base = full && (e & 0x80);
    int64_t disp = (int8_t)(e & 0xFF);
    if (full) {
        if ((e & 0xF) || !((e >> 4) & 3)) return false;
        unsigned bd = (e >> 4) & 3;
        disp = bd == 1 ? 0 : bd == 2 ? (int16_t)f16(t) : (int32_t)f32(t);
    }
    unsigned a = tmp(t);
    if (no_base) mov_imm(t, a, 0); else mov_r(t, a, base);
    if (!no_index) {
        unsigned ix = (e & 0x8000) ? get_a(t, r) : get_d(t, r), v = tmp(t);
        if (e & 0x800) mov_r(t, v, ix); else sext(t, v, ix, 2);
        unsigned sc = (e >> 9) & 3;
        emit(t, 0x8B000000u | (v << 16) | (sc << 10) | (a << 5) | a);   /* add a, a, v, lsl sc */
    }
    if (disp > 0 && disp < 4096) add_i(t, a, a, (unsigned)disp);
    else if (disp < 0 && disp > -4096) sub_i(t, a, a, (unsigned)-disp);
    else if (disp) { unsigned d = tmp(t); mov_imm(t, d, (uint64_t)disp); add_r(t, a, a, d); }
    *out = a;
    return true;
}
/* ea() of the interpreter: address registers change in this instruction's temporaries only */
static bool ea(T *t, unsigned mode, unsigned reg, unsigned s, Loc *l)
{
    unsigned r = regB(t, reg);
    unsigned step = (s == 1 && reg == 7 && !(t->pfx & PFX_B)) ? 2 : s;
    l->kind = L_MEM;
    switch (mode) {
    case 0: l->kind = L_D; l->reg = r; return true;
    case 1: l->kind = L_A; l->reg = r; return true;
    case 2: l->addr = get_a(t, r); return true;
    case 3: { unsigned a = get_a(t, r), n = tmp(t); add_i(t, n, a, step); set_a(t, r, n); l->addr = a; return true; }
    case 4: { unsigned a = get_a(t, r), n = tmp(t); sub_i(t, n, a, step); set_a(t, r, n); l->addr = n; return true; }
    case 5: {
        int64_t d = (int16_t)f16(t);
        unsigned a = get_a(t, r), n = tmp(t);
        if (d >= 0 && d < 4096) add_i(t, n, a, (unsigned)d);
        else if (d < 0 && d > -4096) sub_i(t, n, a, (unsigned)-d);
        else { mov_imm(t, n, (uint64_t)d); add_r(t, n, a, n); }
        l->addr = n; return true;
    }
    case 6: { unsigned a = get_a(t, r); return index_brief(t, a, &l->addr); }
    default:
        switch (reg) {
        case 0: { unsigned n = tmp(t); mov_imm(t, n, (uint64_t)(int64_t)(int16_t)f16(t)); l->addr = n; return true; }
        case 1: { unsigned n = tmp(t); mov_imm(t, n, (uint64_t)(int64_t)(int32_t)f32(t)); l->addr = n; return true; }
        case 2: { uint64_t b = t->pc; unsigned n = tmp(t); mov_imm(t, n, b + (uint64_t)(int64_t)(int16_t)f16(t)); l->addr = n; return true; }
        case 3: { unsigned b = tmp(t); mov_imm(t, b, t->pc); return index_brief(t, b, &l->addr); }
        case 4:
            l->kind = L_IMM;
            l->imm = s == 1 ? (f16(t) & 0xFF) : s == 2 ? f16(t) : s == 4 ? f32(t) : f64(t);
            return true;
        default: return false;
        }
    }
}
static void check_loc(T *t, const Loc *l, unsigned s, bool write) { if (l->kind == L_MEM) check(t, l->addr, s, write); }
/* value of a location, zero extended from s bytes */
static unsigned ld(T *t, const Loc *l, unsigned s)
{
    unsigned h = tmp(t);
    switch (l->kind) {
    case L_D: zext(t, h, get_d(t, l->reg), s); break;
    case L_A: zext(t, h, get_a(t, l->reg), s); break;
    case L_IMM: mov_imm(t, h, l->imm); break;
    default: load(t, h, l->addr, s); break;
    }
    return h;
}
static void st(T *t, const Loc *l, unsigned s, unsigned v)
{
    if (l->kind == L_D) set_d(t, l->reg, s, v);
    else if (l->kind == L_A) { unsigned n = tmp(t); mov_r(t, n, v); set_a(t, l->reg, n); }
    else store(t, v, l->addr, s);
}

/* ------------------------------------------------------------------ flags */
/* The pending flags into the CCR in memory; X stays as it is there. */
static void materialize(T *t, int pend)
{
    if (pend == PEND_NONE) return;
    lsr_i(t, S0, RFLAGS, 28);                    /* host N Z C V in bits 3..0 */
    ubfx(t, S1, S0, 2, 2); lsl_i(t, S1, S1, 2);  /* N Z at 3, 2 */
    bfi(t, S1, S0, 1, 1);                        /* V at 1 */
    ubfx(t, S0, S0, 1, 1);                       /* C at 0: the carry, or for SUB the borrow */
    if (pend == PEND_SUB) emit(t, 0xD2400000u | (S0 << 5) | S0);
    bfi(t, S1, S0, 0, 1);
    ldrb_(t, S0, RCPU, OFF_CCR);
    ubfx(t, S0, S0, 4, 1); bfi(t, S1, S0, 4, 1); /* X as it is */
    strb_(t, S1, RCPU, OFF_CCR);
}
static void flush_flags(T *t) { materialize(t, t->pend); t->pend = PEND_NONE; }
/* logic: N and Z of the low s bytes of v, V = C = 0, X kept */
static void flags_logic(T *t, unsigned v, unsigned s)
{
    unsigned n = tmp(t);
    lsl_i(t, n, v, 64 - s * 8);
    ands_r(t, true, ZR, n, n);
    mrs_nzcv(t, RFLAGS);
    t->pend = PEND_LOGIC;
}
/* r = a +/- b on s bytes with 68k flags; the result zero extended. keepx: CMP leaves X. */
static unsigned arith(T *t, bool sub, unsigned a, unsigned b, unsigned s, bool flags, bool keepx)
{
    unsigned sh = 64 - s * 8, ta = tmp(t), tb = tmp(t), r = tmp(t);
    lsl_i(t, ta, a, sh); lsl_i(t, tb, b, sh);
    if (sub) subs_r(t, true, r, ta, tb); else adds_r(t, true, r, ta, tb);
    if (flags) {
        mrs_nzcv(t, RFLAGS);
        t->pend = sub ? PEND_SUB : PEND_ADD;
        if (!keepx) {                            /* X = C, at once */
            unsigned c = tmp(t), x = tmp(t);
            ubfx(t, c, RFLAGS, 29, 1);
            if (sub) emit(t, 0xD2400000u | (c << 5) | c);
            ldrb_(t, x, RCPU, OFF_CCR);
            bfi(t, x, c, 4, 1);
            strb_(t, x, RCPU, OFF_CCR);
        }
    }
    lsr_i(t, r, r, sh);
    return r;
}
/* host NZCV for a 68k condition, with host C = not 68k C: every 68k condition is one host condition */
static const uint8_t cond_map[16] = { C_AL, 0xFF, C_HI, C_LS, C_HS, C_LO, C_NE, C_EQ, C_VC, C_VS, C_PL, C_MI, C_GE, C_LT, C_GT, C_LE };
static void ccr_to_nzcv(T *t)
{
    if (t->pend == PEND_SUB) { msr_nzcv(t, RFLAGS); return; }
    flush_flags(t);
    unsigned c = tmp(t), w = tmp(t), b = tmp(t);
    ldrb_(t, c, RCPU, OFF_CCR);
    ubfx(t, w, c, 2, 2); lsl_i(t, w, w, 30);        /* N Z */
    ubfx(t, b, c, 0, 1); emit(t, 0xD2400000u | (b << 5) | b); bfi(t, w, b, 29, 1);   /* not C */
    ubfx(t, b, c, 1, 1); bfi(t, w, b, 28, 1);       /* V */
    msr_nzcv(t, w);
}
/* An instruction that writes the whole CCR itself (t->ccr) needs the X in memory only. */
static void ccr_written(T *t) { t->pend = PEND_NONE; }

/* ------------------------------------------------------------------ C helpers
 * For instructions with many cases. They follow qrx68100ec64.c line by line. A call clobbers
 * x0-x17: the instruction's pending registers are stored first (nothing can fail after the
 * checks), and an address that must survive goes in x26. */
#define XF_ 0x10
typedef struct { uint64_t v, ccr; } HR;
static uint64_t mask_of(unsigned s) { return s == 8 ? UINT64_MAX : (UINT64_C(1) << (s * 8)) - 1; }
/* shift() of the interpreter. meta: size | kind << 4 | left << 8 */
static HR h_shift(uint64_t v, uint64_t n64, uint64_t meta, uint64_t ccr)
{
    unsigned s = meta & 15, kind = (meta >> 4) & 3, n = (unsigned)n64;
    bool left = meta & 0x100;
    unsigned bits = s * 8;
    uint64_t m = mask_of(s), sb = UINT64_C(1) << (bits - 1);
    bool cy = false, ov = false, xb = ccr & XF_;
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
    if (kind < 2 ? n != 0 : true) f |= (kind == 3) ? (ccr & XF_) : (xb ? XF_ : 0);
    else f |= ccr & XF_;
    if (cy) f |= 1;
    if (ov) f |= 2;
    if (!v) f |= 4;
    if (v & sb) f |= 8;
    return (HR){ v, f };
}
static void set_nz_(qec64_cpu *c, uint64_t v, unsigned s)
{
    v &= mask_of(s);
    c->ccr = (uint8_t)((c->ccr & 0x13) | (v == 0 ? 4 : 0) | ((v >> (s * 8 - 1)) & 1 ? 8 : 0));
}
static void dset_(qec64_cpu *c, unsigned r, unsigned s, uint64_t v)
{
    uint64_t *d = &c->d[r];
    if (s == 4) *d = (uint32_t)v; else *d = v;
}
/* The DIV part of op_muldiv_long; the divisor is not zero (checked before) */
static void h_div(qec64_cpu *c, uint64_t src, uint64_t e, uint64_t s)
{
    bool is_signed = e & 0x800, dual = e & 0x400;
    unsigned dl = (e >> 12) & 7, dh = e & 7;
    if (s == 8) {
        uint64_t n = c->d[dl];
        c->ccr &= XF_;
        if (is_signed) {
            if ((int64_t)n == INT64_MIN && (int64_t)src == -1) { c->ccr |= 2; return; }
            int64_t q = (int64_t)n / (int64_t)src, rem = (int64_t)n % (int64_t)src;
            if (dh != dl) c->d[dh] = (uint64_t)rem;
            c->d[dl] = (uint64_t)q;
            set_nz_(c, (uint64_t)q, 8);
        } else {
            uint64_t q = n / src, rem = n % src;
            if (dh != dl) c->d[dh] = rem;
            c->d[dl] = q;
            set_nz_(c, q, 8);
        }
        return;
    }
    if (dual) {
        uint64_t n = (c->d[dh] & 0xFFFFFFFFu) << 32 | (c->d[dl] & 0xFFFFFFFFu);
        if (is_signed) {
            if ((int64_t)n == INT64_MIN && (int32_t)src == -1) { c->ccr |= 2; return; }
            int64_t q = (int64_t)n / (int32_t)src, rem = (int64_t)n % (int32_t)src;
            if (q > INT32_MAX || q < INT32_MIN) { c->ccr |= 2; return; }
            dset_(c, dh, 4, (uint64_t)rem); dset_(c, dl, 4, (uint64_t)q);
            c->ccr &= XF_;
            set_nz_(c, (uint64_t)q, 4);
        } else {
            uint64_t d = (uint32_t)src, q = n / d, rem = n % d;
            if (q > UINT32_MAX) { c->ccr |= 2; return; }
            dset_(c, dh, 4, rem); dset_(c, dl, 4, q);
            c->ccr &= XF_;
            set_nz_(c, q, 4);
        }
        return;
    }
    uint32_t n = (uint32_t)c->d[dl], d = (uint32_t)src;
    if (is_signed) {
        if ((int32_t)n == INT32_MIN && (int32_t)d == -1) { c->ccr |= 2; return; }
        int32_t q = (int32_t)n / (int32_t)d, rem = (int32_t)n % (int32_t)d;
        if (dh != dl) dset_(c, dh, 4, (uint32_t)rem);
        dset_(c, dl, 4, (uint32_t)q);
        c->ccr &= XF_;
        set_nz_(c, (uint32_t)q, 4);
    } else {
        uint32_t q = n / d, rem = n % d;
        if (dh != dl) dset_(c, dh, 4, rem);
        dset_(c, dl, 4, q);
        c->ccr &= XF_;
        set_nz_(c, q, 4);
    }
}
static void spill_pending(T *t)
{
    for (unsigned r = 0; r < 16; r++) {
        if (t->dn[r] >= 0) { str_x(t, (unsigned)t->dn[r], RCPU, OFF_D(r)); t->dn[r] = -1; }
        if (t->an[r] >= 0) { str_x(t, (unsigned)t->an[r], RCPU, OFF_A(r)); t->an[r] = -1; }
    }
}
/* Calls fn with up to 4 arguments from temporaries; x0 and x1 have its results after. */
static void call(T *t, void *fn, const unsigned *args, unsigned n)
{
    if (t->ntmp > 11) { t->bad = true; return; }
    for (unsigned i = 0; i < n; i++) mov_r(t, 12 + i, args[i]);
    for (unsigned i = 0; i < n; i++) mov_r(t, i, 12 + i);
    mov_imm(t, S0, (uint64_t)(uintptr_t)fn);
    emit(t, 0xD63F0000u | (S0 << 5));          /* blr */
    t->ntmp = 2;                               /* x0, x1 are the results */
}
#define RKEEP 26

/* ------------------------------------------------------------------ instructions */
static void commit(T *t)
{
    for (unsigned r = 0; r < 16; r++) {
        if (t->dn[r] >= 0) str_x(t, (unsigned)t->dn[r], RCPU, OFF_D(r));
        if (t->an[r] >= 0) str_x(t, (unsigned)t->an[r], RCPU, OFF_A(r));
    }
    if (t->ccr >= 0) strb_(t, (unsigned)t->ccr, RCPU, OFF_CCR);
}
static bool alterable_ok(unsigned mode, unsigned reg) { return !(mode == 7 && reg > 3); }

static bool i_move(T *t)
{
    unsigned op = t->op, line = op >> 12, s = line == 1 ? 1 : line == 3 ? 2 : wide(t, 4);
    unsigned smode = (op >> 3) & 7, sreg = op & 7, dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
    if ((s == 1 && smode == 1) || (dmode == 1 && s == 1) || !alterable_ok(dmode, dreg)) return false;
    Loc from, to;
    if (!ea(t, smode, sreg, s, &from)) return false;
    if (dmode == 1) {
        check_loc(t, &from, s, false);
        unsigned v = ld(t, &from, s), n = tmp(t);
        sext(t, n, v, s);
        set_a(t, regR(t, dreg), n);
        return true;
    }
    /* The source is read before the destination's address changes a register. */
    unsigned v = from.kind != L_MEM ? ld(t, &from, s) : 0;
    unsigned sv = t->pfx;
    t->pfx = (sv & ~PFX_B) | ((sv & PFX_R) ? PFX_B : 0);
    bool ok = ea(t, dmode, dreg, s, &to);
    t->pfx = sv;
    if (!ok) return false;
    check_loc(t, &from, s, false);
    check_loc(t, &to, s, true);
    if (from.kind == L_MEM) v = ld(t, &from, s);
    if (!(t->pfx & PFX_N)) flags_logic(t, v, s);
    st(t, &to, s, v);
    return true;
}
static bool i_moveq(T *t)
{
    if (t->op & 0x100) return false;
    unsigned r = regR(t, (t->op >> 9) & 7), h = tmp(t);
    mov_imm(t, h, (uint64_t)(int64_t)(int8_t)t->op);
    t->dn[r] = (int)h;
    flags_logic(t, h, 4);
    return true;
}
/* returns 1 translated, 2 translated and the block ends (pc written), 0 not */
static void set_pc_imm(T *t, uint64_t pc) { unsigned h = tmp(t); mov_imm(t, h, pc); str_x(t, h, RCPU, OFF_PC); }
static int i_line4(T *t)
{
    unsigned op = t->op, mode = (op >> 3) & 7, reg = op & 7;
    if (op == 0x4E71) return 1;
    if (op == 0x4E75) {                                     /* RTS */
        unsigned sp = get_a(t, 7), n = tmp(t), v = tmp(t);
        check(t, sp, 8, false);
        load(t, v, sp, 8);
        add_i(t, n, sp, 8); set_a(t, 7, n);
        str_x(t, v, RCPU, OFF_PC);
        return 2;
    }
    if ((op & 0xFFF8) == 0x4E50 || (op & 0xFFF8) == 0x4808) {   /* LINK */
        unsigned r = regB(t, reg);
        if (r == 7) return 0;
        int64_t disp = (op & 0xFFF8) == 0x4E50 ? (int16_t)f16(t) : (int32_t)f32(t);
        unsigned sp = get_a(t, 7), ns = tmp(t), ar = get_a(t, r);
        sub_i(t, ns, sp, 8);
        check(t, ns, 8, true);
        store(t, ar, ns, 8);
        unsigned fp = tmp(t); mov_r(t, fp, ns); set_a(t, r, fp);
        unsigned d = tmp(t), s2 = tmp(t); mov_imm(t, d, (uint64_t)disp); add_r(t, s2, ns, d); set_a(t, 7, s2);
        return 1;
    }
    if ((op & 0xFFF8) == 0x4E58) {                          /* UNLK */
        unsigned r = regB(t, reg);
        if (r == 7) return 0;
        unsigned fp = get_a(t, r), v = tmp(t), ns = tmp(t);
        check(t, fp, 8, false);
        load(t, v, fp, 8);
        add_i(t, ns, fp, 8);
        set_a(t, 7, ns); set_a(t, r, v);
        return 1;
    }
    if ((op & 0xFF80) == 0x4E80) {                          /* JSR, JMP */
        if (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg == 4)) return 0;
        Loc l;
        if (!ea(t, mode, reg, 2, &l) || l.kind != L_MEM) return 0;
        if (!(op & 0x40)) {
            unsigned sp = get_a(t, 7), ns = tmp(t), ret = tmp(t);
            sub_i(t, ns, sp, 8);
            check(t, ns, 8, true);
            mov_imm(t, ret, t->pc);
            store(t, ret, ns, 8);
            set_a(t, 7, ns);
        }
        str_x(t, l.addr, RCPU, OFF_PC);
        return 2;
    }
    if ((op & 0xF1C0) == 0x41C0) {                          /* LEA */
        if (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg == 4)) return 0;
        Loc l;
        if (!ea(t, mode, reg, 4, &l) || l.kind != L_MEM) return 0;
        unsigned n = tmp(t); mov_r(t, n, l.addr); set_a(t, regR(t, (op >> 9) & 7), n);
        return 1;
    }
    if ((op & 0xFFC0) == 0x4840 && mode != 0) {             /* PEA */
        if (mode == 1 || mode == 3 || mode == 4 || (mode == 7 && reg == 4)) return 0;
        Loc l;
        if (!ea(t, mode, reg, 4, &l) || l.kind != L_MEM) return 0;
        unsigned sp = get_a(t, 7), ns = tmp(t);
        sub_i(t, ns, sp, 8);
        check(t, ns, 8, true);
        store(t, l.addr, ns, 8);
        set_a(t, 7, ns);
        return 1;
    }
    if ((op & 0xFFB8) == 0x4880) {                          /* EXT */
        unsigned r = regB(t, reg), d = get_d(t, r), v = tmp(t);
        if (op & 0x40) {
            if (t->pfx & PFX_W) { sext(t, v, d, 4); t->dn[r] = (int)v; flags_logic(t, v, 8); }
            else { sext(t, v, d, 2); set_d(t, r, 4, v); flags_logic(t, v, 4); }
        } else { sext(t, v, d, 1); set_d(t, r, 2, v); flags_logic(t, v, 2); }
        return 1;
    }
    if ((op & 0xFFF8) == 0x49C0) {                          /* EXTB */
        unsigned r = regB(t, reg), s = wide(t, 4), d = get_d(t, r), v = tmp(t);
        sext(t, v, d, 1); set_d(t, r, s, v); flags_logic(t, v, s);
        return 1;
    }
    if ((op & 0xFFC0) == 0x4C40) {                          /* DIVU.L, DIVS.L */
        uint16_t e = f16(t);
        if (mode == 1) return 0;
        unsigned s = wide(t, 4);
        if (s == 8 && (e & 0x400)) return 0;
        Loc l; if (!ea(t, mode, reg, s, &l)) return 0;
        check_loc(t, &l, s, false);
        unsigned src = ld(t, &l, s);
        if (s == 4) zext(t, src, src, 4);
        emit(t, 0xB4000000u | src);                         /* cbz src: divide by zero, interpreter */
        leave_before(t, t->p - 1);
        flush_flags(t);
        spill_pending(t);
        unsigned ex = tmp(t), sz = tmp(t), self = tmp(t);
        mov_imm(t, ex, e); mov_imm(t, sz, s); mov_r(t, self, RCPU);
        unsigned args[4] = { self, src, ex, sz };
        call(t, (void *)h_div, args, 4);
        ccr_written(t);
        return 1;
    }
    if ((op & 0xFFC0) == 0x4C00) {                          /* MULU.L, MULS.L without the high register */
        uint16_t e = f16(t);
        bool sg = e & 0x800;
        if ((e & 0x400) || mode == 1) return 0;
        unsigned s = wide(t, 4), dl = (e >> 12) & 7;
        Loc l; if (!ea(t, mode, reg, s, &l)) return 0;
        check_loc(t, &l, s, false);
        unsigned src = ld(t, &l, s), a = get_d(t, dl), lo = tmp(t), v = tmp(t), hi = tmp(t);
        if (s == 8) {
            emit(t, 0x9B007C00u | (a << 16) | (src << 5) | lo);                          /* mul */
            emit(t, (sg ? 0x9B407C00u : 0x9BC07C00u) | (a << 16) | (src << 5) | hi);     /* smulh, umulh */
            if (sg) { asr_i(t, v, lo, 63); cmp_r(t, hi, v); } else cmp_i(t, hi, 0);
            cset(t, v, C_NE);
            t->dn[dl] = (int)lo;
        } else {
            emit(t, (sg ? 0x9B207C00u : 0x9BA07C00u) | (a << 16) | (src << 5) | hi);     /* smull, umull */
            zext(t, lo, hi, 4);
            if (sg) { sext(t, v, hi, 4); cmp_r(t, hi, v); } else { lsr_i(t, v, hi, 32); cmp_i(t, v, 0); }
            cset(t, v, C_NE);
            t->dn[dl] = (int)lo;
        }
        /* X kept, N and Z of the result, V the overflow, C clear */
        unsigned out = tmp(t), z = tmp(t);
        ubfx(t, out, lo, s * 8 - 1, 1); lsl_i(t, out, out, 3);
        cmp_i(t, lo, 0); cset(t, z, C_EQ); bfi(t, out, z, 2, 1);
        bfi(t, out, v, 1, 1);
        if (t->ccr >= 0) mov_r(t, z, (unsigned)t->ccr); else ldrb_(t, z, RCPU, OFF_CCR);
        ubfx(t, z, z, 4, 1); bfi(t, out, z, 4, 1);
        t->ccr = (int)out;
        ccr_written(t);
        return 1;
    }
    if ((op & 0xFB80) == 0x4880) {                          /* MOVEM */
        unsigned s = (op & 0x40) ? wide(t, 4) : 2;
        bool to_regs = op & 0x400;
        uint16_t mask = f16(t);
        if (mode < 2 || (to_regs && mode == 4) || (!to_regs && mode == 3)) return 0;
        unsigned cnt = (unsigned)__builtin_popcount(mask);
        if (!cnt) return 0;
        unsigned r = regB(t, reg);
        if (mode == 4) {
            unsigned a = get_a(t, r), low = tmp(t), ad = tmp(t);
            sub_i(t, low, a, cnt * s);
            check_range(t, low, cnt * s, s, true);
            /* the registers' values before the instruction, An's too, as the interpreter stores */
            unsigned off = cnt * s;
            for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
                unsigned keep = t->ntmp;
                off -= s;
                unsigned v = i < 8 ? get_a(t, 7 - (unsigned)i) : get_d(t, 15 - (unsigned)i);
                add_i(t, ad, low, off);
                store(t, v, ad, s);
                t->ntmp = keep;
            }
            set_a(t, r, low);
            return 1;
        }
        unsigned base;
        if (mode == 3) base = get_a(t, r);
        else { Loc l; if (!ea(t, mode, reg, s, &l) || l.kind != L_MEM) return 0; base = l.addr; }
        check_range(t, base, cnt * s, s, !to_regs);
        /* Nothing below can fail: registers are written at once. */
        unsigned off = 0, ad = tmp(t);
        for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
            unsigned keep = t->ntmp;
            add_i(t, ad, base, off);
            unsigned v = tmp(t);
            if (to_regs) {
                load(t, v, ad, s);
                if (s != 8) sext(t, v, v, s);
                str_x(t, v, RCPU, i < 8 ? OFF_D((unsigned)i) : OFF_A((unsigned)i - 8));
            } else {
                unsigned g = i < 8 ? get_d(t, (unsigned)i) : get_a(t, (unsigned)i - 8);
                store(t, g, ad, s);
            }
            t->ntmp = keep;
            off += s;
        }
        if (mode == 3) { unsigned n = tmp(t); add_i(t, n, base, cnt * s); set_a(t, r, n); }
        return 1;
    }
    unsigned f = (op >> 6) & 3;
    if (f == 3) return 0;
    unsigned s = f == 3 ? 0 : wide(t, 1u << f);
    unsigned k = (op >> 8) & 15;
    if (k == 10) {                                          /* TST */
        if (mode == 1 && s == 1) return 0;
        Loc l; if (!ea(t, mode, reg, s, &l)) return 0;
        check_loc(t, &l, s, false);
        unsigned v = ld(t, &l, s);
        flags_logic(t, v, s);
        return 1;
    }
    if (k != 2 && k != 4 && k != 6) return 0;
    if (mode == 1 || !alterable_ok(mode, reg)) return 0;
    Loc l; if (!ea(t, mode, reg, s, &l)) return 0;
    check_loc(t, &l, s, true);
    if (k == 2) {                                           /* CLR */
        unsigned z = tmp(t), c = tmp(t);
        mov_imm(t, z, 0);
        if (t->ccr >= 0) mov_r(t, c, (unsigned)t->ccr); else ldrb_(t, c, RCPU, OFF_CCR);
        ubfx(t, c, c, 4, 1); lsl_i(t, c, c, 4);
        emit(t, 0xB27E0000u | (c << 5) | c);                 /* orr c, c, #4 */
        t->ccr = (int)c;
        ccr_written(t);
        st(t, &l, s, z);
        return 1;
    }
    unsigned v = ld(t, &l, s), r;
    if (k == 4) { unsigned z = tmp(t); mov_imm(t, z, 0); r = arith(t, true, z, v, s, true, false); }   /* NEG */
    else { r = tmp(t); mvn_r(t, r, v); zext(t, r, r, s); flags_logic(t, r, s); }                   /* NOT */
    st(t, &l, s, r);
    return 1;
}
static int i_line5(T *t)
{
    unsigned op = t->op, mode = (op >> 3) & 7, reg = op & 7, f = (op >> 6) & 3;
    if (f == 3) {
        unsigned cc = (op >> 8) & 15;
        if (mode == 1) {                                    /* DBcc */
            uint64_t base = t->pc;
            int64_t disp = (int16_t)f16(t);
            unsigned r = regB(t, reg);
            uint32_t *taken = NULL;
            if (cc == 0) { set_pc_imm(t, t->pc); return 2; }   /* DBT: never loops */
            if (cc != 1) { ccr_to_nzcv(t); taken = bcond_site(t, cond_map[cc]); }
            /* condition false: count down */
            unsigned d = get_d(t, r), n = tmp(t), m = tmp(t);
            sub_i(t, n, d, 1);
            mov_r(t, m, d); bfi(t, m, n, 0, 16);
            str_x(t, m, RCPU, OFF_D(r));
            unsigned lo = tmp(t), pc1 = tmp(t), pc2 = tmp(t);
            ubfx(t, lo, m, 0, 16);
            mov_imm(t, pc1, base + (uint64_t)disp);
            mov_imm(t, pc2, t->pc);
            mov_imm(t, n, 0xFFFF);
            cmp_r(t, lo, n);
            csel(t, pc1, pc2, pc1, C_EQ);
            str_x(t, pc1, RCPU, OFF_PC);
            uint32_t *out = b_site(t);
            if (taken) { patch(taken, t->p); set_pc_imm(t, t->pc); }
            patch(out, t->p);
            return 2;
        }
        if (mode == 7 && reg >= 2) return 0;                /* TRAPcc and others */
        Loc l; if (!ea(t, mode, reg, 1, &l)) return 0;     /* Scc */
        if (l.kind == L_A) return 0;
        check_loc(t, &l, 1, true);
        unsigned v = tmp(t);
        if (cc == 0) mov_imm(t, v, 0xFF);
        else if (cc == 1) mov_imm(t, v, 0);
        else { ccr_to_nzcv(t); cset(t, v, cond_map[cc]); emit(t, 0xCB000000u | (v << 16) | (ZR << 5) | v); zext(t, v, v, 1); }
        st(t, &l, 1, v);
        return 1;
    }
    uint64_t n = (op >> 9) & 7;
    if (!n) n = 8;
    unsigned s = wide(t, 1u << f);
    if (mode == 1) {
        if (s == 1) return 0;
        unsigned r = regB(t, reg), a = get_a(t, r), h = tmp(t);
        if (op & 0x100) sub_i(t, h, a, (unsigned)n); else add_i(t, h, a, (unsigned)n);
        set_a(t, r, h);
        return 1;
    }
    if (!alterable_ok(mode, reg)) return 0;
    Loc l; if (!ea(t, mode, reg, s, &l)) return 0;
    check_loc(t, &l, s, true);
    unsigned v = ld(t, &l, s), k = tmp(t);
    mov_imm(t, k, n);
    unsigned r = arith(t, op & 0x100, v, k, s, true, false);
    st(t, &l, s, r);
    return 1;
}
/* 3: the block ends here in exits to fixed targets (x_cond, x_taken, x_next) */
static int i_branch(T *t)
{
    unsigned op = t->op, cc = (op >> 8) & 15;
    uint64_t base = t->pc;
    int64_t d = (int8_t)op;
    if (d == 0) d = (int16_t)f16(t);
    else if ((op & 0xFF) == 0xFF) d = (int32_t)f32(t);
    uint64_t target = base + (uint64_t)d, next = t->pc;
    t->x_taken = target; t->x_next = next; t->x_cond = C_AL;
    if (cc == 1) {                                          /* BSR */
        unsigned sp = get_a(t, 7), ns = tmp(t), ret = tmp(t);
        sub_i(t, ns, sp, 8);
        check(t, ns, 8, true);
        mov_imm(t, ret, next);
        store(t, ret, ns, 8);
        set_a(t, 7, ns);
        return 3;
    }
    if (cc == 0) return 3;
    ccr_to_nzcv(t);                                         /* host flags stay until the exits */
    t->x_cond = cond_map[cc];
    return 3;
}
static bool i_alu(T *t)
{
    unsigned op = t->op, line = op >> 12, mode = (op >> 3) & 7, reg = op & 7, om = (op >> 6) & 7;
    unsigned r = regR(t, (op >> 9) & 7);
    if ((line == 8 || line == 12) && (om == 3 || om == 7)) return false;   /* DIV, MUL */
    if (om == 3 || om == 7) {                               /* ADDA, SUBA, CMPA */
        unsigned s = om == 3 ? 2 : wide(t, 4);
        Loc l; if (!ea(t, mode, reg, s, &l)) return false;
        check_loc(t, &l, s, false);
        unsigned v = ld(t, &l, s), e = tmp(t);
        sext(t, e, v, s);
        unsigned a = get_a(t, r);
        if (line == 0xD || line == 9) { unsigned h = tmp(t); if (line == 0xD) add_r(t, h, a, e); else sub_r(t, h, a, e); set_a(t, r, h); }
        else arith(t, true, a, e, 8, true, true);
        return true;
    }
    unsigned f = om & 3;
    unsigned s = wide(t, 1u << f);
    bool to_ea = om & 4;
    if (to_ea && mode <= 1 && !(line == 0xB && mode == 0)) return false;
    if (line == 0xB && !to_ea) {                            /* CMP */
        if (s == 1 && mode == 1) return false;
        Loc l; if (!ea(t, mode, reg, s, &l)) return false;
        check_loc(t, &l, s, false);
        unsigned d = get_d(t, r), dz = tmp(t);
        zext(t, dz, d, s);
        unsigned v = ld(t, &l, s);
        arith(t, true, dz, v, s, true, true);
        return true;
    }
    if (!to_ea && s == 1 && mode == 1) return false;
    if (!to_ea && mode == 1 && (line == 8 || line == 12)) return false;
    if (to_ea && !alterable_ok(mode, reg)) return false;
    Loc l; if (!ea(t, mode, reg, s, &l)) return false;
    check_loc(t, &l, s, to_ea);
    unsigned e = ld(t, &l, s), d = tmp(t);
    zext(t, d, get_d(t, r), s);
    unsigned a = to_ea ? e : d, b = to_ea ? d : e, res;
    switch (line) {
    case 8: res = tmp(t); orr_r(t, res, a, b); flags_logic(t, res, s); break;
    case 12: res = tmp(t); and_r(t, res, a, b); flags_logic(t, res, s); break;
    case 0xB: res = tmp(t); eor_r(t, res, a, b); flags_logic(t, res, s); break;
    case 9: res = arith(t, true, a, b, s, true, false); break;
    default: res = arith(t, false, a, b, s, true, false); break;
    }
    if (to_ea) st(t, &l, s, res); else set_d(t, r, s, res);
    return true;
}
static bool i_bitop(T *t);
static bool i_immediate(T *t)
{
    unsigned op = t->op, kind = (op >> 9) & 7, f = (op >> 6) & 3, mode = (op >> 3) & 7, reg = op & 7;
    if (mode == 7 && reg == 4) return false;                /* to CCR, SR */
    if ((op & 0x138) == 0x108) return false;                /* MOVEP */
    if (kind == 4 || (op & 0x100)) return i_bitop(t);
    if (f == 3 || kind == 7) return false;
    if (kind != 0 && kind != 1 && kind != 2 && kind != 3 && kind != 5 && kind != 6) return false;
    if (mode == 1) return false;
    if (kind != 6 && !alterable_ok(mode, reg)) return false;
    if (kind == 6 && mode == 7 && reg > 3) return false;
    unsigned s = wide(t, 1u << f);
    uint64_t imm = s == 1 ? (f16(t) & 0xFF) : s == 2 ? f16(t) : s == 4 ? f32(t) : f64(t);
    Loc l; if (!ea(t, mode, reg, s, &l)) return false;
    check_loc(t, &l, s, kind != 6);
    unsigned v = ld(t, &l, s), k = tmp(t), r;
    mov_imm(t, k, imm);
    switch (kind) {
    case 0: r = tmp(t); orr_r(t, r, v, k); flags_logic(t, r, s); break;
    case 1: r = tmp(t); and_r(t, r, v, k); flags_logic(t, r, s); break;
    case 5: r = tmp(t); eor_r(t, r, v, k); flags_logic(t, r, s); break;
    case 2: r = arith(t, true, v, k, s, true, false); break;
    case 3: r = arith(t, false, v, k, s, true, false); break;
    default: arith(t, true, v, k, s, true, true); return true;
    }
    st(t, &l, s, r);
    return true;
}
static bool i_shift(T *t)
{
    unsigned op = t->op, f = (op >> 6) & 3;
    bool left = op & 0x100;
    if (f == 3) {                                           /* memory, a word by one */
        unsigned mode = (op >> 3) & 7, reg = op & 7, kind = (op >> 9) & 3;
        if (op & 0x800) return false;                       /* bit fields */
        if (mode < 2 || !alterable_ok(mode, reg)) return false;
        Loc l; if (!ea(t, mode, reg, 2, &l)) return false;
        check(t, l.addr, 2, true);
        unsigned v = ld(t, &l, 2);
        mov_r(t, RKEEP, l.addr);
        flush_flags(t);
        spill_pending(t);
        unsigned n = tmp(t), meta = tmp(t), cc = tmp(t);
        mov_imm(t, n, 1); mov_imm(t, meta, 2u | kind << 4 | (left ? 0x100u : 0));
        ldrb_(t, cc, RCPU, OFF_CCR);
        unsigned args[4] = { v, n, meta, cc };
        call(t, (void *)h_shift, args, 4);
        unsigned res = tmp(t), ccr = tmp(t);
        mov_r(t, res, 0); mov_r(t, ccr, 1);
        store(t, res, RKEEP, 2);
        t->ccr = (int)ccr; ccr_written(t);
        return true;
    }
    unsigned s = wide(t, 1u << f), r = regB(t, op & 7), kind = (op >> 3) & 3, cnt = (op >> 9) & 7;
    unsigned v = get_d(t, r), n = tmp(t);
    if (op & 0x20) { ubfx(t, n, get_d(t, regR(t, cnt)), 0, 6); }
    else mov_imm(t, n, cnt ? cnt : 8);
    flush_flags(t);
    unsigned meta = tmp(t), cc = tmp(t);
    mov_imm(t, meta, s | kind << 4 | (left ? 0x100u : 0));
    ldrb_(t, cc, RCPU, OFF_CCR);
    unsigned args[4] = { v, n, meta, cc };
    call(t, (void *)h_shift, args, 4);
    unsigned res = tmp(t), ccr = tmp(t);
    mov_r(t, res, 0); mov_r(t, ccr, 1);
    set_d(t, r, s, res);
    t->ccr = (int)ccr; ccr_written(t);
    return true;
}
/* BTST, BCHG, BCLR, BSET: Z only */
static bool i_bitop(T *t)
{
    unsigned op = t->op, mode = (op >> 3) & 7, reg = op & 7, how = (op >> 6) & 3;
    unsigned bit;
    bool dyn = op & 0x100;
    uint16_t imm = 0;
    if (dyn) { if (mode == 1) return false; }
    else imm = f16(t) & 0xFF;
    unsigned s = mode == 0 ? wide(t, 4) : 1;
    Loc l; if (!ea(t, mode, reg, 1, &l)) return false;
    if (how && l.kind != L_D && l.kind != L_MEM) return false;
    if (l.kind == L_A) return false;
    if (how && l.kind == L_MEM && !alterable_ok(mode, reg)) return false;
    check_loc(t, &l, s, how != 0);
    bit = tmp(t);
    if (dyn) mov_r(t, bit, get_d(t, regR(t, (op >> 9) & 7))); else mov_imm(t, bit, imm);
    ubfx(t, bit, bit, 0, s == 8 ? 6 : s == 4 ? 5 : 3);
    unsigned v = ld(t, &l, s), m = tmp(t), one = tmp(t);
    mov_imm(t, one, 1);
    emit(t, 0x9AC02000u | (bit << 16) | (one << 5) | m);    /* lslv m, one, bit */
    flush_flags(t);
    unsigned cc = tmp(t), z = tmp(t);
    ldrb_(t, cc, RCPU, OFF_CCR);
    ands_r(t, true, ZR, v, m); cset(t, z, C_EQ); bfi(t, cc, z, 2, 1);
    t->ccr = (int)cc; ccr_written(t);
    if (how) {
        unsigned r = tmp(t);
        if (how == 1) eor_r(t, r, v, m);
        else if (how == 2) emit(t, 0x8A200000u | (m << 16) | (v << 5) | r);   /* bic */
        else orr_r(t, r, v, m);
        st(t, &l, s, r);
    }
    return true;
}
/* QRX_EC64_JIT_OFF: groups the JIT leaves to the interpreter, to find a faulty one. Bits: 1 MOVE,
 * 2 MOVEQ, 4 line 4 (also 0x200 MOVEM, 0x400 MUL/DIV), 8 line 5, 0x10 branches, 0x20 ALU,
 * 0x40 immediates and bit operations, 0x80 shifts, 0x800 chaining. */
static unsigned jit_off(void)
{
    static int done; static unsigned v;
    if (!done) { const char *e = getenv("QRX_EC64_JIT_OFF"); v = e ? (unsigned)strtoul(e, NULL, 0) : 0; done = 1; }
    return v;
}
/* One instruction: 1 translated, 2 translated and ends the block, 0 not translated. */
static int translate_one(T *t)
{
    t->start = t->pc;
    t->ntmp = 0;
    for (unsigned r = 0; r < 16; r++) { t->an[r] = -1; t->dn[r] = -1; }
    t->ccr = -1;
    t->pend_start = t->pend;
    t->pfx = 0;
    uint16_t op;
    if (!code16(t, &op)) { t->pc = t->start; return 0; }
    if ((op >> 12) == 0xA) {
        if (op & 0xFE0) { t->pc = t->start; return 0; }
        t->pfx = op & 0x1F;
        if (!code16(t, &op) || (op >> 12) == 0xA) { t->pc = t->start; return 0; }
    }
    t->op = op;
    if ((t->pfx & PFX_N) && ((op >> 12) < 1 || (op >> 12) > 3)) { t->pc = t->start; return 0; }
    uint32_t *mark = t->p;
    unsigned nstub = t->nstub;
    int res;
    unsigned off = jit_off(), line = op >> 12;
    if (off && (((off & 1) && line >= 1 && line <= 3) || ((off & 2) && line == 7) || ((off & 4) && line == 4) ||
                ((off & 0x200) && (op & 0xFB80) == 0x4880 && ((op >> 3) & 7) >= 2) || ((off & 0x400) && (op & 0xFF80) == 0x4C00) ||
                ((off & 8) && line == 5) || ((off & 0x10) && line == 6) || ((off & 0x20) && (line == 8 || line == 9 || (line >= 11 && line <= 13))) ||
                ((off & 0x40) && line == 0) || ((off & 0x80) && line == 14))) { t->pc = t->start; return 0; }
    switch (op >> 12) {
    case 0: res = i_immediate(t); break;
    case 1: case 2: case 3: res = i_move(t); break;
    case 4: res = i_line4(t); break;
    case 5: res = i_line5(t); break;
    case 6: res = i_branch(t); break;
    case 7: res = i_moveq(t); break;
    case 8: case 9: case 11: case 12: case 13: res = i_alu(t); break;
    case 14: res = i_shift(t); break;
    default: res = 0; break;
    }
    if (res == 0 || t->bad) {                               /* undo this instruction's code */
        t->p = mark; t->nstub = nstub; t->pc = t->start; t->pend = t->pend_start; t->bad = t->p >= t->j->end;
        return 0;
    }
    commit(t);
    t->done++;
    return res;
}

/* ------------------------------------------------------------------ blocks */
static void prologue(T *t)
{
    emit(t, 0xA9BB7BFDu);   /* stp x29, x30, [sp, #-80]! */
    emit(t, 0xA90153F3u);   /* stp x19, x20, [sp, #16] */
    emit(t, 0xA9025BF5u);   /* stp x21, x22, [sp, #32] */
    emit(t, 0xA90363F7u);   /* stp x23, x24, [sp, #48] */
    emit(t, 0xA9046BF9u);   /* stp x25, x26, [sp, #64] */
    mov_r(t, RCPU, 0);
    mov_r(t, RLIMIT, 1);
    mov_imm(t, RDONE, 0);
    ldr_x(t, RRAM, RCPU, (unsigned)offsetof(qec64_cpu, ram));
    ldr_x(t, RSIZE, RCPU, (unsigned)offsetof(qec64_cpu, ram_size));
    ldr_x(t, RMAP, RCPU, (unsigned)offsetof(qec64_cpu, code_map));
}
static void epilogue(T *t)
{
    emit(t, 0xA9446BF9u);   /* ldp x25, x26, [sp, #64] */
    emit(t, 0xA94363F7u);   /* ldp x23, x24, [sp, #48] */
    emit(t, 0xA9425BF5u);   /* ldp x21, x22, [sp, #32] */
    emit(t, 0xA94153F3u);   /* ldp x19, x20, [sp, #16] */
    emit(t, 0xA8C57BFDu);   /* ldp x29, x30, [sp], #80 */
    emit(t, 0xD65F03C0u);   /* ret */
}
static void jit_flush(Jit *j)
{
    j->last_exit = NULL;
    j->flushes++;
    j->pos = j->buf;
    memset(j->hash, 0, sizeof j->hash);
    j->nblocks = 0;
}
static Jit *jit_of(qec64_cpu *c)
{
    if (c->jit) return c->jit;
    Jit *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    int flags = MAP_PRIVATE | MAP_ANON;
#if defined(__APPLE__)
    flags |= MAP_JIT;
#endif
    void *m = mmap(NULL, CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
    if (m == MAP_FAILED) { free(j); return NULL; }
    j->buf = m; j->end = j->buf + CODE_BYTES / 4 - 4096;
    j->maxblocks = 1u << 18;
    j->blocks = calloc(j->maxblocks, sizeof(Block));
    if (!j->blocks) { munmap(m, CODE_BYTES); free(j); return NULL; }
    jit_flush(j);
    j->gen = __atomic_load_n(c->code_gen, __ATOMIC_ACQUIRE);
    if (getenv("QRX_EC64_JIT_STATS")) j->single = calloc(1u << 21, sizeof(uint32_t));
    c->jit = j;
    return j;
}
void qec64_jit_free(qec64_cpu *c)
{
    Jit *j = c->jit;
    if (!j) return;
    if (getenv("QRX_EC64_JIT_STATS"))
        fprintf(stderr, "[EC64 JIT] core %u: %llu blocks translated, %llu entered, %llu chained, %llu single instructions in the interpreter, %llu flushes\n",
                c->core_id, (unsigned long long)j->translated, (unsigned long long)j->entered, (unsigned long long)j->chained, (unsigned long long)j->interpreted, (unsigned long long)j->flushes);
    if (j->single) {
        for (int k = 0; k < 12; k++) {
            unsigned best = 0;
            for (unsigned i = 1; i < (1u << 21); i++) if (j->single[i] > j->single[best]) best = i;
            if (!j->single[best]) break;
            fprintf(stderr, "[EC64 JIT]   interpreted %02x:%04x %u\n", best >> 16, best & 0xFFFF, j->single[best]);
            j->single[best] = 0;
        }
        free(j->single);
    }
    munmap(j->buf, CODE_BYTES);
    free(j->blocks);
    free(j);
    c->jit = NULL;
}
static inline unsigned hash_pc(uint64_t pc) { return (unsigned)((pc >> 1) ^ (pc >> 17)) & ((1u << HASH_BITS) - 1); }

/* Leave for a fixed pc: count, then chain into its block when patched and the budget allows,
 * else return to the dispatcher, which patches the branch once the target has code. */
static void exit_to(T *t, uint64_t target)
{
    materialize(t, t->pend);
    add_i(t, RDONE, RDONE, t->done);
    cmp_r(t, RDONE, RLIMIT);
    uint32_t *full = bcond_site(t, C_HS);
    uint32_t *site = b_site(t);
    patch(full, t->p);
    patch(site, t->p);
    mov_imm(t, S0, target);
    str_x(t, S0, RCPU, OFF_PC);
    mov_imm(t, S0, (uint64_t)(uintptr_t)&t->j->last_exit);
    mov_imm(t, S1, (uint64_t)(uintptr_t)site);
    str_x(t, S1, S0, 0);
    mov_r(t, 0, RDONE);
    epilogue(t);
}
static Block *translate(qec64_cpu *c, Jit *j, uint64_t pc)
{
    if (j->nblocks >= j->maxblocks || j->end - j->pos < 65536) jit_flush(j);
    static __thread T t;
    memset(&t, 0, offsetof(T, stub));
    t.c = c; t.j = j; t.p = j->pos; t.pc = pc; t.nstub = 0;
#if defined(__APPLE__)
    pthread_jit_write_protect_np(0);
#endif
    uint32_t *code = t.p;
    prologue(&t);
    uint32_t *body = t.p;
    int last = 0;
    while (t.done < qec64_jit_block_limit && !t.bad) {
        last = translate_one(&t);
        if (last != 1) break;
    }
    Block *b = &j->blocks[j->nblocks++];
    b->pc = pc;
    b->count = t.done;
    if (t.done == 0 || t.bad) {
        b->code = NULL;
        t.p = code;
    } else {
        t.ntmp = 0;
        if (last == 3) {                       /* a branch: one or two fixed targets */
            if (t.x_cond != C_AL) {
                uint32_t *taken = bcond_site(&t, t.x_cond);
                exit_to(&t, t.x_next);
                patch(taken, t.p);
            }
            exit_to(&t, t.x_taken);
        } else if (last == 2) {                /* pc already written */
            materialize(&t, t.pend);
            add_i(&t, 0, RDONE, t.done);
            epilogue(&t);
        } else exit_to(&t, t.pc);              /* falls into the next instruction */
        for (unsigned i = 0; i < t.nstub; i++) {
            patch(t.stub[i].at, t.p);
            materialize(&t, t.stub[i].pend);
            mov_imm(&t, S0, t.stub[i].pc);
            str_x(&t, S0, RCPU, OFF_PC);
            add_i(&t, 0, RDONE, t.stub[i].done);
            epilogue(&t);
        }
        if (t.bad) { b->code = NULL; t.p = code; }
        else {
            b->code = code;
            b->body = body;
            /* mark the RAM this block came from */
            for (uint64_t g = pc >> QEC64_CODE_SHIFT; g <= (t.pc - 1) >> QEC64_CODE_SHIFT; g++)
                __atomic_store_n(&c->code_map[g], 1, __ATOMIC_RELAXED);
        }
    }
#if defined(__APPLE__)
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(code, (size_t)((char *)t.p - (char *)code));
#else
    __builtin___clear_cache((char *)code, (char *)t.p);
#endif
    if (b->code) qrx_perfmap(code, (size_t)((char *)t.p - (char *)code), "ec64_%016llx", (unsigned long long)pc);
    j->pos = t.p;
    b->next = j->hash[hash_pc(pc)];
    j->hash[hash_pc(pc)] = b;
    j->translated++;
    return b;
}

static void count_single(qec64_cpu *c, Jit *j)
{
    if (!j->single) return;
    uint64_t pc = c->pc;
    if (pc + 4 > c->ram_size) return;
    unsigned w = (unsigned)(c->ram[pc] << 8 | c->ram[pc + 1]), key = w;
    if ((w >> 12) == 0xA) key = ((w & 0x1F) << 16) | (unsigned)(c->ram[pc + 2] << 8 | c->ram[pc + 3]);
    j->single[key & 0x1FFFFF]++;
}
static uint64_t pending_irq(qec64_cpu *c)
{
    uint32_t p = __atomic_load_n(&c->pending, __ATOMIC_RELAXED);
    if (!p || !c->vbr_set) return 0;
    unsigned level = 31u - (unsigned)__builtin_clz(p);
    return level > c->ipl || level == 7;
}

/* QRX_EC64_JIT_VERIFY=1: every block runs in the interpreter first; its RAM writes are undone, the
 * translation runs from the same state, and both results must agree. The first block that does
 * not is reported with both states, and the run stops. Slow; for finding JIT bugs. */
static int verify_on(void)
{
    static int v = -1;
    if (v < 0) v = getenv("QRX_EC64_JIT_VERIFY") != NULL;
    return v;
}
static void verify_report(const qec64_cpu *s0, const qec64_cpu *si, const qec64_cpu *sj, const Block *b, const char *what)
{
    fprintf(stderr, "[EC64 JIT VERIFY] core %u block %llx (%u instructions): %s\n", s0->core_id,
            (unsigned long long)b->pc, b->count, what);
    fprintf(stderr, "  code:");
    for (unsigned i = 0; i < 48 && b->pc + i * 2 + 1 < s0->ram_size; i++)
        fprintf(stderr, " %02x%02x", s0->ram[b->pc + i * 2], s0->ram[b->pc + i * 2 + 1]);
    fprintf(stderr, "\n");
    for (unsigned r = 0; r < 16; r++) {
        if (si->d[r] != sj->d[r] || si->d[r] != s0->d[r] || r < 8)
            fprintf(stderr, "  d%-2u before %016llx interp %016llx jit %016llx%s\n", r, (unsigned long long)s0->d[r],
                    (unsigned long long)si->d[r], (unsigned long long)sj->d[r], si->d[r] != sj->d[r] ? "  <--" : "");
    }
    for (unsigned r = 0; r < 16; r++) {
        if (si->a[r] != sj->a[r] || r < 8)
            fprintf(stderr, "  a%-2u before %016llx interp %016llx jit %016llx%s\n", r, (unsigned long long)s0->a[r],
                    (unsigned long long)si->a[r], (unsigned long long)sj->a[r], si->a[r] != sj->a[r] ? "  <--" : "");
    }
    fprintf(stderr, "  pc before %llx interp %llx jit %llx; ccr before %02x interp %02x jit %02x\n",
            (unsigned long long)s0->pc, (unsigned long long)si->pc, (unsigned long long)sj->pc, s0->ccr, si->ccr, sj->ccr);
}
/* One block both ways. Returns 1 when the block was handled (the core is in the interpreter's state
 * and *n has the instructions run), 0 when it must run normally. */
static int verify_block(qec64_cpu *c, Block *b, uint64_t *n, enum qec64_status *st)
{
    static __thread qec64_undo log[4096];
    static __thread uint8_t now[4096][8];
    qec64_cpu s0 = *c;
    uint32_t gen0 = __atomic_load_n(c->code_gen, __ATOMIC_ACQUIRE);
    qec64_undo_log = log; qec64_undo_n = 0; qec64_undo_cap = 4096; qec64_side_effects = 0;
    *st = qec64_run(c, b->count);
    qec64_undo_log = NULL;
    *n = c->instructions - s0.instructions;
    /* Not comparable: something happened that must not happen twice, or that changes the code,
     * or the interpreter took an interrupt or stopped. Keep the interpreter's result. */
    if (*st != QEC64_RUNNING || qec64_side_effects || __atomic_load_n(c->code_gen, __ATOMIC_ACQUIRE) != gen0 ||
        *n != b->count || c->supervisor != s0.supervisor || c->ipl != s0.ipl || c->waiting)
        return 1;
    qec64_cpu si = *c;
    unsigned nw = qec64_undo_n;
    for (unsigned i = 0; i < nw; i++) memcpy(now[i], c->ram + log[i].addr, log[i].n);
    for (unsigned i = nw; i-- > 0;) memcpy(c->ram + log[i].addr, log[i].old, log[i].n);
    *c = s0;
    Jit *j = c->jit;
    uint64_t done = ((uint64_t (*)(qec64_cpu *, uint64_t))(void *)b->code)(c, 0);
    j->last_exit = NULL;
    c->instructions += done;
    if (done < b->count) {                       /* left early: the interpreter finishes the block */
        enum qec64_status s2 = qec64_run(c, b->count - done);
        if (s2 != QEC64_RUNNING || c->supervisor != s0.supervisor) {
            /* an interrupt came in between: take the interpreter's result */
            for (unsigned i = 0; i < nw; i++) memcpy(c->ram + log[i].addr, now[i], log[i].n);
            *c = si;
            return 1;
        }
    }
    qec64_cpu sj = *c;
    const char *bad = NULL;
    if (memcmp(si.d, sj.d, sizeof si.d) || memcmp(si.a, sj.a, sizeof si.a)) bad = "registers differ";
    else if (si.pc != sj.pc) bad = "pc differs";
    else if ((si.ccr & 0x1F) != (sj.ccr & 0x1F)) bad = "flags differ";
    for (unsigned i = 0; !bad && i < nw; i++)
        if (memcmp(c->ram + log[i].addr, now[i], log[i].n)) {
            static char m[96];
            snprintf(m, sizeof m, "memory at %llx differs", (unsigned long long)log[i].addr);
            bad = m;
        }
    if (bad) {
        if (si.pending != s0.pending || sj.pending != s0.pending) {   /* an interrupt arrived meanwhile */
            for (unsigned i = 0; i < nw; i++) memcpy(c->ram + log[i].addr, now[i], log[i].n);
            *c = si;
            return 1;
        }
        verify_report(&s0, &si, &sj, b, bad);
        static int reports;
        if (++reports >= (getenv("QRX_EC64_JIT_VERIFY_MAX") ? atoi(getenv("QRX_EC64_JIT_VERIFY_MAX")) : 1)) abort();
        for (unsigned i = 0; i < nw; i++) memcpy(c->ram + log[i].addr, now[i], log[i].n);
        *c = si;
    }
    return 1;
}

enum qec64_status qec64_jit_run(qec64_cpu *c, uint64_t max)
{
    if (!c->code_map || !c->m64) return qec64_run(c, max);
    Jit *j = jit_of(c);
    if (!j) return qec64_run(c, max);
    uint64_t left = max;
    c->status = QEC64_RUNNING;
    while (left) {
        if (c->gate) { c->gate(c); j->last_exit = NULL; }
        if (!c->m64 || c->waiting || pending_irq(c)) {
            j->last_exit = NULL;
            uint64_t before = c->instructions;
            enum qec64_status st = qec64_run(c, 1);
            uint64_t n = c->instructions - before;
            left = n >= left ? 0 : left - (n ? n : 1);
            if (st != QEC64_RUNNING) return st;
            continue;
        }
        uint32_t g = __atomic_load_n(c->code_gen, __ATOMIC_ACQUIRE);
        if (g != j->gen) { jit_flush(j); j->gen = g; }
        uint64_t pc = c->pc;
        Block *b = j->hash[hash_pc(pc)];
        while (b && b->pc != pc) b = b->next;
        uint32_t *site = j->last_exit;
        j->last_exit = NULL;
        if (site && j->last_exit_pc != pc) site = NULL;
        if (!b) {
            uint64_t fl = j->flushes;
            b = translate(c, j, pc);
            if (fl != j->flushes) site = NULL;
        }
        if (verify_on()) site = NULL;                    /* each block on its own */
        if (site && b->code && !(jit_off() & 0x800) && !c->gate) {   /* chain the exit that brought us here */
            int64_t dist = b->body - site;
#if defined(__APPLE__)
            pthread_jit_write_protect_np(0);
#endif
            *site = 0x14000000u | (uint32_t)(dist & 0x3FFFFFF);
#if defined(__APPLE__)
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(site, 4);
#else
            __builtin___clear_cache((char *)site, (char *)(site + 1));
#endif
            j->chained++;
        }
        if (!b->code || b->count > left) {
            j->interpreted++;
            j->last_exit = NULL;
            count_single(c, j);
            uint64_t before = c->instructions;
            enum qec64_status st = qec64_run(c, 1);
            uint64_t n = c->instructions - before;
            left = n >= left ? 0 : left - (n ? n : 1);
            if (st != QEC64_RUNNING) return st;
            continue;
        }
        if (__builtin_expect(verify_on(), 0)) {
            uint64_t n = 0;
            enum qec64_status st = QEC64_RUNNING;
            verify_block(c, b, &n, &st);
            left = n >= left ? 0 : left - (n ? n : 1);
            if (st != QEC64_RUNNING) return st;
            continue;
        }
        j->entered++;
        if (__builtin_expect((j->entered & ((1u << 26) - 1)) == 0, 0) && getenv("QRX_EC64_JIT_SAMPLE"))
            fprintf(stderr, "[EC64 JIT] core %u at %llx sp %llx d0 %llx a0 %llx sr ipl %u super %d\n", c->core_id, (unsigned long long)pc,
                    (unsigned long long)c->a[7], (unsigned long long)c->d[0], (unsigned long long)c->a[0], c->ipl, c->supervisor);
        uint64_t limit = left > MAX_INSNS ? left - MAX_INSNS : 0;
        uint64_t done = ((uint64_t (*)(qec64_cpu *, uint64_t))(void *)b->code)(c, limit);
        j->last_exit_pc = c->pc;
        c->instructions += done;
        left = done >= left ? 0 : left - done;
        if (c->pc == pc && done == 0) {
            /* left before its first instruction: the interpreter runs that one */
            j->interpreted++;
            j->last_exit = NULL;
            count_single(c, j);
            uint64_t before = c->instructions;
            enum qec64_status st = qec64_run(c, 1);
            uint64_t n = c->instructions - before;
            left = n >= left ? 0 : left - (n ? n : 1);
            if (st != QEC64_RUNNING) return st;
        }
    }
    c->budget = 0;
    return QEC64_RUNNING;
}
#else
enum qec64_status qec64_jit_run(qec64_cpu *c, uint64_t max) { return qec64_run(c, max); }
void qec64_jit_free(qec64_cpu *c) { (void)c; }
#endif
