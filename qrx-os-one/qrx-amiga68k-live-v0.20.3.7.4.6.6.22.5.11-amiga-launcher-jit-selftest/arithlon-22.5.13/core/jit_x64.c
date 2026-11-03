/* Arithlon 22.5.13 - 68k -> x86-64 dynamic recompiler.
 *
 * Translation unit: a basic block of up to 64 68k instructions, ending at a
 * branch, jump, return, TRAP or an instruction the translator cannot follow.
 *
 * Host registers inside translated code:
 *   rbx = m68k state (D0-D7, A0-A7, PC, CCR bytes ...), r12 = guest memory base,
 *   eax/ecx/edx = scratch.  Guest registers stay in the state structure; x86-64
 *   operates on them directly as memory operands.  Guest memory is big endian:
 *   loads/stores use MOVBE.
 * Condition codes: the 68k N Z V C flags map one-to-one onto x86 SF ZF OF CF
 *   for ADD/SUB/CMP/AND/OR/EOR/NEG/TST/MOVE (CMP dst,src: x86 "cmp dst,src").
 *   A backward liveness pass per block decides which flags must be stored
 *   (SETcc into the CCR bytes); a Bcc right after the flag-setting instruction
 *   branches on the x86 flags directly.
 * Everything not translated natively (FPU, division, bit fields, BCD, ...)
 *   calls the reference interpreter m68k_step() for that one instruction and
 *   continues in translated code; control-flow instructions handled that way
 *   end the block.
 * Direct block exits are chained (patched JMP to the successor block); indirect
 *   exits (RTS, JMP (An)) return to the C dispatcher which looks the target up. */
#define _GNU_SOURCE
#include "jit_x64.h"
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <cpuid.h>

#define CODE_SIZE   (64u << 20)
#define HASH_BITS   16
#define MAX_INSN    64
#define ICACHE_BITS 16

enum { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7, R12 = 12 };
enum { FC = 1, FV = 2, FZ = 4, FN = 8, FX = 16, FALL = 31 };

typedef struct block { uint32_t pc; uint8_t *code; struct block *next; } block;

struct jit {
    uint8_t *code, *p, *end;
    block *hash[1u << HASH_BITS];
    block *pool;
    size_t npool, maxpool;
    int flush_pending;
    uint8_t *(*enter)(m68k *c, uint8_t *mem, uint8_t *code);
    struct icache { uint32_t pc, pad; uint8_t *code; } *icache;   /* indirect targets, probed by translated code */
    uint64_t *prof;                                               /* ARITHLON_PROF: interpreted opcodes */
    /* statistics */
    unsigned long blocks, native, fallback, flushes;
    unsigned long long dispatches;
};

/* ================================================================== x86-64 encoder */
typedef struct { int base, index, scale; int32_t disp; } M;     /* [base + index*2^scale + disp] */
static M mm(int base, int32_t disp) { M m = { base, -1, 0, disp }; return m; }
static M mx(int base, int index, int32_t disp) { M m = { base, index, 0, disp }; return m; }

static void B(jit_t *j, uint8_t b) { *j->p++ = b; }
static void D32(jit_t *j, uint32_t v) { memcpy(j->p, &v, 4); j->p += 4; }
static void D16(jit_t *j, uint16_t v) { memcpy(j->p, &v, 2); j->p += 2; }

/* prefix(66) REX opcode modrm [sib] [disp]; rm = register (rmr >= 0) or memory */
static void enc(jit_t *j, int sz, const uint8_t *op, int nop, int reg, int rmr, const M *m, int w64)
{
    int rex = w64 ? 8 : 0, i;
    if (sz == 2) B(j, 0x66);
    if (reg & 8) rex |= 4;
    if (rmr >= 0) { if (rmr & 8) rex |= 1; }
    else { if (m->base & 8) rex |= 1; if (m->index >= 0 && (m->index & 8)) rex |= 2; }
    if (rex) B(j, (uint8_t)(0x40 | rex));
    for (i = 0; i < nop; i++) B(j, op[i]);
    if (rmr >= 0) { B(j, (uint8_t)(0xC0 | (reg & 7) << 3 | (rmr & 7))); return; }
    {
        int mod = m->disp == 0 && (m->base & 7) != 5 ? 0 : (m->disp >= -128 && m->disp <= 127) ? 1 : 2;
        if (m->index < 0 && (m->base & 7) != 4) B(j, (uint8_t)(mod << 6 | (reg & 7) << 3 | (m->base & 7)));
        else {
            int idx = m->index < 0 ? 4 : m->index;
            B(j, (uint8_t)(mod << 6 | (reg & 7) << 3 | 4));
            B(j, (uint8_t)(m->scale << 6 | (idx & 7) << 3 | (m->base & 7)));
        }
        if (mod == 1) B(j, (uint8_t)(int8_t)m->disp); else if (mod == 2) D32(j, (uint32_t)m->disp);
    }
}
#define OP1(o) (const uint8_t[]){ o }, 1
#define OP2(a, b) (const uint8_t[]){ a, b }, 2
#define OP3(a, b, c) (const uint8_t[]){ a, b, c }, 3

