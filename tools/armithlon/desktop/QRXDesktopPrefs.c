/* QRX desktop look: wallpaper and mouse pointer. Tells the running desktop through its
 * port QRXDESKTOP; the desktop applies the choice and saves it. */
#include <exec/types.h>
#include <exec/ports.h>
#include <intuition/intuition.h>
#include <dos/dos.h>
#include <graphics/rpattr.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct Library *CyberGfxBase;
struct QRXDesktopLook { struct Message msg; char wallpaper[64]; LONG pointer; };

#define MAX_WALL 32
#define WW 520
#define WH 380
#define TH_W 320
#define TH_H 180
static char g_names[MAX_WALL][64];
static int g_count, g_cur, g_ptr;
static const char *const g_pointers[] = { "macOS arrow", "QRX neon", "Amiga style", "Crosshair", "Large white" };
#define NPTR 5
static uint8_t g_thumb[TH_W * TH_H * 3];

static void add_name(const char *n) {
    int i;
    for (i = 0; i < g_count; i++) if (!strcmp(g_names[i], n)) return;
    if (g_count < MAX_WALL) { strncpy(g_names[g_count], n, 63); g_count++; }
}
static void scan(void) {
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR l = Lock((STRPTR)"SYS:QRX/Wallpapers", ACCESS_READ);
    if (l && fib && Examine(l, fib)) {
        while (ExNext(l, fib)) {
            char n[108], *dash;
            size_t len = strlen(fib->fib_FileName);
            if (len < 6 || strcmp(fib->fib_FileName + len - 6, ".qrxwp")) continue;
            strncpy(n, fib->fib_FileName, sizeof(n) - 1);
            n[sizeof(n) - 1] = 0;
            dash = strrchr(n, '-');
            if (dash) { *dash = 0; add_name(n); }
        }
    }
    if (l) UnLock(l);
    if (fib) FreeDosObject(DOS_FIB, fib);
    add_name("Classic");
}
static void load_config(void) {
    char buf[160], cur[64] = "Convergence";
    int i;
    BPTR fh = Open((STRPTR)"SYS:QRX/Prefs/desktop.cfg", MODE_OLDFILE);
    g_ptr = 1;
    if (fh) {
        while (FGets(fh, buf, sizeof(buf))) {
            size_t n = strlen(buf);
            while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
            if (!strncmp(buf, "wallpaper=", 10)) strncpy(cur, buf + 10, sizeof(cur) - 1);
            else if (!strncmp(buf, "pointer=", 8)) g_ptr = atoi(buf + 8);
        }
        Close(fh);
    }
    if (g_ptr < 0 || g_ptr >= NPTR) g_ptr = 1;
    for (i = 0; i < g_count; i++) if (!strcmp(g_names[i], cur)) g_cur = i;
}
static void thumb(struct RastPort *rp, int x, int y) {
    char path[128];
    BPTR fh;
    uint8_t head[12];
    int ix, iy, ok = 0;
    memset(g_thumb, 0x18, sizeof(g_thumb));
    snprintf(path, sizeof(path), "SYS:QRX/Wallpapers/%s-1280x720.qrxwp", g_names[g_cur]);
    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (fh && Read(fh, head, 12) == 12) {
        int w = (head[6] << 8) | head[7], h = (head[10] << 8) | head[11];
        uint8_t *row = (uint8_t *)malloc((size_t)w * 3);
        if (row && w > 0 && h > 0) {
            for (iy = 0; iy < TH_H; iy++) {
                int sy = iy * h / TH_H;
                Seek(fh, 12 + (LONG)sy * w * 3, OFFSET_BEGINNING);
                if (Read(fh, row, (LONG)w * 3) != (LONG)w * 3) break;
                for (ix = 0; ix < TH_W; ix++) memcpy(g_thumb + (iy * TH_W + ix) * 3, row + (ix * w / TH_W) * 3, 3);
            }
            ok = 1;
        }
        free(row);
    }
    if (fh) Close(fh);
    (void)ok;
    WritePixelArray(g_thumb, 0, 0, TH_W * 3, rp, x, y, TH_W, TH_H, RECTFMT_RGB);
}
static void colour(struct RastPort *rp, ULONG rgb) { SetRPAttrs(rp, RPTAG_PenMode, FALSE, RPTAG_FgColor, 0xFF000000UL | rgb, TAG_DONE); }
static void box(struct RastPort *rp, ULONG rgb, int x0, int y0, int x1, int y1) { colour(rp, rgb); RectFill(rp, x0, y0, x1, y1); }
static void label(struct RastPort *rp, ULONG rgb, int x, int y, const char *s) {
    colour(rp, rgb);
    SetDrMd(rp, JAM1);
    Move(rp, x, y);
    Text(rp, (STRPTR)s, strlen(s));
}
static void button(struct RastPort *rp, int x0, int y0, int x1, int y1, const char *s) {
    box(rp, 0x2B3A58, x0, y0, x1, y1);
    colour(rp, 0x37E6C8);
    Move(rp, x0, y0); Draw(rp, x1, y0); Draw(rp, x1, y1); Draw(rp, x0, y1); Draw(rp, x0, y0);
    label(rp, 0xFFFFFF, x0 + ((x1 - x0) - (int)strlen(s) * 8) / 2, y0 + 15, s);
}
static void draw(struct Window *w) {
    struct RastPort *rp = w->RPort;
    int ox = w->BorderLeft, oy = w->BorderTop;
    box(rp, 0x121826, ox, oy, w->Width - w->BorderRight - 1, w->Height - w->BorderBottom - 1);
    label(rp, 0x37E6C8, ox + 16, oy + 22, "WALLPAPER");
    thumb(rp, ox + 90, oy + 34);
    button(rp, ox + 16, oy + 110, ox + 76, oy + 134, "<");
    button(rp, ox + 424, oy + 110, ox + 484, oy + 134, ">");
    box(rp, 0x121826, ox + 90, oy + 218, ox + 410, oy + 236);
    label(rp, 0xFFFFFF, ox + 90, oy + 232, g_names[g_cur]);
    label(rp, 0x37E6C8, ox + 16, oy + 262, "MOUSE POINTER");
    button(rp, ox + 16, oy + 272, ox + 76, oy + 296, "<");
    button(rp, ox + 424, oy + 272, ox + 484, oy + 296, ">");
    label(rp, 0xFFFFFF, ox + 90, oy + 289, g_pointers[g_ptr]);
    button(rp, ox + 300, oy + 312, ox + 484, oy + 340, "APPLY");
}
static int apply(void) {
    struct MsgPort *reply = CreateMsgPort(), *port;
    struct QRXDesktopLook m;
    int ok = 0;
    if (!reply) return 0;
    memset(&m, 0, sizeof(m));
    m.msg.mn_ReplyPort = reply;
    m.msg.mn_Length = sizeof(m);
    strncpy(m.wallpaper, g_names[g_cur], sizeof(m.wallpaper) - 1);
    m.pointer = g_ptr;
    Forbid();
    port = FindPort((STRPTR)"QRXDESKTOP");
    if (port) { PutMsg(port, &m.msg); ok = 1; }
    Permit();
    if (ok) { WaitPort(reply); GetMsg(reply); }
    DeleteMsgPort(reply);
    return ok;
}
int main(void) {
    struct Screen *s;
    struct Window *w;
    int run = 1;
    CyberGfxBase = OpenLibrary((STRPTR)"cybergraphics.library", 41);
    if (!CyberGfxBase) { puts("QRXDesktopPrefs needs a true colour screen"); return 20; }
    scan();
    load_config();
    s = LockPubScreen(NULL);
    if (!s) return 20;
    w = OpenWindowTags(NULL, WA_PubScreen, (IPTR)s, WA_InnerWidth, WW - 20, WA_InnerHeight, WH - 30,
                       WA_Left, (s->Width - WW) / 2, WA_Top, (s->Height - WH) / 2, WA_Title, (IPTR)"QRX Desktop Look",
                       WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE,
                       WA_SmartRefresh, TRUE, WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_REFRESHWINDOW, TAG_DONE);
    UnlockPubScreen(NULL, s);
    if (!w) return 20;
    draw(w);
    while (run) {
        struct IntuiMessage *m;
        WaitPort(w->UserPort);
        while ((m = (struct IntuiMessage *)GetMsg(w->UserPort)) != NULL) {
            ULONG c = m->Class;
            UWORD code = m->Code;
            int x = m->MouseX - w->BorderLeft, y = m->MouseY - w->BorderTop, redraw = 0;
            ReplyMsg((struct Message *)m);
            if (c == IDCMP_CLOSEWINDOW) run = 0;
            else if (c == IDCMP_REFRESHWINDOW) { BeginRefresh(w); draw(w); EndRefresh(w, TRUE); }
            else if (c == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                if (y >= 110 && y <= 134 && x >= 16 && x <= 76) { g_cur = (g_cur + g_count - 1) % g_count; redraw = 1; }
                else if (y >= 110 && y <= 134 && x >= 424 && x <= 484) { g_cur = (g_cur + 1) % g_count; redraw = 1; }
                else if (y >= 272 && y <= 296 && x >= 16 && x <= 76) { g_ptr = (g_ptr + NPTR - 1) % NPTR; redraw = 1; }
                else if (y >= 272 && y <= 296 && x >= 424 && x <= 484) { g_ptr = (g_ptr + 1) % NPTR; redraw = 1; }
                else if (y >= 312 && y <= 340 && x >= 300 && x <= 484) apply();
                if (redraw) draw(w);
            }
        }
    }
    CloseWindow(w);
    CloseLibrary(CyberGfxBase);
    return 0;
}
