/*
 * QRX ABI Startup Isolation 04
 * First Intuition public-screen call.
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

    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 0);
    if (!DOSBase)
        return 20;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 0);
    if (!IntuitionBase) {
        CloseLibrary((struct Library *)DOSBase);
        return 20;
    }

    PutStr((STRPTR)"QRXABI04 before LockPubScreen\n");
    wb = LockPubScreen((STRPTR)"Workbench");
    if (wb) {
        PutStr((STRPTR)"QRXABI04 Workbench locked\n");
        UnlockPubScreen((STRPTR)"Workbench", wb);
        PutStr((STRPTR)"QRXABI04 Workbench unlocked\n");
    } else {
        PutStr((STRPTR)"QRXABI04 Workbench unavailable\n");
    }

    CloseLibrary((struct Library *)IntuitionBase);
    CloseLibrary((struct Library *)DOSBase);
    return 0;
}
