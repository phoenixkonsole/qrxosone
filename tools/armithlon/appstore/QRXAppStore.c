/* QRXAppStore: the QRX App Store. Large programs (Blender first) are not part of the system image;
 * this window lists what the catalog offers, downloads an app on request, checks it and installs
 * it in APPS:, opens and removes it. Laid out like Apple's App Store: a sidebar with search and
 * sections, a large title, a featured card, rows with round GET/OPEN buttons, a progress ring
 * while loading, and a page per app.
 *
 * The work happens on the host (door service 11, /qrx/netctl store-..., see
 * tools/armithlon/pi/image/rootfs/qrx/appstore/store): it holds the catalog, which pins every
 * archive by SHA-256, downloads over HTTPS and installs only an archive whose hash matches.
 * Without that host (QRX OS One on a Mac) the window says so.
 *
 * Drawing: the surfaces are drawn with anti-aliased edges into an ARGB buffer and written with
 * cybergraphics; the text comes on top with diskfont's Open Sans (Bold, Semibold, Regular). */
#include <exec/types.h>
#include <exec/tasks.h>
#include <intuition/intuition.h>
#include <graphics/text.h>
#include <graphics/rpattr.h>
#include <cybergraphx/cybergraphics.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/diskfont.h>
#include <proto/cybergraphics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *DiskfontBase, *CyberGfxBase;

#define WIN_W 960
#define WIN_H 640
#define SIDE_W 214

/* ------------------------------------------------------------------ the host (door service 11) */
static ULONG q[32] __attribute__((aligned(16)));
static char args[512] __attribute__((aligned(16)));
static char out[16384] __attribute__((aligned(16)));
static int ctl(const char *a0, const char *a1)
{
    const char *a[2] = { a0, a1 };
    int n = 0;
    memset(args, 0, sizeof args);
    for (int i = 0; i < 2 && a[i]; i++) {
        int l = (int)strlen(a[i]);
        if (n + l + 2 > (int)sizeof args) return -1;
        memcpy(args + n, a[i], l);
        n += l + 1;
    }
    out[0] = 0;
    memset(q, 0, sizeof q);
    q[0] = 11; q[4] = (ULONG)(IPTR)args; q[5] = (ULONG)(IPTR)out; q[6] = sizeof out;
#if defined(__m68k64__)
    if (*(volatile UQUAD *)0xFFFFFFFF00000000UL != 0x5152584543363442UL) return -1;
    *(volatile UQUAD *)0xFFFFFFFF00000400UL = (UQUAD)(IPTR)q;
#else
    if (*(volatile ULONG *)0xFF000880UL != 0x51383644UL) return -1;
    *(volatile ULONG *)0xFF00088CUL = (ULONG)(IPTR)q;
#endif
    __asm__ volatile("" ::: "memory");
    return q[12] == 0xFFFFFFFFUL ? -1 : (int)q[13];
}

/* ------------------------------------------------------------------ the catalog */
#define MAX_APPS 32
typedef struct {
    char id[44], name[48], sub[96], cat[24], ver[24], dev[48], lic[32], state[12], launch[64], desc[1024];
    ULONG size, isize, done, total;
    char msg[64];
} App;
static App apps[MAX_APPS];
static int napps, host_ok;
static char host_msg[160];

#if defined(__m68k64__)
#define MY_ARCH "m68k64"
#define MY_SYSTEM "QRX OS One 64"
#else
#define MY_ARCH "m68k"
#define MY_SYSTEM "QRX OS One"
#endif
static int split(char *line, char **f, int n)
{
    int k = 0;
    while (k < n) {
        f[k++] = line;
        char *t = strchr(line, '\t');
        if (!t) break;
        *t = 0;
        line = t + 1;
    }
    return k;
}
static void load_catalog(void)
{
    napps = 0;
    int r = ctl("store-catalog", NULL);
    host_ok = r == 0;
    if (r < 0) { snprintf(host_msg, sizeof host_msg, "The App Store needs the QRX appliance (Raspberry Pi image)."); return; }
    if (r != 0) { snprintf(host_msg, sizeof host_msg, "%.150s", out[0] ? out : "The catalog could not be read."); return; }
    char *p = out;
    while (*p && napps < MAX_APPS) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        char *f[13];
        int nf = split(p, f, 13);
        /* only apps for this system: 32-bit hunk programs here, m68k64 ELF programs on QRX OS One 64 */
        const char *arch = nf == 13 && f[12][0] ? f[12] : "m68k";
        if (nf >= 12 && !strcmp(arch, MY_ARCH)) {
            App *a = &apps[napps++];
            memset(a, 0, sizeof *a);
            snprintf(a->id, sizeof a->id, "%s", f[0]); snprintf(a->name, sizeof a->name, "%s", f[1]);
            snprintf(a->sub, sizeof a->sub, "%s", f[2]); snprintf(a->cat, sizeof a->cat, "%s", f[3]);
            snprintf(a->ver, sizeof a->ver, "%s", f[4]); a->size = strtoul(f[5], NULL, 10); a->isize = strtoul(f[6], NULL, 10);
            snprintf(a->dev, sizeof a->dev, "%s", f[7]); snprintf(a->lic, sizeof a->lic, "%s", f[8]);
            snprintf(a->state, sizeof a->state, "%s", f[9]); snprintf(a->launch, sizeof a->launch, "%s", f[10]);
            snprintf(a->desc, sizeof a->desc, "%s", f[11]);
        }
        if (!e) break;
        p = e + 1;
    }
}
static void poll_app(App *a)
{
    if (ctl("store-status", a->id) != 0) return;
    char *f[4], *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    if (split(out, f, 4) < 3) return;
    snprintf(a->state, sizeof a->state, "%s", f[0]);
    a->done = strtoul(f[1], NULL, 10); a->total = strtoul(f[2], NULL, 10);
    snprintf(a->msg, sizeof a->msg, "%s", f[3] ? f[3] : "");
}

