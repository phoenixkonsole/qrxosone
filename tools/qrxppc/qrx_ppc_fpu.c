/*
 * QRXPPC P1.3 floating-point helpers.
 *
 * The PPC32 G4 keeps single values in double format in the FPRs and uses
 * FPSCR rounding modes for arithmetic and conversions.  This file implements
 * the arithmetic with integer significands so the host and the 68k guest
 * produce the same bits; it deliberately does not depend on the host FPU
 * rounding mode.
 */
#include "qrx_ppc_fpu.h"

#include <string.h>

#define FP_FRAC       0x000FFFFFFFFFFFFFull
#define FP_SIGN       0x8000000000000000ull
#define FP_QNAN_BIT    0x0008000000000000ull
#define FP_QNAN        0x7FF8000000000000ull
#define FP_INF         0x7FF0000000000000ull
#define FP_MIN_NORMAL  0x0010000000000000ull

typedef struct {
    unsigned sign;
    int exp;            /* exponent of the implicit leading bit */
    uint64_t mant;      /* normalised, bit 63 set */
    int zero;
    int inf;
    int nan;
    int snan;
} fp_dec_t;

static void fp_decode(uint64_t bits, fp_dec_t *v)
{
    unsigned e = (unsigned)((bits >> 52) & 0x7FFu);
    uint64_t frac = bits & FP_FRAC;

    memset(v, 0, sizeof(*v));
    v->sign = (unsigned)(bits >> 63);
    if (e == 0x7FFu) {
        v->nan = frac != 0;
        v->inf = frac == 0;
        v->snan = v->nan && (frac & FP_QNAN_BIT) == 0;
        return;
    }
    if (e == 0) {
        if (frac == 0) {
            v->zero = 1;
            return;
        }
        v->exp = -1022;
        v->mant = frac;
        while ((v->mant & (1ull << 52)) == 0) {
            v->mant <<= 1;
            v->exp--;
        }
        v->mant <<= 11;
        return;
    }
    v->exp = (int)e - 1023;
    v->mant = ((1ull << 52) | frac) << 11;
}

static void fp_raise(qrx_ppc_t *p, uint32_t flags)
{
    p->cpu.fpscr |= QRX_FPSCR_FX | QRX_FPSCR_FEX | flags;
}

static void fp_invalid(qrx_ppc_t *p, uint32_t detail)
{
    fp_raise(p, QRX_FPSCR_VX | detail);
}

static void fp_inexact(qrx_ppc_t *p, int increment)
{
    uint32_t flags = QRX_FPSCR_XX | QRX_FPSCR_FR;

    if (increment)
        flags |= QRX_FPSCR_FI;
    fp_raise(p, flags);
}

static void fp_overflow(qrx_ppc_t *p)
{
    fp_raise(p, QRX_FPSCR_OX | QRX_FPSCR_XX | QRX_FPSCR_FR);
}

static void fp_underflow(qrx_ppc_t *p)
{
    fp_raise(p, QRX_FPSCR_UX | QRX_FPSCR_XX | QRX_FPSCR_FR);
}

static void fp_divzero(qrx_ppc_t *p)
{
    fp_raise(p, QRX_FPSCR_ZX);
}

static int fp_round_increment(unsigned mode, unsigned sign, int guard,
                              int sticky, uint64_t sig)
{
    if (!guard && !sticky)
        return 0;
    switch (mode) {
    case 0: /* round to nearest, ties to even */
        return guard && (sticky || (sig & 1u));
    case 1: /* round toward zero */
        return 0;
    case 2: /* round toward +infinity */
        return !sign;
    default: /* round toward -infinity */
        return sign;
    }
}

