/* QRX OS One 22.5.12 - qmath: deterministic double-precision math for the x86 runtime.
 * Sources: musl 1.2.5 src/math (MIT, COPYRIGHT.musl), renamed to qm_* so that the
 * host libm is never linked in by accident (AROS m68k libm floor/ceil/frexp/fmod/
 * remainder return wrong values; found by the x87 differential test on the target). */
#ifndef QMATH_H
#define QMATH_H
#define hidden
#define sin qm_sin
#define cos qm_cos
#define tan qm_tan
#define atan qm_atan
#define atan2 qm_atan2
#define asin qm_asin
#define acos qm_acos
#define exp qm_exp
#define log qm_log
#define log10 qm_log10
#define pow qm_pow
#define sinh qm_sinh
#define cosh qm_cosh
#define tanh qm_tanh
#define expm1 qm_expm1
#define log1p qm_log1p
#define floor qm_floor
#define ceil qm_ceil
#define trunc qm_trunc
#define frexp qm_frexp
#define scalbn qm_scalbn
#define ldexp qm_ldexp
#define fmod qm_fmod
#define remainder qm_remainder
#define remquo qm_remquo
#define modf qm_modf
#define rint qm_rint
#define sqrt qm_sqrt
#define __sin qm__sin
#define __cos qm__cos
#define __tan qm__tan
#define __rem_pio2 qm__rem_pio2
#define __rem_pio2_large qm__rem_pio2_large
#define __expo2 qm__expo2
#define __math_divzero qm__math_divzero
#define __math_invalid qm__math_invalid
#define __math_oflow qm__math_oflow
#define __math_uflow qm__math_uflow
#define __math_xflow qm__math_xflow
#define __exp_data qm__exp_data
#define __log_data qm__log_data
#define __pow_log_data qm__pow_log_data
double qm_sin(double); double qm_cos(double); double qm_tan(double); double qm_atan(double); double qm_atan2(double, double);
double qm_asin(double); double qm_acos(double); double qm_exp(double); double qm_log(double); double qm_log10(double); double qm_pow(double, double);
double qm_sinh(double); double qm_cosh(double); double qm_tanh(double); double qm_expm1(double); double qm_log1p(double);
double qm_floor(double); double qm_ceil(double); double qm_trunc(double); double qm_frexp(double, int *); double qm_scalbn(double, int);
double qm_ldexp(double, int); double qm_fmod(double, double); double qm_remainder(double, double); double qm_remquo(double, double, int *);
double qm_modf(double, double *); double qm_rint(double); double qm_sqrt(double);
/* classification by bit pattern: AROS m68k <math.h> isinf() was found to misreport -inf */
#include <stdint.h>
#include <string.h>
static inline uint64_t qm_bits(double x) { uint64_t u; memcpy(&u, &x, 8); return u; }
static inline int qm_isnan(double x) { return (qm_bits(x) << 1) > 0xFFE0000000000000ull; }
static inline int qm_isinf(double x) { return (qm_bits(x) << 1) == 0xFFE0000000000000ull; }
static inline int qm_isfinite(double x) { return (qm_bits(x) << 1) < 0xFFE0000000000000ull; }
static inline int qm_signbit(double x) { return (int)(qm_bits(x) >> 63); }
#ifndef QMATH_SYS_MATH_H
#undef isnan
#undef isinf
#undef isfinite
#undef signbit
#define isnan(x) qm_isnan(x)
#define isinf(x) qm_isinf(x)
#define isfinite(x) qm_isfinite(x)
#define signbit(x) qm_signbit(x)
#endif
#endif