/* ------------------------------------------------------------------ drawing into an ARGB buffer */
static ULONG *cv;                      /* WIN_W x WIN_H */
static int clip_x0, clip_y0, clip_x1, clip_y1;
#define RGB(r, g, b) (((ULONG)(r) << 16) | ((ULONG)(g) << 8) | (ULONG)(b))
static inline void blend(int x, int y, ULONG c, int a)       /* a 0..255 */
{
    if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1 || a <= 0) return;
    ULONG *d = &cv[y * WIN_W + x];
    if (a >= 255) { *d = c; return; }
    ULONG o = *d;
    int r = ((o >> 16) & 255) + ((((c >> 16) & 255) - (int)((o >> 16) & 255)) * a) / 255;
    int g = ((o >> 8) & 255) + ((((c >> 8) & 255) - (int)((o >> 8) & 255)) * a) / 255;
    int b = (o & 255) + (((c & 255) - (int)(o & 255)) * a) / 255;
    *d = RGB(r, g, b);
}
/* Integers only: the drawing works in 1/16 pixel (Q4), no floating point. */
#define Q 16
static ULONG lerp(ULONG c0, ULONG c1, int t)                 /* t 0..256 */
{
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    int r = ((c0 >> 16) & 255) + (((int)((c1 >> 16) & 255) - (int)((c0 >> 16) & 255)) * t >> 8);
    int g = ((c0 >> 8) & 255) + (((int)((c1 >> 8) & 255) - (int)((c0 >> 8) & 255)) * t >> 8);
    int b = (c0 & 255) + (((int)(c1 & 255) - (int)(c0 & 255)) * t >> 8);
    return RGB(r, g, b);
}
static ULONG isqrt(ULONG v)
{
    ULONG r = 0, b = 1UL << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}
/* how far (Q4) a coverage distance is inside an edge -> 0..255 */
static int cov_of(int in16) { return in16 <= 0 ? 0 : in16 >= Q ? 255 : in16 * 255 / Q; }
/* coverage of the pixel (px, py) by a rounded rectangle, 0..255 */
static int rr_cov(int px, int py, int x, int y, int w, int h, int r)
{
    int X = px * Q + Q / 2, Y = py * Q + Q / 2, x0 = x * Q, y0 = y * Q, x1 = (x + w) * Q, y1 = (y + h) * Q, R = r * Q;
    int cx = X < x0 + R ? x0 + R : X > x1 - R ? x1 - R : X;
    int cy = Y < y0 + R ? y0 + R : Y > y1 - R ? y1 - R : Y;
    int in = Q;
    if (cx != X || cy != Y) {
        int dx = X - cx, dy = Y - cy;
        in = R + Q / 2 - (int)isqrt((ULONG)(dx * dx + dy * dy));
    }
    int e = X + Q / 2 - x0;                      /* the straight edges */
    if (x1 - (X - Q / 2) < e) e = x1 - (X - Q / 2);
    if (Y + Q / 2 - y0 < e) e = Y + Q / 2 - y0;
    if (y1 - (Y - Q / 2) < e) e = y1 - (Y - Q / 2);
    if (e < in) in = e;
    return cov_of(in);
}
/* a rounded rectangle; c1 != c0 gives a gradient from the top left to the bottom right */
/* the loops only visit what the clip rectangle lets through */
#define CLAMP_Y(a, b) int ya = (a) < clip_y0 ? clip_y0 : (a), yb = (b) >= clip_y1 ? clip_y1 - 1 : (b)
#define CLAMP_X(a, b) int xa = (a) < clip_x0 ? clip_x0 : (a), xb = (b) >= clip_x1 ? clip_x1 - 1 : (b)
static void rrect(int x, int y, int w, int h, int r, ULONG c0, ULONG c1, int alpha)
{
    if (w <= 0 || h <= 0) return;
    CLAMP_Y(y - 1, y + h);
    CLAMP_X(x - 1, x + w);
    for (int py = ya; py <= yb; py++) {
        /* rows away from the rounded corners: only the outermost pixels need a coverage */
        int inner = py > y + r && py < y + h - r - 1;
        for (int px = xa; px <= xb; px++) {
            int a = inner && px > x && px < x + w - 1 ? 255 : rr_cov(px, py, x, y, w, h, r);
            if (!a) continue;
            if (!inner && px > x + r && px < x + w - r - 1 && py > y && py < y + h - 1) a = 255;
            ULONG c = c0 == c1 ? c0 : lerp(c0, c1, (px - x) * 166 / w + (py - y) * 90 / h);
            blend(px, py, c, a * alpha / 255);
        }
    }
}
static void rect(int x, int y, int w, int h, ULONG c)
{
    CLAMP_Y(y, y + h - 1);
    CLAMP_X(x, x + w - 1);
    for (int py = ya; py <= yb; py++) { ULONG *d = &cv[py * WIN_W]; for (int px = xa; px <= xb; px++) d[px] = c; }
}
/* a disc, centre and radius in 1/16 pixel */
static void disc16(int cx, int cy, int r, ULONG c, int alpha)
{
    CLAMP_Y((cy - r) / Q - 1, (cy + r) / Q + 1);
    CLAMP_X((cx - r) / Q - 1, (cx + r) / Q + 1);
    for (int py = ya; py <= yb; py++)
        for (int px = xa; px <= xb; px++) {
            int dx = px * Q + Q / 2 - cx, dy = py * Q + Q / 2 - cy;
            int in = r + Q / 2 - (int)isqrt((ULONG)(dx * dx + dy * dy));
            if (in > 0) blend(px, py, c, cov_of(in) * alpha / 255);
        }
}
/* integer centre in pixels, radius in tenths of a pixel */
#define disc(x, y, r10, c, a) disc16((x) * Q + Q / 2, (y) * Q + Q / 2, (r10) * Q / 10, c, a)
/* the angle of (dx, dy) clockwise from 12 o'clock, 0..1023 */
static int angle1024(int dx, int dy)
{
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, a;
    if (!ax && !ay) return 0;
    /* 0..128 for 0..45 degrees: atan(t) ~ t * (1.0 + 0.27 * (1 - t)) * pi/4, t in 0..1 (Q8) */
    if (ay >= ax) { int t = ax * 256 / ay; a = t * (256 + 70 * (256 - t) / 256) / 512; }
    else { int t = ay * 256 / ax; a = 256 - t * (256 + 70 * (256 - t) / 256) / 512; }
    /* a is 0..256 from the vertical axis within the quadrant */
    if (dx >= 0 && dy < 0) return a;                 /* top right */
    if (dx >= 0) return 512 - a;                     /* bottom right */
    if (dy >= 0) return 512 + a;                     /* bottom left */
    return 1024 - a;                                 /* top left */
}
/* a ring from 12 o'clock clockwise over f/1000; centre, radius, thickness in pixels */
static void ring(int cx, int cy, int r, int th, int f, ULONG c)
{
    int CX = cx * Q, CY = cy * Q, mid = (r * 2 - th) * Q / 2, half = th * Q / 2, lim = f * 1024 / 1000;
    CLAMP_Y(cy - r - 1, cy + r + 1);
    CLAMP_X(cx - r - 1, cx + r + 1);
    for (int py = ya; py <= yb; py++)
        for (int px = xa; px <= xb; px++) {
            int dx = px * Q + Q / 2 - CX, dy = py * Q + Q / 2 - CY;
            int d = (int)isqrt((ULONG)(dx * dx + dy * dy)), off = d - mid;
            int in = half + Q / 2 - (off < 0 ? -off : off);
            if (in <= 0) continue;
            if (f < 1000 && angle1024(dx, dy) > lim) continue;
            blend(px, py, c, cov_of(in));
        }
}

