/* QRX OS One 22.5.12 - qrx-win32 AROS backend.
 * Every Win32 top-level window becomes an Intuition window on the default
 * public screen (Workbench; RTG or native).  The window's inner area shows the
 * Win32 menu bar (drawn by user.c) above the client framebuffer, which is
 * copied with cybergraphics WritePixelArray().  Popup menus/dialogs are
 * separate windows.  Input arrives through one shared IDCMP port; timers use
 * the IDCMP wait with timer.device timeouts. */
#ifdef __AROS__
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <graphics/gfx.h>
#include <devices/timer.h>
#include <devices/inputevent.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/keymap.h>
#include <proto/dos.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "qrxwin.h"

struct Library *CyberGfxBase;
static struct Screen *g_scr;
static struct MsgPort *g_port, *g_tport;
static struct timerequest *g_treq;
static int g_topen;
static uint32_t *g_tmp;            /* conversion buffer (ARGB, big endian) */
static size_t g_tmpn;
static int g_truecolor;            /* screen bitmap is a >8 bit CyberGraphX bitmap */
static UBYTE *g_chunky;            /* CLUT screens: pen-mapped chunky buffer */
static size_t g_chunkyn;
static LONG g_pen[4096];           /* 4-4-4 RGB -> obtained pen (-1 = not yet) */

typedef struct bew {
    struct Window *win;
    uint32_t h;
    int mbh;
    int full;                       /* next presentation must cover the whole window */
    struct Screen *scr;             /* own full-screen display (DirectDraw exclusive mode) */
    int scr8;                       /* that screen is 8-bit CLUT: palette loaded with LoadRGB32 */
    uint32_t pal[256];              /* palette currently loaded into scr */
    struct ScreenBuffer *sb[2];     /* RTG direct: display buffers of scr, kept locked */
    APTR lock[2];
    UBYTE *fbaddr[2];
    int fbn, fbshown;
    ULONG fbpitch;
    char title[260];
    qsurf mb;
} bew;

static int ar_init(qwin *w)
{
    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);
    g_scr = LockPubScreen(NULL);
    if (!g_scr) return 0;
    g_truecolor = CyberGfxBase && GetCyberMapAttr(g_scr->RastPort.BitMap, CYBRMATTR_ISCYBERGFX) &&
                  GetCyberMapAttr(g_scr->RastPort.BitMap, CYBRMATTR_DEPTH) > 8;
    { int i; for (i = 0; i < 4096; i++) g_pen[i] = -1; }
    if (w->verbose) qw_log(w, "AROS backend: screen %dx%d, %s output", g_scr->Width, g_scr->Height, g_truecolor ? "truecolor WritePixelArray" : "pen-mapped WriteChunkyPixels");
    g_port = CreateMsgPort();
    g_tport = CreateMsgPort();
    if (!g_port || !g_tport) return 0;
    g_treq = (struct timerequest *)CreateIORequest(g_tport, sizeof(struct timerequest));
    if (g_treq && !OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)g_treq, 0)) g_topen = 1;
    return 1;
}

static void latin1(char *d, const wchar16 *s, int max)
{
    int i;
    for (i = 0; s[i] && i < max - 1; i++) d[i] = s[i] < 256 ? (char)s[i] : '?';
    d[i] = 0;
}

/* DirectDraw exclusive mode: an own 8-bit screen with a borderless backdrop window,
 * like Amiga games; returns 0 if no such display mode/screen is available */
