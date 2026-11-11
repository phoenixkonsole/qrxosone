/* Tests for compiler-rt's builtins on m68k64: soft float (the core has no FPU) and 128-bit
 * integers. Built for the host too (HOST_TEST); results are bit patterns, so they must match the
 * host's IEEE arithmetic exactly. See run-rt-tests.sh. */
typedef long long i64;
typedef unsigned long long u64;

static u64 dbits(double d) { union { double d; u64 u; } x = { d }; return x.u; }
static u64 fbits(float f) { union { float f; unsigned u; } x = { f }; return x.u; }
static u64 mix(u64 h, u64 v) { return (h ^ v) * 0x100000001b3ull + (h >> 29); }

/* volatile so the compiler cannot fold the arithmetic away at build time */
static volatile double dv[] = { 1.0, -2.5, 3.141592653589793, 1e300, -1e-300, 0.1, 7.0, 123456789.125 };
static volatile float fv[] = { 1.0f, -2.5f, 3.1415927f, 1e30f, -1e-30f, 0.1f, 7.0f, 12345.125f };

i64 test_double_arith(void) {
    u64 h = 0;
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            double a = dv[i], b = dv[j];
            h = mix(h, dbits(a + b)); h = mix(h, dbits(a - b));
            h = mix(h, dbits(a * b)); h = mix(h, dbits(a / b));
        }
    return (i64)h;
}
i64 test_float_arith(void) {
    u64 h = 0;
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            float a = fv[i], b = fv[j];
            h = mix(h, fbits(a + b)); h = mix(h, fbits(a - b));
            h = mix(h, fbits(a * b)); h = mix(h, fbits(a / b));
        }
    return (i64)h;
}
i64 test_compare(void) {
    u64 h = 0;
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            h = h * 7 + (dv[i] < dv[j]) * 4 + (dv[i] == dv[j]) * 2 + (fv[i] >= fv[j]);
        }
    return (i64)h;
}
i64 test_convert(void) {
    u64 h = 0;
    for (int i = 0; i < 8; i++) {
        double d = dv[i];
        if (d > -1e18 && d < 1e18) { h = mix(h, (u64)(i64)d); h = mix(h, (u64)(int)(float)d); }
        h = mix(h, fbits((float)d));
        h = mix(h, dbits((double)fv[i]));
        h = mix(h, dbits((double)(i64)(i * 1234567891011ll - 5)));
        h = mix(h, dbits((double)(u64)(0xfedcba9876543210ull >> i)));
        h = mix(h, fbits((float)(i64)(i * 98765431ll)));
    }
    return (i64)h;
}
i64 test_int128(void) {
    static volatile u64 a[] = { 0x123456789abcdef0ull, 3, 0xffffffffffffffffull, 0x8000000000000001ull };
    u64 h = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            unsigned __int128 x = ((unsigned __int128)a[i] << 64) | a[j];
            unsigned __int128 y = (unsigned __int128)a[j] * a[i] + 1;
            h = mix(h, (u64)(x * y)); h = mix(h, (u64)((x * y) >> 64));
            h = mix(h, (u64)(x / y)); h = mix(h, (u64)(x % y));
            h = mix(h, (u64)(x >> (i * 17 + j))); h = mix(h, (u64)((x << (j * 19 + i)) >> 64));
            __int128 s = (__int128)x;
            h = mix(h, (u64)(s >> (i * 31 + 1)));
            h = mix(h, (u64)(s / (__int128)(i64)(a[j] | 1)));
        }
    return (i64)h;
}
#ifdef HOST_TEST
#include <stdio.h>
#define T(n) printf(#n " %016llx\n", (unsigned long long)test_##n());
int main(void) { T(double_arith) T(float_arith) T(compare) T(convert) T(int128) return 0; }
#endif