/* ------------------------------------------------------------------ text with diskfont's Open Sans */
static struct Window *win;
static struct RastPort *rp;
enum { F_TITLE, F_HEAD, F_CARD, F_NAME, F_BODY, F_SMALL, F_BTN, F_N };
static struct TextFont *fonts[F_N];
static const struct { const char *name; UWORD size; } font_spec[F_N] = {
    { "Open Sans Bold.font", 30 }, { "Open Sans Bold.font", 20 }, { "Open Sans Bold.font", 26 },
    { "Open Sans Semibold.font", 15 }, { "Open Sans Regular.font", 14 }, { "Open Sans Semibold.font", 11 },
    { "Open Sans Bold.font", 13 },
};
static void open_fonts(void)
{
    for (int i = 0; i < F_N; i++) {
        struct TextAttr ta = { (STRPTR)font_spec[i].name, font_spec[i].size, 0, 0 };
        fonts[i] = DiskfontBase ? OpenDiskFont(&ta) : NULL;
        if (!fonts[i]) { struct TextAttr t8 = { (STRPTR)"topaz.font", 8, 0, 0 }; fonts[i] = OpenFont(&t8); }
    }
}
static struct { ULONG rgb; LONG pen; } pens[32];
static int npens;
static LONG pen_of(ULONG rgb)
{
    for (int i = 0; i < npens; i++) if (pens[i].rgb == rgb) return pens[i].pen;
    struct ColorMap *cm = win->WScreen->ViewPort.ColorMap;
    LONG p = ObtainBestPen(cm, ((rgb >> 16) & 255) * 0x01010101UL, ((rgb >> 8) & 255) * 0x01010101UL, (rgb & 255) * 0x01010101UL,
                           OBP_Precision, PRECISION_EXACT, TAG_DONE);
    if (p < 0) p = 1;
    if (npens < 32) { pens[npens].rgb = rgb; pens[npens].pen = p; npens++; }
    return p;
}
static int tw(int f, const char *s) { SetFont(rp, fonts[f]); return TextLength(rp, (STRPTR)s, strlen(s)); }
static int font_h(int f);
/* text queued while the surfaces are drawn, written after the buffer */
#define MAX_TEXT 160
static struct { int f, x, y, max; ULONG c; char s[120]; } texts[MAX_TEXT];
static int ntexts;
static void text(int f, int x, int y, ULONG c, const char *s, int max)
{
    if (ntexts >= MAX_TEXT) return;
    texts[ntexts].f = f; texts[ntexts].x = x; texts[ntexts].y = y; texts[ntexts].c = c; texts[ntexts].max = max;
    snprintf(texts[ntexts].s, sizeof texts[ntexts].s, "%s", s);
    ntexts++;
}
static void flush_text(void)
{
    SetDrMd(rp, JAM1);
    for (int i = 0; i < ntexts; i++) {
        const char *s = texts[i].s;
        int n = (int)strlen(s);
        if (texts[i].y + font_h(texts[i].f) <= clip_y0 || texts[i].y >= clip_y1 || texts[i].x >= clip_x1) continue;
        if (texts[i].x < clip_x0 && texts[i].x + tw(texts[i].f, s) <= clip_x0) continue;
        SetFont(rp, fonts[texts[i].f]);
        if (texts[i].max > 0) while (n > 0 && TextLength(rp, (STRPTR)s, n) > texts[i].max) n--;
        /* a pen of the screen's palette: AROS' text routine here ignores RPTAG_FgColor (the text
         * came out in one pen's colour), ObtainBestPen gives the nearest colour on every screen */
        SetAPen(rp, pen_of(texts[i].c));
        Move(rp, win->BorderLeft + texts[i].x, win->BorderTop + texts[i].y + rp->TxBaseline);
        Text(rp, (STRPTR)s, n);
    }
    ntexts = 0;
}
static int font_h(int f) { return fonts[f]->tf_YSize; }
/* word-wrapped paragraph; returns the height used */
static int para(int f, int x, int y, int w, ULONG c, const char *s, int max_lines)
{
    char line[120];
    int lines = 0;
    while (*s && lines < max_lines) {
        int n = 0, last = -1;
        while (s[n] && s[n] != '\n' && n < 118) {
            line[n] = s[n]; line[n + 1] = 0;
            if (s[n] == ' ') last = n;
            if (tw(f, line) > w) { if (last > 0) n = last; break; }
            n++;
        }
        memcpy(line, s, n); line[n] = 0;
        text(f, x, y + lines * (font_h(f) + 4), c, line, w);
        lines++;
        s += n;
        while (*s == ' ' || *s == '\n') s++;
    }
    return lines * (font_h(f) + 4);
}

