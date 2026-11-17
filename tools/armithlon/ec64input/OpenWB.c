/* OpenWB: opens Intuition's Workbench screen, for QRX OS One 64 where no Workbench program does.
 * QRXDesktop draws on that screen and finds none without it. Startup-Sequence: C:OpenWB before
 * QRXDesktop. Built with the AROS 64 SDK: cc64 -O2 -o OpenWB OpenWB.c */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

struct IntuitionBase *IntuitionBase;

int main(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    if (!IntuitionBase) return 20;
    int ok = OpenWorkBench() != 0;
    if (!ok) PutStr("OpenWB: the Workbench screen did not open\n");
    CloseLibrary((struct Library *)IntuitionBase);
    return ok ? 0 : 10;
}
