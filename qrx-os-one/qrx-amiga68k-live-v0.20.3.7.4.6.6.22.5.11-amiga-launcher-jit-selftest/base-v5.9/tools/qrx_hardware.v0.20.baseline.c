/*
 * QRX OS One / Amiga 68K - Hardware Manager v0.8
 *
 * Reports the active QRX display profile and Poseidon USB availability.
 * USB mass-storage volumes are exposed through the normal DOS namespace,
 * so any mounted USB volume can be opened directly in QRX Drive.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/gfxbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>

#define PEN_BAR       224
#define PEN_BAR_TEXT  225
#define PEN_ACCENT    226
#define PEN_PANEL     227
#define PEN_PANEL_2   228
#define PEN_TEXT      229
#define PEN_MUTED     230
#define PEN_EDGE      231
#define PEN_GREEN     236
#define PEN_RED       234
#define PEN_BLACK     238
#define PEN_BUTTON    239

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct DosLibrary *DOSBase = NULL;
struct Library *PoseidonBase = NULL;

static struct Screen *g_screen = NULL;
static struct Window *g_window = NULL;

static void qfill(struct RastPort *rp, UBYTE pen,
                  WORD x1, WORD y1, WORD x2, WORD y2) {
    SetAPen(rp, pen);
    RectFill(rp, x1, y1, x2, y2);
}

static void qline(struct RastPort *rp, UBYTE pen,
                  WORD x1, WORD y1, WORD x2, WORD y2) {
    SetAPen(rp, pen);
    Move(rp, x1, y1);
    Draw(rp, x2, y2);
}

static void qtext(struct RastPort *rp, UBYTE pen,
                  WORD x, WORD y, const char *s) {
    if (!rp || !s) return;
    SetAPen(rp, pen);
    Move(rp, x, y);
    Text(rp, (STRPTR)s, strlen(s));
}

static int lock_exists(const char *path) {
    BPTR lock;
    if (!path) return 0;
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock) return 0;
    UnLock(lock);
    return 1;
}

static void draw_manager(void) {
    struct RastPort *rp;
    char buf[160];
    ULONG allmem, chipmem, fastmem;
    ULONG depth;
    int y = 72;
    int usb_any = 0;
    int i;
    char name[16];

    if (!g_window || !g_screen) return;
    rp = g_window->RPort;

    qfill(rp, PEN_PANEL, 0, 0, g_window->Width - 1, g_window->Height - 1);
    qfill(rp, PEN_BAR, 0, 0, g_window->Width - 1, 28);
    qtext(rp, PEN_ACCENT, 10, 19, "QRX");
    qtext(rp, PEN_BAR_TEXT, 43, 19, "Hardware Manager 68K");

    depth = GetBitMapAttr(g_screen->RastPort.BitMap, BMA_DEPTH);
    snprintf(buf, sizeof(buf), "Display  %dx%d x %lu",
             g_screen->Width, g_screen->Height, depth);
    qtext(rp, PEN_TEXT, 20, y, buf); y += 18;

    snprintf(buf, sizeof(buf), "Profile  %s",
             (depth > 8 || g_screen->Width > 640 || g_screen->Height > 512)
             ? "RTG / Enhanced" : "AGA / Classic");
    qtext(rp, PEN_TEXT, 20, y, buf); y += 28;

    qtext(rp, PEN_MUTED, 20, y, "USB"); y += 18;
    snprintf(buf, sizeof(buf), "Poseidon library  %s",
             PoseidonBase ? "available" : "not detected");
    qtext(rp, PoseidonBase ? PEN_GREEN : PEN_RED, 20, y, buf); y += 18;

    for (i = 0; i < 4; ++i) {
        snprintf(name, sizeof(name), "USB%d:", i);
        if (lock_exists(name)) {
            snprintf(buf, sizeof(buf), "%s mounted and accessible via QRX Drive", name);
            qtext(rp, PEN_TEXT, 34, y, buf);
            y += 16;
            usb_any = 1;
        }
    }

    if (!usb_any) {
        qtext(rp, PEN_MUTED, 34, y,
              "No USB0:-USB3: volume currently mounted.");
        y += 18;
        qtext(rp, PEN_MUTED, 34, y,
              "Poseidon mass-storage volumes remain normal DOS volumes.");
        y += 18;
    }

    y += 10;
    qtext(rp, PEN_MUTED, 20, y, "Memory"); y += 18;
    allmem = AvailMem(MEMF_ANY);
    chipmem = AvailMem(MEMF_CHIP);
    fastmem = AvailMem(MEMF_FAST);
    snprintf(buf, sizeof(buf), "All %lu KB   Chip %lu KB   Fast %lu KB",
             allmem / 1024UL, chipmem / 1024UL, fastmem / 1024UL);
    qtext(rp, PEN_TEXT, 20, y, buf); y += 28;

    qtext(rp, PEN_MUTED, 20, y, "QRX hardware policy"); y += 18;
    qtext(rp, PEN_TEXT, 34, y,
          "RTG is preferred; AGA remains the compatibility fallback."); y += 16;
    qtext(rp, PEN_TEXT, 34, y,
          "USB uses the AROS/Poseidon stack and normal DOS mounts."); y += 16;
    qtext(rp, PEN_TEXT, 34, y,
          "Legacy TAKEOVER still releases the QRX display for classic apps.");

    qfill(rp, PEN_BUTTON, 20, g_window->Height - 50,
          110, g_window->Height - 27);
    qline(rp, PEN_EDGE, 20, g_window->Height - 50,
          110, g_window->Height - 50);
    qtext(rp, PEN_TEXT, 30, g_window->Height - 34, "REFRESH");
}

int main(void) {
    struct IntuiMessage *msg;
    int running = 1;
    WORD w, h;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 39);
    PoseidonBase = OpenLibrary("poseidon.library", 0);

    if (!IntuitionBase || !GfxBase || !DOSBase)
        goto cleanup;

    g_screen = LockPubScreen((UBYTE *)"QRX");
    if (!g_screen) {
        PutStr("QRX: public screen 'QRX' is required.\n");
        goto cleanup;
    }

    w = g_screen->Width > 720 ? 680 : g_screen->Width - 40;
    h = g_screen->Height > 520 ? 480 : g_screen->Height - 60;
    if (w < 520) w = 520;
    if (h < 360) h = 360;
    if (w > g_screen->Width) w = g_screen->Width;
    if (h > g_screen->Height) h = g_screen->Height;

    g_window = OpenWindowTags(NULL,
        WA_CustomScreen, (ULONG)g_screen,
        WA_Left, (g_screen->Width - w) / 2,
        WA_Top, (g_screen->Height - h) / 2,
        WA_Width, w,
        WA_Height, h,
        WA_Title, (ULONG)"QRX Hardware Manager 68K",
        WA_Flags, WFLG_ACTIVATE | WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                  WFLG_CLOSEGADGET | WFLG_SMART_REFRESH,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS |
                  IDCMP_REFRESHWINDOW,
        TAG_DONE);
    if (!g_window)
        goto cleanup;

    draw_manager();

    while (running) {
        WaitPort(g_window->UserPort);
        while ((msg = (struct IntuiMessage *)GetMsg(g_window->UserPort)) != NULL) {
            ULONG cls = msg->Class;
            UWORD code = msg->Code;
            WORD mx = msg->MouseX;
            WORD my = msg->MouseY;
            ReplyMsg((struct Message *)msg);

            if (cls == IDCMP_CLOSEWINDOW) {
                running = 0;
            } else if (cls == IDCMP_REFRESHWINDOW) {
                BeginRefresh(g_window);
                draw_manager();
                EndRefresh(g_window, TRUE);
            } else if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN &&
                       mx >= 20 && mx <= 110 &&
                       my >= g_window->Height - 50 &&
                       my <= g_window->Height - 27) {
                draw_manager();
            }
        }
    }

cleanup:
    if (g_window) {
        CloseWindow(g_window);
        g_window = NULL;
    }
    if (g_screen) {
        UnlockPubScreen((UBYTE *)"QRX", g_screen);
        g_screen = NULL;
    }
    if (PoseidonBase) CloseLibrary(PoseidonBase);
    if (DOSBase) CloseLibrary((struct Library *)DOSBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
