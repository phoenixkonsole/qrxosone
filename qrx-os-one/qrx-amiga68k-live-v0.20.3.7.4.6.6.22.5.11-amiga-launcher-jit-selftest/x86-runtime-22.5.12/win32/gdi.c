/* QRX OS One 22.5.12 - qrx-win32: GDI on 32-bit software surfaces. */
#include <stdlib.h>
#include <string.h>
#include "qrxwin.h"
#include "qrxwin_fontdata.h"
#include "../qmath/qmath.h"

#define FN(name) static uint32_t name(qwin *w, uint32_t *a)
#define C (&w->cpu)
#define RD32(x) x86_rd32(C, (x))
#define WR32(x, v) x86_wr32(C, (x), (v))
#define UNUSED (void)w; (void)a
#define OBJ_BASE 0x00400000u
#define H2I(h) (((h) - OBJ_BASE) >> 2)
#define I2H(i) (OBJ_BASE + ((uint32_t)(i) << 2))

enum { F_UI13 = 0, F_UI11, F_UI16, F_UI20, F_UI25, F_FIXED13 };

/* COLORREF 0x00BBGGRR <-> surface 0x00RRGGBB */
uint32_t rgb_of(uint32_t cr) { return ((cr & 0xFF) << 16) | (cr & 0xFF00) | ((cr >> 16) & 0xFF); }
static uint32_t cr_of(uint32_t rgb) { return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF); }

uint32_t sys_color(int idx)
{
    switch (idx) {
    case 0: return 0xC0C0C0;           /* COLOR_SCROLLBAR */
    case 1: return 0x3A6EA5;           /* COLOR_BACKGROUND */
    case 2: return 0x0A246A;           /* COLOR_ACTIVECAPTION */
    case 3: return 0x808080;
    case 4: return 0xD4D0C8;           /* COLOR_MENU */
    case 5: return 0xFFFFFF;           /* COLOR_WINDOW */
    case 6: return 0x000000;           /* COLOR_WINDOWFRAME */
    case 7: return 0x000000;           /* COLOR_MENUTEXT */
    case 8: return 0x000000;           /* COLOR_WINDOWTEXT */
    case 9: return 0xFFFFFF;           /* COLOR_CAPTIONTEXT */
    case 12: return 0x808080;          /* COLOR_APPWORKSPACE */
    case 13: return 0x0A246A;          /* COLOR_HIGHLIGHT */
    case 14: return 0xFFFFFF;          /* COLOR_HIGHLIGHTTEXT */
    case 15: return 0xD4D0C8;          /* COLOR_BTNFACE */
    case 16: return 0x808080;          /* COLOR_BTNSHADOW */
    case 17: return 0x808080;          /* COLOR_GRAYTEXT */
    case 18: return 0x000000;          /* COLOR_BTNTEXT */
    case 20: return 0xFFFFFF;          /* COLOR_BTNHIGHLIGHT */
    case 21: return 0x404040;          /* COLOR_3DDKSHADOW */
    case 22: return 0xD4D0C8;          /* COLOR_3DLIGHT */
    case 23: return 0x000000;
    case 24: return 0xFFFFE1;          /* COLOR_INFOBK */
    default: return 0xD4D0C8;
    }
}

void surf_resize(qsurf *s, int w, int h)
{
    uint32_t *n;
    int y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (s->pix && s->w == w && s->h == h) return;
    n = (uint32_t *)calloc((size_t)w * (size_t)h, 4);
    if (!n) return;
    if (s->pix) {
        for (y = 0; y < h && y < s->h; y++) memcpy(n + (size_t)y * (size_t)w, s->pix + (size_t)y * (size_t)s->w, (size_t)(w < s->w ? w : s->w) * 4);
        free(s->pix);
    }
    s->pix = n; s->w = w; s->h = h;
}

/* ---------------------------------------------------------------- objects */
qobj *O(qwin *w, uint32_t h, int type)
{
    uint32_t i = H2I(h);
    if (h < OBJ_BASE || i >= QW_MAXOBJ || ((h - OBJ_BASE) & 3)) return NULL;
    if (!w->obj[i].type || (type && w->obj[i].type != type)) return NULL;
    return &w->obj[i];
}

uint32_t gdi_new(qwin *w, int type)
{
    int i;
    for (i = 32; i < QW_MAXOBJ; i++)
        if (!w->obj[i].type) { memset(&w->obj[i], 0, sizeof(qobj)); w->obj[i].type = type; return I2H(i); }
    qw_log(w, "GDI object table full");
    return 0;
}

static void stock(qwin *w, int idx, int type, uint32_t rgb, int style)
{
    qobj *o = &w->obj[idx + 1];
    memset(o, 0, sizeof(*o));
    o->type = type; o->stock = 1; o->color = rgb; o->style = style; o->width = 1;
}
#define STOCK(i) I2H((i) + 1)

void gdi_init(qwin *w)
{
    stock(w, 0, GO_BRUSH, 0xFFFFFF, 0); stock(w, 1, GO_BRUSH, 0xC0C0C0, 0); stock(w, 2, GO_BRUSH, 0x808080, 0);
    stock(w, 3, GO_BRUSH, 0x404040, 0); stock(w, 4, GO_BRUSH, 0x000000, 0); stock(w, 5, GO_BRUSH, 0, 1);
    stock(w, 6, GO_PEN, 0xFFFFFF, 0); stock(w, 7, GO_PEN, 0x000000, 0); stock(w, 8, GO_PEN, 0, 5);
    stock(w, 10, GO_FONT, 0, 0); w->obj[11].font = F_FIXED13;
    stock(w, 11, GO_FONT, 0, 0); w->obj[12].font = F_FIXED13;
    stock(w, 12, GO_FONT, 0, 0); stock(w, 13, GO_FONT, 0, 0); stock(w, 14, GO_FONT, 0, 0); stock(w, 16, GO_FONT, 0, 0);
    stock(w, 17, GO_FONT, 0, 0); stock(w, 18, GO_BRUSH, 0xFFFFFF, 0); stock(w, 19, GO_PEN, 0, 0);
    stock(w, 15, GO_PEN, 0, 0);
    /* system colour brushes at 20..50 */
    {
        int i;
        for (i = 0; i < 31; i++) stock(w, 20 + i - 1 + 1, GO_BRUSH, sys_color(i), 0);
    }
}
static uint32_t syscolor_brush(int i) { return I2H(20 + 1 + i); }

/* ---------------------------------------------------------------- DCs */
qdc *DCP(qwin *w, uint32_t hdc)
{
    qobj *o = O(w, hdc, GO_DC);
    return o ? (qdc *)o->dc : NULL;
}

