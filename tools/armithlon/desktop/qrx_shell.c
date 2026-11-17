/* qrx_shell.c: the desktop's shell, included into QRXDesktop by build-desktop.py.
 *
 *  - The taskbar: a borderless bar along the bottom, in the manner of polybar. On the left the QRX
 *    home button and the overview button, then one button per window, on the right modules for
 *    the load of each core, memory, network and the clock. Pointing at a window button shows a
 *    preview of the window above it. The bar comes to the front when the pointer touches the
 *    bottom edge of the screen.
 *  - The home menu: pinned apps in a grid, typing filters them, power at the bottom; flat, centred
 *    over the bar as in Windows 11.
 *  - The overview: every window scaled into a grid (like Exposé); a click brings one to the
 *    front. From the bar's button, or with the pointer in the top left corner of the screen.
 *  - Wallpapers from pictures: PNG, BMP and PPM decoded here, other formats through datatypes
 *    where there are any; the picture is fitted to the screen and stored as a .qrxwp file.
 *
 * Needs from QRXDesktop: g_screen, g_desktop, g_terminal, g_files, g_screen_width/height,
 * g_wallpaper, g_files_path, launch_native(), open_filemanager(), redraw_desktop(),
 * save_preferences(), qrx_wallpaper_forget(). The windows of the shell have their own ports; the
 * desktop's loop waits on qrx_shell_sigmask() and calls qrx_shell_handle(). */
#include <devices/timer.h>
#include <datatypes/datatypes.h>
#include <datatypes/pictureclass.h>
#include <proto/datatypes.h>
#include <graphics/scale.h>
#include <graphics/layers.h>
#include <graphics/clip.h>

#define SH_BAR_H 40
#define SH_MENU_W 576
#define SH_MENU_H 470
#define SH_PEEK_W 248
#define SH_PEEK_H 176
#define SH_MAXWIN 24

/* flat colours */
#define SH_BG       0xFF0F172Au
#define SH_BAR      0xF0111827u
#define SH_PANEL    0xFF1E293Bu
#define SH_PANEL2   0xFF273449u
#define SH_HOVER    0xFF334155u
#define SH_TEXT     0xFFE2E8F0u
#define SH_DIM      0xFF94A3B8u
#define SH_ACCENT   0xFF3B82F6u
#define SH_ACCENT2  0xFF60A5FAu
#define SH_GOOD     0xFF22C55Eu
#define SH_WARN     0xFFF59E0Bu
#define SH_BAD      0xFFEF4444u
#define SH_TRACK    0xFF334155u

static struct Window *sh_bar, *sh_menu, *sh_peek, *sh_expo;
static struct MsgPort *sh_tport;
static struct timerequest *sh_treq;
static int sh_timer_open, sh_timer_busy;
struct Library *DataTypesBase;

/* ---------------------------------------------------------------- pens */
static struct { ULONG argb; LONG pen; } sh_pens[48];
static int sh_npens;
static LONG sh_pen(ULONG argb)
{
    int i;
    ULONG r, g, b;
    LONG pen;
    for (i = 0; i < sh_npens; i++) if (sh_pens[i].argb == argb) return sh_pens[i].pen;
    r = (argb >> 16) & 0xFF; g = (argb >> 8) & 0xFF; b = argb & 0xFF;
    pen = ObtainBestPen(g_screen->ViewPort.ColorMap, r * 0x01010101UL, g * 0x01010101UL, b * 0x01010101UL,
                        OBP_Precision, PRECISION_EXACT, TAG_DONE);
    if (pen < 0) pen = 1;
    if (sh_npens < 48) { sh_pens[sh_npens].argb = argb; sh_pens[sh_npens].pen = pen; sh_npens++; }
    return pen;
}
static void sh_release_pens(void)
{
    int i;
    for (i = 0; i < sh_npens; i++) ReleasePen(g_screen->ViewPort.ColorMap, sh_pens[i].pen);
    sh_npens = 0;
}
static void sh_fill(struct RastPort *rp, int x, int y, int w, int h, ULONG c)
{
    if (w <= 0 || h <= 0) return;
    SetAPen(rp, sh_pen(c));
    RectFill(rp, x, y, x + w - 1, y + h - 1);
}
/* a rectangle with its corners cut, the flat look's "rounded" box */
static void sh_round(struct RastPort *rp, int x, int y, int w, int h, ULONG c)
{
    if (w < 6 || h < 6) { sh_fill(rp, x, y, w, h, c); return; }
    sh_fill(rp, x + 2, y, w - 4, h, c);
    sh_fill(rp, x, y + 2, 2, h - 4, c);
    sh_fill(rp, x + w - 2, y + 2, 2, h - 4, c);
    sh_fill(rp, x + 1, y + 1, 1, h - 2, c);
    sh_fill(rp, x + w - 2, y + 1, 1, h - 2, c);
}
static void sh_text(struct RastPort *rp, int x, int y, ULONG c, const char *s, int max)
{
    int n = (int)strlen(s);
    if (max > 0) {
        while (n > 0 && TextLength(rp, (STRPTR)s, n) > max) n--;
    }
    SetAPen(rp, sh_pen(c));
    SetDrMd(rp, JAM1);
    Move(rp, x, y + rp->TxBaseline);
    Text(rp, (STRPTR)s, n);
}
static int sh_textw(struct RastPort *rp, const char *s) { return TextLength(rp, (STRPTR)s, strlen(s)); }
static void sh_text_c(struct RastPort *rp, int cx, int y, ULONG c, const char *s, int max)
{
    int w = sh_textw(rp, s);
    if (max > 0 && w > max) w = max;
    sh_text(rp, cx - w / 2, y, c, s, max);
}

/* ---------------------------------------------------------------- the machine */
#define SH_BOARD_Q(off) (*(volatile UQUAD *)(0xFFFFFFFF00000000UL + (off)))
#define SH_BOARD_L(off) (*(volatile ULONG *)(0xFFFFFFFF00000000UL + (off)))
static int sh_board = -1;
static unsigned sh_cores = 1;
static UQUAD sh_idle_prev[8], sh_t_prev;
static int sh_load[8];
static void sh_sample(void)
{
#if defined(__m68k64__)
    unsigned i;
    UQUAD t;
    if (sh_board < 0) {
        sh_board = SH_BOARD_Q(0) == 0x5152584543363442UL;
        if (sh_board) { sh_cores = SH_BOARD_L(0x31C); if (!sh_cores || sh_cores > 8) sh_cores = 1; }
    }
    if (!sh_board) return;
    t = SH_BOARD_Q(0x368);
    for (i = 0; i < sh_cores; i++) {
        UQUAD idle = SH_BOARD_Q(0x320 + i * 8);
        if (sh_t_prev && t > sh_t_prev) {
            int l = 100 - (int)((idle - sh_idle_prev[i]) * 100 / (t - sh_t_prev));
            sh_load[i] = l < 0 ? 0 : l > 100 ? 100 : l;
        }
        sh_idle_prev[i] = idle;
    }
    sh_t_prev = t;
#else
    sh_board = 0;
#endif
}
static int sh_mem_pct(void)
{
    ULONG freem = AvailMem(MEMF_ANY), tot = AvailMem(MEMF_ANY | MEMF_TOTAL);
    if (!tot) return 0;
    return (int)(100 - (ULONG)((UQUAD)freem * 100 / tot));
}
static int sh_net_up(void)
{
    int up;
    Forbid();
    up = FindName(&SysBase->LibList, "bsdsocket.library") != NULL;
    Permit();
    return up;
}

