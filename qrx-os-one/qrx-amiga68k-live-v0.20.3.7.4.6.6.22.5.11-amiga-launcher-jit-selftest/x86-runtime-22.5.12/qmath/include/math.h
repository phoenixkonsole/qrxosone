/* QRX qmath: minimal <math.h> for the musl-derived sources in this directory only
 * (keeps the host's math.h - and its own __sin/__cos names - out of the build). */
#ifndef QMATH_SYS_MATH_H
#define QMATH_SYS_MATH_H
#include <stdint.h>
typedef double double_t;
typedef float float_t;
#define NAN       (__builtin_nanf(""))
#define INFINITY  (__builtin_inff())
#define HUGE_VAL  (__builtin_inf())
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
static inline uint64_t qm__bits(double x) { union { double f; uint64_t i; } u; u.f = x; return u.i; }
static inline int qm__fpclassify(double x)
{
    uint64_t i = qm__bits(x);
    int e = (int)(i >> 52 & 0x7ff);
    if (!e) return i << 1 ? FP_SUBNORMAL : FP_ZERO;
    if (e == 0x7ff) return i << 12 ? FP_NAN : FP_INFINITE;
    return FP_NORMAL;
}
#define fpclassify(x) qm__fpclassify(x)
#define isnan(x)      (qm__fpclassify(x) == FP_NAN)
#define isinf(x)      (qm__fpclassify(x) == FP_INFINITE)
#define isfinite(x)   (qm__fpclassify(x) > FP_INFINITE)
#define isnormal(x)   (qm__fpclassify(x) == FP_NORMAL)
#define signbit(x)    ((int)(qm__bits(x) >> 63))
double sqrt(double);
double fabs(double);
#define sqrt(x) __builtin_sqrt(x)
#define fabs(x) __builtin_fabs(x)
#endif
