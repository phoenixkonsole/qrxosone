/* QRX OS One 22.5.12 - qrx-win32: DirectX 3 HLE (DirectDraw, DirectSound, DirectInput).
 *
 * COM objects live in guest memory as { vtable pointer, host index }; every
 * vtable slot is an HLE thunk (stdcall, "this" first).  Surfaces are plain guest
 * memory, so Lock() hands the program a pointer it can write to directly; the
 * primary surface is shown in the cooperative-level window (8-bit through the
 * palette, 16-bit RGB565, 32-bit).  DirectSound buffers keep their data and a
 * play cursor that advances with the clock (no audio output yet); DirectInput
 * reports the keyboard/mouse state of the window system.
 *
 * Implemented: IDirectDraw, IDirectDraw2, IDirectDrawSurface(1-3), IDirectDrawPalette,
 * IDirectDrawClipper, IDirectSound, IDirectSoundBuffer, IDirectInputA,
 * IDirectInputDeviceA.  Not implemented: Direct3D, overlays, DirectDraw4/7. */
#include <stdlib.h>
#include <string.h>
#include "qrxwin.h"

#define FN(name) static uint32_t name(qwin *w, uint32_t *a)
#define C (&w->cpu)
#define RD32(x) x86_rd32(C, (x))
#define WR16(x, v) x86_wr16(C, (x), (uint16_t)(v))
#define WR8(x, v) x86_wr8(C, (x), (uint8_t)(v))
#define WR32(x, v) x86_wr32(C, (x), (v))
#define UNUSED (void)w; (void)a

#define DD_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_FAIL_ 0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_NOTFOUND 0x887600FFu
#define DDERR_GENERIC E_FAIL_
#define DDERR_NOPALETTEATTACHED 0x8876023Cu

/* ------------------------------------------------------------------ host objects */
enum { OB_FREE, OB_DD, OB_SURF, OB_PAL, OB_CLIP, OB_DS, OB_DSB, OB_DI, OB_DID };
typedef struct dxobj {
    int type, refs;
    uint32_t guest;                  /* guest COM object (vtable, index) */
    /* DirectDraw */
    uint32_t hwnd;
    int mw, mh, mbpp, exclusive;
    /* surface */
    int sw, sh, bpp, pitch, primary, back, offscreen;
    uint32_t mem, caps;
    uint32_t rm, gm, bm, am;         /* pixel format masks, 0: the default for the depth */
    uint32_t pal_mirror, gen;        /* host side: palette in guest memory, content generation */
    int attached;                    /* next surface of the flip chain */
    int nback;                       /* primary: back buffers in its chain */
    int pal;                         /* palette object index or -1 */
    int ck_on; uint32_t ck_lo, ck_hi;
    uint32_t ownmem;                 /* SetSurfaceDesc: the program's memory replaced ours (keep ours to free) */
    int rtg, fb;                     /* RTG direct: mem is display buffer fb (QW_FB_BASE + fb * QW_FB_STRIDE) */
    uint32_t dc;                     /* GetDC: memory DC, its bitmap and the original pixels */
    uint32_t dcbm, *dcorig;
    int dd;                          /* owning DirectDraw object */
    /* palette */
    uint32_t ent[256];               /* 0x00RRGGBB */
    /* DirectSound buffer */
    uint32_t size, rate, align, bps, chans, bits;
    int playing, looping;
    uint64_t t0; uint32_t pos0;
    int primbuf;
    int32_t vol, pan; uint32_t freq;
    /* DirectInput device */
    int kind;                        /* 1 keyboard, 2 mouse */
    int lastx, lasty;
    uint8_t prevkeys[256];
    uint32_t event;                  /* SetEventNotification handle */
    uint8_t prevbtn[3];
} dxobj;
#define DX_MAX 128
static dxobj g_o[DX_MAX];
static uint32_t g_vt[24];           /* guest vtables per interface */
enum { VT_DD, VT_DD2, VT_SURF, VT_PAL, VT_CLIP, VT_DS, VT_DSB, VT_DI, VT_DID };

static int obj_new(qwin *w, int type, int vt)
{
    int i;
    for (i = 0; i < DX_MAX; i++) if (g_o[i].type == OB_FREE) {
        memset(&g_o[i], 0, sizeof(g_o[i]));
        g_o[i].type = type; g_o[i].refs = 1; g_o[i].pal = -1; g_o[i].attached = -1; g_o[i].dd = -1;
        g_o[i].guest = qw_alloc(w, 64);     /* surfaces publish their layout to the host: see publish() */
        WR32(g_o[i].guest, g_vt[vt]);
        WR32(g_o[i].guest + 4, (uint32_t)i);
        return i;
    }
    qw_log(w, "DirectX: too many objects");
    return -1;
}
static dxobj *ob(qwin *w, uint32_t guest, int type)
{
    uint32_t i;
    if (!guest) return NULL;
    i = RD32(guest + 4);
    if (i >= DX_MAX || g_o[i].type != type) return NULL;
    return &g_o[i];
}
static int idx_of(const dxobj *o) { return (int)(o - g_o); }

/* ------------------------------------------------------------------ RTG direct */
/* In exclusive 8-bit mode the primary surface (and its back buffer) are the display
 * buffers of the own screen: their guest pages are mapped onto the frame buffer memory
 * the backend hands out (on AROS: the RTG card's screen bitmaps), so the program's
 * Lock/Blt writes land in video memory without any copy, and Flip is a page flip.
 * Whole pages are mapped; the last, partial page of a buffer is an ordinary guest
 * page copied into the frame buffer when the buffer is shown. */
static struct {
    qwnd *t;
    int n, pitch, sh;
    uint32_t bytes, full;            /* buffer size, part of it in whole pages */
    uint8_t *host[2];
} g_fb;
static uint32_t fb_guest(int i) { return QW_FB_BASE + (uint32_t)i * QW_FB_STRIDE; }
static int fb_map(qwin *w, int i)
{
    uint32_t g = fb_guest(i);
    if (g_fb.full && !x86_map_ext(C, g, g_fb.full, g_fb.host[i], 1)) return 0;
    if (g_fb.bytes > g_fb.full && !x86_is_mapped(C, g + g_fb.full) && !x86_map(C, g + g_fb.full, X86_PAGE_SIZE, 1)) return 0;
    return 1;
}
static void fb_detach(qwin *w, int close)
{
    int i, k;
    qwnd *t = g_fb.t;
    if (!g_fb.n) return;
    for (k = 0; k < DX_MAX; k++) {                   /* surfaces keep their pixels in ordinary memory */
        dxobj *s = &g_o[k];
        uint32_t m;
        uint8_t *tmp;
        if (s->type != OB_SURF || !s->rtg) continue;
        m = qw_alloc(w, g_fb.bytes + 16u);
        tmp = (uint8_t *)malloc(g_fb.bytes);
        if (m && tmp && x86_read_mem(C, s->mem, tmp, g_fb.bytes)) x86_write_mem(C, m, tmp, g_fb.bytes);
        free(tmp);
        s->mem = m; s->rtg = 0;
    }
    for (i = 0; i < g_fb.n; i++) x86_unmap(C, fb_guest(i), g_fb.full + X86_PAGE_SIZE);
    g_fb.n = 0; g_fb.t = NULL;
    w->fb_lost = NULL;
    if (close && t && w->be->fb_close) w->be->fb_close(w, t);
    if (w->verbose) qw_log(w, "DirectDraw: RTG direct buffers released");
}
static void fb_lost(qwin *w, qwnd *t) { (void)t; fb_detach(w, 0); }
static qwnd *dd_top(qwin *w, dxobj *dd)
{
    qwnd *t = dd && dd->hwnd ? W(w, dd->hwnd) : NULL;
    return t ? win_top(w, t) : NULL;
}
static void fb_attach(qwin *w, dxobj *dd, dxobj *prim)
{
    qwnd *t = dd_top(w, dd);
    const char *e = getenv("QRX_DX_RTG");
    uint8_t *buf[2];
    int need = prim->attached >= 0 ? 2 : 1, pitch = 0, n, i;
    if (e && e[0] == '0') return;
    if (prim->nback > 1) return;                      /* the two RTG pages hold one back buffer */
    if (g_fb.n || !t || !t->be || !w->be->fb_open || !dd->exclusive || prim->bpp != 8 ||
        t->fs_w != prim->sw || t->fs_h != prim->sh) return;
    n = w->be->fb_open(w, t, need, buf, &pitch);
    if (n != need || pitch < prim->sw) {
        if (n > 0) w->be->fb_close(w, t);
        if (w->verbose) qw_log(w, "DirectDraw: no RTG direct display buffers, frames are copied");
        return;
    }
    g_fb.t = t; g_fb.n = n; g_fb.pitch = pitch; g_fb.sh = prim->sh;
    g_fb.bytes = (uint32_t)pitch * (uint32_t)prim->sh;
    g_fb.full = g_fb.bytes & ~X86_PAGE_MASK;
    for (i = 0; i < n; i++) {
        dxobj *s = i ? &g_o[prim->attached] : prim;
        g_fb.host[i] = buf[i];
        memset(buf[i], 0, g_fb.bytes);
        if (!fb_map(w, i)) { qw_log(w, "DirectDraw: mapping the display buffers failed"); fb_detach(w, 1); return; }
        qw_free(w, s->mem);
        s->mem = fb_guest(i); s->pitch = pitch; s->rtg = 1; s->fb = i;
    }
    w->fb_lost = fb_lost;
    if (w->verbose) qw_log(w, "DirectDraw: RTG direct, %d display buffer(s) %dx%d pitch %d mapped at %08x", n, prim->sw, prim->sh, pitch, fb_guest(0));
}
/* show the primary's display buffer: tail page, palette, page flip */
static void fb_show(qwin *w, dxobj *prim, const uint32_t *pal)
{
    static uint32_t grey[256];
    int i = prim->fb;
    uint8_t *a, *tail;
    if (g_fb.bytes > g_fb.full && (tail = x86_host_ptr(C, fb_guest(i) + g_fb.full)) != NULL)
        memcpy(g_fb.host[i] + g_fb.full, tail, g_fb.bytes - g_fb.full);
    if (!pal) { int k; for (k = 0; k < 256; k++) grey[k] = (uint32_t)k * 0x010101u; pal = grey; }
    a = w->be->fb_show(w, g_fb.t, i, pal);
    if (a && a != g_fb.host[i]) {                   /* the backend moved the buffer: follow it */
        g_fb.host[i] = a;
        if (!fb_map(w, i)) { qw_log(w, "DirectDraw: remapping a moved display buffer failed"); fb_detach(w, 1); }
    }
}

/* Two opt-in markers allow the host to time a fixed displayed-frame interval
 * independently of loading and the guest/backend clock. No guest state changes. */
static void frame_marker(qwin *w)
{
    unsigned frame=(unsigned)w->frames;
    if(w->bench_frame_start && (frame==w->bench_frame_start||frame==w->bench_frame_end)){
        char path[320];size_t n=strlen(w->bench_marker_dir);
        const char *separator=n && (w->bench_marker_dir[n-1]==':'||w->bench_marker_dir[n-1]=='/')?"":"/";
        snprintf(path,sizeof path,"%s%sframe-%s.mark",w->bench_marker_dir,separator,frame==w->bench_frame_start?"start":"end");
        FILE *f=fopen(path,"w");
        if(!f){qw_log(w,"frame marker open failed: %s",path);return;}
        fprintf(f,"%u\n",frame);
        if(fclose(f))qw_log(w,"frame marker close failed: %s",path);
    }
}

/* ------------------------------------------------------------------ presentation */
/* show the primary surface in the cooperative window */
static void present(qwin *w, dxobj *prim)
{
    dxobj *dd = prim->dd >= 0 ? &g_o[prim->dd] : NULL;
    qwnd *t = dd && dd->hwnd ? W(w, dd->hwnd) : NULL;
    const uint32_t *pal = prim->pal >= 0 ? g_o[prim->pal].ent : NULL;
    int x, y, ww, hh;
    if (!t) return;
    t = win_top(w, t);
    if (!t) return;
    w->frames++;
    frame_marker(w);
    if (w->verbose) {
        static uint64_t t0;
        static int f0;
        uint64_t now = w->be->now(w);
        if (!t0) { t0 = now; f0 = w->frames; }
        if (now - t0 >= 5000u) {
            qw_log(w, "DirectDraw: %d frames per second", (int)((w->frames - f0) * 1000u / (uint32_t)(now - t0)));
            t0 = now; f0 = w->frames;
        }
    }
    if (t->cw != prim->sw || t->ch != prim->sh) {
        const qhle *mv = qw_find_hle("user32", "MoveWindow");
        uint32_t b[16];
        memset(b, 0, sizeof(b));
        b[0] = t->h; b[1] = 0; b[2] = 0; b[3] = (uint32_t)(prim->sw + t->nl + t->nr); b[4] = (uint32_t)(prim->sh + t->nt + t->nb); b[5] = 1;
        if (mv) mv->fn(w, b);
    }
    if (prim->rtg && g_fb.n) { fb_show(w, prim, pal); return; }
    if (prim->bpp >= 16 && t->fs_w == prim->sw && t->fs_h == prim->sh && t->fs_bpp == prim->bpp && t->be && w->be->present_deep &&
        w->be->present_deep(w, t, prim->mem, prim->pitch, prim->sw, prim->sh, prim->bpp)) return;
    if (prim->bpp == 8 && w->be->present8 && t->be) {      /* fast path: 8-bit frame straight to the backend */
        static uint8_t *frame;
        static size_t framen;
        size_t n = (size_t)prim->pitch * (size_t)prim->sh;
        static uint32_t grey[256];
        if (n > framen) { free(frame); frame = (uint8_t *)malloc(n); framen = frame ? n : 0; }
        if (frame) {
            if (!pal) { int i; for (i = 0; i < 256; i++) grey[i] = (uint32_t)i * 0x010101u; pal = grey; }
            x86_read_mem(C, prim->mem, frame, (uint32_t)n);
            w->be->present8(w, t, frame, prim->pitch, pal, prim->sw, prim->sh);
            return;
        }
    }
    surf_resize(&t->surf, t->cw, t->ch);
    ww = prim->sw < t->surf.w ? prim->sw : t->surf.w;
    hh = prim->sh < t->surf.h ? prim->sh : t->surf.h;
    {
        for (y = 0; y < hh; y++) {
            uint32_t *dst = t->surf.pix + (size_t)y * (size_t)t->surf.w;
            uint32_t ga = prim->mem + (uint32_t)(y * prim->pitch);
            /* rows may cross guest pages: read through the page-safe accessors when needed */
            uint8_t *row = x86_host_ptr(C, ga);
            int contiguous = row && ((ga & 0xFFFu) + (uint32_t)prim->pitch <= 0x1000u);
            for (x = 0; x < ww; x++) {
                uint32_t v;
                if (prim->bpp == 8) {
                    uint8_t i8 = contiguous ? row[x] : x86_rd8(C, ga + (uint32_t)x);
                    v = pal ? pal[i8] : (uint32_t)i8 * 0x010101u;
                } else if (prim->bpp == 16) {
                    uint32_t p = contiguous ? (uint32_t)(row[x * 2] | (row[x * 2 + 1] << 8)) : x86_rd16(C, ga + (uint32_t)x * 2);
                    v = ((p >> 11 & 31) << 19) | ((p >> 5 & 63) << 10) | ((p & 31) << 3);
                } else v = (contiguous ? (uint32_t)(row[x * 4] | (row[x * 4 + 1] << 8) | (row[x * 4 + 2] << 16)) : x86_rd32(C, ga + (uint32_t)x * 4)) & 0xFFFFFF;
                dst[x] = v;
            }
        }
    }
    t->present = 1;
    t->px0 = 0; t->py0 = 0; t->px1 = t->surf.w; t->py1 = t->surf.h;
    if (w->be->present) w->be->present(w, t);
}