/* sized "op r/m, r" style instructions with 8-bit variant opcode o8 and o32 */
static void rm_op(jit_t *j, int sz, uint8_t o8, uint8_t o32, int reg, int rmr, const M *m)
{
    uint8_t o = sz == 1 ? o8 : o32;
    enc(j, sz, &o, 1, reg, rmr, m, 0);
}
static void mov_r_m(jit_t *j, int sz, int r, M m) { rm_op(j, sz, 0x8A, 0x8B, r, -1, &m); }
static void mov_m_r(jit_t *j, int sz, M m, int r) { rm_op(j, sz, 0x88, 0x89, r, -1, &m); }
static void mov_r_r(jit_t *j, int r, int s) { enc(j, 4, OP1(0x89), s, r, NULL, 0); }
static void mov_r_imm(jit_t *j, int r, uint32_t v) { if (r & 8) B(j, 0x41); B(j, (uint8_t)(0xB8 + (r & 7))); D32(j, v); }
static void mov_m_imm(jit_t *j, int sz, M m, uint32_t v)
{
    rm_op(j, sz, 0xC6, 0xC7, 0, -1, &m);
    if (sz == 1) B(j, (uint8_t)v); else if (sz == 2) D16(j, (uint16_t)v); else D32(j, v);
}
/* 0F B6/B7 (movzx), 0F BE/BF (movsx); 32-bit sources are a plain mov */
static void ext_op(jit_t *j, int sz, uint8_t o8, int r, int rmr, const M *m)
{
    uint8_t op[2] = { 0x0F, (uint8_t)(sz == 1 ? o8 : o8 + 1) };
    enc(j, 4, op, 2, r, rmr, m, 0);
}
static void movzx_r_m(jit_t *j, int sz, int r, M m) { if (sz == 4) mov_r_m(j, 4, r, m); else ext_op(j, sz, 0xB6, r, -1, &m); }
static void movsx_r_m(jit_t *j, int sz, int r, M m) { if (sz == 4) mov_r_m(j, 4, r, m); else ext_op(j, sz, 0xBE, r, -1, &m); }
static void movzx_r_r(jit_t *j, int sz, int r, int s) { if (sz != 4) ext_op(j, sz, 0xB6, r, s, NULL); }
static void movsx_r_r(jit_t *j, int sz, int r, int s) { if (sz != 4) ext_op(j, sz, 0xBE, r, s, NULL); }
static void lea(jit_t *j, int r, M m) { enc(j, 4, OP1(0x8D), r, -1, &m, 0); }
/* guest memory (big endian) at [r12 + rcx + disp] */
static void ld_guest(jit_t *j, int sz, int r, int32_t disp)
{
    M m = mx(R12, RCX, disp);
    if (sz == 1) { enc(j, 4, OP2(0x0F, 0xB6), r, -1, &m, 0); return; }
    enc(j, sz, OP3(0x0F, 0x38, 0xF0), r, -1, &m, 0);
    if (sz == 2) movzx_r_r(j, 2, r, r);
}
static void st_guest(jit_t *j, int sz, int r, int32_t disp)
{
    M m = mx(R12, RCX, disp);
    if (sz == 1) { mov_m_r(j, 1, m, r); return; }
    enc(j, sz, OP3(0x0F, 0x38, 0xF1), r, -1, &m, 0);
}
/* ALU: 0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp */
static void alu_m_r(jit_t *j, int sz, int a, M m, int r) { rm_op(j, sz, (uint8_t)(a * 8), (uint8_t)(a * 8 + 1), r, -1, &m); }
static void alu_r_r(jit_t *j, int sz, int a, int d, int s) { rm_op(j, sz, (uint8_t)(a * 8), (uint8_t)(a * 8 + 1), s, d, NULL); }
static void alu_rm_imm(jit_t *j, int sz, int a, int rmr, M m, uint32_t v)
{
    if (sz == 1) { enc(j, 1, OP1(0x80), a, rmr, &m, 0); B(j, (uint8_t)v); return; }
    if ((int32_t)v >= -128 && (int32_t)v <= 127 && (sz == 4 || (int16_t)v == (int32_t)v)) { enc(j, sz, OP1(0x83), a, rmr, &m, 0); B(j, (uint8_t)v); return; }
    enc(j, sz, OP1(0x81), a, rmr, &m, 0);
    if (sz == 2) D16(j, (uint16_t)v); else D32(j, v);
}
static void test_r_r(jit_t *j, int sz, int r) { rm_op(j, sz, 0x84, 0x85, r, r, NULL); }
static void unop(jit_t *j, int sz, int digit, int rmr, M m) { rm_op(j, sz, 0xF6, 0xF7, digit, rmr, &m); }   /* 2 not, 3 neg, 4 mul */
static void shift_imm(jit_t *j, int sz, int digit, int rmr, M m, int cnt) { rm_op(j, sz, 0xC0, 0xC1, digit, rmr, &m); B(j, (uint8_t)cnt); }
static void setcc_m(jit_t *j, int cc, M m) { enc(j, 1, OP2(0x0F, (uint8_t)(0x90 + cc)), 0, -1, &m, 0); }
static void setcc_r(jit_t *j, int cc, int r) { enc(j, 1, OP2(0x0F, (uint8_t)(0x90 + cc)), 0, r, NULL, 0); }
static uint8_t *jcc32(jit_t *j, int cc) { B(j, 0x0F); B(j, (uint8_t)(0x80 + cc)); D32(j, 0); return j->p - 4; }
static uint8_t *jmp32(jit_t *j) { B(j, 0xE9); D32(j, 0); return j->p - 4; }
static void patch(uint8_t *at, uint8_t *to) { int32_t r = (int32_t)(to - (at + 4)); memcpy(at, &r, 4); }
static void call_abs(jit_t *j, void *fn) { B(j, 0x48); B(j, 0xB8); { uint64_t a = (uint64_t)fn; memcpy(j->p, &a, 8); j->p += 8; } B(j, 0xFF); B(j, 0xD0); }
static void ret(jit_t *j) { B(j, 0xC3); }

/* state field operands */
#define ST(f) mm(RBX, (int32_t)offsetof(m68k, f))
static M Dn(int n) { return mm(RBX, (int32_t)(offsetof(m68k, d) + 4u * (unsigned)n)); }
static M An(int n) { return mm(RBX, (int32_t)(offsetof(m68k, a) + 4u * (unsigned)n)); }
static M FLAG(int f)
{
    switch (f) {
    case FC: return ST(c);
    case FV: return ST(v);
    case FZ: return ST(z);
    case FN: return ST(n);
    default: return ST(x);
    }
}

/* ================================================================== decoding */
static inline uint16_t g16(const m68k *c, uint32_t a) { uint16_t v; memcpy(&v, c->mem + a, 2); return __builtin_bswap16(v); }
static inline uint32_t g32(const m68k *c, uint32_t a) { uint32_t v; memcpy(&v, c->mem + a, 4); return __builtin_bswap32(v); }

typedef struct {
    int mode, reg;
    int32_t disp;           /* modes 5, 6, 7.2/7.3 (absolute base for PC modes) */
    uint32_t val;           /* absolute address / immediate */
    int xr, xlong, xscale;  /* index register (-1 none) */
    int bs;                 /* full format: base register suppressed */
} jea;

enum { K_FALLBACK, K_FALLBACK_END, K_MOVE, K_MOVEQ, K_LEA, K_PEA, K_CLR, K_TST, K_NOT, K_NEG, K_EXT, K_SWAP,
       K_ALU, K_ALU_A, K_SHIFT, K_BCC, K_DBCC, K_SCC, K_JSR, K_JMP, K_RTS, K_LINK, K_UNLK, K_MOVEM,
       K_MULW, K_MULL, K_ADDQ_A, K_SHIFTR, K_BFEXT };

typedef struct {
    uint32_t pc, next;
    int kind, sz, op, reg, cc, cnt, left, sgn;
    jea s, d;
    uint32_t target;
    uint8_t defs, uses, need;
} jins;

/* extension words of an effective address: length in bytes; 0 = not translatable natively */
static int ea_ext(const m68k *c, uint32_t at, int mode, int reg, int sz, jea *e, int *native)
{
    e->mode = mode; e->reg = reg; e->xr = -1; e->bs = 0;
    switch (mode) {
    case 0: case 1: case 2: case 3: case 4: return 0;
    case 5: e->disp = (int16_t)g16(c, at); return 2;
    case 6: case 7: {
        uint16_t ext;
        if (mode == 7) {
            switch (reg) {
            case 0: e->val = (uint32_t)(int16_t)g16(c, at); return 2;
            case 1: e->val = g32(c, at); return 4;
            case 2: e->val = at + (uint32_t)(int16_t)g16(c, at); return 2;
            case 4: e->val = sz == 1 ? (g16(c, at) & 0xFFu) : sz == 2 ? g16(c, at) : g32(c, at); return sz == 4 ? 4 : 2;
            case 3: break;
            default: *native = 0; return -1;
            }
        }
        ext = g16(c, at);
        if (!(ext & 0x100)) {
            e->xr = (ext >> 12) & 15; e->xlong = (ext >> 11) & 1; e->xscale = (ext >> 9) & 3;
            e->disp = (int8_t)ext;
            if (mode == 7) e->val = at;            /* PC base = address of the extension word */
            return 2;
        }
        {                                          /* 68020 full format */
            int n = 2, bd = (ext >> 4) & 3, od = ext & 3;
            int32_t bdv = 0;
            if (bd == 2) { bdv = (int16_t)g16(c, at + 2); n += 2; } else if (bd == 3) { bdv = (int32_t)g32(c, at + 2); n += 4; }
            if ((ext & 7) == 0 && bd) {                /* no memory indirection: native */
                e->bs = (ext & 0x80) != 0;
                if (!(ext & 0x40)) { e->xr = (ext >> 12) & 15; e->xlong = (ext >> 11) & 1; e->xscale = (ext >> 9) & 3; }
                e->disp = bdv;
                if (mode == 7) e->val = at;
                return n;
            }
            *native = 0;                           /* memory indirect: interpreter */
            n += od == 2 ? 2 : od == 3 ? 4 : 0;
            return n;
        }
    }
    default: return 0;
    }
}

static const int SZ[4] = { 1, 2, 4, 0 };