/* ---------------------------------------------------------------- windows of the screen */
struct sh_win { struct Window *w; char title[48]; };
static struct sh_win sh_wins[SH_MAXWIN];
static int sh_nwins;
static int sh_is_ours(struct Window *w)
{
    return w == g_desktop || w == sh_bar || w == sh_menu || w == sh_peek || w == sh_expo;
}
static int sh_window_alive(struct Window *w);
static void sh_collect(void)
{
    struct Window *w, *all[SH_MAXWIN * 2];
    ULONG lock;
    int n = 0, i;
    sh_nwins = 0;
    if (!g_screen) return;
    /* Only the pointers under the lock: no Intuition call may happen while it is held. */
    lock = LockIBase(0);
    for (w = g_screen->FirstWindow; w && n < SH_MAXWIN * 2; w = w->NextWindow)
        if (!sh_is_ours(w)) all[n++] = w;
    UnlockIBase(lock);
    for (i = 0; i < n && sh_nwins < SH_MAXWIN; i++) {
        const char *t;
        w = all[i];
        if (!sh_window_alive(w)) continue;
        if (w != g_terminal && !IsWindowVisible(w)) continue;
        if (w->Width < 40 || w->Height < 20) continue;
        t = w == g_terminal ? "Terminal" : w == g_files ? "QRX Drive" : w->Title ? (const char *)w->Title : "Window";
        sh_wins[sh_nwins].w = w;
        strncpy(sh_wins[sh_nwins].title, t, sizeof(sh_wins[0].title) - 1);
        sh_wins[sh_nwins].title[sizeof(sh_wins[0].title) - 1] = 0;
        sh_nwins++;
    }
}
static int sh_window_alive(struct Window *w)
{
    struct Window *x;
    ULONG lock;
    int ok = 0;
    if (!g_screen || !w) return 0;
    lock = LockIBase(0);
    for (x = g_screen->FirstWindow; x; x = x->NextWindow) if (x == w) { ok = 1; break; }
    UnlockIBase(lock);
    return ok;
}
static void sh_focus(struct Window *w)
{
    if (!sh_window_alive(w)) return;
    if (!IsWindowVisible(w)) ShowWindow(w, WINDOW_FRONTMOST);
    WindowToFront(w);
    ActivateWindow(w);
    if (sh_bar) WindowToFront(sh_bar);
}

/* A window's picture as it is on the screen, scaled to dw x dh as RGB; NULL when it cannot be made.
 * Read with cybergraphics and scaled here: graphics' BitMapScale left stripes at the edge. */
static UBYTE *sh_snapshot(struct Window *w, int dw, int dh)
{
    UBYTE *src, *dst;
    int sx = w->LeftEdge, sy = w->TopEdge, sw = w->Width, sh = w->Height, x, y;
    if (!CyberGfxBase) return NULL;
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw > g_screen->Width) sw = g_screen->Width - sx;
    if (sy + sh > g_screen->Height) sh = g_screen->Height - sy;
    if (sw < 2 || sh < 2 || dw < 2 || dh < 2) return NULL;
    src = (UBYTE *)malloc((ULONG)sw * sh * 3);
    dst = (UBYTE *)malloc((ULONG)dw * dh * 3);
    if (!src || !dst) { free(src); free(dst); return NULL; }
    ReadPixelArray(src, 0, 0, (UWORD)(sw * 3), &g_screen->RastPort, (UWORD)sx, (UWORD)sy, (UWORD)sw, (UWORD)sh, RECTFMT_RGB);
    for (y = 0; y < dh; y++) {
        const UBYTE *row = src + (ULONG)(y * sh / dh) * sw * 3;
        UBYTE *o = dst + (ULONG)y * dw * 3;
        for (x = 0; x < dw; x++) {
            const UBYTE *p = row + (x * sw / dw) * 3;
            o[x * 3] = p[0]; o[x * 3 + 1] = p[1]; o[x * 3 + 2] = p[2];
        }
    }
    free(src);
    return dst;
}
static void sh_blit(UBYTE *rgb, struct RastPort *rp, int x, int y, int w, int h)
{
    WritePixelArray(rgb, 0, 0, (UWORD)(w * 3), rp, (UWORD)x, (UWORD)y, (UWORD)w, (UWORD)h, RECTFMT_RGB);
}
static void sh_fit(int sw, int sh, int bw, int bh, int *dw, int *dh)
{
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    if ((long)sw * bh > (long)sh * bw) { *dw = bw; *dh = (int)((long)sh * bw / sw); }
    else { *dh = bh; *dw = (int)((long)sw * bh / sh); }
    if (*dw < 2) *dw = 2;
    if (*dh < 2) *dh = 2;
}

/* ---------------------------------------------------------------- apps for the home menu */
struct sh_app { const char *name, *cmd; ULONG color; const char *glyph; };
static const struct sh_app sh_apps[] = {
    { "Terminal",   "*terminal",            0xFF0EA5E9u, ">_" },
    { "QRX Drive",  "*files",               0xFFF59E0Bu, "QD" },
    { "Overview",   "*overview",            0xFF8B5CF6u, "OV" },
    { "About",      "C:QRXInfo",            0xFF3B82F6u, "i"  },
    { "Writer",     "QRX:C/QRXWriter",      0xFF2563EBu, "W"  },
    { "Sheet",      "QRX:C/QRXSpreadsheet", 0xFF16A34Au, "S"  },
    { "Presenter",  "QRX:C/QRXPresenter",   0xFFEA580Cu, "P"  },
    { "Web",        "QRX:C/QRXWeb",         0xFF0891B2u, "WB" },
    { "QRXAI",      "QRX:C/QRXAI",          0xFFDC2626u, "AI" },
    { "OpenGL",     "C:EC64GLDemo",         0xFF7C3AEDu, "3D" },
    { "Settings",   "QRX:C/QRXSettings",    0xFF64748Bu, "SE" },
    { "Hardware",   "QRX:C/QRXHardware",    0xFF475569u, "HW" },
    { "Welcome",    "QRX:C/QRXWelcome",     0xFF0D9488u, "HI" },
    { "Antivirus",  "QRX:C/ARESAntivirus",  0xFFBE123Cu, "AV" },
    { "Apps",       "*apps",                0xFFCA8A04u, "AP" },
    { "Desktop",    "QRX:C/QRXDesktopPrefs", 0xFF9333EAu, "DP" },
};
#define SH_NAPPS (int)(sizeof sh_apps / sizeof sh_apps[0])
static int sh_app_ok[SH_NAPPS];
static char sh_filter[24];
static int sh_menu_hover = -1, sh_power_open;
static int sh_shown[SH_NAPPS], sh_nshown;

