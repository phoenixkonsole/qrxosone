/* The 68100EC64 JIT against the reference interpreter: random long mode programs from the
 * instructions the JIT translates (and a few it does not), run both ways from the same state;
 * registers, flags, pc, status, instruction count and RAM must agree. */
#include "qrx68100ec64.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE (4u << 20)
#define CODE 0x10000u
#define DATA 0x100000u
#define STACK 0x300000u

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

static uint16_t prog[4096];
static unsigned np;
static void w16(uint16_t v) { prog[np++] = v; }
static void w32(uint32_t v) { w16((uint16_t)(v >> 16)); w16((uint16_t)v); }
static void w64(uint64_t v) { w32((uint32_t)(v >> 32)); w32((uint32_t)v); }

/* An effective address for size s (1, 2, 4, 8): mode and register in *m, *r, extension words in ext */
typedef struct { unsigned m, r, n; uint16_t ext[4]; } EA;
static EA gen_ea(unsigned s, int kinds)   /* kinds: 1 data reg, 2 address reg, 4 memory, 8 immediate, 16 pc relative */
{
    EA e = { 0, 0, 0, { 0 } };
    for (;;) {
        unsigned k = rn(10);
        if (k == 0 && (kinds & 1)) { e.m = 0; e.r = rn(8); return e; }
        if (k == 1 && (kinds & 2) && s > 1) { e.m = 1; e.r = rn(8); return e; }
        if (k >= 2 && k <= 6 && (kinds & 4)) {
            e.m = k; e.r = rn(7);   /* not A7: the stack stays usable */
            if (k == 5) { e.ext[0] = (uint16_t)(rn(64) * 8 - 128); e.n = 1; }
            if (k == 6) { unsigned xr = rn(8), xa = rn(2); e.ext[0] = (uint16_t)((xa << 15) | (xr << 12) | (rn(2) << 11) | (rn(4) << 9) | (rn(32) * 4)); e.n = 1; }
            return e;
        }
        if (k == 7 && (kinds & 4)) { e.m = 7; e.r = 0; e.ext[0] = (uint16_t)(0x1000 + rn(0x600) * 8); e.n = 1; return e; }
        if (k == 8 && (kinds & 8)) {
            e.m = 7; e.r = 4;
            uint64_t v = rnd();
            if (s == 1) { e.ext[0] = v & 0xFF; e.n = 1; }
            else if (s == 2) { e.ext[0] = (uint16_t)v; e.n = 1; }
            else if (s == 4) { e.ext[0] = (uint16_t)(v >> 16); e.ext[1] = (uint16_t)v; e.n = 2; }
            else { for (int i = 0; i < 4; i++) e.ext[i] = (uint16_t)(v >> (48 - 16 * i)); e.n = 4; }
            return e;
        }
        if (k == 9 && (kinds & 16)) { e.m = 7; e.r = 2; e.ext[0] = (uint16_t)(rn(16) * 2); e.n = 1; return e; }
    }
}
static void put_ea(const EA *e) { for (unsigned i = 0; i < e->n; i++) w16(e->ext[i]); }
static unsigned szf(unsigned s) { return s == 1 ? 0 : s == 2 ? 1 : 2; }   /* for .Q: .L with W */