/* length of an instruction the JIT leaves to the interpreter; 0 = ends the block */
static int fallback_len(const m68k *c, uint32_t pc, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7, nat = 1, n;
    jea e;
#define EAL(sz) (n = ea_ext(c, pc + (uint32_t)len, mode, reg, (sz), &e, &nat), n < 0 ? -1000 : n)
    int len = 2;
    switch (op >> 12) {
    case 0x0:
        if (op & 0x100) { if (mode == 1) return 0; return len + EAL(1); }
        if (op == 0x003C || op == 0x023C || op == 0x0A3C) return 4;
        if ((op & 0xF9C0) == 0x08C0 && (op & 0x600)) { if ((op & 0x3F) == 0x3C) return 0; len += 2; return len + EAL(4); }
        if (((op >> 9) & 7) == 4) { len += 2; return len + EAL(1); }
        if (!SZ[(op >> 6) & 3] || ((op >> 9) & 7) == 7) return 0;
        len += SZ[(op >> 6) & 3] == 4 ? 4 : 2;
        return len + EAL(SZ[(op >> 6) & 3]);
    case 0x1: case 0x2: case 0x3: {
        static const int msz[4] = { 0, 1, 4, 2 };
        int sz = msz[(op >> 12) & 3], dl;
        len += EAL(sz);
        dl = ea_ext(c, pc + (uint32_t)len, (op >> 6) & 7, (op >> 9) & 7, sz, &e, &nat);
        return dl < 0 ? 0 : len + dl; }
    case 0x4:
        if ((op & 0xFF80) == 0x4C00) { len += 2; return len + EAL(4); }               /* MULx.L/DIVx.L */
        if ((op & 0xFFF8) == 0x4808) return 6;                                           /* LINK.L */
        if ((op & 0xF140) == 0x4100) return len + EAL((op & 0x80) ? 2 : 4);              /* CHK */
        if ((op & 0xFFC0) == 0x4800) return len + EAL(1);                                /* NBCD */
        if ((op & 0xFFC0) == 0x42C0 || (op & 0xFFC0) == 0x44C0) return len + EAL(2);     /* MOVE from/to CCR */
        if ((op & 0xFFC0) == 0x4AC0 && op != 0x4AFC) return len + EAL(1);                /* TAS */
        if ((op & 0xFF00) == 0x4000 && SZ[(op >> 6) & 3]) return len + EAL(SZ[(op >> 6) & 3]);   /* NEGX */
        return 0;
    case 0x5:
        if (SZ[(op >> 6) & 3] || mode == 1 || (mode == 7 && reg >= 2)) return 0;       /* only Scc reaches here */
        return len + EAL(1);
    case 0x8: case 0x9: case 0xB: case 0xC: case 0xD: {
        int opm = (op >> 6) & 7, sz;
        if ((op >> 12) == 0x8 || (op >> 12) == 0xC) {
            if ((op & 0x1F0) == 0x100 || (op & 0x130) == 0x100) return 2;                /* ABCD/SBCD/EXG */
            if ((op & 0x1F0) == 0x140 || (op & 0x1F0) == 0x180) return 0;                /* PACK/UNPK */
            if ((op & 0xC0) == 0xC0) return len + EAL(2);                                 /* MUL/DIV .W */
        }
        if (opm == 3 || opm == 7) sz = opm == 3 ? 2 : 4; else sz = SZ[opm & 3];
        if (opm >= 4 && opm != 7 && mode < 2) return 2;                                  /* ADDX/SUBX/CMPM */
        return len + EAL(sz);
    }
    case 0xE:
        if ((op & 0xF8C0) == 0xE8C0) { len += 2; return len + EAL(4); }                 /* bit field */
        if ((op & 0xC0) == 0xC0) return len + EAL(2);
        return 2;
    case 0xF:
        if ((op & 0xFFC0) == 0xF200) {
            uint16_t ext = g16(c, pc + 2);
            int opc = ext >> 13, fmt = (ext >> 10) & 7;
            static const int fsz[8] = { 4, 4, 12, 12, 2, 8, 1, 0 };
            len = 4;
            if (opc == 0) return 4;
            if (opc == 2 || opc == 3) {
                if (opc == 2 && fmt == 7) return 4;
                if (mode == 7 && reg == 4) return 4 + (fsz[fmt] < 2 ? 2 : fsz[fmt]);
                return len + EAL(4);
            }
            if (opc == 4 || opc == 5) {
                int cnt = ((ext >> 12) & 1) + ((ext >> 11) & 1) + ((ext >> 10) & 1);
                if (mode == 7 && reg == 4) return 4 + 4 * cnt;
                return len + EAL(4);
            }
            return len + EAL(4);                                                         /* FMOVEM */
        }
        if ((op & 0xFFC0) == 0xF240 && mode != 1 && !(mode == 7 && reg >= 2)) { len = 4; return len + EAL(1); }   /* FScc */
        return 0;
    default: return 0;
    }
#undef EAL
}

static int x86cc(int cc)
{
    static const int t[16] = { -1, -1, 7, 6, 3, 2, 5, 4, 1, 0, 9, 8, 13, 12, 15, 14 };
    return t[cc & 15];
}
static uint8_t cc_uses(int cc)
{
    switch (cc & 15) {
    case 0: case 1: return 0;
    case 2: case 3: return FC | FZ;
    case 4: case 5: return FC;
    case 6: case 7: return FZ;
    case 8: case 9: return FV;
    case 10: case 11: return FN;
    case 12: case 13: return FN | FV;
    default: return FN | FV | FZ;
    }
}