/* ------------------------------------------------------------------ IUnknown helpers */
static uint32_t addref(dxobj *o) { if (!o) return 0; return (uint32_t)++o->refs; }
static void obj_free(qwin *w, dxobj *o)
{
    if (o->rtg) fb_detach(w, 1);                      /* back to ordinary memory first */
    free(o->dcorig);
    if (o->ownmem) { qw_free(w, o->ownmem); o->ownmem = 0; }
    else if (o->mem) qw_free(w, o->mem);              /* each surface/buffer owns exactly one block (Flip swaps them) */
    qw_free(w, o->guest);
    o->type = OB_FREE;
}
static int guid_is(qwin *w, uint32_t g, uint32_t d1) { return g && RD32(g) == d1; }

/* ================================================================== DirectDraw */
/* For Direct3D on the host: +8 w, +12 h, +16 pitch, +20 bpp, +24 mem, +28 generation, +32..+44
 * r,g,b,a masks, +48 colour key on, +52 key, +56 palette as 256 0x00RRGGBB. */
static void publish(qwin *w, dxobj *s)
{
    uint32_t g = s->guest;
    s->gen++;
    WR32(g + 8, (uint32_t)s->sw); WR32(g + 12, (uint32_t)s->sh); WR32(g + 16, (uint32_t)s->pitch); WR32(g + 20, (uint32_t)s->bpp);
    WR32(g + 24, s->mem); WR32(g + 28, s->gen);
    WR32(g + 32, s->rm); WR32(g + 36, s->gm); WR32(g + 40, s->bm); WR32(g + 44, s->am);
    WR32(g + 48, (uint32_t)s->ck_on); WR32(g + 52, s->ck_lo);
    if (s->bpp == 8 && s->pal >= 0) {
        uint32_t k;
        if (!s->pal_mirror) s->pal_mirror = qw_alloc(w, 1024);
        for (k = 0; k < 256; k++) WR32(s->pal_mirror + k * 4, g_o[s->pal].ent[k]);
        WR32(g + 56, s->pal_mirror);
    }
}
static uint32_t new_surface(qwin *w, int dd, int sw, int sh, int bpp, uint32_t caps)
{
    int i = obj_new(w, OB_SURF, VT_SURF);
    dxobj *s;
    if (i < 0) return 0;
    s = &g_o[i];
    s->sw = sw; s->sh = sh; s->bpp = bpp; s->pitch = ((sw * bpp / 8) + 3) & ~3; s->caps = caps; s->dd = dd;
    s->mem = qw_alloc(w, (uint32_t)(s->pitch * sh) + 16u);
    if (!s->mem) { qw_log(w, "DirectDraw: no guest memory for %dx%d surface", sw, sh); s->type = OB_FREE; return 0; }
    return (uint32_t)i + 1;
}

static uint32_t d3d_new_root(qwin *w, int ver);
FN(dd_QueryInterface) {
    dxobj *o = ob(w, a[0], OB_DD);
    if (!o) return E_NOINTERFACE;
    if (guid_is(w, a[1], 0xB3A6F3E0u)) {                 /* IID_IDirectDraw2: second vtable on the same object */
        uint32_t g = qw_alloc(w, 8);
        WR32(g, g_vt[VT_DD2]); WR32(g + 4, (uint32_t)idx_of(o));
        o->refs++;
        WR32(a[2], g);
        return DD_OK;
    }
    if (guid_is(w, a[1], 0x9C59509Au) || guid_is(w, a[1], 0x15E65EC0u)) {   /* DD4, DD7: the DD2 table carries their methods */
        uint32_t g = qw_alloc(w, 8);
        WR32(g, g_vt[VT_DD2]); WR32(g + 4, (uint32_t)idx_of(o));
        o->refs++;
        WR32(a[2], g);
        return DD_OK;
    }
    if (w->cpu.door) {
        int ver = guid_is(w, a[1], 0xF5049E77u) ? 7 : guid_is(w, a[1], 0xBB223240u) ? 3 : guid_is(w, a[1], 0x6AAE1EC1u) ? 2 : 0;
        if (ver) { WR32(a[2], d3d_new_root(w, ver)); return DD_OK; }
    }
    if (guid_is(w, a[1], 0x6C14DB80u)) { addref(o); WR32(a[2], a[0]); return DD_OK; }   /* IID_IDirectDraw */
    if (w->verbose) qw_log(w, "IDirectDraw::QueryInterface(%08x-...): not supported", a[1] ? RD32(a[1]) : 0);
    WR32(a[2], 0);
    return E_NOINTERFACE;
}
FN(dd_AddRef) { return addref(ob(w, a[0], OB_DD)); }
FN(dd_Release) { dxobj *o = ob(w, a[0], OB_DD); if (!o) return 0; if (--o->refs > 0) return (uint32_t)o->refs; return 0; }
FN(dd_ok) { UNUSED; return DD_OK; }
FN(dd_CreateClipper) { int i = obj_new(w, OB_CLIP, VT_CLIP); if (i < 0) return E_FAIL_; WR32(a[2], g_o[i].guest); return DD_OK; }
FN(dd_CreatePalette) {                 /* (this, flags, entries, pp, outer) */
    int i = obj_new(w, OB_PAL, VT_PAL), k;
    if (i < 0) return E_FAIL_;
    for (k = 0; k < 256; k++) {
        uint32_t e = a[2] ? RD32(a[2] + (uint32_t)k * 4) : 0;   /* PALETTEENTRY r,g,b,flags */
        g_o[i].ent[k] = ((e & 0xFF) << 16) | (e & 0xFF00) | ((e >> 16) & 0xFF);
    }
    WR32(a[3], g_o[i].guest);
    return DD_OK;
}
static void fill_pf(qwin *w, uint32_t pf, int bpp)
{
    uint32_t i;
    for (i = 0; i < 32; i += 4) WR32(pf + i, 0);
    WR32(pf, 32);
    WR32(pf + 12, (uint32_t)bpp);
    if (bpp == 8) WR32(pf + 4, 0x20 | 0x40);                  /* PALETTEINDEXED8 | RGB */
    else {
        WR32(pf + 4, 0x40);
        if (bpp == 16) { WR32(pf + 16, 0xF800); WR32(pf + 20, 0x07E0); WR32(pf + 24, 0x001F); }
        else { WR32(pf + 16, 0xFF0000); WR32(pf + 20, 0xFF00); WR32(pf + 24, 0xFF); }
    }
}
static void fill_desc(qwin *w, uint32_t d, dxobj *s, uint32_t surfptr)
{
    uint32_t i;
    for (i = 4; i < 108; i += 4) WR32(d + i, 0);
    WR32(d, 108);
    WR32(d + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (surfptr ? 0x800u : 0u));   /* CAPS HEIGHT WIDTH PITCH PIXELFORMAT [LPSURFACE] */
    WR32(d + 8, (uint32_t)s->sh); WR32(d + 12, (uint32_t)s->sw); WR32(d + 16, (uint32_t)s->pitch);
    if (s->primary && s->attached >= 0) { WR32(d + 4, RD32(d + 4) | 0x20); WR32(d + 20, (uint32_t)(s->nback ? s->nback : 1)); }
    WR32(d + 36, surfptr);
    if (s->ck_on) { WR32(d + 64, s->ck_lo); WR32(d + 68, s->ck_hi); WR32(d + 4, RD32(d + 4) | 0x10000); }
    fill_pf(w, d + 72, s->bpp);
    if (s->rm | s->gm | s->bm) {
        WR32(d + 72 + 16, s->rm); WR32(d + 72 + 20, s->gm); WR32(d + 72 + 24, s->bm); WR32(d + 72 + 28, s->am);
        if (s->am) WR32(d + 72 + 4, RD32(d + 72 + 4) | 1u);   /* ALPHAPIXELS */
    }
    WR32(d + 104, s->caps);
}
FN(dd_CreateSurface) {                  /* (this, desc, pp, outer) */
    dxobj *dd = ob(w, a[0], OB_DD);
    uint32_t d = a[1], flags, caps, id;
    int sw, sh, bpp;
    dxobj *s;
    if (!dd || !d) return DDERR_INVALIDPARAMS;
    flags = RD32(d + 4); caps = ((flags & 1) || RD32(d + 104)) ? RD32(d + 104) : 0x40;   /* Doom95 fills caps without the flag */
    if (w->verbose) qw_log(w, "DirectDraw: CreateSurface flags=%08x caps=%08x back=%u size=%ux%u", flags, caps, RD32(d + 20), RD32(d + 12), RD32(d + 8));
    if (!dd->mw) { dd->mw = 640; dd->mh = 480; dd->mbpp = 16; }          /* no SetDisplayMode: desktop-like 640x480x16 */
    bpp = (flags & 0x1000) ? (int)RD32(d + 84) : dd->mbpp;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) bpp = dd->mbpp;
    if (caps & 0x200) {                                                    /* PRIMARYSURFACE */
        id = new_surface(w, idx_of(dd), dd->mw, dd->mh, dd->mbpp, caps | 0x8000 | 0x4000 | ((caps & 0x10) ? 0x20 : 0));
        if (!id) return E_FAIL_;
        s = &g_o[id - 1]; s->primary = 1;
        /* A FLIP primary always has a back buffer, even if the count flag is unset. */
        if (((flags & 0x20) && RD32(d + 20) >= 1) || (caps & 0x10)) {                       /* back buffer(s): one is supported */
            /* A real flip chain: primary -> back 1 -> ... -> back n -> primary. */
            uint32_t want = (flags & 0x20) ? RD32(d + 20) : 1, k;
            dxobj *last = s;
            if (want < 1) want = 1;
            if (want > 7) want = 7;
            for (k = 0; k < want; k++) {
                uint32_t bid = new_surface(w, idx_of(dd), dd->mw, dd->mh, dd->mbpp, (k == 0 ? 0x4u : 0u) | 0x10 | 0x4000);
                if (!bid) return E_FAIL_;
                g_o[bid - 1].back = 1;
                last->attached = (int)bid - 1;
                last = &g_o[bid - 1];
            }
            if (want > 1) last->attached = idx_of(s);
            s->nback = (int)want;
        }
        if (w->verbose) qw_log(w, "DirectDraw: primary %dx%dx%d%s", dd->mw, dd->mh, dd->mbpp, s->attached >= 0 ? " + back buffer" : "");
        fb_attach(w, dd, s);
    } else {
        sw = (flags & 4) ? (int)RD32(d + 12) : dd->mw;
        sh = (flags & 2) ? (int)RD32(d + 8) : dd->mh;
        if (sw <= 0 || sh <= 0 || sw > 4096 || sh > 4096) return DDERR_INVALIDPARAMS;
        id = new_surface(w, idx_of(dd), sw, sh, bpp, (caps | 0x800) & ~0x4000u);
        if (!id) return E_FAIL_;
        s = &g_o[id - 1]; s->offscreen = 1;
        if (flags & 0x10000) { s->ck_on = 1; s->ck_lo = RD32(d + 64); s->ck_hi = RD32(d + 68); }
    }
    WR32(a[2], s->guest);
    return DD_OK;
}
FN(dd_EnumDisplayModes) {               /* (this, flags, desc, ctx, callback) */
    static const int modes[][3] = { { 320, 200, 8 }, { 320, 240, 8 }, { 640, 400, 8 }, { 640, 480, 8 }, { 640, 480, 16 }, { 800, 600, 8 }, { 800, 600, 16 } };
    uint32_t d = qw_alloc(w, 108);
    dxobj tmp;
    unsigned k;
    for (k = 0; k < sizeof(modes) / sizeof(modes[0]); k++) {
        memset(&tmp, 0, sizeof(tmp));
        tmp.sw = modes[k][0]; tmp.sh = modes[k][1]; tmp.bpp = modes[k][2]; tmp.pitch = tmp.sw * tmp.bpp / 8;
        fill_desc(w, d, &tmp, 0);
        WR32(d + 4, 0x2 | 0x4 | 0x8 | 0x1000 | 0x200000);                 /* + REFRESHRATE */
        WR32(d + 24, 60);
        if (!qw_call(w, a[4], 2, d, a[3], 0, 0)) break;
    }
    qw_free(w, d);
    return DD_OK;
}
FN(dd_GetCaps) {
    uint32_t i, p;
    UNUSED;
    for (i = 1; i < 3; i++) {
        p = a[i];
        if (!p) continue;
        { uint32_t sz = RD32(p), k; for (k = 4; k < sz && k < 400; k += 4) WR32(p + k, 0); }
        WR32(p + 4, 0x40 | 0x40000 | 0x80000);      /* BLT | COLORKEY | PALETTE */
        WR32(p + 12, 0x200);                        /* CKEYCAPS: SRCBLT */
        WR32(p + 24, 0x4 | 0x40);                   /* PALCAPS: 8BIT | PRIMARYSURFACE */
        WR32(p + 60, 8u << 20); WR32(p + 64, 8u << 20);
    }
    return DD_OK;
}
FN(dd_GetDisplayMode) {
    dxobj *dd = ob(w, a[0], OB_DD), tmp;
    if (!dd) return DDERR_INVALIDPARAMS;
    memset(&tmp, 0, sizeof(tmp));
    tmp.sw = dd->mw ? dd->mw : 640; tmp.sh = dd->mh ? dd->mh : 480; tmp.bpp = dd->mbpp ? dd->mbpp : 16; tmp.pitch = tmp.sw * tmp.bpp / 8;
    fill_desc(w, a[1], &tmp, 0);
    return DD_OK;
}
FN(dd_GetFourCCCodes) { UNUSED; if (a[1]) WR32(a[1], 0); return DD_OK; }
FN(dd_GetMonitorFrequency) { UNUSED; if (a[1]) WR32(a[1], 60); return DD_OK; }
FN(dd_GetScanLine) { UNUSED; if (a[1]) WR32(a[1], 0); return DD_OK; }
/* A 60 Hz display spends about 2 ms of each 16 in vertical blank. Programs wait
 * for this to change, so a constant answer hangs them. */
