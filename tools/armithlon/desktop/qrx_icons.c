/* QRXDesktop icon drawing. Included by the desktop source at build time, see
 * tools/armithlon/build-desktop.py.
 *
 * Amiga and AROS icons go through icon.library, which reads classic planar
 * images, NewIcons, GlowIcons and AROS PNG icons. Windows programs and .ico files
 * have no .info, so their icons come from the file itself. */
#include "qrx_wicon.h"
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>

struct Library *CyberGfxBase = NULL;

#define QRX_ICON_CACHE 128
struct QRXIconSlot {
    char *path;
    struct DiskObject *dobj;
    uint32_t *argb;          /* Windows icon, QRX_WICON_SIZE square */
    struct Screen *laid_out;
    WORD w, h;               /* drawn size; also the size of argb */
    UBYTE generated;
};
static struct QRXIconSlot g_icon_cache[QRX_ICON_CACHE];
static int g_icon_next = 0;

static int qrx_icon_dos_read(void *ctx, uint32_t off, void *buf, uint32_t n) {
    BPTR fh = (BPTR)ctx;
    if (Seek(fh, (LONG)off, OFFSET_BEGINNING) < 0) return 0;
    return Read(fh, buf, (LONG)n) == (LONG)n;
}

static uint32_t *qrx_icon_load_windows(const char *path) {
    int is_ico = qrx_ci_ends(path, ".ico") || qrx_ci_ends(path, ".cur");
    uint32_t *argb;
    BPTR fh;
    int ok;
    if (!is_ico && !qrx_ci_ends(path, ".exe") && !qrx_ci_ends(path, ".dll") &&
        !qrx_ci_ends(path, ".scr")) return NULL;
    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) return NULL;
    argb = (uint32_t *)malloc(QRX_WICON_SIZE * QRX_WICON_SIZE * 4);
    ok = argb && (is_ico ? qrx_wicon_from_ico(qrx_icon_dos_read, (void *)fh, argb)
                         : qrx_wicon_from_pe(qrx_icon_dos_read, (void *)fh, argb));
    Close(fh);
    if (ok) return argb;
    free(argb);
    return NULL;
}

static void qrx_icon_slot_clear(struct QRXIconSlot *s) {
    if (s->dobj) FreeDiskObject(s->dobj);
    free(s->argb);
    free(s->path);
    memset(s, 0, sizeof(*s));
}

static void qrx_icon_cache_free(void) {
    int i;
    for (i = 0; i < QRX_ICON_CACHE; i++) qrx_icon_slot_clear(&g_icon_cache[i]);
    g_icon_next = 0;
}

/* Classic planar icons assume the Workbench colours in pens 0-7. A deep screen's pens
 * hold something else (the desktop gives low pens to its wallpaper), so the image is
 * converted here. Colour 0 is the icon background: transparent. */
static const uint32_t qrx_wb_colors[8] = {
    0x00000000u, 0xFF000000u, 0xFFFFFFFFu, 0xFF3B67A2u,
    0xFF7B7B7Bu, 0xFFAFAFAFu, 0xFFAA907Cu, 0xFFFFA997u
};
static uint32_t *qrx_icon_classic_argb(struct DiskObject *o, WORD *pw, WORD *ph) {
    struct Image *img = (struct Image *)o->do_Gadget.GadgetRender;
    int x, y, d, row, plane = 0;
    uint32_t *out;
    const UWORD *data;
    if (!(o->do_Gadget.Flags & GFLG_GADGIMAGE) || !img || !img->ImageData) return NULL;
    if (img->Width < 1 || img->Height < 1 || img->Width > 256 || img->Height > 256) return NULL;
    out = (uint32_t *)malloc((size_t)img->Width * img->Height * 4);
    if (!out) return NULL;
    data = img->ImageData;
    row = (img->Width + 15) / 16;
    for (y = 0; y < img->Height; y++) {
        for (x = 0; x < img->Width; x++) {
            unsigned c = 0;
            plane = 0;
            for (d = 0; d < 8; d++) {
                if (img->PlanePick & (1u << d)) {
                    if (plane < img->Depth) {
                        const UWORD *w = data + plane * row * img->Height + y * row + x / 16;
                        if (*w & (0x8000u >> (x & 15))) c |= 1u << d;
                    }
                    plane++;
                } else if (img->PlaneOnOff & (1u << d)) c |= 1u << d;
            }
            out[y * img->Width + x] = qrx_wb_colors[c & 7];
        }
    }
    *pw = img->Width;
    *ph = img->Height;
    return out;
}

/* A real .info wins; then the file's own Windows icon; then icon.library's
 * default for the object type. */