/* decode one instruction at pc */
static void decode(const m68k *c, uint32_t pc, jins *I)
{
    uint16_t op = g16(c, pc);
    int mode = (op >> 3) & 7, reg = op & 7, nat = 1, n;
    uint32_t at = pc + 2;
    memset(I, 0, sizeof(*I));
    I->pc = pc; I->op = op;
#define SRC(m, r, sz) do { n = ea_ext(c, at, (m), (r), (sz), &I->s, &nat); if (n < 0) goto fb; at += (uint32_t)n; } while (0)
#define DST(m, r, sz) do { n = ea_ext(c, at, (m), (r), (sz), &I->d, &nat); if (n < 0) goto fb; at += (uint32_t)n; } while (0)
    switch (op >> 12) {
    case 0x1: case 0x2: case 0x3: {
        static const int msz[4] = { 0, 1, 4, 2 };
        int sz = msz[(op >> 12) & 3], dm = (op >> 6) & 7, dr = (op >> 9) & 7;
        if (dm == 7 && dr >= 2) goto fb;
        if (sz == 1 && (mode == 1 || dm == 1)) goto fb;
        I->sz = sz; SRC(mode, reg, sz); DST(dm, dr, sz);
        if (!nat) goto fb;
        I->kind = K_MOVE; I->defs = dm == 1 ? 0 : (FN | FZ | FV | FC);
        break; }
    case 0x7:
        if (op & 0x100) goto fb;
        I->kind = K_MOVEQ; I->reg = (op >> 9) & 7; I->s.val = (uint32_t)(int8_t)op; I->defs = FN | FZ | FV | FC;
        break;
    case 0x4:
        if (op == 0x4E71) { I->kind = K_MOVE; I->sz = 0; break; }                    /* NOP: K_MOVE with sz 0 = nothing */
        if (op == 0x4E75) { I->kind = K_RTS; break; }
        if ((op & 0xFFF8) == 0x4E50 && reg != 7) { I->kind = K_LINK; I->reg = reg; I->s.val = (uint32_t)(int16_t)g16(c, at); at += 2; break; }
        if ((op & 0xFFF8) == 0x4E58 && reg != 7) { I->kind = K_UNLK; I->reg = reg; break; }
        if ((op & 0xFFC0) == 0x4E80 || (op & 0xFFC0) == 0x4EC0) {                    /* JSR / JMP */
            if (mode < 2 || mode == 3 || mode == 4) goto fb;
            SRC(mode, reg, 4); if (!nat) goto fb;
            I->kind = (op & 0x40) ? K_JMP : K_JSR;
            break;
        }
        if ((op & 0xF1C0) == 0x41C0 && mode) {                                       /* LEA */
            if (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg == 4)) goto fb;
            SRC(mode, reg, 4); if (!nat) goto fb;
            I->kind = K_LEA; I->reg = (op >> 9) & 7; break;
        }
        if ((op & 0xFFC0) == 0x4840 && mode >= 2) {                                  /* PEA */
            if (mode == 3 || mode == 4 || (mode == 7 && reg == 4)) goto fb;
            SRC(mode, reg, 4); if (!nat) goto fb;
            I->kind = K_PEA; break;
        }
        if ((op & 0xFFF8) == 0x4840) { I->kind = K_SWAP; I->reg = reg; I->defs = FN | FZ | FV | FC; break; }
        if ((op & 0xFFF8) == 0x4880 || (op & 0xFFF8) == 0x48C0 || (op & 0xFFF8) == 0x49C0) {
            I->kind = K_EXT; I->reg = reg; I->cnt = (op >> 6) & 7; I->defs = FN | FZ | FV | FC; break;  /* cnt: 2 ext.w, 3 ext.l, 7 extb.l */
        }
        if ((op & 0xFB80) == 0x4880 && mode >= 2) {                                  /* MOVEM */
            int sz = (op & 0x40) ? 4 : 2;
            I->cnt = g16(c, at); at += 2;
            if ((op & 0x400) && mode == 4) goto fb;
            if (!(op & 0x400) && mode == 3) goto fb;
            if (mode != 3 && mode != 4) { if (mode == 7 && reg == 4) goto fb; SRC(mode, reg, 4); if (!nat) goto fb; }
            else { I->s.mode = mode; I->s.reg = reg; }
            if (mode == 4 && ((I->cnt >> (7 - reg)) & 1)) goto fb;                   /* address register in the list */
            if (mode == 3 && ((I->cnt >> (8 + reg)) & 1)) goto fb;
            I->kind = K_MOVEM; I->sz = sz; I->left = (op & 0x400) != 0; break;
        }
        if ((op & 0xFF80) == 0x4C00) {                                               /* MULx.L 32-bit result */
            uint16_t ext = g16(c, at);
            if ((op & 0x40) || (ext & 0x400) || (ext & 0x83F8)) goto fb;
            at += 2; SRC(mode, reg, 4); if (!nat || mode == 1) goto fb;
            I->kind = K_MULL; I->reg = (ext >> 12) & 7; I->sgn = (ext & 0x800) != 0; I->defs = FN | FZ | FV | FC; break;
        }
        {
            int sz = SZ[(op >> 6) & 3], hi = (op >> 8) & 15;
            if (sz && (hi == 2 || hi == 4 || hi == 6 || hi == 0xA)) {
                if ((mode == 1 && !(hi == 0xA && sz > 1)) || (mode == 7 && reg >= 2 && !(hi == 0xA && reg <= 4))) goto fb;
                if (hi != 0xA && mode == 7) goto fb;
                I->sz = sz; I->d.mode = mode; SRC(mode, reg, sz); if (!nat) goto fb;
                I->d = I->s;
                I->kind = hi == 2 ? K_CLR : hi == 4 ? K_NEG : hi == 6 ? K_NOT : K_TST;
                I->defs = hi == 4 ? FALL : (FN | FZ | FV | FC);
                break;
            }
        }
        goto fb;
    case 0x5: {
        int sz = SZ[(op >> 6) & 3];
        if (!sz) {
            if (mode == 1) {                                                         /* DBcc */
                I->kind = K_DBCC; I->reg = reg; I->cc = (op >> 8) & 15;
                I->target = at + (uint32_t)(int16_t)g16(c, at); at += 2;
                I->uses = cc_uses(I->cc); break;
            }
            if (mode == 7 && reg >= 2) goto fb;
            SRC(mode, reg, 1); if (!nat) goto fb;
            I->kind = K_SCC; I->cc = (op >> 8) & 15; I->d = I->s; I->sz = 1; I->uses = cc_uses(I->cc); break;
        }
        I->cnt = (op >> 9) & 7; if (!I->cnt) I->cnt = 8;
        I->sz = sz;
        if (mode == 1) { I->kind = K_ADDQ_A; I->reg = reg; I->left = (op & 0x100) != 0; break; }
        if (mode == 7) goto fb;
        SRC(mode, reg, sz); if (!nat) goto fb;
        I->d = I->s;
        I->kind = K_ALU; I->op = (op & 0x100) ? 5 : 0; I->s.mode = 7; I->s.reg = 4; I->s.val = (uint32_t)I->cnt;
        I->defs = FALL; break; }
    case 0x6: {
        int32_t disp = (int8_t)op;
        uint32_t base = at;
        if ((op & 0xFF) == 0) { disp = (int16_t)g16(c, at); at += 2; }
        else if ((op & 0xFF) == 0xFF) { disp = (int32_t)g32(c, at); at += 4; }
        I->kind = K_BCC; I->cc = (op >> 8) & 15; I->target = base + (uint32_t)disp;
        I->uses = I->cc == 1 ? 0 : cc_uses(I->cc);
        break; }
    case 0x0: {                                                                      /* ORI ANDI SUBI ADDI EORI CMPI */
        int sz = SZ[(op >> 6) & 3], k = (op >> 9) & 7;
        static const int amap[8] = { 1, 4, 5, 0, -1, 6, 7, -1 };
        uint32_t imm;
        if ((op & 0x100) || !sz || amap[k] < 0 || mode == 1 || (mode == 7)) goto fb;
        imm = sz == 1 ? (g16(c, at) & 0xFFu) : sz == 2 ? g16(c, at) : g32(c, at);
        at += sz == 4 ? 4 : 2;
        SRC(mode, reg, sz); if (!nat) goto fb;
        I->d = I->s; I->s.mode = 7; I->s.reg = 4; I->s.val = imm;
        I->kind = K_ALU; I->op = amap[k]; I->sz = sz;
        I->defs = (k == 2 || k == 3) ? FALL : (FN | FZ | FV | FC);
        break; }
    case 0x8: case 0x9: case 0xB: case 0xC: case 0xD: {
        int opm = (op >> 6) & 7, dn = (op >> 9) & 7, hi = op >> 12;
        if ((hi == 0x8 || hi == 0xC) && (op & 0x1F0) == 0x100) goto fb;              /* SBCD/ABCD */
        if (hi == 0xC && (op & 0x130) == 0x100) goto fb;                              /* EXG */
        if (hi == 0x8 && ((op & 0x1F0) == 0x140 || (op & 0x1F0) == 0x180)) goto fb;
        if (hi == 0xC && (op & 0xC0) == 0xC0) {                                       /* MULU.W / MULS.W */
            SRC(mode, reg, 2); if (!nat || mode == 1) goto fb;
            I->kind = K_MULW; I->reg = dn; I->sgn = (op & 0x100) != 0; I->defs = FN | FZ | FV | FC; break;
        }
        if (hi == 0x8 && (op & 0xC0) == 0xC0) goto fb;                                /* DIV.W */
        if (opm == 3 || opm == 7) {                                                   /* ADDA SUBA CMPA */
            int sz = opm == 3 ? 2 : 4;
            if (hi == 0x8 || hi == 0xC) goto fb;
            SRC(mode, reg, sz); if (!nat) goto fb;
            I->kind = K_ALU_A; I->reg = dn; I->sz = sz; I->op = hi == 0xD ? 0 : hi == 0x9 ? 5 : 7;
            I->defs = hi == 0xB ? (FN | FZ | FV | FC) : 0; break;
        }
        {
            int sz = SZ[opm & 3], a;
            a = hi == 0x8 ? 1 : hi == 0x9 ? 5 : hi == 0xB ? (opm < 3 ? 7 : 6) : hi == 0xC ? 4 : 0;
            if (opm >= 4 && mode < 2 && !(hi == 0xB && mode == 0)) goto fb;          /* ADDX SUBX CMPM */
            if (sz == 1 && mode == 1) goto fb;
            I->sz = sz; I->op = a;
            if (opm < 4) {                                                            /* <ea> op Dn -> Dn */
                if (mode == 1 && (a == 1 || a == 4)) goto fb;
                SRC(mode, reg, sz); if (!nat) goto fb;
                I->d.mode = 0; I->d.reg = dn;
            } else {                                                                  /* Dn op <ea> -> <ea> (EOR: also Dn) */
                if (mode == 7) goto fb;
                SRC(mode, reg, sz); if (!nat) goto fb;
                I->d = I->s; I->s.mode = 0; I->s.reg = dn;
            }
            I->kind = K_ALU;
            I->defs = (a == 0 || a == 5) ? FALL : (FN | FZ | FV | FC);
        }
        break; }
    case 0xE:
        if ((op & 0xFDF8) == 0xE9C0) {                                                /* BFEXTU/BFEXTS Dn{#o:#w},Dm */
            uint16_t ext = g16(c, at);
            int off = (ext >> 6) & 31, w = ext & 31;
            if (!w) w = 32;
            if ((ext & 0x8820) || off + w > 32) goto fb;
            at += 2;
            I->kind = K_BFEXT; I->reg = reg; I->cc = (ext >> 12) & 7; I->cnt = off; I->left = w; I->sgn = (op & 0x200) != 0;
            I->defs = FN | FZ | FV | FC;
            break;
        }
        if ((op & 0xC0) != 0xC0 && (op & 0x20) && ((op >> 3) & 3) <= 1 && !(((op >> 3) & 3) == 0 && (op & 0x100))) {
            I->kind = K_SHIFTR; I->sz = SZ[(op >> 6) & 3]; I->reg = reg; I->cc = (op >> 9) & 7;   /* LSL/LSR/ASR Dc,Dn */
            I->left = (op >> 8) & 1; I->op = (op >> 3) & 3;
            I->defs = FALL; I->uses = FX;
            break;
        }
        if ((op & 0xC0) == 0xC0 || (op & 0x20)) goto fb;                              /* memory / register count */
        {
            int type = (op >> 3) & 3, sz = SZ[(op >> 6) & 3], cnt = (op >> 9) & 7;
            if (!cnt) cnt = 8;
            if (type == 2) goto fb;                                                   /* ROXL/ROXR */
            if (cnt >= sz * 8) goto fb;
            if (type == 0 && (op & 0x100) && cnt > 1) goto fb;                        /* ASL by >1: V */
            I->kind = K_SHIFT; I->sz = sz; I->reg = reg; I->cnt = cnt; I->left = (op >> 8) & 1; I->op = type;
            I->defs = type == 3 ? (FN | FZ | FV | FC) : FALL;
        }
        break;
    default: goto fb;
    }
    I->next = at;
    return;