/* ------------------------------------------------------------------ the pages */
static const char *sections[] = { "Discover", "Create", "Work", "Play", "Develop", "Updates" };
#define NSECT 6
static int section, detail = -1, scroll;
static char search[40];
static int dyn[8][4], ndyn;                /* regions of loading apps: x0 y0 x1 y1 */
static void add_dyn(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > WIN_W) x1 = WIN_W;
    if (y1 > WIN_H) y1 = WIN_H;
    if (ndyn < 8 && x1 > x0 && y1 > y0) { dyn[ndyn][0] = x0; dyn[ndyn][1] = y0; dyn[ndyn][2] = x1; dyn[ndyn][3] = y1; ndyn++; }
}
enum { A_NONE, A_SECTION, A_APP, A_GET, A_OPEN, A_REMOVE, A_BACK, A_SEARCH };
static struct { int x, y, w, h, act, arg; } hits[64];
static int nhits;
static void hit(int x, int y, int w, int h, int act, int arg)
{
    if (nhits < 64) { hits[nhits].x = x; hits[nhits].y = y; hits[nhits].w = w; hits[nhits].h = h; hits[nhits].act = act; hits[nhits].arg = arg; nhits++; }
}

#define C_BLUE RGB(0, 122, 255)
#define C_TEXT RGB(29, 29, 31)
#define C_GRAY RGB(134, 134, 139)
#define C_LINE RGB(229, 229, 234)
#define C_PILL RGB(238, 238, 240)

