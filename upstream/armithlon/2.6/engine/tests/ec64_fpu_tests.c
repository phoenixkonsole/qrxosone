/* The 68100EC64's FPU (docs/armithlon/68100ec64.md "FPU"): programs in its 68881 encoding run by the
 * reference interpreter, their results against the host's doubles. */
#include "qrx68100ec64.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W(...) (const uint16_t[]){ __VA_ARGS__ }, sizeof((const uint16_t[]){ __VA_ARGS__ }) / 2
#define Q64(v) (uint16_t)((uint64_t)(v) >> 48), (uint16_t)((uint64_t)(v) >> 32), (uint16_t)((uint64_t)(v) >> 16), (uint16_t)(v)
#define RAM_SIZE (UINT64_C(64) << 20)
#define CODE UINT64_C(0x10000)
#define DATA UINT64_C(0x20000)
static uint8_t *ram;
static qec64_cpu c;
static unsigned failures, checks;
#define CHECK(x, ...) do { checks++; if (!(x)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); failures++; } } while (0)

static void put16(uint64_t a, uint16_t v) { ram[a] = (uint8_t)(v >> 8); ram[a + 1] = (uint8_t)v; }
static uint64_t get64(uint64_t a) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | ram[a + i]; return v; }
static uint32_t get32(uint64_t a) { return (uint32_t)(get64(a) >> 32); }
static void put64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; i++) ram[a + i] = (uint8_t)(v >> (56 - i * 8)); }
static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static enum qec64_status run(const uint16_t *w, size_t n)
{
    for (size_t i = 0; i < n; i++) put16(CODE + i * 2, w[i]);
    put16(CODE + n * 2, 0x4AFC);    /* ILLEGAL: the core stops */
    c.pc = CODE;
    return qec64_run(&c, 100000);
}
static void fresh(void)
{
    qec64_init(&c, ram, RAM_SIZE, 0);
    c.m64 = true;
    c.a[7] = UINT64_C(0x30000);
    c.a[0] = DATA;
}
#define FMOVE_D_IMM(n) 0xF23C, (uint16_t)(0x5400 | (n) << 7)          /* FMOVE.D #imm,FPn */
#define FOP_REG(src, dst, opm) 0xF200, (uint16_t)((src) << 10 | (dst) << 7 | (opm))
#define FPCC(x) ((c.fpsr >> 24) & 15)                                   /* N Z I NAN */

static void arithmetic(void)
{
    fresh();
    run(W(FMOVE_D_IMM(0), Q64(bits(1.5)), FMOVE_D_IMM(1), Q64(bits(0.25)),
          FOP_REG(1, 0, 0x22),                     /* FADD FP1,FP0 */
          FOP_REG(0, 2, 0x00), FOP_REG(1, 2, 0x23), /* FMOVE FP0,FP2; FMUL FP1,FP2 */
          FOP_REG(1, 3, 0x00), FOP_REG(0, 3, 0x20), /* FP3 = 0.25 / 1.75 */
          FOP_REG(1, 4, 0x00), FOP_REG(0, 4, 0x28), /* FP4 = 0.25 - 1.75 */
          0xF210, 0x7400));                         /* FMOVE.D FP0,(A0) */
    CHECK(c.fp[0] == 1.75 && c.fp[2] == 1.75 * 0.25 && c.fp[3] == 0.25 / 1.75 && c.fp[4] == -1.5,
          "add mul div sub: %g %g %g %g", c.fp[0], c.fp[2], c.fp[3], c.fp[4]);
    CHECK(get64(DATA) == bits(1.75), "FMOVE.D to memory: %016" PRIx64, get64(DATA));
    CHECK(FPCC(c) == 8, "N after a negative result: %x", FPCC(c));
    /* <ea> sources of each format */
    fresh(); c.d[1] = (uint64_t)-7; put64(DATA, bits(2.5)); ram[DATA + 8] = 0xFE; put16(DATA + 10, 0x8000);
    run(W(0xF201, 0x4000,                         /* FMOVE.L D1,FP0 */
          0xF210, 0x5480,                         /* FMOVE.D (A0),FP1 */
          0xF228, 0x5900, 0x0008,                 /* FMOVE.B 8(A0),FP2 */
          0xF228, 0x5180, 0x000A,                 /* FMOVE.W 10(A0),FP3 */
          0xF23C, 0x4600, 0x4049, 0x0FDB,         /* FMOVE.S #3.14159274,FP4 */
          0xA008, 0xF201, 0x4280));               /* FMOVE.Q D1,FP5 (W prefix: L is 64 bits) */
    CHECK(c.fp[0] == -7 && c.fp[1] == 2.5 && c.fp[2] == -2 && c.fp[3] == -32768 &&
          c.fp[4] == (double)3.14159274f && c.fp[5] == -7,
          "formats: %g %g %g %g %.9g %g", c.fp[0], c.fp[1], c.fp[2], c.fp[3], c.fp[4], c.fp[5]);
    /* single precision rounding of FSMUL, sqrt, abs, neg */
    fresh();
    run(W(FMOVE_D_IMM(0), Q64(bits(1.0 / 3.0)), FOP_REG(0, 1, 0x00), FMOVE_D_IMM(2), Q64(bits(3.0)),
          FOP_REG(2, 1, 0x63),                     /* FSMUL FP2,FP1 */
          FMOVE_D_IMM(3), Q64(bits(2.0)), FOP_REG(3, 3, 0x04),  /* FSQRT FP3 */
          FMOVE_D_IMM(4), Q64(bits(-4.5)), FOP_REG(4, 5, 0x18), FOP_REG(4, 6, 0x1A)));
    CHECK(c.fp[1] == (double)(float)(1.0 / 3.0 * 3.0) && c.fp[3] == sqrt(2.0) && c.fp[5] == 4.5 && c.fp[6] == 4.5,
          "FSMUL %.17g FSQRT %.17g FABS %g FNEG %g", c.fp[1], c.fp[3], c.fp[5], c.fp[6]);
}