static qdc *dc_new(qwin *w, uint32_t *h)
{
    qdc *d;
    qobj *o;
    *h = gdi_new(w, GO_DC);
    o = O(w, *h, GO_DC);
    if (!o) return NULL;
    d = (qdc *)calloc(1, sizeof(qdc));
    o->dc = d;
    d->w = w;
    d->pen = STOCK(7); d->brush = STOCK(0); d->font = STOCK(13);
    d->textcol = 0x000000; d->bkcol = 0xFFFFFF; d->bkmode = 2; d->rop2 = 13;
    return d;
}

static void clip_to(qdc *d, int x0, int y0, int x1, int y1)
{
    if (x0 > d->cx0) d->cx0 = x0;
    if (y0 > d->cy0) d->cy0 = y0;
    if (x1 < d->cx1) d->cx1 = x1;
    if (y1 < d->cy1) d->cy1 = y1;
}

uint32_t dc_for_window(qwin *w, qwnd *x, int client)
{
    uint32_t h;
    qdc *d = dc_new(w, &h);
    qwnd *t = win_top(w, x), *p;
    int ax, ay;
    (void)client;
    if (!d || !t) return h;
    surf_resize(&t->surf, t->cw, t->ch);
    d->win = x->h;
    d->s = &t->surf;
    win_abs_origin(w, x, &ax, &ay);
    d->ox = ax; d->oy = ay;
    d->cx0 = ax; d->cy0 = ay; d->cx1 = ax + x->cw; d->cy1 = ay + x->ch;
    clip_to(d, 0, 0, t->surf.w, t->surf.h);
    for (p = W(w, x->parent); p && (p->style & WS_CHILD); p = W(w, p->parent)) {
        int px, py;
        win_abs_origin(w, p, &px, &py);
        clip_to(d, px, py, px + p->cw, py + p->ch);
    }
    if (x->font) d->font = x->font;
    t->present = 1;
    return h;
}

void dc_release(qwin *w, uint32_t hdc)
{
    qobj *o = O(w, hdc, GO_DC);
    qdc *d;
    if (!o) return;
    d = (qdc *)o->dc;
    if (d) { free(d->membmp.pix); free(d); }
    o->type = GO_FREE;
    o->dc = NULL;
}

/* ---------------------------------------------------------------- primitives (logical coordinates) */
#define TX(d, x) ((x) + (d)->ox + (d)->vox)
#define TY(d, y) ((y) + (d)->oy + (d)->voy)

static void px(qdc *d, int x, int y, uint32_t rgb)
{
    if (x < d->cx0 || y < d->cy0 || x >= d->cx1 || y >= d->cy1) return;
    if (d->rop2 == 7) d->s->pix[(size_t)y * (size_t)d->s->w + (size_t)x] ^= rgb;               /* R2_XORPEN */
    else if (d->rop2 == 6) d->s->pix[(size_t)y * (size_t)d->s->w + (size_t)x] ^= 0xFFFFFF;      /* R2_NOT */
    else d->s->pix[(size_t)y * (size_t)d->s->w + (size_t)x] = rgb;
}

static void fill_abs(qdc *d, int x0, int y0, int x1, int y1, uint32_t rgb)
{
    int x, y;
    if (x0 < d->cx0) x0 = d->cx0;
    if (y0 < d->cy0) y0 = d->cy0;
    if (x1 > d->cx1) x1 = d->cx1;
    if (y1 > d->cy1) y1 = d->cy1;
    for (y = y0; y < y1; y++) {
        uint32_t *r = d->s->pix + (size_t)y * (size_t)d->s->w;
        for (x = x0; x < x1; x++) r[x] = rgb;
    }
}

void gdi_fill(qdc *d, int x0, int y0, int x1, int y1, uint32_t rgb)
{
    fill_abs(d, TX(d, x0), TY(d, y0), TX(d, x1), TY(d, y1), rgb);
}

static void hline(qdc *d, int x0, int x1, int y, uint32_t rgb) { fill_abs(d, TX(d, x0), TY(d, y), TX(d, x1), TY(d, y) + 1, rgb); }
static void vline(qdc *d, int x, int y0, int y1, uint32_t rgb) { fill_abs(d, TX(d, x), TY(d, y0), TX(d, x) + 1, TY(d, y1), rgb); }

void gdi_frame3d(qdc *d, int x0, int y0, int x1, int y1, int sunken, int thick)
{
    uint32_t hi = sunken ? 0x808080 : 0xFFFFFF, lo = sunken ? 0xFFFFFF : 0x404040;
    uint32_t hi2 = sunken ? 0x404040 : 0xD4D0C8, lo2 = sunken ? 0xD4D0C8 : 0x808080;
    hline(d, x0, x1 - 1, y0, hi); vline(d, x0, y0, y1 - 1, hi);
    hline(d, x0, x1, y1 - 1, lo); vline(d, x1 - 1, y0, y1, lo);
    if (thick) {
        hline(d, x0 + 1, x1 - 2, y0 + 1, hi2); vline(d, x0 + 1, y0 + 1, y1 - 2, hi2);
        hline(d, x0 + 1, x1 - 1, y1 - 2, lo2); vline(d, x1 - 2, y0 + 1, y1 - 1, lo2);
    }
}

/* Liang-Barsky: clip the segment to the (widened) clip rectangle so that
 * absurd coordinates never turn into billions of plotted points */
static int clip_line(qdc *d, int *x0, int *y0, int *x1, int *y1, int m)
{
    double t0 = 0.0, t1 = 1.0, dx = (double)*x1 - *x0, dy = (double)*y1 - *y0;
    double p[4], q[4];
    int i;
    p[0] = -dx; q[0] = (double)*x0 - (d->cx0 - m);
    p[1] = dx;  q[1] = (double)(d->cx1 + m) - *x0;
    p[2] = -dy; q[2] = (double)*y0 - (d->cy0 - m);
    p[3] = dy;  q[3] = (double)(d->cy1 + m) - *y0;
    for (i = 0; i < 4; i++) {
        if (p[i] == 0) { if (q[i] < 0) return 0; continue; }
        {
            double r = q[i] / p[i];
            if (p[i] < 0) { if (r > t1) return 0; if (r > t0) t0 = r; }
            else { if (r < t0) return 0; if (r < t1) t1 = r; }
        }
    }
    {
        double ax = *x0, ay = *y0;
        *x0 = (int)(ax + t0 * dx + (ax + t0 * dx < 0 ? -0.5 : 0.5));
        *y0 = (int)(ay + t0 * dy + (ay + t0 * dy < 0 ? -0.5 : 0.5));
        *x1 = (int)(ax + t1 * dx + (ax + t1 * dx < 0 ? -0.5 : 0.5));
        *y1 = (int)(ay + t1 * dy + (ay + t1 * dy < 0 ? -0.5 : 0.5));
    }
    return 1;
}