static uint64_t fp_pack_round(qrx_ppc_t *p, unsigned sign, int exp,
                              uint64_t mant, int sticky)
{
    unsigned mode = (unsigned)(p->cpu.fpscr & QRX_FPSCR_RN_MASK);
    uint64_t result;
    uint64_t sig;
    int guard = 0;
    int round_up = 0;
    int inexact = 0;
    int overflow = 0;
    int underflow = 0;

    if (mant == 0) {
        if (sticky)
            fp_inexact(p, 0);
        return (uint64_t)sign << 63;
    }

    if (exp > 1023) {
        overflow = 1;
        result = ((uint64_t)sign << 63) | FP_INF;
    } else if (exp >= -1022) {
        sig = mant >> 11;
        guard = (int)((mant >> 10) & 1u);
        sticky |= (mant & 0x3FFu) != 0;
        inexact = guard || sticky;
        round_up = fp_round_increment(mode, sign, guard, sticky, sig);
        if (round_up) {
            sig++;
            if (sig == (1ull << 53)) {
                sig >>= 1;
                exp++;
            }
        }
        if (exp > 1023) {
            overflow = 1;
            result = ((uint64_t)sign << 63) | FP_INF;
        } else {
            result = ((uint64_t)sign << 63) |
                     ((uint64_t)(exp + 1023) << 52) |
                     (sig & FP_FRAC);
        }
    } else {
        int rshift = -(exp + 1011);

        sig = 0;
        if (rshift < 64) {
            sig = mant >> rshift;
            guard = (int)((mant >> (rshift - 1)) & 1u);
            if (rshift > 1)
                sticky |= (mant & ((1ull << (rshift - 1)) - 1u)) != 0;
        } else {
            guard = (rshift == 64) ? (int)((mant >> 63) & 1u) : 0;
            if (rshift == 64)
                sticky |= (mant & 0x7FFFFFFFFFFFFFFFull) != 0;
            else
                sticky |= mant != 0;
        }
        inexact = guard || sticky;
        round_up = fp_round_increment(mode, sign, guard, sticky, sig);
        if (round_up)
            sig++;
        if (sig >= (1ull << 52))
            result = ((uint64_t)sign << 63) | FP_MIN_NORMAL;
        else
            result = ((uint64_t)sign << 63) | sig;
        underflow = inexact;
    }

    if (overflow)
        fp_overflow(p);
    else if (underflow)
        fp_underflow(p);
    else if (inexact)
        fp_inexact(p, round_up);
    return result;
}

static uint32_t fp_pack_single(qrx_ppc_t *p, unsigned sign, int exp,
                               uint64_t mant, int sticky)
{
    unsigned mode = (unsigned)(p->cpu.fpscr & QRX_FPSCR_RN_MASK);
    uint32_t result;
    uint64_t sig;
    int guard = 0;
    int round_up = 0;
    int inexact = 0;
    int overflow = 0;
    int underflow = 0;

    if (mant == 0) {
        if (sticky)
            fp_inexact(p, 0);
        return (uint32_t)sign << 31;
    }

    if (exp > 127) {
        overflow = 1;
        result = ((uint32_t)sign << 31) | 0x7F800000u;
    } else if (exp >= -126) {
        sig = mant >> 40;
        guard = (int)((mant >> 39) & 1u);
        sticky |= (mant & 0x7FFFFFFFFFull) != 0;
        inexact = guard || sticky;
        round_up = fp_round_increment(mode, sign, guard, sticky, sig);
        if (round_up) {
            sig++;
            if (sig == (1ull << 24)) {
                sig >>= 1;
                exp++;
            }
        }
        if (exp > 127) {
            overflow = 1;
            result = ((uint32_t)sign << 31) | 0x7F800000u;
        } else {
            result = ((uint32_t)sign << 31) |
                     ((uint32_t)(exp + 127) << 23) |
                     ((uint32_t)sig & 0x7FFFFFu);
        }
    } else {
        int rshift = -(exp + 86);

        sig = 0;
        if (rshift < 64) {
            sig = mant >> rshift;
            guard = (int)((mant >> (rshift - 1)) & 1u);
            if (rshift > 1)
                sticky |= (mant & ((1ull << (rshift - 1)) - 1u)) != 0;
        } else {
            guard = (rshift == 64) ? (int)((mant >> 63) & 1u) : 0;
            if (rshift == 64)
                sticky |= (mant & 0x7FFFFFFFFFFFFFFFull) != 0;
            else
                sticky |= mant != 0;
        }
        inexact = guard || sticky;
        round_up = fp_round_increment(mode, sign, guard, sticky, sig);
        if (round_up)
            sig++;
        if (sig >= (1ull << 23))
            result = ((uint32_t)sign << 31) | 0x00800000u;
        else
            result = ((uint32_t)sign << 31) | (uint32_t)sig;
        underflow = inexact;
    }

    if (overflow)
        fp_overflow(p);
    else if (underflow)
        fp_underflow(p);
    else if (inexact)
        fp_inexact(p, round_up);
    return result;
}