static int sh_ci_has(const char *s, const char *f)
{
    int n = (int)strlen(f), i, k;
    if (!n) return 1;
    for (i = 0; s[i]; i++) {
        for (k = 0; k < n; k++) {
            int a = s[i + k], b = f[k];
            if (!a) return 0;
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (k == n) return 1;
    }
    return 0;
}
static void sh_check_apps(void)
{
    int i;
    for (i = 0; i < SH_NAPPS; i++) {
        const char *c = sh_apps[i].cmd;
        if (c[0] == '*') sh_app_ok[i] = 1;
        else {
            BPTR l = Lock((STRPTR)c, ACCESS_READ);
            sh_app_ok[i] = l != 0;
            if (l) UnLock(l);
        }
    }
}
static void sh_overview_open(void);
static void sh_run_app(int i)
{
    const char *c = sh_apps[i].cmd;
    if (!strcmp(c, "*terminal")) { if (g_terminal) sh_focus(g_terminal); }
    else if (!strcmp(c, "*files")) open_filemanager(g_files_path);
    else if (!strcmp(c, "*overview")) sh_overview_open();
    else if (!strcmp(c, "*apps")) {
        BPTR l = Lock((STRPTR)"APPS:", ACCESS_READ);
        if (l) { UnLock(l); open_filemanager("APPS:"); } else open_filemanager("SYS:");
    } else launch_native(c);
}

/* ---------------------------------------------------------------- taskbar */
static int sh_hover = -1, sh_hover_ticks, sh_peek_index = -1;
static int sh_home_down, sh_overview_down;
static int sh_btn_x0, sh_btn_w, sh_mod_x;

static int sh_bar_layout(void)
{
    int avail;
    sh_btn_x0 = 100;
    sh_mod_x = g_screen_width - 12 - ((int)sh_cores * 30 + 60) - 6 - 70 - 6 - 50 - 6 - 118;
    avail = sh_mod_x - 12 - sh_btn_x0;
    sh_btn_w = sh_nwins ? avail / sh_nwins : 0;
    if (sh_btn_w > 180) sh_btn_w = 180;
    return sh_nwins;
}
static void sh_bar_module(struct RastPort *rp, int x, int w, const char *label, ULONG underline)
{
    sh_fill(rp, x, 8, w, SH_BAR_H - 16, SH_PANEL);
    sh_fill(rp, x, SH_BAR_H - 10, w, 2, underline);
    sh_text_c(rp, x + w / 2, 14, SH_TEXT, label, w - 6);
}
static void sh_draw_bar(void)
{
    struct RastPort *rp;
    int i, x;
    char s[32];
    struct DateStamp ds;
    struct Window *active = IntuitionBase->ActiveWindow;
    if (!sh_bar) return;
    rp = sh_bar->RPort;
    sh_collect();
    sh_bar_layout();
    sh_fill(rp, 0, 0, sh_bar->Width, SH_BAR_H, SH_BAR);
    sh_fill(rp, 0, 0, sh_bar->Width, 1, SH_HOVER);

    /* home */
    sh_round(rp, 6, 4, 40, 32, sh_menu ? SH_ACCENT2 : SH_ACCENT);
    sh_text_c(rp, 26, 16, 0xFFFFFFFFu, "QRX", 36);
    /* overview: four small panes */
    sh_round(rp, 52, 4, 40, 32, sh_expo ? SH_HOVER : SH_PANEL);
    sh_fill(rp, 62, 11, 9, 7, SH_DIM); sh_fill(rp, 73, 11, 9, 7, SH_DIM);
    sh_fill(rp, 62, 20, 9, 7, SH_DIM); sh_fill(rp, 73, 20, 9, 7, SH_DIM);

    /* windows */
    for (i = 0, x = sh_btn_x0; i < sh_nwins; i++, x += sh_btn_w) {
        int is_active = sh_wins[i].w == active, w = sh_btn_w - 4;
        ULONG bg = is_active ? SH_HOVER : (i == sh_hover ? SH_PANEL2 : SH_PANEL);
        sh_round(rp, x, 4, w, 32, bg);
        if (is_active) sh_fill(rp, x + w / 4, 33, w / 2, 3, SH_ACCENT);
        else sh_fill(rp, x + w / 2 - 3, 33, 6, 2, SH_DIM);
        sh_fill(rp, x + 8, 12, 14, 14, SH_ACCENT2);
        sh_text(rp, x + 28, 15, is_active ? 0xFFFFFFFFu : SH_TEXT, sh_wins[i].title, w - 34);
    }

    /* modules, right */
    x = sh_mod_x;
    sh_fill(rp, x, 8, (int)sh_cores * 30 + 60, SH_BAR_H - 16, SH_PANEL);
    sh_text(rp, x + 6, 14, SH_DIM, "CPU", 0);
    for (i = 0; i < (int)sh_cores; i++) {
        int bx = x + 36 + i * 30, h = 20 * sh_load[i] / 100;
        ULONG c = sh_load[i] < 60 ? SH_GOOD : sh_load[i] < 85 ? SH_WARN : SH_BAD;
        sh_fill(rp, bx, 10, 22, 20, SH_TRACK);
        if (h) sh_fill(rp, bx, 30 - h, 22, h, c);
    }
    sh_fill(rp, x, SH_BAR_H - 10, (int)sh_cores * 30 + 60, 2, SH_GOOD);
    x += (int)sh_cores * 30 + 66;
    snprintf(s, sizeof s, "RAM %d%%", sh_mem_pct());
    sh_bar_module(rp, x, 70, s, SH_WARN);
    x += 76;
    sh_bar_module(rp, x, 50, sh_net_up() ? "NET" : "OFF", sh_net_up() ? SH_ACCENT : SH_BAD);
    x += 56;
    DateStamp(&ds);
    {
        long days = ds.ds_Days, y = 1978, m = 1;
        static const int md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        for (;;) {
            int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0, yd = 365 + leap;
            if (days < yd) break;
            days -= yd; y++;
        }
        for (m = 0; m < 12; m++) {
            int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0, d = md[m] + (m == 1 && leap);
            if (days < d) break;
            days -= d;
        }
        snprintf(s, sizeof s, "%02ld:%02ld  %02ld.%02ld.", (long)ds.ds_Minute / 60, (long)ds.ds_Minute % 60, days + 1, m + 1);
    }
    sh_bar_module(rp, x, 118, s, SH_ACCENT2);
}
static int sh_bar_hit(int mx)
{
    if (mx >= 6 && mx < 46) return -2;               /* home */
    if (mx >= 52 && mx < 92) return -3;              /* overview */
    if (sh_btn_w > 0 && mx >= sh_btn_x0 && mx < sh_btn_x0 + sh_btn_w * sh_nwins) return (mx - sh_btn_x0) / sh_btn_w;
    return -1;
}

/* ---------------------------------------------------------------- preview */
static void sh_peek_close(void)
{
    if (sh_peek) { CloseWindow(sh_peek); sh_peek = NULL; }
    sh_peek_index = -1;
}
static void sh_peek_open(int index)
{
    struct Window *w;
    UBYTE *bm;
    int dw, dh, x;
    sh_peek_close();
    if (index < 0 || index >= sh_nwins) return;
    w = sh_wins[index].w;
    if (!sh_window_alive(w)) return;
    sh_fit(w->Width, w->Height, SH_PEEK_W - 16, SH_PEEK_H - 34, &dw, &dh);
    bm = sh_snapshot(w, dw, dh);            /* before the preview covers anything */
    x = sh_btn_x0 + index * sh_btn_w + sh_btn_w / 2 - SH_PEEK_W / 2;
    if (x < 4) x = 4;
    if (x + SH_PEEK_W > g_screen_width - 4) x = g_screen_width - 4 - SH_PEEK_W;
    sh_peek = OpenWindowTags(NULL, g_screen_borrowed ? WA_PubScreen : WA_CustomScreen, (IPTR)g_screen, WA_Left, x, WA_Top, g_screen_height - SH_BAR_H - SH_PEEK_H - 6,
                             WA_Width, SH_PEEK_W, WA_Height, SH_PEEK_H, WA_Borderless, TRUE, WA_SmartRefresh, TRUE,
                             WA_IDCMP, 0, TAG_DONE);
    if (sh_peek) {
        struct RastPort *rp = sh_peek->RPort;
        sh_fill(rp, 0, 0, SH_PEEK_W, SH_PEEK_H, SH_PANEL);
        sh_fill(rp, 0, 0, SH_PEEK_W, 2, SH_ACCENT);
        sh_text(rp, 10, 9, SH_TEXT, sh_wins[index].title, SH_PEEK_W - 20);
        if (bm) sh_blit(bm, rp, (SH_PEEK_W - dw) / 2, 26 + (SH_PEEK_H - 34 - dh) / 2, dw, dh);
        else sh_text_c(rp, SH_PEEK_W / 2, SH_PEEK_H / 2, SH_DIM, "no preview", 0);
        sh_peek_index = index;
    }
    free(bm);
}

/* ---------------------------------------------------------------- home menu */
static void sh_menu_layout(void)
{
    int i;
    sh_nshown = 0;
    for (i = 0; i < SH_NAPPS; i++)
        if (sh_app_ok[i] && sh_ci_has(sh_apps[i].name, sh_filter)) sh_shown[sh_nshown++] = i;
}
/* tile i of the shown apps: 6 columns */
static void sh_tile_rect(int k, int *x, int *y)
{
    *x = 24 + (k % 6) * 88;
    *y = 112 + (k / 6) * 92;
}
static void sh_draw_menu(void)
{
    struct RastPort *rp;
    int k;
    if (!sh_menu) return;
    rp = sh_menu->RPort;
    sh_menu_layout();
    sh_fill(rp, 0, 0, SH_MENU_W, SH_MENU_H, SH_PANEL);
    sh_fill(rp, 0, 0, SH_MENU_W, 1, SH_HOVER);
    /* search */
    sh_round(rp, 24, 20, SH_MENU_W - 48, 32, SH_BG);
    sh_fill(rp, 24, 50, SH_MENU_W - 48, 2, SH_ACCENT);
    if (sh_filter[0]) { char s[40]; snprintf(s, sizeof s, "%s_", sh_filter); sh_text(rp, 40, 31, SH_TEXT, s, 0); }
    else sh_text(rp, 40, 31, SH_DIM, "Type to search apps", 0);
    sh_text(rp, 24, 78, SH_TEXT, "Pinned", 0);
    sh_text(rp, SH_MENU_W - 24 - sh_textw(rp, "QRX OS One"), 78, SH_DIM, "QRX OS One", 0);
    for (k = 0; k < sh_nshown && k < 18; k++) {
        int x, y, i = sh_shown[k];
        sh_tile_rect(k, &x, &y);
        if (k == sh_menu_hover) sh_round(rp, x - 6, y - 6, 80, 84, SH_HOVER);
        sh_round(rp, x + 14, y, 40, 40, sh_apps[i].color);
        sh_text_c(rp, x + 34, y + 16, 0xFFFFFFFFu, sh_apps[i].glyph, 38);
        sh_text_c(rp, x + 34, y + 52, SH_TEXT, sh_apps[i].name, 78);
    }
    if (!sh_nshown) sh_text_c(rp, SH_MENU_W / 2, 200, SH_DIM, "No app matches", 0);
    /* bottom: user and power */
    sh_fill(rp, 0, SH_MENU_H - 60, SH_MENU_W, 60, SH_PANEL2);
    sh_round(rp, 24, SH_MENU_H - 46, 32, 32, SH_ACCENT);
    sh_text_c(rp, 40, SH_MENU_H - 34, 0xFFFFFFFFu, "Q", 0);
    sh_text(rp, 66, SH_MENU_H - 34, SH_TEXT, "QRX User", 0);
    if (sh_power_open) {
        sh_round(rp, SH_MENU_W - 268, SH_MENU_H - 48, 80, 36, SH_BAD);
        sh_text_c(rp, SH_MENU_W - 228, SH_MENU_H - 34, 0xFFFFFFFFu, "Shut down", 76);
        sh_round(rp, SH_MENU_W - 180, SH_MENU_H - 48, 80, 36, SH_WARN);
        sh_text_c(rp, SH_MENU_W - 140, SH_MENU_H - 34, 0xFF111827u, "Restart", 76);
        sh_round(rp, SH_MENU_W - 92, SH_MENU_H - 48, 68, 36, SH_HOVER);
        sh_text_c(rp, SH_MENU_W - 58, SH_MENU_H - 34, SH_TEXT, "Cancel", 64);
    } else {
        sh_round(rp, SH_MENU_W - 60, SH_MENU_H - 48, 36, 36, SH_HOVER);
        SetAPen(rp, sh_pen(SH_TEXT));
        DrawEllipse(rp, SH_MENU_W - 42, SH_MENU_H - 29, 9, 9);
        sh_fill(rp, SH_MENU_W - 43, SH_MENU_H - 42, 3, 12, SH_HOVER);
        sh_fill(rp, SH_MENU_W - 43, SH_MENU_H - 41, 2, 10, SH_TEXT);
    }
}
static void sh_menu_close(void)
{
    if (sh_menu) { CloseWindow(sh_menu); sh_menu = NULL; }
    sh_filter[0] = 0; sh_menu_hover = -1; sh_power_open = 0;
    sh_draw_bar();
}
static void sh_menu_open(void)
{
    sh_peek_close();
    if (sh_menu) { sh_menu_close(); return; }
    sh_check_apps();
    sh_filter[0] = 0; sh_menu_hover = -1; sh_power_open = 0;
    sh_menu = OpenWindowTags(NULL, g_screen_borrowed ? WA_PubScreen : WA_CustomScreen, (IPTR)g_screen, WA_Left, (g_screen_width - SH_MENU_W) / 2,
                             WA_Top, g_screen_height - SH_BAR_H - SH_MENU_H - 8, WA_Width, SH_MENU_W, WA_Height, SH_MENU_H,
                             WA_Borderless, TRUE, WA_SmartRefresh, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_ReportMouse, TRUE,
                             WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_VANILLAKEY | IDCMP_INACTIVEWINDOW | IDCMP_REFRESHWINDOW,
                             TAG_DONE);
    sh_draw_menu();
    sh_draw_bar();
}
static int sh_menu_tile_at(int mx, int my)
{
    int k;
    for (k = 0; k < sh_nshown && k < 18; k++) {
        int x, y;
        sh_tile_rect(k, &x, &y);
        if (mx >= x - 6 && mx < x + 74 && my >= y - 6 && my < y + 78) return k;
    }
    return -1;
}
static void sh_menu_message(struct IntuiMessage *m)
{
    ULONG cls = m->Class;
    UWORD code = m->Code;
    int mx = m->MouseX, my = m->MouseY;
    ReplyMsg((struct Message *)m);
    if (cls == IDCMP_REFRESHWINDOW) { BeginRefresh(sh_menu); EndRefresh(sh_menu, TRUE); sh_draw_menu(); return; }
    if (cls == IDCMP_INACTIVEWINDOW) {
        /* clicked elsewhere; the home button closes it itself */
        struct Window *a = IntuitionBase->ActiveWindow;
        if (a != sh_bar) sh_menu_close();
        return;
    }
    if (cls == IDCMP_MOUSEMOVE) {
        int h = sh_menu_tile_at(mx, my);
        if (h != sh_menu_hover) { sh_menu_hover = h; sh_draw_menu(); }
        return;
    }
    if (cls == IDCMP_VANILLAKEY) {
        int n = (int)strlen(sh_filter);
        if (code == 27) { sh_menu_close(); return; }
        if (code == 13) { sh_menu_layout(); if (sh_nshown) { int i = sh_shown[0]; sh_menu_close(); sh_run_app(i); } return; }
        if (code == 8) { if (n) sh_filter[n - 1] = 0; }
        else if (code >= 32 && code < 127 && n < (int)sizeof(sh_filter) - 1) { sh_filter[n] = (char)code; sh_filter[n + 1] = 0; }
        sh_menu_hover = -1;
        sh_draw_menu();
        return;
    }
    if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
        int k = sh_menu_tile_at(mx, my);
        if (k >= 0) { int i = sh_shown[k]; sh_menu_close(); sh_run_app(i); return; }
        if (my >= SH_MENU_H - 48 && my < SH_MENU_H - 12) {
            if (!sh_power_open && mx >= SH_MENU_W - 60 && mx < SH_MENU_W - 24) { sh_power_open = 1; sh_draw_menu(); return; }
            if (sh_power_open) {
                if (mx >= SH_MENU_W - 268 && mx < SH_MENU_W - 188) { sh_menu_close(); launch_native("C:Shutdown"); return; }
                if (mx >= SH_MENU_W - 180 && mx < SH_MENU_W - 100) { sh_menu_close(); ColdReboot(); return; }
                if (mx >= SH_MENU_W - 92 && mx < SH_MENU_W - 24) { sh_power_open = 0; sh_draw_menu(); return; }
            }
        }
    }
}