FN(dd_GetVerticalBlankStatus) { if (a[1]) WR32(a[1], (uint32_t)w->be->now(w) % 16u < 2u); return DD_OK; }
FN(dd_SetCooperativeLevel) {
    dxobj *dd = ob(w, a[0], OB_DD);
    if (!dd) return DDERR_INVALIDPARAMS;
    dd->hwnd = a[1];
    dd->exclusive = (a[2] & 0x10) != 0;
    if (w->verbose) qw_log(w, "DirectDraw: cooperative level %08x window %08x", a[2], a[1]);
    return DD_OK;
}
/* exclusive 8-bit modes: ask the backend for an own screen of that size (AROS) */
static void fullscreen(qwin *w, dxobj *dd, int on)
{
    qwnd *t = dd->hwnd ? W(w, dd->hwnd) : NULL;
    if (t) t = win_top(w, t);
    if (!t || !w->be->reopen) return;
    fb_detach(w, 1);                                  /* mode change: display buffers go away */
    if (on && !dd->exclusive) return;
    if (on) {
        const qhle *mv = qw_find_hle("user32", "MoveWindow");
        uint32_t b[16];
        memset(b, 0, sizeof(b));
        if (mv && (t->cw != dd->mw || t->ch != dd->mh)) {
            b[0] = t->h; b[3] = (uint32_t)(dd->mw + t->nl + t->nr); b[4] = (uint32_t)(dd->mh + t->nt + t->nb); b[5] = 1;
            mv->fn(w, b);
        }
        if (t->fs_w == dd->mw && t->fs_h == dd->mh && t->fs_bpp == dd->mbpp) return;
        t->fs_w = dd->mw; t->fs_h = dd->mh; t->fs_bpp = dd->mbpp;
    } else {
        if (!t->fs_w) return;
        t->fs_w = t->fs_h = t->fs_bpp = 0;
    }
    if (t->be) w->be->reopen(w, t);
}
static uint32_t set_mode(qwin *w, uint32_t self, uint32_t mw, uint32_t mh, uint32_t bpp)
{
    dxobj *dd = ob(w, self, OB_DD);
    if (!dd) return DDERR_INVALIDPARAMS;
    if ((bpp != 8 && bpp != 16 && bpp != 32) || mw < 64 || mh < 64 || mw > 1600 || mh > 1200) {
        qw_log(w, "DirectDraw: display mode %ux%ux%u not supported", mw, mh, bpp);
        return 0x8876024Eu;                         /* DDERR_UNSUPPORTEDMODE */
    }
    dd->mw = (int)mw; dd->mh = (int)mh; dd->mbpp = (int)bpp;
    if (w->verbose) qw_log(w, "DirectDraw: display mode %ux%ux%u", mw, mh, bpp);
    fullscreen(w, dd, 1);
    return DD_OK;
}
FN(dd_RestoreDisplayMode) { dxobj *dd = ob(w, a[0], OB_DD); if (dd) fullscreen(w, dd, 0); return DD_OK; }
FN(dd_SetDisplayMode) { return set_mode(w, a[0], a[1], a[2], a[3]); }
FN(dd2_SetDisplayMode) { return set_mode(w, a[0], a[1], a[2], a[3]); }
FN(dd2_GetAvailableVidMem) { UNUSED; if (a[2]) WR32(a[2], 8u << 20); if (a[3]) WR32(a[3], 8u << 20); return DD_OK; }

/* ------------------------------------------------------------------ surfaces */
static dxobj *sf(qwin *w, uint32_t g) { return ob(w, g, OB_SURF); }
static uint32_t d3d_new_texture(qwin *w, uint32_t surface, int v2);
FN(s_QueryInterface) {
    dxobj *s = sf(w, a[0]);
    uint32_t d1 = a[1] ? RD32(a[1]) : 0;
    if (s && (d1 == 0x2CDCD9E0u || d1 == 0x93281502u)) { WR32(a[2], d3d_new_texture(w, a[0], d1 == 0x93281502u)); return DD_OK; }
    if (s && (d1 == 0x6C14DB81u || d1 == 0x57805885u || d1 == 0xDA044E00u || d1 == 0x69C11C3Eu || d1 == 0x0B2B8630u || d1 == 0x06675A80u)) { addref(s); WR32(a[2], a[0]); return DD_OK; }  /* Surface, Surface2, Surface3 */
    if (w->verbose) qw_log(w, "IDirectDrawSurface::QueryInterface(%08x-...): not supported", d1);
    WR32(a[2], 0);
    return E_NOINTERFACE;
}
FN(s_AddRef) { return addref(sf(w, a[0])); }
FN(s_Release) {
    dxobj *s = sf(w, a[0]);
    if (!s) return 0;
    if (--s->refs > 0) return (uint32_t)s->refs;
    if (s->primary) {
        int k = s->attached, n;
        for (n = 0; n < s->nback && k >= 0 && g_o[k].type == OB_SURF && k != idx_of(s); n++) {
            int next = g_o[k].attached;
            obj_free(w, &g_o[k]);
            k = next;
        }
    }
    obj_free(w, s);
    return 0;
}
static int rect_of(qwin *w, uint32_t p, int *r, int dw, int dh)
{
    if (!p) { r[0] = 0; r[1] = 0; r[2] = dw; r[3] = dh; return 1; }
    r[0] = (int32_t)RD32(p); r[1] = (int32_t)RD32(p + 4); r[2] = (int32_t)RD32(p + 8); r[3] = (int32_t)RD32(p + 12);
    return r[2] > r[0] && r[3] > r[1];
}
static uint32_t get_px(qwin *w, dxobj *s, int x, int y)
{
    uint32_t a = s->mem + (uint32_t)(y * s->pitch);
    if (s->bpp == 8) return x86_rd8(C, a + (uint32_t)x);
    if (s->bpp == 16) return x86_rd16(C, a + (uint32_t)x * 2);
    return x86_rd32(C, a + (uint32_t)x * 4);
}
static void put_px(qwin *w, dxobj *s, int x, int y, uint32_t v)
{
    uint32_t a = s->mem + (uint32_t)(y * s->pitch);
    if (s->bpp == 8) x86_wr8(C, a + (uint32_t)x, (uint8_t)v);
    else if (s->bpp == 16) x86_wr16(C, a + (uint32_t)x * 2, (uint16_t)v);
    else x86_wr32(C, a + (uint32_t)x * 4, v);
}
/* copy rows with host memcpy when no colour key / scaling is involved (the hot path) */
static void blit(qwin *w, dxobj *d, int *dr, dxobj *s, int *sr, int key, uint32_t klo, uint32_t khi)
{
    int dw = dr[2] - dr[0], dh = dr[3] - dr[1], sw = sr[2] - sr[0], sh = sr[3] - sr[1], x, y;
    int bpp = d->bpp / 8;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (!key && dw == sw && dh == sh && s->bpp == d->bpp) {
        uint8_t row[16384];
        int n = dw * bpp;
        for (y = 0; y < dh; y++) {
            int sy = sr[1] + y, dy = dr[1] + y;
            int x0 = dr[0], sx0 = sr[0], m = n;
            if (dy < 0 || dy >= d->sh || sy < 0 || sy >= s->sh) continue;
            if (x0 < 0) { sx0 -= x0; m += x0 * bpp; x0 = 0; }
            if (sx0 < 0) { x0 -= sx0; m += sx0 * bpp; sx0 = 0; }
            if (x0 * bpp + m > d->pitch) m = d->pitch - x0 * bpp;
            if (sx0 * bpp + m > s->pitch) m = s->pitch - sx0 * bpp;
            if (m <= 0 || m > (int)sizeof(row)) continue;
            x86_read_mem(C, s->mem + (uint32_t)(sy * s->pitch + sx0 * bpp), row, (uint32_t)m);
            x86_write_mem(C, d->mem + (uint32_t)(dy * d->pitch + x0 * bpp), row, (uint32_t)m);
        }
        return;
    }
    for (y = 0; y < dh; y++) {
        int dy = dr[1] + y, sy = sr[1] + (dh == sh ? y : y * sh / dh);
        if (dy < 0 || dy >= d->sh || sy < 0 || sy >= s->sh) continue;
        for (x = 0; x < dw; x++) {
            int dx = dr[0] + x, sx = sr[0] + (dw == sw ? x : x * sw / dw);
            uint32_t v;
            if (dx < 0 || dx >= d->sw || sx < 0 || sx >= s->sw) continue;
            v = get_px(w, s, sx, sy);
            if (key && v >= klo && v <= khi) continue;
            put_px(w, d, dx, dy, v);
        }
    }
}
static void fill(qwin *w, dxobj *d, int *r, uint32_t color)
{
    int x, y, x0 = r[0] < 0 ? 0 : r[0], y0 = r[1] < 0 ? 0 : r[1], x1 = r[2] > d->sw ? d->sw : r[2], y1 = r[3] > d->sh ? d->sh : r[3];
    uint8_t row[16384];
    int bpp = d->bpp / 8, n = (x1 - x0) * bpp;
    if (x1 <= x0 || y1 <= y0 || n > (int)sizeof(row)) return;
    for (x = 0; x < x1 - x0; x++) {
        if (bpp == 1) row[x] = (uint8_t)color;
        else if (bpp == 2) { row[x * 2] = (uint8_t)color; row[x * 2 + 1] = (uint8_t)(color >> 8); }
        else { row[x * 4] = (uint8_t)color; row[x * 4 + 1] = (uint8_t)(color >> 8); row[x * 4 + 2] = (uint8_t)(color >> 16); row[x * 4 + 3] = (uint8_t)(color >> 24); }
    }
    for (y = y0; y < y1; y++) x86_write_mem(C, d->mem + (uint32_t)(y * d->pitch + x0 * bpp), row, (uint32_t)n);
}
static void after_draw(qwin *w, dxobj *d) { publish(w, d); if (d->primary) present(w, d); }
FN(s_Blt) {                              /* (this, destrect, src, srcrect, flags, fx) */
    dxobj *d = sf(w, a[0]), *s = a[2] ? sf(w, a[2]) : NULL;
    uint32_t fl = a[4], fx = a[5];
    int dr[4], sr[4];
    if (!d) return DDERR_INVALIDPARAMS;
    if (!rect_of(w, a[1], dr, d->sw, d->sh)) return DD_OK;
    if (fl & 0x400) {                                                  /* COLORFILL */
        fill(w, d, dr, fx ? RD32(fx + 80) : 0);
        after_draw(w, d);
        return DD_OK;
    }
    if (!s) {
        if ((fl & 0x20000) && fx && RD32(fx + 8) == 0x00000042u) { fill(w, d, dr, 0); after_draw(w, d); return DD_OK; }   /* ROP BLACKNESS */
        if (w->verbose) qw_log(w, "IDirectDrawSurface::Blt flags %08x without source: ignored", fl);
        return DD_OK;
    }
    if (!rect_of(w, a[3], sr, s->sw, s->sh)) return DD_OK;
    if (fl & 0x10000) blit(w, d, dr, s, sr, 1, RD32(fx + 92), RD32(fx + 96));          /* KEYSRCOVERRIDE */
    else blit(w, d, dr, s, sr, (fl & 0x8000) && s->ck_on, s->ck_lo, s->ck_hi);           /* KEYSRC */
    after_draw(w, d);
    return DD_OK;
}
FN(s_BltFast) {                          /* (this, x, y, src, srcrect, trans) */
    dxobj *d = sf(w, a[0]), *s = sf(w, a[3]);
    int dr[4], sr[4];
    if (!d || !s) return DDERR_INVALIDPARAMS;
    if (!rect_of(w, a[4], sr, s->sw, s->sh)) return DD_OK;
    dr[0] = (int32_t)a[1]; dr[1] = (int32_t)a[2]; dr[2] = dr[0] + sr[2] - sr[0]; dr[3] = dr[1] + sr[3] - sr[1];
    blit(w, d, dr, s, sr, (a[5] & 1) && s->ck_on, s->ck_lo, s->ck_hi);
    after_draw(w, d);
    return DD_OK;
}
static int g_d3d_active;
static void d3d_host_present(qwin *w);
FN(s_Flip) {
    dxobj *p = sf(w, a[0]), *b;
    uint32_t t;
    if (!p) return DDERR_INVALIDPARAMS;
    if (g_d3d_active) { d3d_host_present(w); return DD_OK; }
    if (p->attached < 0) { present(w, p); return DD_OK; }
    b = &g_o[p->attached];
    if (p->nback > 1) {
        /* Each surface takes the memory of the next one; the last gets the old front. */
        dxobj *cur = p;
        int n;
        t = p->mem;
        for (n = 0; n < p->nback; n++) { dxobj *next = &g_o[cur->attached]; cur->mem = next->mem; cur = next; }
        cur->mem = t;
        present(w, p);
        return DD_OK;
    }
    t = p->mem; p->mem = b->mem; b->mem = t;           /* swap front and back buffer memory */
    if (p->rtg) { int f = p->fb; p->fb = b->fb; b->fb = f; }   /* RTG direct: the page flip itself */
    present(w, p);
    return DD_OK;
}
FN(s_GetAttachedSurface) {
    dxobj *s = sf(w, a[0]);
    if (!s || s->attached < 0) { WR32(a[2], 0); return DDERR_NOTFOUND; }
    addref(&g_o[s->attached]);
    WR32(a[2], g_o[s->attached].guest);
    return DD_OK;
}
FN(s_EnumAttachedSurfaces) {             /* (this, ctx, callback(surface, desc, ctx)) */
    dxobj *s = sf(w, a[0]);
    if (s && s->attached >= 0) {
        uint32_t d = qw_alloc(w, 108);
        fill_desc(w, d, &g_o[s->attached], 0);
        addref(&g_o[s->attached]);
        qw_call(w, a[2], 3, g_o[s->attached].guest, d, a[1], 0);
        qw_free(w, d);
    }
    return DD_OK;
}
FN(s_GetCaps) { dxobj *s = sf(w, a[0]); if (!s) return DDERR_INVALIDPARAMS; WR32(a[1], s->caps); return DD_OK; }
FN(s_GetColorKey) { dxobj *s = sf(w, a[0]); if (!s || !s->ck_on) return 0x887600F0u; WR32(a[2], s->ck_lo); WR32(a[2] + 4, s->ck_hi); return DD_OK; }
FN(s_SetColorKey) {                      /* (this, flags, key) */
    dxobj *s = sf(w, a[0]);
    if (!s) return DDERR_INVALIDPARAMS;
    if (!(a[1] & 8)) return DD_OK;                    /* only SRCBLT keys are used for blits */
    if (!a[2]) { s->ck_on = 0; return DD_OK; }
    s->ck_on = 1; s->ck_lo = RD32(a[2]); s->ck_hi = RD32(a[2] + 4);
    if (s->ck_hi < s->ck_lo) s->ck_hi = s->ck_lo;
    publish(w, s);
    return DD_OK;
}
FN(s_GetPixelFormat) {
    dxobj *s = sf(w, a[0]);
    if (!s) return DDERR_INVALIDPARAMS;
    fill_pf(w, a[1], s->bpp);
    if (s->rm | s->gm | s->bm) {
        WR32(a[1] + 16, s->rm); WR32(a[1] + 20, s->gm); WR32(a[1] + 24, s->bm); WR32(a[1] + 28, s->am);
        if (s->am) WR32(a[1] + 4, RD32(a[1] + 4) | 1u);
    }
    return DD_OK;
}
FN(s_GetSurfaceDesc) { dxobj *s = sf(w, a[0]); if (!s) return DDERR_INVALIDPARAMS; fill_desc(w, a[1], s, 0); return DD_OK; }
FN(s_Lock) {                             /* (this, rect, desc, flags, event) */
    dxobj *s = sf(w, a[0]);
    int r[4];
    if (!s || !a[2]) return DDERR_INVALIDPARAMS;
    rect_of(w, a[1], r, s->sw, s->sh);
    fill_desc(w, a[2], s, s->mem + (uint32_t)(r[1] * s->pitch + r[0] * (s->bpp / 8)));
    return DD_OK;
}
FN(s_Unlock) { dxobj *s = sf(w, a[0]); if (s) { publish(w, s); after_draw(w, s); } return DD_OK; }
FN(s_GetPalette) { dxobj *s = sf(w, a[0]); if (!s || s->pal < 0) { WR32(a[1], 0); return DDERR_NOPALETTEATTACHED; } addref(&g_o[s->pal]); WR32(a[1], g_o[s->pal].guest); return DD_OK; }
FN(s_SetPalette) {
    dxobj *s = sf(w, a[0]), *p = a[1] ? ob(w, a[1], OB_PAL) : NULL;
    if (!s) return DDERR_INVALIDPARAMS;
    s->pal = p ? idx_of(p) : -1;
    if (p) p->refs++;
    if (s->attached >= 0) g_o[s->attached].pal = s->pal;
    publish(w, s);
    if (s->primary) present(w, s);
    return DD_OK;
}
FN(s_IsLost) { UNUSED; return DD_OK; }
FN(s_GetDC) {                            /* memory DC on a 32-bit copy of the surface */
    dxobj *s = sf(w, a[0]);
    const uint32_t *pal;
    const qhle *ccdc = qw_find_hle("gdi32", "CreateCompatibleDC"), *sel = qw_find_hle("gdi32", "SelectObject");
    uint32_t b[16], dc, bm;
    qobj *o;
    int x, y;
    if (!s || !ccdc || !sel) return E_FAIL_;
    if (s->dc) { WR32(a[1], s->dc); return DD_OK; }
    pal = s->pal >= 0 ? g_o[s->pal].ent : (s->primary || s->back || s->dd < 0) ? NULL : NULL;
    if (!pal && s->bpp == 8 && s->dd >= 0) {               /* off-screen 8-bit: use the primary's palette */
        int i;
        for (i = 0; i < DX_MAX; i++) if (g_o[i].type == OB_SURF && g_o[i].primary && g_o[i].pal >= 0) { pal = g_o[g_o[i].pal].ent; break; }
    }
    memset(b, 0, sizeof(b));
    dc = ccdc->fn(w, b);
    bm = gdi_new(w, GO_BITMAP);
    o = O(w, bm, GO_BITMAP);
    if (!o) return E_FAIL_;
    surf_resize(&o->bmp, s->sw, s->sh);
    free(s->dcorig);
    s->dcorig = (uint32_t *)malloc((size_t)s->sw * (size_t)s->sh * 4u);
    for (y = 0; y < s->sh; y++) for (x = 0; x < s->sw; x++) {
        uint32_t v = get_px(w, s, x, y), c32;
        if (s->bpp == 8) c32 = pal ? pal[v & 0xFF] : (v & 0xFF) * 0x010101u;
        else if (s->bpp == 16) c32 = ((v >> 11 & 31) << 19) | ((v >> 5 & 63) << 10) | ((v & 31) << 3);
        else c32 = v & 0xFFFFFF;
        o->bmp.pix[(size_t)y * (size_t)s->sw + (size_t)x] = c32;
        if (s->dcorig) s->dcorig[(size_t)y * (size_t)s->sw + (size_t)x] = c32;
    }
    b[0] = dc; b[1] = bm; sel->fn(w, b);
    s->dc = dc; s->dcbm = bm;
    WR32(a[1], dc);
    return DD_OK;
}
static uint8_t nearest(const uint32_t *pal, uint32_t c)
{
    int i, best = 0;
    uint32_t bd = 0xFFFFFFFFu;
    for (i = 0; i < 256; i++) {
        int dr = (int)((pal[i] >> 16) & 255) - (int)((c >> 16) & 255), dg = (int)((pal[i] >> 8) & 255) - (int)((c >> 8) & 255), db = (int)(pal[i] & 255) - (int)(c & 255);
        uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
        if (d < bd) { bd = d; best = i; if (!d) break; }
    }
    return (uint8_t)best;
}
FN(s_ReleaseDC) {                        /* write back the pixels GDI changed */
    dxobj *s = sf(w, a[0]);
    qobj *o;
    const uint32_t *pal = NULL;
    int x, y;
    if (!s || !s->dc) return DDERR_INVALIDPARAMS;
    o = O(w, s->dcbm, GO_BITMAP);
    if (s->pal >= 0) pal = g_o[s->pal].ent;
    else { int i; for (i = 0; i < DX_MAX; i++) if (g_o[i].type == OB_SURF && g_o[i].primary && g_o[i].pal >= 0) { pal = g_o[g_o[i].pal].ent; break; } }
    if (o && o->bmp.pix && s->dcorig) {
        uint32_t lastc = 0xFFFFFFFFu; uint8_t lasti = 0;
        for (y = 0; y < s->sh; y++) for (x = 0; x < s->sw; x++) {
            uint32_t c32 = o->bmp.pix[(size_t)y * (size_t)s->sw + (size_t)x];
            if (c32 == s->dcorig[(size_t)y * (size_t)s->sw + (size_t)x]) continue;
            if (s->bpp == 8) { if (c32 != lastc) { lastc = c32; lasti = pal ? nearest(pal, c32) : (uint8_t)(c32 & 255); } put_px(w, s, x, y, lasti); }
            else if (s->bpp == 16) put_px(w, s, x, y, ((c32 >> 19 & 31) << 11) | ((c32 >> 10 & 63) << 5) | (c32 >> 3 & 31));
            else put_px(w, s, x, y, c32);
        }
    }
    { const qhle *ddc = qw_find_hle("gdi32", "DeleteDC"), *dob = qw_find_hle("gdi32", "DeleteObject"); uint32_t b[16]; memset(b, 0, sizeof(b));
      b[0] = s->dc; if (ddc) ddc->fn(w, b); b[0] = s->dcbm; if (dob) dob->fn(w, b); }
    s->dc = 0; s->dcbm = 0;
    free(s->dcorig); s->dcorig = NULL;
    after_draw(w, s);
    return DD_OK;
}
/* IDirectDrawSurface3::SetSurfaceDesc (this, desc, flags): the surface now uses the
 * program's memory (lpSurface) with the given size/pitch/pixel format */
