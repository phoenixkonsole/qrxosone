/* QRX OS One 22.5.12 - qrx-win32: USER (windows, messages, dialogs,
 * standard controls, menus, timers, keyboard/mouse input). */
#include <stdlib.h>
#include <string.h>
#include "qrxwin.h"
#include <ctype.h>

#define FN(name) static uint32_t name(qwin *w, uint32_t *a)
#define C (&w->cpu)
#define RD32(x) x86_rd32(C, (x))
#define WR32(x, v) x86_wr32(C, (x), (v))
#define UNUSED (void)w; (void)a
#define WND_BASE 0x00010000u
#define MENU_BASE 0x00300000u
#define CW_USEDEFAULT 0x80000000u
#define BASEX 6
#define BASEY 13

enum { B_NONE = 0, B_BUTTON, B_STATIC, B_EDIT, B_DIALOG, B_LISTBOX, B_SCROLLBAR, B_COMBOBOX, B_MENU };

static uint32_t def_window_proc(qwin *w, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp);
static void win_destroy(qwin *w, qwnd *x);
static void set_focus(qwin *w, uint32_t h);
static void activate_top(qwin *w, uint32_t h);
static qwnd *find_ctl(qwin *w, qwnd *p, uint32_t id);
struct qsheet;
static int sheet_proc(qwin *w, struct qsheet *s, uint32_t *a, uint32_t *ret);
static int sheet_key(qwin *w, qwnd *f, uint32_t hwnd, uint32_t msg, uint32_t wp);
static int g_dlg_child;                                 /* the next dialog is a page inside a frame */

/* ====================================================================== handles */
qwnd *W(qwin *w, uint32_t h)
{
    uint32_t i;
    if (h < WND_BASE || ((h - WND_BASE) & 3)) return NULL;
    i = (h - WND_BASE) >> 2;
    if (i >= QW_MAXWND || !w->wnd[i].used || w->wnd[i].destroyed) return NULL;
    return &w->wnd[i];
}
qmenu *M(qwin *w, uint32_t h)
{
    uint32_t i = h - MENU_BASE;
    if (h < MENU_BASE || i >= QW_MAXMENU || !w->menu[i].used) return NULL;
    return &w->menu[i];
}
static uint32_t menu_new(qwin *w)
{
    int i;
    for (i = 1; i < QW_MAXMENU; i++) if (!w->menu[i].used) { memset(&w->menu[i], 0, sizeof(qmenu)); w->menu[i].used = 1; return MENU_BASE + (uint32_t)i; }
    return 0;
}
qwnd *win_top(qwin *w, qwnd *x)
{
    while (x && (x->style & WS_CHILD)) { qwnd *p = W(w, x->parent); if (!p) break; x = p; }
    return x;
}
void win_abs_origin(qwin *w, qwnd *x, int *ax, int *ay)
{
    int sx = 0, sy = 0;
    while (x && (x->style & WS_CHILD)) { sx += x->x; sy += x->y; x = W(w, x->parent); }
    *ax = sx; *ay = sy;
}
/* screen position of a window's client origin */
static void client_screen(qwin *w, qwnd *x, int *sx, int *sy)
{
    int ax, ay;
    qwnd *t = win_top(w, x);
    win_abs_origin(w, x, &ax, &ay);
    *sx = (t ? t->x : 0) + ax;
    *sy = (t ? t->y : 0) + ay;
}

/* ====================================================================== classes */
static qclass *find_class(qwin *w, uint32_t name_or_atom, const wchar16 *hostname)
{
    int i;
    wchar16 n[64];
    if (hostname) wcpy16(n, hostname, 64);
    else if (name_or_atom < 0x10000u) { for (i = 0; i < QW_MAXCLS; i++) if (w->cls[i].used && w->cls[i].atom == name_or_atom) return &w->cls[i]; return NULL; }
    else qw_rd_wstr(w, name_or_atom, n, 64);
    for (i = 0; i < QW_MAXCLS; i++) if (w->cls[i].used && !wicmp16(w->cls[i].name, n)) return &w->cls[i];
    return NULL;
}
static qclass *add_class(qwin *w, const wchar16 *name)
{
    int i;
    for (i = 0; i < QW_MAXCLS; i++)
        if (!w->cls[i].used) {
            memset(&w->cls[i], 0, sizeof(qclass));
            w->cls[i].used = 1;
            wcpy16(w->cls[i].name, name, 64);
            w->cls[i].atom = 0xC000u + (uint32_t)i;
            return &w->cls[i];
        }
    return NULL;
}

/* ====================================================================== messages */
uint32_t win_send(qwin *w, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp)
{
    qwnd *x = W(w, h);
    if (!x || w->exiting) return 0;
    return qw_call(w, x->wndproc, 4, h, msg, wp, lp);
}
void win_post(qwin *w, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp)
{
    int n = (w->qt + 1) % QW_MAXQ;
    if (n == w->qh) { qw_log(w, "message queue full, dropping %04x", msg); return; }
    w->q[w->qt].hwnd = h; w->q[w->qt].msg = msg; w->q[w->qt].wp = wp; w->q[w->qt].lp = lp;
    w->q[w->qt].time = (uint32_t)w->be->now(w); w->q[w->qt].x = w->mouse_x; w->q[w->qt].y = w->mouse_y;
    w->qt = n;
}

void win_invalidate(qwin *w, qwnd *x, int erase)
{
    int i;
    if (!x) return;
    x->dirty = 1;
    if (erase) x->erase = 1;
    for (i = 0; i < QW_MAXWND; i++) {
        qwnd *c = &w->wnd[i];
        if (c->used && !c->destroyed && c->parent == x->h && (c->style & WS_CHILD)) win_invalidate(w, c, 1);
    }
}

static int is_visible(qwin *w, qwnd *x)
{
    while (x) {
        if (!x->visible) return 0;
        if (!(x->style & WS_CHILD)) return 1;
        x = W(w, x->parent);
    }
    return 0;
}

/* ====================================================================== non-client metrics */
static void nc_metrics(qwin *w, qwnd *x)
{
    x->nl = x->nt = x->nr = x->nb = 0;
    if (x->style & WS_CHILD) {
        if ((x->exstyle & WS_EX_CLIENTEDGE)) { x->nl = x->nt = x->nr = x->nb = 2; }
        else if (x->style & WS_BORDER) { x->nl = x->nt = x->nr = x->nb = 1; }
        return;
    }
    if (x->style & (WS_THICKFRAME | WS_DLGFRAME | WS_CAPTION | WS_BORDER)) x->nl = x->nr = x->nb = x->nt = w->frame_w;
    if ((x->style & WS_CAPTION) == WS_CAPTION) x->nt += w->caption_h;
    if (x->menu && M(w, x->menu)) x->nt += w->menu_bar_h;
}

/* ====================================================================== window creation */
static qwnd *alloc_wnd(qwin *w)
{
    int i;
    for (i = 1; i < QW_MAXWND; i++)
        if (!w->wnd[i].used || w->wnd[i].destroyed) {
            free(w->wnd[i].extra);
            free(w->wnd[i].surf.pix);
            memset(&w->wnd[i], 0, sizeof(qwnd));
            w->wnd[i].used = 1;
            w->wnd[i].h = WND_BASE + ((uint32_t)i << 2);
            return &w->wnd[i];
        }
    return NULL;
}

static void show_top(qwin *w, qwnd *x)
{
    if (x->style & WS_CHILD) return;
    if (!x->be && w->be->create) w->be->create(w, x);
    if (!w->main_wnd) w->main_wnd = x->h;
    w->active = x->h;
}

#define QW_MAXCBT 4
static uint32_t g_cbthook[QW_MAXCBT];    /* WH_CBT */
static uint32_t create_window(qwin *w, uint32_t exstyle, qclass *cls, const wchar16 *title, uint32_t style,
                              int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t parent, uint32_t menu,
                              uint32_t hinst, uint32_t param)
{
    qwnd *n = alloc_wnd(w), *p;
    uint32_t cs, r, h;
    if (!n || !cls) return 0;
    h = n->h;
    n->cls = cls;
    n->style = style;
    n->exstyle = exstyle;
    n->wndproc = cls->wndproc;
    n->hinst = hinst;
    n->enabled = !(style & WS_DISABLED);
    n->nextra = (int)cls->cbwnd + 16;
    n->extra = (uint8_t *)calloc(1, (size_t)n->nextra);
    if (title) wcpy16(n->text, title, 256);
    if (style & WS_CHILD) { n->parent = parent; n->id = menu; }
    else {
        n->owner = parent;
        if (menu) n->menu = menu;
        else if (cls->menuid || cls->menuname[0]) {
            uint32_t d, s;
            int ok = 0;
            if (cls->menuid) ok = res_find_id(w, 4, (int)cls->menuid, &d, &s);
            else { uint32_t g = qw_alloc(w, 130); qw_wr_wstr(w, g, cls->menuname, 64); ok = res_find(w, 4, g, &d, &s); qw_free(w, g); }
            if (ok) n->menu = menu_load(w, d);
        }
    }
    nc_metrics(w, n);
    if (!(style & WS_CHILD)) {
        if ((uint32_t)x == CW_USEDEFAULT) { x = 40 + (int32_t)(w->active ? 24 : 0); y = 40; }
        if ((uint32_t)cx == CW_USEDEFAULT) { cx = 320; cy = 240; }
        n->x = x + n->nl; n->y = y + n->nt;
        n->cw = cx - n->nl - n->nr; n->ch = cy - n->nt - n->nb;
    } else {
        p = W(w, parent);
        n->x = x + n->nl; n->y = y + n->nt;
        n->cw = cx - n->nl - n->nr; n->ch = cy - n->nt - n->nb;
        if (p) n->font = p->font;
        (void)p;
    }
    if (n->cw < 0) n->cw = 0;
    if (n->ch < 0) n->ch = 0;
    if (!(style & WS_CHILD)) surf_resize(&n->surf, n->cw, n->ch);
    /* CREATESTRUCTW in guest memory */
    cs = qw_alloc(w, 48);
    {
        uint32_t tp = qw_alloc(w, 520), cp = qw_alloc(w, 132);
        qw_wr_wstr(w, tp, n->text, 256);
        qw_wr_wstr(w, cp, cls->name, 64);
        WR32(cs, param); WR32(cs + 4, hinst); WR32(cs + 8, menu); WR32(cs + 12, parent);
        WR32(cs + 16, (uint32_t)cy); WR32(cs + 20, (uint32_t)cx); WR32(cs + 24, (uint32_t)y); WR32(cs + 28, (uint32_t)x);
        WR32(cs + 32, style); WR32(cs + 36, tp); WR32(cs + 40, cp); WR32(cs + 44, exstyle);
    }
    /* HCBT_CREATEWND, before the window's first message: MFC attaches its object here. */
    {
        int k;
        for (k = 0; k < QW_MAXCBT; k++) if (g_cbthook[k]) {
            uint32_t g = qw_alloc(w, 8);
            WR32(g, cs); WR32(g + 4, 0);
            qw_call(w, g_cbthook[k], 3, 3, h, g, 0);
            qw_free(w, g);
            if (!W(w, h)) return 0;
        }
    }
    r = win_send(w, h, WM_NCCREATE, 0, cs);
    if (!W(w, h)) return 0;
    if (!r) { qw_log(w, "WM_NCCREATE refused"); win_destroy(w, n); return 0; }
    r = win_send(w, h, WM_CREATE, 0, cs);
    if (!W(w, h)) return 0;
    if (r == 0xFFFFFFFFu) { win_destroy(w, n); return 0; }
    win_send(w, h, WM_SIZE, 0, ((uint32_t)n->ch << 16) | (uint32_t)n->cw);
    if (style & WS_VISIBLE) {
        n->visible = 1;
        show_top(w, n);
        win_invalidate(w, n, 1);
    }
    return h;
}

static void win_destroy(qwin *w, qwnd *x)
{
    int i;
    uint32_t h;
    if (!x || x->destroyed) return;
    h = x->h;
    for (i = 0; i < QW_MAXWND; i++) {                      /* children first */
        qwnd *c = &w->wnd[i];
        if (c->used && !c->destroyed && ((c->parent == h && (c->style & WS_CHILD)) || c->owner == h) && c != x) win_destroy(w, c);
    }
    win_send(w, h, WM_DESTROY, 0, 0);
    win_send(w, h, WM_NCDESTROY, 0, 0);
    x = W(w, h);
    if (!x) return;
    for (i = 0; i < QW_MAXTIMER; i++) if (w->timer[i].used && w->timer[i].hwnd == h) w->timer[i].used = 0;
    if (!(x->style & WS_CHILD) && x->be && w->be->destroy) w->be->destroy(w, x);
    x->destroyed = 1;
    if (w->focus == h) w->focus = 0;
    if (w->capture == h) w->capture = 0;
    if (w->active == h) {
        /* Windows activates the owner (else the next top-level window) and tells it: games
         * such as Freddi Fish pause their sound while a dialog is active and resume it on
         * WM_ACTIVATE. */
        qwnd *n = W(w, x->owner);
        uint32_t old_focus = w->focus;
        w->active = 0;
        if (!n || n->destroyed || (n->style & WS_CHILD) || !n->visible) {
            n = NULL;
            for (i = QW_MAXWND - 1; i > 0; i--) { qwnd *t = &w->wnd[i]; if (t->used && !t->destroyed && !(t->style & WS_CHILD) && t->visible) { n = t; break; } }
        }
        if (n && !w->exiting) {
            qwnd *f = W(w, old_focus);
            while (f && (f->style & WS_CHILD) && W(w, f->parent)) f = W(w, f->parent);
            if (f == n) activate_top(w, n->h); else set_focus(w, n->h);
        } else if (n) w->active = n->h;
    }
    if (w->main_wnd == h) w->main_wnd = 0;
    {   /* parent must repaint the uncovered area */
        qwnd *p = W(w, x->parent);
        if (p && (x->style & WS_CHILD)) win_invalidate(w, p, 1);
    }
}

/* ====================================================================== painting */
static uint32_t ctlcolor(qwin *w, qwnd *x, uint32_t hdc, uint32_t msg)
{
    qwnd *p = W(w, x->parent);
    uint32_t br = 0;
    if (p) br = win_send(w, p->h, msg, hdc, x->h);
    return br;
}
static uint32_t brush_rgb(qwin *w, uint32_t hbr, uint32_t def)
{
    qobj *o;
    if (!hbr) return def;
    if (hbr <= 31) return sys_color((int)hbr - 1);
    o = O(w, hbr, GO_BRUSH);
    return o && o->style != 1 ? o->color : def;
}

static void draw_border(qwin *w, qwnd *x)
{
    /* child window border (WS_EX_CLIENTEDGE / WS_BORDER) drawn into the parent's area */
    qwnd *p = W(w, x->parent);
    uint32_t hdc;
    qdc *d;
    if (!p || (!x->nl && !x->nt)) return;
    hdc = dc_for_window(w, p, 1);
    d = DCP(w, hdc);
    if (d) {
        int l = x->x - x->nl, t = x->y - x->nt, r = x->x + x->cw + x->nr, b = x->y + x->ch + x->nb;
        if (x->exstyle & WS_EX_CLIENTEDGE) gdi_frame3d(d, l, t, r, b, 1, 1);
        else { gdi_fill(d, l, t, r, t + 1, 0); gdi_fill(d, l, b - 1, r, b, 0); gdi_fill(d, l, t, l + 1, b, 0); gdi_fill(d, r - 1, t, r, b, 0); }
    }
    dc_release(w, hdc);
}

/* ====================================================================== builtin controls */
static int btype(qwnd *x) { return (int)(x->style & 0xF); }

static void button_paint(qwin *w, qwnd *x, uint32_t hdc)
{
    qdc *d = DCP(w, hdc);
    int t = btype(x), rc[4];
    uint32_t br;
    if (!d) return;
    rc[0] = 0; rc[1] = 0; rc[2] = x->cw; rc[3] = x->ch;
    if (t == 0xB) {                                  /* BS_OWNERDRAW */
        uint32_t dis = qw_alloc(w, 48);
        qwnd *p = W(w, x->parent);
        WR32(dis, 4); WR32(dis + 4, x->id); WR32(dis + 8, 0); WR32(dis + 12, 1);
        WR32(dis + 16, ((x->state & 4) ? 1u : 0u) | (x->enabled ? 0u : 4u) | (w->focus == x->h ? 0x10u : 0u));
        WR32(dis + 20, x->h); WR32(dis + 24, hdc);
        WR32(dis + 28, 0); WR32(dis + 32, 0); WR32(dis + 36, (uint32_t)x->cw); WR32(dis + 40, (uint32_t)x->ch); WR32(dis + 44, 0);
        if (p) win_send(w, p->h, WM_DRAWITEM, x->id, dis);
        qw_free(w, dis);
        return;
    }
    br = ctlcolor(w, x, hdc, t == 0 || t == 1 ? WM_CTLCOLORBTN : WM_CTLCOLORSTATIC);
    if (t == 7) {                                    /* BS_GROUPBOX */
        int fh = gdi_font_height(0), tw = gdi_text_width(0, x->text, wlen16(x->text));
        /* Only a frame: the controls inside it are its siblings and may be painted before it. */
        gdi_frame3d(d, 0, fh / 2, x->cw, x->ch, 1, 0);
        gdi_frame3d(d, 1, fh / 2 + 1, x->cw, x->ch, 0, 0);
        if (x->text[0]) {
            int r2[4]; r2[0] = 8; r2[1] = 0; r2[2] = 8 + tw + 4; r2[3] = fh;
            gdi_fill(d, 6, 0, 12 + tw, fh, brush_rgb(w, br, sys_color(15)));
            d->textcol = x->enabled ? 0 : 0x808080; d->bkmode = 1;
            gdi_drawtext(d, x->text, wlen16(x->text), r2, 0x20);
        }
        return;
    }
    if (t == 0 || t == 1) {                          /* push buttons */
        gdi_fill(d, 0, 0, x->cw, x->ch, sys_color(15));
        if (t == 1 || w->focus == x->h) {
            gdi_fill(d, 0, 0, x->cw, 1, 0); gdi_fill(d, 0, x->ch - 1, x->cw, x->ch, 0);
            gdi_fill(d, 0, 0, 1, x->ch, 0); gdi_fill(d, x->cw - 1, 0, x->cw, x->ch, 0);
            if (x->state & 4) gdi_frame3d(d, 1, 1, x->cw - 1, x->ch - 1, 1, 0);
            else gdi_frame3d(d, 1, 1, x->cw - 1, x->ch - 1, 0, 1);
        } else gdi_frame3d(d, 0, 0, x->cw, x->ch, (x->state & 4) != 0, 1);
        d->bkmode = 1;
        if (x->state & 4) { rc[0]++; rc[1]++; rc[2]++; rc[3]++; }
        if (!x->enabled) {
            int r2[4]; memcpy(r2, rc, sizeof(r2)); r2[0]++; r2[1]++; r2[2]++; r2[3]++;
            d->textcol = 0xFFFFFF; gdi_drawtext(d, x->text, wlen16(x->text), r2, 0x25);
            d->textcol = 0x808080;
        } else d->textcol = 0;
        gdi_drawtext(d, x->text, wlen16(x->text), rc, 0x25);
        return;
    }
    {   /* check boxes and radio buttons */
        int radio = t == 4 || t == 9, cy = x->ch / 2, bx = 0;
        gdi_fill(d, 0, 0, x->cw, x->ch, brush_rgb(w, br, sys_color(15)));
        if (radio) {
            int yy;
            for (yy = 0; yy < 12; yy++) {
                int r = yy < 6 ? yy : 11 - yy, half = r < 2 ? 2 + r * 2 : 6;
                gdi_fill(d, bx + 6 - half, cy - 6 + yy, bx + 6 + half, cy - 5 + yy, 0xFFFFFF);
            }
            gdi_fill(d, bx + 2, cy - 6, bx + 10, cy - 5, 0x808080); gdi_fill(d, bx, cy - 4, bx + 1, cy + 4, 0x808080);
            gdi_fill(d, bx + 2, cy + 5, bx + 10, cy + 6, 0xD4D0C8); gdi_fill(d, bx + 11, cy - 4, bx + 12, cy + 4, 0xD4D0C8);
            if (x->state & 3) gdi_fill(d, bx + 4, cy - 2, bx + 8, cy + 2, 0);
        } else {
            gdi_fill(d, bx, cy - 6, bx + 13, cy + 7, 0xFFFFFF);
            gdi_frame3d(d, bx, cy - 6, bx + 13, cy + 7, 1, 1);
            if (x->state & 3) {
                int k;
                for (k = 0; k < 7; k++) { int yy = k < 3 ? cy - 1 + k : cy + 1 - (k - 2); gdi_fill(d, bx + 3 + k, yy, bx + 4 + k, yy + 3, 0); }
            }
        }
        rc[0] = 17; d->bkmode = 1; d->textcol = x->enabled ? 0 : 0x808080;
        gdi_drawtext(d, x->text, wlen16(x->text), rc, 0x24);
    }
}

static void notify_parent(qwin *w, qwnd *x, uint32_t code)
{
    qwnd *p = W(w, x->parent);
    if (p) win_send(w, p->h, WM_COMMAND, (code << 16) | (x->id & 0xFFFF), x->h);
}

/* clicking an auto radio button unchecks the other radio buttons of its group
 * (siblings in creation order, delimited by WS_GROUP) */
static void radio_group_clear(qwin *w, qwnd *x)
{
    int i, me = (int)((x->h - 0x00010000u) >> 2), start = me, end = me;
    for (i = me; i > 0; i--) {
        qwnd *c = &w->wnd[i];
        if (!c->used || c->destroyed || c->parent != x->parent || !(c->style & WS_CHILD)) continue;
        start = i;
        if (c->style & WS_GROUP) break;
    }
    for (i = me + 1; i < QW_MAXWND; i++) {
        qwnd *c = &w->wnd[i];
        if (!c->used || c->destroyed || c->parent != x->parent || !(c->style & WS_CHILD)) continue;
        if (c->style & WS_GROUP) break;
        end = i;
    }
    for (i = start; i <= end; i++) {
        qwnd *c = &w->wnd[i];
        if (c != x && c->used && !c->destroyed && c->parent == x->parent && c->cls && c->cls->builtin == B_BUTTON && (btype(c) == 9 || btype(c) == 4) && (c->state & 3)) {
            c->state &= ~3; win_invalidate(w, c, 1);
        }
    }
}

/* What a click does, for the keyboard and BM_CLICK as well. */
static void button_activate(qwin *w, qwnd *x)
{
    int t = btype(x);
    if (t == 3) x->state ^= 1;
    else if (t == 6) x->state = (x->state & ~3) | (((x->state & 3) + 1) % 3);
    else if (t == 9) { radio_group_clear(w, x); x->state |= 1; }
    win_invalidate(w, x, 1);
    notify_parent(w, x, 0);
}