/* ---------------------------------------------------------------- overview */
static struct { struct Window *w; UBYTE *bm; int x, y, dw, dh; char title[48]; } sh_tiles[SH_MAXWIN];
static int sh_ntiles, sh_expo_hover = -1;
static struct Window *sh_order[SH_MAXWIN];
static int sh_order_n;
static void sh_overview_free(void)
{
    int i;
    for (i = 0; i < sh_ntiles; i++) { free(sh_tiles[i].bm); sh_tiles[i].bm = NULL; }
    sh_ntiles = 0;
}
static void sh_draw_overview(void)
{
    struct RastPort *rp;
    int i;
    if (!sh_expo) return;
    rp = sh_expo->RPort;
    sh_fill(rp, 0, 0, sh_expo->Width, sh_expo->Height, SH_BG);
    sh_text_c(rp, sh_expo->Width / 2, 18, SH_DIM, sh_ntiles ? "Overview - click a window, Esc to leave" : "No windows open", 0);
    for (i = 0; i < sh_ntiles; i++) {
        int x = sh_tiles[i].x, y = sh_tiles[i].y;
        ULONG frame = i == sh_expo_hover ? SH_ACCENT : SH_HOVER;
        sh_fill(rp, x - 3, y - 3, sh_tiles[i].dw + 6, sh_tiles[i].dh + 6, frame);
        if (sh_tiles[i].bm) sh_blit(sh_tiles[i].bm, rp, x, y, sh_tiles[i].dw, sh_tiles[i].dh);
        else sh_fill(rp, x, y, sh_tiles[i].dw, sh_tiles[i].dh, SH_PANEL);
        sh_text_c(rp, x + sh_tiles[i].dw / 2, y + sh_tiles[i].dh + 8, i == sh_expo_hover ? 0xFFFFFFFFu : SH_TEXT,
                  sh_tiles[i].title, sh_tiles[i].dw + 20);
    }
}
static void sh_overview_close(void)
{
    if (sh_expo) { CloseWindow(sh_expo); sh_expo = NULL; }
    sh_overview_free();
    sh_expo_hover = -1;
    sh_draw_bar();
}
static void sh_overview_open(void)
{
    int i, cols, rows, cw, ch, top = 44, W = g_screen_width, H = g_screen_height;
    if (sh_expo) { sh_overview_close(); return; }
    if (sh_menu) sh_menu_close();
    sh_peek_close();
    sh_collect();
    sh_overview_free();
    cols = 1;
    while (cols * cols < sh_nwins) cols++;
    rows = sh_nwins ? (sh_nwins + cols - 1) / cols : 1;
    cw = (W - 40) / cols; ch = (H - top - 20) / rows;
    /* Pictures first, the overview will cover the windows. Each window comes to the front for its
     * picture, so none shows another's parts; afterwards the old order comes back. */
    {
        struct Layer *l;
        ULONG lock = LockIBase(0);
        sh_order_n = 0;
        for (l = g_screen->LayerInfo.top_layer; l && l->back; l = l->back) ;
        for (; l && sh_order_n < SH_MAXWIN; l = l->front)        /* from the back to the front */
            if (l->Window && !sh_is_ours((struct Window *)l->Window)) sh_order[sh_order_n++] = (struct Window *)l->Window;
        UnlockIBase(lock);
    }
    for (i = 0; i < sh_nwins; i++) {
        struct Window *w = sh_wins[i].w;
        WindowToFront(w);
        Delay(3);                       /* the window's own task draws what was covered */
        int dw, dh, cx = 20 + (i % cols) * cw, cy = top + (i / cols) * ch;
        sh_fit(w->Width, w->Height, cw - 40, ch - 44, &dw, &dh);
        sh_tiles[i].w = w;
        sh_tiles[i].bm = sh_snapshot(w, dw, dh);
        sh_tiles[i].dw = dw; sh_tiles[i].dh = dh;
        sh_tiles[i].x = cx + (cw - dw) / 2; sh_tiles[i].y = cy + (ch - 24 - dh) / 2;
        strcpy(sh_tiles[i].title, sh_wins[i].title);
    }
    sh_ntiles = sh_nwins;
    for (i = 0; i < sh_order_n; i++) if (sh_window_alive(sh_order[i])) WindowToFront(sh_order[i]);
    sh_expo = OpenWindowTags(NULL, g_screen_borrowed ? WA_PubScreen : WA_CustomScreen, (IPTR)g_screen, WA_Left, 0, WA_Top, 0, WA_Width, W, WA_Height, H,
                             WA_Borderless, TRUE, WA_SmartRefresh, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_ReportMouse, TRUE,
                             WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_VANILLAKEY | IDCMP_REFRESHWINDOW, TAG_DONE);
    if (!sh_expo) { sh_overview_free(); return; }
    sh_draw_overview();
}
static int sh_overview_at(int mx, int my)
{
    int i;
    for (i = 0; i < sh_ntiles; i++)
        if (mx >= sh_tiles[i].x - 3 && mx < sh_tiles[i].x + sh_tiles[i].dw + 3 && my >= sh_tiles[i].y - 3 &&
            my < sh_tiles[i].y + sh_tiles[i].dh + 20) return i;
    return -1;
}
static void sh_overview_message(struct IntuiMessage *m)
{
    ULONG cls = m->Class;
    UWORD code = m->Code;
    int mx = m->MouseX, my = m->MouseY;
    ReplyMsg((struct Message *)m);
    if (cls == IDCMP_REFRESHWINDOW) { BeginRefresh(sh_expo); EndRefresh(sh_expo, TRUE); sh_draw_overview(); return; }
    if (cls == IDCMP_VANILLAKEY) { if (code == 27 || code == 13) sh_overview_close(); return; }
    if (cls == IDCMP_MOUSEMOVE) {
        int h = sh_overview_at(mx, my);
        if (h != sh_expo_hover) { sh_expo_hover = h; sh_draw_overview(); }
        return;
    }
    if (cls == IDCMP_MOUSEBUTTONS && (code == SELECTDOWN || code == MENUDOWN)) {
        int h = code == SELECTDOWN ? sh_overview_at(mx, my) : -1;
        struct Window *w = h >= 0 ? sh_tiles[h].w : NULL;
        sh_overview_close();
        if (w) sh_focus(w);
    }
}