FN(s_SetSurfaceDesc) {
    dxobj *s = sf(w, a[0]);
    uint32_t d = a[1], fl;
    if (!s || !d) return DDERR_INVALIDPARAMS;
    fl = RD32(d + 4);
    if (!(fl & 0x800) || !RD32(d + 36)) return DDERR_INVALIDPARAMS;
    if (s->rtg) fb_detach(w, 1);
    if (!s->ownmem) s->ownmem = s->mem;
    s->mem = RD32(d + 36);
    if (fl & 4) s->sw = (int)RD32(d + 12);
    if (fl & 2) s->sh = (int)RD32(d + 8);
    if (fl & 0x1000) { int b = (int)RD32(d + 84); if (b == 8 || b == 16 || b == 32) s->bpp = b; }
    s->pitch = (fl & 8) ? (int)RD32(d + 16) : ((s->sw * s->bpp / 8) + 3) & ~3;
    return DD_OK;
}
FN(s_GetDDInterface) { dxobj *s = sf(w, a[0]); if (!s || s->dd < 0) return DDERR_INVALIDPARAMS; addref(&g_o[s->dd]); WR32(a[1], g_o[s->dd].guest); return DD_OK; }

/* ------------------------------------------------------------------ palettes, clippers */
FN(p_QueryInterface) { UNUSED; WR32(a[2], 0); return E_NOINTERFACE; }
FN(p_AddRef) { return addref(ob(w, a[0], OB_PAL)); }
FN(p_Release) { dxobj *p = ob(w, a[0], OB_PAL); if (!p) return 0; if (--p->refs > 0) return (uint32_t)p->refs; obj_free(w, p); return 0; }
FN(p_GetCaps) { UNUSED; WR32(a[1], 0x4); return DD_OK; }
FN(p_GetEntries) {                       /* (this, flags, start, count, entries) */
    dxobj *p = ob(w, a[0], OB_PAL);
    uint32_t i;
    if (!p) return DDERR_INVALIDPARAMS;
    for (i = 0; i < a[3] && a[2] + i < 256; i++) { uint32_t c = p->ent[a[2] + i]; WR32(a[4] + i * 4, ((c >> 16) & 255) | (c & 0xFF00) | ((c & 255) << 16)); }
    return DD_OK;
}
FN(p_SetEntries) {
    dxobj *p = ob(w, a[0], OB_PAL);
    uint32_t i;
    int k;
    if (!p) return DDERR_INVALIDPARAMS;
    for (i = 0; i < a[3] && a[2] + i < 256; i++) { uint32_t e = RD32(a[4] + i * 4); p->ent[a[2] + i] = ((e & 0xFF) << 16) | (e & 0xFF00) | ((e >> 16) & 0xFF); }
    for (k = 0; k < DX_MAX; k++) if (g_o[k].type == OB_SURF && g_o[k].primary && g_o[k].pal == idx_of(p)) present(w, &g_o[k]);   /* palette animation */
    return DD_OK;
}
FN(c_AddRef) { return addref(ob(w, a[0], OB_CLIP)); }
FN(c_Release) { dxobj *c = ob(w, a[0], OB_CLIP); if (!c) return 0; if (--c->refs > 0) return (uint32_t)c->refs; obj_free(w, c); return 0; }
FN(c_SetHWnd) { dxobj *c = ob(w, a[0], OB_CLIP); if (c) c->hwnd = a[2]; return DD_OK; }
FN(c_GetHWnd) { dxobj *c = ob(w, a[0], OB_CLIP); WR32(a[1], c ? c->hwnd : 0); return DD_OK; }

/* ------------------------------------------------------------------ entry points */
FN(x_DirectDrawCreate) {                 /* (guid, pp, outer) */
    int i = obj_new(w, OB_DD, VT_DD);
    if (i < 0) return E_FAIL_;
    WR32(a[1], g_o[i].guest);
    if (w->verbose) qw_log(w, "DirectDrawCreate -> %08x", g_o[i].guest);
    return DD_OK;
}
FN(x_DirectDrawEnumerateA) {             /* (callback(guid, desc, name, ctx), ctx) */
    uint32_t d = qw_alloc(w, 64), n = qw_alloc(w, 16);
    static const char desc[] = "QRX DirectDraw (HLE)", name[] = "display";
    uint32_t i;
    for (i = 0; i < sizeof(desc); i++) x86_wr8(C, d + i, (uint8_t)desc[i]);
    for (i = 0; i < sizeof(name); i++) x86_wr8(C, n + i, (uint8_t)name[i]);
    qw_call(w, a[0], 4, 0, d, n, a[1]);
    return DD_OK;
}

/* ================================================================== DirectSound */
static uint64_t now_ms(qwin *w) { return w->be->now(w); }
static int ds_host(qwin *w, dxobj *b, uint32_t op, uint32_t arg);
static uint32_t ds_host_pos(qwin *w, dxobj *b, int *playing);
#define DOOR_SVC_SOUND 1u
enum { DS_START = 1, DS_STOP, DS_SET, DS_POS, DS_FREE };
static uint32_t play_pos(qwin *w, dxobj *b)
{
    uint64_t el;
    uint32_t adv;
    if (w->cpu.door && !b->primbuf) { int on = 0; uint32_t p = ds_host_pos(w, b, &on); b->playing = on; return p; }
    if (!b->playing || !b->size) return b->pos0;
    el = now_ms(w) - b->t0;
    adv = (uint32_t)(el * (uint64_t)(b->bps ? b->bps : 22050) / 1000u);
    if (b->looping) return (b->pos0 + adv) % b->size;
    if (b->pos0 + adv >= b->size) { b->playing = 0; b->pos0 = 0; return 0; }
    return b->pos0 + adv;
}
/* With the ARMithlon door the host mixes: buffers stay in x86 memory and only state
 * changes come through here. Without it, the silent clock model above remains. */
