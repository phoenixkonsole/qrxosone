/* The FPU's registers go with a task (kernel_cpu.c, m68k64_FPUSave/Load): four of these at once on
 * two cores, each keeping its own value in FP0 and FP7 while the others run. Without the save,
 * some come back wrong ("2 bad"); with it none. Run: Run fpuswitch 1, Run fpuswitch 2, ... */
#include <stdio.h>
#include <stdlib.h>
#include <proto/dos.h>
static long fp_roundtrip(long v)
{
    long out;
    /* FMOVE.L D0,FP0 ... FP7 (via FP0 copies), spin, FADD FP7,FP0 ... then FMOVE.L FP0,D1 */
    register long d0 asm("d0") = v;
    register long d1 asm("d1");
    asm volatile(
        ".short 0xF200, 0x4000\n"      /* FMOVE.L D0,FP0 */
        ".short 0xF200, 0x0380\n"      /* FMOVE FP0,FP7 */
        : : "d"(d0) : "memory");
    for (volatile long k = 0; k < 20000; k++) {}   /* the other task runs meanwhile */
    asm volatile(
        ".short 0xF200, 0x1C28\n"      /* FSUB FP7,FP0: 0 */
        ".short 0xF200, 0x1C22\n"      /* FADD FP7,FP0: v again */
        ".short 0xF201, 0x6000\n"      /* FMOVE.L FP0,D1 */
        : "=d"(d1) : : "cc", "memory");
    out = d1;
    return out;
}
int main(int argc, char **argv)
{
    long base = argc > 1 ? atol(argv[1]) : 1, bad = 0;
    for (long i = 0; i < 200; i++) {
        long v = base * 1000 + i;
        if (fp_roundtrip(v) != v) bad++;
        if (i % 50 == 0) Delay(1);
    }
    printf("fpuswitch %ld: %ld bad\n", base, bad);
    return bad ? 20 : 0;
}
