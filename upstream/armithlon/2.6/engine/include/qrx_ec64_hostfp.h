/* 68100EC64: host floating point ($AFnn), an extension of this machine (the core has no FPU).
 *
 * One instruction word $AF00 | n, in long mode only. It takes D0 and D1 as raw IEEE 754 bits
 * (single precision in the low 32 bits, double precision in all 64) and leaves the result in D0;
 * D1, the other registers and the condition codes stay. The host computes with its own IEEE
 * arithmetic, rounding to nearest even: the same results as compiler-rt's soft float, which the
 * m68k64 compiler calls for every float operation (__addsf3 and the others). The guest's
 * replacements for those (tools/armithlon/aros-qrx/qrxhostfp) are this instruction and RTS:
 * one instruction where the soft float took 50 to 200.
 *
 * Integer results: 32-bit ones are sign extended to 64 bits, 64-bit ones fill D0. Conversions to
 * integers and NaNs follow compiler-rt bit for bit (see qec64_fixint and qec64_arith_s); the test
 * tools/armithlon/aros-qrx/qrxhostfp/fptest.c compares the two in the guest. */
#ifndef QRX_EC64_HOSTFP_H
#define QRX_EC64_HOSTFP_H
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
    QEC64_FP_ADDS = 0x00, QEC64_FP_SUBS, QEC64_FP_MULS, QEC64_FP_DIVS,
    QEC64_FP_LES,       /* __lesf2/__ltsf2: -1 less, 0 equal, 1 greater or unordered */
    QEC64_FP_GES,       /* __gesf2/__gtsf2: -1 less or unordered, 0 equal, 1 greater */
    QEC64_FP_UNORDS,    /* __unordsf2: 1 if either is NaN */
    QEC64_FP_S2I,       /* __fixsfsi */
    QEC64_FP_S2U,       /* __fixunssfsi */
    QEC64_FP_S2L,       /* __fixsfdi */
    QEC64_FP_S2UL,      /* __fixunssfdi */
    QEC64_FP_I2S,       /* __floatsisf */
    QEC64_FP_U2S,       /* __floatunsisf */
    QEC64_FP_L2S,       /* __floatdisf */
    QEC64_FP_UL2S,      /* __floatundisf */
    QEC64_FP_S2D,       /* __extendsfdf2 */
    QEC64_FP_SQRTS,     /* sqrtf */
    QEC64_FP_ADDD = 0x20, QEC64_FP_SUBD, QEC64_FP_MULD, QEC64_FP_DIVD,
    QEC64_FP_LED, QEC64_FP_GED, QEC64_FP_UNORDD,
    QEC64_FP_D2I, QEC64_FP_D2U, QEC64_FP_D2L, QEC64_FP_D2UL,
    QEC64_FP_I2D, QEC64_FP_U2D, QEC64_FP_L2D, QEC64_FP_UL2D,
    QEC64_FP_D2S,       /* __truncdfsf2 */
    QEC64_FP_SQRTD      /* sqrt */
};

