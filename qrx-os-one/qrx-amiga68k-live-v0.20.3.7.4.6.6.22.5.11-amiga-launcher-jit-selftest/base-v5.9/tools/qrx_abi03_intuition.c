/*
 * QRX ABI Startup Isolation 03
 * Opens DOS + Intuition, but does not touch any screen.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <intuition/intuitionbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

struct DosLibrary *DOSBase = 0;
struct IntuitionBase *IntuitionBase = 0;

int main(void)
{
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 0);
    if (!DOSBase)
        return 20;
    PutStr((STRPTR)"QRXABI03 DOS OK\n");

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 0);
    if (!IntuitionBase) {
        PutStr((STRPTR)"QRXABI03 Intuition OPEN FAILED\n");
        CloseLibrary((struct Library *)DOSBase);
        return 20;
    }

    PutStr((STRPTR)"QRXABI03 Intuition OpenLibrary OK\n");
    CloseLibrary((struct Library *)IntuitionBase);
    IntuitionBase = 0;
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = 0;
    return 0;
}