static int open_fullscreen(qwin *w, qwnd *t, bew *b)
{
    ULONG id = INVALID_ID;
    if (w->verbose) qw_log(w, "AROS backend: looking for a %dx%dx8 display mode", t->fs_w, t->fs_h);
    if (CyberGfxBase) id = BestCModeIDTags(CYBRBIDTG_NominalWidth, (ULONG)t->fs_w, CYBRBIDTG_NominalHeight, (ULONG)t->fs_h, CYBRBIDTG_Depth, 8, TAG_DONE);
    if (id == INVALID_ID) id = BestModeID(BIDTAG_NominalWidth, (ULONG)t->fs_w, BIDTAG_NominalHeight, (ULONG)t->fs_h, BIDTAG_Depth, 8, TAG_DONE);
    if (id == INVALID_ID) { qw_log(w, "AROS: no %dx%d 8-bit display mode, staying in a window", t->fs_w, t->fs_h); return 0; }
    if (w->verbose) qw_log(w, "AROS backend: mode %08lx, opening screen", (unsigned long)id);
    b->scr = OpenScreenTags(NULL, SA_DisplayID, id, SA_Width, (ULONG)t->fs_w, SA_Height, (ULONG)t->fs_h, SA_Depth, 8,
                            SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Type, CUSTOMSCREEN, SA_Title, (IPTR)"QRX DirectDraw", TAG_DONE);
    if (!b->scr) { qw_log(w, "AROS: OpenScreen %dx%dx8 failed, staying in a window", t->fs_w, t->fs_h); return 0; }
    if (w->verbose) qw_log(w, "AROS backend: screen open, opening backdrop window");
    b->scr8 = !(CyberGfxBase && GetCyberMapAttr(b->scr->RastPort.BitMap, CYBRMATTR_ISCYBERGFX) &&
                GetCyberMapAttr(b->scr->RastPort.BitMap, CYBRMATTR_DEPTH) > 8);
    b->win = OpenWindowTags(NULL, WA_CustomScreen, (IPTR)b->scr, WA_Left, 0, WA_Top, 0, WA_Width, (ULONG)t->fs_w, WA_Height, (ULONG)t->fs_h,
                            WA_Borderless, TRUE, WA_Backdrop, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_ReportMouse, TRUE,
                            WA_SimpleRefresh, TRUE, WA_NoCareRefresh, TRUE, WA_IDCMP, 0, TAG_DONE);
    if (!b->win) { CloseScreen(b->scr); b->scr = NULL; return 0; }
    b->mbh = 0;
    memset(b->pal, 0xFF, sizeof(b->pal));
    if (w->verbose) qw_log(w, "AROS backend: own screen %dx%dx8 (mode %08lx, %s) for window %08x", t->fs_w, t->fs_h, (unsigned long)id, b->scr8 ? "CLUT" : "truecolor", t->h);
    return 1;
}

static void ar_create(qwin *w, qwnd *t)
{
    bew *b = (bew *)calloc(1, sizeof(bew));
    int popup = (t->style & WS_POPUP) && !(t->style & WS_CAPTION);
    if (!b) return;
    if (t->fs_w > 0 && open_fullscreen(w, t, b)) {
        b->h = t->h;
        b->win->UserPort = g_port;
        b->win->UserData = (APTR)b;
        ModifyIDCMP(b->win, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_RAWKEY | IDCMP_ACTIVEWINDOW);
        t->be = b;
        t->present = 1;
        b->full = 1;
        return;
    }
    b->h = t->h;
    b->mbh = user_menubar_height(w, t);
    latin1(b->title, t->text, sizeof(b->title));
    b->win = OpenWindowTags(NULL,
        WA_Left, t->x - (popup ? 0 : g_scr->WBorLeft), WA_Top, t->y - b->mbh - (popup ? 0 : g_scr->WBorTop + g_scr->Font->ta_YSize + 1),
        WA_InnerWidth, t->cw, WA_InnerHeight, t->ch + b->mbh,
        WA_Title, (IPTR)(popup ? NULL : b->title),
        WA_DragBar, !popup, WA_DepthGadget, !popup, WA_CloseGadget, !popup && !(t->style & 0x00000080u) /* not DS_MODALFRAME-only */,
        WA_Borderless, popup, WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_ReportMouse, TRUE,
        WA_SmartRefresh, TRUE, WA_NoCareRefresh, FALSE,
        WA_PubScreen, (IPTR)g_scr, WA_IDCMP, 0,
        TAG_DONE);
    if (!b->win) { free(b); qw_log(w, "AROS: OpenWindow failed"); return; }
    b->win->UserPort = g_port;
    b->win->UserData = (APTR)b;
    ModifyIDCMP(b->win, IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_RAWKEY | IDCMP_ACTIVEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_CHANGEWINDOW);
    t->be = b;
    t->present = 1;
    b->full = 1;
    if (w->verbose) qw_log(w, "AROS backend: window %08x opened at %d,%d inner %dx%d (+%d menu bar)", t->h, t->x, t->y, t->cw, t->ch, b->mbh);
}