static uint64_t fp_single_to_double(uint32_t single)
{
    float f;
    double d;
    uint64_t bits;

    memcpy(&f, &single, sizeof(f));
    d = (double)f;
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

static int fp_nan_bits(uint64_t bits)
{
    return ((bits >> 52) & 0x7FFu) == 0x7FFu &&
           (bits & FP_FRAC) != 0;
}

static int fp_snan_bits(uint64_t bits)
{
    return fp_nan_bits(bits) && (bits & FP_QNAN_BIT) == 0;
}

static uint64_t fp_shift_right(uint64_t mant, unsigned shift, int *sticky)
{
    if (shift == 0)
        return mant;
    if (shift >= 64) {
        *sticky |= mant != 0;
        return 0;
    }
    if ((mant & ((1ull << shift) - 1u)) != 0)
        *sticky = 1;
    return mant >> shift;
}

uint64_t qrx_fp_add(qrx_ppc_t *p, uint64_t a, uint64_t b, int subtract)
{
    fp_dec_t va, vb;
    uint64_t ma, mb;
    uint64_t mant;
    int exp;
    int sticky = 0;
    unsigned sign;

    fp_decode(a, &va);
    fp_decode(b, &vb);
    if (va.snan || vb.snan) {
        fp_invalid(p, QRX_FPSCR_VXSNAN);
        return FP_QNAN;
    }
    if (va.nan || vb.nan)
        return FP_QNAN;

    if (subtract)
        vb.sign ^= 1u;

    if (va.inf || vb.inf) {
        if (va.inf && vb.inf) {
            if (va.sign != vb.sign) {
                fp_invalid(p, QRX_FPSCR_VXISI);
                return FP_QNAN;
            }
            return ((uint64_t)va.sign << 63) | FP_INF;
        }
        if (va.inf)
            return ((uint64_t)va.sign << 63) | FP_INF;
        return ((uint64_t)vb.sign << 63) | FP_INF;
    }

    if (va.zero && vb.zero) {
        unsigned zero_sign;

        if (va.sign == vb.sign)
            zero_sign = va.sign;
        else
            zero_sign = (unsigned)((p->cpu.fpscr & QRX_FPSCR_RN_MASK) == 3u);
        return (uint64_t)zero_sign << 63;
    }
    if (va.zero)
        return (b & ~FP_SIGN) | ((uint64_t)vb.sign << 63);
    if (vb.zero)
        return (a & ~FP_SIGN) | ((uint64_t)va.sign << 63);

    if (va.exp < vb.exp) {
        fp_dec_t t = va;
        va = vb;
        vb = t;
    }

    exp = va.exp;
    ma = va.mant;
    mb = fp_shift_right(vb.mant, (unsigned)(va.exp - vb.exp), &sticky);

    if (va.sign == vb.sign) {
        uint64_t sum = ma + mb;

        sign = va.sign;
        if (sum < ma) {
            mant = (sum >> 1) | FP_SIGN;
            sticky |= (int)(sum & 1u);
            exp++;
        } else {
            mant = sum;
        }
    } else {
        if (ma < mb) {
            uint64_t t = ma;
            ma = mb;
            mb = t;
            sign = vb.sign;
        } else {
            sign = va.sign;
        }
        mant = ma - mb;
        if (mant == 0) {
            if (sticky)
                fp_inexact(p, 0);
            return (uint64_t)sign << 63;
        }
        while ((mant & FP_SIGN) == 0) {
            mant <<= 1;
            exp--;
        }
    }
    return fp_pack_round(p, sign, exp, mant, sticky);
}

static void fp_mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t a0 = a & 0xFFFFFFFFu;
    uint64_t a1 = a >> 32;
    uint64_t b0 = b & 0xFFFFFFFFu;
    uint64_t b1 = b >> 32;
    uint64_t p0 = a0 * b0;
    uint64_t p1 = a1 * b0;
    uint64_t p2 = a0 * b1;
    uint64_t p3 = a1 * b1;
    uint64_t t = p0 + (p1 << 32);
    uint64_t carry1 = t < p0;
    uint64_t u = t + (p2 << 32);
    uint64_t carry2 = u < t;

    *lo = u;
    *hi = p3 + (p1 >> 32) + (p2 >> 32) + carry1 + carry2;
}