/* an app's icon: a rounded square in the app's colours with its first letter or Blender's orb */
static void icon(const App *a, int x, int y, int s)
{
    int r = s * 22 / 100;
    if (!strcmp(a->id, "blender")) {
        rrect(x, y, s, s, r, RGB(36, 38, 46), RGB(18, 19, 24), 255);
        int cx = x * Q + s * Q * 52 / 100, cy = y * Q + s * Q * 54 / 100;      /* in 1/16 pixel */
        disc16(cx, cy, s * Q * 30 / 100, RGB(234, 118, 0), 255);
        disc16(cx, cy, s * Q * 19 / 100, RGB(255, 255, 255), 255);
        disc16(cx + s * Q * 2 / 100, cy, s * Q * 10 / 100, RGB(38, 87, 135), 255);
        rrect(x + s * 10 / 100, y + s * 30 / 100, s * 36 / 100, s * 9 / 100, s * 45 / 1000, RGB(234, 118, 0), RGB(234, 118, 0), 255);
        rrect(x + s * 16 / 100, y + s * 45 / 100, s * 24 / 100, s * 8 / 100, s * 4 / 100, RGB(234, 118, 0), RGB(234, 118, 0), 255);
        return;
    }
    ULONG h = 0;
    for (const char *p = a->id; *p; p++) h = h * 31 + (UBYTE)*p;
    static const ULONG pal[][2] = { { RGB(90, 200, 250), RGB(0, 122, 255) }, { RGB(255, 149, 0), RGB(255, 59, 48) },
                                    { RGB(52, 199, 89), RGB(0, 150, 136) }, { RGB(175, 82, 222), RGB(88, 86, 214) } };
    rrect(x, y, s, s, r, pal[h % 4][0], pal[h % 4][1], 255);
    char l[2] = { a->name[0], 0 };
    int f = s >= 90 ? F_TITLE : F_HEAD;
    text(f, x + (s - tw(f, l)) / 2, y + (s - font_h(f)) / 2, RGB(255, 255, 255), l, 0);
}
static const char *pill_label(const App *a)
{
    if (!strcmp(a->state, "installed")) return "OPEN";
    if (!strcmp(a->state, "update")) return "UPDATE";
    if (!strcmp(a->state, "failed")) return "RETRY";
    return "GET";
}
/* the GET / OPEN button, or the progress ring while the app loads; right aligned at x+w */
static void action(const App *a, int idx, int right, int cy, int big)
{
    if (!strcmp(a->state, "loading")) {
        int f = a->total ? (int)((a->done / 1000) * 1000 / (a->total / 1000 ? a->total / 1000 : 1)) : 0;
        disc(right - 14, cy, 140, C_PILL, 255);
        ring(right - 14, cy, 14, 3, 1000, RGB(214, 214, 219));
        if (f > 0) ring(right - 14, cy, 14, 3, f, C_BLUE);
        rrect(right - 18, (int)cy - 4, 8, 8, 2, C_BLUE, C_BLUE, 255);
        return;
    }
    const char *l = pill_label(a);
    int w = tw(F_BTN, l) + (big ? 40 : 30), h = big ? 30 : 26, x = right - w, y = cy - h / 2;
    if (big) rrect(x, y, w, h, h / 2, C_BLUE, C_BLUE, 255);
    else rrect(x, y, w, h, h / 2, C_PILL, C_PILL, 255);
    text(F_BTN, x + (w - tw(F_BTN, l)) / 2, y + (h - font_h(F_BTN)) / 2, big ? RGB(255, 255, 255) : C_BLUE, l, 0);
    hit(x, y, w, h, !strcmp(a->state, "installed") ? A_OPEN : A_GET, idx);
}
static void human(ULONG b, char *o, int n)
{
    if (b >= 1000000) snprintf(o, n, "%lu.%lu MB", (unsigned long)(b / 1000000), (unsigned long)(b % 1000000 / 100000));
    else snprintf(o, n, "%lu KB", (unsigned long)((b + 999) / 1000));
}
static int matches(const App *a)
{
    if (search[0]) {
        char n[48], s[40];
        int i;
        for (i = 0; a->name[i] && i < 47; i++) n[i] = (char)tolower((UBYTE)a->name[i]);
        n[i] = 0;
        for (i = 0; search[i] && i < 39; i++) s[i] = (char)tolower((UBYTE)search[i]);
        s[i] = 0;
        return strstr(n, s) != NULL;
    }
    if (section == 0) return 1;
    if (section == 5) return !strcmp(a->state, "update") || !strcmp(a->state, "installed");
    return !strcmp(a->cat, sections[section]);
}

static void draw_sidebar(void)
{
    rect(0, 0, SIDE_W, WIN_H, RGB(236, 236, 238));
    rect(SIDE_W - 1, 0, 1, WIN_H, RGB(214, 214, 219));
    /* search field */
    rrect(14, 16, SIDE_W - 28, 28, 7, RGB(255, 255, 255), RGB(255, 255, 255), 255);
    disc(30, 30, 55, C_GRAY, 255); disc(30, 30, 38, RGB(255, 255, 255), 255);
    for (int i = 0; i < 4; i++) disc16(552 + i * 14, 552 + i * 14, 18, C_GRAY, 255);
    text(F_BODY, 44, 30 - font_h(F_BODY) / 2, search[0] ? C_TEXT : C_GRAY, search[0] ? search : "Search", SIDE_W - 66);
    hit(14, 16, SIDE_W - 28, 28, A_SEARCH, 0);
    for (int i = 0; i < NSECT; i++) {
        int y = 62 + i * 36 + (i == 5 ? 16 : 0);
        int sel = i == section && detail < 0 && !search[0];
        if (sel) rrect(10, y, SIDE_W - 20, 30, 7, RGB(210, 210, 215), RGB(210, 210, 215), 255);
        /* a small glyph per section, in the system blue */
        int gx = 30, gy = y + 15;
        switch (i) {
        case 0: disc(gx, gy, 80, C_BLUE, 255); disc(gx, gy, 58, sel ? RGB(210, 210, 215) : RGB(236, 236, 238), 255); disc(gx, gy, 22, C_BLUE, 255); break;
        case 1: rrect((int)gx - 7, (int)gy - 7, 14, 14, 3, C_BLUE, C_BLUE, 255); disc(gx, gy, 35, RGB(255, 255, 255), 255); break;
        case 2: rrect((int)gx - 8, (int)gy - 5, 16, 11, 2, C_BLUE, C_BLUE, 255); rrect((int)gx - 4, (int)gy - 8, 8, 4, 1, C_BLUE, C_BLUE, 255); break;
        case 3: rrect((int)gx - 9, (int)gy - 5, 18, 11, 5, C_BLUE, C_BLUE, 255); disc(gx - 4, gy, 16, RGB(255, 255, 255), 255); disc(gx + 4, gy, 16, RGB(255, 255, 255), 255); break;
        case 4: rrect((int)gx - 8, (int)gy - 7, 16, 14, 2, C_BLUE, C_BLUE, 255); rect((int)gx - 5, (int)gy - 2, 6, 2, RGB(255, 255, 255)); break;
        case 5: disc(gx, gy, 80, C_BLUE, 255); rect((int)gx - 1, (int)gy - 5, 2, 7, RGB(255, 255, 255)); disc(gx, gy + 4, 14, RGB(255, 255, 255), 255); break;
        }
        text(F_NAME, 48, y + 15 - font_h(F_NAME) / 2, C_TEXT, sections[i], 0);
        hit(10, y, SIDE_W - 20, 30, A_SECTION, i);
    }
    text(F_SMALL, 18, WIN_H - 30, C_GRAY, MY_SYSTEM, 0);
}