static struct QRXIconSlot *qrx_icon_slot(const char *path) {
    struct QRXIconSlot *s;
    int i;
    for (i = 0; i < QRX_ICON_CACHE; i++)
        if (g_icon_cache[i].path && !strcmp(g_icon_cache[i].path, path)) return &g_icon_cache[i];
    s = &g_icon_cache[g_icon_next];
    g_icon_next = (g_icon_next + 1) % QRX_ICON_CACHE;
    qrx_icon_slot_clear(s);
    s->path = strdup(path);
    if (!s->path) return NULL;
    s->dobj = GetDiskObject((STRPTR)path);
    if (!s->dobj) {
        s->argb = qrx_icon_load_windows(path);
        if (s->argb) s->w = s->h = QRX_WICON_SIZE;
        else { s->dobj = GetDiskObjectNew((STRPTR)path); s->generated = 1; }
    }
    /* Glow, NewIcon and PNG icons carry colour data of their own; only a classic
     * icon has neither palette mapped nor ARGB images. */
    if (s->dobj && IconBase->lib_Version >= 44) {
        const void *chunky = NULL, *argb = NULL;
        IconControl(s->dobj, ICONCTRLA_GetImageData1, (IPTR)&chunky,
                    ICONCTRLA_GetARGBImageData1, (IPTR)&argb, TAG_DONE);
        if (!chunky && !argb) s->argb = qrx_icon_classic_argb(s->dobj, &s->w, &s->h);
    }
    return s;
}

