/* QRX OS One 22.5.12 - x87 differential test (build: gcc -m32 -static -O1 -mfpmath=387).
 * Prints every result as an exact hex double; run natively and under qrx-i386 and
 * compare with compare-fpu.py (exact for + - * / sqrt and conversions, a few ulp
 * for transcendental instructions because qrx86core uses host double libm). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
/* note: static i386 libm.a on the build host lacks fmod, so a local helper is used */
static double md(double a, double b) { return a - b * (double)(long long)(a / b); }
static uint32_t seed = 22512;
static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 1; }
static double rd(void) {
    double v = (double)(int32_t)rnd() / (double)(rnd() % 100000 + 1);
    switch (rnd() % 6) { case 0: return v * 1e-30; case 1: return v * 1e30; case 2: return v; case 3: return (int)v; default: return v / 7.0; }
}
#define OP2(name, ins) static double name(double a, double b) { double r; __asm__ volatile ("fldl %2\n\tfldl %1\n\t" ins "\n\tfstpl %0" : "=m"(r) : "m"(a), "m"(b) : "eax"); return r; }
OP2(f_add, "faddp") OP2(f_sub, "fsubp") OP2(f_subr, "fsubrp") OP2(f_mul, "fmulp") OP2(f_div, "fdivp") OP2(f_divr, "fdivrp")
/* fprem/fprem1 are used in the documented loop until C2 = 0 (partial remainders differ by design) */
OP2(f_prem, "1: fprem\n\tfnstsw %%ax\n\ttestw $0x400, %%ax\n\tjnz 1b\n\tfstp %%st(1)") OP2(f_prem1, "1: fprem1\n\tfnstsw %%ax\n\ttestw $0x400, %%ax\n\tjnz 1b\n\tfstp %%st(1)") OP2(f_scale, "fscale\n\tfstp %%st(1)")
OP2(f_patan, "fpatan") OP2(f_yl2x, "fyl2x")
#define OP1(name, ins) static double name(double a) { double r; __asm__ volatile ("fldl %1\n\t" ins "\n\tfstpl %0" : "=m"(r) : "m"(a)); return r; }
OP1(f_sqrt, "fsqrt") OP1(f_abs, "fabs") OP1(f_chs, "fchs") OP1(f_rnd, "frndint") OP1(f_sin, "fsin") OP1(f_cos, "fcos")
OP1(f_tan, "fptan\n\tfstp %%st(0)") OP1(f_2xm1, "f2xm1") OP1(f_xtract, "fxtract\n\tfstp %%st(1)") OP1(f_xtracte, "fxtract\n\tfstp %%st(0)")
static int32_t f_ist(double a, uint16_t cw) { int32_t r; uint16_t o; __asm__ volatile ("fnstcw %1\n\tfldcw %2\n\tfldl %3\n\tfistpl %0\n\tfldcw %1" : "=m"(r), "=m"(o) : "m"(cw), "m"(a)); return r; }
static int64_t f_ist64(double a) { int64_t r; __asm__ volatile ("fldl %1\n\tfistpll %0" : "=m"(r) : "m"(a)); return r; }
static int16_t f_ist16(double a) { int16_t r; __asm__ volatile ("fldl %1\n\tfistps %0" : "=m"(r) : "m"(a)); return r; }
static float f_flt(double a) { float r; __asm__ volatile ("fldl %1\n\tfstps %0" : "=m"(r) : "m"(a)); return r; }
static uint16_t f_com(double a, double b) { uint16_t sw; __asm__ volatile ("fldl %2\n\tfldl %1\n\tfcompp\n\tfnstsw %0" : "=m"(sw) : "m"(a), "m"(b)); return sw & 0x4500; }
static uint32_t f_comi(double a, double b) { uint32_t fl; __asm__ volatile ("fldl %2\n\tfldl %1\n\tfcomip %%st(1), %%st\n\tfstp %%st(0)\n\tpushfl\n\tpopl %0" : "=r"(fl) : "m"(a), "m"(b)); return fl & 0x45; }
static uint16_t f_xam(double a) { uint16_t sw; __asm__ volatile ("fldl %1\n\tfxam\n\tfnstsw %0\n\tfstp %%st(0)" : "=m"(sw) : "m"(a)); return sw & 0x4700; }
static double f_ld80(double a) { unsigned char t[10]; double r; __asm__ volatile ("fldl %2\n\tfstpt %1\n\tfldt %1\n\tfstpl %0" : "=m"(r), "=m"(t) : "m"(a)); return r; }
static double f_cmov(double a, double b, int c) { double r; __asm__ volatile ("fldl %2\n\tfldl %1\n\tcmpl $0, %3\n\tfcmove %%st(1), %%st\n\tfstp %%st(1)\n\tfstpl %0" : "=m"(r) : "m"(a), "m"(b), "r"(c)); return r; }
static void P(const char *t, double v) { uint64_t u; memcpy(&u, &v, 8); printf("%s %016llx\n", t, (unsigned long long)u); }
int main(void)
{
    int i;
    uint16_t cw = 0x027F;     /* double precision so native and emulated rounding agree */
    __asm__ volatile ("fldcw %0" :: "m"(cw));
    for (i = 0; i < 400; i++) {
        double a = rd(), b = rd(), pa = fabs(a) + 1e-3;
        P("add", f_add(a, b)); P("sub", f_sub(a, b)); P("subr", f_subr(a, b)); P("mul", f_mul(a, b));
        P("div", f_div(a, b)); P("divr", f_divr(a, b)); P("sqrt", f_sqrt(pa)); P("abs", f_abs(a)); P("chs", f_chs(a));
        P("rnd", f_rnd(a)); P("flt", f_flt(a)); P("ld80", f_ld80(a)); P("xtract", f_xtract(a)); P("xtracte", f_xtracte(a));
        P("prem", f_prem(md(a, 1e6), b)); P("prem1", f_prem1(md(a, 1e6), b)); P("scale", f_scale(a, (double)(int)md(b, 60)));
        P("cmov", f_cmov(a, b, i & 1));
        printf("ist %d %d %d %d %lld %d\n", f_ist(md(a, 2e9), 0x027F), f_ist(md(a, 2e9), 0x067F), f_ist(md(a, 2e9), 0x0A7F), f_ist(md(a, 2e9), 0x0E7F), (long long)f_ist64(md(a, 9e18)), f_ist16(md(a, 3e4)));
        printf("com %04x %04x %04x %02x %02x\n", f_com(a, b), f_com(a, a), f_xam(a), f_comi(a, b), f_comi(b, b));
        P("~sin", f_sin(md(a, 100))); P("~cos", f_cos(md(a, 100))); P("big-sin", f_sin(a * 1e25)); P("inf-sin", f_sin(a * 1e300 * 1e10)); P("~tan", f_tan(md(a, 1.5)));
        P("~patan", f_patan(a, b)); P("~yl2x", f_yl2x(pa, b)); P("~2xm1", f_2xm1(a / (1.0 + fabs(a))));
    }
    P("~exp", exp(1.5)); P("~log", log(10.0)); P("~pow", pow(2.5, 3.7)); P("~atan2", atan2(1.0, 3.0));
    printf("xam-special %04x %04x %04x\n", f_xam(0.0), f_xam(-INFINITY), f_xam(NAN));
    return 0;
}
