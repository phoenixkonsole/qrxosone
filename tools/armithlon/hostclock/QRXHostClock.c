/* QRXHostClock: sets the AROS system clock from the Mac's, through the ARMithlon host door
 * (network service 4, NET_TIME). ARMithlon emulates no battery clock, so without this the
 * system starts on 1 January 1978. The local time zone comes from the locale (GMT offset).
 * Build: aros-gcc -O2 -m68040 -fno-pic QRXHostClock.c -o QRXHostClock (then elf2hunk). */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/locale.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <libraries/locale.h>
#include <stdint.h>
#include <string.h>

#define DOOR_MAGIC_REG 0xFF000880u
#define DOOR_CALL_REG 0xFF00088Cu
#define DOOR_FEATURES_REG 0xFF000890u
#define DOOR_MAGIC 0x51383644u
#define DOOR_FEATURE_NATIVE_NET 1u
#define SVC_NET 4u
#define NET_TIME 20u
#define AMIGA_EPOCH 252460800u   /* 1978-01-01 in Unix time */

struct LocaleBase *LocaleBase;

int main(void)
{
    uint32_t magic, features, q[32] __attribute__((aligned(4)));
    /* Whole 32-bit reads: a byte access to the door faults the emulator. */
    __asm__ volatile("move.l 0xFF000880,%0" : "=d"(magic));
    if (magic != DOOR_MAGIC) { PutStr("QRXHostClock: no ARMithlon host door\n"); return 5; }
    __asm__ volatile("move.l 0xFF000890,%0" : "=d"(features));
    if (!(features & DOOR_FEATURE_NATIVE_NET)) { PutStr("QRXHostClock: the host door has no clock\n"); return 5; }
    memset(q, 0, sizeof q);
    q[0] = SVC_NET; q[3] = NET_TIME;
    q[12] = UINT32_MAX;
    *(volatile uint32_t *)DOOR_CALL_REG = (uint32_t)(uintptr_t)q;
    __asm__ volatile("" ::: "memory");
    if (q[12] == UINT32_MAX || q[14] < AMIGA_EPOCH) { PutStr("QRXHostClock: no time from the host\n"); return 5; }
    int32_t west = 0;   /* minutes west of GMT */
    if (q[16]) west = -(int32_t)q[16] / 60;   /* the Mac's own offset, from newer engines */
    else if ((LocaleBase = (struct LocaleBase *)OpenLibrary("locale.library", 38))) {
        struct Locale *l = OpenLocale(NULL);
        if (l) { west = l->loc_GMTOffset; CloseLocale(l); }
        CloseLibrary((struct Library *)LocaleBase);
    }
    struct MsgPort *port = CreateMsgPort();
    struct timerequest *tr = port ? (struct timerequest *)CreateIORequest(port, sizeof *tr) : NULL;
    int rc = 5;
    if (tr && !OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *)tr, 0)) {
        tr->tr_node.io_Command = TR_SETSYSTIME;
        tr->tr_time.tv_secs = q[14] - AMIGA_EPOCH - west * 60;
        tr->tr_time.tv_micro = q[15];
        if (!DoIO((struct IORequest *)tr)) rc = 0;
        CloseDevice((struct IORequest *)tr);
    }
    if (tr) DeleteIORequest((struct IORequest *)tr);
    if (port) DeleteMsgPort(port);
    if (rc) PutStr("QRXHostClock: could not set the clock\n");
    return rc;
}