uint64_t qrx_fp_mul(qrx_ppc_t *p, uint64_t a, uint64_t b)
{
    fp_dec_t va, vb;
    uint64_t hi, lo;
    uint64_t mant;
    int sticky;
    int exp;

    fp_decode(a, &va);
    fp_decode(b, &vb);
    if (va.snan || vb.snan) {
        fp_invalid(p, QRX_FPSCR_VXSNAN);
        return FP_QNAN;
    }
    if (va.nan || vb.nan)
        return FP_QNAN;
    if ((va.inf && vb.zero) || (va.zero && vb.inf)) {
        fp_invalid(p, QRX_FPSCR_VXIMZ);
        return FP_QNAN;
    }
    if (va.inf || vb.inf)
        return ((uint64_t)(va.sign ^ vb.sign) << 63) | FP_INF;
    if (va.zero || vb.zero)
        return (uint64_t)(va.sign ^ vb.sign) << 63;

    fp_mul64(va.mant, vb.mant, &hi, &lo);
    exp = va.exp + vb.exp;
    if ((hi & FP_SIGN) != 0) {
        mant = hi;
        exp++;
        sticky = lo != 0;
    } else {
        mant = (hi << 1) | (lo >> 63);
        sticky = (lo & 0x7FFFFFFFFFFFFFFFull) != 0;
    }
    return fp_pack_round(p, va.sign ^ vb.sign, exp, mant, sticky);
}

static uint64_t fp_div64(uint64_t num, uint64_t den, uint64_t *remainder)
{
    uint64_t q = 0;
    uint64_t r = num;
    unsigned i;

    for (i = 0; i < 64; i++) {
        uint64_t r2 = r << 1;
        int carry = (r >> 63) != 0;
        int bit;

        if (carry || r2 >= den) {
            r2 -= den;
            bit = 1;
        } else {
            bit = 0;
        }
        r = r2;
        q = (q << 1) | (uint64_t)bit;
    }
    *remainder = r;
    return q;
}

uint64_t qrx_fp_div(qrx_ppc_t *p, uint64_t a, uint64_t b)
{
    fp_dec_t va, vb;
    uint64_t q, r;
    uint64_t mant;
    int sticky;
    int exp;

    fp_decode(a, &va);
    fp_decode(b, &vb);
    if (va.snan || vb.snan) {
        fp_invalid(p, QRX_FPSCR_VXSNAN);
        return FP_QNAN;
    }
    if (va.nan || vb.nan)
        return FP_QNAN;
    if (va.inf && vb.inf) {
        fp_invalid(p, QRX_FPSCR_VXIDI);
        return FP_QNAN;
    }
    if (va.zero && vb.zero) {
        fp_invalid(p, QRX_FPSCR_VXZDZ);
        return FP_QNAN;
    }
    if (va.inf)
        return ((uint64_t)(va.sign ^ vb.sign) << 63) | FP_INF;
    if (vb.inf)
        return (uint64_t)(va.sign ^ vb.sign) << 63;
    if (vb.zero) {
        fp_divzero(p);
        return ((uint64_t)(va.sign ^ vb.sign) << 63) | FP_INF;
    }
    if (va.zero)
        return (uint64_t)(va.sign ^ vb.sign) << 63;

    exp = va.exp - vb.exp;
    if (va.mant < vb.mant) {
        q = fp_div64(va.mant, vb.mant, &r);
        mant = q;
        exp--;
        sticky = r != 0;
    } else {
        q = fp_div64(va.mant - vb.mant, vb.mant, &r);
        mant = FP_SIGN | (q >> 1);
        sticky = (int)((q & 1u) != 0 || r != 0);
    }
    return fp_pack_round(p, va.sign ^ vb.sign, exp, mant, sticky);
}

uint64_t qrx_fp_frsp(qrx_ppc_t *p, uint64_t a)
{
    fp_dec_t va;
    uint32_t single;
    uint64_t result;

    fp_decode(a, &va);
    if (va.snan) {
        fp_invalid(p, QRX_FPSCR_VXSNAN);
        qrx_fp_class(p, FP_QNAN);
        return FP_QNAN;
    }
    if (va.nan) {
        qrx_fp_class(p, FP_QNAN);
        return FP_QNAN;
    }
    if (va.inf) {
        result = (a & FP_SIGN) | FP_INF;
        qrx_fp_class(p, result);
        return result;
    }
    if (va.zero) {
        qrx_fp_class(p, a);
        return a;
    }

    single = fp_pack_single(p, va.sign, va.exp, va.mant, 0);
    result = fp_single_to_double(single);
    qrx_fp_class(p, result);
    return result;
}

