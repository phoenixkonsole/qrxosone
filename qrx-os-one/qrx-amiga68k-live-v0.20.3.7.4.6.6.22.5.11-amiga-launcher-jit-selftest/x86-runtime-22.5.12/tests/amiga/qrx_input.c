/* QRX OS One 22.5.12 test helper: inject real input events through input.device.
 *   QRXInput click X Y [right]     move the pointer to screen X,Y and click
 *   QRXInput move X Y
 *   QRXInput key RAWCODE           press + release an Amiga raw key (hex or decimal)
 * Used by the FS-UAE scenario to operate Win32 programs in their live Intuition
 * windows (the same path a user's mouse takes). */
#include <exec/types.h>
#include <exec/io.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct MsgPort *port;
static struct IOStdReq *io;

static void send(struct InputEvent *ie)
{
    io->io_Command = IND_WRITEEVENT;
    io->io_Data = ie;
    io->io_Length = sizeof(*ie);
    DoIO((struct IORequest *)io);
}

static void pointer(int x, int y)
{
    struct InputEvent ie;
    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = IECLASS_POINTERPOS;
    ie.ie_X = (WORD)x;
    ie.ie_Y = (WORD)y;
    send(&ie);
}

static void button(UWORD code)
{
    struct InputEvent ie;
    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = IECLASS_RAWMOUSE;
    ie.ie_Code = code;
    ie.ie_Qualifier = IEQUALIFIER_RELATIVEMOUSE;
    send(&ie);
}

static void rawkey(UWORD code)
{
    struct InputEvent ie;
    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = IECLASS_RAWKEY;
    ie.ie_Code = code;
    send(&ie);
}

int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) { printf("usage: QRXInput click X Y [right] | move X Y | key RAWCODE\n"); return 20; }
    port = CreateMsgPort();
    io = port ? (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq)) : NULL;
    if (!io || OpenDevice((CONST_STRPTR)"input.device", 0, (struct IORequest *)io, 0)) { printf("QRXInput: input.device unavailable\n"); return 20; }
    if (!strcmp(argv[1], "move") && argc >= 4) pointer(atoi(argv[2]), atoi(argv[3]));
    else if (!strcmp(argv[1], "click") && argc >= 4) {
        int right = argc >= 5 && !strcmp(argv[4], "right");
        pointer(atoi(argv[2]), atoi(argv[3]));
        Delay(2);
        button(right ? IECODE_RBUTTON : IECODE_LBUTTON);
        Delay(2);
        button((right ? IECODE_RBUTTON : IECODE_LBUTTON) | IECODE_UP_PREFIX);
        Delay(2);
    } else if (!strcmp(argv[1], "key") && argc >= 3) {
        UWORD k = (UWORD)strtol(argv[2], NULL, 0);
        rawkey(k); Delay(1); rawkey(k | IECODE_UP_PREFIX); Delay(1);
    } else { printf("QRXInput: bad arguments\n"); rc = 10; }
    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    return rc;
}
