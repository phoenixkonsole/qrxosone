/* QRX 22.5.11 test helper: dump the front Intuition screen as binary PPM.
 *   QRXScreenGrab <file.ppm>    (uses cybergraphics ReadPixelArray: works on RTG and native) */
#include <exec/types.h>
#include <intuition/intuitionbase.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <stdio.h>
#include <string.h>
struct Library *CyberGfxBase;
int main(int argc, char **argv)
{
    struct Screen *s;
    UBYTE *line;
    BPTR fh;
    WORD w, h, y;
    char hdr[40];
    ULONG lock;
    if (argc < 2) return 20;
    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);
    if (!CyberGfxBase) { printf("no cybergraphics.library\n"); return 20; }
    lock = LockIBase(0);
    s = IntuitionBase->FirstScreen;
    UnlockIBase(lock);
    if (!s) return 20;
    w = s->Width; h = s->Height;
    line = AllocVec((ULONG)w * 3, MEMF_ANY);
    fh = Open((CONST_STRPTR)argv[1], MODE_NEWFILE);
    if (!line || !fh) return 20;
    sprintf(hdr, "P6\n%d %d\n255\n", (int)w, (int)h);
    Write(fh, hdr, strlen(hdr));
    for (y = 0; y < h; y++) {
        ReadPixelArray(line, 0, 0, (UWORD)(w * 3), &s->RastPort, 0, y, w, 1, RECTFMT_RGB);
        Write(fh, line, (LONG)w * 3);
    }
    Close(fh);
    FreeVec(line);
    printf("QRXGRAB %dx%d depth=%lu title=%s\n", (int)w, (int)h,
           (unsigned long)GetBitMapAttr(s->RastPort.BitMap, BMA_DEPTH), s->Title ? (char *)s->Title : "");
    CloseLibrary(CyberGfxBase);
    return 0;
}