static int qrx_icon_draw_argb(struct RastPort *rp, const uint32_t *argb, WORD sw, WORD sh,
                              WORD x, WORD y, WORD maxw, WORD maxh) {
    static uint32_t line[64 * 64];
    int w = sw, h = sh, ix, iy;
    if (!CyberGfxBase) return 0;
    if (GetBitMapAttr(rp->BitMap, BMA_DEPTH) <= 8) return 0;
    if (maxw > 64) maxw = 64;
    if (maxh > 64) maxh = 64;
    if (w > maxw) { h = h * maxw / w; w = maxw; }
    if (h > maxh) { w = w * maxh / h; h = maxh; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    x += (maxw - w) / 2;
    y += (maxh - h) / 2;
    ReadPixelArray(line, 0, 0, w * 4, rp, x, y, w, h, RECTFMT_ARGB);
    for (iy = 0; iy < h; iy++) {
        int sy0 = iy * sh / h, sy1 = (iy + 1) * sh / h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (ix = 0; ix < w; ix++) {
            int sx0 = ix * sw / w, sx1 = (ix + 1) * sw / w, sx, sy;
            uint32_t d = line[iy * w + ix], a = 0, r = 0, g = 0, b = 0, n = 0, out;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            /* Averaged weighted by alpha: transparent pixels add no colour of their own. */
            for (sy = sy0; sy < sy1; sy++) for (sx = sx0; sx < sx1; sx++) {
                uint32_t s = argb[sy * sw + sx], sa = s >> 24;
                a += sa; r += ((s >> 16) & 255) * sa; g += ((s >> 8) & 255) * sa; b += (s & 255) * sa; n++;
            }
            if (a) {
                uint32_t ca = a / n, ia = 255 - ca;
                out = 0xFF000000u | (((r / a * ca + ((d >> 16) & 255) * ia) / 255) << 16) |
                      (((g / a * ca + ((d >> 8) & 255) * ia) / 255) << 8) | ((b / a * ca + (d & 255) * ia) / 255);
            } else out = d;
            line[iy * w + ix] = out;
        }
    }
    WritePixelArray(line, 0, 0, w * 4, rp, x, y, w, h, RECTFMT_ARGB);
    return 1;
}

/* Icons larger than the box are drawn off screen and shrunk. */
static int qrx_icon_draw_scaled(struct RastPort *rp, struct DiskObject *o, WORD x, WORD y,
                                WORD iw, WORD ih, WORD maxw, WORD maxh, UBYTE bg) {
    struct BitMap *src, *dst;
    struct RastPort srp, drp;
    struct BitScaleArgs a;
    WORD w = maxw, h = (WORD)((LONG)ih * maxw / iw);
    ULONG depth = GetBitMapAttr(rp->BitMap, BMA_DEPTH);
    int done = 0;
    if (h > maxh) { h = maxh; w = (WORD)((LONG)iw * maxh / ih); }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    src = AllocBitMap(iw, ih, depth, BMF_CLEAR, rp->BitMap);
    dst = AllocBitMap(w, h, depth, BMF_CLEAR, rp->BitMap);
    if (src && dst) {
        InitRastPort(&srp); srp.BitMap = src;
        InitRastPort(&drp); drp.BitMap = dst;
        /* Transparent parts show what is behind the icon; shrinking keeps it uniform. */
        if (CyberGfxBase && depth > 8) {
            FillPixelArray(&srp, 0, 0, iw, ih, ReadRGBPixel(rp, x, y));
        } else {
            SetAPen(&srp, bg);
            RectFill(&srp, 0, 0, iw - 1, ih - 1);
        }
        DrawIconState(&srp, o, NULL, 0, 0, IDS_NORMAL, ICONDRAWA_Borderless, TRUE,
                      ICONDRAWA_EraseBackground, FALSE, TAG_DONE);
        memset(&a, 0, sizeof(a));
        a.bsa_SrcWidth = iw; a.bsa_SrcHeight = ih;
        a.bsa_XSrcFactor = iw; a.bsa_YSrcFactor = ih;
        a.bsa_XDestFactor = w; a.bsa_YDestFactor = h;
        a.bsa_SrcBitMap = src; a.bsa_DestBitMap = dst;
        BitMapScale(&a);
        ClipBlit(&drp, 0, 0, rp, x, y, w, h, 0xC0);
        done = 1;
    }
    if (dst) FreeBitMap(dst);
    if (src) FreeBitMap(src);
    return done;
}

/* Returns 1 when something was drawn. bg is the pen behind a shrunk icon. */
static int qrx_icon_draw(struct RastPort *rp, const char *path, WORD x, WORD y,
                         WORD maxw, WORD maxh, UBYTE bg) {
    struct QRXIconSlot *s;
    if (!IconBase) return 0;
    s = qrx_icon_slot(path);
    if (!s) return 0;
    if (s->argb) {
        if (qrx_icon_draw_argb(rp, s->argb, s->w, s->h, x, y, maxw, maxh)) return 1;
        if (!s->dobj) return 0;
        s->w = s->h = 0;
    }
    if (!s->dobj) return 0;
    if (IconBase->lib_Version >= 44 && g_screen) {
        if (s->laid_out != g_screen) {
            struct DrawInfo *di = GetScreenDrawInfo(g_screen);
            struct Rectangle r;
            LayoutIcon(s->dobj, g_screen, TAG_DONE);
            memset(&r, 0, sizeof(r));
            if (GetIconRectangle(rp, s->dobj, NULL, &r, ICONDRAWA_Borderless, TRUE,
                                 ICONDRAWA_DrawInfo, (IPTR)di, TAG_DONE)) {
                s->w = r.MaxX - r.MinX + 1;
                s->h = r.MaxY - r.MinY + 1;
            }
            if (di) FreeScreenDrawInfo(g_screen, di);
            s->laid_out = g_screen;
        }
        if (s->w > 0 && s->h > 0) {
            if (s->w > maxw || s->h > maxh)
                return qrx_icon_draw_scaled(rp, s->dobj, x, y, s->w, s->h, maxw, maxh, bg);
            DrawIconState(rp, s->dobj, NULL, x + (maxw - s->w) / 2, y + (maxh - s->h) / 2, IDS_NORMAL,
                          ICONDRAWA_Borderless, TRUE, ICONDRAWA_EraseBackground, FALSE, TAG_DONE);
            return 1;
        }
    }
    {
        struct Image *img = (struct Image *)s->dobj->do_Gadget.GadgetRender;
        if ((s->dobj->do_Gadget.Flags & GFLG_GADGIMAGE) && img && img->Width <= maxw && img->Height <= maxh) {
            DrawImage(rp, img, x, y);
            return 1;
        }
    }
    return 0;
}

/* ---- The look of the desktop: icons per design and the wallpaper. The files come from
 * gen-assets.swift. When they are missing, the desktop draws what it always did. */
#define QRX_THEME_ICON 48
enum { QK_DRAWER, QK_DISK, QK_RAM, QK_APPS, QK_FILE, QK_PROGRAM, QK_WINDOWS, QK_COUNT };
static const char *const qrx_kind_names[QK_COUNT] = { "drawer", "disk", "ram", "apps", "file", "program", "windows" };
static uint32_t *g_theme_icons[4][QK_COUNT];
static UBYTE g_theme_tried[4][QK_COUNT];

static int qrx_theme_index(void) {
    switch (g_look) {
    case LOOK_WINDOWS: return 1;
    case LOOK_MACOS: return 2;
    case LOOK_AMIGAOS: return 3;
    default: return 0;
    }
}
static const uint32_t *qrx_theme_icon(int kind) {
    static const char *const dirs[4] = { "qrx", "windows", "mac", "amiga" };
    int t = qrx_theme_index();
    if (!g_theme_tried[t][kind]) {
        char path[96];
        BPTR fh;
        uint32_t *px = (uint32_t *)malloc(QRX_THEME_ICON * QRX_THEME_ICON * 4);
        g_theme_tried[t][kind] = 1;
        snprintf(path, sizeof(path), "SYS:QRX/Icons/%s/%s.argb", dirs[t], qrx_kind_names[kind]);
        fh = Open((STRPTR)path, MODE_OLDFILE);
        if (fh && px) {
            char head[12];
            if (Read(fh, head, 12) == 12 && !memcmp(head, "QRXI", 4) &&
                Read(fh, px, QRX_THEME_ICON * QRX_THEME_ICON * 4) == QRX_THEME_ICON * QRX_THEME_ICON * 4) {
                g_theme_icons[t][kind] = px;
                px = NULL;
            }
        }
        if (fh) Close(fh);
        free(px);
    }
    return g_theme_icons[t][kind];
}
static int qrx_theme_icon_draw(struct RastPort *rp, int kind, WORD x, WORD y, WORD maxw, WORD maxh) {
    const uint32_t *px = qrx_theme_icon(kind);
    return px ? qrx_icon_draw_argb(rp, px, QRX_THEME_ICON, QRX_THEME_ICON, x, y, maxw, maxh) : 0;
}
static void qrx_theme_icons_free(void) {
    int t, k;
    for (t = 0; t < 4; t++) for (k = 0; k < QK_COUNT; k++) { free(g_theme_icons[t][k]); g_theme_icons[t][k] = NULL; g_theme_tried[t][k] = 0; }
}

/* An icon of a file or drawer: designs draw drawers and files without a picture of their own. */
static int qrx_object_icon_draw(struct RastPort *rp, const char *path, int is_dir, WORD x, WORD y, WORD maxw, WORD maxh, UBYTE bg) {
    struct QRXIconSlot *s;
    if (is_dir) {
        if (qrx_theme_icon_draw(rp, QK_DRAWER, x, y, maxw, maxh)) return 1;
        return qrx_icon_draw(rp, path, x, y, maxw, maxh, bg);
    }
    s = IconBase ? qrx_icon_slot(path) : NULL;
    if (s && s->generated) {
        int kind = qrx_ci_ends(path, ".exe") || qrx_ci_ends(path, ".dll") ? QK_WINDOWS : QK_FILE;
        if (qrx_theme_icon_draw(rp, kind, x, y, maxw, maxh)) return 1;
    }
    return qrx_icon_draw(rp, path, x, y, maxw, maxh, bg);
}

/* ---- wallpaper */
static struct BitMap *g_qw_bitmap;
static int g_qw_failed;

static void qrx_wallpaper_forget(void) {
    if (g_qw_bitmap) { FreeBitMap(g_qw_bitmap); g_qw_bitmap = NULL; }
    g_qw_failed = 0;
}
static BPTR qrx_wallpaper_open(int *w, int *h) {
    static const short sizes[][2] = { { 1920, 1080 }, { 1280, 1024 }, { 1280, 720 }, { 1024, 768 } };
    char path[128];
    BPTR fh;
    int i;
    /* The exact size, else the one whose shape is nearest. */
    snprintf(path, sizeof(path), "SYS:QRX/Wallpapers/%s-%dx%d.qrxwp", g_wallpaper, (int)g_screen_width, (int)g_screen_height);
    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (fh) { *w = g_screen_width; *h = g_screen_height; return fh; }
    {
        long want = (long)g_screen_width * 1000 / (g_screen_height ? g_screen_height : 1), best = 0x7FFFFFFF;
        int pick = -1;
        for (i = 0; i < 4; i++) {
            long d = (long)sizes[i][0] * 1000 / sizes[i][1] - want;
            if (d < 0) d = -d;
            if (d < best || (d == best && sizes[i][0] >= g_screen_width)) { best = d; pick = i; }
        }
        snprintf(path, sizeof(path), "SYS:QRX/Wallpapers/%s-%dx%d.qrxwp", g_wallpaper, (int)sizes[pick][0], (int)sizes[pick][1]);
        *w = sizes[pick][0]; *h = sizes[pick][1];
        return Open((STRPTR)path, MODE_OLDFILE);
    }
}
static int qrx_wallpaper_load(struct RastPort *rp) {
    int iw = 0, ih = 0, y;
    BPTR fh;
    uint8_t *pix, *line;
    struct RastPort crp;
    if (!CyberGfxBase || g_qw_failed || !g_wallpaper[0] || !strcmp(g_wallpaper, "Classic")) return 0;
    if (GetBitMapAttr(rp->BitMap, BMA_DEPTH) <= 8) return 0;
    g_qw_failed = 1;
    fh = qrx_wallpaper_open(&iw, &ih);
    if (!fh) return 0;
    pix = (uint8_t *)malloc((size_t)iw * ih * 3 + 12);
    line = (uint8_t *)malloc((size_t)g_screen_width * 3);
    g_qw_bitmap = AllocBitMap(g_screen_width, g_screen_height, GetBitMapAttr(rp->BitMap, BMA_DEPTH), BMF_MINPLANES, rp->BitMap);
    if (pix && line && g_qw_bitmap && Read(fh, pix, (LONG)iw * ih * 3 + 12) == (LONG)iw * ih * 3 + 12) {
        const uint8_t *data = pix + 12;
        InitRastPort(&crp);
        crp.BitMap = g_qw_bitmap;
        if (iw == g_screen_width && ih == g_screen_height) {
            WritePixelArray((APTR)data, 0, 0, (UWORD)(iw * 3), &crp, 0, 0, (UWORD)iw, (UWORD)ih, RECTFMT_RGB);
        } else {
            /* Fills the screen and keeps the shape: what sticks out is cut off. */
            long sx = (long)iw * 4096 / g_screen_width, sy = (long)ih * 4096 / g_screen_height;
            long sc = sx < sy ? sx : sy;
            long ox = ((long)iw * 4096 - sc * g_screen_width) / 2, oy = ((long)ih * 4096 - sc * g_screen_height) / 2;
            for (y = 0; y < g_screen_height; y++) {
                const uint8_t *row = data + ((oy + sc * y) >> 12) * iw * 3;
                int x;
                for (x = 0; x < g_screen_width; x++) {
                    const uint8_t *p = row + ((ox + sc * x) >> 12) * 3;
                    line[x * 3] = p[0]; line[x * 3 + 1] = p[1]; line[x * 3 + 2] = p[2];
                }
                WritePixelArray(line, 0, 0, (UWORD)(g_screen_width * 3), &crp, 0, (UWORD)y, (UWORD)g_screen_width, 1, RECTFMT_RGB);
            }
        }
        g_qw_failed = 0;
    } else if (g_qw_bitmap) { FreeBitMap(g_qw_bitmap); g_qw_bitmap = NULL; }
    free(pix);
    free(line);
    Close(fh);
    return g_qw_bitmap != NULL;
}
static int qrx_wallpaper_draw(struct RastPort *rp) {
    if (!g_qw_bitmap && !qrx_wallpaper_load(rp)) return 0;
    BltBitMapRastPort(g_qw_bitmap, 0, 0, rp, 0, 0, g_screen_width, g_screen_height, 0xC0);
    return 1;
}

/* ---- the pointer is the Mac's: the door tells the host which one to show */
static void qrx_pointer_apply(void) {
    static ULONG q[32];
    char v[8];
    if (GetVar((STRPTR)"QRX86_HOST", (STRPTR)v, sizeof(v), 0) < 0) return;
    q[0] = 6;
    q[3] = (ULONG)g_pointer;
    *(volatile ULONG *)0xFF00088CUL = (ULONG)(IPTR)q;
}

/* ---- QRXDesktopPrefs talks to the running desktop through this port */
struct QRXDesktopLook { struct Message msg; char wallpaper[64]; LONG pointer; };
static struct MsgPort *g_look_port;
static void qrx_look_port_open(void) {
    g_look_port = CreateMsgPort();
    if (!g_look_port) return;
    g_look_port->mp_Node.ln_Name = (char *)"QRXDESKTOP";
    g_look_port->mp_Node.ln_Pri = 0;
    AddPort(g_look_port);
}
static void qrx_look_port_close(void) {
    if (!g_look_port) return;
    RemPort(g_look_port);
    DeleteMsgPort(g_look_port);
    g_look_port = NULL;
}
/* Returns 1 when the look changed and the desktop must be drawn again. */
static int qrx_look_port_handle(void) {
    struct QRXDesktopLook *m;
    int changed = 0;
    if (!g_look_port) return 0;
    while ((m = (struct QRXDesktopLook *)GetMsg(g_look_port)) != NULL) {
        strncpy(g_wallpaper, m->wallpaper, sizeof(g_wallpaper) - 1);
        g_wallpaper[sizeof(g_wallpaper) - 1] = 0;
        g_pointer = (int)m->pointer;
        ReplyMsg(&m->msg);
        qrx_wallpaper_forget();
        qrx_pointer_apply();
        changed = 1;
    }
    return changed;
}