static void conversions(void)
{
    /* to integers: FPCR's rounding mode, saturation, NaN gives 0 */
    const double v[] = { 2.5, -2.5, 3.7, -3.7, 1e300, -1e300, NAN, 2147483647.6 };
    for (unsigned m = 0; m < 4; m++)
        for (unsigned i = 0; i < sizeof v / sizeof *v; i++) {
            fresh(); c.d[0] = m << 4;
            run(W(0xF200, 0x9000,                 /* FMOVE.L D0,FPCR */
                  FMOVE_D_IMM(0), Q64(bits(v[i])),
                  0xF201, 0x6000,                 /* FMOVE.L FP0,D1 */
                  0xA008, 0xF202, 0x6000,         /* FMOVE.Q FP0,D2 */
                  FOP_REG(0, 1, 0x01),            /* FINT FP0,FP1 */
                  FOP_REG(0, 2, 0x03)));          /* FINTRZ FP0,FP2 */
            double r = v[i] != v[i] ? 0 : m == 0 ? nearbyint(v[i]) : m == 1 ? trunc(v[i]) : m == 2 ? floor(v[i]) : ceil(v[i]);
            int64_t want32 = r >= 2147483647.0 ? INT32_MAX : r <= -2147483648.0 ? INT32_MIN : (int64_t)r;
            int64_t want64 = r >= 9223372036854775807.0 ? INT64_MAX : r <= -9223372036854775808.0 ? INT64_MIN : (int64_t)r;
            CHECK((int32_t)c.d[1] == want32 && (int64_t)c.d[2] == want64, "mode %u %g: L %d (want %" PRId64 ") Q %" PRId64,
                  m, v[i], (int32_t)c.d[1], want32, (int64_t)c.d[2]);
            double fi = m == 0 ? nearbyint(v[i]) : r;
            CHECK((v[i] != v[i] && c.fp[1] != c.fp[1]) || c.fp[1] == (v[i] != v[i] ? 0 : fi), "FINT mode %u %g: %g", m, v[i], c.fp[1]);
            CHECK((v[i] != v[i] && c.fp[2] != c.fp[2]) || c.fp[2] == trunc(v[i]), "FINTRZ %g: %g", v[i], c.fp[2]);
        }
    /* to single precision in memory */
    fresh();
    run(W(FMOVE_D_IMM(0), Q64(bits(0.1)), 0xF210, 0x6400));   /* FMOVE.S FP0,(A0) */
    float f = 0.1f; uint32_t fu; memcpy(&fu, &f, 4);
    CHECK(get32(DATA) == fu, "FMOVE.S to memory: %08x", get32(DATA));
}

