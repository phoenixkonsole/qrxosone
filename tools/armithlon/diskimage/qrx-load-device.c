/*
 * QRX OS One: keep DEVS:diskimage.device resident and open units 0..9 for
 * the native mount tools. A diskimage unit is destroyed (and its image is
 * ejected) when its last opener closes it. The mount command is short-lived,
 * so this helper keeps the units alive while DOS handlers and QRXDesktop use
 * them.
 */
#include <exec/exec.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define QRX_IMAGE_UNITS 10

int main(void)
{
    struct MsgPort *port[QRX_IMAGE_UNITS] = { 0 };
    struct IOStdReq *io[QRX_IMAGE_UNITS] = { 0 };
    ULONG unit;
    LONG err;

    for (unit = 0; unit < QRX_IMAGE_UNITS; unit++) {
        port[unit] = CreateMsgPort();
        io[unit] = port[unit] ?
            (struct IOStdReq *)CreateIORequest(port[unit], sizeof(struct IOStdReq)) : NULL;
        if (io[unit] == NULL) {
            Printf((STRPTR)"QRXLoadDevice: no memory for unit %ld\n", (LONG)unit);
            continue;
        }
        err = OpenDevice((STRPTR)"diskimage.device", unit, (struct IORequest *)io[unit], 0);
        if (err != 0) {
            Printf((STRPTR)"QRXLoadDevice: cannot open unit %ld (error %ld)\n", (LONG)unit, err);
            DeleteIORequest((struct IORequest *)io[unit]);
            DeleteMsgPort(port[unit]);
            io[unit] = NULL;
            port[unit] = NULL;
        }
    }

    for (;;)
        Delay(100);
}