fb:
    memset(I, 0, sizeof(*I));
    I->pc = pc; I->op = op;
    n = fallback_len(c, pc, op);
    if (n <= 0) { I->kind = K_FALLBACK_END; I->next = 0; }
    else { I->kind = K_FALLBACK; I->next = pc + (uint32_t)n; }
    I->uses = FALL; I->defs = 0;
#undef SRC
#undef DST
}

/* ================================================================== code generation */
typedef struct { jit_t *j; int intact; } gen;

static void store_flags(gen *g, uint8_t need)
{
    jit_t *j = g->j;
    if (need & FC) setcc_m(j, 2, FLAG(FC));
    if (need & FX) setcc_m(j, 2, FLAG(FX));
    if (need & FV) setcc_m(j, 0, FLAG(FV));
    if (need & FZ) setcc_m(j, 4, FLAG(FZ));
    if (need & FN) setcc_m(j, 8, FLAG(FN));
}
/* x86 EFLAGS <- CCR bytes (only when a branch needs flags that are not live in EFLAGS) */
static void load_flags(gen *g)
{
    jit_t *j = g->j;
    movzx_r_m(j, 1, RAX, FLAG(FC));
    movzx_r_m(j, 1, RCX, FLAG(FZ)); shift_imm(j, 4, 4, RCX, mm(0, 0), 6); alu_r_r(j, 4, 1, RAX, RCX);
    movzx_r_m(j, 1, RCX, FLAG(FN)); shift_imm(j, 4, 4, RCX, mm(0, 0), 7); alu_r_r(j, 4, 1, RAX, RCX);
    movzx_r_m(j, 1, RCX, FLAG(FV)); shift_imm(j, 4, 4, RCX, mm(0, 0), 11); alu_r_r(j, 4, 1, RAX, RCX);
    B(j, 0x50); B(j, 0x9D);                         /* push rax; popfq */
    g->intact = 1;
}

/* guest address of a memory operand -> ecx (no x86 flags touched) */
static void gen_addr(gen *g, const jea *e, int sz)
{
    jit_t *j = g->j;
    int step = (sz == 1 && e->reg == 7) ? 2 : sz;
    switch (e->mode) {
    case 2: mov_r_m(j, 4, RCX, An(e->reg)); return;
    case 3: mov_r_m(j, 4, RCX, An(e->reg)); lea(j, RDX, mm(RCX, step)); mov_m_r(j, 4, An(e->reg), RDX); return;
    case 4: mov_r_m(j, 4, RCX, An(e->reg)); lea(j, RCX, mm(RCX, -step)); mov_m_r(j, 4, An(e->reg), RCX); return;
    case 5: mov_r_m(j, 4, RCX, An(e->reg)); lea(j, RCX, mm(RCX, e->disp)); return;
    case 6: case 7:
        if (e->mode == 7 && e->reg != 3) { mov_r_imm(j, RCX, e->val); return; }
        if (e->bs) mov_r_imm(j, RCX, 0);
        else if (e->mode == 6) mov_r_m(j, 4, RCX, An(e->reg));
        else mov_r_imm(j, RCX, e->val);
        if (e->xr < 0) { lea(j, RCX, mm(RCX, e->disp)); return; }
        {
            M x = e->xr < 8 ? Dn(e->xr) : An(e->xr - 8), a;
            if (e->xlong) mov_r_m(j, 4, RDX, x); else movsx_r_m(j, 2, RDX, x);
            a = mx(RCX, RDX, e->disp); a.scale = e->xscale;
            lea(j, RCX, a);
        }
        return;
    default: return;
    }
}
/* operand value -> eax (zero-extended) */
static void gen_load(gen *g, const jea *e, int sz)
{
    jit_t *j = g->j;
    switch (e->mode) {
    case 0: movzx_r_m(j, sz, RAX, Dn(e->reg)); return;
    case 1: movzx_r_m(j, sz, RAX, An(e->reg)); return;
    default:
        if (e->mode == 7 && e->reg == 4) { mov_r_imm(j, RAX, e->val); return; }
        gen_addr(g, e, sz);
        ld_guest(j, sz, RAX, 0);
    }
}