static int ds_host(qwin *w, dxobj *b, uint32_t op, uint32_t arg)
{
    static uint32_t q[16];
    if (!w->cpu.door || b->primbuf) return 0;   /* the primary buffer is the mix itself, not a voice */
    q[0] = DOOR_SVC_SOUND; q[3] = op; q[4] = (uint32_t)idx_of(b); q[5] = b->mem; q[6] = b->size;
    q[7] = b->freq ? b->freq : b->rate; q[8] = b->chans; q[9] = b->bits;
    q[10] = (uint32_t)b->vol; q[11] = (uint32_t)b->pan; q[12] = (uint32_t)b->looping; q[13] = arg; q[14] = 0;
    if (!x86_door_call(&w->cpu, q)) return 0;
    return 1;
}
static uint32_t ds_host_pos(qwin *w, dxobj *b, int *playing)
{
    static uint32_t q[16];
    q[0] = DOOR_SVC_SOUND; q[3] = DS_POS; q[4] = (uint32_t)idx_of(b); q[14] = 0; q[15] = 0;
    if (!x86_door_call(&w->cpu, q)) { *playing = 0; return 0; }
    *playing = (int)q[15];
    return q[14];
}
/* A host mixer voice outside the DirectSound objects, for waveOut. False without the door. */
int qw_ds_voice(qwin *w, uint32_t op, uint32_t voice, uint32_t mem, uint32_t size, uint32_t rate, uint32_t ch, uint32_t bits, int loop, uint32_t arg, uint32_t *pos, int *playing)
{
    static uint32_t q[16];
    if (!w->cpu.door) return 0;
    q[0] = DOOR_SVC_SOUND; q[3] = op; q[4] = voice; q[5] = mem; q[6] = size;
    q[7] = rate; q[8] = ch; q[9] = bits; q[10] = 0; q[11] = 0; q[12] = (uint32_t)loop; q[13] = arg; q[14] = 0; q[15] = 0;
    if (!x86_door_call(&w->cpu, q)) return 0;
    if (pos) *pos = q[14];
    if (playing) *playing = (int)q[15];
    return 1;
}
FN(ds_QueryInterface) { UNUSED; WR32(a[2], 0); return E_NOINTERFACE; }
FN(ds_AddRef) { return addref(ob(w, a[0], OB_DS)); }
FN(ds_Release) { dxobj *o = ob(w, a[0], OB_DS); if (!o) return 0; if (--o->refs > 0) return (uint32_t)o->refs; obj_free(w, o); return 0; }
FN(ds_CreateSoundBuffer) {               /* (this, desc, pp, outer) */
    uint32_t d = a[1], fmt, fl;
    int i = obj_new(w, OB_DSB, VT_DSB);
    dxobj *b;
    if (i < 0 || !d) return DDERR_INVALIDPARAMS;
    b = &g_o[i];
    fl = RD32(d + 4);
    b->size = RD32(d + 8);
    fmt = RD32(d + 16);
    b->primbuf = (fl & 1) != 0;
    if (fmt) { b->chans = x86_rd16(C, fmt + 2); b->rate = RD32(fmt + 4); b->bps = RD32(fmt + 8); b->align = x86_rd16(C, fmt + 12); b->bits = x86_rd16(C, fmt + 14); }
    else { b->chans = 2; b->rate = 22050; b->bps = 88200; b->align = 4; b->bits = 16; }
    if (b->primbuf && !b->size) b->size = 8192;
    if (b->size > (8u << 20)) { obj_free(w, b); return DDERR_INVALIDPARAMS; }
    b->mem = qw_alloc(w, b->size + 16);
    b->freq = b->rate;
    WR32(a[2], b->guest);
    if (w->verbose) qw_log(w, "DirectSound: buffer %u bytes %u Hz%s", b->size, b->rate, b->primbuf ? " primary" : "");
    return DD_OK;
}
FN(ds_GetCaps) { uint32_t sz = RD32(a[1]), k; UNUSED; for (k = 4; k < sz && k < 200; k += 4) WR32(a[1] + k, 0); WR32(a[1] + 4, 0x1F00); return DD_OK; }
FN(ds_DuplicateSoundBuffer) {
    dxobj *s = ob(w, a[1], OB_DSB);
    int i;
    if (!s) return DDERR_INVALIDPARAMS;
    i = obj_new(w, OB_DSB, VT_DSB);
    if (i < 0) return E_FAIL_;
    { uint32_t g = g_o[i].guest; g_o[i] = *s; g_o[i].guest = g; g_o[i].refs = 1; g_o[i].playing = 0; g_o[i].mem = qw_alloc(w, s->size + 16); }
    { uint8_t tmp[4096]; uint32_t off; for (off = 0; off < s->size; off += sizeof(tmp)) { uint32_t n = s->size - off < sizeof(tmp) ? s->size - off : (uint32_t)sizeof(tmp); x86_read_mem(C, s->mem + off, tmp, n); x86_write_mem(C, g_o[i].mem + off, tmp, n); } }
    WR32(a[2], g_o[i].guest);
    return DD_OK;
}
FN(ds_GetSpeakerConfig) { UNUSED; WR32(a[1], 4); return DD_OK; }
FN(b_AddRef) { return addref(ob(w, a[0], OB_DSB)); }
FN(b_Release) { dxobj *o = ob(w, a[0], OB_DSB); if (!o) return 0; if (--o->refs > 0) return (uint32_t)o->refs; ds_host(w, o, DS_FREE, 0); obj_free(w, o); return 0; }
FN(b_GetCaps) { dxobj *b = ob(w, a[0], OB_DSB); if (!b) return DDERR_INVALIDPARAMS; WR32(a[1] + 4, b->primbuf ? 1u : 0x20u); WR32(a[1] + 8, b->size); WR32(a[1] + 12, 0); WR32(a[1] + 16, 0); return DD_OK; }
FN(b_GetCurrentPosition) { dxobj *b = ob(w, a[0], OB_DSB); uint32_t p; if (!b) return DDERR_INVALIDPARAMS; p = play_pos(w, b); if (a[1]) WR32(a[1], p); if (a[2]) WR32(a[2], b->size ? (p + b->align * 64) % b->size : 0); return DD_OK; }
FN(b_GetFormat) {
    dxobj *b = ob(w, a[0], OB_DSB);
    if (!b) return DDERR_INVALIDPARAMS;
    if (a[3]) WR32(a[3], 18);
    if (a[1] && a[2] >= 16) {
        x86_wr16(C, a[1], 1); x86_wr16(C, a[1] + 2, (uint16_t)b->chans);
        WR32(a[1] + 4, b->rate); WR32(a[1] + 8, b->bps); x86_wr16(C, a[1] + 12, (uint16_t)b->align); x86_wr16(C, a[1] + 14, (uint16_t)b->bits);
    }
    return DD_OK;
}
FN(b_GetVolume) { dxobj *b = ob(w, a[0], OB_DSB); WR32(a[1], b ? (uint32_t)b->vol : 0); return DD_OK; }
FN(b_GetPan) { dxobj *b = ob(w, a[0], OB_DSB); WR32(a[1], b ? (uint32_t)b->pan : 0); return DD_OK; }
FN(b_GetFrequency) { dxobj *b = ob(w, a[0], OB_DSB); WR32(a[1], b ? b->freq : 22050); return DD_OK; }
FN(b_GetStatus) { dxobj *b = ob(w, a[0], OB_DSB); uint32_t st = 0; if (b) { play_pos(w, b); if (b->playing) st = 1 | (b->looping ? 4u : 0u); } WR32(a[1], st); return DD_OK; }
FN(b_Lock) {                             /* (this, off, bytes, p1, n1, p2, n2, flags) */
    dxobj *b = ob(w, a[0], OB_DSB);
    uint32_t off = a[1], n = a[2];
    if (!b || !b->size) return DDERR_INVALIDPARAMS;
    if (a[7] & 1) off = (play_pos(w, b) + b->align * 64) % b->size;    /* FROMWRITECURSOR */
    if (a[7] & 2) { off = 0; n = b->size; }                            /* ENTIREBUFFER */
    off %= b->size;
    if (n > b->size) n = b->size;
    if (off + n <= b->size) {
        WR32(a[3], b->mem + off); WR32(a[4], n);
        if (a[5]) WR32(a[5], 0);
        if (a[6]) WR32(a[6], 0);
    } else {
        WR32(a[3], b->mem + off); WR32(a[4], b->size - off);
        if (a[5]) WR32(a[5], b->mem);
        if (a[6]) WR32(a[6], n - (b->size - off));
    }
    return DD_OK;
}
FN(b_Play) { dxobj *b = ob(w, a[0], OB_DSB); if (!b) return DDERR_INVALIDPARAMS; if (!b->playing) { b->t0 = now_ms(w); b->playing = 1; } b->looping = (a[3] & 1) != 0; ds_host(w, b, DS_START, 0); return DD_OK; }
FN(b_SetCurrentPosition) { dxobj *b = ob(w, a[0], OB_DSB); if (!b) return DDERR_INVALIDPARAMS; b->pos0 = b->size ? a[1] % b->size : 0; b->t0 = now_ms(w); ds_host(w, b, DS_SET, b->pos0 + 1u); return DD_OK; }
FN(b_SetFormat) { dxobj *b = ob(w, a[0], OB_DSB); if (b && a[1]) { b->chans = x86_rd16(C, a[1] + 2); b->rate = RD32(a[1] + 4); b->bps = RD32(a[1] + 8); b->align = x86_rd16(C, a[1] + 12); b->bits = x86_rd16(C, a[1] + 14); } return DD_OK; }
FN(b_SetVolume) { dxobj *b = ob(w, a[0], OB_DSB); if (b) { b->vol = (int32_t)a[1]; ds_host(w, b, DS_SET, 0); } return DD_OK; }
FN(b_SetPan) { dxobj *b = ob(w, a[0], OB_DSB); if (b) { b->pan = (int32_t)a[1]; ds_host(w, b, DS_SET, 0); } return DD_OK; }
FN(b_SetFrequency) { dxobj *b = ob(w, a[0], OB_DSB); if (b && a[1]) { b->freq = a[1]; ds_host(w, b, DS_SET, 0); } return DD_OK; }
FN(b_Stop) { dxobj *b = ob(w, a[0], OB_DSB); if (b) { b->pos0 = play_pos(w, b); b->playing = 0; ds_host(w, b, DS_STOP, 0); } return DD_OK; }
FN(b_Unlock) { UNUSED; return DD_OK; }
FN(x_DirectSoundCreate) {                /* (guid, pp, outer) */
    int i = obj_new(w, OB_DS, VT_DS);
    if (i < 0) return E_FAIL_;
    WR32(a[1], g_o[i].guest);
    return DD_OK;
}
FN(x_DirectSoundEnumerateA) {            /* (callback(guid, desc, module, ctx), ctx) */
    uint32_t d = qw_alloc(w, 64);
    static const char desc[] = "QRX DirectSound (silent)";
    uint32_t i;
    for (i = 0; i < sizeof(desc); i++) x86_wr8(C, d + i, (uint8_t)desc[i]);
    qw_call(w, a[0], 4, 0, d, d + sizeof(desc) - 1, a[1]);
    return DD_OK;
}