static void draw_list(void)
{
    int x0 = SIDE_W + 34, w = WIN_W - x0 - 34, y = 26 - scroll;
    text(F_TITLE, x0, y, C_TEXT, search[0] ? "Results" : sections[section], 0);
    y += font_h(F_TITLE) + 18;
    if (!host_ok) { para(F_BODY, x0, y, w, C_GRAY, host_msg, 3); return; }
    /* the featured card: the first app of the page */
    int feat = -1;
    for (int i = 0; i < napps; i++) if (matches(&apps[i])) { feat = i; break; }
    if (feat >= 0 && !search[0] && section != 5) {
        App *a = &apps[feat];
        int ch = 210;
        rrect(x0, y, w, ch, 14, RGB(255, 149, 0), RGB(175, 82, 222), 255);
        /* the darker band at the bottom, inside the card's rounded shape */
        CLAMP_Y(y + ch - 74, y + ch);
        CLAMP_X(x0 - 1, x0 + w);
        for (int py = ya; py <= yb; py++)
            for (int px = xa; px <= xb; px++) {
                int c = rr_cov(px, py, x0, y, w, ch, 14);
                if (c) blend(px, py, RGB(0, 0, 0), c * 56 / 255);
            }
        text(F_SMALL, x0 + 24, y + 22, RGB(255, 255, 255), "NOW IN THE QRX APP STORE", 0);
        text(F_CARD, x0 + 24, y + 40, RGB(255, 255, 255), a->name, w - 48);
        para(F_BODY, x0 + 24, y + 80, w - 300, RGB(255, 255, 255), a->sub, 2);
        icon(a, x0 + 22, y + ch - 62, 50);
        text(F_NAME, x0 + 84, y + ch - 56, RGB(255, 255, 255), a->name, w - 260);
        text(F_SMALL, x0 + 84, y + ch - 34, RGB(245, 245, 247), a->cat, 0);
        action(a, feat, x0 + w - 20, y + ch - 37, 0);
        if (!strcmp(a->state, "loading")) add_dyn(x0 + w - 60, y + ch - 56, x0 + w - 2, y + ch - 18);
        hit(x0, y, w, ch - 74, A_APP, feat);
        y += ch + 30;
        text(F_HEAD, x0, y, C_TEXT, "Apps for You", 0);
        y += font_h(F_HEAD) + 12;
    }
    int shown = 0;
    for (int i = 0; i < napps; i++) {
        App *a = &apps[i];
        if (!matches(a)) continue;
        shown++;
        icon(a, x0, y, 64);
        text(F_NAME, x0 + 80, y + 10, C_TEXT, a->name, w - 220);
        text(F_BODY, x0 + 80, y + 32, C_GRAY, a->sub, w - 220);
        if (!strcmp(a->state, "loading")) {
            char t[64], d[24], s[24];
            human(a->done, d, sizeof d); human(a->total, s, sizeof s);
            snprintf(t, sizeof t, "%s  %s of %s", a->msg[0] ? a->msg : "loading", d, s);
            text(F_SMALL, x0 + 80, y + 50, C_GRAY, t, w - 220);
            add_dyn(x0 + 80, y + 48, x0 + w + 2, y + 70);
        } else if (!strcmp(a->state, "failed") && a->msg[0]) text(F_SMALL, x0 + 80, y + 50, RGB(255, 59, 48), a->msg, w - 220);
        action(a, i, x0 + w, y + 32, 0);
        if (!strcmp(a->state, "loading")) add_dyn(x0 + w - 40, y + 14, x0 + w + 2, y + 50);
        hit(x0, y, w - 120, 64, A_APP, i);
        rect(x0 + 80, y + 76, w - 80, 1, C_LINE);
        y += 88;
    }
    if (!shown) text(F_BODY, x0, y, C_GRAY, search[0] ? "No apps match." : "Nothing here yet.", 0);
}

