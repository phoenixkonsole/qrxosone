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

static void ar_create(qwin *w, qwnd *t)
{
    bew *b = (bew *)calloc(1, sizeof(bew));
    int popup = (t->style & WS_POPUP) && !(t->style & WS_CAPTION);
    if (!b) return;
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

static void ar_destroy(qwin *w, qwnd *t)
{
    bew *b = (bew *)t->be;
    (void)w;
    if (!b) return;
    if (b->win) strip_close(b->win);
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
    if (mbh) {
        surf_resize(&b->mb, t->cw, mbh);
        user_render_menubar(w, t, &b->mb);
        blit(b->win, b->mb.pix, t->cw, b->win->BorderLeft, b->win->BorderTop, t->cw, mbh);
    }
    blit(b->win, t->surf.pix, t->surf.w, b->win->BorderLeft, b->win->BorderTop + mbh, t->cw < t->surf.w ? t->cw : t->surf.w, t->ch < t->surf.h ? t->ch : t->surf.h);
}

static void ar_title(qwin *w, qwnd *t)
{
    bew *b = (bew *)t->be;
    (void)w;
    if (!b || !b->win) return;
    latin1(b->title, t->text, sizeof(b->title));
    SetWindowTitles(b->win, (CONST_STRPTR)b->title, (CONST_STRPTR)~0);
}

static void ar_menu(qwin *w, qwnd *t) { t->present = 1; (void)w; }

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
        if (t) t->present = 1;
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

const qbackend qw_aros = { "aros", ar_init, ar_create, ar_destroy, ar_present, ar_title, ar_menu, ar_wait, ar_now, ar_shutdown };
#endif