static void strip_close(struct Window *win)
{
    struct IntuiMessage *m;
    struct Node *n;
    Forbid();
    m = (struct IntuiMessage *)win->UserPort->mp_MsgList.lh_Head;
    while ((n = m->ExecMessage.mn_Node.ln_Succ)) {
        if (m->IDCMPWindow == win) { Remove((struct Node *)m); ReplyMsg((struct Message *)m); }
        m = (struct IntuiMessage *)n;
    }
    win->UserPort = NULL;
    ModifyIDCMP(win, 0);
    Permit();
    CloseWindow(win);
}

static void ar_fb_close(qwin *w, qwnd *t);
static void ar_destroy(qwin *w, qwnd *t)
{
    bew *b = (bew *)t->be;
    if (!b) return;
    if (b->fbn) { if (w->fb_lost) w->fb_lost(w, t); ar_fb_close(w, t); }
    if (b->win) strip_close(b->win);
    if (b->scr) CloseScreen(b->scr);
    free(b->mb.pix);
    free(b);
    t->be = NULL;
}

static LONG pen_for(uint32_t rgb)
{
    unsigned k = ((rgb >> 12) & 0xF00) | ((rgb >> 8) & 0xF0) | ((rgb >> 4) & 0xF);
    if (g_pen[k] < 0) {
        ULONG r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
        g_pen[k] = ObtainBestPen(g_scr->ViewPort.ColorMap, r * 0x01010101u, g * 0x01010101u, b * 0x01010101u, OBP_Precision, PRECISION_IMAGE, TAG_DONE);
        if (g_pen[k] < 0) g_pen[k] = 1;
    }
    return g_pen[k];
}

static void blit(struct Window *win, uint32_t *pix, int sw, int x, int y, int cw, int ch)
{
    size_t n = (size_t)sw * (size_t)ch;
    int i;
    if (!g_truecolor) {               /* CLUT (8 bit RTG or native planar) screen */
        size_t m = (size_t)cw * (size_t)ch;
        int yy, xx;
        if (m > g_chunkyn) { free(g_chunky); g_chunky = (UBYTE *)malloc(m); g_chunkyn = g_chunky ? m : 0; }
        if (!g_chunky) return;
        for (yy = 0; yy < ch; yy++) for (xx = 0; xx < cw; xx++) g_chunky[(size_t)yy * (size_t)cw + (size_t)xx] = (UBYTE)pen_for(pix[(size_t)yy * (size_t)sw + (size_t)xx]);
        WriteChunkyPixels(win->RPort, (ULONG)x, (ULONG)y, (ULONG)(x + cw - 1), (ULONG)(y + ch - 1), g_chunky, (LONG)cw);
        return;
    }
    if (n > g_tmpn) { free(g_tmp); g_tmp = (uint32_t *)malloc(n * 4); g_tmpn = g_tmp ? n : 0; }
    if (!g_tmp) return;
    /* surface pixels are 0x00RRGGBB host words: on big-endian m68k that already is ARGB */
    for (i = 0; i < (int)n; i++) g_tmp[i] = pix[i] | 0xFF000000u;
    WritePixelArray(g_tmp, 0, 0, (UWORD)(sw * 4), win->RPort, (UWORD)x, (UWORD)y, (UWORD)cw, (UWORD)ch, RECTFMT_ARGB);
}