static void draw_detail(void)
{
    App *a = &apps[detail];
    int x0 = SIDE_W + 34, w = WIN_W - x0 - 34, y = 20;
    /* back */
    for (int i = 0; i < 7; i++) { disc16((x0 + 3) * Q + i * 14, (y + 9) * Q - i * 14, 21, C_BLUE, 255); disc16((x0 + 3) * Q + i * 14, (y + 9) * Q + i * 14, 21, C_BLUE, 255); }
    text(F_NAME, x0 + 18, y + 8 - font_h(F_NAME) / 2 + 1, C_BLUE, sections[section], 0);
    hit(x0, y, 120, 22, A_BACK, 0);
    y += 40;
    icon(a, x0, y, 128);
    text(F_CARD, x0 + 152, y + 8, C_TEXT, a->name, w - 160);
    text(F_BODY, x0 + 152, y + 46, C_GRAY, a->sub, w - 160);
    action(a, detail, x0 + 152 + 120, y + 96, 1);
    if (!strcmp(a->state, "installed") || !strcmp(a->state, "update")) {
        const char *l = "Remove";
        text(F_BODY, x0 + 152 + 140, y + 96 - font_h(F_BODY) / 2, RGB(255, 59, 48), l, 0);
        hit(x0 + 152 + 136, y + 84, tw(F_BODY, l) + 8, 24, A_REMOVE, detail);
    }
    if (!strcmp(a->state, "loading")) {
        char t[64], d[24], s[24];
        human(a->done, d, sizeof d); human(a->total, s, sizeof s);
        snprintf(t, sizeof t, "%s  %s of %s", a->msg[0] ? a->msg : "loading", d, s);
        text(F_SMALL, x0 + 152 + 136, y + 96 - font_h(F_SMALL) / 2, C_GRAY, t, 0);
        add_dyn(x0 + 150, y + 76, x0 + w, y + 118);
    } else if (!strcmp(a->state, "failed") && a->msg[0]) text(F_SMALL, x0 + 152 + 136, y + 96 - font_h(F_SMALL) / 2, RGB(255, 59, 48), a->msg, 0);
    y += 152;
    /* the information strip */
    rect(x0, y, w, 1, C_LINE);
    char sz[24], isz[24];
    human(a->size, sz, sizeof sz); human(a->isize, isz, sizeof isz);
    const char *k[5] = { "SIZE", "INSTALLED", "CATEGORY", "VERSION", "LICENSE" };
    const char *v[5] = { sz, isz, a->cat, a->ver, a->lic };
    int cw = w / 5;
    for (int i = 0; i < 5; i++) {
        int cx = x0 + i * cw + cw / 2;
        text(F_SMALL, cx - tw(F_SMALL, k[i]) / 2, y + 14, C_GRAY, k[i], cw - 8);
        text(F_HEAD, cx - tw(F_HEAD, v[i]) / 2, y + 34, RGB(99, 99, 102), v[i], cw - 8);
        if (i) rect(x0 + i * cw, y + 16, 1, 46, C_LINE);
    }
    y += 80;
    rect(x0, y, w, 1, C_LINE);
    y += 20;
    text(F_HEAD, x0, y, C_TEXT, "About", 0);
    y += font_h(F_HEAD) + 10;
    y += para(F_BODY, x0, y, w, C_TEXT, a->desc, 9) + 14;
    char dv[80];
    snprintf(dv, sizeof dv, "Developer: %s", a->dev);
    text(F_BODY, x0, y, C_GRAY, dv, w);
}

/* Everything into the buffer, then onto the window; with part set, only the regions of loading
 * apps (dyn, found by the last full drawing): the progress twice a second costs little. */
static void redraw_clip(int x0, int y0, int x1, int y1)
{
    nhits = 0; ntexts = 0; ndyn = 0;
    clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
    rect(0, 0, WIN_W, WIN_H, RGB(255, 255, 255));
    draw_sidebar();
    clip_x0 = x0 > SIDE_W ? x0 : SIDE_W;
    if (detail >= 0) draw_detail(); else draw_list();
    clip_x0 = x0;
    WritePixelArray(cv, x0, y0, WIN_W * 4, rp, win->BorderLeft + x0, win->BorderTop + y0, x1 - x0, y1 - y0, RECTFMT_ARGB);
    flush_text();
}
static void redraw(void) { redraw_clip(0, 0, WIN_W, WIN_H); }
static void redraw_progress(void)
{
    int n = ndyn;
    int r[8][4];
    memcpy(r, dyn, sizeof r);
    for (int i = 0; i < n; i++) redraw_clip(r[i][0], r[i][1], r[i][2], r[i][3]);
    if (!n) redraw();
}

/* ------------------------------------------------------------------ actions */
static void open_app(App *a)
{
    char cmd[200];
    BPTR dir;
    snprintf(cmd, sizeof cmd, "APPS:%s", a->id);
    dir = Lock((STRPTR)cmd, ACCESS_READ);
    if (!dir) return;
    snprintf(cmd, sizeof cmd, "%s", a->launch[0] ? a->launch : a->id);
    /* a launcher script (Blender sets up Python's paths) or the program itself, in its folder */
    BPTR old = CurrentDir(dir);
    /* a program starts with the hunk header 0x3F3; anything else is a script for Execute (the s bit
     * does not survive the archive on the host's file system) */
    BPTR test = Open((STRPTR)cmd, MODE_OLDFILE);
    if (test) {
        ULONG magic = 0;
        int script = Read(test, &magic, 4) != 4 || magic != 0x000003F3UL;
        Close(test);
        char line[240];
        snprintf(line, sizeof line, script ? "Execute APPS:%s/%s" : "APPS:%s/%s", a->id, cmd);
        SystemTags((STRPTR)line, SYS_Asynch, TRUE, SYS_Input, Open((STRPTR)"NIL:", MODE_OLDFILE),
                   SYS_Output, Open((STRPTR)"NIL:", MODE_NEWFILE), NP_CurrentDir, DupLock(dir), TAG_DONE);
    }
    CurrentDir(old);
    UnLock(dir);
}
static int any_loading(void)
{
    for (int i = 0; i < napps; i++) if (!strcmp(apps[i].state, "loading")) return 1;
    return 0;
}

static int app_main(int argc, char **argv);
/* A program started with Run gets the shell's small stack; the drawing and the C library's
 * printf want more. Run on a stack of our own. */