static uint32_t button_proc(qwin *w, uint32_t *a)
{
    qwnd *x = W(w, a[0]);
    uint32_t msg = a[1];
    int t;
    if (!x) return 0;
    t = btype(x);
    switch (msg) {
    case WM_PAINT: {
        uint32_t ps = qw_alloc(w, 64), hdc;
        hdc = qw_call(w, qw_thunk(w, "user32", "BeginPaint"), 2, x->h, ps, 0, 0);
        button_paint(w, x, hdc);
        qw_call(w, qw_thunk(w, "user32", "EndPaint"), 2, x->h, ps, 0, 0);
        qw_free(w, ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (t == 7) return 0;
        w->capture = x->h;
        x->state |= 4;
        if (t == 0 || t == 1 || t == 0xB) set_focus(w, x->h);
        win_invalidate(w, x, 1);
        return 0;
    case WM_LBUTTONUP: {
        int inside = (int16_t)(a[3] & 0xFFFF) >= 0 && (int16_t)(a[3] & 0xFFFF) < x->cw && (int16_t)(a[3] >> 16) >= 0 && (int16_t)(a[3] >> 16) < x->ch;
        uint32_t h = x->h;
        if (!(x->state & 4)) return 0;
        x->state &= ~4;
        if (w->capture == x->h) w->capture = 0;
        win_invalidate(w, x, 1);
        if (inside) button_activate(w, x);
        (void)h;
        return 0; }
    case WM_MOUSEMOVE: return 0;
    case BM_GETCHECK: return x->state & 3;
    case BM_SETCHECK: x->state = (x->state & ~3) | (a[2] & 3); win_invalidate(w, x, 1); return 0;
    case BM_GETSTATE: return (uint32_t)x->state;
    case BM_SETSTATE: if (a[2]) x->state |= 4; else x->state &= ~4; win_invalidate(w, x, 1); return 0;
    case BM_SETSTYLE: x->style = (x->style & ~0xFFFFu) | (a[2] & 0xFFFF); win_invalidate(w, x, 1); return 0;
    case BM_CLICK: button_activate(w, x); return 0;
    case WM_SETTEXT: def_window_proc(w, a[0], msg, a[2], a[3]); win_invalidate(w, x, 1); return 1;
    case WM_ENABLE: win_invalidate(w, x, 1); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: if (t == 0 || t == 1) win_invalidate(w, x, 1); return 0;
    case WM_GETDLGCODE: return t == 1 ? 0x10 : t == 0 ? 0x20 : (t == 9 || t == 4) ? 0x40 : 0;
    case WM_KEYUP: if (a[2] == ' ') button_activate(w, x); return 0;
    }
    return def_window_proc(w, a[0], msg, a[2], a[3]);
}

static uint32_t static_proc(qwin *w, uint32_t *a)
{
    qwnd *x = W(w, a[0]);
    uint32_t msg = a[1];
    if (!x) return 0;
    switch (msg) {
    case WM_PAINT: {
        uint32_t ps = qw_alloc(w, 64), hdc, br;
        qdc *d;
        int st = (int)(x->style & 0x1F), rc[4];
        hdc = qw_call(w, qw_thunk(w, "user32", "BeginPaint"), 2, x->h, ps, 0, 0);
        d = DCP(w, hdc);
        if (d) {
            br = ctlcolor(w, x, hdc, WM_CTLCOLORSTATIC);
            rc[0] = 0; rc[1] = 0; rc[2] = x->cw; rc[3] = x->ch;
            if (st == 4) gdi_fill(d, 0, 0, x->cw, x->ch, 0);
            else if (st == 7) gdi_frame3d(d, 0, 0, x->cw, x->ch, 1, 0);
            else if (st == 5 || st == 8) gdi_fill(d, 0, 0, x->cw, x->ch, 0x808080);
            else if (st == 6) gdi_fill(d, 0, 0, x->cw, x->ch, 0xFFFFFF);
            else if (st == 0x10) { gdi_fill(d, 0, 0, x->cw, 1, 0x808080); gdi_fill(d, 0, 1, x->cw, 2, 0xFFFFFF); }
            else if (st == 0x11) { gdi_fill(d, 0, 0, 1, x->ch, 0x808080); gdi_fill(d, 1, 0, 2, x->ch, 0xFFFFFF); }
            else if (st == 0x12) { gdi_frame3d(d, 0, 0, x->cw, x->ch, 1, 0); }
            else if (st != 3 && st != 0xE) {
                uint32_t fmt = st == 1 ? 0x11 : st == 2 ? 0x12 : st == 0xB || st == 0xC ? 0x20 : 0x10;
                if (x->style & 0x200) fmt |= 0x24;                      /* SS_CENTERIMAGE */
                if (x->style & 0x80) fmt |= 0x800;                      /* SS_NOPREFIX */
                gdi_fill(d, 0, 0, x->cw, x->ch, brush_rgb(w, br, sys_color(15)));
                if (!br) { d->textcol = 0; }
                if (!x->enabled) d->textcol = 0x808080;
                d->bkmode = 1;
                gdi_drawtext(d, x->text, wlen16(x->text), rc, fmt);
            }
            if (x->style & 0x1000) gdi_frame3d(d, 0, 0, x->cw, x->ch, 1, 0);       /* SS_SUNKEN */
        }
        qw_call(w, qw_thunk(w, "user32", "EndPaint"), 2, x->h, ps, 0, 0);
        qw_free(w, ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return (x->style & 0x100) ? 1 : 0xFFFFFFFFu;     /* SS_NOTIFY else HTTRANSPARENT */
    case WM_LBUTTONDOWN: if (x->style & 0x100) notify_parent(w, x, 0); return 0;
    case WM_SETTEXT: def_window_proc(w, a[0], msg, a[2], a[3]); win_invalidate(w, x, 1); return 1;
    case 0x0170: case 0x0172: x->icon = a[2]; return 0;                 /* STM_SETICON / STM_SETIMAGE */
    }
    return def_window_proc(w, a[0], msg, a[2], a[3]);
}

static uint32_t edit_proc(qwin *w, uint32_t *a)
{
    qwnd *x = W(w, a[0]);
    uint32_t msg = a[1];
    if (!x) return 0;
    switch (msg) {
    case WM_PAINT: {
        uint32_t ps = qw_alloc(w, 64), hdc, br;
        qdc *d;
        int rc[4], ro = (x->style & 0x800) != 0;
        hdc = qw_call(w, qw_thunk(w, "user32", "BeginPaint"), 2, x->h, ps, 0, 0);
        d = DCP(w, hdc);
        if (d) {
            br = ctlcolor(w, x, hdc, (ro || !x->enabled) ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT);
            gdi_fill(d, 0, 0, x->cw, x->ch, brush_rgb(w, br, (ro || !x->enabled) ? sys_color(15) : 0xFFFFFF));
            if (!br) d->textcol = 0;
            if (!x->enabled) d->textcol = 0x808080;
            d->bkmode = 1;
            rc[0] = 2; rc[1] = 1; rc[2] = x->cw - 2; rc[3] = x->ch;
            gdi_drawtext(d, x->text, wlen16(x->text), rc, ((x->style & 3) == 2 ? 0x2u : (x->style & 3) == 1 ? 0x1u : 0u) | 0x820u);
            if (w->focus == x->h && !ro) {
                int tw = gdi_text_width(0, x->text, wlen16(x->text)), cx;
                cx = (x->style & 3) == 2 ? x->cw - 2 : 2 + tw;
                gdi_fill(d, cx, 2, cx + 1, 2 + gdi_font_height(0), 0);
            }
        }
        qw_call(w, qw_thunk(w, "user32", "EndPaint"), 2, x->h, ps, 0, 0);
        qw_free(w, ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: set_focus(w, x->h); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: win_invalidate(w, x, 1); return 0;
    case WM_CHAR: {
        int n = wlen16(x->text);
        if (x->style & 0x800) return 0;
        if (a[2] == 8) { if (n) x->text[n - 1] = 0; }
        else if (a[2] >= 32 && n < 255) {
            if ((x->style & 0x2000) && (a[2] < '0' || a[2] > '9')) return 0;   /* ES_NUMBER */
            if (x->caret && n >= x->caret) return 0;                            /* EM_LIMITTEXT */
            x->text[n] = (wchar16)a[2]; x->text[n + 1] = 0;
        } else return 0;
        win_invalidate(w, x, 1);
        notify_parent(w, x, 0x300);                                             /* EN_CHANGE */
        return 0; }
    case WM_GETDLGCODE:                                    /* ES_MULTILINE with ES_WANTRETURN keeps Return */
        return (x->style & 0x1004u) == 0x1004u && a[2] == 0x0D ? 0x85 : 0x81;
    case WM_SETTEXT: def_window_proc(w, a[0], msg, a[2], a[3]); win_invalidate(w, x, 1); return 1;
    case 0x00C5: x->caret = (int)a[2]; return 0;           /* EM_LIMITTEXT */
    case 0x00B1: return 0;                                 /* EM_SETSEL */
    case 0x00B0: { int n = wlen16(x->text); if (a[2]) WR32(a[2], 0); if (a[3]) WR32(a[3], (uint32_t)n); return ((uint32_t)n << 16); }
    case 0x00C2: {                                         /* EM_REPLACESEL */
        wchar16 t[256]; int n = wlen16(x->text), k = qw_rd_wstr(w, a[3], t, 256), i;
        for (i = 0; i < k && n < 255; i++) x->text[n++] = t[i];
        x->text[n] = 0; win_invalidate(w, x, 1); return 0; }
    case 0x00CF: if (a[2]) x->style |= 0x800; else x->style &= ~0x800u; win_invalidate(w, x, 1); return 1;  /* EM_SETREADONLY */
    }
    return def_window_proc(w, a[0], msg, a[2], a[3]);
}

/* listbox: items kept in host memory per window (extra[] holds an index) */
typedef struct qlb { int n, sel; wchar16 item[64][64]; uint32_t data[64]; } qlb;
static qlb *lb_of(qwin *w, qwnd *x)
{
    qlb *l;
    (void)w;
    memcpy(&l, x->extra + x->nextra - 8, sizeof(l));
    if (!l) { l = (qlb *)calloc(1, sizeof(qlb)); l->sel = -1; memcpy(x->extra + x->nextra - 8, &l, sizeof(l)); }
    return l;
}
static uint32_t listbox_proc(qwin *w, uint32_t *a)
{
    qwnd *x = W(w, a[0]);
    qlb *l;
    uint32_t msg = a[1];
    int combo;
    if (!x) return 0;
    l = lb_of(w, x);
    combo = x->cls && x->cls->builtin == B_COMBOBOX;
    if (combo) {                                           /* the same list, under other numbers */
        static const struct { uint16_t cb, lb; } map[] = {
            { 0x143, 0x180 }, { 0x144, 0x182 }, { 0x146, 0x18B }, { 0x147, 0x188 }, { 0x148, 0x189 }, { 0x149, 0x18A }, { 0x14A, 0x181 },
            { 0x14B, 0x184 }, { 0x14C, 0x18F }, { 0x14D, 0x18C }, { 0x14E, 0x186 }, { 0x150, 0x199 }, { 0x151, 0x19A }, { 0x158, 0x1A2 },
        };
        unsigned k;
        for (k = 0; k < sizeof(map) / sizeof(map[0]); k++) if (map[k].cb == msg) { msg = map[k].lb; break; }
        switch (msg) {
        case WM_CREATE: x->ch = gdi_font_height(0) + 8; break;    /* the template's height includes the open list */
        case 0x0141: case 0x014F: case 0x0155: case 0x0157: return 0;   /* limit text, drop down, UI, dropped state */
        case 0x0142: return 1;                                 /* CB_SETEDITSEL */
        case 0x0153: return 0;                                 /* CB_SETITEMHEIGHT */
        case 0x0154: return (uint32_t)gdi_font_height(0) + 2;  /* CB_GETITEMHEIGHT */
        case WM_GETDLGCODE: return 1;                          /* wants the arrows */
        case WM_GETTEXT: return l->sel >= 0 && l->sel < l->n ? (uint32_t)qw_wr_wstr(w, a[3], l->item[l->sel], (int)a[2]) : (a[2] ? (x86_wr16(C, a[3], 0), 0u) : 0u);
        case WM_GETTEXTLENGTH: return l->sel >= 0 && l->sel < l->n ? (uint32_t)wlen16(l->item[l->sel]) : 0;
        case WM_KEYDOWN:
            if ((a[2] == 0x26 || a[2] == 0x28) && l->n) {
                int to = l->sel + (a[2] == 0x28 ? 1 : -1);
                if (to >= 0 && to < l->n && to != l->sel) { l->sel = to; win_invalidate(w, x, 1); notify_parent(w, x, 1); }
            }
            return 0;
        case WM_LBUTTONDOWN:                                   /* no open list here: a click takes the next entry */
            set_focus(w, x->h);
            if (l->n) { l->sel = (l->sel + 1) % l->n; win_invalidate(w, x, 1); notify_parent(w, x, 1); }
            return 0;
        case WM_PAINT: {
            uint32_t ps = qw_alloc(w, 64), hdc = qw_call(w, qw_thunk(w, "user32", "BeginPaint"), 2, x->h, ps, 0, 0);
            qdc *d = DCP(w, hdc);
            if (d) {
                int rc[4];
                gdi_fill(d, 0, 0, x->cw, x->ch, 0x808080);
                gdi_fill(d, 1, 1, x->cw - 1, x->ch - 1, x->enabled ? 0xFFFFFF : sys_color(15));
                gdi_fill(d, x->cw - 16, 1, x->cw - 1, x->ch - 1, sys_color(15));
                rc[0] = 4; rc[1] = 2; rc[2] = x->cw - 18; rc[3] = x->ch - 2;
                d->bkmode = 1; d->textcol = 0;
                if (l->sel >= 0 && l->sel < l->n) gdi_drawtext(d, l->item[l->sel], wlen16(l->item[l->sel]), rc, 0x820);
                { static wchar16 arrow[2] = { 'v', 0 }; rc[0] = x->cw - 13; rc[2] = x->cw - 2; gdi_drawtext(d, arrow, 1, rc, 0x820); }
            }
            qw_call(w, qw_thunk(w, "user32", "EndPaint"), 2, x->h, ps, 0, 0);
            qw_free(w, ps);
            return 0; }
        }
    }
    switch (msg) {
    case WM_PAINT: {
        uint32_t ps = qw_alloc(w, 64), hdc;
        qdc *d;
        int i, fh = gdi_font_height(0);
        hdc = qw_call(w, qw_thunk(w, "user32", "BeginPaint"), 2, x->h, ps, 0, 0);
        d = DCP(w, hdc);
        if (d) {
            gdi_fill(d, 0, 0, x->cw, x->ch, 0xFFFFFF);
            for (i = 0; i < l->n; i++) {
                int rc[4]; rc[0] = 2; rc[1] = i * fh; rc[2] = x->cw; rc[3] = rc[1] + fh;
                if (i == l->sel) { gdi_fill(d, 0, rc[1], x->cw, rc[3], sys_color(13)); d->textcol = 0xFFFFFF; } else d->textcol = 0;
                d->bkmode = 1;
                gdi_drawtext(d, l->item[i], wlen16(l->item[i]), rc, 0x820);
            }
        }
        qw_call(w, qw_thunk(w, "user32", "EndPaint"), 2, x->h, ps, 0, 0);
        qw_free(w, ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case 0x0180: case 0x0181: {                            /* LB_ADDSTRING / LB_INSERTSTRING */
        uint32_t s = msg == 0x0180 ? a[3] : a[3];
        int at = msg == 0x0181 && (int32_t)a[2] >= 0 && (int32_t)a[2] < l->n ? (int)a[2] : l->n, i;
        if (l->n >= 64) return 0xFFFFFFFFu;
        for (i = l->n; i > at; i--) { memcpy(l->item[i], l->item[i - 1], sizeof(l->item[0])); l->data[i] = l->data[i - 1]; }
        qw_rd_wstr(w, s, l->item[at], 64); l->data[at] = 0; l->n++;
        win_invalidate(w, x, 1);
        return (uint32_t)at; }
    case 0x0182: {                                         /* LB_DELETESTRING */
        int i = (int)a[2];
        if (i < 0 || i >= l->n) return 0xFFFFFFFFu;
        for (; i < l->n - 1; i++) { memcpy(l->item[i], l->item[i + 1], sizeof(l->item[0])); l->data[i] = l->data[i + 1]; }
        l->n--; if (l->sel >= l->n) l->sel = l->n - 1;
        win_invalidate(w, x, 1);
        return (uint32_t)l->n; }
    case 0x0184: l->n = 0; l->sel = -1; win_invalidate(w, x, 1); return 0;           /* LB_RESETCONTENT */
    case 0x0186: l->sel = (int32_t)a[2] < l->n ? (int)(int32_t)a[2] : -1; win_invalidate(w, x, 1); return (uint32_t)l->sel;  /* LB_SETCURSEL */
    case 0x0188: return (uint32_t)l->sel;                  /* LB_GETCURSEL */
    case 0x0189: if ((int32_t)a[2] < 0 || (int)a[2] >= l->n) return 0xFFFFFFFFu; return (uint32_t)qw_wr_wstr(w, a[3], l->item[a[2]], 64);  /* LB_GETTEXT */
    case 0x018A: return (int32_t)a[2] >= 0 && (int)a[2] < l->n ? (uint32_t)wlen16(l->item[a[2]]) : 0xFFFFFFFFu;
    case 0x018B: return (uint32_t)l->n;                   /* LB_GETCOUNT */
    case 0x018F: case 0x018C: case 0x01A2: {               /* LB_FINDSTRING, LB_SELECTSTRING, LB_FINDSTRINGEXACT */
        wchar16 t[64];
        int k, L, from = (int32_t)a[2] < 0 ? 0 : (int)a[2] + 1;
        L = qw_rd_wstr(w, a[3], t, 64);
        for (k = 0; k < l->n; k++) {
            int at = (from + k) % l->n, c, same = 1;
            if (msg == 0x01A2 && wlen16(l->item[at]) != L) continue;
            for (c = 0; c < L && same; c++) same = toupper(l->item[at][c] & 255) == toupper(t[c] & 255) && l->item[at][c];
            if (!same) continue;
            if (msg == 0x018C) { l->sel = at; win_invalidate(w, x, 1); }
            return (uint32_t)at;
        }
        return 0xFFFFFFFFu; }
    case 0x0199: return (int)a[2] < l->n ? l->data[a[2]] : 0xFFFFFFFFu;
    case 0x019A: if ((int)a[2] < l->n) l->data[a[2]] = a[3]; return 0;
    case WM_LBUTTONDOWN: {
        int y = (int16_t)(a[3] >> 16), i = y / gdi_font_height(0);
        if (i < l->n) { l->sel = i; win_invalidate(w, x, 1); notify_parent(w, x, 1); }
        return 0; }
    }
    return def_window_proc(w, a[0], msg, a[2], a[3]);
}

static uint32_t scrollbar_proc(qwin *w, uint32_t *a)
{
    qwnd *x = W(w, a[0]);
    if (!x) return 0;
    if (a[1] == WM_PAINT) {
        uint32_t ps = qw_alloc(w, 64), hdc = qw_call(w, qw_thunk(w, "user32", "BeginPaint"), 2, x->h, ps, 0, 0);
        qdc *d = DCP(w, hdc);
        if (d) gdi_fill(d, 0, 0, x->cw, x->ch, 0xE0E0E0);
        qw_call(w, qw_thunk(w, "user32", "EndPaint"), 2, x->h, ps, 0, 0);
        qw_free(w, ps);
        return 0;
    }
    return def_window_proc(w, a[0], a[1], a[2], a[3]);
}

/* ====================================================================== dialogs */
#define DWL_MSGRESULT 0
#define DWL_DLGPROC 4
#define DWL_USER 8

static uint32_t dlg_proc(qwin *w, uint32_t *a)
{
    qwnd *x = W(w, a[0]);
    uint32_t msg = a[1], r = 0;
    if (!x) return 0;
    if (x->sheet && sheet_proc(w, (struct qsheet *)x->sheet, a, &r)) return r;
    if (x->dlgproc) {
        r = qw_call(w, x->dlgproc, 4, a[0], msg, a[2], a[3]);
        x = W(w, a[0]);
        if (!x) return r;
        if (r) {
            if ((msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) || msg == WM_INITDIALOG || msg == 0x0087) return r;
            { uint32_t mr; memcpy(&mr, x->extra, 4); return mr ? mr : r; }   /* DWL_MSGRESULT */
        }
    }
    switch (msg) {
    case WM_ERASEBKGND: {
        uint32_t br = win_send(w, x->h, WM_CTLCOLORDLG, a[2], x->h);
        qdc *d = DCP(w, a[2]);
        if (d) gdi_fill(d, 0, 0, x->cw, x->ch, brush_rgb(w, br, sys_color(15)));
        return 1; }
    case WM_CLOSE:
        win_send(w, x->h, WM_COMMAND, 2, 0);    /* IDCANCEL */
        return 0;
    case WM_COMMAND:
        if (!x->dlgproc && (a[2] & 0xFFFF) >= 1 && (a[2] & 0xFFFF) <= 7) {       /* our message boxes */
            uint32_t id = a[2] & 0xFFFF;
            /* Escape and the close box: an OK box closes as OK, Yes/No ignores them. */
            if (id == 2 && !find_ctl(w, x, 2)) { if (find_ctl(w, x, 1)) id = 1; else return 0; }
            x->modal_done = 1; x->modal_result = (int)id;
        }
        return 0;
    case 0x0400: {                                         /* DM_GETDEFID */
        int i;
        for (i = 1; i < QW_MAXWND; i++) {
            qwnd *c = &w->wnd[i];
            if (c->used && !c->destroyed && c->parent == x->h && c->cls && c->cls->builtin == B_BUTTON && btype(c) == 1) return 0x534B0000u | (c->id & 0xFFFF);
        }
        return 0; }
    case WM_INITDIALOG: return 1;
    case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: return 0;
    }
    return def_window_proc(w, a[0], msg, a[2], a[3]);
}

typedef struct { uint32_t p; int ex; } tplr;
static uint16_t t16(qwin *w, tplr *t) { uint16_t v = x86_rd16(C, t->p); t->p += 2; return v; }
static uint32_t t32(qwin *w, tplr *t) { uint32_t v = RD32(t->p); t->p += 4; return v; }
static void talign(tplr *t, uint32_t base) { t->p = base + (((t->p - base) + 3u) & ~3u); }
/* sz_Or_Ord: returns ordinal (>0) or 0 and fills str */
static uint32_t tsz(qwin *w, tplr *t, wchar16 *s, int max)
{
    uint16_t c = x86_rd16(C, t->p);
    int i = 0;
    s[0] = 0;
    if (c == 0xFFFF) { t->p += 2; return t16(w, t); }
    for (;;) { c = t16(w, t); if (!c) break; if (i < max - 1) s[i++] = c; }
    s[i] = 0;
    return 0;
}

static uint32_t create_dialog(qwin *w, uint32_t tpl, uint32_t parent, uint32_t dlgproc, uint32_t param, uint32_t hinst)
{
    tplr t;
    uint32_t style, exstyle, n, i, menu = 0, h, focusctl = 0, font = 0;
    int16_t x, y, cx, cy;
    wchar16 menuname[64], clsname[64], title[256], face[64];
    uint32_t menuord, clsord;
    qclass *cls;
    qwnd *dw;
    t.p = tpl;
    t.ex = x86_rd16(C, tpl) == 1 && x86_rd16(C, tpl + 2) == 0xFFFF;
    if (t.ex) { t.p += 8; exstyle = t32(w, &t); style = t32(w, &t); }
    else { style = t32(w, &t); exstyle = t32(w, &t); }
    n = t16(w, &t);
    x = (int16_t)t16(w, &t); y = (int16_t)t16(w, &t); cx = (int16_t)t16(w, &t); cy = (int16_t)t16(w, &t);
    if (g_dlg_child) {
        style = (style & ~(WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VISIBLE | 0x80u)) | WS_CHILD;
        g_dlg_child = 0;
        x = y = 0;
    }
    menuord = tsz(w, &t, menuname, 64);
    clsord = tsz(w, &t, clsname, 64);
    tsz(w, &t, title, 256);
    if (style & 0x40) {                                  /* DS_SETFONT */
        uint16_t pt = t16(w, &t), weight = 400;
        if (t.ex) { weight = t16(w, &t); t.p += 2; }
        tsz(w, &t, face, 64);
        font = gdi_new(w, GO_FONT);
        { qobj *o = O(w, font, GO_FONT); if (o) { o->font = pt >= 12 ? 2 : 0; o->width = weight; } }
    }
    if (menuord || menuname[0]) {
        uint32_t d, s;
        int ok = menuord ? res_find_id(w, 4, (int)menuord, &d, &s) : 0;
        if (!ok && menuname[0]) { uint32_t g = qw_alloc(w, 130); qw_wr_wstr(w, g, menuname, 64); ok = res_find(w, 4, g, &d, &s); qw_free(w, g); }
        if (ok) menu = menu_load(w, d);
    }
    cls = clsord ? NULL : (clsname[0] ? find_class(w, 0, clsname) : NULL);
    if (!cls) { static wchar16 dn[8]; a2w(dn, "#32770", 8); cls = find_class(w, 0, dn); }
    {
        int px = x * BASEX / 4, py = y * BASEY / 8, pw = cx * BASEX / 4, ph = cy * BASEY / 8;
        qwnd tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.style = style; tmp.exstyle = exstyle; tmp.menu = menu;
        if (!(style & WS_CHILD)) {
            nc_metrics(w, &tmp);
            /* dialog template size is the client size; position relative to owner (or centred) */
            if (style & 0x0800 || 1) {                    /* DS_CENTER or default placement */
                qwnd *ow = W(w, parent);
                ow = ow ? win_top(w, ow) : NULL;
                if (ow) { px = ow->x + (ow->cw - pw) / 2; py = ow->y + (ow->ch - ph) / 2; }
                else { px = 40 + px; py = 40 + py; }
                if (px < 0) px = 0;
                if (py < tmp.nt) py = tmp.nt;
            }
            h = create_window(w, exstyle, cls, title, style & ~WS_VISIBLE, px - tmp.nl, py - tmp.nt, pw + tmp.nl + tmp.nr, ph + tmp.nt + tmp.nb, parent, menu, hinst, param);
        } else h = create_window(w, exstyle, cls, title, style & ~WS_VISIBLE, px, py, pw, ph, parent, 0, hinst, param);
    }
    dw = W(w, h);
    if (!dw) return 0;
    dw->is_dialog = 1;
    dw->dlgproc = dlgproc;
    dw->font = font;
    if (!(style & WS_CHILD)) dw->owner = parent;
    /* controls */
    for (i = 0; i < n; i++) {
        uint32_t cstyle, cex, id, cord, tord, extra;
        int16_t ix, iy, icx, icy;
        wchar16 cname[64], ctext[256];
        qclass *cc = NULL;
        uint32_t ch;
        talign(&t, tpl);
        if (t.ex) { t.p += 4; cex = t32(w, &t); cstyle = t32(w, &t); }
        else { cstyle = t32(w, &t); cex = t32(w, &t); }
        ix = (int16_t)t16(w, &t); iy = (int16_t)t16(w, &t); icx = (int16_t)t16(w, &t); icy = (int16_t)t16(w, &t);
        id = t.ex ? t32(w, &t) : t16(w, &t);
        cord = tsz(w, &t, cname, 64);
        tord = tsz(w, &t, ctext, 256);
        extra = t16(w, &t);
        t.p += extra;
        if (cord) {
            static const char *names[] = { "Button", "Edit", "Static", "ListBox", "ScrollBar", "ComboBox" };
            if (cord >= 0x80 && cord <= 0x85) a2w(cname, names[cord - 0x80], 64);
        }
        cc = find_class(w, 0, cname);
        if (!cc) { char an[64]; w2a(an, cname, 64); qw_log(w, "dialog control class '%s' unknown - using Static", an); { static wchar16 sn[8]; a2w(sn, "Static", 8); cc = find_class(w, 0, sn); } }
        if (tord) ctext[0] = 0;
        ch = create_window(w, cex, cc, ctext, (cstyle | WS_CHILD) & ~WS_VISIBLE, ix * BASEX / 4, iy * BASEY / 8, icx * BASEX / 4, icy * BASEY / 8, h, id, hinst, 0);
        if (ch) {
            qwnd *cw = W(w, ch);
            if (cw) { cw->font = font; cw->visible = (cstyle & WS_VISIBLE) != 0; if (cw->visible) cw->style |= WS_VISIBLE; }
            if (!focusctl && (cstyle & WS_TABSTOP) && !(cstyle & WS_DISABLED)) focusctl = ch;
        }
    }
    dw = W(w, h);
    if (!dw) return 0;
    if (font) win_send(w, h, WM_SETFONT, font, 0);
    if (qw_call(w, dw->wndproc, 4, h, WM_INITDIALOG, focusctl, param) && focusctl) set_focus(w, focusctl);
    dw = W(w, h);
    if (!dw) return 0;
    if (style & WS_VISIBLE) { dw->visible = 1; dw->style |= WS_VISIBLE; show_top(w, dw); win_invalidate(w, dw, 1); }
    return h;
}

static uint32_t dialog_template(qwin *w, uint32_t hmod, uint32_t name)
{
    uint32_t d, s;
    if (!res_find_in(w, hmod ? hmod : w->image_base, 5, name, &d, &s)) { qw_log(w, "dialog resource %u not found", name); return 0; }
    return d;
}

/* ---------------------------------------------------------------- keyboard in dialogs */
#define DLG_MAXCTL 128
static int dlg_controls(qwin *w, uint32_t hdlg, qwnd **out)
{
    int i, n = 0;
    for (i = 1; i < QW_MAXWND && n < DLG_MAXCTL; i++) {
        qwnd *c = &w->wnd[i];
        if (c->used && !c->destroyed && c->parent == hdlg && (c->style & WS_CHILD)) out[n++] = c;
    }
    return n;
}
static int dlg_can_focus(qwnd *c)
{
    if (!c->visible || !c->enabled) return 0;
    if (c->cls && c->cls->builtin == B_STATIC) return 0;
    if (c->cls && c->cls->builtin == B_BUTTON && btype(c) == 7) return 0;
    return 1;
}
/* The dialog's own child that holds h, or -1. */
static int dlg_index(qwin *w, uint32_t hdlg, uint32_t h, qwnd **list, int n)
{
    qwnd *c = W(w, h);
    int i;
    while (c && c->parent != hdlg && (c->style & WS_CHILD)) c = W(w, c->parent);
    for (i = 0; c && i < n; i++) if (list[i] == c) return i;
    return -1;
}
static uint32_t dlg_next_tab(qwin *w, uint32_t hdlg, uint32_t from, int back)
{
    qwnd *list[DLG_MAXCTL];
    int n = dlg_controls(w, hdlg, list), at = dlg_index(w, hdlg, from, list, n), k;
    if (!n) return 0;
    if (at < 0) at = back ? 0 : n - 1;
    for (k = 1; k <= n; k++) {
        qwnd *c = list[(at + (back ? n - k : k)) % n];
        if (dlg_can_focus(c) && (c->style & WS_TABSTOP)) return c->h;
    }
    return 0;
}
static uint32_t dlg_next_group(qwin *w, uint32_t hdlg, uint32_t from, int back)
{
    qwnd *list[DLG_MAXCTL];
    int n = dlg_controls(w, hdlg, list), at = dlg_index(w, hdlg, from, list, n), first, last, k, size;
    if (at < 0) return 0;
    for (first = at; first > 0 && !(list[first]->style & WS_GROUP); first--) {}
    for (last = at + 1; last < n && !(list[last]->style & WS_GROUP); last++) {}
    size = last - first;
    for (k = 1; k < size; k++) {
        qwnd *c = list[first + (at - first + (back ? size - k : k)) % size];
        if (dlg_can_focus(c)) return c->h;
    }
    return 0;
}
static void dlg_command(qwin *w, uint32_t hdlg, uint32_t id)
{
    qwnd *d = W(w, hdlg), *c = d ? find_ctl(w, d, id) : NULL;
    win_send(w, hdlg, WM_COMMAND, id & 0xFFFF, c ? c->h : 0);
}
/* The & letter of a control's text; 0 when it has none. */
static int dlg_mnemonic(const wchar16 *t)
{
    int i;
    for (i = 0; t[i]; i++) if (t[i] == '&') { if (t[i + 1] != '&') return t[i + 1]; i++; }
    return 0;
}
static int dlg_char(qwin *w, uint32_t hdlg, uint32_t wp, uint32_t code)
{
    qwnd *list[DLG_MAXCTL];
    uint32_t wanted;
    int n, i;
    if ((code & 0x80) || wp < 33 || wp > 255) return 0;
    wanted = (uint32_t)toupper((int)wp);
    n = dlg_controls(w, hdlg, list);
    for (i = 0; i < n; i++) {
        int m = dlg_mnemonic(list[i]->text), k;
        if (!m || (uint32_t)toupper(m) != wanted) continue;
        /* A label names the control after it. */
        for (k = i; k < n && !dlg_can_focus(list[k]); k++) {}
        if (k >= n) return 0;
        set_focus(w, list[k]->h);
        if (list[k]->cls && list[k]->cls->builtin == B_BUTTON) button_activate(w, list[k]);
        return 1;
    }
    return 0;
}
static int dlg_key(qwin *w, uint32_t hdlg, uint32_t hwnd, uint32_t msg, uint32_t wp)
{
    uint32_t code, next;
    qwnd *c;
    if (msg != WM_KEYDOWN && msg != WM_CHAR) return 0;
    c = W(w, hdlg);
    if (c && c->sheet) return sheet_key(w, c, hwnd, msg, wp);
    code = W(w, hwnd) && hwnd != hdlg ? win_send(w, hwnd, WM_GETDLGCODE, wp, 0) : 0;
    if (code & 4) return 0;                                /* wants everything */
    if (msg == WM_CHAR) return dlg_char(w, hdlg, wp, code);
    switch (wp) {
    case 0x09:                                             /* Tab */
        if (code & 2) return 0;
        next = dlg_next_tab(w, hdlg, hwnd, (w->keystate[0x10] & 0x80) != 0);
        if (next) set_focus(w, next);
        return 1;
    case 0x25: case 0x26: case 0x27: case 0x28:            /* arrows: inside the group */
        if (code & 1) return 0;
        next = dlg_next_group(w, hdlg, hwnd, wp == 0x25 || wp == 0x26);
        if (next) {
            set_focus(w, next);
            c = W(w, next);
            if (c && c->cls && c->cls->builtin == B_BUTTON && btype(c) == 9) button_activate(w, c);
        }
        return 1;
    case 0x0D: {                                           /* Return: the focused push button, or the default one */
        uint32_t id = 1, def;
        c = W(w, hwnd);
        if (c && hwnd != hdlg && c->cls && c->cls->builtin == B_BUTTON && btype(c) <= 1) id = c->id;
        else if (((def = win_send(w, hdlg, 0x0400, 0, 0)) >> 16) == 0x534B) id = def & 0xFFFF;
        { qwnd *d = W(w, hdlg), *t = d ? find_ctl(w, d, id) : NULL; if (t && !t->enabled) return 1; }
        dlg_command(w, hdlg, id);
        return 1; }
    case 0x1B:                                             /* Escape */
        dlg_command(w, hdlg, 2);
        return 1;
    }
    return 0;
}

static int run_modal(qwin *w, uint32_t h)
{
    qwnd *x = W(w, h);
    qmsg m;
    uint32_t owner;
    int r;
    if (!x) return -1;
    owner = x->owner;
    if (W(w, owner)) W(w, owner)->enabled = 0;
    x->visible = 1; x->style |= WS_VISIBLE;
    show_top(w, x);
    win_invalidate(w, x, 1);
    w->modal_depth++;
    /* A fault in a window procedure stops all guest code: nothing could end this loop. */
    while ((x = W(w, h)) && !x->modal_done && !w->exiting && !C->fault) {
        if (!user_pump(w, 1, &m, 0)) { if (w->quit_posted) break; continue; }
        if (m.msg == WM_QUIT) { w->quit_posted = 1; break; }
        if (dlg_key(w, h, m.hwnd, m.msg, m.wp)) continue;
        {
            uint32_t mm = qw_alloc(w, 28);
            WR32(mm, m.hwnd); WR32(mm + 4, m.msg); WR32(mm + 8, m.wp); WR32(mm + 12, m.lp); WR32(mm + 16, m.time);
            qw_call(w, qw_thunk(w, "user32", "TranslateMessage"), 1, mm, 0, 0, 0);
            qw_call(w, qw_thunk(w, "user32", "DispatchMessageW"), 1, mm, 0, 0, 0);
            qw_free(w, mm);
        }
    }
    w->modal_depth--;
    x = W(w, h);
    r = x ? x->modal_result : -1;
    if (x) win_destroy(w, x);
    if (W(w, owner)) { W(w, owner)->enabled = 1; w->active = win_top(w, W(w, owner))->h; win_invalidate(w, W(w, owner), 1); }
    return r;
}

uint32_t user_message_box(qwin *w, uint32_t owner, const wchar16 *text, const wchar16 *title, uint32_t type)
{
    static const struct { int n; struct { const char *label; int id; } b[3]; } kinds[6] = {
        { 1, { { "OK", 1 } } },
        { 2, { { "OK", 1 }, { "Cancel", 2 } } },
        { 3, { { "&Abort", 3 }, { "&Retry", 4 }, { "&Ignore", 5 } } },
        { 3, { { "&Yes", 6 }, { "&No", 7 }, { "Cancel", 2 } } },
        { 2, { { "&Yes", 6 }, { "&No", 7 } } },
        { 2, { { "&Retry", 4 }, { "Cancel", 2 } } },
    };
    static wchar16 dn[8], bn[8], sn[8];
    qclass *dc, *bc, *sc;
    uint32_t h, s, focus = 0;
    int lines = 1, i, maxw = 0, cur = 0, wd, ht, kind = (int)(type & 0xF), def = (int)((type >> 8) & 0xF), n, bw;
    if (kind > 5) kind = 0;
    n = kinds[kind].n;
    if (def >= n) def = 0;
    bw = n * 76 + (n - 1) * 8;
    a2w(dn, "#32770", 8); a2w(bn, "Button", 8); a2w(sn, "Static", 8);
    dc = find_class(w, 0, dn); bc = find_class(w, 0, bn); sc = find_class(w, 0, sn);
    for (i = 0; text[i]; i++) { if (text[i] == '\n') { lines++; if (cur > maxw) maxw = cur; cur = 0; } else cur += gdi_text_width(0, &text[i], 1); }
    if (cur > maxw) maxw = cur;
    wd = maxw + 40; if (wd < 200) wd = 200; if (wd < bw + 40) wd = bw + 40;
    ht = lines * gdi_font_height(0) + 70;
    {
        qwnd *ow = W(w, owner);
        int px = ow ? win_top(w, ow)->x + 20 : 60, py = ow ? win_top(w, ow)->y + 20 : 60;
        h = create_window(w, 0, dc, title, WS_POPUP | WS_CAPTION | WS_SYSMENU, px, py, wd + 2 * w->frame_w, ht + 2 * w->frame_w + w->caption_h, owner, 0, 0, 0);
    }
    if (!h) return 0;
    W(w, h)->is_dialog = 1;
    W(w, h)->owner = owner;
    s = create_window(w, 0, sc, text, WS_CHILD | WS_VISIBLE, 20, 14, wd - 40, lines * gdi_font_height(0) + 4, h, 0xFFFF, 0, 0);
    (void)s;
    for (i = 0; i < n; i++) {
        wchar16 label[16];
        uint32_t b;
        a2w(label, kinds[kind].b[i].label, 16);
        b = create_window(w, 0, bc, label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == def ? 1u : 0u) | (i == 0 ? WS_GROUP : 0u), (wd - bw) / 2 + i * 84, ht - 36, 76, 24, h, (uint32_t)kinds[kind].b[i].id, 0, 0);
        if (i == def) focus = b;
    }
    set_focus(w, focus);
    return (uint32_t)run_modal(w, h);
}

/* ====================================================================== menus */
static uint32_t menu_parse(qwin *w, uint32_t *p, int ex, uint32_t base)
{
    uint32_t hm = menu_new(w);
    qmenu *m = M(w, hm);
    for (;;) {
        uint32_t flags, id = 0, type = 0, state = 0;
        wchar16 txt[64];
        int end, popup, i;
        if (ex) {
            *p = base + (((*p - base) + 3u) & ~3u);
            type = RD32(*p); state = RD32(*p + 4); id = RD32(*p + 8); flags = x86_rd16(C, *p + 12); *p += 14;
            i = 0; for (;;) { wchar16 ch = x86_rd16(C, *p); *p += 2; if (!ch) break; if (i < 63) txt[i++] = ch; } txt[i] = 0;
            popup = flags & 1; end = flags & 0x80;
            if (popup) { *p = base + (((*p - base) + 3u) & ~3u); *p += 4; }
            flags = (type & 0x800 ? 0x800u : 0u) | (state & 8 ? 8u : 0u) | (state & 3 ? 1u : 0u);
        } else {
            flags = x86_rd16(C, *p); *p += 2;
            popup = flags & 0x10; end = flags & 0x80;
            if (!popup) { id = x86_rd16(C, *p); *p += 2; }
            i = 0; for (;;) { wchar16 ch = x86_rd16(C, *p); *p += 2; if (!ch) break; if (i < 63) txt[i++] = ch; } txt[i] = 0;
            if (!popup && !id && !txt[0]) flags |= 0x800;             /* separator */
        }
        if (m && m->n < 48) {
            qmenuitem *it = &m->it[m->n++];
            it->id = id; it->flags = flags & ~0x90u; wcpy16(it->text, txt, 64);
            if (popup) { it->sub = menu_parse(w, p, ex, base); it->flags |= 0x10; m = M(w, hm); }
        } else if (popup) menu_parse(w, p, ex, base);
        if (end) break;
    }
    return hm;
}
uint32_t menu_load(qwin *w, uint32_t data)
{
    uint16_t ver = x86_rd16(C, data), off = x86_rd16(C, data + 2);
    uint32_t p;
    if (ver == 1) { p = data + 4 + off; return menu_parse(w, &p, 1, data); }
    p = data + 4;
    return menu_parse(w, &p, 0, data);
}
static qmenuitem *menu_find(qwin *w, uint32_t hm, uint32_t id, int bypos, uint32_t *owner)
{
    qmenu *m = M(w, hm);
    int i;
    if (!m) return NULL;
    if (bypos) { if (id < (uint32_t)m->n) { if (owner) *owner = hm; return &m->it[id]; } return NULL; }
    for (i = 0; i < m->n; i++) {
        if (!(m->it[i].flags & 0x10) && m->it[i].id == id) { if (owner) *owner = hm; return &m->it[i]; }
        if (m->it[i].sub) { qmenuitem *r = menu_find(w, m->it[i].sub, id, 0, owner); if (r) return r; m = M(w, hm); }
    }
    return NULL;
}
static void menu_changed(qwin *w, uint32_t hm)
{
    int i;
    (void)hm;
    for (i = 0; i < QW_MAXWND; i++) {
        qwnd *x = &w->wnd[i];
        if (x->used && !x->destroyed && x->menu && x->be && w->be->menu) w->be->menu(w, x);
    }
}

/* ====================================================================== menu bar + popup menus */
/* menu bar item text without '&' and the part after '\t' */
static int menu_label(const wchar16 *src, wchar16 *out, int max, wchar16 *accel, int amax)
{
    int i, o = 0, a = 0, tab = 0;
    for (i = 0; src[i]; i++) {
        if (src[i] == '\t') { tab = 1; continue; }
        if (src[i] == '&') { if (src[i + 1] == '&') { if (!tab && o < max - 1) out[o++] = '&'; i++; } continue; }
        if (tab) { if (accel && a < amax - 1) accel[a++] = src[i]; }
        else if (o < max - 1) out[o++] = src[i];
    }
    out[o] = 0;
    if (accel) accel[a] = 0;
    return o;
}

int user_menubar_height(qwin *w, qwnd *top) { return top && top->menu && M(w, top->menu) ? w->menu_bar_h : 0; }

static int menubar_item_at(qwin *w, qwnd *top, int x, int *ix)
{
    qmenu *m = M(w, top->menu);
    int i, cx = 6;
    if (!m) return -1;
    for (i = 0; i < m->n; i++) {
        wchar16 t[64];
        int n = menu_label(m->it[i].text, t, 64, NULL, 0), wd = gdi_text_width(0, t, n) + 12;
        if (x >= cx - 6 && x < cx + wd - 6) { if (ix) *ix = cx - 6; return i; }
        cx += wd;
    }
    return -1;
}

void user_render_menubar(qwin *w, qwnd *top, qsurf *s)
{
    qdc d;
    qmenu *m = M(w, top->menu);
    int i, cx = 6, y;
    memset(&d, 0, sizeof(d));
    d.w = w; d.s = s; d.cx1 = s->w; d.cy1 = s->h; d.font = 0; d.bkmode = 1;
    for (i = 0; i < s->w * s->h; i++) s->pix[i] = sys_color(4);
    for (y = 0; y < s->w; y++) s->pix[(size_t)(s->h - 1) * (size_t)s->w + (size_t)y] = 0xFFFFFF;
    if (!m) return;
    for (i = 0; i < m->n; i++) {
        wchar16 t[64];
        int n = menu_label(m->it[i].text, t, 64, NULL, 0), wd = gdi_text_width(0, t, n);
        if (w->menu_popup && w->menu_owner == top->h && w->menu_open_index == i) {
            gdi_fill(&d, cx - 6, 1, cx + wd + 6, s->h - 1, sys_color(13));
            gdi_text(&d, cx, 3, t, n, 0xFFFFFF);
        } else gdi_text(&d, cx, 3, t, n, (m->it[i].flags & 3) ? 0x808080 : 0);
        cx += wd + 12;
    }
}

#define MI_H 18
#define MI_SEP 8
static int popup_item_at(qwin *w, qmenu *m, int y)
{
    int i, cy = 2;
    for (i = 0; i < m->n; i++) {
        int h = (m->it[i].flags & 0x800) ? MI_SEP : MI_H;
        if (y >= cy && y < cy + h) return i;
        cy += h;
    }
    return -1;
}

static void close_popup(qwin *w)
{
    qwnd *p = W(w, w->menu_popup), *o = W(w, w->menu_owner);
    w->menu_popup = 0;
    w->menu_open_index = -1;
    if (p) win_destroy(w, p);
    if (o) { o->present = 1; w->active = o->h; if (w->be->menu) w->be->menu(w, o); }
}

static void open_popup(qwin *w, qwnd *owner, uint32_t hm, int index, int sx, int sy)
{
    static wchar16 cn[8];
    qmenu *m;
    qclass *c;
    int i, wd = 80, ht = 4, aw = 0;
    uint32_t h;
    if (w->menu_popup) close_popup(w);
    win_send(w, owner->h, WM_INITMENUPOPUP, hm, (uint32_t)index);
    m = M(w, hm);
    if (!m || !m->n) return;
    for (i = 0; i < m->n; i++) {
        wchar16 t[64], acc[32];
        int n = menu_label(m->it[i].text, t, 64, acc, 32), tw = gdi_text_width(0, t, n) + 40, a = gdi_text_width(0, acc, wlen16(acc));
        if (tw > wd) wd = tw;
        if (a > aw) aw = a;
        ht += (m->it[i].flags & 0x800) ? MI_SEP : MI_H;
    }
    wd += aw ? aw + 16 : 0;
    a2w(cn, "#32768", 8);
    c = find_class(w, 0, cn);
    h = create_window(w, 0, c, NULL, WS_POPUP, sx, sy, wd, ht, owner->h, 0, 0, 0);
    if (!h) return;
    W(w, h)->userdata = hm;
    W(w, h)->state = -1;
    W(w, h)->visible = 1;
    W(w, h)->style |= WS_VISIBLE;
    w->menu_popup = h;
    w->menu_owner = owner->h;
    w->menu_open_index = index;
    show_top(w, W(w, h));
    win_invalidate(w, W(w, h), 1);
    owner->present = 1;
    if (w->be->menu) w->be->menu(w, owner);
}

static void menubar_click(qwin *w, qwnd *top, int x)
{
    qmenu *m = M(w, top->menu);
    int ix = 0, i = m ? menubar_item_at(w, top, x, &ix) : -1;
    if (i < 0) { if (w->menu_popup) close_popup(w); return; }
    if (w->menu_popup && w->menu_open_index == i && w->menu_owner == top->h) { close_popup(w); return; }
    if (m->it[i].flags & 3) return;
    if (m->it[i].sub) open_popup(w, top, m->it[i].sub, i, top->x + ix, top->y);
    else { if (w->menu_popup) close_popup(w); win_post(w, top->h, WM_COMMAND, m->it[i].id & 0xFFFF, 0); }
}

static uint32_t menu_popup_proc(qwin *w, uint32_t *a)
{
    qwnd *x = W(w, a[0]);
    qmenu *m;
    if (!x) return 0;
    m = M(w, x->userdata);
    switch (a[1]) {
    case WM_PAINT: {
        uint32_t ps = qw_alloc(w, 64), hdc = qw_call(w, qw_thunk(w, "user32", "BeginPaint"), 2, x->h, ps, 0, 0);
        qdc *d = DCP(w, hdc);
        if (d && m) {
            int i, cy = 2;
            gdi_fill(d, 0, 0, x->cw, x->ch, sys_color(4));
            gdi_frame3d(d, 0, 0, x->cw, x->ch, 0, 1);
            d->bkmode = 1;
            for (i = 0; i < m->n; i++) {
                qmenuitem *it = &m->it[i];
                if (it->flags & 0x800) { gdi_fill(d, 3, cy + 3, x->cw - 3, cy + 4, 0x808080); gdi_fill(d, 3, cy + 4, x->cw - 3, cy + 5, 0xFFFFFF); cy += MI_SEP; continue; }
                {
                    wchar16 t[64], acc[32];
                    int n = menu_label(it->text, t, 64, acc, 32), al = wlen16(acc);
                    uint32_t col = (it->flags & 3) ? 0x808080 : 0;
                    if (i == x->state && !(it->flags & 3)) { gdi_fill(d, 3, cy, x->cw - 3, cy + MI_H, sys_color(13)); col = 0xFFFFFF; }
                    gdi_text(d, 20, cy + 2, t, n, col);
                    if (al) gdi_text(d, x->cw - 10 - gdi_text_width(0, acc, al), cy + 2, acc, al, col);
                    if (it->flags & 8) {               /* check mark */
                        int k;
                        for (k = 0; k < 7; k++) { int yy = k < 3 ? cy + 8 + k : cy + 10 - (k - 2); gdi_fill(d, 6 + k, yy, 7 + k, yy + 2, col); }
                    }
                    if (it->sub) { int k; for (k = 0; k < 4; k++) gdi_fill(d, x->cw - 10 + k, cy + 5 + k, x->cw - 9 + k, cy + 13 - k, col); }
                }
                cy += MI_H;
            }
        }
        qw_call(w, qw_thunk(w, "user32", "EndPaint"), 2, x->h, ps, 0, 0);
        qw_free(w, ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        int i = m ? popup_item_at(w, m, (int16_t)(a[3] >> 16)) : -1;
        if (i != x->state) { x->state = i; win_invalidate(w, x, 1); }
        return 0; }
    case WM_LBUTTONUP: case WM_RBUTTONUP: {
        int i = m ? popup_item_at(w, m, (int16_t)(a[3] >> 16)) : -1;
        uint32_t owner = w->menu_owner;
        if (i < 0 || (m->it[i].flags & 0x803) || m->it[i].sub) return 0;
        {
            uint32_t id = m->it[i].id;
            close_popup(w);
            win_post(w, owner, WM_COMMAND, id & 0xFFFF, 0);
        }
        return 0; }
    }
    return def_window_proc(w, a[0], a[1], a[2], a[3]);
}

/* ====================================================================== focus / input */
/* Windows tells a top-level window that it became active before it gets the keyboard. Games
 * rely on this: Doom95 pauses, and ignores its keyboard, until WM_ACTIVATEAPP arrives. */
static void activate_top(qwin *w, uint32_t h)
{
    qwnd *x = W(w, h);
    uint32_t old;
    while (x && (x->style & WS_CHILD) && W(w, x->parent)) x = W(w, x->parent);
    if (!x) return;
    old = w->active;
    if (old != x->h && W(w, old)) win_send(w, old, WM_ACTIVATE, 0, x->h);
    w->active = x->h;
    if (!x->app_activated) {
        x->app_activated = 1;
        win_send(w, x->h, 0x001C /* WM_ACTIVATEAPP */, 1, 0);
    }
    if (old != x->h) win_send(w, x->h, WM_ACTIVATE, 1, old);
}

static void set_focus(qwin *w, uint32_t h)
{
    uint32_t old = w->focus;
    if (W(w, h)) activate_top(w, h);
    if (old == h) return;
    w->focus = h;
    if (W(w, old)) win_send(w, old, WM_KILLFOCUS, h, 0);
    if (W(w, h)) win_send(w, h, WM_SETFOCUS, old, 0);
}

static qwnd *child_at(qwin *w, qwnd *p, int x, int y)
{
    int i;
    for (i = QW_MAXWND - 1; i > 0; i--) {
        qwnd *c = &w->wnd[i];
        if (!c->used || c->destroyed || c->parent != p->h || !(c->style & WS_CHILD) || !c->visible) continue;
        if (x >= c->x && y >= c->y && x < c->x + c->cw && y < c->y + c->ch) {
            qwnd *d = child_at(w, c, x - c->x, y - c->y);
            if (d) return d;
            if (c->cls && c->cls->builtin == B_STATIC && !(c->style & 0x100)) continue;
            if (c->cls && c->cls->builtin == B_BUTTON && btype(c) == 7) continue;
            return c;
        }
    }
    return NULL;
}

static qwnd *active_top(qwin *w)
{
    qwnd *t = W(w, w->active);
    int i;
    if (t && t->visible && t->enabled) return t;
    for (i = QW_MAXWND - 1; i > 0; i--) { qwnd *x = &w->wnd[i]; if (x->used && !x->destroyed && !(x->style & WS_CHILD) && x->visible && x->enabled) return x; }
    return t;
}

static void mouse_event(qwin *w, qwnd *top, int type, int x, int y, int button)
{
    qwnd *tgt;
    int lx, ly;
    uint32_t msg, mk = 0;
    if (!top || !top->enabled) return;
    if (w->menu_popup && top->h != w->menu_popup && type == QE_MOUSEDOWN) { close_popup(w); return; }
    w->mouse_x = top->x + x; w->mouse_y = top->y + y;
    tgt = W(w, w->capture);
    if (!tgt) { tgt = child_at(w, top, x, y); if (!tgt) tgt = top; }
    if (!tgt->enabled) return;
    {
        int ax, ay;
        win_abs_origin(w, tgt, &ax, &ay);
        lx = x - ax; ly = y - ay;
    }
    if (button == 1) mk = 1; else if (button == 2) mk = 2; else if (button == 3) mk = 0x10;
    if (type == QE_MOUSEMOVE) msg = WM_MOUSEMOVE;
    else if (type == QE_MOUSEDOWN) msg = button == 2 ? WM_RBUTTONDOWN : button == 3 ? WM_MBUTTONDOWN : WM_LBUTTONDOWN;
    else msg = button == 2 ? WM_RBUTTONUP : button == 3 ? WM_MBUTTONUP : WM_LBUTTONUP;
    if (type == QE_MOUSEUP) mk = 0;
    if (type == QE_MOUSEDOWN) w->keystate[button == 2 ? 2 : button == 3 ? 4 : 1] = 0x80;
    if (type == QE_MOUSEUP) w->keystate[button == 2 ? 2 : button == 3 ? 4 : 1] = 0;
    /* hover tracking (TrackMouseEvent TME_LEAVE) */
    {
        int i;
        for (i = 0; i < QW_MAXWND; i++) {
            qwnd *c = &w->wnd[i];
            if (c->used && !c->destroyed && (c->tme & 2) && c != tgt) { c->tme = 0; win_post(w, c->h, WM_MOUSELEAVE, 0, 0); }
        }
    }
    win_post(w, tgt->h, msg, mk, ((uint32_t)(uint16_t)ly << 16) | (uint16_t)lx);
}

static uint8_t g_vkchar[256];
/* WH_KEYBOARD hooks: Doom95 reads the keyboard only through one and leaves WM_KEYDOWN alone. */
#define QW_MAXHOOKS 4
static uint32_t g_kbhook[QW_MAXHOOKS];

static uint32_t scan_of_vk(int vk)
{
    static const char *const top = "QWERTYUIOP", *const mid = "ASDFGHJKL", *const low = "ZXCVBNM";
    const char *p;
    if (vk >= '1' && vk <= '9') return 2u + (uint32_t)(vk - '1');
    if (vk == '0') return 0x0B;
    if (vk >= 'A' && vk <= 'Z') {
        if ((p = strchr(top, vk)) != NULL) return 0x10u + (uint32_t)(p - top);
        if ((p = strchr(mid, vk)) != NULL) return 0x1Eu + (uint32_t)(p - mid);
        if ((p = strchr(low, vk)) != NULL) return 0x2Cu + (uint32_t)(p - low);
    }
    if (vk >= 0x70 && vk <= 0x79) return 0x3Bu + (uint32_t)(vk - 0x70);   /* F1-F10 */
    switch (vk) {
    case 0x08: return 0x0E; case 0x09: return 0x0F; case 0x0D: return 0x1C; case 0x10: return 0x2A;
    case 0x11: return 0x1D; case 0x12: return 0x38; case 0x1B: return 0x01; case 0x20: return 0x39;
    case 0x25: return 0x4B; case 0x26: return 0x48; case 0x27: return 0x4D; case 0x28: return 0x50;
    case 0x7A: return 0x57; case 0x7B: return 0x58;
    case 0xBC: return 0x33; case 0xBE: return 0x34; case 0xBF: return 0x35; case 0xBA: return 0x27;
    case 0xBB: return 0x0D; case 0xBD: return 0x0C; case 0xC0: return 0x29;
    case 0x21: return 0x49; case 0x22: return 0x51; case 0x23: return 0x4F; case 0x24: return 0x47;
    case 0x2D: return 0x52; case 0x2E: return 0x53;
    }
    return 0;
}

/* True when a hook swallowed the key. */
/* The scan code in lParam, with the extended bit that arrows and page keys carry. */
static uint32_t key_scan_bits(int vk)
{
    uint32_t scan = scan_of_vk(vk) << 16;
    if ((vk >= 0x21 && vk <= 0x28) || vk == 0x2D || vk == 0x2E) scan |= 0x01000000u;
    return scan;
}
static int kb_hook(qwin *w, int vk, int up, int was_down)
{
    uint32_t lp = 1u | key_scan_bits(vk) | (was_down ? 0x40000000u : 0) | (up ? 0x80000000u : 0);
    int i;
    for (i = QW_MAXHOOKS - 1; i >= 0; i--) {
        if (g_kbhook[i] && qw_call(w, g_kbhook[i], 3, 0 /* HC_ACTION */, (uint32_t)vk, lp, 0)) return 1;
    }
    return 0;
}

FN(u_SetWindowsHookEx) {
    int i;
    (void)w;
    if (a[0] == 5 /* WH_CBT */) {
        for (i = 0; i < QW_MAXCBT; i++) if (!g_cbthook[i]) { g_cbthook[i] = a[1]; return 0x4A00u + (uint32_t)i; }
        return 0;
    }
    if (a[0] != 2 /* WH_KEYBOARD */) { qw_log(w, "SetWindowsHookEx: hook type %d is not delivered", (int)a[0]); return 0x4900u + a[0]; }
    for (i = 0; i < QW_MAXHOOKS; i++) {
        if (!g_kbhook[i]) { g_kbhook[i] = a[1]; return 0x4800u + (uint32_t)i; }
    }
    return 0;
}
FN(u_UnhookWindowsHookEx) {
    uint32_t i = a[0] - 0x4800u;
    (void)w;
    if (i < QW_MAXHOOKS) g_kbhook[i] = 0;
    if (a[0] - 0x4A00u < QW_MAXCBT) g_cbthook[a[0] - 0x4A00u] = 0;
    return 1;
}

static void key_event(qwin *w, int type, int vk, int ch)
{
    int was_down = 0;
    qwnd *tgt = W(w, w->focus);
    qwnd *top = active_top(w);
    /* The key state first: a key let go while no window has the focus must not stay held. */
    if (vk > 0 && vk < 256 && type != QE_CHAR) {
        int down = type == QE_KEYDOWN;
        was_down = w->keystate[vk] != 0;
        w->keystate[vk] = down ? 0x80 : 0;
        if (was_down != down) qw_dinput_key(w, vk, down);
        if (w->verbose > 1) qw_log(w, "key %02x %s%s", vk, down ? "down" : "up", was_down && down ? " repeat" : "");
        if (ch) g_vkchar[vk] = (uint8_t)ch;
    }
    if (!tgt || win_top(w, tgt) != top) tgt = top;
    if (!tgt) return;
    /* Not a Windows function, but what people expect of it: works for every DirectDraw game. */
    if (type == QE_KEYDOWN && vk == 0x0D && w->keystate[0x12] && w->frames > 0 && !was_down) {
        qw_dd_toggle_window(w);
        w->keystate[vk] = 0;
        return;
    }
    if ((type == QE_KEYDOWN || type == QE_KEYUP) && kb_hook(w, vk, type == QE_KEYUP, was_down)) return;
    /* Engines such as SciTech MGL read the scan code in lParam, not the virtual key. */
    {
        uint32_t scan = key_scan_bits(vk);
        if (type == QE_KEYDOWN) win_post(w, tgt->h, WM_KEYDOWN, (uint32_t)vk, 1u | scan | (was_down ? 0x40000000u : 0));
        else if (type == QE_KEYUP) win_post(w, tgt->h, WM_KEYUP, (uint32_t)vk, 0xC0000001u | scan);
    }
    if (type == QE_CHAR) win_post(w, tgt->h, WM_CHAR, (uint32_t)ch, 1);
}

static qwnd *find_ctl(qwin *w, qwnd *p, uint32_t id)
{
    int i;
    for (i = 1; i < QW_MAXWND; i++) {
        qwnd *c = &w->wnd[i];
        if (c->used && !c->destroyed && (c->style & WS_CHILD) && c->id == id) {
            qwnd *q = c;
            while (q && q != p && (q->style & WS_CHILD)) q = W(w, q->parent);
            if (q == p) return c;
        }
    }
    return NULL;
}

/* ====================================================================== rendering for snapshots */
void user_render_all(qwin *w)
{
    int i;
    for (i = 1; i < QW_MAXWND; i++) {
        qwnd *x = &w->wnd[i];
        if (x->used && !x->destroyed && !(x->style & WS_CHILD) && x->visible && x->present && w->be->present) { w->be->present(w, x); x->present = 0; }
    }
}

static void put_text_surf(qsurf *s, int x, int y, const wchar16 *t, uint32_t col, int maxx, qwin *w)
{
    qdc d;
    memset(&d, 0, sizeof(d));
    d.w = w; d.s = s; d.cx0 = 0; d.cy0 = 0; d.cx1 = maxx; d.cy1 = s->h;
    d.font = 0;
    gdi_text(&d, x, y, t, wlen16(t), col);
}

int user_snapshot(qwin *w, const char *path)
{
    int i, W0 = 640, H0 = 480, x, y;
    qsurf desk;
    FILE *f;
    memset(&desk, 0, sizeof(desk));
    for (i = 1; i < QW_MAXWND; i++) {
        qwnd *t = &w->wnd[i];
        if (!t->used || t->destroyed || (t->style & WS_CHILD) || !t->visible) continue;
        if (t->x + t->cw + t->nr + 20 > W0) W0 = t->x + t->cw + t->nr + 20;
        if (t->y + t->ch + t->nb + 20 > H0) H0 = t->y + t->ch + t->nb + 20;
    }
    surf_resize(&desk, W0, H0);
    for (i = 0; i < W0 * H0; i++) desk.pix[i] = 0x3A6EA5;
    for (i = 1; i < QW_MAXWND; i++) {
        qwnd *t = &w->wnd[i];
        int fx, fy, fw, fh;
        if (!t->used || t->destroyed || (t->style & WS_CHILD) || !t->visible || !t->surf.pix) continue;
        fx = t->x - t->nl; fy = t->y - t->nt; fw = t->cw + t->nl + t->nr; fh = t->ch + t->nt + t->nb;
        for (y = fy; y < fy + fh; y++) for (x = fx; x < fx + fw; x++)
            if (x >= 0 && y >= 0 && x < W0 && y < H0) {
                uint32_t c = 0xD4D0C8;
                if (x == fx || y == fy) c = 0xFFFFFF;
                if (x == fx + fw - 1 || y == fy + fh - 1) c = 0x404040;
                desk.pix[y * W0 + x] = c;
            }
        if ((t->style & WS_CAPTION) == WS_CAPTION) {
            int cy0 = fy + w->frame_w, cy1 = cy0 + w->caption_h - 1;
            uint32_t col = (w->active == t->h) ? 0x0A246A : 0x808080;
            for (y = cy0; y < cy1; y++) for (x = fx + w->frame_w; x < fx + fw - w->frame_w; x++) if (x >= 0 && y >= 0 && x < W0 && y < H0) desk.pix[y * W0 + x] = col;
            put_text_surf(&desk, fx + w->frame_w + 4, cy0 + 2, t->text, 0xFFFFFF, fx + fw - w->frame_w, w);
        }
        if (user_menubar_height(w, t)) {
            qsurf mb;
            int mh = user_menubar_height(w, t);
            memset(&mb, 0, sizeof(mb));
            surf_resize(&mb, t->cw, mh);
            user_render_menubar(w, t, &mb);
            for (y = 0; y < mh; y++) for (x = 0; x < t->cw; x++) {
                int dx = t->x + x, dy = t->y - mh + y;
                if (dx >= 0 && dy >= 0 && dx < W0 && dy < H0) desk.pix[dy * W0 + dx] = mb.pix[y * t->cw + x];
            }
            free(mb.pix);
        }
        for (y = 0; y < t->ch; y++) for (x = 0; x < t->cw; x++) {
            int dx = t->x + x, dy = t->y + y;
            if (dx >= 0 && dy >= 0 && dx < W0 && dy < H0 && x < t->surf.w && y < t->surf.h) desk.pix[dy * W0 + dx] = t->surf.pix[y * t->surf.w + x];
        }
    }
    f = fopen(path, "wb");
    if (!f) { free(desk.pix); return 0; }
    fprintf(f, "P6\n%d %d\n255\n", W0, H0);
    for (i = 0; i < W0 * H0; i++) { uint32_t c = desk.pix[i]; fputc((int)(c >> 16) & 255, f); fputc((int)(c >> 8) & 255, f); fputc((int)c & 255, f); }
    fclose(f);
    free(desk.pix);
    return 1;
}

/* ====================================================================== the pump */
static int script_event(qwin *w, qevent *ev)
{
    qwnd *top = active_top(w);
    switch (ev->type) {
    case QE_CLICKID: {
        qwnd *c = top ? find_ctl(w, top, (uint32_t)ev->id) : NULL;
        int ax, ay;
        if (!c) { qw_log(w, "script: control %d not found in the active window", ev->id); w->expect_fail++; return 1; }
        win_abs_origin(w, c, &ax, &ay);
        mouse_event(w, top, QE_MOUSEDOWN, ax + c->cw / 2, ay + c->ch / 2, 1);
        mouse_event(w, top, QE_MOUSEUP, ax + c->cw / 2, ay + c->ch / 2, 1);
        return 1; }
    case QE_SNAP:
        user_render_all(w);
        if (user_snapshot(w, ev->arg)) qw_log(w, "snapshot %s", ev->arg);
        return 1;
    case QE_TEXT: case QE_EXPECT: {
        qwnd *c = top ? (ev->id ? find_ctl(w, top, (uint32_t)ev->id) : top) : NULL;
        char t[512];
        if (!c) { qw_log(w, "script: control %d not found", ev->id); w->expect_fail++; return 1; }
        w2a(t, c->text, 512);
        if (ev->type == QE_TEXT) { printf("TEXT %d: %s\n", ev->id, t); fflush(stdout); }
        else {
            if (!strcmp(t, ev->arg)) { w->expect_ok++; printf("EXPECT %d == \"%s\": OK\n", ev->id, ev->arg); }
            else { w->expect_fail++; printf("EXPECT %d == \"%s\": FAIL (got \"%s\")\n", ev->id, ev->arg, t); }
            fflush(stdout);
        }
        return 1; }
    case QE_MENU: if (top) win_post(w, top->h, WM_COMMAND, (uint32_t)ev->id & 0xFFFF, 0); return 1;
    case QE_MENUBAR: {
        qwnd *t = ev->win ? W(w, ev->win) : top;
        if (t && w->menu_popup && t->h == w->menu_popup) t = W(w, w->menu_owner);
        if (t) menubar_click(w, t, ev->x);
        return 1; }
    case QE_CLOSE: { qwnd *t = ev->win ? W(w, ev->win) : top; if (t) win_post(w, t->h, WM_CLOSE, 0, 0); return 1; }
    case QE_MOUSEDOWN: case QE_MOUSEUP: case QE_MOUSEMOVE: {
        qwnd *t = ev->win ? W(w, ev->win) : top;
        if (t && !ev->win && ev->arg[0] == 's') { /* screen coordinates */ }
        mouse_event(w, t, ev->type, ev->x, ev->y, ev->button);
        return 1; }
    case QE_KEYDOWN: case QE_KEYUP: case QE_CHAR: key_event(w, ev->type, ev->vk, ev->ch); return 1;
    case QE_ACTIVATE: if (W(w, ev->win)) w->active = ev->win; return 1;
    case QE_QUIT: win_post(w, 0, WM_QUIT, 0, 0); w->quit_posted = 1; return 1;
    }
    return 0;
}
void user_input_event(qwin *w, qevent *ev) { script_event(w, ev); }

static qwnd *first_dirty(qwin *w, uint32_t parent, int top)
{
    int i;
    for (i = 1; i < QW_MAXWND; i++) {
        qwnd *x = &w->wnd[i];
        if (!x->used || x->destroyed) continue;
        if (top ? (x->style & WS_CHILD) != 0 : (!(x->style & WS_CHILD) || x->parent != parent)) continue;
        if (!x->visible) continue;
        if (x->dirty && is_visible(w, x)) return x;
        { qwnd *c = first_dirty(w, x->h, 0); if (c) return c; }
    }
    return NULL;
}

int user_pump(qwin *w, int block, qmsg *out, uint32_t filt)
{
    int spins = 0;
    for (;;) {
        uint64_t now;
        int i;
        qwnd *dx;
        if (w->exiting) { memset(out, 0, sizeof(*out)); out->msg = WM_QUIT; return 1; }
        /* posted messages */
        for (i = w->qh; i != w->qt; i = (i + 1) % QW_MAXQ) {
            qmsg *m = &w->q[i];
            if (filt && m->hwnd != filt) continue;
            *out = *m;
            /* remove i */
            {
                int j = i;
                while (j != w->qh) { int p = (j - 1 + QW_MAXQ) % QW_MAXQ; w->q[j] = w->q[p]; j = p; }
                w->qh = (w->qh + 1) % QW_MAXQ;
            }
            return 1;
        }
        if (w->quit_posted) { memset(out, 0, sizeof(*out)); out->msg = WM_QUIT; out->wp = (uint32_t)w->quit_code; return 1; }
        /* present finished frames before serving timers: on a slow host a timer may be
         * due again as soon as painting ends, which would otherwise starve the display */
        if (!first_dirty(w, 0, 1)) user_render_all(w);
        /* timers */
        now = w->be->now(w);
        for (i = 0; i < QW_MAXTIMER; i++) {
            qtimer *t = &w->timer[i];
            if (t->used && now >= t->next && (!filt || t->hwnd == filt)) {
                t->next = now + (t->ms ? t->ms : 1);
                memset(out, 0, sizeof(*out));
                out->hwnd = t->hwnd; out->msg = WM_TIMER; out->wp = t->id; out->lp = t->proc; out->time = (uint32_t)now;
                return 1;
            }
        }
        /* paint */
        dx = first_dirty(w, 0, 1);
        if (dx) { memset(out, 0, sizeof(*out)); out->hwnd = dx->h; out->msg = WM_PAINT; return 1; }
        user_render_all(w);
        if (!block) {
            /* PeekMessage: poll the backend once so input (and test scripts) reach
             * programs that never block (game loops) */
            qevent ev;
            int r;
            memset(&ev, 0, sizeof(ev));
            r = spins < 8 ? qw_be_wait(w, 0, &ev) : 0;
            if (r > 0) { spins++; script_event(w, &ev); continue; }
            if (r < 0) {
                qwnd *t = active_top(w);
                if (++spins > 3 || !t) { w->exiting = 1; continue; }
                win_post(w, t->h, WM_CLOSE, 0, 0);
                continue;
            }
            return 0;
        }
        /* wait for input or the next timer */
        {
            uint64_t next = now + 1000000u;
            qevent ev;
            int r;
            for (i = 0; i < QW_MAXTIMER; i++) if (w->timer[i].used && w->timer[i].next < next) next = w->timer[i].next;
            memset(&ev, 0, sizeof(ev));
            if (w->verbose > 1) { static int nw; if (nw++ < 40) qw_log(w, "pump: wait %lu ms", (unsigned long)(next > now ? next - now : 0)); }
            r = qw_be_wait(w, (uint32_t)(next > now ? next - now : 0), &ev);
            if (r < 0) {
                /* backend finished (script end / window system gone): ask the program to close */
                qwnd *t = active_top(w);
                if (++spins > 3 || !t) { w->exiting = 1; w->exit_code = w->exit_code ? w->exit_code : 0; continue; }
                win_post(w, t->h, WM_CLOSE, 0, 0);
                continue;
            }
            if (r > 0) script_event(w, &ev);
        }
    }
}

/* Input the program asks for directly (DirectInput, GetAsyncKeyState) must be current even when
 * it has not pumped messages since: take what the window system has waiting. */
void qw_input_poll(qwin *w)
{
    static int busy;
    int n;
    if (busy) return;
    busy = 1;
    for (n = 0; n < 64; n++) {
        qevent ev;
        memset(&ev, 0, sizeof(ev));
        if (qw_be_wait(w, 0, &ev) <= 0) break;
        script_event(w, &ev);
    }
    busy = 0;
}

/* ====================================================================== DefWindowProc */
static uint32_t def_window_proc(qwin *w, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp)
{
    qwnd *x = W(w, h);
    if (!x) return 0;
    switch (msg) {
    case WM_NCCREATE: {
        uint32_t name = RD32(lp + 36);
        if (name && name >= 0x10000u) qw_rd_wstr(w, name, x->text, 256);
        return 1; }
    case WM_CLOSE: win_destroy(w, x); return 0;
    case WM_ERASEBKGND: {
        qdc *d = DCP(w, wp);
        uint32_t br = x->cls ? x->cls->brush : 0;
        if (d && br) gdi_fill(d, 0, 0, x->cw, x->ch, brush_rgb(w, br, sys_color(15)));
        return br ? 1 : 0; }
    case WM_PAINT: {
        uint32_t ps = qw_alloc(w, 64);
        qw_call(w, qw_thunk(w, "user32", "BeginPaint"), 2, h, ps, 0, 0);
        qw_call(w, qw_thunk(w, "user32", "EndPaint"), 2, h, ps, 0, 0);
        qw_free(w, ps);
        return 0; }
    case WM_SETTEXT:
        qw_rd_wstr(w, lp, x->text, 256);
        if (w->verbose && (x->style & WS_CHILD)) { char t[260]; w2a(t, x->text, 260); qw_log(w, "text %u: %s", x->id, t); }
        if (!(x->style & WS_CHILD) && x->be && w->be->title) w->be->title(w, x);
        return 1;
    case WM_GETTEXT: return (uint32_t)qw_wr_wstr(w, lp, x->text, (int)wp);
    case WM_GETTEXTLENGTH: return (uint32_t)wlen16(x->text);
    case WM_SETFONT: x->font = wp; if (lp) win_invalidate(w, x, 1); return 0;
    case WM_GETFONT: return x->font;
    case WM_SETICON: { uint32_t o = x->icon; x->icon = lp; return o; }
    case WM_SYSCOMMAND: if ((wp & 0xFFF0) == 0xF060) win_post(w, h, WM_CLOSE, 0, 0); return 0;
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC: {
        qdc *d = DCP(w, wp);
        if (d) { d->textcol = 0; d->bkcol = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX ? 0xFFFFFF : 0xC8D0D4; }
        return msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX ? 6u : 16u; }     /* COLOR_WINDOW+1 / COLOR_BTNFACE+1 */
    case WM_MOUSEACTIVATE: return 1;
    case WM_NCHITTEST: return 1;
    case WM_KEYDOWN: if (wp == 0x79) return 0; return 0;
    case WM_GETDLGCODE: return 0;
    case WM_WINDOWPOSCHANGED: return 0;
    }
    return 0;
}

/* ====================================================================== user32 API */
static void wnd_rect(qwin *w, qwnd *x, uint32_t out, int client)
{
    if (client) { WR32(out, 0); WR32(out + 4, 0); WR32(out + 8, (uint32_t)x->cw); WR32(out + 12, (uint32_t)x->ch); return; }
    {
        int sx, sy;
        client_screen(w, x, &sx, &sy);
        WR32(out, (uint32_t)(sx - x->nl)); WR32(out + 4, (uint32_t)(sy - x->nt));
        WR32(out + 8, (uint32_t)(sx + x->cw + x->nr)); WR32(out + 12, (uint32_t)(sy + x->ch + x->nb));
    }
}

FN(u_RegisterClassExW) {
    wchar16 n[64];
    qclass *c;
    uint32_t nm = RD32(a[0] + 40), mn = RD32(a[0] + 36);
    if (nm < 0x10000u) return 0;
    qw_rd_wstr(w, nm, n, 64);
    c = find_class(w, 0, n);
    if (!c) c = add_class(w, n);
    if (!c) return 0;
    c->style = RD32(a[0] + 4); c->wndproc = RD32(a[0] + 8); c->cbcls = RD32(a[0] + 12); c->cbwnd = RD32(a[0] + 16);
    c->hinst = RD32(a[0] + 20); c->icon = RD32(a[0] + 24); c->cursor = RD32(a[0] + 28); c->brush = RD32(a[0] + 32);
    if (mn && mn < 0x10000u) c->menuid = mn; else if (mn) qw_rd_wstr(w, mn, c->menuname, 64);
    return c->atom;
}
FN(u_RegisterClassW) {
    uint32_t ex = qw_alloc(w, 48), i, r;
    WR32(ex, 48);
    for (i = 0; i < 40; i += 4) WR32(ex + 4 + i, RD32(a[0] + i));
    WR32(ex + 44, 0);
    { uint32_t b[1]; b[0] = ex; r = u_RegisterClassExW(w, b); }
    qw_free(w, ex);
    return r;
}
FN(u_UnregisterClassW) { UNUSED; return 1; }
FN(u_GetClassInfoExW) {
    qclass *c = find_class(w, a[1], NULL);
    if (!c) return 0;
    WR32(a[2] + 4, c->style); WR32(a[2] + 8, c->wndproc); WR32(a[2] + 16, c->cbwnd); WR32(a[2] + 32, c->brush);
    return c->atom;
}
FN(u_CreateWindowExW) {
    qclass *c = find_class(w, a[1], NULL);
    wchar16 t[256];
    if (!c) { wchar16 n[64]; char an[64]; if (a[1] >= 0x10000) { qw_rd_wstr(w, a[1], n, 64); w2a(an, n, 64); } else snprintf(an, sizeof(an), "#%u", a[1]); qw_log(w, "CreateWindowEx: unknown class %s", an); return 0; }
    if (a[2] >= 0x10000u) qw_rd_wstr(w, a[2], t, 256); else t[0] = 0;
    return create_window(w, a[0], c, t, a[3], (int32_t)a[4], (int32_t)a[5], (int32_t)a[6], (int32_t)a[7], a[8], a[9], a[10], a[11]);
}
/* (hwnd, POINT by value in client coordinates): a direct child there, else the window itself. */
FN(u_ChildWindowFromPoint) {
    qwnd *p = W(w, a[0]), *c;
    int x = (int32_t)a[1], y = (int32_t)a[2], i;
    if (!p || x < 0 || y < 0 || x >= p->cw || y >= p->ch) return 0;
    for (i = QW_MAXWND - 1; i > 0; i--) {
        c = &w->wnd[i];
        if (c->used && !c->destroyed && c->parent == p->h && (c->style & WS_CHILD) && x >= c->x && y >= c->y && x < c->x + c->cw && y < c->y + c->ch) return c->h;
    }
    return p->h;
}
FN(u_DestroyWindow) { qwnd *x = W(w, a[0]); if (!x) return 0; win_destroy(w, x); return 1; }
FN(u_ShowWindow) {
    qwnd *x = W(w, a[0]);
    int was;
    uint32_t old_active = w->active;
    if (!x) return 0;
    was = x->visible;
    x->visible = a[1] != 0;
    if (x->visible) { x->style |= WS_VISIBLE; show_top(w, x); win_invalidate(w, x, 1); }
    else {
        x->style &= ~WS_VISIBLE;
        if (x->style & WS_CHILD) { qwnd *p = W(w, x->parent); if (p) win_invalidate(w, p, 1); }
        else if (x->be && w->be->destroy) { w->be->destroy(w, x); x->be = NULL; }
    }
    if (was != x->visible) win_send(w, x->h, WM_SHOWWINDOW, (uint32_t)x->visible, 0);
    /* Showing a top-level window activates it, except SW_SHOWMINNOACTIVE, SW_SHOWNA and
     * SW_SHOWNOACTIVATE. Freddi Fish runs its game only after WM_ACTIVATEAPP and WM_SETFOCUS. */
    if (W(w, a[0]) && x->visible && !(x->style & WS_CHILD) && a[1] != 4 && a[1] != 7 && a[1] != 8) {
        qwnd *f = W(w, w->focus);
        while (f && (f->style & WS_CHILD) && W(w, f->parent)) f = W(w, f->parent);
        w->active = old_active;
        if (f == x) activate_top(w, x->h);             /* a control in it has the focus already */
        else set_focus(w, x->h);
    }
    return (uint32_t)was;
}
FN(u_UpdateWindow) {
    qwnd *x = W(w, a[0]);
    if (x && x->dirty && is_visible(w, x)) win_send(w, x->h, WM_PAINT, 0, 0);
    return 1;
}
FN(u_DefWindowProcW) { return def_window_proc(w, a[0], a[1], a[2], a[3]); }
FN(u_DefDlgProcW) { return dlg_proc(w, a); }
FN(u_CallWindowProcW) { return qw_call(w, a[0], 4, a[1], a[2], a[3], a[4]); }
FN(u_SendMessageW) { return win_send(w, a[0], a[1], a[2], a[3]); }
FN(u_PostMessageW) { win_post(w, a[0], a[1], a[2], a[3]); return 1; }
FN(u_PostQuitMessage) { w->quit_posted = 1; w->quit_code = (int)a[0]; return 0; }
static void put_msg(qwin *w, uint32_t p, qmsg *m)
{
    WR32(p, m->hwnd); WR32(p + 4, m->msg); WR32(p + 8, m->wp); WR32(p + 12, m->lp); WR32(p + 16, m->time);
    WR32(p + 20, (uint32_t)m->x); WR32(p + 24, (uint32_t)m->y);
}
FN(u_GetMessageW) {
    qmsg m;
    user_pump(w, 1, &m, a[1]);
    put_msg(w, a[0], &m);
    if (m.msg == WM_QUIT) { w->quit_posted = 0; return 0; }
    return 1;
}
FN(u_PeekMessageW) {
    qmsg m;
    int qh = w->qh, qt = w->qt;
    if (!user_pump(w, 0, &m, a[1])) return 0;
    put_msg(w, a[0], &m);
    if (!(a[4] & 1)) {   /* PM_NOREMOVE: put posted messages back */
        if (m.msg != WM_PAINT && m.msg != WM_TIMER && m.msg != WM_QUIT) { w->qh = qh; w->qt = qt; }
    }
    if (m.msg == WM_QUIT && (a[4] & 1)) w->quit_posted = 0;
    return 1;
}
FN(u_WaitMessage) { qmsg m; if (!user_pump(w, 1, &m, 0)) return 1; if (m.msg != WM_PAINT && m.msg != WM_TIMER) { int p = (w->qh - 1 + QW_MAXQ) % QW_MAXQ; w->q[p] = m; w->qh = p; } return 1; }
FN(u_TranslateMessage) {
    uint32_t msg, vk;
    if (!a[0]) return 0;
    msg = RD32(a[0] + 4); vk = RD32(a[0] + 8);
    if (msg == WM_KEYDOWN && vk < 256 && g_vkchar[vk]) { win_post(w, RD32(a[0]), WM_CHAR, g_vkchar[vk], RD32(a[0] + 12)); return 1; }
    return 0;
}
FN(u_DispatchMessageW) {
    uint32_t h = RD32(a[0]), msg = RD32(a[0] + 4), wp = RD32(a[0] + 8), lp = RD32(a[0] + 12);
    qwnd *x = W(w, h);
    if (msg == WM_TIMER && lp) return qw_call(w, lp, 4, h, msg, wp, (uint32_t)w->be->now(w));
    if (!x) return 0;
    if (msg == WM_PAINT) {
        uint32_t r = win_send(w, h, msg, wp, lp);
        x = W(w, h);
        if (x) { x->dirty = 0; x->erase = 0; }
        return r;
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) {
        qwnd *t = win_top(w, x);
        if (t && t->h != w->active && t->enabled) { w->active = t->h; }
    }
    return win_send(w, h, msg, wp, lp);
}
/* As on Windows it handles every message for the dialog, the ordinary ones by dispatching them. */
FN(u_IsDialogMessageW) {
    qwnd *d = W(w, a[0]), *c;
    uint32_t hwnd;
    if (!d || !d->is_dialog || !a[1]) return 0;
    hwnd = RD32(a[1]);
    c = W(w, hwnd);
    while (c && c != d && (c->style & WS_CHILD)) c = W(w, c->parent);
    if (c != d) return 0;
    if (dlg_key(w, a[0], hwnd, RD32(a[1] + 4), RD32(a[1] + 8))) return 1;
    u_TranslateMessage(w, a + 1);
    u_DispatchMessageW(w, a + 1);
    return 1;
}
FN(u_BeginPaint) {
    qwnd *x = W(w, a[0]);
    uint32_t hdc;
    int i;
    if (!x) return 0;
    hdc = dc_for_window(w, x, 1);
    for (i = 0; i < 64; i += 4) WR32(a[1] + (uint32_t)i, 0);
    WR32(a[1], hdc);
    draw_border(w, x);
    if (x->erase) { uint32_t r = win_send(w, x->h, WM_ERASEBKGND, hdc, 0); WR32(a[1] + 4, r ? 0 : 1); }
    x = W(w, a[0]);
    if (x) {
        WR32(a[1] + 8, 0); WR32(a[1] + 12, 0); WR32(a[1] + 16, (uint32_t)x->cw); WR32(a[1] + 20, (uint32_t)x->ch);
        x->dirty = 0; x->erase = 0;
    }
    return hdc;
}
FN(u_EndPaint) { dc_release(w, RD32(a[1])); return 1; }
FN(u_GetDC) { qwnd *x = W(w, a[0]); if (!x) x = active_top(w); if (!x) return 0; return dc_for_window(w, x, 1); }
FN(u_ReleaseDC) { dc_release(w, a[1]); return 1; }
FN(u_InvalidateRect) {
    qwnd *x = W(w, a[0]);
    if (!a[0]) { int i; for (i = 1; i < QW_MAXWND; i++) if (w->wnd[i].used && !w->wnd[i].destroyed && !(w->wnd[i].style & WS_CHILD)) win_invalidate(w, &w->wnd[i], 1); return 1; }
    if (!x) return 0;
    win_invalidate(w, x, (int)a[2]);
    return 1;
}
FN(u_ValidateRect) { qwnd *x = W(w, a[0]); if (x) x->dirty = 0; return 1; }
FN(u_RedrawWindow) {
    qwnd *x = W(w, a[0]);
    if (!x) return 0;
    if (a[3] & 0x5) win_invalidate(w, x, (a[3] & 4) != 0);
    if (a[3] & 0x100) {
        qmsg m;
        while (first_dirty(w, 0, 1)) { qwnd *d = first_dirty(w, 0, 1); win_send(w, d->h, WM_PAINT, 0, 0); if (W(w, d->h)) { d->dirty = 0; } (void)m; }
    }
    return 1;
}
FN(u_GetClientRect) { qwnd *x = W(w, a[0]); if (!x) return 0; wnd_rect(w, x, a[1], 1); return 1; }
FN(u_GetWindowRect) { qwnd *x = W(w, a[0]); if (!x) return 0; wnd_rect(w, x, a[1], 0); return 1; }
/* top-level windows: Windows clamps every size change to the WM_GETMINMAXINFO tracking limits */
static void minmax_clamp(qwin *w, qwnd *x, int *ow, int *oh)
{
    uint32_t mm = qw_alloc(w, 40);
    int mnw, mnh, mxw, mxh;
    if (!mm) return;
    WR32(mm + 8, 640); WR32(mm + 12, 480);           /* ptMaxSize */
    WR32(mm + 24, 112); WR32(mm + 28, 27);           /* ptMinTrackSize (SM_CXMINTRACK/SM_CYMINTRACK) */
    WR32(mm + 32, 4096); WR32(mm + 36, 4096);        /* ptMaxTrackSize */
    win_send(w, x->h, WM_GETMINMAXINFO, 0, mm);
    mnw = (int32_t)RD32(mm + 24); mnh = (int32_t)RD32(mm + 28); mxw = (int32_t)RD32(mm + 32); mxh = (int32_t)RD32(mm + 36);
    qw_free(w, mm);
    if (*ow < mnw) *ow = mnw;
    if (*oh < mnh) *oh = mnh;
    if (*ow > mxw) *ow = mxw;
    if (*oh > mxh) *oh = mxh;
}
FN(u_MoveWindow) {
    qwnd *x = W(w, a[0]);
    int ncw, nch, ow = (int32_t)a[3], oh = (int32_t)a[4];
    if (!x) return 0;
    nc_metrics(w, x);
    if (!(x->style & WS_CHILD)) minmax_clamp(w, x, &ow, &oh);
    ncw = ow - x->nl - x->nr; nch = oh - x->nt - x->nb;
    if (ncw < 0) ncw = 0;
    if (nch < 0) nch = 0;
    x->x = (int32_t)a[1] + x->nl; x->y = (int32_t)a[2] + x->nt;
    if (ncw != x->cw || nch != x->ch) {
        x->cw = ncw; x->ch = nch;
        if (!(x->style & WS_CHILD)) { surf_resize(&x->surf, x->cw, x->ch); if (x->be && w->be->destroy) { w->be->destroy(w, x); x->be = NULL; if (x->visible) show_top(w, x); } }
        win_send(w, x->h, WM_SIZE, 0, ((uint32_t)x->ch << 16) | (uint32_t)x->cw);
    }
    if (x->style & WS_CHILD) { qwnd *p = W(w, x->parent); if (p) win_invalidate(w, p, 1); }
    win_invalidate(w, x, 1);
    return 1;
}
FN(u_SetWindowPos) {
    qwnd *x = W(w, a[0]);
    uint32_t fl = a[6];
    uint32_t b[6];
    if (!x) return 0;
    {
        int sx, sy;
        client_screen(w, x, &sx, &sy);
        if (x->style & WS_CHILD) { sx = x->x; sy = x->y; }
        b[0] = a[0];
        b[1] = (fl & 2) ? (uint32_t)(sx - x->nl) : a[2];          /* SWP_NOMOVE */
        b[2] = (fl & 2) ? (uint32_t)(sy - x->nt) : a[3];
        b[3] = (fl & 1) ? (uint32_t)(x->cw + x->nl + x->nr) : a[4]; /* SWP_NOSIZE */
        b[4] = (fl & 1) ? (uint32_t)(x->ch + x->nt + x->nb) : a[5];
        b[5] = 1;
    }
    if ((fl & 3) != 3) u_MoveWindow(w, b);
    if (fl & 0x40) { uint32_t s[2]; s[0] = a[0]; s[1] = 5; u_ShowWindow(w, s); }
    if (fl & 0x80) { uint32_t s[2]; s[0] = a[0]; s[1] = 0; u_ShowWindow(w, s); }
    return 1;
}
FN(u_AdjustWindowRectEx) {
    qwnd tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.style = a[1]; tmp.exstyle = a[3];
    nc_metrics(w, &tmp);
    if (a[2]) tmp.nt += w->menu_bar_h;
    WR32(a[0], RD32(a[0]) - (uint32_t)tmp.nl); WR32(a[0] + 4, RD32(a[0] + 4) - (uint32_t)tmp.nt);
    WR32(a[0] + 8, RD32(a[0] + 8) + (uint32_t)tmp.nr); WR32(a[0] + 12, RD32(a[0] + 12) + (uint32_t)tmp.nb);
    return 1;
}
FN(u_AdjustWindowRect) { uint32_t b[4]; b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; b[3] = 0; return u_AdjustWindowRectEx(w, b); }
void qw_client_origin(qwin *w, uint32_t h, int *x, int *y)
{
    qwnd *t = W(w, h);
    if (t) client_screen(w, t, x, y);
}
FN(u_ClientToScreen) { qwnd *x = W(w, a[0]); int sx, sy; if (!x) return 0; client_screen(w, x, &sx, &sy); WR32(a[1], RD32(a[1]) + (uint32_t)sx); WR32(a[1] + 4, RD32(a[1] + 4) + (uint32_t)sy); return 1; }
FN(u_ScreenToClient) { qwnd *x = W(w, a[0]); int sx, sy; if (!x) return 0; client_screen(w, x, &sx, &sy); WR32(a[1], RD32(a[1]) - (uint32_t)sx); WR32(a[1] + 4, RD32(a[1] + 4) - (uint32_t)sy); return 1; }
FN(u_MapWindowPoints) {
    qwnd *f = W(w, a[0]), *t = W(w, a[1]);
    int fx = 0, fy = 0, tx = 0, ty = 0;
    uint32_t i;
    if (f) client_screen(w, f, &fx, &fy);
    if (t) client_screen(w, t, &tx, &ty);
    for (i = 0; i < a[3]; i++) { WR32(a[2] + i * 8, RD32(a[2] + i * 8) + (uint32_t)(fx - tx)); WR32(a[2] + i * 8 + 4, RD32(a[2] + i * 8 + 4) + (uint32_t)(fy - ty)); }
    return ((uint32_t)(uint16_t)(fy - ty) << 16) | (uint16_t)(fx - tx);
}
FN(u_GetSystemMetrics) {
    UNUSED;
    switch (a[0]) {
    case 0: return 1024; case 1: return 768;                 /* SM_CXSCREEN / SM_CYSCREEN */
    case 2: case 3: return 16;                               /* scroll bar */
    case 4: return (uint32_t)w->caption_h;                   /* SM_CYCAPTION */
    case 5: case 6: return 1;                                /* SM_CXBORDER */
    case 7: case 8: return (uint32_t)w->frame_w;             /* SM_CXDLGFRAME */
    case 11: case 12: return 32;                             /* SM_CXICON */
    case 13: case 14: return 32;                             /* cursor */
    case 15: return (uint32_t)w->menu_bar_h;                 /* SM_CYMENU */
    case 16: return 1024; case 17: return 740;               /* full screen client */
    case 32: case 33: return (uint32_t)w->frame_w;           /* SM_CXFRAME */
    case 45: case 46: return 2;                              /* SM_CXEDGE */
    case 49: case 50: return 16;                             /* SM_CXSMICON */
    case 61: case 62: return 1024;
    case 76: case 77: return 0; case 78: return 1024; case 79: return 768;  /* virtual screen */
    case 80: return 1;                                       /* SM_CMONITORS */
    default: return 0;
    }
}
FN(u_GetDesktopWindow) { UNUSED; return 0x0000FFF0u; }
FN(u_GetParent) { qwnd *x = W(w, a[0]); if (!x) return 0; return (x->style & WS_CHILD) ? x->parent : x->owner; }
FN(u_GetWindow) {
    qwnd *x = W(w, a[0]);
    int i;
    if (!x) return 0;
    if (a[1] == 4) return x->owner;                          /* GW_OWNER */
    if (a[1] == 5) {                                         /* GW_CHILD */
        for (i = 1; i < QW_MAXWND; i++) if (w->wnd[i].used && !w->wnd[i].destroyed && (w->wnd[i].style & WS_CHILD) && w->wnd[i].parent == x->h) return w->wnd[i].h;
        return 0;
    }
    if (a[1] == 2) {                                         /* GW_HWNDNEXT */
        for (i = (int)((x->h - WND_BASE) >> 2) + 1; i < QW_MAXWND; i++) if (w->wnd[i].used && !w->wnd[i].destroyed && w->wnd[i].parent == x->parent && (w->wnd[i].style & WS_CHILD) == (x->style & WS_CHILD)) return w->wnd[i].h;
        return 0;
    }
    return 0;
}
FN(u_IsWindow) { return W(w, a[0]) ? 1 : 0; }
FN(u_IsWindowVisible) { qwnd *x = W(w, a[0]); return x && is_visible(w, x); }
FN(u_IsWindowEnabled) { qwnd *x = W(w, a[0]); return x && x->enabled; }
FN(u_IsChild) { qwnd *c = W(w, a[1]); while (c && (c->style & WS_CHILD)) { if (c->parent == a[0]) return 1; c = W(w, c->parent); } return 0; }
FN(u_IsIconic) { UNUSED; return 0; }
FN(u_EnableWindow) {
    qwnd *x = W(w, a[0]);
    int was;
    if (!x) return 0;
    was = !x->enabled;
    x->enabled = a[1] != 0;
    if (x->enabled) x->style &= ~WS_DISABLED; else x->style |= WS_DISABLED;
    if (was == x->enabled) { win_send(w, x->h, WM_ENABLE, a[1], 0); win_invalidate(w, x, 1); }
    return (uint32_t)was;
}
FN(u_GetDlgItem) { qwnd *p = W(w, a[0]); qwnd *c = p ? find_ctl(w, p, a[1]) : NULL; return c ? c->h : 0; }
FN(u_GetDlgCtrlID) { qwnd *x = W(w, a[0]); return x ? x->id : 0; }
FN(u_SetWindowTextW) { return win_send(w, a[0], WM_SETTEXT, 0, a[1]); }
FN(u_GetWindowTextW) { return win_send(w, a[0], WM_GETTEXT, a[2], a[1]); }
FN(u_GetWindowTextLengthW) { return win_send(w, a[0], WM_GETTEXTLENGTH, 0, 0); }
FN(u_SetDlgItemTextW) { uint32_t b[1], h; b[0] = 0; (void)b; h = u_GetDlgItem(w, a); return h ? win_send(w, h, WM_SETTEXT, 0, a[2]) : 0; }
FN(u_GetDlgItemTextW) { uint32_t h = u_GetDlgItem(w, a); if (!h) { if (a[3]) x86_wr16(C, a[2], 0); return 0; } return win_send(w, h, WM_GETTEXT, a[3], a[2]); }
FN(u_SetDlgItemInt) {
    uint32_t h = u_GetDlgItem(w, a), t;
    char b[32]; wchar16 wb[32];
    if (!h) return 0;
    if (a[3]) snprintf(b, sizeof(b), "%d", (int32_t)a[2]); else snprintf(b, sizeof(b), "%u", a[2]);
    a2w(wb, b, 32);
    t = qw_alloc(w, 64); qw_wr_wstr(w, t, wb, 32);
    win_send(w, h, WM_SETTEXT, 0, t);
    qw_free(w, t);
    return 1;
}
FN(u_GetDlgItemInt) {
    qwnd *c = W(w, u_GetDlgItem(w, a));
    char b[64]; char *e;
    long v;
    if (!c) { if (a[2]) WR32(a[2], 0); return 0; }
    w2a(b, c->text, 64);
    v = strtol(b, &e, 10);
    if (a[2]) WR32(a[2], e != b ? 1u : 0u);
    return (uint32_t)v;
}
FN(u_CheckDlgButton) { uint32_t h = u_GetDlgItem(w, a); if (h) win_send(w, h, BM_SETCHECK, a[2], 0); return h != 0; }
FN(u_IsDlgButtonChecked) { uint32_t h = u_GetDlgItem(w, a); return h ? win_send(w, h, BM_GETCHECK, 0, 0) : 0; }
FN(u_CheckRadioButton) {
    uint32_t id;
    for (id = a[1]; id <= a[2]; id++) { uint32_t b[2], h; b[0] = a[0]; b[1] = id; h = u_GetDlgItem(w, b); if (h) win_send(w, h, BM_SETCHECK, id == a[3], 0); }
    return 1;
}
FN(u_SendDlgItemMessageW) { uint32_t h = u_GetDlgItem(w, a); return h ? win_send(w, h, a[2], a[3], a[4]) : 0; }
FN(u_GetClassNameW) { qwnd *x = W(w, a[0]); if (!x || !x->cls) return 0; return (uint32_t)qw_wr_wstr(w, a[1], x->cls->name, (int)a[2]); }
FN(u_EnumChildWindows) {
    uint32_t list[QW_MAXWND];
    int n = 0, i;
    for (i = 1; i < QW_MAXWND; i++) {
        qwnd *c = &w->wnd[i], *q = c;
        if (!c->used || c->destroyed || !(c->style & WS_CHILD)) continue;
        while (q && (q->style & WS_CHILD) && q->parent != a[0]) q = W(w, q->parent);
        if (q && (q->style & WS_CHILD) && q->parent == a[0]) list[n++] = c->h;
    }
    for (i = 0; i < n; i++) if (W(w, list[i]) && !qw_call(w, a[1], 2, list[i], a[2], 0, 0)) break;
    return 1;
}
FN(u_SetFocus) { uint32_t o = w->focus; set_focus(w, a[0]); return o; }
FN(u_GetFocus) { UNUSED; return w->focus; }
FN(u_SetCapture) { uint32_t o = w->capture; w->capture = a[0]; return o; }
FN(u_ReleaseCapture) { UNUSED; w->capture = 0; return 1; }
FN(u_GetCapture) { UNUSED; return w->capture; }
FN(u_GetActiveWindow) { UNUSED; return w->active; }
FN(u_SetActiveWindow) { uint32_t o = w->active; if (W(w, a[0])) w->active = a[0]; return o; }
FN(u_FindWindowW) {
    wchar16 cn[64], tn[256];
    int i;
    if (a[0] >= 0x10000) qw_rd_wstr(w, a[0], cn, 64); else cn[0] = 0;
    if (a[1]) qw_rd_wstr(w, a[1], tn, 256); else tn[0] = 0;
    for (i = 1; i < QW_MAXWND; i++) {
        qwnd *x = &w->wnd[i];
        if (!x->used || x->destroyed || (x->style & WS_CHILD)) continue;
        if (cn[0] && (!x->cls || wicmp16(x->cls->name, cn))) continue;
        if (tn[0] && wicmp16(x->text, tn)) continue;
        return x->h;
    }
    return 0;
}
FN(u_LoadStringW) {
    uint32_t d, s, id = a[1], p, i;
    if (!res_find_in(w, a[0], 6, id / 16 + 1, &d, &s)) { if (a[3]) x86_wr16(C, a[2], 0); return 0; }
    p = d;
    for (i = 0; i < (id & 15); i++) p += 2u + 2u * x86_rd16(C, p);
    {
        uint32_t n = x86_rd16(C, p), k;
        if (!a[3]) { WR32(a[2], p + 2); return n; }
        for (k = 0; k < n && k + 1 < a[3]; k++) x86_wr16(C, a[2] + k * 2, x86_rd16(C, p + 2 + k * 2));
        x86_wr16(C, a[2] + k * 2, 0);
        return k;
    }
}
FN(u_LoadIconW) { uint32_t h = gdi_new(w, GO_ICON); (void)a; return h; }
FN(u_LoadCursorW) { uint32_t h = gdi_new(w, GO_CURSOR); (void)a; return h; }
FN(u_LoadCursorFromFile) { (void)a; return gdi_new(w, GO_CURSOR); }   /* drawn as the normal pointer */
FN(u_LoadBitmapW) {
    uint32_t d, s;
    if (!a[0] || !res_find_in(w, a[0], 2, a[1], &d, &s)) { qobj *o; uint32_t h = gdi_new(w, GO_BITMAP); o = O(w, h, GO_BITMAP); if (o) surf_resize(&o->bmp, 1, 1); return h; }
    return qw_bitmap_from_dib(w, d, s);
}
FN(u_LoadImageW) {
    uint32_t b[2];
    if (a[2] == 0) { b[0] = a[0]; b[1] = a[1]; return u_LoadBitmapW(w, b); }
    return gdi_new(w, a[2] == 2 ? GO_CURSOR : GO_ICON);
}
FN(u_DestroyIcon) { UNUSED; return 1; }
FN(u_SetCursor) { UNUSED; return 0; }
FN(u_ShowCursor) { UNUSED; return 1; }
FN(u_GetCursorPos) { WR32(a[0], (uint32_t)w->mouse_x); WR32(a[0] + 4, (uint32_t)w->mouse_y); return 1; }
FN(u_MessageBoxW) {
    wchar16 t[1024], c[256];
    qw_rd_wstr(w, a[1], t, 1024);
    if (a[2]) qw_rd_wstr(w, a[2], c, 256); else a2w(c, "Error", 256);
    return user_message_box(w, a[0], t, c, a[3]);
}
FN(u_MessageBeep) { UNUSED; return 1; }
FN(u_wsprintfW) { return (uint32_t)qw_format(w, a[0], -1, 1, a[1], 1, C->r[X86_ESP] + 12); }
FN(u_wvsprintfW) { return (uint32_t)qw_format(w, a[0], -1, 1, a[1], 1, a[2]); }
FN(u_wsprintfA) { return (uint32_t)qw_format(w, a[0], -1, 0, a[1], 0, C->r[X86_ESP] + 12); }
FN(u_CharNextW) { UNUSED; return x86_rd16(C, a[0]) ? a[0] + 2 : a[0]; }
FN(u_CharUpperW) {
    if (a[0] < 0x10000u) return a[0] >= 'a' && a[0] <= 'z' ? a[0] - 32 : a[0];
    { uint32_t p; for (p = a[0]; ; p += 2) { uint16_t ch = x86_rd16(C, p); if (!ch) break; if (ch >= 'a' && ch <= 'z') x86_wr16(C, p, (uint16_t)(ch - 32)); } }
    return a[0];
}
FN(u_GetKeyState) { return w->keystate[a[0] & 0xFF] ? 0xFFFF8000u : 0; }
FN(u_GetAsyncKeyState) { qw_input_poll(w); return w->keystate[a[0] & 0xFF] ? 0xFFFF8000u : 0; }
FN(u_GetKeyboardState) { int i; qw_input_poll(w); for (i = 0; i < 256; i++) x86_wr8(C, a[0] + (uint32_t)i, w->keystate[i]); return 1; }
FN(u_GetKeyboardLayout) { UNUSED; return 0x04070407u; }
FN(u_MapVirtualKeyExW) { UNUSED; if (a[1] == 2) return g_vkchar[a[0] & 0xFF] ? g_vkchar[a[0] & 0xFF] : (a[0] >= 0x30 && a[0] <= 0x5A ? a[0] : 0); return a[0] & 0xFF; }
FN(u_ToAsciiEx) { uint32_t ch = g_vkchar[a[0] & 0xFF]; if (!ch) return 0; x86_wr16(C, a[3], (uint16_t)ch); return 1; }
FN(u_OpenClipboard) { UNUSED; return 1; }
FN(u_CloseClipboard) { UNUSED; return 1; }
FN(u_EmptyClipboard) { w->clip_fmt = 0; w->clip_data = 0; (void)a; return 1; }
FN(u_SetClipboardData) { w->clip_fmt = a[0]; w->clip_data = a[1]; return a[1]; }
FN(u_GetClipboardData) { return a[0] == w->clip_fmt ? w->clip_data : 0; }
FN(u_IsClipboardFormatAvailable) { return a[0] == w->clip_fmt && w->clip_data; }
FN(u_SetRect) { WR32(a[0], a[1]); WR32(a[0] + 4, a[2]); WR32(a[0] + 8, a[3]); WR32(a[0] + 12, a[4]); return 1; }
FN(u_SetRectEmpty) { WR32(a[0], 0); WR32(a[0] + 4, 0); WR32(a[0] + 8, 0); WR32(a[0] + 12, 0); return 1; }
FN(u_CopyRect) { int i; for (i = 0; i < 16; i += 4) WR32(a[0] + (uint32_t)i, RD32(a[1] + (uint32_t)i)); return 1; }
FN(u_OffsetRect) { WR32(a[0], RD32(a[0]) + a[1]); WR32(a[0] + 8, RD32(a[0] + 8) + a[1]); WR32(a[0] + 4, RD32(a[0] + 4) + a[2]); WR32(a[0] + 12, RD32(a[0] + 12) + a[2]); return 1; }
FN(u_InflateRect) { WR32(a[0], RD32(a[0]) - a[1]); WR32(a[0] + 8, RD32(a[0] + 8) + a[1]); WR32(a[0] + 4, RD32(a[0] + 4) - a[2]); WR32(a[0] + 12, RD32(a[0] + 12) + a[2]); return 1; }
FN(u_IsRectEmpty) { return (int32_t)RD32(a[0] + 8) <= (int32_t)RD32(a[0]) || (int32_t)RD32(a[0] + 12) <= (int32_t)RD32(a[0] + 4); }
FN(u_PtInRect) { int32_t x = (int32_t)a[1], y = (int32_t)a[2]; return x >= (int32_t)RD32(a[0]) && x < (int32_t)RD32(a[0] + 8) && y >= (int32_t)RD32(a[0] + 4) && y < (int32_t)RD32(a[0] + 12); }
FN(u_IntersectRect) {
    int32_t l = (int32_t)RD32(a[1]) > (int32_t)RD32(a[2]) ? (int32_t)RD32(a[1]) : (int32_t)RD32(a[2]);
    int32_t t = (int32_t)RD32(a[1] + 4) > (int32_t)RD32(a[2] + 4) ? (int32_t)RD32(a[1] + 4) : (int32_t)RD32(a[2] + 4);
    int32_t r = (int32_t)RD32(a[1] + 8) < (int32_t)RD32(a[2] + 8) ? (int32_t)RD32(a[1] + 8) : (int32_t)RD32(a[2] + 8);
    int32_t b = (int32_t)RD32(a[1] + 12) < (int32_t)RD32(a[2] + 12) ? (int32_t)RD32(a[1] + 12) : (int32_t)RD32(a[2] + 12);
    if (r <= l || b <= t) { l = t = r = b = 0; }
    WR32(a[0], (uint32_t)l); WR32(a[0] + 4, (uint32_t)t); WR32(a[0] + 8, (uint32_t)r); WR32(a[0] + 12, (uint32_t)b);
    return r > l && b > t;
}
FN(u_UnionRect) {
    int32_t l = (int32_t)RD32(a[1]) < (int32_t)RD32(a[2]) ? (int32_t)RD32(a[1]) : (int32_t)RD32(a[2]);
    int32_t t = (int32_t)RD32(a[1] + 4) < (int32_t)RD32(a[2] + 4) ? (int32_t)RD32(a[1] + 4) : (int32_t)RD32(a[2] + 4);
    int32_t r = (int32_t)RD32(a[1] + 8) > (int32_t)RD32(a[2] + 8) ? (int32_t)RD32(a[1] + 8) : (int32_t)RD32(a[2] + 8);
    int32_t b = (int32_t)RD32(a[1] + 12) > (int32_t)RD32(a[2] + 12) ? (int32_t)RD32(a[1] + 12) : (int32_t)RD32(a[2] + 12);
    WR32(a[0], (uint32_t)l); WR32(a[0] + 4, (uint32_t)t); WR32(a[0] + 8, (uint32_t)r); WR32(a[0] + 12, (uint32_t)b);
    return 1;
}
FN(u_EqualRect) { int i; for (i = 0; i < 16; i += 4) if (RD32(a[0] + (uint32_t)i) != RD32(a[1] + (uint32_t)i)) return 0; return 1; }
FN(u_MonitorFromRect) { UNUSED; return 0x00012340u; }
FN(u_GetMonitorInfoW) {
    WR32(a[1] + 4, 0); WR32(a[1] + 8, 0); WR32(a[1] + 12, 1024); WR32(a[1] + 16, 768);
    WR32(a[1] + 20, 0); WR32(a[1] + 24, 0); WR32(a[1] + 28, 1024); WR32(a[1] + 32, 740); WR32(a[1] + 36, 1);
    UNUSED; return 1;
}
/* LOGFONT in the ANSI or wide form: as long as the structure's size says. */
static void spi_font(qwin *w, uint32_t p, int wide)
{
    static const char face[] = "Arial";
    uint32_t i, n = wide ? 92u : 60u;
    for (i = 0; i < n; i += 4) WR32(p + i, 0);
    WR32(p, (uint32_t)-11);                                /* lfHeight */
    WR32(p + 16, 400);                                     /* lfWeight */
    for (i = 0; face[i]; i++) { if (wide) x86_wr16(C, p + 28 + i * 2, (uint8_t)face[i]); else x86_wr8(C, p + 28 + i, (uint8_t)face[i]); }
}
FN(u_SystemParametersInfoW) {
    if (a[0] == 0x29 && a[2]) {                            /* SPI_GETNONCLIENTMETRICS */
        uint32_t m = a[2], size = RD32(m);
        int wide = size >= 500;
        uint32_t lf = wide ? 92u : 60u;
        if (size != 340 && !wide) return 0;
        WR32(m + 4, 1); WR32(m + 8, 16); WR32(m + 12, 16); WR32(m + 16, 18); WR32(m + 20, 18);
        spi_font(w, m + 24, wide);
        WR32(m + 24 + lf, 12); WR32(m + 28 + lf, 15);
        spi_font(w, m + 32 + lf, wide);
        WR32(m + 32 + lf * 2, 18); WR32(m + 36 + lf * 2, 18);
        spi_font(w, m + 40 + lf * 2, wide);
        spi_font(w, m + 40 + lf * 3, wide);
        spi_font(w, m + 40 + lf * 4, wide);
        return 1;
    }
    if (a[0] == 0x30) { WR32(a[2], 0); WR32(a[2] + 4, 0); WR32(a[2] + 8, 1024); WR32(a[2] + 12, 740); return 1; }
    if (a[2] && a[1] && a[1] < 1024) { uint32_t i; for (i = 0; i < a[1]; i += 4) WR32(a[2] + i, 0); }
    UNUSED; return 0;
}
/* accelerators: table in guest memory (resource) -> handle = resource address */
FN(u_LoadAcceleratorsW) { uint32_t d, s; if (!res_find_in(w, a[0], 9, a[1], &d, &s)) return 0; return d; }
FN(u_TranslateAcceleratorW) {
    uint32_t msg = RD32(a[2] + 4), vk = RD32(a[2] + 8), p;
    if (!a[1] || (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN)) return 0;
    for (p = a[1]; ; p += 8) {
        uint16_t fl = x86_rd16(C, p), key = x86_rd16(C, p + 2), id = x86_rd16(C, p + 4);
        if ((fl & 1) && key == vk) {              /* FVIRTKEY */
            int ctrl = w->keystate[0x11] != 0, shift = w->keystate[0x10] != 0;
            if (!!(fl & 8) == ctrl && !!(fl & 4) == shift) { win_send(w, a[0], WM_COMMAND, 0x10000u | id, 0); return 1; }
        }
        if (fl & 0x80) break;
    }
    return 0;
}
FN(u_SetTimer) {
    int i;
    for (i = 0; i < QW_MAXTIMER; i++) if (w->timer[i].used && w->timer[i].hwnd == a[0] && w->timer[i].id == a[1] && a[0]) break;
    if (i == QW_MAXTIMER) for (i = 0; i < QW_MAXTIMER; i++) if (!w->timer[i].used) break;
    if (i == QW_MAXTIMER) return 0;
    w->timer[i].used = 1; w->timer[i].hwnd = a[0]; w->timer[i].id = a[0] ? a[1] : 0x100u + (uint32_t)i;
    w->timer[i].ms = a[2] < 10 ? 10 : a[2]; w->timer[i].proc = a[3];
    w->timer[i].next = w->be->now(w) + w->timer[i].ms;
    return w->timer[i].id;
}
FN(u_KillTimer) { int i; for (i = 0; i < QW_MAXTIMER; i++) if (w->timer[i].used && w->timer[i].hwnd == a[0] && w->timer[i].id == a[1]) { w->timer[i].used = 0; return 1; } return 0; }
FN(u_GetMessageTime) { UNUSED; return (uint32_t)w->be->now(w); }
FN(u_GetMessagePos) { UNUSED; return ((uint32_t)(uint16_t)w->mouse_y << 16) | (uint16_t)w->mouse_x; }
FN(u_RegisterWindowMessageW) { static uint32_t n = 0xC100; UNUSED; return n++; }
static uint32_t wlong(qwin *w, qwnd *x, int32_t idx, int set, uint32_t v)
{
    uint32_t old = 0, *f = NULL;
    switch (idx) {
    case -4: f = &x->wndproc; break;
    case -6: f = &x->hinst; break;
    case -8: f = (x->style & WS_CHILD) ? &x->parent : &x->owner; break;
    case -12: f = &x->id; break;
    case -16: f = &x->style; break;
    case -20: f = &x->exstyle; break;
    case -21: f = &x->userdata; break;
    default:
        if (x->is_dialog && idx == DWL_DLGPROC) { old = x->dlgproc; if (set) x->dlgproc = v; return old; }
        if (idx >= 0 && idx + 4 <= x->nextra) {
            memcpy(&old, x->extra + idx, 4);
            if (set) memcpy(x->extra + idx, &v, 4);
            return old;
        }
        return 0;
    }
    old = *f;
    if (set) {
        *f = v;
        if (idx == -16) { x->visible = (v & WS_VISIBLE) != 0; x->enabled = !(v & WS_DISABLED); win_invalidate(w, x, 1); }
    }
    return old;
}
FN(u_GetWindowLongW) { qwnd *x = W(w, a[0]); return x ? wlong(w, x, (int32_t)a[1], 0, 0) : 0; }
FN(u_SetWindowLongW) { qwnd *x = W(w, a[0]); return x ? wlong(w, x, (int32_t)a[1], 1, a[2]) : 0; }
FN(u_GetClassLongW) {
    qwnd *x = W(w, a[0]);
    if (!x || !x->cls) return 0;
    switch ((int32_t)a[1]) { case -10: return x->cls->brush; case -14: return x->cls->icon; case -24: return x->cls->wndproc; case -26: return x->cls->style; default: return 0; }
}
FN(u_SetClassLongW) {
    qwnd *x = W(w, a[0]);
    uint32_t o = 0;
    if (!x || !x->cls) return 0;
    if ((int32_t)a[1] == -10) { o = x->cls->brush; x->cls->brush = a[2]; }
    else if ((int32_t)a[1] == -14 || (int32_t)a[1] == -34) { o = x->cls->icon; x->cls->icon = a[2]; }
    return o;
}
FN(u_TrackMouseEvent) { qwnd *x = W(w, RD32(a[0] + 8)); if (x && (RD32(a[0] + 4) & 2)) x->tme = 2; return 1; }
FN(u_CreateDialogParamW) {
    uint32_t t = dialog_template(w, a[0], a[1]);
    return t ? create_dialog(w, t, a[2], a[3], a[4], a[0]) : 0;
}
FN(u_CreateDialogIndirectParamW) { return create_dialog(w, a[1], a[2], a[3], a[4], a[0]); }
FN(u_DialogBoxParamW) {
    uint32_t t = dialog_template(w, a[0], a[1]), h;
    if (!t) return 0xFFFFFFFFu;
    h = create_dialog(w, t, a[2], a[3], a[4], a[0]);
    if (!h) return 0xFFFFFFFFu;
    return (uint32_t)run_modal(w, h);
}
FN(u_DialogBoxIndirectParamW) { uint32_t h = create_dialog(w, a[1], a[2], a[3], a[4], a[0]); return h ? (uint32_t)run_modal(w, h) : 0xFFFFFFFFu; }
FN(u_EndDialog) { qwnd *x = W(w, a[0]); if (!x) return 0; x->modal_done = 1; x->modal_result = (int)a[1]; if (!w->modal_depth) win_destroy(w, x); return 1; }
FN(u_GetNextDlgTabItem) { return dlg_next_tab(w, a[0], a[1], a[2] != 0); }
FN(u_GetNextDlgGroupItem) { return dlg_next_group(w, a[0], a[1], a[2] != 0); }
FN(u_MapDialogRect) {
    WR32(a[1], RD32(a[1]) * BASEX / 4); WR32(a[1] + 8, RD32(a[1] + 8) * BASEX / 4);
    WR32(a[1] + 4, RD32(a[1] + 4) * BASEY / 8); WR32(a[1] + 12, RD32(a[1] + 12) * BASEY / 8);
    UNUSED; return 1;
}
FN(u_GetDialogBaseUnits) { UNUSED; return (BASEY << 16) | BASEX; }
/* menus */
FN(u_LoadMenuW) { uint32_t d, s; if (!res_find_in(w, a[0], 4, a[1], &d, &s)) return 0; return menu_load(w, d); }
FN(u_CreateMenu) { UNUSED; return menu_new(w); }
FN(u_DestroyMenu) { qmenu *m = M(w, a[0]); if (m) m->used = 0; return m != NULL; }
FN(u_GetMenu) { qwnd *x = W(w, a[0]); return x ? x->menu : 0; }
FN(u_SetMenu) {
    qwnd *x = W(w, a[0]);
    int oldnt;
    if (!x) return 0;
    oldnt = x->nt;
    x->menu = a[1];
    nc_metrics(w, x);
    x->y += x->nt - oldnt;     /* keep the client size; the window grows by the menu bar */
    menu_changed(w, a[1]);
    if (x->be && w->be->menu) w->be->menu(w, x);
    return 1;
}
FN(u_GetSubMenu) { qmenu *m = M(w, a[0]); if (!m || a[1] >= (uint32_t)m->n) return 0; return m->it[a[1]].sub; }
FN(u_GetMenuItemCount) { qmenu *m = M(w, a[0]); return m ? (uint32_t)m->n : 0xFFFFFFFFu; }
FN(u_GetMenuItemID) { qmenu *m = M(w, a[0]); if (!m || a[1] >= (uint32_t)m->n) return 0xFFFFFFFFu; return m->it[a[1]].sub ? 0xFFFFFFFFu : m->it[a[1]].id; }
FN(u_CheckMenuItem) {
    qmenuitem *it = menu_find(w, a[0], a[1], (a[2] & 0x400) != 0, NULL);
    uint32_t old;
    if (!it) return 0xFFFFFFFFu;
    old = it->flags & 8;
    if (a[2] & 8) it->flags |= 8; else it->flags &= ~8u;
    menu_changed(w, a[0]);
    return old;
}
FN(u_EnableMenuItem) {
    qmenuitem *it = menu_find(w, a[0], a[1], (a[2] & 0x400) != 0, NULL);
    uint32_t old;
    if (!it) return 0xFFFFFFFFu;
    old = it->flags & 3;
    it->flags = (it->flags & ~3u) | (a[2] & 3);
    menu_changed(w, a[0]);
    return old;
}
FN(u_CheckMenuRadioItem) {
    uint32_t id;
    for (id = a[1]; id <= a[2]; id++) { qmenuitem *it = menu_find(w, a[0], id, (a[4] & 0x400) != 0, NULL); if (it) { if (id == a[3]) it->flags |= 8; else it->flags &= ~8u; } }
    menu_changed(w, a[0]);
    return 1;
}
FN(u_GetMenuState) { qmenuitem *it = menu_find(w, a[0], a[1], (a[2] & 0x400) != 0, NULL); return it ? it->flags : 0xFFFFFFFFu; }
FN(u_AppendMenuW) {
    qmenu *m = M(w, a[0]);
    qmenuitem *it;
    if (!m || m->n >= 48) return 0;
    it = &m->it[m->n++];
    memset(it, 0, sizeof(*it));
    it->flags = a[1] & ~0x10u;
    if (a[1] & 0x10) { it->sub = a[2]; it->flags |= 0x10; } else it->id = a[2];
    if (!(a[1] & 0x800) && a[3] >= 0x10000u) qw_rd_wstr(w, a[3], it->text, 64);
    menu_changed(w, a[0]);
    return 1;
}
/* (menu, item, by position, MENUITEMINFO) */
static uint32_t insert_menu_item(qwin *w, uint32_t *a, int wide)
{
    qmenu *m = M(w, a[0]);
    uint32_t info = a[3], mask, type, state;
    qmenuitem *it;
    int at, i;
    if (!m || m->n >= 48 || !info) return 0;
    mask = RD32(info + 4); type = RD32(info + 8); state = RD32(info + 12);
    at = m->n;
    if (a[2]) { if ((int32_t)a[1] >= 0 && (int)a[1] < m->n) at = (int)a[1]; }
    else for (i = 0; i < m->n; i++) if (m->it[i].id == a[1]) { at = i; break; }
    for (i = m->n; i > at; i--) m->it[i] = m->it[i - 1];
    m->n++;
    it = &m->it[at];
    memset(it, 0, sizeof(*it));
    if (mask & 0x100u) type = 0;                           /* MIIM_FTYPE and string come separately */
    if ((mask & 0x10u) && (type & 0x800u)) it->flags |= 0x800u;   /* separator */
    if (mask & 1u) it->flags |= state & 0x0Bu;             /* disabled, grey, checked */
    if (mask & 2u) it->id = RD32(info + 16);
    if ((mask & 4u) && RD32(info + 20)) { it->sub = RD32(info + 20); it->flags |= 0x10u; }
    if (((mask & 0x40u) || ((mask & 0x10u) && !(type & 0x900u))) && RD32(info + 36) >= 0x10000u) {
        if (wide) qw_rd_wstr(w, RD32(info + 36), it->text, 64);
        else { char t[64]; qw_rd_str(w, RD32(info + 36), t, sizeof(t)); a2w(it->text, t, 64); }
    }
    menu_changed(w, a[0]);
    return 1;
}
FN(u_InsertMenuItemA) { return insert_menu_item(w, a, 0); }
FN(u_InsertMenuItemW) { return insert_menu_item(w, a, 1); }
FN(u_ModifyMenuW) {
    qmenuitem *it = menu_find(w, a[0], a[1], (a[2] & 0x400) != 0, NULL);
    if (!it) return 0;
    it->flags = a[2] & ~0x400u;
    if (a[2] & 0x10) it->sub = a[3]; else it->id = a[3];
    if (a[4] >= 0x10000u) qw_rd_wstr(w, a[4], it->text, 64);
    menu_changed(w, a[0]);
    return 1;
}
FN(u_DeleteMenu) {
    qmenu *m = M(w, a[0]);
    int i;
    uint32_t own = 0;
    qmenuitem *it = menu_find(w, a[0], a[1], (a[2] & 0x400) != 0, &own);
    if (!it) return 0;
    m = M(w, own);
    for (i = (int)(it - m->it); i < m->n - 1; i++) m->it[i] = m->it[i + 1];
    m->n--;
    menu_changed(w, a[0]);
    return 1;
}
FN(u_GetMenuStringW) { qmenuitem *it = menu_find(w, a[0], a[1], (a[4] & 0x400) != 0, NULL); if (!it) return 0; return (uint32_t)qw_wr_wstr(w, a[2], it->text, (int)a[3]); }
FN(u_DrawMenuBar) { qwnd *x = W(w, a[0]); if (x && x->be && w->be->menu) w->be->menu(w, x); return 1; }
FN(u_TrackPopupMenu) { UNUSED; qw_log(w, "TrackPopupMenu: context menus are not shown in this build"); return 0; }
FN(u_GetSystemMenu) { UNUSED; return 0; }
FN(u_one) { UNUSED; return 1; }
FN(u_zero) { UNUSED; return 0; }

/* builtin window procedures (callable from guest via CallWindowProc) */
static uint32_t h_ButtonProc(qwin *w, uint32_t *a) { return button_proc(w, a); }
static uint32_t h_StaticProc(qwin *w, uint32_t *a) { return static_proc(w, a); }
static uint32_t h_EditProc(qwin *w, uint32_t *a) { return edit_proc(w, a); }
static uint32_t h_DialogProc(qwin *w, uint32_t *a) { return dlg_proc(w, a); }
static uint32_t h_ListBoxProc(qwin *w, uint32_t *a) { return listbox_proc(w, a); }
static uint32_t h_ScrollBarProc(qwin *w, uint32_t *a) { return scrollbar_proc(w, a); }
static uint32_t h_MenuProc(qwin *w, uint32_t *a) { return menu_popup_proc(w, a); }

#define U(n, f, c) { "user32", n, c, f }
/* ====================================================================== property sheets
 * The frame is a dialog of ours, with the pages as child dialogs under it. Pages are created
 * when first shown, and everything a page decides reaches it as WM_NOTIFY. */
#define QS_MAX 32
#define QS_BACK 0x3023u
#define QS_NEXT 0x3024u
#define QS_FINISH 0x3025u
#define QS_APPLY 0x3021u
#define QS_TAB 0x3100u
enum { PSN_SETACTIVE = -200, PSN_KILLACTIVE = -201, PSN_APPLY = -202, PSN_RESET = -203, PSN_WIZBACK = -206, PSN_WIZNEXT = -207, PSN_WIZFINISH = -208, PSN_QUERYCANCEL = -209 };
typedef struct qspage { uint32_t psp, hwnd, tpl; } qspage;
struct qsheet {
    uint32_t h, flags, hinst, wizbtn;
    int n, cur, wizard, wide, modeless, pw, ph, top, result, done, saved;
    wchar16 caption[128];
    qspage page[QS_MAX];
};
typedef struct qsheet qsheet;

/* A string argument that may be a resource number. */
static void sheet_text(qwin *w, qsheet *s, uint32_t hinst, uint32_t p, wchar16 *out, int max)
{
    out[0] = 0;
    if (!p) return;
    if (p < 0x10000u) {
        uint32_t b[4], g = qw_alloc(w, (uint32_t)max * 2u);
        b[0] = hinst; b[1] = p; b[2] = g; b[3] = (uint32_t)max;
        u_LoadStringW(w, b);
        qw_rd_wstr(w, g, out, max);
        qw_free(w, g);
    } else if (s->wide) qw_rd_wstr(w, p, out, max);
    else { char t[260]; qw_rd_str(w, p, t, sizeof(t)); a2w(out, t, max); }
}
static uint32_t sheet_template(qwin *w, qsheet *s, uint32_t psp)
{
    uint32_t flags = RD32(psp + 4), name = RD32(psp + 12), hinst = RD32(psp + 8), r;
    if (flags & 1u) return name;                           /* PSP_DLGINDIRECT */
    if (!hinst) hinst = s->hinst;
    if (name < 0x10000u || s->wide) return dialog_template(w, hinst, name);
    {
        wchar16 t[64];
        uint32_t g = qw_alloc(w, 128);
        char c[64];
        qw_rd_str(w, name, c, sizeof(c));
        a2w(t, c, 64);
        qw_wr_wstr(w, g, t, 64);
        r = dialog_template(w, hinst, g);
        qw_free(w, g);
    }
    return r;
}
static int32_t sheet_notify(qwin *w, qsheet *s, int i, int code, uint32_t lp)
{
    uint32_t nm, r;
    qwnd *x;
    if (i < 0 || i >= s->n || !W(w, s->page[i].hwnd)) return 0;
    nm = qw_alloc(w, 16);
    WR32(nm, s->h); WR32(nm + 4, 0); WR32(nm + 8, (uint32_t)code); WR32(nm + 12, lp);
    x = W(w, s->page[i].hwnd);
    if (x->extra) memset(x->extra, 0, 4);
    r = win_send(w, s->page[i].hwnd, WM_NOTIFY, 0, nm);
    qw_free(w, nm);
    x = W(w, s->page[i].hwnd);
    if (!x || !r) return 0;
    if (x->extra) { uint32_t mr; memcpy(&mr, x->extra, 4); return (int32_t)mr; }
    return 0;
}
static void sheet_vis(qwin *w, uint32_t h, int on)
{
    qwnd *x = W(w, h);
    if (!x) return;
    x->visible = on;
    if (on) x->style |= WS_VISIBLE; else x->style &= ~WS_VISIBLE;
    win_send(w, h, WM_SHOWWINDOW, (uint32_t)on, 0);
    x = W(w, h);
    if (x) win_invalidate(w, on ? x : W(w, x->parent), 1);
}
static void sheet_button_style(qwin *w, qsheet *s, uint32_t id, int def)
{
    qwnd *b = find_ctl(w, W(w, s->h), id);
    if (b) win_send(w, b->h, BM_SETSTYLE, def ? 1u : 0u, 0);
}
static void sheet_wiz_buttons(qwin *w, qsheet *s)
{
    qwnd *f = W(w, s->h), *b;
    int fin = (s->wizbtn & 0xC) != 0;
    if (!s->wizard || !f) return;
    if ((b = find_ctl(w, f, QS_BACK)) != NULL) { b->enabled = (s->wizbtn & 1) != 0; win_invalidate(w, b, 1); }
    if ((b = find_ctl(w, f, QS_NEXT)) != NULL) { b->enabled = (s->wizbtn & 2) != 0; sheet_vis(w, b->h, !fin); }
    if ((b = find_ctl(w, f, QS_FINISH)) != NULL) { b->enabled = (s->wizbtn & 4) != 0; sheet_vis(w, b->h, fin); }
    sheet_button_style(w, s, QS_NEXT, !fin);
    sheet_button_style(w, s, QS_FINISH, fin);
    /* The buttons usually change after the page is shown: focus must not stay on a hidden one. */
    b = W(w, w->focus);
    if (b && b->parent == s->h && (!b->visible || !b->enabled)) {
        qwnd *d = find_ctl(w, f, fin ? QS_FINISH : QS_NEXT);
        if (d && d->visible && d->enabled) set_focus(w, d->h);
        else set_focus(w, s->h);
    }
}
static int sheet_create_page(qwin *w, qsheet *s, int i)
{
    uint32_t psp = s->page[i].psp, flags = RD32(psp + 4), proc = RD32(psp + 24), hinst = RD32(psp + 8), cb = RD32(psp + 32);
    qwnd *x;
    if ((flags & 0x80u) && cb && !qw_call(w, cb, 3, 0, 2 /* PSPCB_CREATE */, psp, 0)) return 0;
    if (!s->page[i].tpl) return 0;
    g_dlg_child = 1;
    s->page[i].hwnd = create_dialog(w, s->page[i].tpl, s->h, proc, psp, hinst ? hinst : s->hinst);
    g_dlg_child = 0;
    x = W(w, s->page[i].hwnd);
    if (!x) { s->page[i].hwnd = 0; return 0; }
    x->y = s->top;
    return 1;
}
static int sheet_index_of_id(qwin *w, qsheet *s, uint32_t id)
{
    int i;
    for (i = 0; i < s->n; i++) if (RD32(s->page[i].psp + 12) == id) return i;
    (void)w;
    return -1;
}
/* dir is where to go on when a page refuses to be shown. */
static int sheet_select(qwin *w, qsheet *s, int i, int dir)
{
    int tries, old = s->cur;
    uint32_t next;
    wchar16 t[128];
    for (tries = 0; tries <= s->n; tries++) {
        int32_t r;
        if (i < 0 || i >= s->n) return 0;
        if (!s->page[i].hwnd && !sheet_create_page(w, s, i)) { i += dir; continue; }
        s->cur = i;                                        /* a page asks the frame for the current one */
        r = sheet_notify(w, s, i, PSN_SETACTIVE, 0);
        if (r == -1) { i += dir; continue; }
        if (r) { int k = sheet_index_of_id(w, s, (uint32_t)r); if (k < 0) { i += dir; continue; } i = k; continue; }
        break;
    }
    if (tries > s->n) { s->cur = old; return 0; }
    if (old >= 0 && old != i && old < s->n && s->page[old].hwnd) sheet_vis(w, s->page[old].hwnd, 0);
    s->cur = i;
    sheet_vis(w, s->page[i].hwnd, 1);
    if (RD32(s->page[i].psp + 4) & 8u) sheet_text(w, s, RD32(s->page[i].psp + 8) ? RD32(s->page[i].psp + 8) : s->hinst, RD32(s->page[i].psp + 20), t, 128);
    else t[0] = 0;
    if (s->wizard && W(w, s->h)) {
        qwnd *f = W(w, s->h);
        const wchar16 *cap = t[0] ? t : s->caption;
        int k;
        for (k = 0; cap[k] && k < 255; k++) f->text[k] = cap[k];
        f->text[k] = 0;
        win_invalidate(w, f, 1);
    }
    /* Focus into the page, but not onto a push button such as Browse: Return must go on. */
    next = dlg_next_tab(w, s->page[i].hwnd, 0, 0);
    { qwnd *fc = W(w, next); if (fc && fc->cls && fc->cls->builtin == B_BUTTON && btype(fc) <= 1) next = 0; }
    if (next) set_focus(w, next);
    else { qwnd *b = find_ctl(w, W(w, s->h), s->wizard ? ((s->wizbtn & 0xC) ? QS_FINISH : QS_NEXT) : 1u); if (b) set_focus(w, b->h); else set_focus(w, s->h); }
    return 1;
}
static void sheet_end(qwin *w, qsheet *s, int result)
{
    qwnd *f = W(w, s->h);
    s->done = 1;
    s->result = result;
    if (f && !s->modeless) { f->modal_done = 1; f->modal_result = result; }
}
static void sheet_cancel(qwin *w, qsheet *s)
{
    int i;
    if (sheet_notify(w, s, s->cur, PSN_QUERYCANCEL, 0)) return;
    for (i = 0; i < s->n; i++) if (s->page[i].hwnd) sheet_notify(w, s, i, PSN_RESET, 0);
    sheet_end(w, s, s->result ? s->result : s->saved);
}
static int sheet_apply(qwin *w, qsheet *s)
{
    int i;
    if (sheet_notify(w, s, s->cur, PSN_KILLACTIVE, 0)) return 0;
    for (i = 0; i < s->n; i++) if (s->page[i].hwnd && sheet_notify(w, s, i, PSN_APPLY, 0)) { sheet_select(w, s, i, 0); return 0; }
    s->saved = 1;
    return 1;
}
static void sheet_go(qwin *w, qsheet *s, int code, int dir)
{
    int32_t r = sheet_notify(w, s, s->cur, code, 0);
    int k = s->cur + dir;
    if (r == -1) return;
    if (r) { k = sheet_index_of_id(w, s, (uint32_t)r); if (k < 0) return; }
    sheet_select(w, s, k, dir);
}
static void sheet_command(qwin *w, qsheet *s, uint32_t id)
{
    if (s->done) return;
    if (id == QS_BACK) sheet_go(w, s, PSN_WIZBACK, -1);
    else if (id == QS_NEXT) sheet_go(w, s, PSN_WIZNEXT, 1);
    else if (id == QS_FINISH) { if (!sheet_notify(w, s, s->cur, PSN_WIZFINISH, 0)) sheet_end(w, s, s->result ? s->result : 1); }
    else if (id == 2) sheet_cancel(w, s);
    else if (id == 1) { if (s->wizard) { /* Return with no button left: nothing to do */ } else if (sheet_apply(w, s)) sheet_end(w, s, s->result ? s->result : 1); }
    else if (id == QS_APPLY) {
        if (sheet_apply(w, s)) { qwnd *a = find_ctl(w, W(w, s->h), QS_APPLY); if (a) { a->enabled = 0; win_invalidate(w, a, 1); } }
    } else if (id >= QS_TAB && id < QS_TAB + QS_MAX) {
        if ((int)(id - QS_TAB) != s->cur && !sheet_notify(w, s, s->cur, PSN_KILLACTIVE, 0)) sheet_select(w, s, (int)(id - QS_TAB), 1);
    }
}
static int sheet_add(qwin *w, qsheet *s, int at, uint32_t psp)
{
    int i;
    if (s->n >= QS_MAX) return 0;
    if (at < 0 || at > s->n) at = s->n;
    for (i = s->n; i > at; i--) s->page[i] = s->page[i - 1];
    s->page[at].psp = psp; s->page[at].hwnd = 0;
    s->page[at].tpl = sheet_template(w, s, psp);
    s->n++;
    if (s->cur >= at && s->h) s->cur++;
    return 1;
}
/* Returns 1 when the message was one of the frame's. */
static int sheet_proc(qwin *w, struct qsheet *s, uint32_t *a, uint32_t *ret)
{
    uint32_t wp = a[2], lp = a[3];
    int i;
    *ret = 0;
    switch (a[1]) {
    case WM_COMMAND: sheet_command(w, s, wp & 0xFFFF); return 1;
    case WM_CLOSE: sheet_command(w, s, 2); return 1;
    case 0x0400: {                                         /* DM_GETDEFID */
        qwnd *f = W(w, s->h), *b = f ? find_ctl(w, f, s->wizard ? ((s->wizbtn & 0xC) ? QS_FINISH : QS_NEXT) : 1u) : NULL;
        *ret = b && b->enabled ? 0x534B0000u | (b->id & 0xFFFF) : 0;
        return 1; }
    case 0x0401: i = (int)wp; if (i < 0 || i >= s->n) { i = -1; { int k; for (k = 0; k < s->n; k++) if (s->page[k].psp == lp) i = k; } }  /* PSM_SETCURSEL */
        if (i >= 0 && (s->cur < 0 || !sheet_notify(w, s, s->cur, PSN_KILLACTIVE, 0))) *ret = (uint32_t)sheet_select(w, s, i, i >= s->cur ? 1 : -1);
        return 1;
    case 0x0402: {                                         /* PSM_REMOVEPAGE */
        i = (int)wp;
        if (i < 0 || i >= s->n) return 1;
        if (s->page[i].hwnd) win_destroy(w, W(w, s->page[i].hwnd));
        for (; i + 1 < s->n; i++) s->page[i] = s->page[i + 1];
        s->n--;
        if (s->cur >= s->n) s->cur = s->n - 1;
        return 1; }
    case 0x0403: *ret = (uint32_t)sheet_add(w, s, -1, lp); return 1;                 /* PSM_ADDPAGE */
    case 0x0477: *ret = (uint32_t)sheet_add(w, s, (int)wp, lp); return 1;            /* PSM_INSERTPAGE, by index */
    case 0x0404: { qwnd *b = find_ctl(w, W(w, s->h), QS_APPLY); if (b) { b->enabled = 1; win_invalidate(w, b, 1); } return 1; }  /* PSM_CHANGED */
    case 0x040D: return 1;                                 /* PSM_UNCHANGED */
    case 0x0409: s->result = 2; return 1;                  /* PSM_RESTARTWINDOWS */
    case 0x040A: s->result = 3; return 1;                  /* PSM_REBOOTSYSTEM */
    case 0x040B: { qwnd *b = find_ctl(w, W(w, s->h), 2); if (b) { a2w(b->text, "Close", 256); win_invalidate(w, b, 1); } return 1; }  /* PSM_CANCELTOCLOSE */
    case 0x040C:                                           /* PSM_QUERYSIBLINGS */
        for (i = 0; i < s->n; i++) if (s->page[i].hwnd && (*ret = win_send(w, s->page[i].hwnd, 0x040C, wp, lp)) != 0) break;
        return 1;
    case 0x046E: *ret = (uint32_t)sheet_apply(w, s); return 1;                       /* PSM_APPLY */
    case 0x046F: case 0x0478: {                            /* PSM_SETTITLE */
        qwnd *f = W(w, s->h);
        int was = s->wide, k;
        s->wide = a[1] == 0x0478;
        sheet_text(w, s, s->hinst, lp, s->caption, 128);
        s->wide = was;
        if (f) { for (k = 0; s->caption[k] && k < 255; k++) f->text[k] = s->caption[k]; f->text[k] = 0; win_invalidate(w, f, 1); }
        return 1; }
    case 0x0470: s->wizbtn = lp; sheet_wiz_buttons(w, s); return 1;                  /* PSM_SETWIZBUTTONS */
    case 0x0471: {                                         /* PSM_PRESSBUTTON */
        static const uint32_t ids[] = { QS_BACK, QS_NEXT, QS_FINISH, 1, QS_APPLY, 2, 9 };
        if (wp < 7) sheet_command(w, s, ids[wp]);
        return 1; }
    case 0x0472: i = lp ? sheet_index_of_id(w, s, lp) : -1; if (i >= 0 && !sheet_notify(w, s, s->cur, PSN_KILLACTIVE, 0)) *ret = (uint32_t)sheet_select(w, s, i, 1); return 1;  /* PSM_SETCURSELID */
    case 0x0473: case 0x0479: {                            /* PSM_SETFINISHTEXT */
        qwnd *b = find_ctl(w, W(w, s->h), QS_FINISH);
        int was = s->wide;
        s->wide = a[1] == 0x0479;
        if (b) sheet_text(w, s, s->hinst, lp, b->text, 64);
        s->wide = was;
        s->wizbtn |= 4;
        sheet_wiz_buttons(w, s);
        return 1; }
    case 0x0474: return 1;                                 /* PSM_GETTABCONTROL: we have none */
    case 0x0475: { uint32_t b[2]; b[0] = s->h; b[1] = lp; *ret = u_IsDialogMessageW(w, b); return 1; }  /* PSM_ISDIALOGMESSAGE */
    case 0x0476: *ret = !s->done && s->cur >= 0 && s->cur < s->n ? s->page[s->cur].hwnd : 0; return 1;  /* PSM_GETCURRENTPAGEHWND */
    case 0x0481: for (i = 0; i < s->n; i++) if (s->page[i].hwnd == wp) { *ret = (uint32_t)i; return 1; } *ret = 0xFFFFFFFFu; return 1;  /* PSM_HWNDTOINDEX */
    case 0x0482: *ret = wp < (uint32_t)s->n ? s->page[wp].hwnd : 0; return 1;        /* PSM_INDEXTOHWND */
    case 0x0483: for (i = 0; i < s->n; i++) if (s->page[i].psp == lp) { *ret = (uint32_t)i; return 1; } *ret = 0xFFFFFFFFu; return 1;  /* PSM_PAGETOINDEX */
    case 0x0484: *ret = wp < (uint32_t)s->n ? s->page[wp].psp : 0; return 1;         /* PSM_INDEXTOPAGE */
    case 0x0485: *ret = (uint32_t)sheet_index_of_id(w, s, lp); return 1;             /* PSM_IDTOINDEX */
    case 0x0486: *ret = wp < (uint32_t)s->n ? RD32(s->page[wp].psp + 12) : 0; return 1;  /* PSM_INDEXTOID */
    case 0x0487: *ret = (uint32_t)s->result; return 1;     /* PSM_GETRESULT */
    case WM_DESTROY:
        for (i = 0; i < s->n; i++) {
            uint32_t psp = s->page[i].psp, cb = RD32(psp + 32);
            if ((RD32(psp + 4) & 0x80u) && cb) qw_call(w, cb, 3, 0, 1 /* PSPCB_RELEASE */, psp, 0);
        }
        return 0;
    }
    return 0;
}
/* Tab runs through the page and then the frame's buttons; the rest goes to the page. */
static int sheet_key(qwin *w, qwnd *f, uint32_t hwnd, uint32_t msg, uint32_t wp)
{
    qsheet *s = (qsheet *)f->sheet;
    uint32_t page = s->cur >= 0 && s->cur < s->n ? s->page[s->cur].hwnd : 0;
    qwnd *c = W(w, hwnd);
    int in_page = 0;
    uint32_t code;
    { qwnd *q = c; while (q && q->h != page && (q->style & WS_CHILD)) q = W(w, q->parent); in_page = q && q->h == page; }
    code = c && hwnd != f->h ? win_send(w, hwnd, WM_GETDLGCODE, wp, 0) : 0;
    if (code & 4) return 0;
    if (msg == WM_CHAR) return (in_page && dlg_char(w, page, wp, code)) || dlg_char(w, f->h, wp, code);
    if (msg != WM_KEYDOWN) return 0;
    switch (wp) {
    case 0x09: {
        uint32_t list[DLG_MAXCTL + 16], at = 0;
        qwnd *pc[DLG_MAXCTL];
        int n = 0, k, m, here = -1, back = (w->keystate[0x10] & 0x80) != 0;
        if (code & 2) return 0;
        m = page ? dlg_controls(w, page, pc) : 0;
        for (k = 0; k < m; k++) if (dlg_can_focus(pc[k]) && (pc[k]->style & WS_TABSTOP)) list[n++] = pc[k]->h;
        m = dlg_controls(w, f->h, pc);
        for (k = 0; k < m && n < DLG_MAXCTL; k++) if (pc[k]->h != page && dlg_can_focus(pc[k]) && (pc[k]->style & WS_TABSTOP) && pc[k]->id < QS_TAB) list[n++] = pc[k]->h;
        for (k = 0; k < n; k++) { qwnd *q = c; while (q && q->h != list[k] && (q->style & WS_CHILD)) q = W(w, q->parent); if (q && q->h == list[k]) here = k; }
        if (!n) return 1;
        at = (uint32_t)(here < 0 ? (back ? n - 1 : 0) : (here + (back ? n - 1 : 1)) % n);
        set_focus(w, list[at]);
        return 1; }
    case 0x25: case 0x26: case 0x27: case 0x28:
        return in_page ? dlg_key(w, page, hwnd, msg, wp) : 1;
    case 0x0D: {
        uint32_t def;
        if (c && hwnd != f->h && c->cls && c->cls->builtin == B_BUTTON && btype(c) <= 1) { win_send(w, c->parent, WM_COMMAND, c->id & 0xFFFF, c->h); return 1; }
        def = win_send(w, f->h, 0x0400, 0, 0);
        if ((def >> 16) == 0x534B) sheet_command(w, s, def & 0xFFFF);
        return 1; }
    case 0x1B: sheet_command(w, s, 2); return 1;
    }
    return 0;
}
static uint32_t property_sheet(qwin *w, uint32_t *a, int wide)
{
    uint32_t hdr = a[0], flags = RD32(hdr + 4), parent = RD32(hdr + 8), hinst = RD32(hdr + 12), npages = RD32(hdr + 24), start = RD32(hdr + 28), pages = RD32(hdr + 32), cb = RD32(hdr + 36), h;
    qsheet *s;
    qclass *dc, *bc;
    static wchar16 dn[8], bn[8];
    int i, cw, ch, bw = 76, bh = 24, gap = 8, nb = 0, px, py, firstpage;
    struct { const char *label; uint32_t id; } b[5];
    qwnd tmp, *ow;
    s = (qsheet *)calloc(1, sizeof(*s));
    if (!s) return 0xFFFFFFFFu;
    s->flags = flags; s->hinst = hinst; s->wide = wide; s->cur = -1; s->result = 0;
    s->modeless = (flags & 0x400u) != 0;
    s->wizard = (flags & (0x20u | 0x2000u | 0x1000000u)) != 0;
    s->wizbtn = 3;
    for (i = 0; i < (int)npages && i < QS_MAX; i++) {
        uint32_t src = (flags & 8u) ? pages : RD32(pages + (uint32_t)i * 4u), psp;
        /* An array of pages: each is as long as it says. */
        if (flags & 8u) { uint32_t k; for (k = 0; k < (uint32_t)i; k++) src += RD32(src); }
        {
            uint32_t size = RD32(src), k;
            psp = qw_alloc(w, size);
            for (k = 0; k < size; k += 4) WR32(psp + k, RD32(src + k));
        }
        sheet_add(w, s, -1, psp);
    }
    if (!s->n) { free(s); return 0xFFFFFFFFu; }
    for (i = 0; i < s->n; i++) {
        uint32_t t = s->page[i].tpl;
        int ex, cx, cy;
        if (!t) continue;
        ex = x86_rd16(C, t) == 1 && x86_rd16(C, t + 2) == 0xFFFF;
        cx = (int16_t)x86_rd16(C, t + (ex ? 22 : 14)); cy = (int16_t)x86_rd16(C, t + (ex ? 24 : 16));
        if (cx * BASEX / 4 > s->pw) s->pw = cx * BASEX / 4;
        if (cy * BASEY / 8 > s->ph) s->ph = cy * BASEY / 8;
    }
    if (s->pw < 200) s->pw = 200;
    if (s->ph < 100) s->ph = 100;
    s->top = s->wizard ? 0 : 28;
    sheet_text(w, s, hinst, RD32(hdr + 20), s->caption, 128);
    if (s->wizard) {
        b[nb].label = "< &Back"; b[nb++].id = QS_BACK;
        b[nb].label = "&Next >"; b[nb++].id = QS_NEXT;
        b[nb].label = "Finish"; b[nb++].id = QS_FINISH;
        b[nb].label = "Cancel"; b[nb++].id = 2;
    } else {
        b[nb].label = "OK"; b[nb++].id = 1;
        b[nb].label = "Cancel"; b[nb++].id = 2;
        if (!(flags & 0x80u)) { b[nb].label = "&Apply"; b[nb++].id = QS_APPLY; }   /* PSH_NOAPPLYNOW */
    }
    cw = s->pw; ch = s->top + s->ph + 44;
    memset(&tmp, 0, sizeof(tmp));
    tmp.style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    nc_metrics(w, &tmp);
    ow = W(w, parent);
    ow = ow ? win_top(w, ow) : NULL;
    px = ow ? ow->x + (ow->cw - cw) / 2 : 100;
    py = ow ? ow->y + (ow->ch - ch) / 2 : 80;
    if (px < 0) px = 0;
    if (py < tmp.nt) py = tmp.nt;
    a2w(dn, "#32770", 8); a2w(bn, "Button", 8);
    dc = find_class(w, 0, dn); bc = find_class(w, 0, bn);
    h = create_window(w, 0, dc, s->caption, WS_POPUP | WS_CAPTION | WS_SYSMENU, px - tmp.nl, py - tmp.nt, cw + tmp.nl + tmp.nr, ch + tmp.nt + tmp.nb, parent, 0, hinst, 0);
    if (!h) { free(s); return 0xFFFFFFFFu; }
    s->h = h;
    W(w, h)->is_dialog = 1;
    W(w, h)->owner = parent;
    W(w, h)->sheet = s;
    {   /* Finish takes Next's place. */
        int shown = s->wizard ? nb - 1 : nb, k = 0;
        int x0 = cw - shown * bw - (shown - 1) * gap - 10;
        for (i = 0; i < nb; i++) {
            wchar16 label[16];
            uint32_t c;
            int slot = k;
            if (b[i].id == QS_FINISH) slot = k - 1; else k++;
            a2w(label, b[i].label, 16);
            c = create_window(w, 0, bc, label, WS_CHILD | WS_TABSTOP | (b[i].id == QS_FINISH ? 0u : WS_VISIBLE), x0 + slot * (bw + gap), ch - bh - 10, bw, bh, h, b[i].id, 0, 0);
            if (c && W(w, c)) { W(w, c)->visible = b[i].id != QS_FINISH; }
            if (c && b[i].id == QS_APPLY) W(w, c)->enabled = 0;
        }
    }
    if (!s->wizard) for (i = 0; i < s->n; i++) {               /* tabs: a row of buttons */
        wchar16 t[64];
        char nm[16];
        uint32_t psp = s->page[i].psp;
        t[0] = 0;
        if (RD32(psp + 4) & 8u) sheet_text(w, s, RD32(psp + 8) ? RD32(psp + 8) : hinst, RD32(psp + 20), t, 64);
        if (!t[0] && s->page[i].tpl) {                         /* the page's own caption */
            tplr r;
            wchar16 skip[64];
            r.p = s->page[i].tpl;
            r.ex = x86_rd16(C, r.p) == 1 && x86_rd16(C, r.p + 2) == 0xFFFF;
            r.p += r.ex ? 26 : 18;
            tsz(w, &r, skip, 64);
            tsz(w, &r, skip, 64);
            tsz(w, &r, t, 64);
        }
        if (!t[0]) { snprintf(nm, sizeof(nm), "Page %d", i + 1); a2w(t, nm, 64); }
        create_window(w, 0, bc, t, WS_CHILD | WS_VISIBLE, 4 + i * 84, 2, 80, 22, h, QS_TAB + (uint32_t)i, 0, 0);
    }
    sheet_wiz_buttons(w, s);
    if (!s->wizard) sheet_button_style(w, s, 1, 1);
    if ((flags & 0x100u) && cb) qw_call(w, cb, 3, h, 1 /* PSCB_INITIALIZED */, 0, 0);
    firstpage = (flags & 0x40u) ? 0 : (int)start;              /* PSH_USEPSTARTPAGE names a title: start at the first */
    if (firstpage >= s->n) firstpage = 0;
    if (!sheet_select(w, s, firstpage, 1)) { sheet_end(w, s, 0); }
    if (s->modeless) {
        qwnd *x = W(w, h);
        if (x) { x->visible = 1; x->style |= WS_VISIBLE; show_top(w, x); win_invalidate(w, x, 1); }
        return h;
    }
    {
        uint32_t r = (uint32_t)run_modal(w, h);
        for (i = 0; i < s->n; i++) qw_free(w, s->page[i].psp);
        free(s);
        return r;
    }
}
FN(u_PropertySheetA) { return property_sheet(w, a, 0); }
FN(u_PropertySheetW) { return property_sheet(w, a, 1); }
FN(u_CreatePropertySheetPage) {
    uint32_t size = RD32(a[0]), p = qw_alloc(w, size), k;
    for (k = 0; k < size; k += 4) WR32(p + k, RD32(a[0] + k));
    return p;
}
FN(u_DestroyPropertySheetPage) { (void)w; (void)a; return 1; }

const qhle qw_user32[] = {
    { "comctl32", "PropertySheetA", 1, u_PropertySheetA }, { "comctl32", "PropertySheetW", 1, u_PropertySheetW },
    { "comctl32", "CreatePropertySheetPageA", 1, u_CreatePropertySheetPage }, { "comctl32", "CreatePropertySheetPageW", 1, u_CreatePropertySheetPage },
    { "comctl32", "DestroyPropertySheetPage", 1, u_DestroyPropertySheetPage },
    U("RegisterClassExW", u_RegisterClassExW, 1), U("RegisterClassW", u_RegisterClassW, 1), U("UnregisterClassW", u_UnregisterClassW, 2),
    U("GetClassInfoExW", u_GetClassInfoExW, 3),
    U("CreateWindowExW", u_CreateWindowExW, 12), U("DestroyWindow", u_DestroyWindow, 1), U("ShowWindow", u_ShowWindow, 2),
    U("UpdateWindow", u_UpdateWindow, 1), U("DefWindowProcW", u_DefWindowProcW, 4), U("DefDlgProcW", u_DefDlgProcW, 4),
    U("CallWindowProcW", u_CallWindowProcW, 5), U("SendMessageW", u_SendMessageW, 4), U("PostMessageW", u_PostMessageW, 4),
    U("PostQuitMessage", u_PostQuitMessage, 1), U("GetMessageW", u_GetMessageW, 4), U("PeekMessageW", u_PeekMessageW, 5),
    U("WaitMessage", u_WaitMessage, 0), U("TranslateMessage", u_TranslateMessage, 1), U("DispatchMessageW", u_DispatchMessageW, 1),
    U("IsDialogMessageW", u_IsDialogMessageW, 2), U("BeginPaint", u_BeginPaint, 2), U("EndPaint", u_EndPaint, 2),
    U("GetDC", u_GetDC, 1), U("GetWindowDC", u_GetDC, 1), U("ReleaseDC", u_ReleaseDC, 2),
    U("InvalidateRect", u_InvalidateRect, 3), U("InvalidateRgn", u_InvalidateRect, 3), U("ValidateRect", u_ValidateRect, 2),
    U("RedrawWindow", u_RedrawWindow, 4), U("GetClientRect", u_GetClientRect, 2), U("GetWindowRect", u_GetWindowRect, 2),
    U("MoveWindow", u_MoveWindow, 6), U("SetWindowPos", u_SetWindowPos, 7), U("AdjustWindowRect", u_AdjustWindowRect, 3),
    U("AdjustWindowRectEx", u_AdjustWindowRectEx, 4), U("ClientToScreen", u_ClientToScreen, 2), U("ScreenToClient", u_ScreenToClient, 2),
    U("MapWindowPoints", u_MapWindowPoints, 4), U("GetSystemMetrics", u_GetSystemMetrics, 1), U("GetDesktopWindow", u_GetDesktopWindow, 0),
    U("GetParent", u_GetParent, 1), U("GetWindow", u_GetWindow, 2), U("IsWindow", u_IsWindow, 1), U("IsWindowVisible", u_IsWindowVisible, 1),
    U("IsWindowEnabled", u_IsWindowEnabled, 1), U("IsChild", u_IsChild, 2), U("IsIconic", u_IsIconic, 1), U("IsZoomed", u_IsIconic, 1),
    U("EnableWindow", u_EnableWindow, 2), U("GetDlgItem", u_GetDlgItem, 2), U("GetDlgCtrlID", u_GetDlgCtrlID, 1),
    U("SetWindowTextW", u_SetWindowTextW, 2), U("GetWindowTextW", u_GetWindowTextW, 3), U("GetWindowTextLengthW", u_GetWindowTextLengthW, 1),
    U("SetDlgItemTextW", u_SetDlgItemTextW, 3), U("GetDlgItemTextW", u_GetDlgItemTextW, 4), U("SetDlgItemInt", u_SetDlgItemInt, 4),
    U("GetDlgItemInt", u_GetDlgItemInt, 4), U("CheckDlgButton", u_CheckDlgButton, 3), U("IsDlgButtonChecked", u_IsDlgButtonChecked, 2),
    U("CheckRadioButton", u_CheckRadioButton, 4), U("SendDlgItemMessageW", u_SendDlgItemMessageW, 5), U("GetClassNameW", u_GetClassNameW, 3),
    U("EnumChildWindows", u_EnumChildWindows, 3), U("SetFocus", u_SetFocus, 1), U("ChildWindowFromPoint", u_ChildWindowFromPoint, 3), U("GetFocus", u_GetFocus, 0),
    U("SetCapture", u_SetCapture, 1), U("ReleaseCapture", u_ReleaseCapture, 0), U("GetCapture", u_GetCapture, 0),
    U("GetActiveWindow", u_GetActiveWindow, 0), U("GetForegroundWindow", u_GetActiveWindow, 0), U("SetActiveWindow", u_SetActiveWindow, 1),
    U("SetForegroundWindow", u_one, 1), U("BringWindowToTop", u_one, 1), U("FindWindowW", u_FindWindowW, 2),
    U("LoadStringW", u_LoadStringW, 4), U("LoadIconW", u_LoadIconW, 2), U("LoadCursorW", u_LoadCursorW, 2), U("LoadCursorFromFileA", u_LoadCursorFromFile, 1), U("LoadCursorFromFileW", u_LoadCursorFromFile, 1), U("LoadCursorA", u_LoadCursorW, 2), U("LoadIconA", u_LoadIconW, 2), U("LoadBitmapW", u_LoadBitmapW, 2),
    U("LoadImageW", u_LoadImageW, 6), U("DestroyIcon", u_DestroyIcon, 1), U("DestroyCursor", u_DestroyIcon, 1), U("SetCursor", u_SetCursor, 1),
    U("ShowCursor", u_ShowCursor, 1), U("GetCursorPos", u_GetCursorPos, 1), U("MessageBoxW", u_MessageBoxW, 4), U("MessageBeep", u_MessageBeep, 1),
    U("wsprintfW", u_wsprintfW, -1), U("wvsprintfW", u_wvsprintfW, 3), U("wsprintfA", u_wsprintfA, -1),
    U("CharNextW", u_CharNextW, 1), U("CharUpperW", u_CharUpperW, 1),
    U("GetKeyState", u_GetKeyState, 1), U("GetAsyncKeyState", u_GetAsyncKeyState, 1), U("GetKeyboardState", u_GetKeyboardState, 1),
    U("GetKeyboardLayout", u_GetKeyboardLayout, 1), U("MapVirtualKeyExW", u_MapVirtualKeyExW, 3), U("MapVirtualKeyW", u_MapVirtualKeyExW, 2),
    U("ToAsciiEx", u_ToAsciiEx, 6), U("ToAscii", u_ToAsciiEx, 5),
    U("OpenClipboard", u_OpenClipboard, 1), U("CloseClipboard", u_CloseClipboard, 0), U("EmptyClipboard", u_EmptyClipboard, 0),
    U("SetClipboardData", u_SetClipboardData, 2), U("GetClipboardData", u_GetClipboardData, 1), U("IsClipboardFormatAvailable", u_IsClipboardFormatAvailable, 1),
    U("SetRect", u_SetRect, 5), U("SetRectEmpty", u_SetRectEmpty, 1), U("CopyRect", u_CopyRect, 2), U("OffsetRect", u_OffsetRect, 3),
    U("InflateRect", u_InflateRect, 3), U("IsRectEmpty", u_IsRectEmpty, 1), U("PtInRect", u_PtInRect, 3), U("IntersectRect", u_IntersectRect, 3),
    U("UnionRect", u_UnionRect, 3), U("EqualRect", u_EqualRect, 2),
    U("MonitorFromRect", u_MonitorFromRect, 2), U("MonitorFromWindow", u_MonitorFromRect, 2), U("MonitorFromPoint", u_MonitorFromRect, 3),
    U("GetMonitorInfoW", u_GetMonitorInfoW, 2), U("SystemParametersInfoW", u_SystemParametersInfoW, 4),
    U("LoadAcceleratorsW", u_LoadAcceleratorsW, 2), U("TranslateAcceleratorW", u_TranslateAcceleratorW, 3),
    U("SetTimer", u_SetTimer, 4), U("KillTimer", u_KillTimer, 2), U("GetMessageTime", u_GetMessageTime, 0), U("GetMessagePos", u_GetMessagePos, 0),
    U("RegisterWindowMessageW", u_RegisterWindowMessageW, 1),
    U("GetWindowLongW", u_GetWindowLongW, 2), U("SetWindowLongW", u_SetWindowLongW, 3),
    /* ANSI forms without string arguments (string forms: ansi.c) */
    U("GetWindowLongA", u_GetWindowLongW, 2), U("SetWindowLongA", u_SetWindowLongW, 3),
    U("GetClassLongA", u_GetClassLongW, 2), U("SetClassLongA", u_SetClassLongW, 3),
    U("GetMessageA", u_GetMessageW, 4), U("PeekMessageA", u_PeekMessageW, 5), U("DispatchMessageA", u_DispatchMessageW, 1),
    U("PostMessageA", u_PostMessageW, 4), U("TranslateAcceleratorA", u_TranslateAcceleratorW, 3),
    U("IsDialogMessageA", u_IsDialogMessageW, 2), U("DefDlgProcA", u_DefDlgProcW, 4),
    U("SystemParametersInfoA", u_SystemParametersInfoW, 4), U("MapVirtualKeyA", u_MapVirtualKeyExW, 2),
    U("GetWindowTextLengthA", u_GetWindowTextLengthW, 1), U("SetWindowsHookExA", u_SetWindowsHookEx, 4),
    U("GetWindowLongPtrW", u_GetWindowLongW, 2), U("SetWindowLongPtrW", u_SetWindowLongW, 3),
    U("GetClassLongW", u_GetClassLongW, 2), U("SetClassLongW", u_SetClassLongW, 3),
    U("TrackMouseEvent", u_TrackMouseEvent, 1),
    U("CreateDialogParamW", u_CreateDialogParamW, 5), U("CreateDialogIndirectParamW", u_CreateDialogIndirectParamW, 5), U("CreateDialogIndirectParamA", u_CreateDialogIndirectParamW, 5), U("DialogBoxIndirectParamA", u_DialogBoxIndirectParamW, 5),
    U("DialogBoxParamW", u_DialogBoxParamW, 5), U("DialogBoxIndirectParamW", u_DialogBoxIndirectParamW, 5), U("EndDialog", u_EndDialog, 2),
    U("GetNextDlgTabItem", u_GetNextDlgTabItem, 3), U("GetNextDlgGroupItem", u_GetNextDlgGroupItem, 3), U("MapDialogRect", u_MapDialogRect, 2), U("GetDialogBaseUnits", u_GetDialogBaseUnits, 0),
    U("LoadMenuW", u_LoadMenuW, 2), U("CreateMenu", u_CreateMenu, 0), U("CreatePopupMenu", u_CreateMenu, 0), U("DestroyMenu", u_DestroyMenu, 1),
    U("GetMenu", u_GetMenu, 1), U("SetMenu", u_SetMenu, 2), U("GetSubMenu", u_GetSubMenu, 2), U("GetMenuItemCount", u_GetMenuItemCount, 1),
    U("GetMenuItemID", u_GetMenuItemID, 2), U("CheckMenuItem", u_CheckMenuItem, 3), U("EnableMenuItem", u_EnableMenuItem, 3),
    U("CheckMenuRadioItem", u_CheckMenuRadioItem, 5), U("GetMenuState", u_GetMenuState, 3), U("AppendMenuW", u_AppendMenuW, 4),
    U("InsertMenuItemA", u_InsertMenuItemA, 4), U("InsertMenuItemW", u_InsertMenuItemW, 4), U("ModifyMenuW", u_ModifyMenuW, 5), U("DeleteMenu", u_DeleteMenu, 3), U("RemoveMenu", u_DeleteMenu, 3), U("GetMenuStringW", u_GetMenuStringW, 5),
    U("DrawMenuBar", u_DrawMenuBar, 1), U("TrackPopupMenu", u_TrackPopupMenu, 7), U("TrackPopupMenuEx", u_TrackPopupMenu, 6),
    U("GetSystemMenu", u_GetSystemMenu, 2), U("SetWindowContextHelpId", u_one, 2), U("WinHelpW", u_one, 4),
    U("SetWindowsHookExW", u_SetWindowsHookEx, 4), U("UnhookWindowsHookEx", u_UnhookWindowsHookEx, 1), U("CallNextHookEx", u_zero, 4),
    U("qrx_ButtonProc", h_ButtonProc, 4), U("qrx_StaticProc", h_StaticProc, 4), U("qrx_EditProc", h_EditProc, 4),
    U("qrx_DialogProc", h_DialogProc, 4), U("qrx_ListBoxProc", h_ListBoxProc, 4), U("qrx_ScrollBarProc", h_ScrollBarProc, 4),
    U("qrx_MenuProc", h_MenuProc, 4),
    { NULL, NULL, 0, NULL }
};

void user_init(qwin *w)
{
    static const struct { const char *name, *proc; int b; int extra; } bi[] = {
        { "Button", "qrx_ButtonProc", B_BUTTON, 0 }, { "Static", "qrx_StaticProc", B_STATIC, 0 }, { "Edit", "qrx_EditProc", B_EDIT, 0 },
        { "#32770", "qrx_DialogProc", B_DIALOG, 30 }, { "ListBox", "qrx_ListBoxProc", B_LISTBOX, 0 }, { "ScrollBar", "qrx_ScrollBarProc", B_SCROLLBAR, 0 },
        { "ComboBox", "qrx_ListBoxProc", B_COMBOBOX, 0 }, { "#32768", "qrx_MenuProc", B_MENU, 0 },
    };
    unsigned i;
    w->caption_h = 19; w->frame_w = 3; w->menu_bar_h = 19;
    w->menu_open_index = -1;
    for (i = 0; i < sizeof(bi) / sizeof(bi[0]); i++) {
        wchar16 n[32];
        qclass *c;
        a2w(n, bi[i].name, 32);
        c = add_class(w, n);
        c->builtin = bi[i].b;
        c->wndproc = qw_thunk(w, "user32", bi[i].proc);
        c->cbwnd = (uint32_t)bi[i].extra;
        c->brush = bi[i].b == B_DIALOG ? 16 : 0;
        if (bi[i].b == B_DIALOG) c->atom = 0x8002u;
    }
}
