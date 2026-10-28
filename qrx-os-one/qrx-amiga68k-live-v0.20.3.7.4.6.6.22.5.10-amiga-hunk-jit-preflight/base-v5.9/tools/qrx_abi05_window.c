/*
 * QRX ABI Startup Isolation 05
 * Opens one tiny window on the existing Workbench public screen.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

struct DosLibrary *DOSBase = 0;
struct IntuitionBase *IntuitionBase = 0;

int main(void)
{
    struct Screen *wb;
    struct Window *win;

    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 0);
    if (!DOSBase)
        return 20;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 0);
    if (!IntuitionBase) {
        CloseLibrary((struct Library *)DOSBase);
        return 20;
    }

    wb = LockPubScreen((STRPTR)"Workbench");
    if (!wb) {
        PutStr((STRPTR)"QRXABI05 Workbench unavailable\n");
        CloseLibrary((struct Library *)IntuitionBase);
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }

    PutStr((STRPTR)"QRXABI05 before OpenWindowTags\n");
    win = OpenWindowTags(0,
        WA_PubScreen, (ULONG)wb,
        WA_Left, 16,
        WA_Top, 24,
        WA_Width, 240,
        WA_Height, 80,
        WA_Title, (ULONG)"QRX ABI 05",
        WA_DragBar, TRUE,
        WA_DepthGadget, TRUE,
        WA_CloseGadget, TRUE,
        WA_Activate, TRUE,
        TAG_DONE);

    if (win) {
        PutStr((STRPTR)"QRXABI05 OpenWindowTags OK\n");
        Delay(50);
        CloseWindow(win);
    } else {
        PutStr((STRPTR)"QRXABI05 OpenWindowTags FAILED\n");
    }

    UnlockPubScreen((STRPTR)"Workbench", wb);
    CloseLibrary((struct Library *)IntuitionBase);
    CloseLibrary((struct Library *)DOSBase);
    return win ? 0 : 10;
}