static void gen_insn(void)
{
    unsigned pfx = 0, s = 1u << rn(4);           /* 1, 2, 4 or 8 */
    if (s == 8) pfx |= 8;
    if (rn(4) == 0) pfx |= rn(8) & 7 & ~0u;      /* R, X, B now and then */
    pfx &= 0xF;
    unsigned k = rn(36);
    unsigned at = np;
    if (pfx) w16((uint16_t)(0xA000 | pfx));
    unsigned sz = s == 8 ? 4 : s;
    switch (k) {
    case 0: case 1: case 2: case 3: case 4: {   /* MOVE, MOVEA */
        EA src = gen_ea(sz == 8 ? 8 : s, 1 | 2 | 4 | 8 | 16);
        unsigned line = sz == 1 ? 1 : sz == 2 ? 3 : 2;
        if (rn(4) == 0 && s > 1) { w16((uint16_t)(line << 12 | rn(8) << 9 | 1 << 6 | src.m << 3 | src.r)); put_ea(&src); break; }
        EA dst = gen_ea(s, 1 | 4);
        if (rn(16) == 0) { dst.m = 7; dst.r = 2; dst.ext[0] = (uint16_t)(rn(32) * 2); dst.n = 1; }   /* into the code */
        if (rn(6) == 0) { if (pfx == 0) w16(0xA010); }
        w16((uint16_t)(line << 12 | dst.r << 9 | dst.m << 6 | src.m << 3 | src.r)); put_ea(&src); put_ea(&dst);
        break;
    }
    case 5: w16((uint16_t)(0x7000 | rn(8) << 9 | rn(256))); break;          /* MOVEQ */
    case 6: case 7: case 8: {                                                 /* ADD SUB AND OR CMP EOR */
        static const unsigned lines[] = { 0xD, 0x9, 0xC, 0x8, 0xB };
        unsigned line = lines[rn(5)], toea = rn(2);
        if (line == 0xB && toea) {                                            /* EOR Dn,<ea> */
            EA e = gen_ea(s, 1 | 4);
            w16((uint16_t)(0xB000 | rn(8) << 9 | (4 + szf(sz)) << 6 | e.m << 3 | e.r)); put_ea(&e); break;
        }
        if (toea) { EA e = gen_ea(s, 4); w16((uint16_t)(line << 12 | rn(8) << 9 | (4 + szf(sz)) << 6 | e.m << 3 | e.r)); put_ea(&e); break; }
        EA e = gen_ea(s, 1 | 4 | 8 | 16 | ((line == 0xD || line == 0x9 || line == 0xB) ? 2 : 0));
        w16((uint16_t)(line << 12 | rn(8) << 9 | szf(sz) << 6 | e.m << 3 | e.r)); put_ea(&e);
        break;
    }
    case 9: {                                                                 /* ADDA SUBA CMPA */
        static const unsigned lines[] = { 0xD, 0x9, 0xB };
        unsigned w = rn(2);
        EA e = gen_ea(w ? (s == 8 ? 8 : 4) : 2, 1 | 2 | 4 | 8 | 16);
        w16((uint16_t)(lines[rn(3)] << 12 | rn(8) << 9 | (w ? 7 : 3) << 6 | e.m << 3 | e.r)); put_ea(&e);
        break;
    }
    case 10: {                                                                /* ADDQ SUBQ */
        EA e = gen_ea(s, 1 | 2 | 4);
        if (e.m == 1 && s == 1) e.m = 0;
        w16((uint16_t)(0x5000 | rn(8) << 9 | rn(2) << 8 | szf(sz) << 6 | e.m << 3 | e.r)); put_ea(&e);
        break;
    }
    case 11: {                                                                /* ORI ANDI SUBI ADDI EORI CMPI */
        static const unsigned kinds[] = { 0, 1, 2, 3, 5, 6 };
        unsigned kind = kinds[rn(6)];
        EA e = gen_ea(s, 1 | 4 | (kind == 6 ? 16 : 0));
        w16((uint16_t)(kind << 9 | szf(sz) << 6 | e.m << 3 | e.r));
        if (s == 1) w16(rn(256)); else if (s == 2) w16((uint16_t)rnd()); else if (s == 4) w32((uint32_t)rnd()); else w64(rnd());
        put_ea(&e);
        break;
    }
    case 12: { EA e = gen_ea(s, 1 | 2 | 4); if (e.m == 1 && s == 1) e.m = 0;   /* TST CLR NEG NOT */
        unsigned kk = rn(4), op = kk == 0 ? 0x4A00 : kk == 1 ? 0x4200 : kk == 2 ? 0x4400 : 0x4600;
        if (kk && e.m == 1) e.m = 0;
        w16((uint16_t)(op | szf(sz) << 6 | e.m << 3 | e.r)); put_ea(&e); break; }
    case 13: { unsigned kk = rn(4); w16((uint16_t)(kk == 0 ? 0x4880 : kk == 1 ? 0x48C0 : kk == 2 ? 0x49C0 : 0x48C0) | rn(8)); break; }   /* EXT, EXTB */
    case 14: {                                                                /* LSL LSR ASR (and ASL, ROR: interpreter) */
        unsigned kind = rn(6), left = rn(2);
        unsigned code = kind < 4 ? 1 : kind == 4 ? 0 : 3;
        if (code == 0 && kind == 4) left = rn(2);
        w16((uint16_t)(0xE000 | rn(8) << 9 | left << 8 | szf(sz) << 6 | code << 3 | rn(8)));
        break;
    }
    case 15: { EA e = gen_ea(1, 1 | 4); w16((uint16_t)(0x50C0 | rn(16) << 8 | e.m << 3 | e.r)); put_ea(&e); break; }   /* Scc */
    case 16: {                                                                /* MOVEM */
        unsigned to_regs = rn(2), mode = to_regs ? (rn(2) ? 3 : 5) : (rn(2) ? 4 : 5);
        uint16_t mask = (uint16_t)(rnd() & (to_regs ? 0x7F7F : 0xFEFE));     /* A7 stays */
        if (!mask) mask = 1;
        if (mode == 4) mask = (uint16_t)(rnd() & 0xFFFE) | 2;   /* reversed: bit 0 is A7 */
        unsigned r = rn(7);
        w16((uint16_t)(0x4880 | to_regs << 10 | (s >= 4 ? 1 : 0) << 6 | mode << 3 | r));
        w16(mask);
        if (mode == 5) w16((uint16_t)(rn(32) * 8));
        break;
    }
    case 17: { EA e = gen_ea(4, 4 | 16); if (e.m == 3 || e.m == 4) e.m = 2;   /* LEA */
        w16((uint16_t)(0x41C0 | rn(8) << 9 | e.m << 3 | e.r)); put_ea(&e); break; }
    case 18: if (pfx) { np = at; } w16(0x487A); w16(4); w16(0x4E75); break;         /* PEA (4,PC); RTS */
    case 19: if (pfx) { np = at; } w16(0x4EBA); w16(2); break;                     /* JSR (2,PC) */
    case 20: if (pfx) { np = at; } w16(0x6102); w16(0x4E71); break;                /* BSR.B +2 over a NOP */
    case 21: if (pfx) { np = at; } w16((uint16_t)(0x6002 | rn(16) << 8)); w16(0x4E71); break;   /* Bcc.B +2 */
    case 22: if (pfx) { np = at; } w16((uint16_t)(0x6000 | rn(16) << 8)); w16(4); w16(0x4E71); break;   /* Bcc.W */
    case 23: w16((uint16_t)(0x51C8 | rn(16) << 8 | rn(8))); w16(2); break;   /* DBcc to the next */
    case 24: if (pfx) { np = at; } w16(0x4E56); w16((uint16_t)-(int)(rn(8) * 8)); w16(0x4E5E); break;   /* LINK A6; UNLK A6 */
    case 25: if (pfx) { np = at; } w16((uint16_t)(0x4E50 | rn(6))); w16((uint16_t)-(int)(rn(8) * 8)); break;   /* LINK */
    case 26: { EA e = gen_ea(s == 8 ? 8 : 4, 1 | 4 | 8 | 16); w16((uint16_t)(0x4C00 | e.m << 3 | e.r)); w16((uint16_t)(rn(8) << 12 | rn(2) << 11 | (rn(4) == 0 ? 0x400 | rn(8) : 0))); put_ea(&e); break; }   /* MULS.L, MULU.L */
    case 27: if (pfx) { np = at; } w16(0x4E71); break;
    case 28: case 29: {                                       /* shifts and rotates by register or count, all kinds */
        w16((uint16_t)(0xE000 | rn(8) << 9 | rn(2) << 8 | szf(sz) << 6 | rn(2) << 5 | rn(4) << 3 | rn(8)));
        break;
    }
    case 30: { EA e = gen_ea(2, 4); if (e.m < 2) e.m = 2;     /* memory shift by one */
        w16((uint16_t)(0xE0C0 | rn(4) << 9 | rn(2) << 8 | e.m << 3 | e.r)); put_ea(&e); break; }
    case 31: {                                                /* BTST BCHG BCLR BSET, static and dynamic */
        EA e = gen_ea(1, 1 | 4);
        if (rn(2)) { w16((uint16_t)(0x0800 | rn(4) << 6 | e.m << 3 | e.r)); w16(rn(256)); }
        else w16((uint16_t)(0x0100 | rn(8) << 9 | rn(4) << 6 | e.m << 3 | e.r));
        put_ea(&e); break;
    }
    case 32: { EA e = gen_ea(s == 8 ? 8 : 4, 1 | 4 | 8);      /* DIVU.L DIVS.L */
        w16((uint16_t)(0x4C40 | e.m << 3 | e.r)); w16((uint16_t)(rn(8) << 12 | rn(2) << 11 | (s == 8 ? 0 : rn(2) << 10) | rn(8))); put_ea(&e); break; }
    case 33: {                                                /* LEA with the full index format */
        unsigned bd = 1 + rn(3);
        w16((uint16_t)(0x41F0 | rn(8) << 9 | rn(7)));
        w16((uint16_t)(rn(2) << 15 | rn(8) << 12 | 1 << 11 | rn(4) << 9 | 0x100 | rn(2) << 7 | rn(2) << 6 | bd << 4));
        if (bd == 2) w16((uint16_t)(rn(64) * 8)); else if (bd == 3) w32(rn(64) * 8);
        break;
    }
    case 34: { EA e = gen_ea(2, 1 | 4 | 8);                    /* MULU.W, MULS.W */
        w16((uint16_t)(0xC0C0 | rn(8) << 9 | rn(2) << 8 | e.m << 3 | e.r)); put_ea(&e); break; }
    default: { EA e = gen_ea(s, 1 | 4); w16((uint16_t)(0xD000 | rn(8) << 9 | szf(sz) << 6 | e.m << 3 | e.r)); put_ea(&e); break; }
    }
}

