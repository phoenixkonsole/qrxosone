/* QRX 22.5.11 test helper: list graphics display modes (proves RTG availability). */
#include <exec/types.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <stdio.h>
int main(void)
{
    ULONG id = INVALID_ID;
    int n = 0, rtg = 0;
    while ((id = NextDisplayInfo(id)) != INVALID_ID) {
        struct DimensionInfo dim;
        struct NameInfo name;
        name.Name[0] = 0;
        if (!GetDisplayInfoData(NULL, (UBYTE *)&dim, sizeof(dim), DTAG_DIMS, id)) continue;
        GetDisplayInfoData(NULL, (UBYTE *)&name, sizeof(name), DTAG_NAME, id);
        printf("MODE %08lx %4dx%-4d depth %2d  %s\n", (unsigned long)id,
               (int)(dim.Nominal.MaxX - dim.Nominal.MinX + 1), (int)(dim.Nominal.MaxY - dim.Nominal.MinY + 1),
               (int)dim.MaxDepth, name.Name);
        if (dim.MaxDepth > 8) rtg++;
        n++;
    }
    printf("QRXMODES total=%d deep(>8bit)=%d\n", n, rtg);
    return 0;
}