static inline float qec64_f(uint64_t v) { uint32_t b = (uint32_t)v; float f; memcpy(&f, &b, 4); return f; }
static inline double qec64_d(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static inline uint64_t qec64_fb(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static inline uint64_t qec64_db(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static inline uint64_t qec64_i32(int32_t v) { return (uint64_t)(int64_t)v; }

/* Conversions to integers bit for bit as compiler-rt's fp_fixint_impl.inc and fp_fixuint_impl.inc:
 * NaN and too large values give the largest value of the sign (a NaN's sign), and an exponent of
 * exactly the integer's width minus one wraps (2^31 to int gives INT_MIN). bits: the integer's
 * width; mbits, ebits: the float's significand and exponent widths. */
static inline uint64_t qec64_fixint(uint64_t rep, unsigned mbits, unsigned ebits, unsigned bits)
{
    uint64_t sbit = UINT64_C(1) << (mbits + ebits), abs = rep & (sbit - 1);
    int neg = (rep & sbit) != 0;
    int e = (int)(abs >> mbits) - ((1 << (ebits - 1)) - 1);
    uint64_t m = (abs & ((UINT64_C(1) << mbits) - 1)) | (UINT64_C(1) << mbits);
    uint64_t mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1, max = mask >> 1;
    if (e < 0) return 0;
    if ((unsigned)e >= bits) return neg ? (max + 1) & mask : max;
    uint64_t v = e < (int)mbits ? m >> (mbits - e) : (m << (e - mbits)) & mask;
    if (neg) v = (0 - v) & mask;
    return v;
}
static inline uint64_t qec64_fixuint(uint64_t rep, unsigned mbits, unsigned ebits, unsigned bits)
{
    uint64_t sbit = UINT64_C(1) << (mbits + ebits), abs = rep & (sbit - 1);
    int e = (int)(abs >> mbits) - ((1 << (ebits - 1)) - 1);
    uint64_t m = (abs & ((UINT64_C(1) << mbits) - 1)) | (UINT64_C(1) << mbits);
    uint64_t mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    if ((rep & sbit) || e < 0) return 0;
    if ((unsigned)e >= bits) return mask;
    return e < (int)mbits ? m >> (mbits - e) : (m << (e - mbits)) & mask;
}
#define QEC64_SX32(v) ((uint64_t)(int64_t)(int32_t)(uint32_t)(v))
static inline uint64_t qec64_cmp(double a, double b, int unord) { return qec64_i32(a < b ? -1 : a == b ? 0 : a > b ? 1 : unord); }
/* NaNs as compiler-rt has them: a NaN operand comes back quiet (the first one first), a new NaN
 * is the positive quiet NaN (x86 would make it negative) */
static inline uint64_t qec64_arith_s(uint64_t a, uint64_t b, float r)
{
    if (qec64_f(a) != qec64_f(a)) return (uint32_t)a | 0x00400000u;
    if (qec64_f(b) != qec64_f(b)) return (uint32_t)b | 0x00400000u;
    return r != r ? 0x7FC00000u : qec64_fb(r);
}
static inline uint64_t qec64_arith_d(uint64_t a, uint64_t b, double r)
{
    if (qec64_d(a) != qec64_d(a)) return a | UINT64_C(0x0008000000000000);
    if (qec64_d(b) != qec64_d(b)) return b | UINT64_C(0x0008000000000000);
    return r != r ? UINT64_C(0x7FF8000000000000) : qec64_db(r);
}

/* D0 = op(D0, D1); returns 0 for an n it does not know (the instruction is then illegal) */
static inline int qec64_hostfp(uint64_t *d0, uint64_t d1, unsigned n)
{
    uint64_t a = *d0, r;
    switch (n) {
    case QEC64_FP_ADDS: r = qec64_arith_s(a, d1, qec64_f(a) + qec64_f(d1)); break;
    case QEC64_FP_SUBS: r = qec64_arith_s(a, d1 ^ 0x80000000u, qec64_f(a) - qec64_f(d1)); break;   /* a + -b, as compiler-rt */
    case QEC64_FP_MULS: r = qec64_arith_s(a, d1, qec64_f(a) * qec64_f(d1)); break;
    case QEC64_FP_DIVS: r = qec64_arith_s(a, d1, qec64_f(a) / qec64_f(d1)); break;
    case QEC64_FP_LES: r = qec64_cmp(qec64_f(a), qec64_f(d1), 1); break;
    case QEC64_FP_GES: r = qec64_cmp(qec64_f(a), qec64_f(d1), -1); break;
    case QEC64_FP_UNORDS: r = qec64_f(a) != qec64_f(a) || qec64_f(d1) != qec64_f(d1); break;
    case QEC64_FP_S2I: r = QEC64_SX32(qec64_fixint((uint32_t)a, 23, 8, 32)); break;
    case QEC64_FP_S2U: r = QEC64_SX32(qec64_fixuint((uint32_t)a, 23, 8, 32)); break;
    case QEC64_FP_S2L: r = qec64_fixint((uint32_t)a, 23, 8, 64); break;
    case QEC64_FP_S2UL: r = qec64_fixuint((uint32_t)a, 23, 8, 64); break;
    case QEC64_FP_I2S: r = qec64_fb((float)(int32_t)a); break;
    case QEC64_FP_U2S: r = qec64_fb((float)(uint32_t)a); break;
    case QEC64_FP_L2S: r = qec64_fb((float)(int64_t)a); break;
    case QEC64_FP_UL2S: r = qec64_fb((float)a); break;
    case QEC64_FP_S2D: r = qec64_db((double)qec64_f(a)); break;
    case QEC64_FP_SQRTS: r = qec64_f(a) != qec64_f(a) ? ((uint32_t)a | 0x00400000u) : qec64_f(a) < 0 ? 0x7FC00000u : qec64_fb(sqrtf(qec64_f(a))); break;
    case QEC64_FP_ADDD: r = qec64_arith_d(a, d1, qec64_d(a) + qec64_d(d1)); break;
    case QEC64_FP_SUBD: r = qec64_arith_d(a, d1 ^ UINT64_C(0x8000000000000000), qec64_d(a) - qec64_d(d1)); break;
    case QEC64_FP_MULD: r = qec64_arith_d(a, d1, qec64_d(a) * qec64_d(d1)); break;
    case QEC64_FP_DIVD: r = qec64_arith_d(a, d1, qec64_d(a) / qec64_d(d1)); break;
    case QEC64_FP_LED: r = qec64_cmp(qec64_d(a), qec64_d(d1), 1); break;
    case QEC64_FP_GED: r = qec64_cmp(qec64_d(a), qec64_d(d1), -1); break;
    case QEC64_FP_UNORDD: r = qec64_d(a) != qec64_d(a) || qec64_d(d1) != qec64_d(d1); break;
    case QEC64_FP_D2I: r = QEC64_SX32(qec64_fixint(a, 52, 11, 32)); break;
    case QEC64_FP_D2U: r = QEC64_SX32(qec64_fixuint(a, 52, 11, 32)); break;
    case QEC64_FP_D2L: r = qec64_fixint(a, 52, 11, 64); break;
    case QEC64_FP_D2UL: r = qec64_fixuint(a, 52, 11, 64); break;
    case QEC64_FP_I2D: r = qec64_db((double)(int32_t)a); break;
    case QEC64_FP_U2D: r = qec64_db((double)(uint32_t)a); break;
    case QEC64_FP_L2D: r = qec64_db((double)(int64_t)a); break;
    case QEC64_FP_UL2D: r = qec64_db((double)a); break;
    case QEC64_FP_D2S: r = qec64_fb((float)qec64_d(a)); break;
    case QEC64_FP_SQRTD: r = qec64_d(a) != qec64_d(a) ? (a | UINT64_C(0x0008000000000000)) : qec64_d(a) < 0 ? UINT64_C(0x7FF8000000000000) : qec64_db(sqrt(qec64_d(a))); break;
    default: return 0;
    }
    *d0 = r;
    return 1;
}
#endif