static uint8_t *ram_a, *ram_b;
static void setup(qec64_cpu *c, uint8_t *ram, uint64_t seed)
{
    qec64_init(c, ram, RAM_SIZE, 0);
    c->m64 = true;
    rng = seed;
    for (int i = 0; i < 16; i++) {
        c->d[i] = rn(3) == 0 ? rnd() : rn(3) == 0 ? (uint64_t)(int64_t)(int32_t)rnd() : rn(64);
        c->a[i] = DATA + rn(0x4000) * 8;
    }
    c->a[7] = STACK;
    c->ccr = (uint8_t)rn(32);
    c->pc = CODE;
}

int main(int argc, char **argv)
{
    unsigned runs = argc > 1 ? (unsigned)atoi(argv[1]) : 3000, fails = 0, first = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
    ram_a = calloc(RAM_SIZE, 1); ram_b = calloc(RAM_SIZE, 1);
    uint8_t *map; uint32_t *gen;
    if (!ram_a || !ram_b || !qec64_jit_map_new(RAM_SIZE, &map, &gen)) { puts("no memory"); return 1; }
    uint64_t total = 0, translated_insns = 0;
    qec64_cpu b0;
    memset(&b0, 0, sizeof b0);
    for (unsigned run = first; run < runs; run++) {
        uint64_t seed = 0x1234567ull + run * 0x9E3779B97F4A7C15ull;
        rng = seed;
        np = 0;
        unsigned n = 4 + rn(40);
        for (unsigned i = 0; i < n && np < 4000; i++) gen_insn();
        w16(0x4AFC);
        /* RAM: random data, the program at CODE */
        rng = seed ^ 0xABCDEF;
        for (unsigned i = 0; i < RAM_SIZE; i += 8) { uint64_t v = (i >= DATA && i < STACK + 0x1000) || i < 0x8000 ? rnd() : 0; memcpy(ram_a + i, &v, 8); }
        for (unsigned i = 0; i < np; i++) { ram_a[CODE + i * 2] = (uint8_t)(prog[i] >> 8); ram_a[CODE + i * 2 + 1] = (uint8_t)prog[i]; }
        memcpy(ram_b, ram_a, RAM_SIZE);
        qec64_cpu a, b;
        setup(&a, ram_a, seed); setup(&b, ram_b, seed);
        memset(map, 0, (RAM_SIZE >> QEC64_CODE_SHIFT) + 2);
        b.code_map = map; b.code_gen = gen;
        b.jit = b0.jit;
        (*gen)++;   /* a new program at the same place */
        enum qec64_status sa = qec64_run(&a, 3000), sb = qec64_jit_run(&b, 3000);
        b0.jit = b.jit;
        total += a.instructions;
        bool same = sa == sb && a.pc == b.pc && a.ccr == b.ccr && a.instructions == b.instructions &&
                    !memcmp(a.d, b.d, sizeof a.d) && !memcmp(a.a, b.a, sizeof a.a) && !memcmp(ram_a, ram_b, RAM_SIZE);
        if (!same) {
            if (fails++ < 5) {
                printf("run %u differs: status %d/%d pc %" PRIx64 "/%" PRIx64 " ccr %02x/%02x insns %" PRIu64 "/%" PRIu64 "\n",
                       run, sa, sb, a.pc, b.pc, a.ccr, b.ccr, a.instructions, b.instructions);
                for (int i = 0; i < 16; i++) {
                    if (a.d[i] != b.d[i]) printf("  d%d %016" PRIx64 " / %016" PRIx64 "\n", i, a.d[i], b.d[i]);
                    if (a.a[i] != b.a[i]) printf("  a%d %016" PRIx64 " / %016" PRIx64 "\n", i, a.a[i], b.a[i]);
                }
                for (unsigned i = 0; i < RAM_SIZE; i++) if (ram_a[i] != ram_b[i]) { printf("  ram %x: %02x / %02x\n", i, ram_a[i], ram_b[i]); break; }
                /* step both an instruction at a time to the first difference */
                extern unsigned qec64_jit_block_limit;
                unsigned keep = qec64_jit_block_limit;
                qec64_jit_block_limit = 1;
                memcpy(ram_b, ram_a, 0);
                rng = seed ^ 0xABCDEF;
                for (unsigned i = 0; i < RAM_SIZE; i += 8) { uint64_t v = (i >= DATA && i < STACK + 0x1000) || i < 0x8000 ? rnd() : 0; memcpy(ram_a + i, &v, 8); }
                for (unsigned i = 0; i < np; i++) { ram_a[CODE + i * 2] = (uint8_t)(prog[i] >> 8); ram_a[CODE + i * 2 + 1] = (uint8_t)prog[i]; }
                memcpy(ram_b, ram_a, RAM_SIZE);
                qec64_cpu x, y;
                setup(&x, ram_a, seed); setup(&y, ram_b, seed);
                y.code_map = map; y.code_gen = gen; y.jit = b0.jit; (*gen)++;
                for (unsigned step = 0; step < 3000; step++) {
                    uint64_t pc = x.pc;
                    enum qec64_status s1 = qec64_run(&x, 1), s2 = qec64_jit_run(&y, 1);
                    bool eq = s1 == s2 && x.pc == y.pc && x.ccr == y.ccr && !memcmp(x.d, y.d, sizeof x.d) && !memcmp(x.a, y.a, sizeof x.a) && !memcmp(ram_a, ram_b, RAM_SIZE);
                    if (!eq) {
                        printf("  first difference at step %u, pc %" PRIx64 ": %02x%02x %02x%02x %02x%02x %02x%02x; status %d/%d pc %" PRIx64 "/%" PRIx64 " ccr %02x/%02x\n", step, pc,
                               ram_a[pc], ram_a[pc+1], ram_a[pc+2], ram_a[pc+3], ram_a[pc+4], ram_a[pc+5], ram_a[pc+6], ram_a[pc+7], s1, s2, x.pc, y.pc, x.ccr, y.ccr);
                        for (int i = 0; i < 16; i++) {
                            if (x.d[i] != y.d[i]) printf("    d%d %016" PRIx64 " / %016" PRIx64 "\n", i, x.d[i], y.d[i]);
                            if (x.a[i] != y.a[i]) printf("    a%d %016" PRIx64 " / %016" PRIx64 "\n", i, x.a[i], y.a[i]);
                        }
                        for (unsigned i = 0; i < RAM_SIZE; i++) if (ram_a[i] != ram_b[i]) { printf("    ram %x: %02x / %02x\n", i, ram_a[i], ram_b[i]); break; }
                        break;
                    }
                    if (s1 != QEC64_RUNNING) break;
                }
                b0.jit = y.jit;
                qec64_jit_block_limit = keep;
                printf("  program:");
                for (unsigned i = 0; i < np && i < 64; i++) printf(" %04x", prog[i]);
                printf("\n");
            }
        }
        (void)translated_insns;
    }
    b0.code_map = map; b0.code_gen = gen;
    qec64_jit_free(&b0);
    printf("%u programs, %" PRIu64 " instructions, %u differ\n", runs, total, fails);
    return fails ? 1 : 0;
}