/* ================================================================== DirectInput */
/* Windows virtual-key -> DirectInput scan code (DIK_*) */
static uint8_t vk2dik(int vk)
{
    static const uint8_t letters[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
    if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
    if (vk >= '1' && vk <= '9') return (uint8_t)(2 + vk - '1');
    if (vk == '0') return 0x0B;
    if (vk >= 0x70 && vk <= 0x79) return (uint8_t)(0x3B + vk - 0x70);   /* F1..F10 */
    switch (vk) {
    case 0x1B: return 0x01; case 0x0D: return 0x1C; case 0x20: return 0x39; case 0x09: return 0x0F; case 0x08: return 0x0E;
    case 0x10: case 0xA0: return 0x2A; case 0xA1: return 0x36; case 0x11: case 0xA2: return 0x1D; case 0xA3: return 0x9D;
    case 0x12: case 0xA4: return 0x38; case 0x26: return 0xC8; case 0x28: return 0xD0; case 0x25: return 0xCB; case 0x27: return 0xCD;
    case 0x2D: return 0xD2; case 0x2E: return 0xD3; case 0x24: return 0xC7; case 0x23: return 0xCF; case 0x21: return 0xC9; case 0x22: return 0xD1;
    case 0x7A: return 0x57; case 0x7B: return 0x58; case 0x13: return 0xC5;
    case 0xBD: return 0x0C; case 0xBB: return 0x0D; case 0xDB: return 0x1A; case 0xDD: return 0x1B; case 0xBA: return 0x27; case 0xDE: return 0x28;
    case 0xC0: return 0x29; case 0xDC: return 0x2B; case 0xBC: return 0x33; case 0xBE: return 0x34; case 0xBF: return 0x35;
    default: return 0;
    }
}
static void kbd_state(qwin *w, uint8_t *k)
{
    int vk;
    memset(k, 0, 256);
    for (vk = 1; vk < 256; vk++) if (w->keystate[vk] & 0x80) { uint8_t d = vk2dik(vk); if (d) k[d] = 0x80; }
}
FN(di_QueryInterface) { UNUSED; WR32(a[2], 0); return E_NOINTERFACE; }
/* IDirectInputDevice2A extends IDirectInputDeviceA: the same object with the longer vtable */
FN(dv_QueryInterface) {
    dxobj *d = ob(w, a[0], OB_DID);
    uint32_t d1 = a[1] ? RD32(a[1]) : 0;
    if (d && (d1 == 0x5944E680u || d1 == 0x5944E682u)) { addref(d); WR32(a[2], a[0]); return DD_OK; }
    WR32(a[2], 0);
    return E_NOINTERFACE;
}
FN(dv_SetEventNotification) { dxobj *d = ob(w, a[0], OB_DID); if (d) d->event = a[1]; return DD_OK; }
static int dev_pending(qwin *w, dxobj *d)
{
    if (d->kind == 1) { uint8_t k[256]; kbd_state(w, k); return memcmp(k, d->prevkeys, sizeof(k)) != 0; }
    return w->mouse_x != d->lastx || w->mouse_y != d->lasty ||
           (w->keystate[1] & 0x80) != d->prevbtn[0] || (w->keystate[2] & 0x80) != d->prevbtn[1] || (w->keystate[4] & 0x80) != d->prevbtn[2];
}
/* MsgWaitForMultipleObjects: index of the first handle that belongs to a DirectInput
 * device with new data, -1 if none */
int qw_dinput_ready(qwin *w, uint32_t handles, uint32_t n)
{
    uint32_t i;
    int k;
    for (i = 0; i < n && i < 64; i++) {
        uint32_t h = RD32(handles + i * 4);
        for (k = 0; k < DX_MAX; k++) if (g_o[k].type == OB_DID && g_o[k].event == h && h && dev_pending(w, &g_o[k])) return (int)i;
    }
    return -1;
}
FN(di_AddRef) { return addref(ob(w, a[0], OB_DI)); }
FN(di_Release) { dxobj *o = ob(w, a[0], OB_DI); if (!o) return 0; if (--o->refs > 0) return (uint32_t)o->refs; obj_free(w, o); return 0; }
FN(di_CreateDevice) {                    /* (this, guid, pp, outer) */
    uint32_t d1 = a[1] ? RD32(a[1]) : 0;
    int i, kind = d1 == 0x6F1D2B61u ? 1 : d1 == 0x6F1D2B60u ? 2 : 0;    /* GUID_SysKeyboard / GUID_SysMouse */
    if (!kind) { if (w->verbose) qw_log(w, "DirectInput: device %08x-... not available", d1); WR32(a[2], 0); return 0x80040154u; }   /* DIERR_DEVICENOTREG */
    i = obj_new(w, OB_DID, VT_DID);
    if (i < 0) return E_FAIL_;
    g_o[i].kind = kind;
    g_o[i].lastx = w->mouse_x; g_o[i].lasty = w->mouse_y;
    WR32(a[2], g_o[i].guest);
    return DD_OK;
}
FN(di_EnumDevices) { UNUSED; return DD_OK; }
FN(di_GetDeviceStatus) { UNUSED; return DD_OK; }
FN(dv_AddRef) { return addref(ob(w, a[0], OB_DID)); }
FN(dv_Release) { dxobj *o = ob(w, a[0], OB_DID); if (!o) return 0; if (--o->refs > 0) return (uint32_t)o->refs; obj_free(w, o); return 0; }
FN(dv_GetCapabilities) { dxobj *d = ob(w, a[0], OB_DID); if (!d) return DDERR_INVALIDPARAMS; WR32(a[1] + 4, 1); WR32(a[1] + 8, d->kind == 1 ? 0x13u : 0x12u); WR32(a[1] + 12, d->kind == 1 ? 0 : 2); WR32(a[1] + 16, d->kind == 1 ? 128u : 2u); WR32(a[1] + 20, 0); return DD_OK; }
FN(dv_ok) { UNUSED; return DD_OK; }
FN(dv_GetDeviceState) {                  /* (this, size, data) */
    dxobj *d = ob(w, a[0], OB_DID);
    if (!d) return DDERR_INVALIDPARAMS;
    if (d->kind == 1) {
        uint8_t k[256];
        uint32_t i;
        kbd_state(w, k);
        for (i = 0; i < a[1] && i < 256; i++) x86_wr8(C, a[2] + i, k[i]);
    } else {
        uint32_t i;
        for (i = 0; i < a[1]; i++) x86_wr8(C, a[2] + i, 0);
        if (a[1] >= 16) {
            WR32(a[2], (uint32_t)(w->mouse_x - d->lastx)); WR32(a[2] + 4, (uint32_t)(w->mouse_y - d->lasty)); WR32(a[2] + 8, 0);
            x86_wr8(C, a[2] + 12, w->keystate[1] & 0x80); x86_wr8(C, a[2] + 13, w->keystate[2] & 0x80); x86_wr8(C, a[2] + 14, w->keystate[4] & 0x80);
        }
        d->lastx = w->mouse_x; d->lasty = w->mouse_y;
    }
    return DD_OK;
}
FN(dv_GetDeviceData) {                   /* (this, objsize, data, inout, flags): keyboard changes since the last call */
    dxobj *d = ob(w, a[0], OB_DID);
    uint32_t max = a[3] ? RD32(a[3]) : 0, n = 0, sz = a[1] ? a[1] : 16, t;
    uint8_t k[256];
    int i;
    if (!d) return DDERR_INVALIDPARAMS;
    if (d->kind != 1) {                              /* mouse: DIMOFS_X 0, _Y 4, _BUTTON0.. 12.. */
        int dx = w->mouse_x - d->lastx, dy = w->mouse_y - d->lasty, b;
        t = (uint32_t)now_ms(w);
        if (dx && n < max) { if (a[2]) { uint32_t e = a[2] + n * sz; WR32(e, 0); WR32(e + 4, (uint32_t)dx); WR32(e + 8, t); WR32(e + 12, n); } n++; }
        if (dy && n < max) { if (a[2]) { uint32_t e = a[2] + n * sz; WR32(e, 4); WR32(e + 4, (uint32_t)dy); WR32(e + 8, t); WR32(e + 12, n); } n++; }
        for (b = 0; b < 3 && n < max; b++) {
            uint8_t st = w->keystate[b == 0 ? 1 : b == 1 ? 2 : 4] & 0x80;
            if (st == d->prevbtn[b]) continue;
            if (a[2]) { uint32_t e = a[2] + n * sz; WR32(e, 12u + (uint32_t)b); WR32(e + 4, st); WR32(e + 8, t); WR32(e + 12, n); }
            if (!(a[4] & 1)) d->prevbtn[b] = st;
            n++;
        }
        if (!(a[4] & 1)) { d->lastx = w->mouse_x; d->lasty = w->mouse_y; }
        if (a[3]) WR32(a[3], n);
        return DD_OK;
    }
    kbd_state(w, k);
    t = (uint32_t)now_ms(w);
    for (i = 0; i < 256 && n < max; i++) {
        if (k[i] == d->prevkeys[i]) continue;
        if (a[2]) {
            uint32_t e = a[2] + n * sz;
            WR32(e, (uint32_t)i); WR32(e + 4, k[i]); WR32(e + 8, t); WR32(e + 12, n);
        }
        if (!(a[4] & 1)) d->prevkeys[i] = k[i];          /* DIGDD_PEEK keeps the events */
        n++;
    }
    if (a[3]) WR32(a[3], n);
    return DD_OK;
}
FN(x_DirectInputCreateA) {               /* (hinst, version, pp, outer) */
    int i = obj_new(w, OB_DI, VT_DI);
    if (i < 0) return E_FAIL_;
    WR32(a[2], g_o[i].guest);
    return DD_OK;
}

/* ================================================================== vtables */
typedef struct { const char *name; uint32_t (*fn)(qwin *, uint32_t *); int nargs; const char *host; } vslot;
#define V(n, f, c) { n, f, c, NULL }

/* ================================================================== Direct3D 5, 6 and 7 */
/* The objects live here, because enumerating calls back into the program. Everything a frame
 * needs is a host function: a thunk with that name binds to the host on its first call. The
 * pipeline is the host's fixed-function one, without lighting or execute buffers. */
#define LG(n) "D3DL_" n
static uint32_t call_host(qwin *w, const char *name, int n, const uint32_t *args)
{
    int ok;
    return x86_call(C, qw_thunk(w, "d3dhost", name), n, args, &ok);
}
static void d3d_host_present(qwin *w) { uint32_t self = 0; call_host(w, LG("Present"), 1, &self); }
static uint32_t com_new(qwin *w, int vt, uint32_t data)
{
    uint32_t g = qw_alloc(w, 32);
    WR32(g, g_vt[vt]); WR32(g + 4, 1); WR32(g + 8, data); WR32(g + 12, 0);
    return g;
}
FN(lg_QI) {
    uint32_t d1 = a[1] ? RD32(a[1]) : 0;
    /* A device or root asked for a newer face of itself keeps its vtable if layouts match. */
    if (a[2]) WR32(a[2], 0);
    (void)d1;
    return E_NOINTERFACE;
}
FN(lg_AddRef) { uint32_t r = RD32(a[0] + 4) + 1; WR32(a[0] + 4, r); return r; }
FN(lg_Release) { uint32_t r = RD32(a[0] + 4); if (r) r--; WR32(a[0] + 4, r); return r; }
FN(lg_ok) { UNUSED; return DD_OK; }
static const uint8_t HAL_GUID[16] = { 0xE0,0x3D,0xE6,0x84,0xAA,0x46,0xCF,0x11,0x81,0x6F,0x00,0x00,0xC0,0x20,0x15,0x6E };
static void wr_mem(qwin *w, uint32_t p, const void *d, uint32_t n) { x86_write_mem(C, p, d, n); }
static void prim_caps(qwin *w, uint32_t p)
{
    uint32_t i;
    WR32(p, 56);
    for (i = 4; i < 48; i += 4) WR32(p + i, 0xFFFFFFFFu);
    WR32(p + 48, 32); WR32(p + 52, 32);
}
static void hal_desc(qwin *w, uint32_t p)
{
    uint32_t i;
    for (i = 0; i < 252; i += 4) WR32(p + i, 0);
    WR32(p, 252);
    WR32(p + 4, 0x7FFu);                 /* every field valid */
    WR32(p + 8, 2);                      /* RGB colour model */
    WR32(p + 12, 0xFFFFFu);              /* hardware rasterisation and friends */
    WR32(p + 16, 8); WR32(p + 20, 1);
    WR32(p + 24, 1);
    WR32(p + 28, 16);
    prim_caps(w, p + 44);
    prim_caps(w, p + 100);
    WR32(p + 156, 0x500);                /* render to 16 or 32 bits */
    WR32(p + 160, 0x700);                /* Z in 16, 24 or 32 bits */
    WR32(p + 164, 0);
    WR32(p + 168, 65535);
    WR32(p + 172, 1); WR32(p + 176, 1); WR32(p + 180, 2048); WR32(p + 184, 2048);
    WR32(p + 188, 1); WR32(p + 192, 1); WR32(p + 196, 2048); WR32(p + 200, 2048);
}
static uint32_t guest_str(qwin *w, const char *t)
{
    uint32_t n = (uint32_t)strlen(t) + 1, p = qw_alloc(w, n);
    wr_mem(w, p, t, n);
    return p;
}
FN(lg_EnumDevices23) {                     /* (this, callback, ctx) */
    uint32_t guid = qw_alloc(w, 16), hal = qw_alloc(w, 252), hel = qw_alloc(w, 252), args[6], i;
    int ok;
    wr_mem(w, guid, HAL_GUID, 16);
    hal_desc(w, hal);
    for (i = 0; i < 252; i += 4) WR32(hel + i, 0);
    WR32(hel, 252);
    args[0] = guid; args[1] = guest_str(w, "QRX host GPU"); args[2] = guest_str(w, "Direct3D HAL");
    args[3] = hal; args[4] = hel; args[5] = a[2];
    x86_call(C, a[1], 6, args, &ok);
    return DD_OK;
}
FN(lg_EnumDevices7) {                      /* callback(desc, name, D3DDEVICEDESC7, ctx) */
    uint32_t d = qw_alloc(w, 240), args[4], i;
    int ok;
    for (i = 0; i < 240; i += 4) WR32(d + i, 0);
    WR32(d, 0xFFFFFu);
    prim_caps(w, d + 4);
    prim_caps(w, d + 60);
    WR32(d + 116, 0x500); WR32(d + 120, 0x700);
    WR32(d + 124, 1); WR32(d + 128, 1); WR32(d + 132, 2048); WR32(d + 136, 2048);
    WR32(d + 140, 8192); WR32(d + 144, 2048); WR32(d + 148, 1);
    WR32(d + 176, 0x00100008u);            /* FVF: one texture stage */
    WR32(d + 180, 0xFFFFFFFFu);
    WR16(d + 184, 8); WR16(d + 186, 1);
    wr_mem(w, d + 196, HAL_GUID, 16);
    args[0] = guest_str(w, "QRX host GPU"); args[1] = guest_str(w, "Direct3D HAL"); args[2] = d; args[3] = a[2];
    x86_call(C, a[1], 4, args, &ok);
    return DD_OK;
}
static void pf_rgb(qwin *w, uint32_t p, uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t al)
{
    uint32_t i;
    for (i = 0; i < 32; i += 4) WR32(p + i, 0);
    WR32(p, 32); WR32(p + 4, al ? 0x41u : 0x40u); WR32(p + 12, bits);
    WR32(p + 16, r); WR32(p + 20, g); WR32(p + 24, b); WR32(p + 28, al);
}
/* Calls cb for the formats; as_desc: Device2's callback takes a whole surface description. */
static void enum_tex(qwin *w, uint32_t cb, uint32_t ctx, int as_desc)
{
    static const uint32_t f[][5] = { {16,0xF800,0x07E0,0x001F,0}, {16,0x7C00,0x03E0,0x001F,0x8000}, {16,0x0F00,0x00F0,0x000F,0xF000}, {32,0xFF0000,0xFF00,0xFF,0xFF000000u} };
    uint32_t mem = qw_alloc(w, 124), i, k, args[2];
    for (k = 0; k < 4; k++) {
        int ok;
        uint32_t r;
        if (as_desc) {
            for (i = 0; i < 108; i += 4) WR32(mem + i, 0);
            WR32(mem, 108); WR32(mem + 4, 0x1000);
            pf_rgb(w, mem + 72, f[k][0], f[k][1], f[k][2], f[k][3], f[k][4]);
            args[0] = mem;
        } else {
            pf_rgb(w, mem, f[k][0], f[k][1], f[k][2], f[k][3], f[k][4]);
            args[0] = mem;
        }
        args[1] = ctx;
        r = x86_call(C, cb, 2, args, &ok);
        if (!r) return;
    }
}
FN(lg_EnumTextureFormatsDesc) { enum_tex(w, a[1], a[2], 1); return DD_OK; }
FN(lg_EnumTextureFormatsPF) { enum_tex(w, a[1], a[2], 0); return DD_OK; }
FN(lg_EnumZBufferFormats) {               /* (this, clsid, cb, ctx) */
    uint32_t p = qw_alloc(w, 32), i, args[2];
    int ok;
    for (i = 0; i < 32; i += 4) WR32(p + i, 0);
    WR32(p, 32); WR32(p + 4, 0x400); WR32(p + 12, 16); WR32(p + 20, 0xFFFF);
    args[0] = p; args[1] = a[3];
    x86_call(C, a[2], 2, args, &ok);
    return DD_OK;
}
FN(lg_FindDevice) {                       /* (this, search, result) */
    uint32_t i;
    (void)a;
    WR32(a[2], 8 + 16 + 252 * 2 + 4);
    for (i = 4; i < 4 + 16; i += 4) WR32(a[2] + i, 0);
    wr_mem(w, a[2] + 4, HAL_GUID, 16);
    hal_desc(w, a[2] + 20);
    WR32(a[2] + 272, 252);
    return DD_OK;
}
static uint32_t lg_open_device(qwin *w, uint32_t surface, int vt)
{
    dxobj *s = sf(w, surface);
    uint32_t args[2], dev;
    if (!s) return 0;
    args[0] = (uint32_t)s->sw; args[1] = (uint32_t)s->sh;
    call_host(w, LG("Open"), 2, args);
    g_d3d_active = 1;
    dev = com_new(w, vt, surface);
    return dev;
}
enum { VT_D3D2 = 9, VT_D3D3, VT_D3D7, VT_DEV2, VT_DEV3, VT_DEV7, VT_VP3, VT_MAT3, VT_TEX2, VT_VB7, VT_LIGHT };
FN(lg_CreateDevice2) { uint32_t d = lg_open_device(w, a[2], VT_DEV2); if (!d) return E_FAIL_; WR32(a[3], d); return DD_OK; }
FN(lg_CreateDevice3) { uint32_t d = lg_open_device(w, a[2], VT_DEV3); if (!d) return E_FAIL_; WR32(a[3], d); return DD_OK; }
FN(lg_CreateDevice7) { uint32_t d = lg_open_device(w, a[2], VT_DEV7); if (!d) return E_FAIL_; WR32(a[3], d); return DD_OK; }
FN(lg_CreateMaterial) { WR32(a[1], com_new(w, VT_MAT3, 0)); return DD_OK; }
FN(lg_CreateLight) { WR32(a[1], com_new(w, VT_LIGHT, 0)); return DD_OK; }
FN(lg_CreateViewport) { WR32(a[1], com_new(w, VT_VP3, 0)); return DD_OK; }
/* (this, D3DVERTEXBUFFERDESC {size, caps, fvf, count}, pp, ...) */
FN(lg_CreateVertexBuffer) {
    uint32_t fvf = RD32(a[1] + 8), n = RD32(a[1] + 12), stride = 0, g, f = fvf;
    switch (f & 0xEu) { case 2: stride = 12; break; case 4: stride = 16; break; default: stride = 12 + 4u * (((f & 0xEu) - 6u) / 2u + 1u); break; }
    if (f & 0x10u) stride += 12;
    if (f & 0x20u) stride += 4;
    if (f & 0x40u) stride += 4;
    if (f & 0x80u) stride += 4;
    stride += 8u * ((f >> 8) & 15u);
    g = com_new(w, VT_VB7, qw_alloc(w, n * stride + 16));
    WR32(g + 12, fvf); WR32(g + 16, n); WR32(g + 20, stride);
    WR32(a[2], g);
    return DD_OK;
}
FN(lg_VB_Lock) {                          /* (this, flags, pp, size) */
    if (a[2]) WR32(a[2], RD32(a[0] + 8));
    if (a[3]) WR32(a[3], RD32(a[0] + 16) * RD32(a[0] + 20));
    return DD_OK;
}
FN(lg_VB_GetDesc) { WR32(a[1], 16); WR32(a[1] + 4, 0); WR32(a[1] + 8, RD32(a[0] + 12)); WR32(a[1] + 12, RD32(a[0] + 16)); return DD_OK; }
static uint32_t d3d_new_texture(qwin *w, uint32_t surface, int v2) { (void)v2; return com_new(w, VT_TEX2, surface); }
static uint32_t d3d_new_root(qwin *w, int ver) { return com_new(w, ver == 7 ? VT_D3D7 : ver == 3 ? VT_D3D3 : VT_D3D2, 0); }
FN(lg_Tex_GetHandle) { if (a[2]) WR32(a[2], RD32(a[0] + 8)); (void)w; return DD_OK; }  /* the surface is the handle */
/* Materials: D3DMATERIAL {size, diffuse r,g,b,a ...}; the handle is the object. */
FN(lg_Mat_SetMaterial) {
    uint32_t i;
    for (i = 0; i < 4; i++) WR32(a[0] + 16 + i * 4, RD32(a[1] + 4 + i * 4));
    return DD_OK;
}
FN(lg_Mat_GetHandle) { if (a[2]) WR32(a[2], a[0]); (void)w; return DD_OK; }
static uint32_t col_byte(uint32_t f)
{
    float v;
    memcpy(&v, &f, 4);
    if (!(v > 0)) return 0;
    return v >= 1 ? 255u : (uint32_t)(v * 255.f);
}
FN(lg_VP_SetBackground) { WR32(a[0] + 12, a[1]); (void)w; return DD_OK; }
static uint32_t vp_color(qwin *w, uint32_t vp)
{
    uint32_t m = RD32(vp + 12);
    if (!m) return 0xFF000000u;
    return col_byte(RD32(m + 28)) << 24 | col_byte(RD32(m + 16)) << 16 | col_byte(RD32(m + 20)) << 8 | col_byte(RD32(m + 24));
}
static uint32_t lg_clear(qwin *w, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, uint32_t z, uint32_t st)
{
    uint32_t args[7];
    args[0] = 0; args[1] = count; args[2] = rects; args[3] = flags; args[4] = color; args[5] = z; args[6] = st;
    return call_host(w, LG("Clear"), 7, args);
}
FN(lg_VP_Clear) { return lg_clear(w, a[1], a[2], a[3], vp_color(w, a[0]), 0x3F800000u, 0); }
FN(lg_VP_Clear2) { return lg_clear(w, a[1], a[2], a[3], a[4], a[5], a[6]); }
static uint32_t g_vp_current;
/* D3DVIEWPORT and D3DVIEWPORT2 both begin size, x, y, w, h. */
static void lg_apply_viewport(qwin *w, uint32_t vp)
{
    uint32_t args[4], i;
    for (i = 0; i < 4; i++) args[i] = RD32(vp + 16 + 16 + i * 4);
    call_host(w, LG("Viewport"), 4, args);
}
FN(lg_VP_SetViewport) {
    uint32_t i;
    for (i = 0; i < 4; i++) WR32(a[0] + 32 + i * 4, RD32(a[1] + 4 + i * 4));
    if (a[0] == g_vp_current) lg_apply_viewport(w, a[0]);
    return DD_OK;
}
FN(lg_VP_GetViewport) {
    uint32_t i;
    for (i = 0; i < 4; i++) WR32(a[1] + 4 + i * 4, RD32(a[0] + 32 + i * 4));
    return DD_OK;
}
FN(lg_Dev_SetCurrentViewport) { g_vp_current = a[1]; if (a[1]) lg_apply_viewport(w, a[1]); return DD_OK; }
FN(lg_Dev_GetCurrentViewport) { WR32(a[1], g_vp_current); return g_vp_current ? DD_OK : E_FAIL_; }
FN(lg_Dev_GetCaps23) {
    if (a[1]) hal_desc(w, a[1]);
    if (a[2]) { uint32_t i; for (i = 4; i < 252; i += 4) WR32(a[2] + i, 0); }
    return DD_OK;
}
FN(lg_Dev7_SetViewport) {
    uint32_t args[4], i;
    for (i = 0; i < 4; i++) args[i] = RD32(a[1] + i * 4);
    call_host(w, LG("Viewport"), 4, args);
    return DD_OK;
}
FN(lg_Dev_GetRenderTarget) { WR32(a[1], RD32(a[0] + 8)); return DD_OK; }
#define VH(n, host, c) { n, NULL, c, host }
#define VG(n, f, c) { n, f, c, NULL }
static const vslot vt_dd[] = {
    V("QueryInterface", dd_QueryInterface, 3), V("AddRef", dd_AddRef, 1), V("Release", dd_Release, 1), V("Compact", dd_ok, 1),
    V("CreateClipper", dd_CreateClipper, 4), V("CreatePalette", dd_CreatePalette, 5), V("CreateSurface", dd_CreateSurface, 4),
    V("DuplicateSurface", NULL, 3), V("EnumDisplayModes", dd_EnumDisplayModes, 5), V("EnumSurfaces", dd_ok, 5),
    V("FlipToGDISurface", dd_ok, 1), V("GetCaps", dd_GetCaps, 3), V("GetDisplayMode", dd_GetDisplayMode, 2),
    V("GetFourCCCodes", dd_GetFourCCCodes, 3), V("GetGDISurface", NULL, 2), V("GetMonitorFrequency", dd_GetMonitorFrequency, 2),
    V("GetScanLine", dd_GetScanLine, 2), V("GetVerticalBlankStatus", dd_GetVerticalBlankStatus, 2), V("Initialize", dd_ok, 2),
    V("RestoreDisplayMode", dd_RestoreDisplayMode, 1), V("SetCooperativeLevel", dd_SetCooperativeLevel, 3), V("SetDisplayMode", dd_SetDisplayMode, 4),
    V("WaitForVerticalBlank", dd_ok, 3), { NULL, NULL, 0, NULL }
};
FN(dd_GetDeviceIdentifier) {
    uint32_t i;
    static const char n[] = "QRX ARMithlon host GPU";
    for (i = 0; i < 1072; i += 4) WR32(a[1] + i, 0);
    for (i = 0; i < sizeof(n); i++) { WR8(a[1] + i, (uint8_t)n[i]); WR8(a[1] + 512 + i, (uint8_t)n[i]); }
    return DD_OK;
}
static const vslot vt_dd2[] = {
    V("QueryInterface", dd_QueryInterface, 3), V("AddRef", dd_AddRef, 1), V("Release", dd_Release, 1), V("Compact", dd_ok, 1),
    V("CreateClipper", dd_CreateClipper, 4), V("CreatePalette", dd_CreatePalette, 5), V("CreateSurface", dd_CreateSurface, 4),
    V("DuplicateSurface", NULL, 3), V("EnumDisplayModes", dd_EnumDisplayModes, 5), V("EnumSurfaces", dd_ok, 5),
    V("FlipToGDISurface", dd_ok, 1), V("GetCaps", dd_GetCaps, 3), V("GetDisplayMode", dd_GetDisplayMode, 2),
    V("GetFourCCCodes", dd_GetFourCCCodes, 3), V("GetGDISurface", NULL, 2), V("GetMonitorFrequency", dd_GetMonitorFrequency, 2),
    V("GetScanLine", dd_GetScanLine, 2), V("GetVerticalBlankStatus", dd_GetVerticalBlankStatus, 2), V("Initialize", dd_ok, 2),
    V("RestoreDisplayMode", dd_RestoreDisplayMode, 1), V("SetCooperativeLevel", dd_SetCooperativeLevel, 3), V("SetDisplayMode", dd2_SetDisplayMode, 6),
    V("WaitForVerticalBlank", dd_ok, 3), V("GetAvailableVidMem", dd2_GetAvailableVidMem, 4),
    V("GetSurfaceFromDC", dd_ok, 3), V("RestoreAllSurfaces", dd_ok, 1), V("TestCooperativeLevel", dd_ok, 1),
    V("GetDeviceIdentifier", dd_GetDeviceIdentifier, 3), V("StartModeTest", dd_ok, 4), V("EvaluateMode", dd_ok, 3), { NULL, NULL, 0, NULL }
};
static const vslot vt_surf[] = {           /* IDirectDrawSurface + Surface2 + Surface3 tail */
    V("QueryInterface", s_QueryInterface, 3), V("AddRef", s_AddRef, 1), V("Release", s_Release, 1), V("AddAttachedSurface", dd_ok, 2),
    V("AddOverlayDirtyRect", dd_ok, 2), V("Blt", s_Blt, 6), V("BltBatch", NULL, 4), V("BltFast", s_BltFast, 6),
    V("DeleteAttachedSurface", dd_ok, 3), V("EnumAttachedSurfaces", s_EnumAttachedSurfaces, 3), V("EnumOverlayZOrders", dd_ok, 4),
    V("Flip", s_Flip, 3), V("GetAttachedSurface", s_GetAttachedSurface, 3), V("GetBltStatus", dd_ok, 2), V("GetCaps", s_GetCaps, 2),
    V("GetClipper", NULL, 2), V("GetColorKey", s_GetColorKey, 3), V("GetDC", s_GetDC, 2), V("GetFlipStatus", dd_ok, 2),
    V("GetOverlayPosition", NULL, 3), V("GetPalette", s_GetPalette, 2), V("GetPixelFormat", s_GetPixelFormat, 2),
    V("GetSurfaceDesc", s_GetSurfaceDesc, 2), V("Initialize", dd_ok, 3), V("IsLost", s_IsLost, 1), V("Lock", s_Lock, 5),
    V("ReleaseDC", s_ReleaseDC, 2), V("Restore", dd_ok, 1), V("SetClipper", dd_ok, 2), V("SetColorKey", s_SetColorKey, 3),
    V("SetOverlayPosition", dd_ok, 3), V("SetPalette", s_SetPalette, 2), V("Unlock", s_Unlock, 2), V("UpdateOverlay", dd_ok, 6),
    V("UpdateOverlayDisplay", dd_ok, 2), V("UpdateOverlayZOrder", dd_ok, 3),
    V("GetDDInterface", s_GetDDInterface, 2), V("PageLock", dd_ok, 2), V("PageUnlock", dd_ok, 2), V("SetSurfaceDesc", s_SetSurfaceDesc, 3),
    /* IDirectDrawSurface4 and 7 only add methods */
    V("SetPrivateData", dd_ok, 5), V("GetPrivateData", dd_ok, 4), V("FreePrivateData", dd_ok, 2), V("GetUniquenessValue", dd_ok, 2),
    V("ChangeUniquenessValue", dd_ok, 1), V("SetPriority", dd_ok, 2), V("GetPriority", dd_ok, 2), V("SetLOD", dd_ok, 2), V("GetLOD", dd_ok, 2),
    { NULL, NULL, 0, NULL }
};
static const vslot vt_pal[] = {
    V("QueryInterface", p_QueryInterface, 3), V("AddRef", p_AddRef, 1), V("Release", p_Release, 1), V("GetCaps", p_GetCaps, 2),
    V("GetEntries", p_GetEntries, 5), V("Initialize", dd_ok, 4), V("SetEntries", p_SetEntries, 5), { NULL, NULL, 0, NULL }
};
static const vslot vt_clip[] = {
    V("QueryInterface", p_QueryInterface, 3), V("AddRef", c_AddRef, 1), V("Release", c_Release, 1), V("GetClipList", NULL, 4),
    V("GetHWnd", c_GetHWnd, 2), V("Initialize", dd_ok, 3), V("IsClipListChanged", dd_ok, 2), V("SetClipList", dd_ok, 3),
    V("SetHWnd", c_SetHWnd, 3), { NULL, NULL, 0, NULL }
};
static const vslot vt_ds[] = {
    V("QueryInterface", ds_QueryInterface, 3), V("AddRef", ds_AddRef, 1), V("Release", ds_Release, 1),
    V("CreateSoundBuffer", ds_CreateSoundBuffer, 4), V("GetCaps", ds_GetCaps, 2), V("DuplicateSoundBuffer", ds_DuplicateSoundBuffer, 3),
    V("SetCooperativeLevel", dd_ok, 3), V("Compact", dd_ok, 1), V("GetSpeakerConfig", ds_GetSpeakerConfig, 2),
    V("SetSpeakerConfig", dd_ok, 2), V("Initialize", dd_ok, 2), { NULL, NULL, 0, NULL }
};
static const vslot vt_dsb[] = {
    V("QueryInterface", ds_QueryInterface, 3), V("AddRef", b_AddRef, 1), V("Release", b_Release, 1), V("GetCaps", b_GetCaps, 2),
    V("GetCurrentPosition", b_GetCurrentPosition, 3), V("GetFormat", b_GetFormat, 4), V("GetVolume", b_GetVolume, 2),
    V("GetPan", b_GetPan, 2), V("GetFrequency", b_GetFrequency, 2), V("GetStatus", b_GetStatus, 2), V("Initialize", dd_ok, 3),
    V("Lock", b_Lock, 8), V("Play", b_Play, 4), V("SetCurrentPosition", b_SetCurrentPosition, 2), V("SetFormat", b_SetFormat, 2),
    V("SetVolume", b_SetVolume, 2), V("SetPan", b_SetPan, 2), V("SetFrequency", b_SetFrequency, 2), V("Stop", b_Stop, 1),
    V("Unlock", b_Unlock, 5), V("Restore", dd_ok, 1), { NULL, NULL, 0, NULL }
};
static const vslot vt_di[] = {
    V("QueryInterface", di_QueryInterface, 3), V("AddRef", di_AddRef, 1), V("Release", di_Release, 1),
    V("CreateDevice", di_CreateDevice, 4), V("EnumDevices", di_EnumDevices, 5), V("GetDeviceStatus", di_GetDeviceStatus, 2),
    V("RunControlPanel", dd_ok, 3), V("Initialize", dd_ok, 3), { NULL, NULL, 0, NULL }
};
static const vslot vt_did[] = {             /* IDirectInputDevice2A (prefix: IDirectInputDeviceA) */
    V("QueryInterface", dv_QueryInterface, 3), V("AddRef", dv_AddRef, 1), V("Release", dv_Release, 1),
    V("GetCapabilities", dv_GetCapabilities, 2), V("EnumObjects", dv_ok, 4), V("GetProperty", dv_ok, 3), V("SetProperty", dv_ok, 3),
    V("Acquire", dv_ok, 1), V("Unacquire", dv_ok, 1), V("GetDeviceState", dv_GetDeviceState, 3), V("GetDeviceData", dv_GetDeviceData, 5),
    V("SetDataFormat", dv_ok, 2), V("SetEventNotification", dv_SetEventNotification, 2), V("SetCooperativeLevel", dv_ok, 3),
    V("GetObjectInfo", NULL, 4), V("GetDeviceInfo", NULL, 2), V("RunControlPanel", dv_ok, 3), V("Initialize", dv_ok, 4),
    V("CreateEffect", NULL, 5), V("EnumEffects", dv_ok, 4), V("GetEffectInfo", NULL, 3), V("GetForceFeedbackState", NULL, 2),
    V("SendForceFeedbackCommand", dv_ok, 2), V("EnumCreatedEffectObjects", dv_ok, 4), V("Escape", NULL, 2), V("Poll", dv_ok, 1),
    V("SendDeviceData", dv_ok, 5), { NULL, NULL, 0, NULL }
};

/* HLE table entries for the vtable slots ("ddraw", "IDirectDraw::CreateSurface" ...) */

#define VD(n, c) VG(n, lg_ok, c)
#define LG_COM VG("QueryInterface", lg_QI, 3), VG("AddRef", lg_AddRef, 1), VG("Release", lg_Release, 1)
static const vslot vt_d3d2[] = { LG_COM, VG("EnumDevices", lg_EnumDevices23, 3), VG("CreateLight", lg_CreateLight, 3), VG("CreateMaterial", lg_CreateMaterial, 3),
    VG("CreateViewport", lg_CreateViewport, 3), VG("FindDevice", lg_FindDevice, 3), VG("CreateDevice", lg_CreateDevice2, 4), { NULL, NULL, 0, NULL } };
static const vslot vt_d3d3[] = { LG_COM, VG("EnumDevices", lg_EnumDevices23, 3), VG("CreateLight", lg_CreateLight, 3), VG("CreateMaterial", lg_CreateMaterial, 3),
    VG("CreateViewport", lg_CreateViewport, 3), VG("FindDevice", lg_FindDevice, 3), VG("CreateDevice", lg_CreateDevice3, 5), VG("CreateVertexBuffer", lg_CreateVertexBuffer, 5),
    VG("EnumZBufferFormats", lg_EnumZBufferFormats, 4), VD("EvictManagedTextures", 1), { NULL, NULL, 0, NULL } };
static const vslot vt_d3d7[] = { LG_COM, VG("EnumDevices", lg_EnumDevices7, 3), VG("CreateDevice", lg_CreateDevice7, 4), VG("CreateVertexBuffer", lg_CreateVertexBuffer, 4),
    VG("EnumZBufferFormats", lg_EnumZBufferFormats, 4), VD("EvictManagedTextures", 1), { NULL, NULL, 0, NULL } };
static const vslot vt_dev2[] = { LG_COM, VG("GetCaps", lg_Dev_GetCaps23, 3), VD("SwapTextureHandles", 3), VD("GetStats", 2), VD("AddViewport", 2), VD("DeleteViewport", 2),
    VD("NextViewport", 4), VG("EnumTextureFormats", lg_EnumTextureFormatsDesc, 3), VH("BeginScene", LG("Scene"), 1), VH("EndScene", LG("Scene"), 1), VD("GetDirect3D", 2),
    VG("SetCurrentViewport", lg_Dev_SetCurrentViewport, 2), VG("GetCurrentViewport", lg_Dev_GetCurrentViewport, 2), VD("SetRenderTarget", 3), VG("GetRenderTarget", lg_Dev_GetRenderTarget, 2),
    VD("Begin", 4), VD("BeginIndexed", 6), VD("Vertex", 2), VD("Index", 2), VD("End", 2), VD("GetRenderState", 3), VH("SetRenderState", LG("SetRenderState"), 3),
    VD("GetLightState", 3), VD("SetLightState", 3), VH("SetTransform", LG("SetTransform"), 3), VD("GetTransform", 3), VD("MultiplyTransform", 3),
    VH("DrawPrimitive", LG("Draw2"), 6), VH("DrawIndexedPrimitive", LG("DrawIndexed2"), 8), VD("SetClipStatus", 2), VD("GetClipStatus", 2), { NULL, NULL, 0, NULL } };
static const vslot vt_dev3[] = { LG_COM, VG("GetCaps", lg_Dev_GetCaps23, 3), VD("GetStats", 2), VD("AddViewport", 2), VD("DeleteViewport", 2), VD("NextViewport", 4),
    VG("EnumTextureFormats", lg_EnumTextureFormatsPF, 3), VH("BeginScene", LG("Scene"), 1), VH("EndScene", LG("Scene"), 1), VD("GetDirect3D", 2),
    VG("SetCurrentViewport", lg_Dev_SetCurrentViewport, 2), VG("GetCurrentViewport", lg_Dev_GetCurrentViewport, 2), VD("SetRenderTarget", 3), VG("GetRenderTarget", lg_Dev_GetRenderTarget, 2),
    VD("Begin", 4), VD("BeginIndexed", 6), VD("Vertex", 2), VD("Index", 2), VD("End", 2), VD("GetRenderState", 3), VH("SetRenderState", LG("SetRenderState"), 3),
    VD("GetLightState", 3), VD("SetLightState", 3), VH("SetTransform", LG("SetTransform"), 3), VD("GetTransform", 3), VD("MultiplyTransform", 3),
    VH("DrawPrimitive", LG("DrawFVF"), 6), VH("DrawIndexedPrimitive", LG("DrawIndexedFVF"), 8), VD("SetClipStatus", 2), VD("GetClipStatus", 2),
    VD("DrawPrimitiveStrided", 6), VD("DrawIndexedPrimitiveStrided", 8), VH("DrawPrimitiveVB", LG("DrawVB"), 6), VH("DrawIndexedPrimitiveVB", LG("DrawIndexedVB3"), 6),
    VD("ComputeSphereVisibility", 6), VD("GetTexture", 3), VH("SetTexture", LG("SetTexture3"), 3), VD("GetTextureStageState", 4), VH("SetTextureStageState", LG("TextureStage"), 4),
    VD("ValidateDevice", 2), { NULL, NULL, 0, NULL } };
static const vslot vt_dev7[] = { LG_COM, VD("GetCaps", 2), VG("EnumTextureFormats", lg_EnumTextureFormatsPF, 3), VH("BeginScene", LG("Scene"), 1), VH("EndScene", LG("Scene"), 1),
    VD("GetDirect3D", 2), VD("SetRenderTarget", 3), VG("GetRenderTarget", lg_Dev_GetRenderTarget, 2), VH("Clear", LG("Clear"), 7), VH("SetTransform", LG("SetTransform"), 3), VD("GetTransform", 3),
    VG("SetViewport", lg_Dev7_SetViewport, 2), VD("MultiplyTransform", 3), VD("GetViewport", 2), VD("SetMaterial", 2), VD("GetMaterial", 2), VD("SetLight", 3), VD("GetLight", 3),
    VH("SetRenderState", LG("SetRenderState"), 3), VD("GetRenderState", 3), VD("BeginStateBlock", 1), VD("EndStateBlock", 2), VD("PreLoad", 2),
    VH("DrawPrimitive", LG("DrawFVF"), 6), VH("DrawIndexedPrimitive", LG("DrawIndexedFVF"), 8), VD("SetClipStatus", 2), VD("GetClipStatus", 2),
    VD("DrawPrimitiveStrided", 6), VD("DrawIndexedPrimitiveStrided", 8), VH("DrawPrimitiveVB", LG("DrawVB"), 6), VH("DrawIndexedPrimitiveVB", LG("DrawIndexedVB7"), 8),
    VD("ComputeSphereVisibility", 6), VD("GetTexture", 3), VH("SetTexture", LG("SetTexture7"), 3), VD("GetTextureStageState", 4), VH("SetTextureStageState", LG("TextureStage"), 4),
    VD("ValidateDevice", 2), VD("ApplyStateBlock", 2), VD("CaptureStateBlock", 2), VD("DeleteStateBlock", 2), VD("CreateStateBlock", 3), VD("Load", 6), VD("LightEnable", 3),
    VD("GetLightEnable", 3), VD("SetClipPlane", 3), VD("GetClipPlane", 3), VD("GetInfo", 4), { NULL, NULL, 0, NULL } };
static const vslot vt_vp3[] = { LG_COM, VD("Initialize", 2), VG("GetViewport", lg_VP_GetViewport, 2), VG("SetViewport", lg_VP_SetViewport, 2), VD("TransformVertices", 5), VD("LightElements", 3),
    VG("SetBackground", lg_VP_SetBackground, 2), VD("GetBackground", 3), VD("SetBackgroundDepth", 2), VD("GetBackgroundDepth", 3), VG("Clear", lg_VP_Clear, 4), VD("AddLight", 2), VD("DeleteLight", 2),
    VD("NextLight", 4), VG("GetViewport2", lg_VP_GetViewport, 2), VG("SetViewport2", lg_VP_SetViewport, 2), VD("SetBackgroundDepth2", 2), VD("GetBackgroundDepth2", 3), VG("Clear2", lg_VP_Clear2, 7), { NULL, NULL, 0, NULL } };
static const vslot vt_mat3[] = { LG_COM, VG("SetMaterial", lg_Mat_SetMaterial, 2), VD("GetMaterial", 2), VG("GetHandle", lg_Mat_GetHandle, 3), { NULL, NULL, 0, NULL } };
static const vslot vt_tex2[] = { LG_COM, VG("GetHandle", lg_Tex_GetHandle, 3), VD("PaletteChanged", 3), VD("Load", 2), { NULL, NULL, 0, NULL } };
static const vslot vt_vb7[] = { LG_COM, VG("Lock", lg_VB_Lock, 4), VD("Unlock", 1), VD("ProcessVertices", 8), VG("GetVertexBufferDesc", lg_VB_GetDesc, 2), VD("Optimize", 3), VD("ProcessVerticesStrided", 8), { NULL, NULL, 0, NULL } };
static const vslot vt_light[] = { LG_COM, VD("Initialize", 2), VD("SetLight", 2), VD("GetLight", 2), { NULL, NULL, 0, NULL } };
#define VMAX 60
#define NIFACES 20
static qhle g_slots[NIFACES][VMAX];
static const struct { const vslot *t; const char *dll, *iface; } g_ifaces[NIFACES] = {
    { vt_dd, "ddraw", "IDirectDraw" }, { vt_dd2, "ddraw", "IDirectDraw2" }, { vt_surf, "ddraw", "IDirectDrawSurface" },
    { vt_pal, "ddraw", "IDirectDrawPalette" }, { vt_clip, "ddraw", "IDirectDrawClipper" }, { vt_ds, "dsound", "IDirectSound" },
    { vt_dsb, "dsound", "IDirectSoundBuffer" }, { vt_di, "dinput", "IDirectInputA" }, { vt_did, "dinput", "IDirectInputDeviceA" },
    { vt_d3d2, "ddraw", "IDirect3D2" }, { vt_d3d3, "ddraw", "IDirect3D3" }, { vt_d3d7, "ddraw", "IDirect3D7" },
    { vt_dev2, "ddraw", "IDirect3DDevice2" }, { vt_dev3, "ddraw", "IDirect3DDevice3" }, { vt_dev7, "ddraw", "IDirect3DDevice7" },
    { vt_vp3, "ddraw", "IDirect3DViewport3" }, { vt_mat3, "ddraw", "IDirect3DMaterial3" }, { vt_tex2, "ddraw", "IDirect3DTexture2" },
    { vt_vb7, "ddraw", "IDirect3DVertexBuffer7" }, { vt_light, "ddraw", "IDirect3DLight" }
};
static char g_names[NIFACES][VMAX][64];
static uint32_t dx_unimpl(qwin *w, uint32_t *a) { (void)a; qw_log(w, "DirectX method not implemented"); return E_FAIL_; }

/* build the guest vtables on first use */
static void dx_init(qwin *w)
{
    static int done;
    int i, k;
    if (done) return;
    done = 1;
    for (i = 0; i < NIFACES; i++) {
        int n = 0;
        while (g_ifaces[i].t[n].name) n++;
        g_vt[i] = qw_alloc(w, (uint32_t)n * 4u);
        for (k = 0; k < n && k < VMAX; k++) {
            const vslot *v = &g_ifaces[i].t[k];
            snprintf(g_names[i][k], sizeof(g_names[i][k]), "%s::%s", g_ifaces[i].iface, v->name);
            /* methods without an implementation report their name when called */
            g_slots[i][k].dll = g_ifaces[i].dll; g_slots[i][k].name = g_names[i][k]; g_slots[i][k].nargs = v->nargs;
            g_slots[i][k].fn = v->fn ? v->fn : dx_unimpl;
            WR32(g_vt[i] + (uint32_t)k * 4u, v->host ? qw_thunk(w, "d3dhost", v->host) : qw_thunk(w, g_ifaces[i].dll, g_names[i][k]));
        }
    }
}
const qhle *qw_find_dx(const char *name)
{
    int i, k;
    for (i = 0; i < NIFACES; i++) for (k = 0; k < VMAX && g_slots[i][k].name; k++) if (!strcmp(g_slots[i][k].name, name)) return &g_slots[i][k];
    return NULL;
}
FN(x_DirectDrawCreate_init) { dx_init(w); return x_DirectDrawCreate(w, a); }
FN(x_DirectSoundCreate_init) { dx_init(w); return x_DirectSoundCreate(w, a); }
FN(x_DirectInputCreateA_init) { dx_init(w); return x_DirectInputCreateA(w, a); }

static uint32_t x_DirectDrawCreateClipper_init(qwin *w, uint32_t *a)
{
    int i;
    dx_init(w);
    i = obj_new(w, OB_CLIP, VT_CLIP);
    if (i < 0) return E_FAIL_;
    WR32(a[1], g_o[i].guest);
    return DD_OK;
}
/* (guid, pp, iid, outer): the object has every method of IDirectDraw7 */
static uint32_t x_DirectDrawCreateEx_init(qwin *w, uint32_t *a)
{
    uint32_t r, g;
    int i;
    dx_init(w);
    r = x_DirectDrawCreate(w, a);
    if (r != DD_OK) return r;
    g = RD32(a[1]);
    i = (int)RD32(g + 4);
    WR32(g, g_vt[VT_DD2]);
    (void)i;
    return DD_OK;
}
/* callback(guid, desc, name, ctx, hmonitor) */
static uint32_t x_DirectDrawEnumerateExA(qwin *w, uint32_t *a)
{
    uint32_t args[5];
    int ok;
    dx_init(w);
    args[0] = 0; args[1] = guest_str(w, "Primary Display Driver"); args[2] = guest_str(w, "display"); args[3] = a[1]; args[4] = 0;
    x86_call(C, a[0], 5, args, &ok);
    return DD_OK;
}
const qhle qw_dx[] = {
    { "ddraw", "DirectDrawCreateEx", 4, x_DirectDrawCreateEx_init }, { "ddraw", "DirectDrawEnumerateExA", 3, x_DirectDrawEnumerateExA },
    { "ddraw", "DirectDrawCreateClipper", 3, x_DirectDrawCreateClipper_init },
    { "ddraw", "DirectDrawCreate", 3, x_DirectDrawCreate_init }, { "ddraw", "DirectDrawEnumerateA", 2, x_DirectDrawEnumerateA },
    { "dsound", "DirectSoundCreate", 3, x_DirectSoundCreate_init }, { "dsound", "DirectSoundEnumerateA", 2, x_DirectSoundEnumerateA },
    { "dsound", "#1", 3, x_DirectSoundCreate_init }, { "dsound", "#2", 2, x_DirectSoundEnumerateA },
    { "dinput", "DirectInputCreateA", 4, x_DirectInputCreateA_init },
    { NULL, NULL, 0, NULL }
};