int main(int argc, char **argv)
{
    struct StackSwapStruct sss;
    struct StackSwapArgs ssa;
    ULONG size = 256 * 1024;
    UBYTE *stack = AllocVec(size, MEMF_ANY);
    if (!stack) return app_main(argc, argv);
    sss.stk_Lower = stack; sss.stk_Upper = (APTR)(stack + size); sss.stk_Pointer = (APTR)(stack + size);
    ssa.Args[0] = (IPTR)argc; ssa.Args[1] = (IPTR)argv;
    int r = (int)NewStackSwap(&sss, (LONG_FUNC)app_main, &ssa);
    FreeVec(stack);
    return r;
}

static int app_main(int argc, char **argv)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39);
    DiskfontBase = OpenLibrary((STRPTR)"diskfont.library", 0);
    CyberGfxBase = OpenLibrary((STRPTR)"cybergraphics.library", 0);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase) return 20;
    cv = (ULONG *)AllocVec(WIN_W * WIN_H * 4, MEMF_ANY);
    if (!cv) return 20;
    /* on the default public screen: the QRX desktop's, not the Workbench screen behind it */
    struct Screen *scr = LockPubScreen(NULL);
    win = OpenWindowTags(NULL, WA_Title, (IPTR)"App Store", WA_InnerWidth, WIN_W, WA_InnerHeight, WIN_H, WA_PubScreen, (IPTR)scr,
                         WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
                         WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_INTUITICKS | IDCMP_VANILLAKEY | IDCMP_RAWKEY | IDCMP_REFRESHWINDOW,
                         WA_SimpleRefresh, TRUE, TAG_DONE);
    if (scr) UnlockPubScreen(NULL, scr);
    if (!win) return 20;
    rp = win->RPort;
    open_fonts();
    load_catalog();
    /* QRXAppStore TEST <id>: the page of that app, and GET pressed (automatic tests) */
    /* QRXAppStore SHOW <id>: only the page of that app */
    if (argc >= 3 && (!strcmp(argv[1], "TEST") || !strcmp(argv[1], "SHOW")))
        for (int i = 0; i < napps; i++) if (!strcmp(apps[i].id, argv[2])) {
            detail = i;
            if (argv[1][0] == 'T' && ctl("store-get", apps[i].id) == 0) { snprintf(apps[i].state, sizeof apps[i].state, "loading"); apps[i].total = apps[i].size; }
        }
    redraw();
    int done = 0, typing = 0, ticks = 0;
    while (!done) {
        WaitPort(win->UserPort);
        struct IntuiMessage *m;
        int dirty = 0;
        while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG cl = m->Class;
            UWORD code = m->Code;
            int mx = m->MouseX - win->BorderLeft, my = m->MouseY - win->BorderTop;
            ReplyMsg((struct Message *)m);
            switch (cl) {
            case IDCMP_CLOSEWINDOW: done = 1; break;
            case IDCMP_REFRESHWINDOW: BeginRefresh(win); EndRefresh(win, TRUE); dirty = 1; break;
            case IDCMP_INTUITICKS:
                if (++ticks >= 5 && any_loading()) {        /* twice a second */
                    ticks = 0;
                    int finished = 0;
                    for (int i = 0; i < napps; i++) if (!strcmp(apps[i].state, "loading")) { poll_app(&apps[i]); finished |= strcmp(apps[i].state, "loading") != 0; }
                    dirty = finished ? 1 : 2;
                }
                break;
            case IDCMP_VANILLAKEY:
                if (!typing) break;
                if (code == 8) { int l = (int)strlen(search); if (l) search[l - 1] = 0; }
                else if (code == 13 || code == 27) typing = 0;
                else if (code >= 32 && strlen(search) < sizeof search - 1) { int l = (int)strlen(search); search[l] = (char)code; search[l + 1] = 0; }
                detail = -1; scroll = 0; dirty = 1;
                break;
            case IDCMP_MOUSEBUTTONS:
                if (code != SELECTDOWN) break;
                for (int i = nhits - 1; i >= 0; i--) {
                    if (mx < hits[i].x || my < hits[i].y || mx >= hits[i].x + hits[i].w || my >= hits[i].y + hits[i].h) continue;
                    int arg = hits[i].arg;
                    typing = hits[i].act == A_SEARCH;
                    switch (hits[i].act) {
                    case A_SECTION: section = arg; detail = -1; search[0] = 0; scroll = 0; break;
                    case A_APP: detail = arg; break;
                    case A_BACK: detail = -1; break;
                    case A_GET:
                        if (ctl("store-get", apps[arg].id) == 0) { snprintf(apps[arg].state, sizeof apps[arg].state, "loading"); apps[arg].done = 0; apps[arg].total = apps[arg].size; apps[arg].msg[0] = 0; }
                        else { snprintf(apps[arg].state, sizeof apps[arg].state, "failed"); snprintf(apps[arg].msg, sizeof apps[arg].msg, "%.60s", out); }
                        break;
                    case A_OPEN: open_app(&apps[arg]); break;
                    case A_REMOVE: if (ctl("store-remove", apps[arg].id) == 0) poll_app(&apps[arg]); break;
                    }
                    dirty = 1;
                    break;
                }
                break;
            }
        }
        if (dirty == 2 && !done) redraw_progress();
        else if (dirty && !done) redraw();
    }
    for (int i = 0; i < npens; i++) ReleasePen(win->WScreen->ViewPort.ColorMap, pens[i].pen);
    CloseWindow(win);
    for (int i = 0; i < F_N; i++) if (fonts[i]) CloseFont(fonts[i]);
    FreeVec(cv);
    if (DiskfontBase) CloseLibrary(DiskfontBase);
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