/* ---------------------------------------------------------------- bar events */
static void sh_bar_message(struct IntuiMessage *m)
{
    ULONG cls = m->Class;
    UWORD code = m->Code;
    int mx = m->MouseX;
    ReplyMsg((struct Message *)m);
    if (cls == IDCMP_REFRESHWINDOW) { BeginRefresh(sh_bar); EndRefresh(sh_bar, TRUE); sh_draw_bar(); return; }
    if (cls == IDCMP_MOUSEMOVE) {
        int h = sh_bar_hit(mx);
        if (h < 0) h = -1;
        if (h != sh_hover) {
            sh_hover = h; sh_hover_ticks = 0;
            if (sh_peek && h != sh_peek_index) sh_peek_close();
            sh_draw_bar();
        }
        return;
    }
    if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
        int h = sh_bar_hit(mx);
        sh_peek_close();
        if (h == -2) { sh_menu_open(); return; }
        if (h == -3) { sh_overview_open(); return; }
        if (h >= 0 && h < sh_nwins) {
            struct Window *w = sh_wins[h].w;
            if (w == IntuitionBase->ActiveWindow) WindowToBack(w);   /* a second click puts it away */
            else sh_focus(w);
            sh_draw_bar();
        }
    }
}

/* ---------------------------------------------------------------- the timer: clock, load, hover, hot corner */
static void sh_timer_send(ULONG micro)
{
    if (!sh_timer_open || sh_timer_busy) return;
    sh_treq->tr_node.io_Command = TR_ADDREQUEST;
    sh_treq->tr_time.tv_secs = 0;
    sh_treq->tr_time.tv_micro = micro;
    SendIO((struct IORequest *)sh_treq);
    sh_timer_busy = 1;
}
static char sh_test[64];
static int sh_ticks, sh_corner = 1;     /* the pointer starts in the corner: armed once it leaves */
static void sh_tick(void)
{
    WORD px = g_screen->MouseX, py = g_screen->MouseY;
    sh_ticks++;
    /* the bar comes up when the pointer reaches the bottom edge */
    if (sh_bar && py >= g_screen_height - 2 && !sh_expo) WindowToFront(sh_bar);
    /* hot corner: top left opens the overview */
    if (px <= 1 && py <= 1) { if (!sh_corner) { sh_corner = 1; sh_overview_open(); } }
    else sh_corner = 0;
    /* a preview after half a second on a window button */
    if (sh_hover >= 0 && !sh_menu && !sh_expo) {
        if (++sh_hover_ticks == 2 && sh_peek_index != sh_hover) sh_peek_open(sh_hover);
    }
    if (sh_peek && strcmp(sh_test, "peek") && (py < g_screen_height - SH_BAR_H - SH_PEEK_H - 10 || sh_hover < 0)) sh_peek_close();
    if (sh_ticks % 4 == 0) { sh_sample(); sh_draw_bar(); }
    if (sh_test[0] && sh_ticks == 40) {
        if (!strcmp(sh_test, "menu")) sh_menu_open();
        else if (!strcmp(sh_test, "overview")) sh_overview_open();
        else if (!strncmp(sh_test, "wallpaper ", 10)) qrx_shell_set_wallpaper(sh_test + 10);
        else if (!strcmp(sh_test, "peek")) { sh_collect(); sh_bar_layout(); sh_hover = 0; sh_hover_ticks = 2; sh_peek_open(0); }
    }
    if (!strcmp(sh_test, "peek") && sh_ticks > 40) sh_hover = 0;     /* keep the test's preview open */
}

