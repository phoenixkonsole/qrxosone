/* QRXSystem 32|64: which QRX OS One the Raspberry Pi appliance starts, then a restart. Through the
 * engine's host control service (door service 11, qrx_hostctl.c) and the image's /qrx/netctl.
 * Without an argument it prints the usage. */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

static ULONG q[32] __attribute__((aligned(16)));
static char args[64] __attribute__((aligned(16)));
static char out[512] __attribute__((aligned(16)));

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "32") && strcmp(argv[1], "64"))) {
        PutStr("QRXSystem 32|64: start the 32-bit QRX OS One or QRX OS One 64 next (Raspberry Pi)\n");
        return 5;
    }
#if defined(__m68k64__)
    if (*(volatile UQUAD *)0xFFFFFFFF00000000UL != 0x5152584543363442UL) { PutStr("no ARMithlon board\n"); return 10; }
#else
    if (*(volatile ULONG *)0xFF000880UL != 0x51383644UL) { PutStr("no ARMithlon door\n"); return 10; }
#endif
    memset(args, 0, sizeof args);
    strcpy(args, "system");
    strcpy(args + 7, argv[1]);
    q[0] = 11; q[4] = (ULONG)(IPTR)args; q[5] = (ULONG)(IPTR)out; q[6] = sizeof out;
#if defined(__m68k64__)
    *(volatile UQUAD *)0xFFFFFFFF00000400UL = (UQUAD)(IPTR)q;
#else
    *(volatile ULONG *)0xFF00088CUL = (ULONG)(IPTR)q;
#endif
    __asm__ volatile("" ::: "memory");
    if (q[12] == 0xFFFFFFFFUL) { PutStr("not on the Raspberry Pi appliance: nothing to switch\n"); return 10; }
    PutStr(out);
    return q[13] ? 10 : 0;
}
