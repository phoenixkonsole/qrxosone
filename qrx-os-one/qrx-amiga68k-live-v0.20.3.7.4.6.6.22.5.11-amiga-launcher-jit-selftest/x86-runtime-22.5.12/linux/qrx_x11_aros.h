/* Intuition backend: one top-level X window per native public-screen window.
 * Uses the same WritePixelArray presentation primitive as Win32 HLE; no screen
 * ownership or global event port is borrowed from that independent runtime. */
#include <cybergraphx/cybergraphics.h>
#include <devices/inputevent.h>
#include <exec/types.h>
#include <graphics/view.h>
#include <intuition/intuition.h>
#include <proto/cybergraphics.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/keymap.h>
struct Library *CyberGfxBase;
typedef struct {
    struct Window *win;
    struct Screen *screen;
    struct Library *cg;
    int truecolor;
    UBYTE *chunky;
    LONG pens[4096];
} qxx_native;
static void native_present(qxx_window *w) {
    qxx_native *b = w->native;
    if (!b || !w->mapped)
        return;
    if (b->truecolor)
        WritePixelArray(w->pixels, 0, 0, w->w * 4, b->win->RPort, b->win->BorderLeft,
                        b->win->BorderTop, w->w, w->h, RECTFMT_ARGB);
    else {
        int i;
        for (i = 0; i < w->w * w->h; i++) {
            uint32_t rgb = w->pixels[i];
            unsigned k = ((rgb >> 12) & 0xf00) | ((rgb >> 8) & 0xf0) | ((rgb >> 4) & 15);
            if (b->pens[k] < 0)
                b->pens[k] =
                    ObtainBestPen(b->screen->ViewPort.ColorMap, ((rgb >> 16) & 255) * 0x01010101u,
                                  ((rgb >> 8) & 255) * 0x01010101u, (rgb & 255) * 0x01010101u,
                                  OBP_Precision, PRECISION_IMAGE, TAG_DONE);
            b->chunky[i] = (UBYTE)(b->pens[k] < 0 ? 1 : b->pens[k]);
        }
        WriteChunkyPixels(b->win->RPort, b->win->BorderLeft, b->win->BorderTop,
                          b->win->BorderLeft + w->w - 1, b->win->BorderTop + w->h - 1, b->chunky,
                          w->w);
    }
}
static void native_close(qxx_window *w) {
    qxx_native *b = w->native;
    if (!b)
        return;
    if (b->win)
        CloseWindow(b->win);
    if (b->screen) {
        unsigned i;
        for (i = 0; i < 4096; i++)
            if (b->pens[i] >= 0)
                ReleasePen(b->screen->ViewPort.ColorMap, (ULONG)b->pens[i]);
        UnlockPubScreen(NULL, b->screen);
    }
    free(b->chunky);
    if (b->cg)
        CloseLibrary(b->cg);
    free(b);
    w->native = NULL;
}
static int native_open(qxx_window *w) {
    qxx_native *b = calloc(1, sizeof(*b));
    unsigned i;
    if (!b)
        return 0;
    w->native = b;
    for (i = 0; i < 4096; i++)
        b->pens[i] = -1;
    b->cg = OpenLibrary((CONST_STRPTR) "cybergraphics.library", 40);
    CyberGfxBase = b->cg;
    b->screen = LockPubScreen(NULL);
    if (!b->screen) {
        fprintf(stderr, "qrx-x11: no public screen\n");
        native_close(w);
        return 0;
    }
    b->truecolor = b->cg && GetCyberMapAttr(b->screen->RastPort.BitMap, CYBRMATTR_ISCYBERGFX) &&
                   GetCyberMapAttr(b->screen->RastPort.BitMap, CYBRMATTR_DEPTH) > 8;
    if (!b->truecolor) {
        b->chunky = malloc((size_t)w->w * w->h);
        if (!b->chunky) {
            native_close(w);
            return 0;
        }
    }
    b->win = OpenWindowTags(NULL, WA_PubScreen, (IPTR)b->screen, WA_Title, (IPTR)w->title, WA_Left,
                            (ULONG)w->x, WA_Top, (ULONG)w->y, WA_InnerWidth, (ULONG)w->w,
                            WA_InnerHeight, (ULONG)w->h, WA_DragBar, TRUE, WA_DepthGadget, TRUE,
                            WA_CloseGadget, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE,
                            WA_ReportMouse, TRUE, WA_SimpleRefresh, TRUE, WA_IDCMP,
                            IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_RAWKEY |
                                IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE,
                            TAG_DONE);
    if (!b->win) {
        fprintf(stderr, "qrx-x11: OpenWindow failed\n");
        native_close(w);
        return 0;
    }
    fprintf(stderr, "qrx-x11: window %08lx opened (%s)\n", (unsigned long)w->id,
            b->truecolor ? "RTG truecolor" : "palette");
    return 1;
}
static void native_title(qxx_window *w) {
    qxx_native *b = w->native;
    if (b)
        SetWindowTitles(b->win, (CONST_STRPTR)w->title, (CONST_STRPTR)-1);
}
void qxx_pump(qxx *x) {
    unsigned i;
    for (i = 0; i < QXX_WINDOWS; i++) {
        qxx_window *w = &x->windows[i];
        qxx_native *b = w->native;
        struct IntuiMessage *m;
        if (!b)
            continue;
        while ((m = (struct IntuiMessage *)GetMsg(b->win->UserPort))) {
            ULONG cls = m->Class;
            UWORD code = m->Code, qual = m->Qualifier;
            int px = m->MouseX - b->win->BorderLeft, py = m->MouseY - b->win->BorderTop;
            ReplyMsg((struct Message *)m);
            if (cls == IDCMP_CLOSEWINDOW) {
                if (w->mask & (1u << 17))
                    event(x, w, 17, 0, 0, 0);
                native_close(w);
                free(w->pixels);
                memset(w, 0, sizeof(*w));
                break;
            }
            if (cls == IDCMP_REFRESHWINDOW) {
                BeginRefresh(b->win);
                native_present(w);
                EndRefresh(b->win, TRUE);
                if (w->mask & (1u << 15))
                    event(x, w, 12, 0, 0, 0);
            } else if (cls == IDCMP_MOUSEMOVE) {
                if (w->mask & (1u << 6))
                    event(x, w, 6, 0, px, py);
            } else if (cls == IDCMP_MOUSEBUTTONS) {
                int up = (code & IECODE_UP_PREFIX) != 0;
                int button = (code & ~IECODE_UP_PREFIX) == IECODE_LBUTTON   ? 1
                             : (code & ~IECODE_UP_PREFIX) == IECODE_RBUTTON ? 3
                                                                            : 2;
                if (w->mask & (1u << (up ? 3 : 2)))
                    event(x, w, up ? 5 : 4, button, px, py);
            } else if (cls == IDCMP_RAWKEY) {
                struct InputEvent ie;
                char ch[8];
                LONG n;
                int up = (code & IECODE_UP_PREFIX) != 0;
                memset(&ie, 0, sizeof(ie));
                ie.ie_Class = IECLASS_RAWKEY;
                ie.ie_Code = code & ~IECODE_UP_PREFIX;
                ie.ie_Qualifier = qual;
                n = MapRawKey(&ie, ch, sizeof(ch), NULL);
                if (n == 1 && (unsigned char)ch[0] >= 8 && (w->mask & (1u << (up ? 1 : 0))))
                    event(x, w, up ? 3 : 2, (unsigned char)ch[0], px, py);
            }
        }
    }
}