/* ---------------------------------------------------------------- open, close, the loop's part */
static void qrx_shell_open(void)
{
    if (!g_screen || sh_bar) return;
    sh_sample();
    sh_bar = OpenWindowTags(NULL, g_screen_borrowed ? WA_PubScreen : WA_CustomScreen, (IPTR)g_screen, WA_Left, 0, WA_Top, g_screen_height - SH_BAR_H,
                            WA_Width, g_screen_width, WA_Height, SH_BAR_H, WA_Borderless, TRUE, WA_SmartRefresh, TRUE,
                            WA_RMBTrap, TRUE, WA_ReportMouse, TRUE,
                            WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_REFRESHWINDOW, TAG_DONE);
    sh_tport = CreateMsgPort();
    sh_treq = sh_tport ? (struct timerequest *)CreateIORequest(sh_tport, sizeof(*sh_treq)) : NULL;
    sh_timer_open = sh_treq && !OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)sh_treq, 0);
    sh_draw_bar();
    sh_timer_send(250000);
    {
        /* tests without a mouse: SYS:QRX/shelltest names what to open, 10 seconds after the start */
        BPTR fh = Open((STRPTR)"SYS:QRX/shelltest", MODE_OLDFILE);
        if (fh) {
            LONG n = Read(fh, sh_test, sizeof sh_test - 1);
            Close(fh);
            sh_test[n > 0 ? n : 0] = 0;
            { char *e = strchr(sh_test, '\n'); if (e) *e = 0; }
        }
    }
}
static void qrx_shell_close(void)
{
    sh_peek_close();
    if (sh_menu) { CloseWindow(sh_menu); sh_menu = NULL; }
    if (sh_expo) { CloseWindow(sh_expo); sh_expo = NULL; }
    sh_overview_free();
    if (sh_bar) { CloseWindow(sh_bar); sh_bar = NULL; }
    if (sh_timer_open) {
        if (sh_timer_busy) { AbortIO((struct IORequest *)sh_treq); WaitIO((struct IORequest *)sh_treq); }
        CloseDevice((struct IORequest *)sh_treq);
    }
    sh_timer_open = sh_timer_busy = 0;
    if (sh_treq) DeleteIORequest((struct IORequest *)sh_treq);
    if (sh_tport) DeleteMsgPort(sh_tport);
    sh_treq = NULL; sh_tport = NULL;
    if (g_screen) sh_release_pens();
}
static ULONG qrx_shell_sigmask(void)
{
    ULONG m = 0;
    if (sh_bar) m |= 1UL << sh_bar->UserPort->mp_SigBit;
    if (sh_menu) m |= 1UL << sh_menu->UserPort->mp_SigBit;
    if (sh_expo) m |= 1UL << sh_expo->UserPort->mp_SigBit;
    if (sh_tport) m |= 1UL << sh_tport->mp_SigBit;
    return m;
}
static void qrx_shell_handle(ULONG signals)
{
    struct IntuiMessage *m;
    (void)signals;
    while (sh_bar && (m = (struct IntuiMessage *)GetMsg(sh_bar->UserPort))) sh_bar_message(m);
    while (sh_menu && (m = (struct IntuiMessage *)GetMsg(sh_menu->UserPort))) sh_menu_message(m);
    while (sh_expo && (m = (struct IntuiMessage *)GetMsg(sh_expo->UserPort))) sh_overview_message(m);
    if (sh_timer_open && sh_timer_busy && CheckIO((struct IORequest *)sh_treq)) {
        WaitIO((struct IORequest *)sh_treq);
        sh_timer_busy = 0;
        sh_tick();
        sh_timer_send(250000);
    }
}

/* ---------------------------------------------------------------- pictures */
static int sh_ends(const char *s, const char *e)
{
    int n = (int)strlen(s), k = (int)strlen(e), i;
    if (n < k) return 0;
    for (i = 0; i < k; i++) {
        int a = s[n - k + i], b = e[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != b) return 0;
    }
    return 1;
}
static int qrx_shell_is_picture(const char *name)
{
    return sh_ends(name, ".png") || sh_ends(name, ".bmp") || sh_ends(name, ".ppm") || sh_ends(name, ".pnm") ||
           sh_ends(name, ".jpg") || sh_ends(name, ".jpeg") || sh_ends(name, ".gif") || sh_ends(name, ".iff") ||
           sh_ends(name, ".ilbm") || sh_ends(name, ".qrxwp");
}