/* FCMP, FTST, FBcc and FScc against the host's comparisons */
static bool host_cond(double d, double s, unsigned cc)
{
    bool un = d != d || s != s, eq = !un && d == s, lt = !un && d < s, gt = !un && d > s;
    switch (cc & 15) {
    case 0: return false; case 1: return eq; case 2: return gt; case 3: return gt || eq;
    case 4: return lt; case 5: return lt || eq; case 6: return lt || gt; case 7: return !un;
    case 8: return un; case 9: return un || eq; case 10: return un || gt; case 11: return un || gt || eq;
    case 12: return un || lt; case 13: return un || lt || eq; case 14: return !eq; default: return true;
    }
}
static void compares(void)
{
    const double v[] = { -1, 0, -0.0, 1, INFINITY, -INFINITY, NAN };
    for (unsigned i = 0; i < 7; i++)
        for (unsigned j = 0; j < 7; j++)
            for (unsigned cc = 0; cc < 32; cc++) {
                fresh();
                run(W(FMOVE_D_IMM(0), Q64(bits(v[i])), FMOVE_D_IMM(1), Q64(bits(v[j])),
                      FOP_REG(1, 0, 0x38),                        /* FCMP FP1,FP0 */
                      0xF242, (uint16_t)cc,                       /* FScc D2 */
                      (uint16_t)(0xF280 | cc), 0x0004,            /* FBcc.W +4 */
                      0x7601,                                     /* MOVEQ #1,D3: not taken */
                      0x4E71));                                   /* NOP: the target */
                bool want = host_cond(v[i], v[j], cc);
                CHECK((uint8_t)c.d[2] == (want ? 0xFF : 0) && (c.d[3] == 1) == !want,
                      "%g cmp %g cc %u: FScc %02x branch %s", v[i], v[j], cc, (uint8_t)c.d[2], c.d[3] ? "not taken" : "taken");
            }
    fresh();
    run(W(FMOVE_D_IMM(0), Q64(bits(-0.0)), FOP_REG(0, 0, 0x3A)));    /* FTST FP0 */
    CHECK(FPCC(c) == 0xC, "FTST -0: N and Z, %x", FPCC(c));
    fresh();
    run(W(FMOVE_D_IMM(0), Q64(bits(-INFINITY)), FOP_REG(0, 0, 0x3A)));
    CHECK(FPCC(c) == 0xA, "FTST -inf: N and I, %x", FPCC(c));
}

static void moves(void)
{
    /* FMOVEM to and from the stack, and FPCR/FPSR/FPIAR */
    fresh();
    c.fp[0] = 1; c.fp[1] = 2; c.fp[7] = 7;
    run(W(0xF227, 0xE083,                        /* FMOVEM FP0/FP1/FP7,-(A7) */
          FOP_REG(0, 0, 0x1A), FOP_REG(1, 1, 0x1A), FOP_REG(7, 7, 0x1A),
          0xF21F, 0xD0C1,                        /* FMOVEM (A7)+,FP0/FP1/FP7 */
          0xF200, 0x9000,                        /* FMOVE.L D0,FPCR */
          0xF201, 0xA800,                        /* FMOVE.L FPSR,D1 */
          0xF202, 0xA400));                      /* FMOVE.L FPIAR,D2 */
    CHECK(c.fp[0] == 1 && c.fp[1] == 2 && c.fp[7] == 7 && c.a[7] == 0x30000, "FMOVEM: %g %g %g sp %" PRIx64,
          c.fp[0], c.fp[1], c.fp[7], c.a[7]);
    CHECK(c.d[2] == (uint32_t)(CODE + 0x0C), "FPIAR, the last FNEG: %" PRIx64, c.d[2]);
    fresh(); c.fp[2] = 3;
    run(W(0xF228, 0xF020, 0x0010));              /* FMOVEM FP2,16(A0) */
    CHECK(get64(DATA + 16) == bits(3.0), "FMOVEM to (d16,An): %016" PRIx64, get64(DATA + 16));
    /* not there: extended and packed formats, FSIN, 32-bit mode */
    fresh();
    CHECK(run(W(0xF23C, 0x4800, 0, 0, 0, 0, 0, 0)) == QEC64_UNIMPLEMENTED, "FMOVE.X is not there");
    fresh();
    CHECK(run(W(FOP_REG(0, 0, 0x0E))) == QEC64_UNIMPLEMENTED, "FSIN is not there");
    fresh(); c.m64 = false;
    CHECK(run(W(FOP_REG(0, 0, 0x22))) == QEC64_UNIMPLEMENTED, "no FPU in 32-bit mode");
}

int main(void)
{
    ram = calloc(1, RAM_SIZE);
    if (!ram) return 2;
    arithmetic();
    conversions();
    compares();
    moves();
    printf("%u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