/* exits ----------------------------------------------------------- */
/* direct exit: "jmp rel32" (initially to the next instruction) + store pc + return the site */
static void exit_direct(gen *g, uint32_t target)
{
    jit_t *j = g->j;
    uint8_t *site = jmp32(j);
    patch(site, j->p);
    mov_m_imm(j, 4, ST(pc), target);
    B(j, 0x48); B(j, 0xB8); { uint64_t a = (uint64_t)site; memcpy(j->p, &a, 8); j->p += 8; }   /* mov rax, site */
    ret(j);
}
static void exit_return(gen *g)
{
    jit_t *j = g->j;
    alu_r_r(j, 4, 6, RAX, RAX);         /* xor eax, eax: no exit site to chain */
    ret(j);
}
/* pc already stored: probe the indirect-target cache, jump straight to the block on a hit */
static void exit_indirect(gen *g)
{
    jit_t *j = g->j;
    uint8_t *miss;
    mov_r_m(j, 4, RAX, ST(pc));
    mov_r_r(j, RCX, RAX);
    shift_imm(j, 4, 5, RCX, mm(0, 0), 1);
    alu_rm_imm(j, 4, 4, RCX, mm(0, 0), (1u << ICACHE_BITS) - 1);
    shift_imm(j, 4, 4, RCX, mm(0, 0), 4);
    B(j, 0x48); B(j, 0xBA); { uint64_t a = (uint64_t)j->icache; memcpy(j->p, &a, 8); j->p += 8; }   /* mov rdx, icache */
    alu_m_r(j, 4, 7, mx(RDX, RCX, 0), RAX);
    miss = jcc32(j, 5);
    { M m = mx(RDX, RCX, 8); enc(j, 4, OP1(0xFF), 4, -1, &m, 0); }                                  /* jmp [rdx+rcx+8] */
    patch(miss, j->p);
    exit_return(g);
}
static void push_eax(gen *g)            /* -(A7) = eax, ecx clobbered */
{
    jit_t *j = g->j;
    mov_r_m(j, 4, RCX, An(7)); lea(j, RCX, mm(RCX, -4)); mov_m_r(j, 4, An(7), RCX);
    st_guest(j, 4, RAX, 0);
}

static void gen_fallback(gen *g, const jins *I)
{
    jit_t *j = g->j;
    mov_m_imm(j, 4, ST(pc), I->pc);
    B(j, 0x48); B(j, 0x89); B(j, 0xDF);  /* mov rdi, rbx */
    if (j->prof) { B(j, 0x48); B(j, 0xB8); { uint64_t a = (uint64_t)&j->prof[I->op]; memcpy(j->p, &a, 8); j->p += 8; } B(j, 0x48); B(j, 0xFF); B(j, 0x00); }
    call_abs(j, (void *)m68k_step);
    g->intact = 0;
    if (I->kind == K_FALLBACK_END) exit_return(g);
}

