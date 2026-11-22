/* QRXInfo: "About QRX OS One" with the system's specs and its load, live.
 *   QRXInfo          a window, updated every second
 *   QRXInfo TEXT     the same once as text in the shell
 * On the 68100EC64 board (QRX OS One 64) it reads the cores, their idle time, the host CPU and the
 * services from the board (qrx_ec64_board.h); elsewhere it shows what exec and graphics know.
 * Built with the AROS 64 SDK: cc64 -O2 -o QRXInfo QRXInfo.c */
#include <exec/execbase.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/rpattr.h>
#include <graphics/view.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <stdio.h>
#include <string.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

#define BOARD_Q(off) (*(volatile UQUAD *)(0xFFFFFFFF00000000UL + (off)))
#define BOARD_L(off) (*(volatile ULONG *)(0xFFFFFFFF00000000UL + (off)))
#define BOARD_MAGIC 0x5152584543363442UL

static int board;
static unsigned cores = 1;
static UQUAD idle_prev[8], t_prev;
static int load[8];

static int have_board(void)
{
#if defined(__m68k64__)
    return BOARD_Q(0) == BOARD_MAGIC;
#else
    return 0;
#endif
}
static void host_cpu(char *out, int n)
{
    strcpy(out, "-");
#if defined(__m68k64__)
    if (!board) return;
    char s[65];
    for (int i = 0; i < 8; i++) {
        UQUAD v = BOARD_Q(0x380 + i * 8);
        for (int k = 0; k < 8; k++) s[i * 8 + k] = (char)(v >> (56 - 8 * k));
    }
    s[64] = 0;
    snprintf(out, n, "%s", s);
#endif
}
/* Load per core in percent since the last call: 100 - idle share */
static void sample_load(void)
{
#if defined(__m68k64__)
    if (!board) return;
    UQUAD t = BOARD_Q(0x368);
    for (unsigned i = 0; i < cores && i < 8; i++) {
        UQUAD idle = BOARD_Q(0x320 + i * 8);
        if (t_prev && t > t_prev) {
            UQUAD dt = t - t_prev, di = idle - idle_prev[i];
            int l = 100 - (int)(di * 100 / dt);
            load[i] = l < 0 ? 0 : l > 100 ? 100 : l;
        }
        idle_prev[i] = idle;
    }
    t_prev = t;
#endif
}
static void uptime_str(char *out, int n)
{
#if defined(__m68k64__)
    if (board) {
        UQUAD s = BOARD_Q(0x368) / 1000000;
        snprintf(out, n, "%lluh %02llum %02llus", s / 3600, s / 60 % 60, s % 60);
        return;
    }
#endif
    snprintf(out, n, "-");
}
static ULONG mb(ULONG flags) { return (ULONG)(AvailMem(flags) >> 20); }

/* ---------------------------------------------------------------- the window */
#define W 560
#define H 470
enum { C_BG = 0xFF111827, C_CARD = 0xFF1F2937, C_TEXT = 0xFFE5E7EB, C_DIM = 0xFF9CA3AF, C_ACCENT = 0xFF3B82F6,
       C_GOOD = 0xFF22C55E, C_WARN = 0xFFF59E0B, C_BAD = 0xFFEF4444, C_TRACK = 0xFF374151 };

