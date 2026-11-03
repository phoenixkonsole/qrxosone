/* TEST ONLY: Debian m68k glibc/libgcc target 68020 and use 64-bit MUL/DIV,
 * absent on 68060. These equivalent C implementations let the QEMU test
 * harness run without an OS instruction emulator. Never link into AROS.
 * Timing remains real process CPU time, not a synthetic counter. */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

void *calloc(size_t n, size_t size)
{
    void *p;
    if (n && size > SIZE_MAX / n) return NULL;
    size_t bytes = n * size;
    p = malloc(bytes);
    if (p) memset(p, 0, bytes);
    return p;
}
clock_t clock(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts)) return (clock_t)-1;
    return (clock_t)((uint32_t)ts.tv_sec * 1000000u + (uint32_t)ts.tv_nsec / 1000u);
}
/* The same portable arithmetic runtime is used by the SDK adapter. */
#include "../core/qrx_int64_runtime.c"
