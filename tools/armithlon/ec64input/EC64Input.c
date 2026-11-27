/* EC64Input: keyboard and mouse of the 68100EC64 board (qrx_ec64_board.h, INPUT_EVENT 0x200) for
 * 64-bit AROS. The board passes macOS key codes and the pointer in framebuffer pixels; this turns
 * them into Amiga raw keys and pointer events and writes them to input.device.
 *   Run >NIL: EC64Input        (Startup-Sequence, once graphics is up)
 * Built with the AROS 64 SDK: cc64 -O2 -o EC64Input EC64Input.c */
#include <exec/types.h>
#include <exec/io.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <string.h>

#define BOARD_Q(off) (*(volatile UQUAD *)(0xFFFFFFFF00000000UL + (off)))
#define BOARD(off)   (*(volatile ULONG *)(0xFFFFFFFF00000000UL + (off)))
#define BOARD_MAGIC  0x5152584543363442UL
#define INPUT_EVENT  0x200

struct IntuitionBase *IntuitionBase;

/* macOS physical key positions -> Amiga raw keys (as in qrx_aros_window.m) */
static const UBYTE rawkeys[128] = {
    0x20,0x21,0x22,0x23,0x25,0x24,0x31,0x32,0x33,0x34,0x30,0x35,0x10,0x11,0x12,0x13,
    0x15,0x14,0x01,0x02,0x03,0x04,0x06,0x05,0x0c,0x09,0x07,0x0b,0x08,0x0a,0x1b,0x18,
    0x16,0x1a,0x17,0x19,0x44,0x28,0x26,0x2a,0x27,0x29,0x0d,0x38,0x3a,0x36,0x37,0x39,
    0x42,0x40,0x00,0x41,0xff,0x45,0x67,0x66,0x60,0x62,0x64,0x63,0x61,0x65,0x63,0xff,
    0xff,0x3c,0xff,0x5d,0xff,0x5e,0xff,0xff,0xff,0xff,0xff,0x5c,0x43,0xff,0x4a,0xff,
    0xff,0xff,0x1d,0x1e,0x1f,0x2d,0x2e,0x2f,0x3d,0x3e,0xff,0x3f,0x4f,0xff,0xff,0xff,
    0x54,0x55,0x56,0x52,0x57,0x58,0xff,0x5a,0xff,0xff,0xff,0xff,0xff,0x59,0xff,0x5b,
    0xff,0xff,0x5f,0x70,0x48,0x46,0x53,0x71,0x51,0x49,0x50,0x4f,0x4e,0x4d,0x4c,0xff};

static struct IOStdReq *io;
static UWORD qual;

static void send(struct InputEvent *ie)
{
    ie->ie_Qualifier |= qual;
    io->io_Command = IND_WRITEEVENT;
    io->io_Data = ie;
    io->io_Length = sizeof *ie;
    DoIO((struct IORequest *)io);
}
static UWORD qual_of(UBYTE raw)
{
    switch (raw) {
    case 0x60: return IEQUALIFIER_LSHIFT;
    case 0x61: return IEQUALIFIER_RSHIFT;
    case 0x62: return IEQUALIFIER_CAPSLOCK;
    case 0x63: return IEQUALIFIER_CONTROL;
    case 0x64: return IEQUALIFIER_LALT;
    case 0x65: return IEQUALIFIER_RALT;
    case 0x66: return IEQUALIFIER_LCOMMAND;
    case 0x67: return IEQUALIFIER_RCOMMAND;
    default: return 0;
    }
}
static void key(ULONG mac, BOOL down)
{
    struct InputEvent ie;
    UBYTE raw = mac < 128 ? rawkeys[mac] : 0xff;
    if (raw == 0xff) return;
    UWORD q = qual_of(raw);
    if (q && raw != 0x62) qual = down ? (qual | q) : (qual & ~q);
    if (raw == 0x62 && down) qual ^= q;
    memset(&ie, 0, sizeof ie);
    ie.ie_Class = IECLASS_RAWKEY;
    ie.ie_Code = raw | (down ? 0 : IECODE_UP_PREFIX);
    send(&ie);
}
static void move(LONG x, LONG y)
{
    struct InputEvent ie;
    struct IEPointerPixel pp;
    struct Screen *scr = IntuitionBase->FirstScreen;
    if (!scr) return;
    memset(&ie, 0, sizeof ie);
    pp.iepp_Screen = scr;
    pp.iepp_Position.X = x - scr->LeftEdge;
    pp.iepp_Position.Y = y - scr->TopEdge;
    ie.ie_Class = IECLASS_NEWPOINTERPOS;
    ie.ie_SubClass = IESUBCLASS_PIXEL;
    ie.ie_EventAddress = &pp;
    send(&ie);
}
static void button(ULONG b, BOOL down)
{
    static const UWORD codes[3] = { IECODE_LBUTTON, IECODE_RBUTTON, IECODE_MBUTTON };
    static const UWORD quals[3] = { IEQUALIFIER_LEFTBUTTON, IEQUALIFIER_RBUTTON, IEQUALIFIER_MIDBUTTON };
    struct InputEvent ie;
    if (b > 2) return;
    qual = down ? (qual | quals[b]) : (qual & ~quals[b]);
    memset(&ie, 0, sizeof ie);
    ie.ie_Class = IECLASS_RAWMOUSE;
    ie.ie_Code = codes[b] | (down ? 0 : IECODE_UP_PREFIX);
    ie.ie_X = ie.ie_Y = 0;
    send(&ie);
}

/* the wheel as AROS's input.device reports it: raw keys NM_WHEEL_UP / NM_WHEEL_DOWN, one per step */
static void wheel(WORD steps)
{
    struct InputEvent ie;
    WORD n = steps < 0 ? -steps : steps;
    while (n-- > 0) {
        memset(&ie, 0, sizeof ie);
        ie.ie_Class = IECLASS_RAWKEY;
        ie.ie_Code = steps < 0 ? 0x7A : 0x7B;   /* NM_WHEEL_UP, NM_WHEEL_DOWN */
        send(&ie);
    }
}
int main(void)
{
    struct MsgPort *port;
    if (BOARD_Q(0) != BOARD_MAGIC) { PutStr("EC64Input: no 68100EC64 board\n"); return 20; }
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    if (!IntuitionBase) { PutStr("EC64Input: no intuition.library\n"); return 20; }
    port = CreateMsgPort();
    io = port ? (struct IOStdReq *)CreateIORequest(port, sizeof *io) : NULL;
    if (!io || OpenDevice("input.device", 0, (struct IORequest *)io, 0)) {
        PutStr("EC64Input: no input.device\n");
        return 20;
    }
    for (;;) {
        ULONG e, n = 0;
        while ((e = BOARD(INPUT_EVENT)) != 0) {
            ULONG kind = e >> 24;
            if (kind == 1 || kind == 2) key((e >> 16) & 0xff, kind == 1);
            else if (kind == 3) move((e >> 12) & 0xfff, e & 0xfff);
            else if (kind == 4) button((e >> 16) & 0xff, (e & 1) != 0);
            else if (kind == 5) wheel((WORD)(e & 0xffff));
            n++;
        }
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) break;
        Delay(1);
    }
    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