/* one native instruction; returns 1 when it ended the block */
static int gen_insn(gen *g, const jins *I)
{
    jit_t *j = g->j;
    int sz = I->sz;
    switch (I->kind) {
    case K_MOVE:
        if (!sz) return 0;                                         /* NOP */
        gen_load(g, &I->s, sz);
        if (I->d.mode == 1) {                                      /* MOVEA: sign extend, no flags */
            if (sz == 2) movsx_r_r(j, 2, RAX, RAX);
            mov_m_r(j, 4, An(I->d.reg), RAX);
            return 0;
        }
        if (I->need) { test_r_r(j, sz, RAX); g->intact = 1; store_flags(g, I->need); }
        else g->intact = 0;                                        /* flags dead: EFLAGS no longer mirror the CCR */
        if (I->d.mode == 0) mov_m_r(j, sz, Dn(I->d.reg), RAX);
        else { gen_addr(g, &I->d, sz); st_guest(j, sz, RAX, 0); }
        return 0;
    case K_MOVEQ:
        mov_r_imm(j, RAX, I->s.val); mov_m_r(j, 4, Dn(I->reg), RAX);
        test_r_r(j, 4, RAX); g->intact = 1; store_flags(g, I->need);
        return 0;
    case K_LEA: gen_addr(g, &I->s, 4); mov_m_r(j, 4, An(I->reg), RCX); return 0;
    case K_PEA: gen_addr(g, &I->s, 4); mov_r_r(j, RAX, RCX); push_eax(g); return 0;
    case K_SWAP:
        mov_r_m(j, 4, RAX, Dn(I->reg)); shift_imm(j, 4, 0, RAX, mm(0, 0), 16);   /* rol eax,16 */
        mov_m_r(j, 4, Dn(I->reg), RAX); test_r_r(j, 4, RAX); g->intact = 1; store_flags(g, I->need);
        return 0;
    case K_EXT:
        if (I->cnt == 2) { movsx_r_m(j, 1, RAX, Dn(I->reg)); mov_m_r(j, 2, Dn(I->reg), RAX); test_r_r(j, 2, RAX); }
        else { movsx_r_m(j, I->cnt == 3 ? 2 : 1, RAX, Dn(I->reg)); mov_m_r(j, 4, Dn(I->reg), RAX); test_r_r(j, 4, RAX); }
        g->intact = 1; store_flags(g, I->need);
        return 0;
    case K_CLR:
        if (I->d.mode == 0) mov_m_imm(j, sz, Dn(I->d.reg), 0);
        else { gen_addr(g, &I->d, sz); alu_r_r(j, 4, 6, RAX, RAX); st_guest(j, sz, RAX, 0); }
        alu_r_r(j, 4, 6, RAX, RAX); g->intact = 1; store_flags(g, I->need);
        return 0;
    case K_TST:
        gen_load(g, &I->s, sz); test_r_r(j, sz, RAX); g->intact = 1; store_flags(g, I->need);
        return 0;
    case K_NOT: case K_NEG:
        if (I->d.mode == 0) {
            unop(j, sz, I->kind == K_NOT ? 2 : 3, -1, Dn(I->d.reg));
            if (I->kind == K_NOT) { movzx_r_m(j, sz, RAX, Dn(I->d.reg)); test_r_r(j, sz, RAX); }
        } else {
            gen_addr(g, &I->d, sz); ld_guest(j, sz, RAX, 0);
            unop(j, sz, I->kind == K_NOT ? 2 : 3, RAX, mm(0, 0));
            st_guest(j, sz, RAX, 0);
            if (I->kind == K_NOT) test_r_r(j, sz, RAX);
        }
        g->intact = 1; store_flags(g, I->need);
        return 0;
    case K_ALU: {
        int a = I->op, simm = I->s.mode == 7 && I->s.reg == 4;
        if (I->d.mode == 0) {                                      /* destination Dn */
            if (simm) alu_rm_imm(j, sz, a, -1, Dn(I->d.reg), I->s.val);
            else { gen_load(g, &I->s, sz); alu_m_r(j, sz, a, Dn(I->d.reg), RAX); }
        } else {                                                   /* destination memory */
            if (!simm) movzx_r_m(j, sz, RSI, Dn(I->s.reg));        /* source Dn (gen_addr leaves esi alone) */
            gen_addr(g, &I->d, sz);
            ld_guest(j, sz, RAX, 0);
            if (simm) alu_rm_imm(j, sz, a, RAX, mm(0, 0), I->s.val);
            else alu_r_r(j, sz, a, RAX, RSI);
            if (a != 7) st_guest(j, sz, RAX, 0);
        }
        g->intact = 1; store_flags(g, I->need);
        return 0; }
    case K_ALU_A:
        gen_load(g, &I->s, sz);
        if (sz == 2) movsx_r_r(j, 2, RAX, RAX);
        if (I->op == 0) { mov_r_m(j, 4, RDX, An(I->reg)); lea(j, RDX, mx(RDX, RAX, 0)); mov_m_r(j, 4, An(I->reg), RDX); }
        else if (I->op == 5) { alu_m_r(j, 4, 5, An(I->reg), RAX); g->intact = 0; }
        else { alu_m_r(j, 4, 7, An(I->reg), RAX); g->intact = 1; store_flags(g, I->need); }
        return 0;
    case K_ADDQ_A:
        mov_r_m(j, 4, RAX, An(I->reg)); lea(j, RAX, mm(RAX, I->left ? -I->cnt : I->cnt)); mov_m_r(j, 4, An(I->reg), RAX);
        return 0;
    case K_SHIFT: {
        static const int dig[4][2] = { { 7, 4 }, { 5, 4 }, { 0, 0 }, { 1, 0 } };   /* [type][left]: sar/shl, shr/shl, -, ror/rol */
        int d = dig[I->op][I->left];
        shift_imm(j, sz, d, -1, Dn(I->reg), I->cnt);
        if (I->op == 3) {                                          /* ROL/ROR: C = last bit, V = 0 */
            if (I->need & FC) setcc_m(j, 2, FLAG(FC));
            if (I->need & (FN | FZ)) { movzx_r_m(j, sz, RAX, Dn(I->reg)); test_r_r(j, sz, RAX); g->intact = 1; store_flags(g, I->need & (FN | FZ)); }
        } else {
            g->intact = 1;
            store_flags(g, I->need & (FC | FX | FZ | FN | (I->op == 0 && I->left ? FV : 0)));
        }
        if ((I->need & FV) && !(I->op == 0 && I->left)) mov_m_imm(j, 1, FLAG(FV), 0);
        g->intact = 0;
        return 0; }
    case K_SHIFTR: {
        static const int dig[2][2] = { { 7, 4 }, { 5, 4 } };
        uint8_t *jz, *jae, *d1, *d2;
        M dn = Dn(I->reg);
        mov_r_m(j, 4, RCX, Dn(I->cc)); alu_rm_imm(j, 4, 4, RCX, mm(0, 0), 63);
        jz = jcc32(j, 4);
        alu_rm_imm(j, 4, 7, RCX, mm(0, 0), (uint32_t)(sz * 8));
        jae = jcc32(j, 3);
        rm_op(j, sz, 0xD2, 0xD3, dig[I->op][I->left], -1, &dn);     /* shift [Dn], cl */
        store_flags(g, I->need & (FC | FX | FZ | FN));
        if (I->need & FV) mov_m_imm(j, 1, FLAG(FV), 0);
        d1 = jmp32(j);
        patch(jz, j->p);                                           /* count 0: C = V = 0, X kept */
        movzx_r_m(j, sz, RAX, dn); test_r_r(j, sz, RAX);
        store_flags(g, I->need & (FC | FV | FZ | FN));
        d2 = jmp32(j);
        patch(jae, j->p);                                          /* count >= width: interpreter */
        mov_m_imm(j, 4, ST(pc), I->pc);
        B(j, 0x48); B(j, 0x89); B(j, 0xDF);
        call_abs(j, (void *)m68k_step);
        patch(d1, j->p); patch(d2, j->p);
        g->intact = 0;
        return 0; }
    case K_BFEXT: {
        int off = I->cnt, w = I->left;
        mov_r_m(j, 4, RAX, Dn(I->reg));
        if (off) shift_imm(j, 4, 4, RAX, mm(0, 0), off);
        if (w < 32) alu_rm_imm(j, 4, 4, RAX, mm(0, 0), ~((1u << (32 - w)) - 1));
        else test_r_r(j, 4, RAX);
        if (w == 32 && off) test_r_r(j, 4, RAX);
        g->intact = 1; store_flags(g, I->need);
        if (w < 32) shift_imm(j, 4, I->sgn ? 7 : 5, RAX, mm(0, 0), 32 - w);
        mov_m_r(j, 4, Dn(I->cc), RAX);
        g->intact = 0;
        return 0; }
    case K_MULW:
        gen_load(g, &I->s, 2);
        if (I->sgn) { movsx_r_r(j, 2, RAX, RAX); movsx_r_m(j, 2, RDX, Dn(I->reg)); }
        else movzx_r_m(j, 2, RDX, Dn(I->reg));
        enc(j, 4, OP2(0x0F, 0xAF), RAX, RDX, NULL, 0);             /* imul eax, edx */
        mov_m_r(j, 4, Dn(I->reg), RAX); test_r_r(j, 4, RAX); g->intact = 1; store_flags(g, I->need);
        return 0;
    case K_MULL:
        gen_load(g, &I->s, 4);
        if (I->sgn) enc(j, 4, OP2(0x0F, 0xAF), RAX, -1, (M[]){ Dn(I->reg) }, 0);    /* imul eax, [Dl] */
        else unop(j, 4, 4, -1, Dn(I->reg));                                         /* mul dword [Dl] */
        if (I->need & FV) setcc_m(j, 0, FLAG(FV));
        mov_m_r(j, 4, Dn(I->reg), RAX); test_r_r(j, 4, RAX);
        g->intact = 1; store_flags(g, I->need & (FN | FZ | FC));
        g->intact = 0;
        return 0;
    case K_SCC: {
        int xc = x86cc(I->cc);
        if (I->cc <= 1) mov_r_imm(j, RAX, I->cc == 0 ? 0xFF : 0);
        else {
            if (!g->intact) load_flags(g);
            setcc_r(j, xc, RAX); unop(j, 1, 3, RAX, mm(0, 0));     /* neg al: 1 -> 0xFF */
            g->intact = 0;
        }
        if (I->d.mode == 0) mov_m_r(j, 1, Dn(I->d.reg), RAX);
        else { gen_addr(g, &I->d, 1); st_guest(j, 1, RAX, 0); }
        return 0; }
    case K_LINK:
        mov_r_m(j, 4, RCX, An(7)); lea(j, RCX, mm(RCX, -4));
        mov_r_m(j, 4, RAX, An(I->reg)); st_guest(j, 4, RAX, 0);
        mov_m_r(j, 4, An(I->reg), RCX); lea(j, RCX, mm(RCX, (int32_t)I->s.val)); mov_m_r(j, 4, An(7), RCX);
        return 0;
    case K_UNLK:
        mov_r_m(j, 4, RCX, An(I->reg)); ld_guest(j, 4, RAX, 0); mov_m_r(j, 4, An(I->reg), RAX);
        lea(j, RCX, mm(RCX, 4)); mov_m_r(j, 4, An(7), RCX);
        return 0;
    case K_MOVEM: {
        int i, off = 0;
        unsigned mask = (unsigned)I->cnt;
        if (I->s.mode == 4) {                                      /* registers -> -(An), mask reversed */
            mov_r_m(j, 4, RCX, An(I->s.reg));
            for (i = 0; i < 16; i++) if (mask & (1u << i)) {
                int r = 15 - i;
                off -= sz;
                movzx_r_m(j, sz, RAX, r < 8 ? Dn(r) : An(r - 8));
                st_guest(j, sz, RAX, off);
            }
            lea(j, RCX, mm(RCX, off)); mov_m_r(j, 4, An(I->s.reg), RCX);
            return 0;
        }
        if (I->s.mode == 3) mov_r_m(j, 4, RCX, An(I->s.reg)); else gen_addr(g, &I->s, 4);
        for (i = 0; i < 16; i++) if (mask & (1u << i)) {
            M r = i < 8 ? Dn(i) : An(i - 8);
            if (I->left) { ld_guest(j, sz, RAX, off); if (sz == 2) movsx_r_r(j, 2, RAX, RAX); mov_m_r(j, 4, r, RAX); }
            else { mov_r_m(j, 4, RAX, r); st_guest(j, sz, RAX, off); }
            off += sz;
        }
        if (I->s.mode == 3) { lea(j, RCX, mm(RCX, off)); mov_m_r(j, 4, An(I->s.reg), RCX); }
        return 0; }
    case K_BCC: {
        if (I->cc == 1) {                                          /* BSR */
            mov_r_imm(j, RAX, I->next); push_eax(g);
            exit_direct(g, I->target);
            return 1;
        }
        if (I->cc == 0) { exit_direct(g, I->target); return 1; }
        {
            uint8_t *t;
            if (!g->intact) load_flags(g);
            t = jcc32(j, x86cc(I->cc));
            exit_direct(g, I->next);
            patch(t, j->p);
            exit_direct(g, I->target);
        }
        return 1; }
    case K_DBCC: {
        uint8_t *t1 = NULL, *t2;
        if (I->cc == 0) { exit_direct(g, I->next); return 1; }   /* DBT: condition always true */
        if (I->cc != 1) { if (!g->intact) load_flags(g); t1 = jcc32(j, x86cc(I->cc)); }   /* condition true: fall through */
        movzx_r_m(j, 2, RAX, Dn(I->reg)); lea(j, RAX, mm(RAX, -1)); mov_m_r(j, 2, Dn(I->reg), RAX);
        alu_rm_imm(j, 2, 7, RAX, mm(0, 0), 0xFFFF);
        t2 = jcc32(j, 5);                                          /* != -1: loop */
        if (t1) patch(t1, j->p);
        exit_direct(g, I->next);
        patch(t2, j->p);
        exit_direct(g, I->target);
        return 1; }
    case K_JSR: case K_JMP:
        if (I->s.mode == 7 && I->s.reg != 3) {
            if (I->kind == K_JSR) { mov_r_imm(j, RAX, I->next); push_eax(g); }
            exit_direct(g, I->s.val);
            return 1;
        }
        gen_addr(g, &I->s, 4); mov_m_r(j, 4, ST(pc), RCX);        /* target first, then the return address */
        if (I->kind == K_JSR) { mov_r_imm(j, RAX, I->next); push_eax(g); }
        exit_indirect(g);
        return 1;
    case K_RTS:
        mov_r_m(j, 4, RCX, An(7)); ld_guest(j, 4, RAX, 0); lea(j, RCX, mm(RCX, 4)); mov_m_r(j, 4, An(7), RCX);
        mov_m_r(j, 4, ST(pc), RAX);
        exit_indirect(g);
        return 1;
    default: return 0;
    }
}

