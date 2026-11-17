/* OpenWB: opens Intuition's Workbench screen, for QRX OS One 64 where no Workbench program does.
 * QRXDesktop draws on that screen and finds none without it. Before it opens, its mode is chosen:
 * the largest that fits the Mac window (the 68100EC64 board's HOST_VIEW), else 1024 x 768. Later
 * changes of the window are QRXDesktop's (its guest tools). Startup-Sequence: C:OpenWB before
 * QRXDesktop. Built with the AROS 64 SDK: cc64 -O2 -o OpenWB OpenWB.c */
#include <graphics/displayinfo.h>
#include <intuition/iprefs.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

static ULONG pick(int w, int h, int *mw, int *mh)
{
    ULONG id = INVALID_ID, best = INVALID_ID;
    long area = 0;
    while ((id = NextDisplayInfo(id)) != INVALID_ID) {
        struct DimensionInfo di;
        int dw, dh;
        if (!GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof di, DTAG_DIMS, id) || di.MaxDepth < 15) continue;
        dw = di.Nominal.MaxX - di.Nominal.MinX + 1;
        dh = di.Nominal.MaxY - di.Nominal.MinY + 1;
        if (dw > w || dh > h) continue;
        if ((long)dw * dh > area) { area = (long)dw * dh; best = id; *mw = dw; *mh = dh; }
    }
    return best;
}

int main(void)
{
    int w = 1024, h = 768, mw = 0, mh = 0, ok;
    ULONG mode;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!IntuitionBase || !GfxBase) return 20;
#if defined(__m68k64__)
    if (*(volatile UQUAD *)0xFFFFFFFF00000000UL == 0x5152584543363442UL) {
        ULONG v = *(volatile ULONG *)0xFFFFFFFF00000130UL;
        if ((v >> 16) >= 640 && (v & 0xFFFF) >= 400) { w = v >> 16; h = v & 0xFFFF; }
    }
#endif
    mode = pick(w, h, &mw, &mh);
    if (mode != INVALID_ID) {
        struct IScreenModePrefs p = { mode, (UWORD)mw, (UWORD)mh, 24, 0 };
        SetIPrefs(&p, sizeof p, IPREFS_TYPE_SCREENMODE);
    }
    ok = OpenWorkBench() != 0;
    if (!ok) PutStr("OpenWB: the Workbench screen did not open\n");
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return ok ? 0 : 10;
}