/* Colours as pens of the window's screen, obtained once each */
static struct ColorMap *g_cm;
static struct { ULONG argb; LONG pen; } g_pens[16];
static int g_npens;
static LONG pen_of(ULONG argb)
{
    for (int i = 0; i < g_npens; i++) if (g_pens[i].argb == argb) return g_pens[i].pen;
    ULONG r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    LONG pen = ObtainBestPen(g_cm, r * 0x01010101UL, g * 0x01010101UL, b * 0x01010101UL,
                             OBP_Precision, PRECISION_EXACT, TAG_DONE);
    if (pen < 0) pen = 1;
    if (g_npens < 16) { g_pens[g_npens].argb = argb; g_pens[g_npens].pen = pen; g_npens++; }
    return pen;
}
static void release_pens(void)
{
    for (int i = 0; i < g_npens; i++) ReleasePen(g_cm, g_pens[i].pen);
    g_npens = 0;
}
static void fill(struct RastPort *rp, int x, int y, int w, int h, ULONG argb)
{
    SetAPen(rp, pen_of(argb));
    RectFill(rp, x, y, x + w - 1, y + h - 1);
}
static void text(struct RastPort *rp, int x, int y, ULONG argb, const char *s)
{
    SetAPen(rp, pen_of(argb));
    SetDrMd(rp, JAM1);
    Move(rp, x, y + rp->TxBaseline);
    Text(rp, (CONST_STRPTR)s, strlen(s));
}
static void bar(struct RastPort *rp, int x, int y, int w, int pct)
{
    ULONG c = pct < 60 ? C_GOOD : pct < 85 ? C_WARN : C_BAD;
    fill(rp, x, y, w, 8, C_TRACK);
    if (pct > 0) fill(rp, x, y, w * pct / 100, 8, c);
}
static int row(struct RastPort *rp, int x, int y, const char *k, const char *v)
{
    text(rp, x, y, C_DIM, k);
    text(rp, x + 150, y, C_TEXT, v);
    return y + 16;
}
static void draw(struct Window *win)
{
    struct RastPort *rp = win->RPort;
    int x0 = win->BorderLeft, y0 = win->BorderTop, w = win->Width - win->BorderLeft - win->BorderRight;
    char s[96], cpu[72];
    struct ExecBase *eb = SysBase;
    struct Screen *scr = win->WScreen;

    fill(rp, x0, y0, w, win->Height - y0 - win->BorderBottom, C_BG);
    /* header */
    fill(rp, x0, y0, w, 64, C_CARD);
    fill(rp, x0, y0 + 63, w, 2, C_ACCENT);
    fill(rp, x0 + 16, y0 + 14, 36, 36, C_ACCENT);
    text(rp, x0 + 22, y0 + 26, 0xFFFFFFFF, "QRX");
    text(rp, x0 + 66, y0 + 16, C_TEXT, board ? "QRX OS One 64" : "QRX OS One");
    snprintf(s, sizeof s, "AROS exec %d.%d  -  %s", eb->LibNode.lib_Version, eb->LibNode.lib_Revision,
             board ? "64-bit SMP on the 68100EC64" : "68k");
    text(rp, x0 + 66, y0 + 34, C_DIM, s);

    int y = y0 + 80, x = x0 + 16;
    text(rp, x, y, C_ACCENT, "SYSTEM"); y += 18;
    snprintf(s, sizeof s, "68100EC64, %u %s", cores, cores == 1 ? "core" : "cores");
    y = row(rp, x, y, "Processor", board ? s : "68k");
    host_cpu(cpu, sizeof cpu);
    y = row(rp, x, y, "Host", cpu);
    snprintf(s, sizeof s, "%lu MB, %lu MB free", (unsigned long)(board ? BOARD_Q(0x360) >> 20 : mb(MEMF_TOTAL)),
             (unsigned long)mb(MEMF_ANY));
    y = row(rp, x, y, "Memory", s);
    snprintf(s, sizeof s, "%d x %d, %d bit", scr->Width, scr->Height, GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH));
    y = row(rp, x, y, "Display", s);
    if (board) {
        ULONG f = BOARD_L(0x408);
        y = row(rp, x, y, "Sound", "48 kHz stereo, board ring to CoreAudio");
        y = row(rp, x, y, "Network", (f & 1) ? "host sockets (bsdsocket.library)" : "-");
        y = row(rp, x, y, "3D", (f & 2) ? "OpenGL 2.1 through the board (Metal)" : "-");
    }
    uptime_str(s, sizeof s);
    y = row(rp, x, y, "Uptime", s);

    y += 10;
    text(rp, x, y, C_ACCENT, "LOAD"); y += 18;
    for (unsigned i = 0; board && i < cores && i < 8; i++) {
        snprintf(s, sizeof s, "Core %u", i);
        text(rp, x, y, C_DIM, s);
        bar(rp, x + 150, y + 3, w - 32 - 150 - 50, load[i]);
        snprintf(s, sizeof s, "%3d%%", load[i]);
        text(rp, x0 + w - 56, y, C_TEXT, s);
        y += 16;
    }
    {
        ULONG total = board ? (ULONG)(BOARD_Q(0x360) >> 20) : mb(MEMF_TOTAL), freem = mb(MEMF_ANY);
        int pct = total ? (int)((total - (freem > total ? total : freem)) * 100 / total) : 0;
        text(rp, x, y, C_DIM, "Memory");
        bar(rp, x + 150, y + 3, w - 32 - 150 - 50, pct);
        snprintf(s, sizeof s, "%3d%%", pct);
        text(rp, x0 + w - 56, y, C_TEXT, s);
    }
}

static void print_text(void)
{
    char cpu[72], up[32];
    struct ExecBase *eb = SysBase;
    host_cpu(cpu, sizeof cpu);
    uptime_str(up, sizeof up);
    printf("%s - AROS exec %d.%d\n", board ? "QRX OS One 64 (64-bit SMP)" : "QRX OS One", eb->LibNode.lib_Version, eb->LibNode.lib_Revision);
    if (board) {
        ULONG f = BOARD_L(0x408);
        printf("Processor  68100EC64, %u cores\nHost       %s\nMemory     %lu MB, %lu MB free\n", cores, cpu,
               (unsigned long)(BOARD_Q(0x360) >> 20), (unsigned long)mb(MEMF_ANY));
        printf("Sound      48 kHz stereo (board)\nNetwork    %s\n3D         %s\nUptime     %s\n",
               (f & 1) ? "host sockets" : "-", (f & 2) ? "OpenGL 2.1 (Metal)" : "-", up);
        sample_load();
        Delay(50);
        sample_load();
        for (unsigned i = 0; i < cores && i < 8; i++) printf("Core %u     %3d%% busy\n", i, load[i]);
    } else
        printf("Memory     %lu MB free\n", (unsigned long)mb(MEMF_ANY));
}

int main(int argc, char **argv)
{
    board = have_board();
#if defined(__m68k64__)
    if (board) { cores = BOARD_L(0x31C); if (!cores || cores > 8) cores = 1; }
#endif
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (argc > 1 || !IntuitionBase || !GfxBase) { print_text(); return 0; }
    sample_load();
    struct Window *win = OpenWindowTags(NULL, WA_Title, (IPTR)"About QRX OS One", WA_InnerWidth, W,
        WA_InnerHeight, 140 + 16 * 8 + 16 * (cores + 1) + 40, WA_DragBar, TRUE, WA_DepthGadget, TRUE,
        WA_CloseGadget, TRUE, WA_Activate, TRUE, WA_SimpleRefresh, TRUE, WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW,
        TAG_DONE);
    if (!win) { print_text(); return 0; }
    g_cm = win->WScreen->ViewPort.ColorMap;
    int done = 0, tick = 0;
    draw(win);
    while (!done) {
        struct IntuiMessage *m;
        while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            if (m->Class == IDCMP_CLOSEWINDOW) done = 1;
            if (m->Class == IDCMP_REFRESHWINDOW) { BeginRefresh(win); EndRefresh(win, TRUE); draw(win); }
            ReplyMsg((struct Message *)m);
        }
        Delay(5);
        if (++tick >= 10) { tick = 0; sample_load(); draw(win); }
    }
    release_pens();
    CloseWindow(win);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