uint32_t qrx_fp_fctiw(qrx_ppc_t *p, uint64_t a, int truncate)
{
    fp_dec_t va;
    unsigned mode;
    double d;
    int64_t truncated;
    double fraction;
    int64_t rounded;
    int increment = 0;
    int inexact;

    fp_decode(a, &va);
    if (va.snan) {
        fp_invalid(p, QRX_FPSCR_VXSNAN);
        return 0x80000000u;
    }
    if (va.nan || va.inf) {
        fp_invalid(p, QRX_FPSCR_VX);
        return 0x80000000u;
    }
    if (va.zero)
        return 0;

    memcpy(&d, &a, sizeof(d));
    if (d >= 2147483648.0 || d < -2147483649.0) {
        fp_invalid(p, QRX_FPSCR_VX);
        return 0x80000000u;
    }

    truncated = (int64_t)d;
    fraction = d - (double)truncated;
    inexact = fraction != 0.0;
    mode = truncate ? 1u : (unsigned)(p->cpu.fpscr & QRX_FPSCR_RN_MASK);
    if (inexact) {
        switch (mode) {
        case 0:
            if (fraction >= 0.5) {
                if (fraction > 0.5 || (truncated & 1) != 0)
                    increment = 1;
            } else if (fraction <= -0.5) {
                if (fraction < -0.5 || (truncated & 1) != 0)
                    increment = -1;
            }
            break;
        case 2:
            if (fraction > 0.0)
                increment = 1;
            break;
        case 3:
            if (fraction < 0.0)
                increment = -1;
            break;
        default:
            break;
        }
    }

    rounded = truncated + increment;
    if (rounded < -2147483648LL || rounded > 2147483647LL) {
        fp_invalid(p, QRX_FPSCR_VX);
        return 0x80000000u;
    }
    if (inexact)
        fp_inexact(p, increment != 0);
    return (uint32_t)(int32_t)rounded;
}

void qrx_fp_compare(qrx_ppc_t *p, unsigned field,
                    uint64_t a, uint64_t b, int ordered)
{
    uint32_t cr;
    uint32_t shift;
    uint32_t mask;

    if (fp_snan_bits(a) || fp_snan_bits(b))
        fp_invalid(p, QRX_FPSCR_VXSNAN);
    if (fp_nan_bits(a) || fp_nan_bits(b)) {
        cr = 1; /* unordered */
        if (ordered)
            fp_invalid(p, QRX_FPSCR_VXVC);
    } else {
        double x, y;

        memcpy(&x, &a, sizeof(x));
        memcpy(&y, &b, sizeof(y));
        if (x < y)
            cr = 8;
        else if (x > y)
            cr = 4;
        else
            cr = 2;
    }
    if (p->cpu.xer & 0x80000000u)
        cr |= 1;
    shift = 28u - field * 4u;
    mask = 0xFu << shift;
    p->cpu.cr = (p->cpu.cr & ~mask) | ((cr & 0xFu) << shift);
}

void qrx_fp_class(qrx_ppc_t *p, uint64_t bits)
{
    unsigned exp = (unsigned)((bits >> 52) & 0x7FFu);
    unsigned sign = (unsigned)(bits >> 63);
    uint64_t frac = bits & FP_FRAC;
    unsigned cls;

    if (exp == 0x7FFu)
        cls = frac ? 0x11u : (sign ? 0x09u : 0x05u);
    else if (exp == 0 && frac == 0)
        cls = sign ? 0x12u : 0x02u;
    else if (exp == 0)
        cls = sign ? 0x18u : 0x10u;
    else
        cls = sign ? 0x08u : 0x04u;
    p->cpu.fpscr = (p->cpu.fpscr & ~QRX_FPSCR_FPRF) | (cls << 12);
}

void qrx_fp_mtfsf(qrx_ppc_t *p, uint64_t src, unsigned fm)
{
    uint32_t mask = 0;
    unsigned i;

    for (i = 0; i < 8; i++) {
        if (fm & (1u << (7 - i)))
            mask |= 0xFu << (28 - i * 4);
    }
    p->cpu.fpscr = (p->cpu.fpscr & ~mask) |
                   ((uint32_t)src & mask);
}