static void ar_present(qwin *w, qwnd *t)
{
    bew *b = (bew *)t->be;
    int mbh;
    if (!b || !b->win || !t->surf.pix) return;
    if (b->scr) return;                             /* own DirectDraw screen: frames arrive via present8 */
    mbh = user_menubar_height(w, t);
    {
        static int npres;
        if (w->verbose && npres++ < 8) qw_log(w, "AROS backend: present %08x %dx%d", t->h, t->cw, t->ch);
    }
    if (mbh != b->mbh) {             /* menu appeared/disappeared: reopen with the new inner size */
        ar_destroy(w, t);
        ar_create(w, t);
        b = (bew *)t->be;
        if (!b) return;
    }
    if (mbh && b->full) {           /* menu bar only when it can have changed */
        surf_resize(&b->mb, t->cw, mbh);
        user_render_menubar(w, t, &b->mb);
        blit(b->win, b->mb.pix, t->cw, b->win->BorderLeft, b->win->BorderTop, t->cw, mbh);
    }
    {
        int x0 = 0, y0 = 0, x1 = t->cw < t->surf.w ? t->cw : t->surf.w, y1 = t->ch < t->surf.h ? t->ch : t->surf.h;
        if (!b->full && t->px1 > t->px0 && t->py1 > t->py0) {       /* only the changed area */
            if (t->px0 > x0) x0 = t->px0;
            if (t->py0 > y0) y0 = t->py0;
            if (t->px1 < x1) x1 = t->px1;
            if (t->py1 < y1) y1 = t->py1;
        }
        if (w->verbose > 1) {
            static int nd;
            if (nd++ < 60) qw_log(w, "AROS present %08x rect %d,%d-%d,%d full=%d pix0=%06x pen0=%ld", t->h, x0, y0, x1, y1, b->full,
                                  (unsigned)t->surf.pix[(size_t)y0 * (size_t)t->surf.w + (size_t)x0], (long)(g_truecolor ? -2 : pen_for(t->surf.pix[(size_t)y0 * (size_t)t->surf.w + (size_t)x0])));
        }
        b->full = 0;
        if (x1 > x0 && y1 > y0)
            blit(b->win, t->surf.pix + (size_t)y0 * (size_t)t->surf.w + (size_t)x0, t->surf.w,
                 b->win->BorderLeft + x0, b->win->BorderTop + mbh + y0, x1 - x0, y1 - y0);
    }
}

static void ar_title(qwin *w, qwnd *t)
{
    bew *b = (bew *)t->be;
    (void)w;
    if (!b || !b->win) return;
    latin1(b->title, t->text, sizeof(b->title));
    SetWindowTitles(b->win, (CONST_STRPTR)b->title, (CONST_STRPTR)~0);
}

static void ar_menu(qwin *w, qwnd *t) { t->present = 1; if (t->be) ((bew *)t->be)->full = 1; (void)w; }

