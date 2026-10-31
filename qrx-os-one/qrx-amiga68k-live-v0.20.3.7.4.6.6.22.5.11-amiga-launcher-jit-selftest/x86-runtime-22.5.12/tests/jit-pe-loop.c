/* Synthetic PE32 register loop plus an actual ExitProcess HLE transition.
 * Not an application/Doom benchmark. Build with i686-w64-mingw32-gcc:
 * -O2 -nostdlib -Wl,--entry,_qrx_entry -o jit-pe-loop.exe jit-pe-loop.c -lkernel32 */
#include <windows.h>
void qrx_entry(void)
{
    unsigned a,d,b;
    __asm__ volatile (
        "movl $2000000,%%ecx\n\t"
        "xorl %%eax,%%eax\n\t"
        "xorl %%edx,%%edx\n\t"
        "xorl %%ebx,%%ebx\n\t"
        "1: addl $3,%%eax\n\t"
        "xorl %%eax,%%edx\n\t"
        "addl %%edx,%%ebx\n\t"
        "subl $1,%%ecx\n\t"
        "jnz 1b\n\t"
        : "=a"(a), "=d"(d), "=b"(b) : : "ecx", "cc");
    /* Reference constants generated independently with unsigned arithmetic. */
    ExitProcess(a == 6000000u && d == 8192256u && b == 1184935552u ? 42 : 99);
}
