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
        int sy = iy * sh / h;
        for (ix = 0; ix < w; ix++) {
            uint32_t s = argb[sy * sw + ix * sw / w], d = line[iy * w + ix];
            uint32_t a = s >> 24, ia = 255 - a, c, out = 0xFF000000u;
            int shift;
            for (shift = 0; shift < 24; shift += 8) {
                c = (((s >> shift) & 255) * a + ((d >> shift) & 255) * ia + 127) / 255;
                out |= c << shift;
            }
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
