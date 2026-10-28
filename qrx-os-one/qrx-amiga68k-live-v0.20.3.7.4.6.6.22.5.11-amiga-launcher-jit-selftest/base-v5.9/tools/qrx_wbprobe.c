#include <exec/types.h>
#include <exec/libraries.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

struct IntuitionBase *IntuitionBase = NULL;
struct DosLibrary *DOSBase = NULL;

int main(void) {
    struct Screen *wb;

    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 39);
    if (!DOSBase) return 20;
    PutStr((STRPTR)"QRXWBPROBE 01 dos.library OK\n");

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39);
    if (!IntuitionBase) {
        PutStr((STRPTR)"QRXWBPROBE 02 intuition.library FAIL\n");
        CloseLibrary((struct Library *)DOSBase);
        return 20;
    }
    PutStr((STRPTR)"QRXWBPROBE 03 intuition.library OK\n");
    PutStr((STRPTR)"QRXWBPROBE 04 before LockPubScreen\n");

    wb = LockPubScreen((STRPTR)"Workbench");
    if (wb) {
        PutStr((STRPTR)"QRXWBPROBE 05 Workbench locked\n");
        UnlockPubScreen((STRPTR)"Workbench", wb);
        PutStr((STRPTR)"QRXWBPROBE 06 Workbench unlocked\n");
    } else {
        PutStr((STRPTR)"QRXWBPROBE 07 Workbench unavailable\n");
    }

    CloseLibrary((struct Library *)IntuitionBase);
    IntuitionBase = NULL;
    PutStr((STRPTR)"QRXWBPROBE 08 done\n");
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = NULL;
    return 0;
}
