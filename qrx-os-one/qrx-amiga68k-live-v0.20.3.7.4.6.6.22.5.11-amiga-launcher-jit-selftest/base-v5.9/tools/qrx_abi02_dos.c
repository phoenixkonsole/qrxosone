/*
 * QRX ABI Startup Isolation 02
 * Opens dos.library and makes the first DOS library call (PutStr).
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct DosLibrary *DOSBase = 0;

int main(void)
{
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 0);
    if (!DOSBase)
        return 20;

    PutStr((STRPTR)"QRXABI02 DOS PutStr OK\n");
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = 0;
    return 0;
}