/* ================================================================== blocks */
static block *lookup(jit_t *j, uint32_t pc)
{
    block *b = j->hash[(pc >> 1) & ((1u << HASH_BITS) - 1)];
    while (b && b->pc != pc) b = b->next;
    return b;
}
static void flush(jit_t *j)
{
    memset(j->hash, 0, sizeof(j->hash));
    memset(j->icache, 0, sizeof(*j->icache) << ICACHE_BITS);
    j->npool = 0;
    j->p = j->code + 256;                           /* keep the entry stub */
    j->flushes++;
    j->flush_pending = 0;
}
static block *translate(jit_t *j, m68k *c, uint32_t pc)
{
    jins ins[MAX_INSN];
    int n = 0, i;
    uint8_t live;
    gen g = { j, 0 };
    block *b;
    uint32_t at = pc;
    if (j->npool >= j->maxpool || j->end - j->p < 64 * 1024) flush(j);
    /* decode */
    while (n < MAX_INSN) {
        jins *I = &ins[n++];
        decode(c, at, I);
        if (I->kind == K_FALLBACK_END || I->kind == K_BCC || I->kind == K_DBCC || I->kind == K_JSR ||
            I->kind == K_JMP || I->kind == K_RTS) break;
        at = I->next;
    }
    /* flag liveness: everything is live after the block */
    live = FALL;
    for (i = n - 1; i >= 0; i--) {
        ins[i].need = (uint8_t)(live & ins[i].defs);
        live = (uint8_t)((live & ~ins[i].defs) | ins[i].uses);
    }
    b = &j->pool[j->npool++];
    b->pc = pc; b->code = j->p;
    b->next = j->hash[(pc >> 1) & ((1u << HASH_BITS) - 1)];
    j->hash[(pc >> 1) & ((1u << HASH_BITS) - 1)] = b;
    mov_m_imm(j, 4, ST(insn_pc), pc);
    for (i = 0; i < n; i++) {
        const jins *I = &ins[i];
        int ended;
        if (I->kind == K_FALLBACK || I->kind == K_FALLBACK_END) { j->fallback++; gen_fallback(&g, I); ended = I->kind == K_FALLBACK_END; }
        else { j->native++; ended = gen_insn(&g, I); }
        if (ended) break;
        if (i == n - 1) exit_direct(&g, I->next);              /* block length limit */
    }
    j->blocks++;
    return b;
}

/* entry stub: save callee-saved registers, rbx = state, r12 = memory, call the block */
static void make_entry(jit_t *j)
{
    static const uint8_t stub[] = {
        0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,   /* push rbx rbp r12 r13 r14 r15 */
        0x48, 0x89, 0xFB,                                             /* mov rbx, rdi */
        0x49, 0x89, 0xF4,                                             /* mov r12, rsi */
        0xFF, 0xD2,                                                   /* call rdx */
        0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5B,   /* pop r15 r14 r13 r12 rbp rbx */
        0xC3 };
    memcpy(j->code, stub, sizeof(stub));
    j->enter = (uint8_t *(*)(m68k *, uint8_t *, uint8_t *))(void *)j->code;
}

jit_t *jit_new(m68k *c)
{
    jit_t *j;
    (void)c;
#if !defined(__x86_64__)
    return NULL;
#else
    {
        unsigned a, b2, cx, d;
        if (!__get_cpuid(1, &a, &b2, &cx, &d) || !(cx & (1u << 22))) return NULL;   /* MOVBE */
    }
    j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->code = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (j->code == MAP_FAILED) { free(j); return NULL; }
    j->end = j->code + CODE_SIZE;
    j->maxpool = 1u << 20;
    j->pool = calloc(j->maxpool, sizeof(block));
    if (!j->pool) { munmap(j->code, CODE_SIZE); free(j); return NULL; }
    j->icache = calloc(1u << ICACHE_BITS, sizeof(*j->icache));
    if (!j->icache) { free(j->pool); munmap(j->code, CODE_SIZE); free(j); return NULL; }
    if (getenv("ARITHLON_PROF")) j->prof = calloc(65536, sizeof(uint64_t));
    make_entry(j);
    j->p = j->code + 256;
    return j;
#endif
}

int jit_run(jit_t *j, m68k *c)
{
    sigjmp_buf jb;
    void *old = c->jb;
    c->jb = &jb;
    if (sigsetjmp(jb, 0)) { c->jb = old; return c->fault; }
    while (!c->stop) {
        block *b;
        uint8_t *site;
        if (j->flush_pending) flush(j);
        b = lookup(j, c->pc);
        if (!b) b = translate(j, c, c->pc);
        j->dispatches++;
        site = j->enter(c, c->mem, b->code);
        if (!site && !c->stop && !j->flush_pending) {             /* indirect exit: remember the target for the probe */
            block *t = lookup(j, c->pc);
            if (!t) t = translate(j, c, c->pc);
            j->icache[(c->pc >> 1) & ((1u << ICACHE_BITS) - 1)].pc = c->pc;
            j->icache[(c->pc >> 1) & ((1u << ICACHE_BITS) - 1)].code = t->code;
            if (j->flushes != 0 && lookup(j, c->pc) != t) j->icache[(c->pc >> 1) & ((1u << ICACHE_BITS) - 1)].pc = 0;
        }
        if (site && !c->stop && !j->flush_pending) {              /* chain the direct exit to its target */
            unsigned long f0 = j->flushes;
            block *t = lookup(j, c->pc);
            if (!t) t = translate(j, c, c->pc);
            if (j->flushes == f0) patch(site, t->code);           /* a flush discarded the exit site */
        }
    }
    c->jb = old;
    return c->fault;
}

void jit_invalidate(jit_t *j, uint32_t addr, uint32_t len) { (void)addr; (void)len; j->flush_pending = 1; }
void jit_sync_pc(jit_t *j, m68k *c) { (void)j; c->pc = c->insn_pc; }
void jit_stats(jit_t *j, FILE *f)
{
    fprintf(f, "arithlon JIT: %lu blocks, %lu native / %lu interpreted instructions translated, %llu dispatches, %lu flushes\n",
            j->blocks, j->native, j->fallback, j->dispatches, j->flushes);
    if (j->prof) {
        int k, i;
        for (k = 0; k < 25; k++) {
            int best = -1;
            for (i = 0; i < 65536; i++) if (j->prof[i] && (best < 0 || j->prof[i] > j->prof[best])) best = i;
            if (best < 0) break;
            fprintf(f, "  interpreted %04x: %llu\n", best, (unsigned long long)j->prof[best]);
            j->prof[best] = 0;
        }
    }
}