static void line_abs(qdc *d, int x0, int y0, int x1, int y1, uint32_t rgb, int width)
{
    int dx, sx, dy, sy, err, e2;
    if ((x0 < d->cx0 - 64 || x0 > d->cx1 + 64 || y0 < d->cy0 - 64 || y0 > d->cy1 + 64 ||
         x1 < d->cx0 - 64 || x1 > d->cx1 + 64 || y1 < d->cy0 - 64 || y1 > d->cy1 + 64) &&
        !clip_line(d, &x0, &y0, &x1, &y1, width + 2)) return;
    dx = abs(x1 - x0); sx = x0 < x1 ? 1 : -1; dy = -abs(y1 - y0); sy = y0 < y1 ? 1 : -1; err = dx + dy;
    for (;;) {
        if (width <= 1) px(d, x0, y0, rgb);
        else { int r = width / 2, i, j; for (i = -r; i < width - r; i++) for (j = -r; j < width - r; j++) px(d, x0 + i, y0 + j, rgb); }
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static qobj *cur_pen(qwin *w, qdc *d) { qobj *o = O(w, d->pen, GO_PEN); return o; }
static qobj *cur_brush(qwin *w, qdc *d) { qobj *o = O(w, d->brush, GO_BRUSH); return o; }
static int pen_visible(qobj *p) { return p && p->style != 5; }
static int brush_visible(qobj *b) { return b && b->style != 1; }

/* ---------------------------------------------------------------- text */
static const qfont_data *FD(int f) { return &qfont_table[f >= 0 && f < QFONT_COUNT ? f : 0]; }
int gdi_font_height(int font) { return FD(font)->height; }

static const qfont_glyph *glyph(const qfont_data *f, unsigned ch)
{
    if (ch < (unsigned)f->lo || ch > (unsigned)f->hi) ch = (unsigned)f->hi + 1;
    return &f->g[ch - (unsigned)f->lo];
}

int gdi_text_width(int font, const wchar16 *s, int n)
{
    const qfont_data *f = FD(font);
    int i, wdt = 0;
    for (i = 0; i < n; i++) { const qfont_glyph *g = glyph(f, s[i] < 256 ? s[i] : '?'); wdt += g->kern + g->space; }
    return wdt;
}

static int font_of(qwin *w, qdc *d, int *bold)
{
    qobj *o = O(w, d->font, GO_FONT);
    *bold = o ? (o->width >= 700) : 0;
    return o ? o->font : 0;
}

/* draw at logical x,y (top of cell); returns advance */
int gdi_text(qdc *d, int x, int y, const wchar16 *s, int n, uint32_t col)
{
    int bold = 0, fi = font_of(d->w, d, &bold), i, gx, gy;
    const qfont_data *f = FD(fi);
    int ax = TX(d, x), ay = TY(d, y), start = ax;
    for (i = 0; i < n; i++) {
        const qfont_glyph *g = glyph(f, s[i] < 256 ? s[i] : '?');
        ax += g->kern;
        for (gy = 0; gy < f->height; gy++) {
            uint32_t r = f->rows[g->off + (uint32_t)gy];
            for (gx = 0; r && gx < g->width; gx++) if (r & (1u << gx)) { px(d, ax + gx, ay + gy, col); if (bold) px(d, ax + gx + 1, ay + gy, col); }
        }
        ax += g->space + (bold ? 1 : 0);
    }
    return ax - start;
}

static int text_out(qwin *w, qdc *d, int x, int y, const wchar16 *s, int n)
{
    int bold, fi = font_of(w, d, &bold);
    int wd = gdi_text_width(fi, s, n) + (bold ? n : 0), ht = FD(fi)->height;
    if ((d->ta & 6) == 6) x -= wd / 2;                 /* TA_CENTER */
    else if (d->ta & 2) x -= wd;                       /* TA_RIGHT */
    if ((d->ta & 0x18) == 0x18) y -= FD(fi)->baseline; /* TA_BASELINE */
    else if (d->ta & 8) y -= ht;                       /* TA_BOTTOM */
    if (d->bkmode == 2) gdi_fill(d, x, y, x + wd, y + ht, rgb_of(d->bkcol));
    gdi_text(d, x, y, s, n, rgb_of(d->textcol));
    return wd;
}

/* DrawText subset: DT_CENTER 1, DT_RIGHT 2, DT_VCENTER 4, DT_BOTTOM 8, DT_WORDBREAK 0x10,
 * DT_SINGLELINE 0x20, DT_CALCRECT 0x400, DT_NOPREFIX 0x800; '&' prefixes are underlined */
int gdi_drawtext(qdc *d, const wchar16 *s, int n, int *rc, uint32_t fmt)
{
    qwin *w = d->w;
    int bold, fi = font_of(w, d, &bold), lh = FD(fi)->height, i, y, lines = 0, maxw = 0;
    int rw = rc[2] - rc[0];
    wchar16 line[512];
    int starts[64], lens[64];
    /* split into lines */
    {
        int pos = 0;
        while (pos < n && lines < 64) {
            int st = pos, L = 0, lastsp = -1;
            while (pos < n && s[pos] != '\n') {
                if (s[pos] == '\r') { pos++; continue; }
                if ((fmt & 0x10) && !(fmt & 0x20)) {
                    if (s[pos] == ' ') lastsp = pos;
                    if (gdi_text_width(fi, s + st, pos - st + 1) > rw && lastsp > st) { pos = lastsp; break; }
                }
                pos++; L++;
            }
            starts[lines] = st; lens[lines] = pos - st; lines++;
            if (pos < n && (s[pos] == '\n' || s[pos] == ' ')) pos++;
            if (fmt & 0x20) break;
        }
        if (!lines) { starts[0] = 0; lens[0] = 0; lines = 1; }
    }
    for (i = 0; i < lines; i++) {
        int k, m = 0, wd;
        for (k = 0; k < lens[i] && m < 511; k++) { wchar16 ch = s[starts[i] + k]; if (ch == '&' && !(fmt & 0x800)) { if (k + 1 < lens[i] && s[starts[i] + k + 1] == '&') { line[m++] = '&'; k++; } continue; } if (ch != '\r') line[m++] = ch; }
        wd = gdi_text_width(fi, line, m);
        if (wd > maxw) maxw = wd;
    }
    if (fmt & 0x400) { rc[2] = rc[0] + maxw; rc[3] = rc[1] + lh * lines; return lh * lines; }
    y = rc[1];
    if ((fmt & 0x20) && (fmt & 4)) y = rc[1] + ((rc[3] - rc[1]) - lh) / 2;
    else if ((fmt & 0x20) && (fmt & 8)) y = rc[3] - lh;
    for (i = 0; i < lines; i++) {
        int k, m = 0, wd, x, ul = -1, ulx = 0;
        for (k = 0; k < lens[i] && m < 511; k++) {
            wchar16 ch = s[starts[i] + k];
            if (ch == '&' && !(fmt & 0x800)) { if (k + 1 < lens[i] && s[starts[i] + k + 1] == '&') { line[m++] = '&'; k++; } else ul = m; continue; }
            if (ch != '\r') line[m++] = ch;
        }
        wd = gdi_text_width(fi, line, m);
        x = (fmt & 1) ? rc[0] + (rw - wd) / 2 : (fmt & 2) ? rc[2] - wd : rc[0];
        if (d->bkmode == 2) gdi_fill(d, x, y, x + wd, y + lh, rgb_of(d->bkcol));
        gdi_text(d, x, y, line, m, rgb_of(d->textcol));
        if (ul >= 0 && ul < m) { ulx = x + gdi_text_width(fi, line, ul); hline(d, ulx, ulx + gdi_text_width(fi, line + ul, 1) - 1, y + FD(fi)->baseline + 1, rgb_of(d->textcol)); }
        y += lh;
    }
    return lh * lines;
}

/* ---------------------------------------------------------------- bitmaps */
uint32_t qw_bitmap_from_dib(qwin *w, uint32_t dib, uint32_t size)
{
    uint32_t hs = RD32(dib), bits, pal, ncol, stride, h;
    int32_t bw = (int32_t)RD32(dib + 4), bh = (int32_t)RD32(dib + 8);
    int bpp = x86_rd16(C, dib + 14), x, y, flip = 1;
    uint32_t comp = RD32(dib + 16);
    qobj *o;
    (void)size;
    if (hs == 12) { bw = x86_rd16(C, dib + 4); bh = x86_rd16(C, dib + 6); bpp = x86_rd16(C, dib + 10); comp = 0; }
    if (bh < 0) { bh = -bh; flip = 0; }
    if (bw <= 0 || bh <= 0 || bw > 4096 || bh > 4096 || comp > 0 && comp != 3) { qw_log(w, "unsupported DIB (%dx%d %dbpp comp %u)", bw, bh, bpp, comp); return 0; }
    ncol = hs >= 40 ? RD32(dib + 32) : 0;
    if (!ncol && bpp <= 8) ncol = 1u << bpp;
    pal = dib + hs;
    bits = pal + ncol * (hs == 12 ? 3 : 4) + (comp == 3 ? 12 : 0);
    stride = (((uint32_t)bw * (uint32_t)bpp + 31) / 32) * 4;
    h = gdi_new(w, GO_BITMAP);
    o = O(w, h, GO_BITMAP);
    if (!o) return 0;
    surf_resize(&o->bmp, bw, bh);
    for (y = 0; y < bh; y++) {
        uint32_t row = bits + (uint32_t)(flip ? bh - 1 - y : y) * stride;
        for (x = 0; x < bw; x++) {
            uint32_t v, rgb;
            if (bpp == 1) v = (x86_rd8(C, row + (uint32_t)x / 8) >> (7 - x % 8)) & 1;
            else if (bpp == 4) v = (x86_rd8(C, row + (uint32_t)x / 2) >> (x % 2 ? 0 : 4)) & 15;
            else if (bpp == 8) v = x86_rd8(C, row + (uint32_t)x);
            else if (bpp == 16) { uint32_t p = x86_rd16(C, row + (uint32_t)x * 2); rgb = ((p >> 10 & 31) << 19) | ((p >> 5 & 31) << 11) | ((p & 31) << 3); o->bmp.pix[(size_t)y * (size_t)bw + (size_t)x] = rgb; continue; }
            else if (bpp == 24) { uint32_t p = row + (uint32_t)x * 3; rgb = ((uint32_t)x86_rd8(C, p + 2) << 16) | ((uint32_t)x86_rd8(C, p + 1) << 8) | x86_rd8(C, p); o->bmp.pix[(size_t)y * (size_t)bw + (size_t)x] = rgb; continue; }
            else { uint32_t p = row + (uint32_t)x * 4; rgb = ((uint32_t)x86_rd8(C, p + 2) << 16) | ((uint32_t)x86_rd8(C, p + 1) << 8) | x86_rd8(C, p); o->bmp.pix[(size_t)y * (size_t)bw + (size_t)x] = rgb; continue; }
            if (hs == 12) { uint32_t p = pal + v * 3; rgb = ((uint32_t)x86_rd8(C, p + 2) << 16) | ((uint32_t)x86_rd8(C, p + 1) << 8) | x86_rd8(C, p); }
            else { uint32_t p = pal + v * 4; rgb = ((uint32_t)x86_rd8(C, p + 2) << 16) | ((uint32_t)x86_rd8(C, p + 1) << 8) | x86_rd8(C, p); }
            o->bmp.pix[(size_t)y * (size_t)bw + (size_t)x] = rgb;
        }
    }
    return h;
}

/* ---------------------------------------------------------------- gdi32 API */
FN(g_GetStockObject) { UNUSED; return a[0] <= 19 ? STOCK(a[0]) : 0; }
FN(g_SetBkMode) { qdc *d = DCP(w, a[0]); int o; if (!d) return 0; o = d->bkmode; d->bkmode = (int)a[1]; return (uint32_t)o; }
FN(g_SetTextColor) { qdc *d = DCP(w, a[0]); uint32_t o; if (!d) return 0xFFFFFFFFu; o = d->textcol; d->textcol = a[1]; return o; }
FN(g_SetBkColor) { qdc *d = DCP(w, a[0]); uint32_t o; if (!d) return 0xFFFFFFFFu; o = d->bkcol; d->bkcol = a[1]; return o; }
FN(g_GetTextColor) { qdc *d = DCP(w, a[0]); return d ? d->textcol : 0; }
FN(g_GetBkColor) { qdc *d = DCP(w, a[0]); return d ? d->bkcol : 0; }
FN(g_SetTextAlign) { qdc *d = DCP(w, a[0]); int o; if (!d) return 0; o = d->ta; d->ta = (int)a[1]; return (uint32_t)o; }
FN(g_SetROP2) { qdc *d = DCP(w, a[0]); int o; if (!d) return 0; o = d->rop2; d->rop2 = (int)a[1]; return (uint32_t)o; }
FN(g_GetTextExtentPoint32W) {
    qdc *d = DCP(w, a[0]);
    wchar16 t[512];
    int n = (int)a[2], bold = 0, fi;
    if (n > 511) n = 511;
    { int i; for (i = 0; i < n; i++) t[i] = x86_rd16(C, a[1] + (uint32_t)i * 2); }
    fi = d ? font_of(w, d, &bold) : 0;
    WR32(a[3], (uint32_t)(gdi_text_width(fi, t, n) + (bold ? n : 0)));
    WR32(a[3] + 4, (uint32_t)FD(fi)->height);
    return 1;
}
FN(g_GetTextMetricsW) {
    qdc *d = DCP(w, a[0]);
    int bold = 0, fi = d ? font_of(w, d, &bold) : 0, i;
    const qfont_data *f = FD(fi);
    wchar16 x = 'x';
    for (i = 0; i < 60; i += 4) WR32(a[1] + (uint32_t)i, 0);
    WR32(a[1], (uint32_t)f->height); WR32(a[1] + 4, (uint32_t)f->baseline); WR32(a[1] + 8, (uint32_t)(f->height - f->baseline));
    WR32(a[1] + 20, (uint32_t)gdi_text_width(fi, &x, 1)); WR32(a[1] + 24, (uint32_t)(gdi_text_width(fi, &x, 1) * 2));
    WR32(a[1] + 28, bold ? 700 : 400);
    x86_wr16(C, a[1] + 44, ' '); x86_wr16(C, a[1] + 46, 0xFF); x86_wr16(C, a[1] + 48, '?'); x86_wr16(C, a[1] + 50, ' ');
    x86_wr8(C, a[1] + 55, fi == F_FIXED13 ? 0x30 : 0x21);
    return 1;
}
FN(g_TextOutW) {
    qdc *d = DCP(w, a[0]);
    wchar16 t[1024];
    int n = (int)a[4], i;
    if (!d) return 0;
    if (n > 1023) n = 1023;
    for (i = 0; i < n; i++) t[i] = x86_rd16(C, a[3] + (uint32_t)i * 2);
    { int adv = text_out(w, d, (int32_t)a[1], (int32_t)a[2], t, n); if (d->ta & 1) d->px += adv; }
    return 1;
}
FN(g_ExtTextOutW) {      /* (hdc, x, y, options, lprect, lpString, count, lpDx) */
    qdc *d = DCP(w, a[0]);
    wchar16 t[1024];
    int n = (int)a[6], i, bk;
    if (!d) return 0;
    if ((a[3] & 2) && a[4]) gdi_fill(d, (int32_t)RD32(a[4]), (int32_t)RD32(a[4] + 4), (int32_t)RD32(a[4] + 8), (int32_t)RD32(a[4] + 12), rgb_of(d->bkcol));
    if (n > 1023) n = 1023;
    for (i = 0; i < n; i++) t[i] = x86_rd16(C, a[5] + (uint32_t)i * 2);
    bk = d->bkmode;
    if (a[3] & 2) d->bkmode = 1;           /* ETO_OPAQUE already filled the rectangle */
    text_out(w, d, (int32_t)a[1], (int32_t)a[2], t, n);
    d->bkmode = bk;
    return 1;
}
FN(g_CreateCompatibleDC) {
    uint32_t h;
    qdc *d = dc_new(w, &h);
    (void)a;
    if (!d) return 0;
    surf_resize(&d->membmp, 1, 1);
    d->s = &d->membmp;
    d->cx0 = d->cy0 = 0; d->cx1 = d->cy1 = 1;
    return h;
}
FN(g_DeleteDC) { dc_release(w, a[0]); return 1; }
FN(g_CreateCompatibleBitmap) {
    uint32_t h = gdi_new(w, GO_BITMAP);
    qobj *o = O(w, h, GO_BITMAP);
    (void)a;
    if (o) surf_resize(&o->bmp, (int)a[1], (int)a[2]);
    return h;
}
FN(g_CreateBitmap) {
    uint32_t h = gdi_new(w, GO_BITMAP);
    qobj *o = O(w, h, GO_BITMAP);
    if (o) surf_resize(&o->bmp, (int)a[0], (int)a[1]);
    return h;
}
FN(g_CreatePen) {
    uint32_t h = gdi_new(w, GO_PEN);
    qobj *o = O(w, h, GO_PEN);
    if (o) { o->style = (int)a[0]; o->width = a[1] ? (int)a[1] : 1; o->color = rgb_of(a[2]); }
    return h;
}
FN(g_CreateSolidBrush) {
    uint32_t h = gdi_new(w, GO_BRUSH);
    qobj *o = O(w, h, GO_BRUSH);
    if (o) o->color = rgb_of(a[0]);
    return h;
}
FN(g_CreateBrushIndirect) {
    uint32_t h = gdi_new(w, GO_BRUSH);
    qobj *o = O(w, h, GO_BRUSH);
    if (o) { o->style = RD32(a[0]) == 1 ? 1 : 0; o->color = rgb_of(RD32(a[0] + 4)); }
    return h;
}
static uint32_t make_font(qwin *w, int32_t height, int weight, int pitch)
{
    uint32_t h = gdi_new(w, GO_FONT);
    qobj *o = O(w, h, GO_FONT);
    int ht = height < 0 ? -height : height, f = F_UI13;
    if (pitch & 1) f = F_FIXED13;
    else if (!ht) f = F_UI13;
    else if (ht <= 11) f = F_UI11;
    else if (ht <= 14) f = F_UI13;
    else if (ht <= 17) f = F_UI16;
    else if (ht <= 22) f = F_UI20;
    else f = F_UI25;
    if (o) { o->font = f; o->width = weight; }
    return h;
}
FN(g_CreateFontW) { UNUSED; return make_font(w, (int32_t)a[0], (int)a[4], (int)(a[12] & 3)); }
FN(g_CreateFontIndirectW) { return make_font(w, (int32_t)RD32(a[0]), (int)RD32(a[0] + 16), x86_rd8(C, a[0] + 27) & 3); }
FN(g_SelectObject) {
    qdc *d = DCP(w, a[0]);
    qobj *o = O(w, a[1], 0);
    uint32_t old = 0;
    if (!d || !o) return 0;
    switch (o->type) {
    case GO_PEN: old = d->pen; d->pen = a[1]; break;
    case GO_BRUSH: old = d->brush; d->brush = a[1]; break;
    case GO_FONT: old = d->font; d->font = a[1]; break;
    case GO_BITMAP:
        old = d->bitmap ? d->bitmap : 1;
        d->bitmap = a[1];
        d->s = &o->bmp;
        d->cx0 = d->cy0 = 0; d->cx1 = o->bmp.w; d->cy1 = o->bmp.h;
        if (old == 1) old = I2H(31);    /* pseudo default bitmap */
        break;
    case GO_RGN: return 1;
    default: return 0;
    }
    return old;
}
FN(g_DeleteObject) {
    qobj *o = O(w, a[0], 0);
    if (!o || o->stock || o->type == GO_DC) return o ? 1 : 0;
    free(o->bmp.pix);
    memset(o, 0, sizeof(*o));
    return 1;
}
FN(g_GetObjectW) {
    qobj *o = O(w, a[0], 0);
    if (!o) return 0;
    if (o->type == GO_BITMAP && a[1] >= 24) {
        uint32_t i;
        for (i = 0; i < 24; i += 4) WR32(a[2] + i, 0);
        WR32(a[2] + 4, (uint32_t)o->bmp.w); WR32(a[2] + 8, (uint32_t)o->bmp.h); WR32(a[2] + 12, (uint32_t)o->bmp.w * 4);
        x86_wr16(C, a[2] + 16, 1); x86_wr16(C, a[2] + 18, 32);
        return 24;
    }
    return 0;
}
static uint32_t rop3(uint32_t rop, uint32_t s, uint32_t dd, uint32_t p)
{
    switch (rop) {
    case 0x00CC0020u: return s;
    case 0x00EE0086u: return s | dd;
    case 0x008800C6u: return s & dd;
    case 0x00660046u: return s ^ dd;
    case 0x00440328u: return s & ~dd;
    case 0x00330008u: return ~s & 0xFFFFFF;
    case 0x001100A6u: return ~(s | dd) & 0xFFFFFF;
    case 0x00BB0226u: return (~s & 0xFFFFFF) | dd;
    case 0x00550009u: return ~dd & 0xFFFFFF;
    case 0x00000042u: return 0;
    case 0x00FF0062u: return 0xFFFFFF;
    case 0x00F00021u: return p;
    case 0x005A0049u: return p ^ dd;
    case 0x00C000CAu: return p & s;
    default: return s;
    }
}
static int blt(qwin *w, qdc *d, int x, int y, int cw, int ch, qdc *s, int sx, int sy, int sw, int sh, uint32_t rop)
{
    qobj *b = cur_brush(w, d);
    uint32_t pat = b ? b->color : 0;
    int i, j;
    if (!d) return 0;
    if (cw < 0 || ch < 0) return 1;
    for (j = 0; j < ch; j++) {
        int dy = TY(d, y + j), syy;
        if (dy < d->cy0 || dy >= d->cy1) continue;
        syy = s ? TY(s, sy + (sh == ch ? j : j * sh / (ch ? ch : 1))) : 0;
        for (i = 0; i < cw; i++) {
            int dx = TX(d, x + i);
            uint32_t sv = 0, *dp;
            if (dx < d->cx0 || dx >= d->cx1) continue;
            if (s) {
                int sxx = TX(s, sx + (sw == cw ? i : i * sw / (cw ? cw : 1)));
                if (sxx < 0 || syy < 0 || sxx >= s->s->w || syy >= s->s->h) continue;
                sv = s->s->pix[(size_t)syy * (size_t)s->s->w + (size_t)sxx];
            }
            dp = &d->s->pix[(size_t)dy * (size_t)d->s->w + (size_t)dx];
            *dp = rop3(rop, sv, *dp, pat) & 0xFFFFFF;
        }
    }
    return 1;
}
FN(g_BitBlt) { return (uint32_t)blt(w, DCP(w, a[0]), (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], DCP(w, a[5]), (int32_t)a[6], (int32_t)a[7], (int32_t)a[3], (int32_t)a[4], a[8]); }
FN(g_StretchBlt) { return (uint32_t)blt(w, DCP(w, a[0]), (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], DCP(w, a[5]), (int32_t)a[6], (int32_t)a[7], (int32_t)a[8], (int32_t)a[9], a[10]); }
FN(g_PatBlt) { return (uint32_t)blt(w, DCP(w, a[0]), (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], NULL, 0, 0, 0, 0, a[5]); }
FN(g_SetPixel) { qdc *d = DCP(w, a[0]); if (d) px(d, TX(d, (int32_t)a[1]), TY(d, (int32_t)a[2]), rgb_of(a[3])); return a[3]; }
FN(g_GetPixel) {
    qdc *d = DCP(w, a[0]);
    int x, y;
    if (!d) return 0xFFFFFFFFu;
    x = TX(d, (int32_t)a[1]); y = TY(d, (int32_t)a[2]);
    if (x < 0 || y < 0 || x >= d->s->w || y >= d->s->h) return 0xFFFFFFFFu;
    return cr_of(d->s->pix[(size_t)y * (size_t)d->s->w + (size_t)x]);
}
FN(g_MoveToEx) { qdc *d = DCP(w, a[0]); if (!d) return 0; if (a[3]) { WR32(a[3], (uint32_t)d->px); WR32(a[3] + 4, (uint32_t)d->py); } d->px = (int32_t)a[1]; d->py = (int32_t)a[2]; return 1; }
FN(g_LineTo) {
    qdc *d = DCP(w, a[0]);
    qobj *p;
    if (!d) return 0;
    p = cur_pen(w, d);
    if (pen_visible(p)) line_abs(d, TX(d, d->px), TY(d, d->py), TX(d, (int32_t)a[1]), TY(d, (int32_t)a[2]), p->color, p->width);
    d->px = (int32_t)a[1]; d->py = (int32_t)a[2];
    return 1;
}
FN(g_Rectangle) {
    qdc *d = DCP(w, a[0]);
    qobj *p, *b;
    int x0 = (int32_t)a[1], y0 = (int32_t)a[2], x1 = (int32_t)a[3], y1 = (int32_t)a[4];
    if (!d) return 0;
    p = cur_pen(w, d); b = cur_brush(w, d);
    if (brush_visible(b)) gdi_fill(d, x0, y0, x1, y1, b->color);
    if (pen_visible(p)) { hline(d, x0, x1, y0, p->color); hline(d, x0, x1, y1 - 1, p->color); vline(d, x0, y0, y1, p->color); vline(d, x1 - 1, y0, y1, p->color); }
    return 1;
}
FN(g_Ellipse) {
    qdc *d = DCP(w, a[0]);
    qobj *p, *b;
    int x0 = (int32_t)a[1], y0 = (int32_t)a[2], x1 = (int32_t)a[3] - 1, y1 = (int32_t)a[4] - 1, y;
    double cx = (x0 + x1) / 2.0, cy = (y0 + y1) / 2.0, rx = (x1 - x0) / 2.0, ry = (y1 - y0) / 2.0;
    if (!d || rx <= 0 || ry <= 0) return 0;
    p = cur_pen(w, d); b = cur_brush(w, d);
    {   /* only the rows/columns inside the clip rectangle */
        int ylo = d->cy0 - d->oy - d->voy - 2, yhi = d->cy1 - d->oy - d->voy + 2;
        if (y0 < ylo) y0 = ylo;
        if (y1 > yhi) y1 = yhi;
    }
    for (y = y0; y <= y1; y++) {
        double dy = (y - cy) / ry, t = 1.0 - dy * dy, hx;
        int xa, xb;
        if (t < 0) continue;
        hx = rx * qm_sqrt(t);
        xa = (int)(cx - hx + 0.5); xb = (int)(cx + hx + 0.5);
        if (brush_visible(b)) hline(d, xa, xb + 1, y, b->color);
        if (pen_visible(p)) {
            int k, pw = p->width > 0 ? p->width : 1;
            for (k = 0; k < pw; k++) { px(d, TX(d, xa + k), TY(d, y), p->color); px(d, TX(d, xb - k), TY(d, y), p->color); }
        }
    }
    if (pen_visible(p)) {      /* close the outline at top/bottom with x-major sampling */
        int x, xlo = d->cx0 - d->ox - d->vox - 2, xhi = d->cx1 - d->ox - d->vox + 2;
        if (x0 < xlo) x0 = xlo;
        if (x1 > xhi) x1 = xhi;
        for (x = x0; x <= x1; x++) {
            double dx = (x - cx) / rx, t = 1.0 - dx * dx, hy;
            if (t < 0) continue;
            hy = ry * qm_sqrt(t);
            px(d, TX(d, x), TY(d, (int)(cy - hy + 0.5)), p->color);
            px(d, TX(d, x), TY(d, (int)(cy + hy + 0.5)), p->color);
        }
    }
    return 1;
}
FN(g_Polygon) {
    qdc *d = DCP(w, a[0]);
    qobj *p, *b;
    int n = (int)a[2], i, y, ymin = 1 << 30, ymax = -(1 << 30);
    int xs[64], ys[64];
    if (!d || n < 2 || n > 64) return 0;
    for (i = 0; i < n; i++) { xs[i] = (int32_t)RD32(a[1] + (uint32_t)i * 8); ys[i] = (int32_t)RD32(a[1] + (uint32_t)i * 8 + 4); if (ys[i] < ymin) ymin = ys[i]; if (ys[i] > ymax) ymax = ys[i]; }
    p = cur_pen(w, d); b = cur_brush(w, d);
    {
        int ylo = d->cy0 - d->oy - d->voy - 1, yhi = d->cy1 - d->oy - d->voy + 1;
        if (ymin < ylo) ymin = ylo;
        if (ymax > yhi) ymax = yhi;
    }
    if (brush_visible(b)) {
        for (y = ymin; y <= ymax; y++) {
            int xi[64], k = 0, j;
            for (i = 0; i < n; i++) {
                int x0 = xs[i], y0 = ys[i], x1 = xs[(i + 1) % n], y1 = ys[(i + 1) % n];
                if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) xi[k++] = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
            }
            for (i = 0; i < k; i++) for (j = i + 1; j < k; j++) if (xi[j] < xi[i]) { int t = xi[i]; xi[i] = xi[j]; xi[j] = t; }
            for (i = 0; i + 1 < k; i += 2) hline(d, xi[i], xi[i + 1] + 1, y, b->color);
        }
    }
    if (pen_visible(p)) for (i = 0; i < n; i++) line_abs(d, TX(d, xs[i]), TY(d, ys[i]), TX(d, xs[(i + 1) % n]), TY(d, ys[(i + 1) % n]), p->color, p->width);
    return 1;
}
FN(g_GetDeviceCaps) {
    UNUSED;
    switch (a[1]) {
    case 8: return 1024; case 10: return 768;        /* HORZRES / VERTRES */
    case 12: return 32; case 14: return 1;          /* BITSPIXEL / PLANES */
    case 88: case 90: return 96;                     /* LOGPIXELSX/Y */
    case 24: return 0xFFFFFFFFu;                     /* NUMCOLORS */
    default: return 0;
    }
}
FN(g_SetViewportOrgEx) { qdc *d = DCP(w, a[0]); if (!d) return 0; if (a[3]) { WR32(a[3], (uint32_t)d->vox); WR32(a[3] + 4, (uint32_t)d->voy); } d->vox = (int32_t)a[1]; d->voy = (int32_t)a[2]; return 1; }
FN(g_SaveDC) { UNUSED; return 1; }
FN(g_RestoreDC) { UNUSED; return 1; }
FN(g_CreateRectRgn) { uint32_t h = gdi_new(w, GO_RGN); qobj *o = O(w, h, GO_RGN); if (o) { o->x = (int32_t)a[0]; o->y = (int32_t)a[1]; o->w = (int32_t)a[2]; o->h = (int32_t)a[3]; } return h; }
FN(g_SelectClipRgn) { UNUSED; return 1; }
FN(g_IntersectClipRect) {
    qdc *d = DCP(w, a[0]);
    if (!d) return 0;
    clip_to(d, TX(d, (int32_t)a[1]), TY(d, (int32_t)a[2]), TX(d, (int32_t)a[3]), TY(d, (int32_t)a[4]));
    return 2;
}

/* ---------------------------------------------------------------- user32 drawing helpers */
static void rect_of(qwin *w, uint32_t p, int *r) { r[0] = (int32_t)RD32(p); r[1] = (int32_t)RD32(p + 4); r[2] = (int32_t)RD32(p + 8); r[3] = (int32_t)RD32(p + 12); }
static uint32_t brush_color(qwin *w, uint32_t hbr, int *ok)
{
    qobj *o;
    *ok = 1;
    if (hbr && hbr <= 31) return sys_color((int)hbr - 1);       /* COLOR_xxx + 1 */
    o = O(w, hbr, GO_BRUSH);
    if (!o || o->style == 1) { *ok = 0; return 0; }
    return o->color;
}
FN(u_FillRect) {
    qdc *d = DCP(w, a[0]);
    int r[4], ok;
    uint32_t col = brush_color(w, a[2], &ok);
    if (!d) return 0;
    rect_of(w, a[1], r);
    if (ok) gdi_fill(d, r[0], r[1], r[2], r[3], col);
    return 1;
}
FN(u_FrameRect) {
    qdc *d = DCP(w, a[0]);
    int r[4], ok;
    uint32_t col = brush_color(w, a[2], &ok);
    if (!d || !ok) return 0;
    rect_of(w, a[1], r);
    hline(d, r[0], r[2], r[1], col); hline(d, r[0], r[2], r[3] - 1, col); vline(d, r[0], r[1], r[3], col); vline(d, r[2] - 1, r[1], r[3], col);
    return 1;
}
FN(u_InvertRect) {
    qdc *d = DCP(w, a[0]);
    int r[4], x, y;
    if (!d) return 0;
    rect_of(w, a[1], r);
    for (y = TY(d, r[1]); y < TY(d, r[3]); y++)
        for (x = TX(d, r[0]); x < TX(d, r[2]); x++)
            if (x >= d->cx0 && y >= d->cy0 && x < d->cx1 && y < d->cy1) d->s->pix[(size_t)y * (size_t)d->s->w + (size_t)x] ^= 0xFFFFFF;
    return 1;
}
FN(u_DrawFocusRect) {
    qdc *d = DCP(w, a[0]);
    int r[4], x, y;
    if (!d) return 0;
    rect_of(w, a[1], r);
    for (x = r[0]; x < r[2]; x += 2) { px(d, TX(d, x), TY(d, r[1]), 0); px(d, TX(d, x), TY(d, r[3] - 1), 0); }
    for (y = r[1]; y < r[3]; y += 2) { px(d, TX(d, r[0]), TY(d, y), 0); px(d, TX(d, r[2] - 1), TY(d, y), 0); }
    return 1;
}
FN(u_DrawEdge) {
    qdc *d = DCP(w, a[0]);
    int r[4];
    uint32_t edge = a[2];
    if (!d) return 0;
    rect_of(w, a[1], r);
    if (edge & 0x0A) gdi_frame3d(d, r[0], r[1], r[2], r[3], 1, (edge & 0x0F) == 0x0A);       /* sunken */
    else gdi_frame3d(d, r[0], r[1], r[2], r[3], 0, (edge & 0x0F) == 0x05);
    if (a[3] & 0x2000) { r[0] += 2; r[1] += 2; r[2] -= 2; r[3] -= 2; x86_wr32(C, a[1], (uint32_t)r[0]); x86_wr32(C, a[1] + 4, (uint32_t)r[1]); x86_wr32(C, a[1] + 8, (uint32_t)r[2]); x86_wr32(C, a[1] + 12, (uint32_t)r[3]); }
    return 1;
}
FN(u_DrawFrameControl) {
    qdc *d = DCP(w, a[0]);
    int r[4];
    if (!d) return 0;
    rect_of(w, a[1], r);
    if (a[2] == 4 && (a[3] & 0x1F) == 0x10) {     /* DFC_BUTTON / DFCS_BUTTONPUSH */
        gdi_fill(d, r[0], r[1], r[2], r[3], sys_color(15));
        gdi_frame3d(d, r[0], r[1], r[2], r[3], (a[3] & 0x200) != 0, 1);
        return 1;
    }
    gdi_fill(d, r[0], r[1], r[2], r[3], sys_color(15));
    gdi_frame3d(d, r[0], r[1], r[2], r[3], (a[3] & 0x200) != 0, 1);
    return 1;
}
FN(u_DrawStateW) {
    qdc *d = DCP(w, a[0]);
    uint32_t flags = a[9];
    wchar16 t[512];
    int n;
    if (!d) return 0;
    if ((flags & 0xF) != 1 && (flags & 0xF) != 2) return 1;     /* DST_TEXT / DST_PREFIXTEXT */
    n = a[4] ? (int)a[4] : qw_rd_wstr(w, a[3], t, 512);
    if (a[4]) { int i; for (i = 0; i < n && i < 511; i++) t[i] = x86_rd16(C, a[3] + (uint32_t)i * 2); t[n < 511 ? n : 511] = 0; }
    if (flags & 0x20) {                                         /* DSS_DISABLED: embossed */
        gdi_text(d, (int32_t)a[5] + 1, (int32_t)a[6] + 1, t, n, 0xFFFFFF);
        gdi_text(d, (int32_t)a[5], (int32_t)a[6], t, n, 0x808080);
    } else {
        int rc[4];
        rc[0] = (int32_t)a[5]; rc[1] = (int32_t)a[6]; rc[2] = rc[0] + 4000; rc[3] = rc[1] + 400;
        {
            int bk = d->bkmode; d->bkmode = 1;
            gdi_drawtext(d, t, n, rc, (flags & 0xF) == 2 ? 0x20 : 0x820);
            d->bkmode = bk;
        }
    }
    return 1;
}
FN(u_DrawTextW) {
    qdc *d = DCP(w, a[0]);
    wchar16 t[1024];
    int n = (int32_t)a[2], r[4], h;
    if (!d) return 0;
    if (n < 0) n = qw_rd_wstr(w, a[1], t, 1024);
    else { int i; if (n > 1023) n = 1023; for (i = 0; i < n; i++) t[i] = x86_rd16(C, a[1] + (uint32_t)i * 2); }
    rect_of(w, a[3], r);
    h = gdi_drawtext(d, t, n, r, a[4]);
    if (a[4] & 0x400) { WR32(a[3] + 8, (uint32_t)r[2]); WR32(a[3] + 12, (uint32_t)r[3]); }
    return (uint32_t)h;
}
FN(u_GetSysColor) { UNUSED; return cr_of(sys_color((int)a[0])); }
FN(u_GetSysColorBrush) { UNUSED; return syscolor_brush((int)a[0]); }
FN(u_DrawIcon) { UNUSED; return 1; }
FN(u_DrawIconEx) { UNUSED; return 1; }

#define G(n, f, c) { "gdi32", n, c, f }
#define U(n, f, c) { "user32", n, c, f }
const qhle qw_gdi32[] = {
    G("GetStockObject", g_GetStockObject, 1), G("SetBkMode", g_SetBkMode, 2), G("SetTextColor", g_SetTextColor, 2),
    G("SetBkColor", g_SetBkColor, 2), G("GetTextColor", g_GetTextColor, 1), G("GetBkColor", g_GetBkColor, 1),
    G("SetTextAlign", g_SetTextAlign, 2), G("SetROP2", g_SetROP2, 2),
    G("GetTextExtentPoint32W", g_GetTextExtentPoint32W, 4), G("GetTextExtentPointW", g_GetTextExtentPoint32W, 4),
    G("GetTextMetricsW", g_GetTextMetricsW, 2), G("TextOutW", g_TextOutW, 5), G("ExtTextOutW", g_ExtTextOutW, 8),
    G("CreateCompatibleDC", g_CreateCompatibleDC, 1), G("DeleteDC", g_DeleteDC, 1),
    G("CreateCompatibleBitmap", g_CreateCompatibleBitmap, 3), G("CreateBitmap", g_CreateBitmap, 5),
    G("CreatePen", g_CreatePen, 3), G("CreateSolidBrush", g_CreateSolidBrush, 1), G("CreateBrushIndirect", g_CreateBrushIndirect, 1),
    G("CreateFontW", g_CreateFontW, 14), G("CreateFontIndirectW", g_CreateFontIndirectW, 1),
    G("SelectObject", g_SelectObject, 2), G("DeleteObject", g_DeleteObject, 1), G("GetObjectW", g_GetObjectW, 3),
    G("BitBlt", g_BitBlt, 9), G("StretchBlt", g_StretchBlt, 11), G("PatBlt", g_PatBlt, 6),
    G("SetPixel", g_SetPixel, 4), G("SetPixelV", g_SetPixel, 4), G("GetPixel", g_GetPixel, 3),
    G("MoveToEx", g_MoveToEx, 4), G("LineTo", g_LineTo, 3), G("Rectangle", g_Rectangle, 5), G("Ellipse", g_Ellipse, 5),
    G("Polygon", g_Polygon, 3), G("GetDeviceCaps", g_GetDeviceCaps, 2), G("SetViewportOrgEx", g_SetViewportOrgEx, 4),
    G("SaveDC", g_SaveDC, 1), G("RestoreDC", g_RestoreDC, 2), G("CreateRectRgn", g_CreateRectRgn, 4),
    G("SelectClipRgn", g_SelectClipRgn, 2), G("IntersectClipRect", g_IntersectClipRect, 5),
    U("FillRect", u_FillRect, 3), U("FrameRect", u_FrameRect, 3), U("InvertRect", u_InvertRect, 2), U("DrawFocusRect", u_DrawFocusRect, 2),
    U("DrawEdge", u_DrawEdge, 4), U("DrawFrameControl", u_DrawFrameControl, 4), U("DrawStateW", u_DrawStateW, 10),
    U("DrawTextW", u_DrawTextW, 5), U("GetSysColor", u_GetSysColor, 1), U("GetSysColorBrush", u_GetSysColorBrush, 1),
    U("DrawIcon", u_DrawIcon, 4), U("DrawIconEx", u_DrawIconEx, 9),
    { NULL, NULL, 0, NULL }
};