static uint64_t ar_now(qwin *w)
{
    struct timeval tv;
    (void)w;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

/* raw key -> Windows virtual key */
static int vk_of_raw(UWORD code, int ch)
{
    switch (code) {
    case 0x44: case 0x43: return 0x0D;     /* Return / Enter */
    case 0x41: return 0x08;                /* Backspace */
    case 0x45: return 0x1B;                /* Esc */
    case 0x46: return 0x2E;                /* Del */
    case 0x42: return 0x09;                /* Tab */
    case 0x40: return 0x20;                /* Space */
    case 0x4C: return 0x26; case 0x4D: return 0x28; case 0x4E: return 0x27; case 0x4F: return 0x25;
    case 0x60: case 0x61: return 0x10;     /* Shift */
    case 0x63: return 0x11;                /* Ctrl */
    case 0x64: case 0x65: return 0x12;     /* Alt */
    case 0x5E: return 0x6B; case 0x4A: return 0x6D; case 0x5D: return 0x6A; case 0x5C: return 0x6F; case 0x3C: return 0x6E;
    }
    if (code >= 0x50 && code <= 0x59) return 0x70 + (code - 0x50);   /* F1..F10 */
    if (code >= 0x0F && code <= 0x3F && ch >= '0' && ch <= '9' && (code == 0x0F || (code >= 0x1D && code <= 0x3F))) return 0x60 + (ch - '0'); /* keypad */
    if (ch >= 'a' && ch <= 'z') return ch - 32;
    if (ch >= 'A' && ch <= 'Z') return ch;
    if (ch >= '0' && ch <= '9') return ch;
    switch (ch) { case '+': return 0xBB; case '-': return 0xBD; case ',': return 0xBC; case '.': return 0xBE; case '*': return 0x6A; case '/': return 0x6F; case '=': return 0xBB; }
    return 0;
}

static int translate(qwin *w, struct IntuiMessage *im, qevent *ev)
{
    bew *b = (bew *)im->IDCMPWindow->UserData;
    int x, y;
    if (!b) return 0;
    ev->win = b->h;
    x = im->MouseX - im->IDCMPWindow->BorderLeft;
    y = im->MouseY - im->IDCMPWindow->BorderTop - b->mbh;
    switch (im->Class) {
    case IDCMP_CLOSEWINDOW: ev->type = QE_CLOSE; return 1;
    case IDCMP_ACTIVEWINDOW: ev->type = QE_ACTIVATE; return 1;
    case IDCMP_REFRESHWINDOW: {
        qwnd *t = W(w, b->h);
        BeginRefresh(im->IDCMPWindow); EndRefresh(im->IDCMPWindow, TRUE);
        if (t) { t->present = 1; ((bew *)t->be)->full = 1; }
        ev->type = QE_NONE;                 /* wake the pump so the window is presented again */
        return 1; }
    case IDCMP_CHANGEWINDOW: {
        qwnd *t = W(w, b->h);
        if (t) { t->x = im->IDCMPWindow->LeftEdge + im->IDCMPWindow->BorderLeft; t->y = im->IDCMPWindow->TopEdge + im->IDCMPWindow->BorderTop + b->mbh; }
        return 0; }
    case IDCMP_MOUSEMOVE: ev->type = QE_MOUSEMOVE; ev->x = x; ev->y = y; return 1;
    case IDCMP_MOUSEBUTTONS:
        ev->x = x; ev->y = y;
        if (y < 0 && b->mbh && (im->Code == SELECTDOWN)) { ev->type = QE_MENUBAR; return 1; }
        switch (im->Code) {
        case SELECTDOWN: ev->type = QE_MOUSEDOWN; ev->button = 1; return 1;
        case SELECTUP: ev->type = QE_MOUSEUP; ev->button = 1; return 1;
        case MENUDOWN: ev->type = QE_MOUSEDOWN; ev->button = 2; return 1;
        case MENUUP: ev->type = QE_MOUSEUP; ev->button = 2; return 1;
        case MIDDLEDOWN: ev->type = QE_MOUSEDOWN; ev->button = 3; return 1;
        case MIDDLEUP: ev->type = QE_MOUSEUP; ev->button = 3; return 1;
        }
        return 0;
    case IDCMP_RAWKEY: {
        struct InputEvent ie;
        char buf[8];
        LONG n;
        memset(&ie, 0, sizeof(ie));
        ie.ie_Class = IECLASS_RAWKEY;
        ie.ie_Code = im->Code & 0x7F;
        ie.ie_Qualifier = im->Qualifier;
        n = MapRawKey(&ie, (STRPTR)buf, sizeof(buf), NULL);
        ev->ch = n == 1 ? (unsigned char)buf[0] : 0;
        ev->vk = vk_of_raw(im->Code & 0x7F, ev->ch);
        ev->type = (im->Code & 0x80) ? QE_KEYUP : QE_KEYDOWN;
        return ev->vk != 0;
    }
    }
    return 0;
}

static int ar_wait(qwin *w, uint32_t timeout, qevent *ev)
{
    for (;;) {
        struct IntuiMessage *im = (struct IntuiMessage *)GetMsg(g_port);
        if (im) {
            int r;
            memset(ev, 0, sizeof(*ev));
            r = translate(w, im, ev);
            ReplyMsg((struct Message *)im);
            if (r) return 1;
            continue;
        }
        if (!timeout) return 0;
        {
            ULONG sigs = (1UL << g_port->mp_SigBit) | SIGBREAKF_CTRL_C, got;
            if (g_topen) {
                g_treq->tr_node.io_Command = TR_ADDREQUEST;
                g_treq->tr_time.tv_secs = timeout / 1000;
                g_treq->tr_time.tv_micro = (timeout % 1000) * 1000;
                SendIO((struct IORequest *)g_treq);
                sigs |= 1UL << g_tport->mp_SigBit;
            }
            got = Wait(sigs);
            if (g_topen) {
                if (!CheckIO((struct IORequest *)g_treq)) AbortIO((struct IORequest *)g_treq);
                WaitIO((struct IORequest *)g_treq);
            }
            if (got & SIGBREAKF_CTRL_C) return -1;
            if (!(got & (1UL << g_port->mp_SigBit))) return 0;
        }
    }
}

static void ar_shutdown(qwin *w)
{
    int i;
    for (i = 0; i < QW_MAXWND; i++) if (w->wnd[i].used && w->wnd[i].be) ar_destroy(w, &w->wnd[i]);
    if (g_topen) CloseDevice((struct IORequest *)g_treq);
    if (g_treq) DeleteIORequest((struct IORequest *)g_treq);
    if (g_tport) DeleteMsgPort(g_tport);
    if (g_port) DeleteMsgPort(g_port);
    if (g_scr) {
        int i;
        for (i = 0; i < 4096; i++) if (g_pen[i] >= 0) ReleasePen(g_scr->ViewPort.ColorMap, (ULONG)g_pen[i]);
        UnlockPubScreen(NULL, g_scr);
    }
    free(g_chunky);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    free(g_tmp);
}

/* DirectDraw 8-bit frames: truecolor screens take the palette directly
 * (WriteLUTPixelArray); CLUT screens get a pen table computed once per palette */
/* -G: read back what the display shows (the window's RastPort) into a PPM file */
static void grab_window(qwin *w, struct Window *win, int x0, int y0, int pw, int ph)
{
    uint8_t *rgb = (uint8_t *)malloc((size_t)pw * (size_t)ph * 3u);
    FILE *f;
    if (!rgb) return;
    ReadPixelArray(rgb, 0, 0, (UWORD)(pw * 3), win->RPort, (UWORD)x0, (UWORD)y0, (UWORD)pw, (UWORD)ph, RECTFMT_RGB);
    f = fopen(w->grab_path, "wb");
    if (f) { fprintf(f, "P6\n%d %d\n255\n", pw, ph); fwrite(rgb, 1, (size_t)pw * (size_t)ph * 3u, f); fclose(f); }
    qw_log(w, "AROS backend: display captured after %d frames -> %s", w->frames, w->grab_path);
    free(rgb);
}

static void ar_present8(qwin *w, qwnd *t, const uint8_t *pix, int pitch, const uint32_t *pal, int pw, int ph)
{
    static uint32_t lastpal[256];
    static UBYTE pens[256];
    static int havepens;
    bew *b = (bew *)t->be;
    int x0, y0, y, x;
    (void)w;
    if (!b || !b->win) return;
    if (pw > t->cw) pw = t->cw;
    if (ph > t->ch) ph = t->ch;
    if (pw <= 0 || ph <= 0) return;
    x0 = b->win->BorderLeft; y0 = b->win->BorderTop + b->mbh;
    if (b->scr) {                                   /* own screen: exact colours, no remapping */
        if (b->scr8) {
            if (memcmp(b->pal, pal, sizeof(b->pal))) {
                static ULONG tab[256 * 3 + 2];
                int i;
                tab[0] = (256UL << 16) | 0;
                for (i = 0; i < 256; i++) {
                    tab[1 + i * 3] = ((pal[i] >> 16) & 0xFF) * 0x01010101u;
                    tab[2 + i * 3] = ((pal[i] >> 8) & 0xFF) * 0x01010101u;
                    tab[3 + i * 3] = (pal[i] & 0xFF) * 0x01010101u;
                }
                tab[1 + 256 * 3] = 0;
                LoadRGB32(&b->scr->ViewPort, tab);
                memcpy(b->pal, pal, sizeof(b->pal));
            }
            WriteChunkyPixels(b->win->RPort, 0, 0, (ULONG)(pw - 1), (ULONG)(ph - 1), (UBYTE *)pix, (LONG)pitch);
        } else WriteLUTPixelArray((APTR)pix, 0, 0, (UWORD)pitch, b->win->RPort, (APTR)pal, 0, 0, (UWORD)pw, (UWORD)ph, CTABFMT_XRGB8);
        if (w->grab_path[0] && ++w->frames == w->grab_at) grab_window(w, b->win, 0, 0, pw, ph);
        return;
    }
    if (g_truecolor) {
        WriteLUTPixelArray((APTR)pix, 0, 0, (UWORD)pitch, b->win->RPort, (APTR)pal, (UWORD)x0, (UWORD)y0, (UWORD)pw, (UWORD)ph, CTABFMT_XRGB8);
        return;
    }
    if (!havepens || memcmp(lastpal, pal, sizeof(lastpal))) {
        for (x = 0; x < 256; x++) pens[x] = (UBYTE)pen_for(pal[x]);
        memcpy(lastpal, pal, sizeof(lastpal));
        havepens = 1;
    }
    {
        size_t m = (size_t)pw * (size_t)ph;
        if (m > g_chunkyn) { free(g_chunky); g_chunky = (UBYTE *)malloc(m); g_chunkyn = g_chunky ? m : 0; }
        if (!g_chunky) return;
        for (y = 0; y < ph; y++) {
            const uint8_t *s = pix + (size_t)y * (size_t)pitch;
            UBYTE *d = g_chunky + (size_t)y * (size_t)pw;
            for (x = 0; x < pw; x++) d[x] = pens[s[x]];
        }
        WriteChunkyPixels(b->win->RPort, (ULONG)x0, (ULONG)y0, (ULONG)(x0 + pw - 1), (ULONG)(y0 + ph - 1), g_chunky, (LONG)pw);
    }
}

static void ar_reopen(qwin *w, qwnd *t)
{
    if (w->verbose) qw_log(w, "AROS backend: reopening window %08x (%s)", t->h, t->fs_w ? "own screen" : "Workbench");
    ar_destroy(w, t);
    if (w->verbose) qw_log(w, "AROS backend: old window closed");
    ar_create(w, t);
}

/* RTG direct: the own screen's bitmap (plus a second screen buffer for page flips)
 * stays locked with LockBitMapTags while DirectDraw uses it.  The lock gives the
 * frame buffer address and keeps the RTG driver from moving the bitmap between
 * video and fast memory; the program then draws straight into it. */
static void ar_fb_close(qwin *w, qwnd *t)
{
    bew *b = (bew *)t->be;
    int i;
    if (!b || !b->fbn) return;
    for (i = 0; i < 2; i++) if (b->lock[i]) { UnLockBitMap(b->lock[i]); b->lock[i] = NULL; }
    if (b->sb[1]) {
        if (b->fbshown == 1) { int k; for (k = 0; k < 50 && !ChangeScreenBuffer(b->scr, b->sb[0]); k++) WaitTOF(); WaitTOF(); }
        FreeScreenBuffer(b->scr, b->sb[1]);
    }
    if (b->sb[0]) FreeScreenBuffer(b->scr, b->sb[0]);
    b->sb[0] = b->sb[1] = NULL;
    b->fbn = 0;
    if (w->verbose) qw_log(w, "AROS backend: RTG display buffers unlocked");
}
static int ar_fb_open(qwin *w, qwnd *t, int nbuf, uint8_t **buf, int *pitch)
{
    bew *b = (bew *)t->be;
    int i;
    if (!b || !b->scr || !b->scr8 || !CyberGfxBase || b->fbn || nbuf < 1 || nbuf > 2) return 0;
    b->fbshown = 0;
    for (i = 0; i < nbuf; i++) {
        IPTR addr = 0;
        ULONG bpr = 0, fmt = 0;
        b->sb[i] = AllocScreenBuffer(b->scr, NULL, i ? 0 : SB_SCREEN_BITMAP);
        if (!b->sb[i]) { qw_log(w, "AROS: AllocScreenBuffer %d failed", i); break; }
        b->lock[i] = LockBitMapTags(b->sb[i]->sb_BitMap, LBMI_BASEADDRESS, (IPTR)&addr, LBMI_BYTESPERROW, (IPTR)&bpr, LBMI_PIXFMT, (IPTR)&fmt, TAG_DONE);
        if (!b->lock[i]) { qw_log(w, "AROS: LockBitMapTags on screen buffer %d failed (no direct RTG access)", i); break; }
        if (fmt != PIXFMT_LUT8 || !addr || (i && bpr != b->fbpitch)) { qw_log(w, "AROS: screen buffer %d: pixel format %lu, %lu bytes per row - not usable", i, (unsigned long)fmt, (unsigned long)bpr); break; }
        b->fbaddr[i] = (UBYTE *)addr; b->fbpitch = bpr;
        buf[i] = (uint8_t *)addr;
        if (w->verbose) qw_log(w, "AROS backend: screen buffer %d at %08lx, %lu bytes per row", i, (unsigned long)addr, (unsigned long)bpr);
    }
    b->fbn = nbuf;                                   /* lets ar_fb_close clean up after a failure */
    if (i < nbuf) { ar_fb_close(w, t); return 0; }
    *pitch = (int)b->fbpitch;
    return nbuf;
}
/* -G: in RTG direct mode the capture comes from the shown buffer itself */
static void grab_fb(qwin *w, const UBYTE *p, ULONG pitch, const uint32_t *pal, int pw, int ph)
{
    FILE *f = fopen(w->grab_path, "wb");
    int x, y;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", pw, ph);
    for (y = 0; y < ph; y++) for (x = 0; x < pw; x++) {
        uint32_t c = pal[p[(size_t)y * pitch + (size_t)x]];
        fputc((int)(c >> 16) & 0xFF, f); fputc((int)(c >> 8) & 0xFF, f); fputc((int)c & 0xFF, f);
    }
    fclose(f);
    qw_log(w, "AROS backend: display buffer captured after %d frames -> %s", w->frames, w->grab_path);
}
static uint8_t *ar_fb_show(qwin *w, qwnd *t, int i, const uint32_t *pal)
{
    bew *b = (bew *)t->be;
    if (!b || i < 0 || i >= b->fbn) return NULL;
    if (memcmp(b->pal, pal, sizeof(b->pal))) {
        static ULONG tab[256 * 3 + 2];
        int k;
        tab[0] = (256UL << 16) | 0;
        for (k = 0; k < 256; k++) {
            tab[1 + k * 3] = ((pal[k] >> 16) & 0xFF) * 0x01010101u;
            tab[2 + k * 3] = ((pal[k] >> 8) & 0xFF) * 0x01010101u;
            tab[3 + k * 3] = (pal[k] & 0xFF) * 0x01010101u;
        }
        tab[1 + 256 * 3] = 0;
        LoadRGB32(&b->scr->ViewPort, tab);
        memcpy(b->pal, pal, sizeof(b->pal));
    }
    if (b->fbn > 1 && i != b->fbshown) {             /* page flip */
        int k;
        for (k = 0; k < 50 && !ChangeScreenBuffer(b->scr, b->sb[i]); k++) WaitTOF();
        b->fbshown = i;
        {   /* the driver may have moved the bitmap into video memory to show it */
            IPTR addr = 0;
            APTR l = LockBitMapTags(b->sb[i]->sb_BitMap, LBMI_BASEADDRESS, (IPTR)&addr, TAG_DONE);
            if (l) { UnLockBitMap(l); if (addr) b->fbaddr[i] = (UBYTE *)addr; }
        }
    }
    if (w->grab_path[0] && ++w->frames == w->grab_at) grab_fb(w, b->fbaddr[i], b->fbpitch, pal, t->fs_w, t->fs_h);
    return b->fbaddr[i];
}

const qbackend qw_aros = { "aros", ar_init, ar_create, ar_destroy, ar_present, ar_title, ar_menu, ar_wait, ar_now, ar_shutdown, ar_present8, ar_reopen,
                           ar_fb_open, ar_fb_show, ar_fb_close };
#endif