/* -- inflate (RFC 1951), enough for PNG */
struct sh_inf { const UBYTE *in; ULONG inlen, pos; ULONG bitbuf; int bitcnt; UBYTE *out; ULONG outlen, outpos; };
static int sh_bits(struct sh_inf *s, int n)
{
    ULONG v;
    while (s->bitcnt < n) {
        if (s->pos >= s->inlen) return -1;
        s->bitbuf |= (ULONG)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    v = s->bitbuf & ((1UL << n) - 1);
    s->bitbuf >>= n; s->bitcnt -= n;
    return (int)v;
}
struct sh_huff { short count[16]; short sym[320]; };
static void sh_huff_build(struct sh_huff *h, const UBYTE *len, int n)
{
    short offs[16];
    int i;
    memset(h->count, 0, sizeof h->count);
    for (i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (short)i;
}
static int sh_decode(struct sh_inf *s, const struct sh_huff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len < 16; len++) {
        int b = sh_bits(s, 1), count;
        if (b < 0) return -1;
        code |= b;
        count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static int sh_put(struct sh_inf *s, int c)
{
    if (s->outpos >= s->outlen) return 0;
    s->out[s->outpos++] = (UBYTE)c;
    return 1;
}
static int sh_codes(struct sh_inf *s, const struct sh_huff *lc, const struct sh_huff *dc)
{
    static const short lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const short lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const short dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const short dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
    for (;;) {
        int sym = sh_decode(s, lc);
        if (sym < 0) return 0;
        if (sym < 256) { if (!sh_put(s, sym)) return 0; }
        else if (sym == 256) return 1;
        else {
            int len, dist, e;
            sym -= 257;
            if (sym >= 29) return 0;
            e = sh_bits(s, lext[sym]); if (e < 0) return 0;
            len = lbase[sym] + e;
            sym = sh_decode(s, dc);
            if (sym < 0 || sym >= 30) return 0;
            e = sh_bits(s, dext[sym]); if (e < 0) return 0;
            dist = dbase[sym] + e;
            if ((ULONG)dist > s->outpos) return 0;
            while (len--) if (!sh_put(s, s->out[s->outpos - dist])) return 0;
        }
    }
}
static int sh_inflate(struct sh_inf *s)
{
    int last;
    do {
        int type;
        last = sh_bits(s, 1);
        type = sh_bits(s, 2);
        if (last < 0 || type < 0) return 0;
        if (type == 0) {
            ULONG n;
            s->bitbuf = 0; s->bitcnt = 0;
            if (s->pos + 4 > s->inlen) return 0;
            n = s->in[s->pos] | (s->in[s->pos + 1] << 8);
            s->pos += 4;
            if (s->pos + n > s->inlen) return 0;
            while (n--) if (!sh_put(s, s->in[s->pos++])) return 0;
        } else if (type == 1) {
            static struct sh_huff lc, dc;
            static int built;
            if (!built) {
                UBYTE l[288];
                int i;
                for (i = 0; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                sh_huff_build(&lc, l, 288);
                for (i = 0; i < 30; i++) l[i] = 5;
                sh_huff_build(&dc, l, 30);
                built = 1;
            }
            if (!sh_codes(s, &lc, &dc)) return 0;
        } else if (type == 2) {
            static const UBYTE order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            UBYTE lens[320];
            struct sh_huff lc, dc, cc;
            int nlen = sh_bits(s, 5) + 257, ndist = sh_bits(s, 5) + 1, ncode = sh_bits(s, 4) + 4, i;
            if (nlen > 286 || ndist > 30) return 0;
            memset(lens, 0, sizeof lens);
            for (i = 0; i < ncode; i++) lens[order[i]] = (UBYTE)sh_bits(s, 3);
            sh_huff_build(&cc, lens, 19);
            for (i = 0; i < nlen + ndist;) {
                int sym = sh_decode(s, &cc), rep = 0, val = 0;
                if (sym < 0) return 0;
                if (sym < 16) { lens[i++] = (UBYTE)sym; continue; }
                if (sym == 16) { if (!i) return 0; val = lens[i - 1]; rep = 3 + sh_bits(s, 2); }
                else if (sym == 17) rep = 3 + sh_bits(s, 3);
                else rep = 11 + sh_bits(s, 7);
                if (i + rep > nlen + ndist) return 0;
                while (rep--) lens[i++] = (UBYTE)val;
            }
            sh_huff_build(&lc, lens, nlen);
            sh_huff_build(&dc, lens + nlen, ndist);
            if (!sh_codes(s, &lc, &dc)) return 0;
        } else return 0;
    } while (!last);
    return 1;
}
static ULONG sh_be32(const UBYTE *p) { return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3]; }
static int sh_paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
}
/* PNG, 8 bits per sample, not interlaced: gray, RGB, palette, gray+alpha, RGBA */
static UBYTE *sh_png(const UBYTE *d, ULONG n, int *w, int *h)
{
    ULONG pos = 8, idat = 0, iw = 0, ih = 0, x, y;
    int ctype = -1, depth = 0, bpp, interlace = 0;
    UBYTE pal[768], *z, *raw = NULL, *rgb = NULL;
    ULONG zlen = 0, stride;
    struct sh_inf s;
    if (n < 8 || memcmp(d, "\x89PNG", 4)) return NULL;
    z = (UBYTE *)malloc(n);
    if (!z) return NULL;
    memset(pal, 0, sizeof pal);
    while (pos + 8 <= n) {
        ULONG len = sh_be32(d + pos);
        const UBYTE *t = d + pos + 4, *c = d + pos + 8;
        if (pos + 12 + len > n) break;
        if (!memcmp(t, "IHDR", 4)) { iw = sh_be32(c); ih = sh_be32(c + 4); depth = c[8]; ctype = c[9]; interlace = c[12]; }
        else if (!memcmp(t, "PLTE", 4)) memcpy(pal, c, len > 768 ? 768 : len);
        else if (!memcmp(t, "IDAT", 4)) { memcpy(z + zlen, c, len); zlen += len; idat++; }
        else if (!memcmp(t, "IEND", 4)) break;
        pos += 12 + len;
    }
    if (!idat || depth != 8 || interlace || !iw || !ih || iw > 8192 || ih > 8192) { free(z); return NULL; }
    bpp = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!bpp) { free(z); return NULL; }
    stride = iw * bpp;
    raw = (UBYTE *)malloc((stride + 1) * ih);
    rgb = (UBYTE *)malloc(iw * ih * 3);
    if (!raw || !rgb || zlen < 6) { free(z); free(raw); free(rgb); return NULL; }
    memset(&s, 0, sizeof s);
    s.in = z + 2; s.inlen = zlen - 2; s.out = raw; s.outlen = (stride + 1) * ih;
    if (!sh_inflate(&s) || s.outpos < s.outlen) { free(z); free(raw); free(rgb); return NULL; }
    free(z);
    for (y = 0; y < ih; y++) {
        UBYTE *row = raw + y * (stride + 1), f = row[0], *p = row + 1;
        UBYTE *up = y ? raw + (y - 1) * (stride + 1) + 1 : NULL;
        for (x = 0; x < stride; x++) {
            int a = x >= (ULONG)bpp ? p[x - bpp] : 0, b = up ? up[x] : 0, c = (up && x >= (ULONG)bpp) ? up[x - bpp] : 0;
            if (f == 1) p[x] = (UBYTE)(p[x] + a);
            else if (f == 2) p[x] = (UBYTE)(p[x] + b);
            else if (f == 3) p[x] = (UBYTE)(p[x] + ((a + b) >> 1));
            else if (f == 4) p[x] = (UBYTE)(p[x] + sh_paeth(a, b, c));
        }
        for (x = 0; x < iw; x++) {
            UBYTE *o = rgb + (y * iw + x) * 3, *q = p + x * bpp;
            if (ctype == 2 || ctype == 6) { o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; }
            else if (ctype == 3) { o[0] = pal[q[0] * 3]; o[1] = pal[q[0] * 3 + 1]; o[2] = pal[q[0] * 3 + 2]; }
            else { o[0] = o[1] = o[2] = q[0]; }
        }
    }
    free(raw);
    *w = (int)iw; *h = (int)ih;
    return rgb;
}
/* BMP: uncompressed 24 or 32 bit */
static UBYTE *sh_bmp(const UBYTE *d, ULONG n, int *w, int *h)
{
    ULONG off, iw, x, y, stride;
    LONG ih;
    int bits, flip = 1;
    UBYTE *rgb;
    if (n < 54 || d[0] != 'B' || d[1] != 'M') return NULL;
    off = d[10] | d[11] << 8 | (ULONG)d[12] << 16 | (ULONG)d[13] << 24;
    iw = d[18] | d[19] << 8 | (ULONG)d[20] << 16 | (ULONG)d[21] << 24;
    ih = (LONG)(d[22] | d[23] << 8 | (ULONG)d[24] << 16 | (ULONG)d[25] << 24);
    bits = d[28] | d[29] << 8;
    if (ih < 0) { ih = -ih; flip = 0; }
    if ((bits != 24 && bits != 32) || !iw || !ih || iw > 8192 || ih > 8192 || (d[30] | d[31] << 8)) return NULL;
    stride = (iw * (bits / 8) + 3) & ~3UL;
    if (off + stride * (ULONG)ih > n) return NULL;
    rgb = (UBYTE *)malloc(iw * ih * 3);
    if (!rgb) return NULL;
    for (y = 0; y < (ULONG)ih; y++) {
        const UBYTE *row = d + off + (flip ? (ih - 1 - y) : y) * stride;
        for (x = 0; x < iw; x++) {
            const UBYTE *p = row + x * (bits / 8);
            UBYTE *o = rgb + (y * iw + x) * 3;
            o[0] = p[2]; o[1] = p[1]; o[2] = p[0];
        }
    }
    *w = (int)iw; *h = (int)ih;
    return rgb;
}
/* PPM P6, 8 bit; also .qrxwp */
static UBYTE *sh_ppm(const UBYTE *d, ULONG n, int *w, int *h)
{
    ULONG pos = 2, v[3];
    int k;
    UBYTE *rgb;
    if (n > 12 && !memcmp(d, "QRXW", 4)) {
        ULONG iw = sh_be32(d + 4), ih = sh_be32(d + 8);
        if (!iw || !ih || 12 + iw * ih * 3 > n) return NULL;
        rgb = (UBYTE *)malloc(iw * ih * 3);
        if (rgb) memcpy(rgb, d + 12, iw * ih * 3);
        *w = (int)iw; *h = (int)ih;
        return rgb;
    }
    if (n < 8 || d[0] != 'P' || d[1] != '6') return NULL;
    for (k = 0; k < 3; k++) {
        while (pos < n && (d[pos] == ' ' || d[pos] == '\n' || d[pos] == '\r' || d[pos] == '\t' || d[pos] == '#')) {
            if (d[pos] == '#') while (pos < n && d[pos] != '\n') pos++;
            else pos++;
        }
        v[k] = 0;
        while (pos < n && d[pos] >= '0' && d[pos] <= '9') v[k] = v[k] * 10 + (d[pos++] - '0');
    }
    pos++;
    if (v[2] != 255 || !v[0] || !v[1] || pos + v[0] * v[1] * 3 > n) return NULL;
    rgb = (UBYTE *)malloc(v[0] * v[1] * 3);
    if (rgb) memcpy(rgb, d + pos, v[0] * v[1] * 3);
    *w = (int)v[0]; *h = (int)v[1];
    return rgb;
}
/* Other formats through datatypes, where the system has them */
static UBYTE *sh_datatype(const char *path, int *w, int *h)
{
    Object *o;
    struct BitMapHeader *bmh = NULL;
    UBYTE *rgb = NULL;
    if (!DataTypesBase) DataTypesBase = OpenLibrary((STRPTR)"datatypes.library", 39);
    if (!DataTypesBase) return NULL;
    o = NewDTObject((APTR)path, DTA_GroupID, GID_PICTURE, PDTA_DestMode, PMODE_V43, PDTA_Remap, FALSE, TAG_DONE);
    if (!o) return NULL;
    if (GetDTAttrs(o, PDTA_BitMapHeader, (IPTR)&bmh, TAG_DONE) && bmh && bmh->bmh_Width && bmh->bmh_Height) {
        rgb = (UBYTE *)malloc((ULONG)bmh->bmh_Width * bmh->bmh_Height * 3);
        if (rgb) {
            struct pdtBlitPixelArray m;
            memset(&m, 0, sizeof m);
            m.MethodID = PDTM_READPIXELARRAY;
            m.pbpa_PixelData = rgb; m.pbpa_PixelFormat = PBPAFMT_RGB; m.pbpa_PixelArrayMod = bmh->bmh_Width * 3;
            m.pbpa_Left = 0; m.pbpa_Top = 0; m.pbpa_Width = bmh->bmh_Width; m.pbpa_Height = bmh->bmh_Height;
            if (DoMethodA(o, (Msg)&m)) { *w = bmh->bmh_Width; *h = bmh->bmh_Height; }
            else { free(rgb); rgb = NULL; }
        }
    }
    DisposeDTObject(o);
    return rgb;
}
static UBYTE *sh_load_picture(const char *path, int *w, int *h)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    UBYTE *d, *rgb = NULL;
    LONG n;
    if (!fh) return NULL;
    Seek(fh, 0, OFFSET_END);
    n = Seek(fh, 0, OFFSET_BEGINNING);
    d = n > 0 ? (UBYTE *)malloc((ULONG)n) : NULL;
    if (d && Read(fh, d, n) == n) {
        rgb = sh_png(d, (ULONG)n, w, h);
        if (!rgb) rgb = sh_bmp(d, (ULONG)n, w, h);
        if (!rgb) rgb = sh_ppm(d, (ULONG)n, w, h);
    }
    free(d);
    Close(fh);
    if (!rgb) rgb = sh_datatype(path, w, h);
    return rgb;
}
/* The picture as the wallpaper "Custom": fitted to the screen (filled, the rest cut off) */
static int qrx_shell_set_wallpaper(const char *path)
{
    int iw = 0, ih = 0, sw = g_screen_width, shh = g_screen_height, x, y;
    UBYTE *img = sh_load_picture(path, &iw, &ih), hdr[12];
    char out[96];
    BPTR fh, l;
    long sx, sy, sc, ox, oy;
    UBYTE *line;
    if (!img) return 0;
    l = CreateDir((STRPTR)"SYS:QRX"); if (l) UnLock(l);
    l = CreateDir((STRPTR)"SYS:QRX/Wallpapers"); if (l) UnLock(l);
    snprintf(out, sizeof out, "SYS:QRX/Wallpapers/Custom-%dx%d.qrxwp", sw, shh);
    fh = Open((STRPTR)out, MODE_NEWFILE);
    line = (UBYTE *)malloc((ULONG)sw * 3);
    if (!fh || !line) { if (fh) Close(fh); free(line); free(img); return 0; }
    memcpy(hdr, "QRXW", 4);
    hdr[4] = (UBYTE)(sw >> 24); hdr[5] = (UBYTE)(sw >> 16); hdr[6] = (UBYTE)(sw >> 8); hdr[7] = (UBYTE)sw;
    hdr[8] = (UBYTE)(shh >> 24); hdr[9] = (UBYTE)(shh >> 16); hdr[10] = (UBYTE)(shh >> 8); hdr[11] = (UBYTE)shh;
    Write(fh, hdr, 12);
    sx = (long)iw * 4096 / sw; sy = (long)ih * 4096 / shh; sc = sx < sy ? sx : sy;
    ox = ((long)iw * 4096 - sc * sw) / 2; oy = ((long)ih * 4096 - sc * shh) / 2;
    for (y = 0; y < shh; y++) {
        const UBYTE *row = img + ((oy + sc * y) >> 12) * iw * 3;
        for (x = 0; x < sw; x++) {
            const UBYTE *p = row + ((ox + sc * x) >> 12) * 3;
            line[x * 3] = p[0]; line[x * 3 + 1] = p[1]; line[x * 3 + 2] = p[2];
        }
        Write(fh, line, sw * 3);
    }
    Close(fh);
    free(line);
    free(img);
    strcpy(g_wallpaper, "Custom");
    qrx_wallpaper_forget();
    redraw_desktop();
    save_preferences();
    return 1;
}
