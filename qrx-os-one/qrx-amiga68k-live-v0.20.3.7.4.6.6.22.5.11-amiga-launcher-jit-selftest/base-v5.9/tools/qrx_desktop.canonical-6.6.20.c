/* QRX 6.6.20 canonical desktop source - no deploy-time legacy repatching */
/* QRX 6.6.13 productive desktop audit */
/* QRX v0.20.3.7.4.6.6.11 unified desktop UX */
/* QRX v0.20.3.7.4.6.6.10 layer-safe desktop */
/* QRX v0.20.3.7.4.6.6.6 desktop interaction + flicker-free terminal + AI/date */
/*
 * QRX OS One - Amiga 68K native desktop prototype v0.20
 *
 * Target:
 *   AmigaOS 3.x compatible ABI / AROS m68k
 *   RTG/P96-class modes preferred, AGA 640x512x8 fallback
 *
 * v0.9:
 *   - preserves all v0.7 RTG/USB/Productivity/Legacy functionality
 *   - QRX POSIX ABI v1 for native Unix/Linux source ports
 *   - Unix-style path translation over AmigaDOS volumes
 *   - file/stat/env/spawn/task/mmap compatibility helpers
 *   - bsdsocket.library capability bridge
 *   - native QRXUnix toolset and terminal Unix aliases
 *   - Linux/m68k ELF32 direct runtime for selected static/freestanding guests
 *   - PT_LOAD loader + ET_DYN R_68K_RELATIVE relocation path
 *   - trap #0 Linux syscall ABI bridge (D0, D1-D5, A0)
 *   - QRX POSIX SDK + native hello/self-test HUNK applications
 *
 * Legacy TAKEOVER still releases the selected QRX screen before classic apps.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <proto/icon.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "qrx_wallpaper.h"

#ifndef INVALID_ID
#define INVALID_ID 0xFFFFFFFFUL
#endif

#define TERM_MAX_LINES 22
#define TERM_LINE_LEN  76
#define INPUT_LEN       160
#define PREFS_PATH      "SYS:QRX/Prefs/desktop.cfg"
#define DOS_CAPTURE     "T:qrx-command-output.txt"

#define LEGACY_LOG_PATH  "SYS:QRX/Legacy/last-session.txt"
#define LEGACY_CMD_LEN   700
#define LEGACY_OPT_LEN   240
#define LEGACY_PATH_LEN  380

#define SESSION_NONE      0
#define SESSION_EXIT      1
#define SESSION_TAKEOVER  2
#define SESSION_WHDLOAD   3

#define RUNMODE_AUTO      0
#define RUNMODE_NORMAL    1
#define RUNMODE_TAKEOVER  2
#define RUNMODE_WHDLOAD   3

#define TERM_MIN_W 360
#define TERM_MIN_H 220
#define TITLE_H    30
#define RESIZE_GRAB 14

#define FILE_MAX_ENTRIES 64
#define FILE_PATH_LEN    255
#define FILE_ROW_H       30
#define FILE_LIST_TOP    112
#define FILE_LEFT_W      100
#define FILE_MIN_W       430
#define FILE_MIN_H       340
#define FILE_GRID_W       96
#define FILE_GRID_H       70
#define FILE_ICON_W       46
#define FILE_ICON_H       32
#define DESKTOP_MAX       16
#define DESKTOP_SCAN_PATH "QRX:Desktop"

#define CLIP_NONE 0
#define CLIP_COPY 1
#define CLIP_MOVE 2

/* Palette 0..223 belongs to the wallpaper.
 * 224..255 are reserved for native QRX desktop/window chrome.
 */
#define PEN_BAR       224
#define PEN_BAR_TEXT  225
#define PEN_ACCENT    226
#define PEN_PANEL     227
#define PEN_PANEL_2   228
#define PEN_TEXT      229
#define PEN_MUTED     230
#define PEN_EDGE      231
#define PEN_LIGHT     232
#define PEN_DARK      233
#define PEN_RED       234
#define PEN_YELLOW    235
#define PEN_GREEN     236
#define PEN_WHITE     237
#define PEN_BLACK     238
#define PEN_BUTTON    239
#define PEN_BLUE      240
#define PEN_GREY      241
#define PEN_GREY_2    242
#define PEN_AMIGA     243
#define PEN_AMIGA_HI  244

enum LookFeel {
    LOOK_QRX = 0,
    LOOK_WINDOWS = 1,
    LOOK_MACOS = 2,
    LOOK_AMIGAOS = 3
};


struct QRXLaunchProfile {
    UBYTE mode;
    char slave[LEGACY_PATH_LEN];
    char options[LEGACY_OPT_LEN];
    char args[LEGACY_OPT_LEN];
};

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct DosLibrary *DOSBase = NULL;
struct Library *IconBase = NULL;
struct Library *PoseidonBase = NULL;

static struct Screen *g_screen = NULL;
static struct Window *g_desktop = NULL;
static struct Window *g_terminal = NULL;
static struct Window *g_files = NULL;
static WORD g_screen_width = QRX_W;
static WORD g_screen_height = QRX_H;
static UBYTE g_screen_depth = QRX_DEPTH;
static ULONG g_screen_mode = INVALID_ID;
static int g_display_is_rtg = 0;
static char g_display_label[64] = "AGA 640x512x8";

/* QRX v0.20.3.7.4.6.5.7 existing public screen + low-depth compatibility */
static int g_screen_borrowed = 0;

/* QRX v0.20.3.7.4.6.5.8 requester-free volumes + clean public-screen host */
#define QRX_MOUNTED_VOLUME_MAX 4
#define QRX_MOUNTED_VOLUME_NAME 64

struct QRXMountedVolume {
    char name[QRX_MOUNTED_VOLUME_NAME];
    char path[QRX_MOUNTED_VOLUME_NAME + 2];
};

static struct QRXMountedVolume g_mounted_volumes[QRX_MOUNTED_VOLUME_MAX];
static int g_mounted_volume_count = 0;
static struct Process *g_qrx_process = NULL;
static APTR g_qrx_saved_window_ptr = NULL;
static int g_qrx_requesters_disabled = 0;
static ULONG g_host_palette[256 * 3];
static ULONG g_host_palette_count = 0;
static int g_host_palette_saved = 0;
static int g_host_title_was_visible = 0;

static enum LookFeel g_look = LOOK_QRX;


static int g_session_action = SESSION_NONE;
static char g_session_command[LEGACY_CMD_LEN] = "";
static char g_session_slave[LEGACY_PATH_LEN] = "";
static char g_session_data[LEGACY_PATH_LEN] = "";
static char g_session_options[LEGACY_OPT_LEN] = "";
static char g_resume_notice[TERM_LINE_LEN + 1] = "";

struct QRXFileEntry {
    char name[108];
    LONG dir_type;
    LONG size;
    ULONG protection;
    UBYTE has_icon;
    UBYTE generated_icon;
    UBYTE icon_type;
    char default_tool[128];
};

struct QRXDesktopEntry {
    char name[108];
    char full_path[FILE_PATH_LEN + 128];
    LONG dir_type;
    UBYTE has_icon;
    UBYTE generated_icon;
    UBYTE icon_type;
    char default_tool[128];
};

static struct QRXFileEntry g_file_entries[FILE_MAX_ENTRIES];
static int g_file_count = 0;
static int g_file_selected = -1;
static int g_file_scroll = 0;
static char g_files_path[FILE_PATH_LEN + 1] = "SYS:";
static char g_term_cwd[FILE_PATH_LEN + 1] = "SYS:";
#define TERM_HISTORY 24
static char g_term_history[TERM_HISTORY][INPUT_LEN + 1];
static int g_term_hist_count = 0, g_term_hist_pos = 0;
static int g_file_icon_view = 1;
/* 22.5.11: QRXWelcome autostart can be switched off to save one process + window (prefs welcome=off). */
static int g_welcome_autostart = 1;
static char g_file_status[160] = "";
static int g_delete_armed = 0;

static int g_clip_mode = CLIP_NONE;
static char g_clip_source[FILE_PATH_LEN + 128] = "";
static char g_clip_name[108] = "";
static int g_clip_is_dir = 0;
static int g_clip_has_icon = 0;

static int g_rename_active = 0;
static char g_rename_text[108] = "";
static int g_rename_len = 0;

static int g_object_drag_candidate = -1;
static int g_object_dragging = 0;
static WORD g_object_drag_start_x = 0;
static WORD g_object_drag_start_y = 0;

static struct QRXDesktopEntry g_desktop_entries[DESKTOP_MAX];
static int g_desktop_count = 0;
static int g_desktop_selected = -1;
static int g_context_open=0; static WORD g_context_x=0,g_context_y=0;
static int g_desktop_last_click = -1;
static ULONG g_desktop_last_sec = 0;
static ULONG g_desktop_last_micro = 0;

static WORD g_files_left = 55;
static WORD g_files_top = 72;
static WORD g_files_width = 530;
static WORD g_files_height = 360;
static int g_files_dragging = 0;
static int g_files_resizing = 0;
static WORD g_files_gesture_abs_x = 0;
static WORD g_files_gesture_abs_y = 0;
static WORD g_files_gesture_left = 0;
static WORD g_files_gesture_top = 0;
static WORD g_files_gesture_width = 0;
static WORD g_files_gesture_height = 0;

static int g_last_file_click = -1;
static ULONG g_last_file_sec = 0;
static ULONG g_last_file_micro = 0;

static WORD g_term_left = 40;
static WORD g_term_top = 58;
static WORD g_term_width = 560;
static WORD g_term_height = 360;

static int g_dragging = 0;
static int g_resizing = 0;
static WORD g_gesture_abs_x = 0;
static WORD g_gesture_abs_y = 0;
static WORD g_gesture_left = 0;
static WORD g_gesture_top = 0;
static WORD g_gesture_width = 0;
static WORD g_gesture_height = 0;

static char lines[TERM_MAX_LINES][TERM_LINE_LEN + 1];
static int line_count = 0;
static char input[INPUT_LEN + 1];
static int input_len = 0;
static int qrx_term_scroll = 0;
static void qrx_complete(void){static const char*cmds[]={"help","version","info","mem","clear","about","writer","sheet","presenter","hardware","files","htop","ai","compat","qrx86","x86run","winrun","wineboot","reactosnotepad","amigarun","amigacheck","i386","win32","rechner","minesweeper","uhr",0};int i,n=0,hit=-1;for(i=0;cmds[i];i++)if(!strncmp(cmds[i],input,input_len)){n++;hit=i;}if(n==1){strncpy(input,cmds[hit],INPUT_LEN);input[INPUT_LEN]=0;input_len=strlen(input);}}

static ULONG rgb32(UBYTE v) {
    return ((ULONG)v << 24) | ((ULONG)v << 16) | ((ULONG)v << 8) | (ULONG)v;
}

/*
 * On the classic Deadwood bootstrap screen the working public screen can be
 * shallower than 8-bit.  Keep the logical QRX chrome pens stable while mapping
 * them to a compact 16-colour set.  8-bit/RTG screens keep the original pens.
 */
static UBYTE qrx_pen(UBYTE pen) {
    UBYTE mapped;
    UBYTE mask;
    if (g_screen_depth >= 8) return pen;
    switch (pen) {
        case PEN_BAR:      mapped = 1;  break;
        case PEN_BAR_TEXT: mapped = 15; break;
        case PEN_ACCENT:   mapped = 10; break;
        case PEN_PANEL:    mapped = 8;  break;
        case PEN_PANEL_2:  mapped = 0;  break;
        case PEN_TEXT:     mapped = 15; break;
        case PEN_MUTED:    mapped = 7;  break;
        case PEN_EDGE:     mapped = 0;  break;
        case PEN_LIGHT:    mapped = 15; break;
        case PEN_DARK:     mapped = 0;  break;
        case PEN_RED:      mapped = 12; break;
        case PEN_YELLOW:   mapped = 14; break;
        case PEN_GREEN:    mapped = 10; break;
        case PEN_WHITE:    mapped = 15; break;
        case PEN_BLACK:    mapped = 0;  break;
        case PEN_BUTTON:   mapped = 7;  break;
        case PEN_BLUE:     mapped = 9;  break;
        case PEN_GREY:     mapped = 8;  break;
        case PEN_GREY_2:   mapped = 7;  break;
        case PEN_AMIGA:    mapped = 9;  break;
        case PEN_AMIGA_HI: mapped = 11; break;
        default:           mapped = (UBYTE)(pen & 15); break;
    }
    mask = (UBYTE)((1U << (g_screen_depth ? g_screen_depth : 1)) - 1U);
    return (UBYTE)(mapped & mask);
}

static void set_color(ULONG pen, UBYTE r, UBYTE g, UBYTE b) {
    if (!g_screen) return;
    SetRGB32(&g_screen->ViewPort, pen, rgb32(r), rgb32(g), rgb32(b));
}

static void apply_look_palette(void) {
    /* Borrowed screens are mutated only after their palette was saved. */
    if (g_screen_borrowed && !g_host_palette_saved) return;

    if (g_screen_depth <= 2) {
        /* OCS 2-bit baseline: four roles, maximum separation, no gradients. */
        set_color(0, 8, 14, 24); set_color(1, 32, 78, 142);
        set_color(2, 142, 160, 174); set_color(3, 244, 248, 252);
        return;
    }
    if (g_screen_depth < 8) {
        static const UBYTE low[16][3] = {
            {  3,   5,   8}, { 18,  52, 110}, { 32,  72,  96}, { 58,  92, 122},
            {120,  40,  40}, {120,  70,  30}, { 50, 105,  70}, {150, 156, 164},
            { 82,  88,  98}, { 48, 110, 220}, { 67, 219, 184}, {140, 190, 245},
            {239,  83,  80}, {200, 100, 190}, {244, 190,  70}, {245, 248, 252}
        };
        ULONG i, count = 1UL << (g_screen_depth ? g_screen_depth : 1);
        if (count > 16) count = 16;
        for (i = 0; i < count; ++i)
            set_color(i, low[i][0], low[i][1], low[i][2]);
        return;
    }

    set_color(PEN_RED,       239,  83,  80);
    set_color(PEN_YELLOW,    244, 190,  70);
    set_color(PEN_GREEN,      66, 190, 101);
    set_color(PEN_WHITE,     245, 248, 252);
    set_color(PEN_BLACK,       3,   5,   8);
    set_color(PEN_BLUE,       28,  86, 186);
    set_color(PEN_GREY,      184, 184, 184);
    set_color(PEN_GREY_2,    112, 112, 112);
    set_color(PEN_AMIGA,      70, 102, 176);
    set_color(PEN_AMIGA_HI,  171, 189, 231);

    if (g_look == LOOK_WINDOWS) {
        set_color(PEN_BAR,      35,  84, 169);
        set_color(PEN_BAR_TEXT,255, 255, 255);
        set_color(PEN_ACCENT,   52, 120, 246);
        set_color(PEN_PANEL,   192, 192, 192);
        set_color(PEN_PANEL_2,  24,  26,  32);
        set_color(PEN_TEXT,    238, 242, 248);
        set_color(PEN_MUTED,   154, 164, 178);
        set_color(PEN_EDGE,     48,  48,  48);
        set_color(PEN_LIGHT,   255, 255, 255);
        set_color(PEN_DARK,     72,  72,  72);
        set_color(PEN_BUTTON,  208, 208, 208);
    } else if (g_look == LOOK_MACOS) {
        set_color(PEN_BAR,     225, 225, 228);
        set_color(PEN_BAR_TEXT, 30,  30,  34);
        set_color(PEN_ACCENT,   58, 145, 255);
        set_color(PEN_PANEL,    42,  44,  52);
        set_color(PEN_PANEL_2,  18,  20,  26);
        set_color(PEN_TEXT,    242, 244, 248);
        set_color(PEN_MUTED,   160, 164, 174);
        set_color(PEN_EDGE,     82,  84,  92);
        set_color(PEN_LIGHT,   242, 242, 244);
        set_color(PEN_DARK,     20,  22,  26);
        set_color(PEN_BUTTON,  204, 204, 208);
    } else if (g_look == LOOK_AMIGAOS) {
        set_color(PEN_BAR,      70, 102, 176);
        set_color(PEN_BAR_TEXT,255, 255, 255);
        set_color(PEN_ACCENT,  171, 189, 231);
        set_color(PEN_PANEL,   174, 174, 174);
        set_color(PEN_PANEL_2,  18,  20,  24);
        set_color(PEN_TEXT,    244, 244, 244);
        set_color(PEN_MUTED,   174, 174, 174);
        set_color(PEN_EDGE,     26,  26,  26);
        set_color(PEN_LIGHT,   255, 255, 255);
        set_color(PEN_DARK,     70,  70,  70);
        set_color(PEN_BUTTON,  174, 174, 174);
    } else {
        set_color(PEN_BAR,      18,  48,  94);
        set_color(PEN_BAR_TEXT,235, 246, 255);
        set_color(PEN_ACCENT,   55, 174, 211);
        set_color(PEN_PANEL,    34,  54,  76);
        set_color(PEN_PANEL_2,  12,  24,  38);
        set_color(PEN_TEXT,    234, 245, 255);
        set_color(PEN_MUTED,   151, 174, 194);
        set_color(PEN_EDGE,     74, 102, 126);
        set_color(PEN_LIGHT,   174, 211, 232);
        set_color(PEN_DARK,      2,   7,  14);
        set_color(PEN_BUTTON,   48,  73,  96);
    }
}

static void add_line(const char *s) {
    int i;
    if (line_count >= TERM_MAX_LINES) {
        for (i = 1; i < TERM_MAX_LINES; ++i)
            strcpy(lines[i - 1], lines[i]);
        line_count = TERM_MAX_LINES - 1;
    }
    strncpy(lines[line_count], s ? s : "", TERM_LINE_LEN);
    lines[line_count][TERM_LINE_LEN] = 0;
    ++line_count;
}

static void add_wrapped(const char *s) {
    char buf[TERM_LINE_LEN + 1];
    const char *p = s;
    int n;

    if (!s || !*s) {
        add_line("");
        return;
    }

    while (*p) {
        n = (int)strlen(p);
        if (n > TERM_LINE_LEN) n = TERM_LINE_LEN;
        strncpy(buf, p, n);
        buf[n] = 0;
        add_line(buf);
        p += n;
    }
}

static void clear_lines(void) {
    line_count = 0;
    input_len = 0;
    input[0] = 0;
}

static const char *look_key(enum LookFeel look) {
    switch (look) {
        case LOOK_WINDOWS: return "windows";
        case LOOK_MACOS: return "macos";
        case LOOK_AMIGAOS: return "amigaos";
        default: return "qrx";
    }
}

static const char *look_name(enum LookFeel look) {
    switch (look) {
        case LOOK_WINDOWS: return "Windows";
        case LOOK_MACOS: return "macOS";
        case LOOK_AMIGAOS: return "AmigaOS";
        default: return "QRX";
    }
}

static int parse_look(const char *s, enum LookFeel *out) {
    if (!s || !out) return 0;
    if (!strcmp(s, "qrx")) *out = LOOK_QRX;
    else if (!strcmp(s, "windows") || !strcmp(s, "win")) *out = LOOK_WINDOWS;
    else if (!strcmp(s, "mac") || !strcmp(s, "macos")) *out = LOOK_MACOS;
    else if (!strcmp(s, "amiga") || !strcmp(s, "amigaos")) *out = LOOK_AMIGAOS;
    else return 0;
    return 1;
}

static void trim_eol(char *s) {
    size_t n;
    if (!s) return;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = 0;
        --n;
    }
}

static void load_preferences(void) {
    BPTR fh;
    char buf[160];
    enum LookFeel look;
    int l, t, w, h;

    fh = Open((STRPTR)PREFS_PATH, MODE_OLDFILE);
    if (!fh) return;

    while (FGets(fh, buf, sizeof(buf))) {
        trim_eol(buf);

        if (!strncmp(buf, "look=", 5)) {
            if (parse_look(buf + 5, &look))
                g_look = look;
        } else if (!strncmp(buf, "files=", 6)) {
            strncpy(g_files_path, buf + 6, FILE_PATH_LEN);
            g_files_path[FILE_PATH_LEN] = 0;
        } else if (!strncmp(buf, "view=", 5)) {
            g_file_icon_view = !strcmp(buf + 5, "icons");
        } else if (!strncmp(buf, "welcome=", 8)) {
            g_welcome_autostart = strcmp(buf + 8, "off") != 0;
        } else if (sscanf(buf, "terminal=%d,%d,%d,%d", &l, &t, &w, &h) == 4) {
            if (w >= TERM_MIN_W && h >= TERM_MIN_H &&
                l >= 0 && t >= 22 && l < 2048 && t < 1536) {
                if (w > 2048) w = 2048;
                if (h > 1536) h = 1536;
                if (w >= TERM_MIN_W && h >= TERM_MIN_H) {
                    g_term_left = (WORD)l;
                    g_term_top = (WORD)t;
                    g_term_width = (WORD)w;
                    g_term_height = (WORD)h;
                }
            }
        }
    }
    Close(fh);
}

static void sync_terminal_geometry(void) {
    if (!g_terminal) return;
    g_term_left = g_terminal->LeftEdge;
    g_term_top = g_terminal->TopEdge;
    g_term_width = g_terminal->Width;
    g_term_height = g_terminal->Height;
}

static void save_preferences(void) {
    BPTR fh;
    char buf[224];

    sync_terminal_geometry();
    fh = Open((STRPTR)PREFS_PATH, MODE_NEWFILE);
    if (!fh) return;

    snprintf(buf, sizeof(buf),
             "; QRX OS One Amiga 68K native desktop preferences v0.20\n"
             "look=%s\n"
             "terminal=%d,%d,%d,%d\n"
             "files=%s\n"
             "view=%s\n"
             "welcome=%s\n",
             look_key(g_look),
             (int)g_term_left, (int)g_term_top,
             (int)g_term_width, (int)g_term_height,
             g_files_path,
             g_file_icon_view ? "icons" : "list",
             g_welcome_autostart ? "on" : "off");
    FPuts(fh, buf);
    Close(fh);
}

static void text_at(struct RastPort *rp, UBYTE pen, WORD x, WORD y, const char *s) {
    if (!rp || !s) return;
    /* 22.5.11: every caller fills the background first; JAM1 keeps the host
     * screen's BPen (grey on a borrowed Wanderer screen) from boxing the text. */
    SetDrMd(rp, JAM1);
    SetAPen(rp, qrx_pen(pen));
    Move(rp, x, y);
    Text(rp, (STRPTR)s, (ULONG)strlen(s));
}

static void fill(struct RastPort *rp, UBYTE pen, WORD x1, WORD y1, WORD x2, WORD y2) {
    SetAPen(rp, qrx_pen(pen));
    RectFill(rp, x1, y1, x2, y2);
}

static void outline(struct RastPort *rp, UBYTE pen, WORD x1, WORD y1, WORD x2, WORD y2) {
    SetAPen(rp, qrx_pen(pen));
    Move(rp, x1, y1); Draw(rp, x2, y1); Draw(rp, x2, y2);
    Draw(rp, x1, y2); Draw(rp, x1, y1);
}

static void bevel(struct RastPort *rp, WORD x1, WORD y1, WORD x2, WORD y2, int raised) {
    UBYTE hi = raised ? PEN_LIGHT : PEN_DARK;
    UBYTE lo = raised ? PEN_DARK : PEN_LIGHT;
    SetAPen(rp, qrx_pen(hi));
    Move(rp, x1, y2); Draw(rp, x1, y1); Draw(rp, x2, y1);
    SetAPen(rp, qrx_pen(lo));
    Move(rp, x2, y1); Draw(rp, x2, y2); Draw(rp, x1, y2);
}

/* QRX v0.20.3.7.4.6.5.9 Flat Retro UI / ECS-OCS visual baseline */
static void qrx_flat_frame(struct RastPort *rp, WORD x1, WORD y1, WORD x2, WORD y2) {
    outline(rp, PEN_DARK, x1, y1, x2, y2);
    if (x2-x1 > 3 && y2-y1 > 3) outline(rp, PEN_LIGHT, x1+1, y1+1, x2-1, y2-1);
}
static void qrx_flat_button(struct RastPort *rp, WORD x1, WORD y1, WORD x2, WORD y2, UBYTE bg, int active) {
    fill(rp, bg, x1, y1, x2, y2); outline(rp, PEN_EDGE, x1, y1, x2, y2);
    SetAPen(rp, qrx_pen(active ? PEN_ACCENT : PEN_LIGHT));
    Move(rp,x1+1,y2-1); Draw(rp,x1+1,y1+1); Draw(rp,x2-1,y1+1);
    SetAPen(rp,qrx_pen(PEN_DARK)); Move(rp,x2-1,y1+1); Draw(rp,x2-1,y2-1); Draw(rp,x1+1,y2-1);
}
/* 22.5.11 resource saving: on RTG/deep screens the RLE wallpaper used to be
 * re-rendered with one RectFill per colour run on EVERY desktop redraw.  It is
 * now rendered once into an off-screen friend bitmap and blitted afterwards.
 * If the bitmap cannot be allocated the old direct path is used unchanged. */
static struct BitMap *g_wall_cache;
static WORD g_wall_cache_w, g_wall_cache_h;
static ULONG g_wall_renders, g_wall_blits;

static void qrx_free_wallpaper_cache(void) {
    if (g_wall_cache) {
        WaitBlit();
        FreeBitMap(g_wall_cache);
        g_wall_cache = NULL;
    }
    g_wall_cache_w = g_wall_cache_h = 0;
}

static void draw_wallpaper(struct Screen *s) {
    struct BitMap *bm;
    struct RastPort *rp;
    int p, y, i;

    if (!s) return;
    rp = &s->RastPort;
    bm = rp->BitMap;
    if (!bm) return;

    if (g_screen_depth < 8) {
        /* Compact classic fallback: no 256-colour wallpaper assumptions. */
        SetAPen(rp, qrx_pen(PEN_PANEL_2));
        RectFill(rp, 0, 0, g_screen_width - 1, g_screen_height - 1);
        SetAPen(rp, qrx_pen(PEN_BAR));
        RectFill(rp, 0, 0, g_screen_width - 1, 28);
        SetAPen(rp, qrx_pen(PEN_ACCENT));
        RectFill(rp, 0, 29, g_screen_width - 1, 31);
        return;
    }

    for (i = 0; i < QRX_COLORS; ++i) {
        SetRGB32(&s->ViewPort, (ULONG)i,
                 rgb32(qrx_palette[i][0]),
                 rgb32(qrx_palette[i][1]),
                 rgb32(qrx_palette[i][2]));
    }

    /*
     * Fast classic path: exact 640x512x8 planar AGA screen.
     * This preserves the v0.1-v0.6 direct bitplane renderer.
     */
    if (g_screen_depth <= 8 &&
        g_screen_width == QRX_W && g_screen_height == QRX_H) {
        for (p = 0; p < QRX_DEPTH; ++p) {
            if (!bm->Planes[p]) continue;
            for (y = 0; y < QRX_H; ++y) {
                UBYTE *dst = (UBYTE *)bm->Planes[p] +
                             (ULONG)y * bm->BytesPerRow;
                const UBYTE *src =
                    &qrx_planes[p][(ULONG)y * QRX_BYTES_PER_ROW];
                CopyMem((APTR)src, (APTR)dst, QRX_BYTES_PER_ROW);
            }
        }
        return;
    }

    /*
     * RTG/deep-screen path.  Do not touch bitmap planes directly: render
     * through graphics.library so P96/RTG owns the physical framebuffer.
     * The 640x512 QRX wallpaper is centered and the surrounding area uses
     * the QRX dark background.  Consecutive same-colour pixels are emitted
     * as RectFill runs to keep the 68K cost bounded.
     */
    SetAPen(rp, 0);
    RectFill(rp, 0, 0, g_screen_width - 1, g_screen_height - 1);

    {
        WORD xoff = (g_screen_width > QRX_W)
                  ? (g_screen_width - QRX_W) / 2 : 0;
        WORD yoff = (g_screen_height > QRX_H)
                  ? (g_screen_height - QRX_H) / 2 : 0;
        WORD draw_w = g_screen_width < QRX_W ? g_screen_width : QRX_W;
        WORD draw_h = g_screen_height < QRX_H ? g_screen_height : QRX_H;
        struct RastPort crp;
        struct RastPort *dst = rp;
        WORD dx = xoff, dy = yoff;

        if (g_wall_cache && (g_wall_cache_w != draw_w || g_wall_cache_h != draw_h))
            qrx_free_wallpaper_cache();
        if (g_wall_cache) {
            BltBitMapRastPort(g_wall_cache, 0, 0, rp, xoff, yoff, draw_w, draw_h, 0xC0);
            g_wall_blits++;
            return;
        }
        g_wall_cache = AllocBitMap((ULONG)draw_w, (ULONG)draw_h,
                                   GetBitMapAttr(bm, BMA_DEPTH), BMF_MINPLANES, bm);
        if (g_wall_cache) {
            InitRastPort(&crp);
            crp.BitMap = g_wall_cache;
            dst = &crp;
            dx = dy = 0;
            g_wall_cache_w = draw_w;
            g_wall_cache_h = draw_h;
        }
        g_wall_renders++;

        for (y = 0; y < draw_h; ++y) {
            ULONG pos = qrx_rle_line_offset[y];
            ULONG end = qrx_rle_line_offset[y + 1];
            WORD x = 0;

            while (pos + 1 < end && x < draw_w) {
                UBYTE run = qrx_rle_data[pos++];
                UBYTE pen = qrx_rle_data[pos++];
                WORD clipped = run;

                if (x + clipped > draw_w)
                    clipped = draw_w - x;

                if (clipped > 0) {
                    SetAPen(dst, qrx_pen(pen));
                    RectFill(dst, dx + x, dy + y,
                             dx + x + clipped - 1, dy + y);
                }
                x += run;
            }
        }
        if (dst != rp) {
            BltBitMapRastPort(g_wall_cache, 0, 0, rp, xoff, yoff, draw_w, draw_h, 0xC0);
            g_wall_blits++;
        }
    }
}

static void draw_look_switcher(struct RastPort *rp) {
    static const char *labels[4] = {"QRX", "WIN", "MAC", "AMI"};
    int i;
    WORD x = 392;

    for (i = 0; i < 4; ++i) {
        enum LookFeel item = (enum LookFeel)i;
        UBYTE bg = (item == g_look) ? PEN_ACCENT : PEN_BUTTON;
        UBYTE fg = (item == g_look && g_look != LOOK_MACOS) ? PEN_BLACK : PEN_BAR_TEXT;
        qrx_flat_button(rp, x, 4, x + 54, 18, bg, item == g_look);
        text_at(rp, fg, x + 13, 15, labels[i]);
        x += 58;
    }
}

static void draw_qrx_desktop(struct RastPort *rp) {
    WORD dock_x = (g_screen_width - 288) / 2;
    WORD dock_y = g_screen_height - 38;

    if (dock_x < 8) dock_x = 8;

    fill(rp, PEN_BAR, 0, 0, g_screen_width - 1, 25);
    outline(rp, PEN_DARK, 0, 0, g_screen_width - 1, 25);
    text_at(rp, PEN_ACCENT, 10, 17, "QRX");
    text_at(rp, PEN_BAR_TEXT, 39, 17, "OS ONE");
    text_at(rp, PEN_MUTED, 103, 17, "Amiga 68K");
    draw_look_switcher(rp);

    fill(rp, PEN_PANEL, dock_x, dock_y, dock_x + 287, dock_y + 32);
    qrx_flat_frame(rp, dock_x, dock_y, dock_x + 287, dock_y + 32);
    qrx_flat_button(rp, dock_x + 16, dock_y + 8, dock_x + 62, dock_y + 24, PEN_ACCENT, 1);
    text_at(rp, PEN_BLACK, dock_x + 23, dock_y + 20, "TERM");
    qrx_flat_button(rp, dock_x + 72, dock_y + 8, dock_x + 118, dock_y + 24, PEN_BUTTON, 0);
    text_at(rp, PEN_TEXT, dock_x + 80, dock_y + 20, "FILES");
    qrx_flat_button(rp, dock_x + 128, dock_y + 8, dock_x + 174, dock_y + 24, PEN_BUTTON, 0);
    text_at(rp, PEN_TEXT, dock_x + 142, dock_y + 20, "AI");
    qrx_flat_button(rp, dock_x + 184, dock_y + 8, dock_x + 270, dock_y + 24, PEN_BUTTON, 0);
    text_at(rp, PEN_TEXT, dock_x + 194, dock_y + 20, "SETTINGS");
}

static void draw_windows_desktop(struct RastPort *rp) {
    WORD y = g_screen_height - 28;

    fill(rp, PEN_PANEL, 0, y, g_screen_width - 1, g_screen_height - 1);
    bevel(rp, 0, y, g_screen_width - 1, g_screen_height - 1, 1);
    fill(rp, PEN_BUTTON, 4, y + 4, 72, y + 23);
    bevel(rp, 4, y + 4, 72, y + 23, 1);
    text_at(rp, PEN_BLACK, 12, y + 18, "START");
    fill(rp, PEN_BAR, 80, y + 4, 242, y + 23);
    text_at(rp, PEN_WHITE, 90, y + 18, "QRX Terminal");
    fill(rp, PEN_BUTTON, 248, y + 4, 324, y + 23);
    bevel(rp, 248, y + 4, 324, y + 23, 1);
    text_at(rp, PEN_BLACK, 260, y + 18, "FILES");
    text_at(rp, PEN_BLACK, g_screen_width - 86, y + 18,
            g_display_is_rtg ? "RTG 68K" : "AGA 68K");
    draw_look_switcher(rp);
    text_at(rp, PEN_WHITE, 10, 17, "QRX OS One");
}

static void draw_macos_desktop(struct RastPort *rp) {
    WORD dock_x = (g_screen_width - 274) / 2;
    WORD dock_y = g_screen_height - 41;
    if (dock_x < 8) dock_x = 8;

    fill(rp, PEN_BAR, 0, 0, g_screen_width - 1, 21);
    text_at(rp, PEN_BAR_TEXT, 10, 15, "QRX");
    text_at(rp, PEN_BAR_TEXT, 48, 15, "File");
    text_at(rp, PEN_BAR_TEXT, 88, 15, "Window");
    text_at(rp, PEN_BAR_TEXT, 145, 15, "Help");
    draw_look_switcher(rp);

    fill(rp, PEN_PANEL, dock_x, dock_y, dock_x + 273, dock_y + 35);
    outline(rp, PEN_EDGE, dock_x, dock_y, dock_x + 273, dock_y + 35);
    fill(rp, PEN_ACCENT, dock_x + 16, dock_y + 9,
         dock_x + 62, dock_y + 26);
    text_at(rp, PEN_BLACK, dock_x + 23, dock_y + 22, "TERM");
    fill(rp, PEN_BUTTON, dock_x + 74, dock_y + 9,
         dock_x + 120, dock_y + 26);
    text_at(rp, PEN_BAR_TEXT, dock_x + 81, dock_y + 22, "FILES");
    fill(rp, PEN_BUTTON, dock_x + 132, dock_y + 9,
         dock_x + 178, dock_y + 26);
    text_at(rp, PEN_BAR_TEXT, dock_x + 146, dock_y + 22, "AI");
    fill(rp, PEN_BUTTON, dock_x + 190, dock_y + 9,
         dock_x + 256, dock_y + 26);
    text_at(rp, PEN_BAR_TEXT, dock_x + 199, dock_y + 22, "QRX");
}

static void draw_amiga_desktop(struct RastPort *rp) {
    WORD y = g_screen_height - 42;

    fill(rp, PEN_AMIGA, 0, 0, g_screen_width - 1, 21);
    fill(rp, PEN_AMIGA_HI, 2, 2, 18, 18);
    bevel(rp, 2, 2, 18, 18, 1);
    text_at(rp, PEN_WHITE, 27, 15, "QRX Workbench 68K");
    text_at(rp, PEN_WHITE, g_screen_width - 96, 15,
            g_display_is_rtg ? "RTG" : "AGA");
    draw_look_switcher(rp);

    fill(rp, PEN_PANEL, 8, y, 176, y + 36);
    bevel(rp, 8, y, 176, y + 36, 1);
    text_at(rp, PEN_BLACK, 18, y + 14, "QRX:");
    text_at(rp, PEN_BLACK, 18, y + 28, "Terminal");
    fill(rp, PEN_PANEL, 184, y, 328, y + 36);
    bevel(rp, 184, y, 328, y + 36, 1);
    text_at(rp, PEN_BLACK, 194, y + 14, "QRX:");
    text_at(rp, PEN_BLACK, 194, y + 28, "Files");
}

static LONG launch_native(const char *command);
static int g_win_start_open=0;
static void qrx_draw_win_start_menu(struct RastPort *rp){WORD y=g_screen_height-28;if(g_look!=LOOK_WINDOWS||!g_win_start_open)return;fill(rp,PEN_PANEL,4,y-174,164,y-2);outline(rp,PEN_EDGE,4,y-174,164,y-2);text_at(rp,PEN_TEXT,16,y-150,"WRITER");text_at(rp,PEN_TEXT,16,y-124,"SPREADSHEET");text_at(rp,PEN_TEXT,16,y-98,"PRESENTER");text_at(rp,PEN_TEXT,16,y-72,"SETTINGS");text_at(rp,PEN_TEXT,16,y-46,"QRXAI");text_at(rp,PEN_TEXT,16,y-20,"WELCOME");}
static int qrx_windows_start_click(WORD x,WORD y){WORD ty=g_screen_height-28;if(g_look!=LOOK_WINDOWS)return 0;if(x>=4&&x<=72&&y>=ty+4&&y<=ty+23){g_win_start_open=!g_win_start_open;qrx_draw_win_start_menu(g_desktop->RPort);return 1;}if(g_win_start_open&&x>=4&&x<=164&&y>=ty-174&&y<ty){WORD r=(y-(ty-174))/26;if(r==0)launch_native("QRX:C/QRXWriter");else if(r==1)launch_native("QRX:C/QRXSpreadsheet");else if(r==2)launch_native("QRX:C/QRXPresenter");else if(r==3)launch_native("QRX:C/QRXSettings");else if(r==4)launch_native("QRX:C/QRXAI");else launch_native("QRX:C/QRXWelcome");g_win_start_open=0;return 1;}if(x>=80&&x<=242&&y>=ty+4&&y<=ty+23){WindowToFront(g_terminal);ActivateWindow(g_terminal);return 1;}return 0;}
static void qrx_draw_datetime(struct RastPort *rp) {
    struct DateStamp ds; struct DateTime dt; char d[LEN_DATSTRING],t[LEN_DATSTRING],out[64]; WORD x;
    DateStamp(&ds); memset(&dt,0,sizeof(dt)); dt.dat_Stamp=ds; dt.dat_Format=FORMAT_DOS;
    dt.dat_StrDate=(STRPTR)d; dt.dat_StrTime=(STRPTR)t;
    if(DateToStr(&dt)){snprintf(out,sizeof(out),"%.5s %.5s",d,t);x=(g_screen_width-(WORD)(strlen(out)*8))/2;if(x<260)x=260;if(x+(WORD)(strlen(out)*8)>382)x=382-(WORD)(strlen(out)*8);text_at(rp,PEN_BAR_TEXT,x,15,out);}
}
static void draw_productivity_launcher(struct RastPort *rp) {
    WORD x=g_screen_width-112,y=58;if(x<520)x=520;
    fill(rp,PEN_PANEL,x,y,g_screen_width-7,y+268);outline(rp,PEN_EDGE,x,y,g_screen_width-7,y+268);text_at(rp,PEN_MUTED,x+10,y+17,"QRX APPS");
#define ROW(yy,label) do{fill(rp,PEN_BUTTON,x+7,y+(yy),g_screen_width-14,y+(yy)+22);outline(rp,PEN_EDGE,x+7,y+(yy),g_screen_width-14,y+(yy)+22);text_at(rp,PEN_TEXT,x+16,y+(yy)+16,label);}while(0)
    ROW(26,"WRITER");ROW(52,"SHEET");ROW(78,"PRESENTER");ROW(104,"HARDWARE");ROW(130,"SETTINGS");ROW(156,"WELCOME");ROW(182,"WEB");ROW(208,"ARES AV");ROW(234,"FILES");
#undef ROW
}
static void draw_resize_handle(struct RastPort *rp, WORD w, WORD h);
static void qrx_draw_ai_float(struct RastPort *rp);
static void redraw_desktop(void);
static void redraw_filemanager(void);
static LONG launch_native(const char *command);
static LONG execute_capture(const char *command);
static int open_filemanager(const char *path);
static int has_foreign_windows(void);
static int qrx_schedule_takeover(const char *command);
static int qrx_schedule_whdload(const char *slave, const char *data_dir,
                                const char *options);

static int qrx_ci_char(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int qrx_ci_cmp(const char *a, const char *b) {
    int ca, cb;
    if (!a) a = "";
    if (!b) b = "";
    while (*a && *b) {
        ca = qrx_ci_char((unsigned char)*a++);
        cb = qrx_ci_char((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return qrx_ci_char((unsigned char)*a) - qrx_ci_char((unsigned char)*b);
}

static void qrx_disable_dos_requesters(void) {
    if (g_qrx_requesters_disabled) return;
    g_qrx_process = (struct Process *)FindTask(NULL);
    if (!g_qrx_process) return;
    g_qrx_saved_window_ptr = g_qrx_process->pr_WindowPtr;
    g_qrx_process->pr_WindowPtr = (APTR)-1;
    g_qrx_requesters_disabled = 1;
}

static void qrx_restore_dos_requesters(void) {
    if (!g_qrx_requesters_disabled || !g_qrx_process) return;
    g_qrx_process->pr_WindowPtr = g_qrx_saved_window_ptr;
    g_qrx_requesters_disabled = 0;
    g_qrx_process = NULL;
    g_qrx_saved_window_ptr = NULL;
}

static void qrx_bstr_to_name(BSTR bstr, char *out, size_t out_size) {
    UBYTE *src;
    ULONG len;
    if (!out || out_size == 0) return;
    out[0] = 0;
    if (!bstr) return;
    src = (UBYTE *)BADDR(bstr);
    if (!src) return;
    len = src[0];
    if (len >= out_size) len = (ULONG)out_size - 1;
    if (len) CopyMem(src + 1, out, len);
    out[len] = 0;
}

static int qrx_volume_name_is_builtin(const char *name) {
    if (!name || !*name) return 1;
    if (!qrx_ci_cmp(name, "RAM Disk")) return 1;
    if (!qrx_ci_cmp(name, "AROS Live CD")) return 1;
    if (!qrx_ci_cmp(name, "AROS Kickstart")) return 1;
    return 0;
}

static int qrx_volume_name_seen(const char *name) {
    int i;
    for (i = 0; i < g_mounted_volume_count; ++i)
        if (!qrx_ci_cmp(name, g_mounted_volumes[i].name)) return 1;
    return 0;
}

static void qrx_refresh_mounted_volumes(void) {
    const ULONG flags = LDF_READ | LDF_VOLUMES;
    struct DosList *cursor;
    struct DosList *entry;

    g_mounted_volume_count = 0;
    cursor = LockDosList(flags);
    if (!cursor) return;

    entry = cursor;
    while ((entry = NextDosEntry(entry, flags)) != NULL &&
           g_mounted_volume_count < QRX_MOUNTED_VOLUME_MAX) {
        char name[QRX_MOUNTED_VOLUME_NAME];
        qrx_bstr_to_name(entry->dol_Name, name, sizeof(name));
        if (qrx_volume_name_is_builtin(name) || qrx_volume_name_seen(name))
            continue;
        strncpy(g_mounted_volumes[g_mounted_volume_count].name,
                name, sizeof(g_mounted_volumes[g_mounted_volume_count].name) - 1);
        g_mounted_volumes[g_mounted_volume_count].name[
            sizeof(g_mounted_volumes[g_mounted_volume_count].name) - 1] = 0;
        snprintf(g_mounted_volumes[g_mounted_volume_count].path,
                 sizeof(g_mounted_volumes[g_mounted_volume_count].path),
                 "%s:", name);
        ++g_mounted_volume_count;
    }
    UnLockDosList(flags);
}

static int qrx_drive_path_for_row(int row, char *dest, size_t dest_size) {
    static const char *base[] = {"SYS:", "QRX:", "RAM:", "T:"};
    if (!dest || dest_size == 0 || row < 0) return 0;
    if (row < 4) {
        snprintf(dest, dest_size, "%s", base[row]);
        return 1;
    }
    row -= 4;
    if (row < 0 || row >= g_mounted_volume_count) return 0;
    snprintf(dest, dest_size, "%s", g_mounted_volumes[row].path);
    return 1;
}

static int qrx_drive_row_from_y(WORD y) {
    int delta, row;
    if (y < 116) return -1;
    delta = y - 116;
    row = delta / 26;
    if ((delta % 26) > 22) return -1;
    if (row >= 4 + g_mounted_volume_count) return -1;
    return row;
}

static int qrx_ends_info(const char *name) {
    size_t n;
    if (!name) return 0;
    n = strlen(name);
    if (n < 5) return 0;
    return qrx_ci_cmp(name + n - 5, ".info") == 0;
}

static void qrx_join_path(char *out, size_t out_size, const char *base_path, const char *name) {
    size_t n;
    if (!out || out_size == 0) return;
    if (!base_path) base_path = "";
    if (!name) name = "";

    n = strlen(base_path);
    if (n == 0) {
        snprintf(out, out_size, "%s", name);
    } else if (base_path[n - 1] == ':' || base_path[n - 1] == '/') {
        snprintf(out, out_size, "%s%s", base_path, name);
    } else {
        snprintf(out, out_size, "%s/%s", base_path, name);
    }
}

static const char *qrx_icon_type_name(UBYTE type) {
    switch (type) {
        case WBDISK: return "Disk";
        case WBDRAWER: return "Drawer";
        case WBTOOL: return "Tool";
        case WBPROJECT: return "Project";
        case WBGARBAGE: return "Trash";
        default: return "Icon";
    }
}

static void qrx_sort_entries(void) {
    int i, j;
    struct QRXFileEntry tmp;

    for (i = 0; i < g_file_count - 1; ++i) {
        for (j = i + 1; j < g_file_count; ++j) {
            int swap = 0;
            if ((g_file_entries[i].dir_type > 0) != (g_file_entries[j].dir_type > 0)) {
                if (g_file_entries[j].dir_type > 0) swap = 1;
            } else if (qrx_ci_cmp(g_file_entries[i].name, g_file_entries[j].name) > 0) {
                swap = 1;
            }
            if (swap) {
                tmp = g_file_entries[i];
                g_file_entries[i] = g_file_entries[j];
                g_file_entries[j] = tmp;
            }
        }
    }
}

static int qrx_load_directory(const char *path) {
    BPTR lock = 0;
    struct FileInfoBlock *fib = NULL;
    char full[FILE_PATH_LEN + 128];
    struct DiskObject *dobj;

    if (!path || !*path) path = "SYS:";

    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock) {
        add_line("QRX Drive: cannot lock requested path.");
        return 0;
    }

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        UnLock(lock);
        add_line("QRX Drive: cannot allocate FileInfoBlock.");
        return 0;
    }

    if (!Examine(lock, fib)) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        add_line("QRX Drive: Examine() failed.");
        return 0;
    }

    strncpy(g_files_path, path, FILE_PATH_LEN);
    g_files_path[FILE_PATH_LEN] = 0;
    g_file_count = 0;
    g_file_selected = -1;
    g_file_scroll = 0;

    while (g_file_count < FILE_MAX_ENTRIES && ExNext(lock, fib)) {
        struct QRXFileEntry *e;

        if (qrx_ends_info(fib->fib_FileName))
            continue;

        e = &g_file_entries[g_file_count];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, fib->fib_FileName, sizeof(e->name) - 1);
        e->dir_type = fib->fib_DirEntryType;
        e->size = fib->fib_Size;
        e->protection = fib->fib_Protection;

        if (IconBase) {
            qrx_join_path(full, sizeof(full), g_files_path, e->name);
            dobj = GetDiskObject((STRPTR)full);
            if (dobj) {
                e->has_icon = 1;
            } else {
                dobj = GetDiskObjectNew((STRPTR)full);
                if (dobj) e->generated_icon = 1;
            }
            if (dobj) {
                e->icon_type = dobj->do_Type;
                if (dobj->do_DefaultTool) {
                    strncpy(e->default_tool, dobj->do_DefaultTool,
                            sizeof(e->default_tool) - 1);
                }
                FreeDiskObject(dobj);
            }
        }

        ++g_file_count;
    }

    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    qrx_sort_entries();
    save_preferences();
    return 1;
}

static int qrx_parent_directory(void) {
    BPTR lock = 0, parent = 0;
    char path[FILE_PATH_LEN + 1];

    lock = Lock((STRPTR)g_files_path, ACCESS_READ);
    if (!lock) return 0;

    parent = ParentDir(lock);
    UnLock(lock);
    if (!parent) return 0;

    path[0] = 0;
    if (!NameFromLock(parent, (STRPTR)path, sizeof(path))) {
        UnLock(parent);
        return 0;
    }
    UnLock(parent);

    return qrx_load_directory(path);
}


static int qrx_path_exists(const char *path) {
    BPTR lock;
    if (!path || !*path) return 0;
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock) return 0;
    UnLock(lock);
    return 1;
}

static int qrx_path_prefix(const char *parent, const char *child) {
    size_t n, i;
    if (!parent || !child) return 0;
    n = strlen(parent);
    if (!n) return 0;
    for (i = 0; i < n; ++i) {
        if (!child[i] ||
            qrx_ci_char((unsigned char)parent[i]) !=
            qrx_ci_char((unsigned char)child[i]))
            return 0;
    }
    return child[n] == 0 || child[n] == '/' || child[n] == ':';
}

static void qrx_set_status(const char *msg) {
    strncpy(g_file_status, msg ? msg : "", sizeof(g_file_status) - 1);
    g_file_status[sizeof(g_file_status) - 1] = 0;
}

static void qrx_scan_desktop(void) {
    BPTR lock;
    struct FileInfoBlock *fib;

    g_desktop_count = 0;
    lock = Lock((STRPTR)DESKTOP_SCAN_PATH, ACCESS_READ);
    if (!lock) return;

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) { UnLock(lock); return; }
    if (!Examine(lock, fib)) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        return;
    }

    while (g_desktop_count < DESKTOP_MAX && ExNext(lock, fib)) {
        struct QRXDesktopEntry *e;
        struct DiskObject *dobj = NULL;

        if (qrx_ends_info(fib->fib_FileName)) continue;

        e = &g_desktop_entries[g_desktop_count];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, fib->fib_FileName, sizeof(e->name) - 1);
        e->dir_type = fib->fib_DirEntryType;
        qrx_join_path(e->full_path, sizeof(e->full_path),
                      DESKTOP_SCAN_PATH, e->name);

        if (IconBase) {
            dobj = GetDiskObject((STRPTR)e->full_path);
            if (dobj) e->has_icon = 1;
            else {
                dobj = GetDiskObjectNew((STRPTR)e->full_path);
                if (dobj) e->generated_icon = 1;
            }
            if (dobj) {
                e->icon_type = dobj->do_Type;
                if (dobj->do_DefaultTool) {
                    strncpy(e->default_tool, dobj->do_DefaultTool,
                            sizeof(e->default_tool) - 1);
                }
                FreeDiskObject(dobj);
            }
        }
        ++g_desktop_count;
    }

    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
}


static int qrx_ci_ncmp(const char *a, const char *b, size_t n) {
    size_t i;
    int ca, cb;
    if (!a) a = "";
    if (!b) b = "";
    for (i = 0; i < n; ++i) {
        ca = qrx_ci_char((unsigned char)a[i]);
        cb = qrx_ci_char((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (!a[i] || !b[i]) return ca - cb;
    }
    return 0;
}

static int qrx_ci_contains(const char *text, const char *needle) {
    size_t n, i;
    if (!text || !needle || !*needle) return 0;
    n = strlen(needle);
    for (i = 0; text[i]; ++i) {
        if (!qrx_ci_ncmp(text + i, needle, n))
            return 1;
    }
    return 0;
}

static int qrx_ci_ends(const char *text, const char *suffix) {
    size_t lt, ls;
    if (!text || !suffix) return 0;
    lt = strlen(text);
    ls = strlen(suffix);
    if (lt < ls) return 0;
    return qrx_ci_cmp(text + lt - ls, suffix) == 0;
}

static const char *qrx_tooltype_value(char **tooltypes, const char *key) {
    size_t key_len;
    int i;
    if (!tooltypes || !key) return NULL;
    key_len = strlen(key);

    for (i = 0; tooltypes[i]; ++i) {
        const char *t = tooltypes[i];
        if (!qrx_ci_ncmp(t, key, key_len) && t[key_len] == '=')
            return t + key_len + 1;
    }
    return NULL;
}

static void qrx_parent_path(const char *full, char *out, size_t out_size) {
    const char *slash = NULL;
    const char *colon = NULL;
    const char *p;
    size_t len;

    if (!out || out_size == 0) return;
    out[0] = 0;
    if (!full || !*full) return;

    for (p = full; *p; ++p) {
        if (*p == '/') slash = p;
        if (*p == ':') colon = p;
    }

    if (slash) {
        len = (size_t)(slash - full);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, full, len);
        out[len] = 0;
        return;
    }

    if (colon) {
        len = (size_t)(colon - full) + 1;
        if (len >= out_size) len = out_size - 1;
        memcpy(out, full, len);
        out[len] = 0;
    }
}

static void qrx_resolve_relative(const char *object_path, const char *value,
                                 char *out, size_t out_size) {
    char parent[LEGACY_PATH_LEN];
    if (!out || out_size == 0) return;
    out[0] = 0;
    if (!value || !*value) return;

    if (strchr(value, ':') || value[0] == '/') {
        snprintf(out, out_size, "%s", value);
        return;
    }

    qrx_parent_path(object_path, parent, sizeof(parent));
    qrx_join_path(out, out_size, parent, value);
}

static void qrx_detect_launch_profile(const char *full,
                                      const char *default_tool,
                                      struct QRXLaunchProfile *profile) {
    struct DiskObject *dobj = NULL;
    const char *mode = NULL;
    const char *slave = NULL;
    const char *opts = NULL;
    const char *args = NULL;

    if (!profile) return;
    memset(profile, 0, sizeof(*profile));
    profile->mode = RUNMODE_AUTO;

    if (full && qrx_ci_ends(full, ".slave")) {
        profile->mode = RUNMODE_WHDLOAD;
        snprintf(profile->slave, sizeof(profile->slave), "%s", full);
    }

    if (default_tool && qrx_ci_contains(default_tool, "whdload"))
        profile->mode = RUNMODE_WHDLOAD;

    if (!IconBase || !full) {
        if (profile->mode == RUNMODE_AUTO)
            profile->mode = RUNMODE_NORMAL;
        return;
    }

    dobj = GetDiskObject((STRPTR)full);
    if (!dobj) {
        if (profile->mode == RUNMODE_AUTO)
            profile->mode = RUNMODE_NORMAL;
        return;
    }

    mode = qrx_tooltype_value(dobj->do_ToolTypes, "QRXMODE");
    slave = qrx_tooltype_value(dobj->do_ToolTypes, "QRXSLAVE");
    if (!slave)
        slave = qrx_tooltype_value(dobj->do_ToolTypes, "SLAVE");
    opts = qrx_tooltype_value(dobj->do_ToolTypes, "QRXWHDOPTS");
    args = qrx_tooltype_value(dobj->do_ToolTypes, "QRXARGS");

    if (mode) {
        if (!qrx_ci_cmp(mode, "NORMAL"))
            profile->mode = RUNMODE_NORMAL;
        else if (!qrx_ci_cmp(mode, "TAKEOVER"))
            profile->mode = RUNMODE_TAKEOVER;
        else if (!qrx_ci_cmp(mode, "WHDLOAD") ||
                 !qrx_ci_cmp(mode, "LEGACY"))
            profile->mode = RUNMODE_WHDLOAD;
    }

    if (slave) {
        qrx_resolve_relative(full, slave,
                             profile->slave, sizeof(profile->slave));
        if (profile->mode == RUNMODE_AUTO)
            profile->mode = RUNMODE_WHDLOAD;
    }

    if (opts)
        snprintf(profile->options, sizeof(profile->options), "%s", opts);
    if (args)
        snprintf(profile->args, sizeof(profile->args), "%s", args);

    FreeDiskObject(dobj);

    if (profile->mode == RUNMODE_AUTO)
        profile->mode = RUNMODE_NORMAL;
}

static const char *qrx_runmode_name(UBYTE mode) {
    switch (mode) {
        case RUNMODE_TAKEOVER: return "TAKEOVER";
        case RUNMODE_WHDLOAD: return "WHDLOAD";
        case RUNMODE_NORMAL: return "NORMAL";
        default: return "AUTO";
    }
}

static int qrx_elf_machine(const char *path) {
    BPTR f;
    UBYTE h[20];
    LONG n;
    UWORD machine;
    if (!path || !*path) return 0;
    f = Open((STRPTR)path, MODE_OLDFILE);
    if (!f) return 0;
    n = Read(f, h, sizeof(h));
    Close(f);
    if (n < 20 || h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F')
        return 0;
    if (h[4] != 1) return 0;
    if (h[5] == 1)
        machine = (UWORD)h[18] | ((UWORD)h[19] << 8);
    else if (h[5] == 2)
        machine = ((UWORD)h[18] << 8) | (UWORD)h[19];
    else return 0;
    if (machine == 3 && h[5] == 1) return 386;
    if (machine == 4 && h[5] == 2) return 68;
    return -1;
}

static int qrx_is_pe32_i386(const char *path) {
    BPTR f;
    UBYTE mz[64], pe[6];
    LONG n;
    ULONG off;
    if (!path || !*path) return 0;
    f=Open((STRPTR)path,MODE_OLDFILE); if(!f) return 0;
    n=Read(f,mz,sizeof(mz));
    if(n<64 || mz[0]!='M' || mz[1]!='Z'){Close(f);return 0;}
    off=(ULONG)mz[0x3c] | ((ULONG)mz[0x3d]<<8) |
        ((ULONG)mz[0x3e]<<16) | ((ULONG)mz[0x3f]<<24);
    if(Seek(f,(LONG)off,OFFSET_BEGINNING)<0){Close(f);return 0;}
    n=Read(f,pe,sizeof(pe));Close(f);
    return n==6 && pe[0]=='P'&&pe[1]=='E'&&pe[2]==0&&pe[3]==0 &&
           pe[4]==0x4c && pe[5]==0x01;
}

static LONG qrx_open_path(const char *full, LONG dir_type, UBYTE has_icon,
                          UBYTE icon_type, const char *default_tool) {
    char command[LEGACY_CMD_LEN];
    char data_dir[LEGACY_PATH_LEN];
    struct QRXLaunchProfile profile;

    if (!full || !*full) return 0;
    if (dir_type > 0) return open_filemanager(full);

    /* Windows PE32/i386 dispatches through QRX Win32/Wine compatibility. */
    if (qrx_is_pe32_i386(full)) {
        snprintf(command, sizeof(command), "QRX:C/QRXWinRun \"%s\"", full);
        execute_capture(command);
        return 1;
    }

    /* ELF dispatch: native m68k direct ABI or interpreted i386 QRX86. */
    {
        int em = qrx_elf_machine(full);
        if (em == 68) {
            snprintf(command, sizeof(command), "QRX:C/QRXLinuxRun \"%s\"", full);
            execute_capture(command);
            return 1;
        }
        if (em == 386) {
            snprintf(command, sizeof(command), "QRX:C/QRX86Run \"%s\"", full);
            execute_capture(command);
            return 1;
        }
    }

    /*
     * QRX productivity document associations.  A physical WBPROJECT
     * DefaultTool still wins; otherwise the versioned QRX extensions are
     * portable application contracts across QRX platforms.
     */
    if (!(has_icon && icon_type == WBPROJECT &&
          default_tool && *default_tool)) {
        if (qrx_ci_ends(full, ".qrxdoc") || qrx_ci_ends(full, ".txt")) {
            snprintf(command, sizeof(command),
                     "QRX:C/QRXWriter \"%s\"", full);
            return launch_native(command);
        }
        if (qrx_ci_ends(full, ".qrxsheet") || qrx_ci_ends(full, ".csv")) {
            snprintf(command, sizeof(command),
                     "QRX:C/QRXSpreadsheet \"%s\"", full);
            return launch_native(command);
        }
        if (qrx_ci_ends(full, ".qrxdeck")) {
            snprintf(command, sizeof(command),
                     "QRX:C/QRXPresenter \"%s\"", full);
            return launch_native(command);
        }
    }

    qrx_detect_launch_profile(full, default_tool, &profile);
    qrx_parent_path(full, data_dir, sizeof(data_dir));

    if (profile.mode == RUNMODE_WHDLOAD) {
        if (!profile.slave[0]) {
            if (qrx_ci_ends(full, ".slave"))
                snprintf(profile.slave, sizeof(profile.slave), "%s", full);
            else {
                add_line("WHDLoad profile needs SLAVE= or QRXSLAVE= ToolType.");
                return 0;
            }
        }
        return qrx_schedule_whdload(profile.slave, data_dir,
                                    profile.options);
    }

    if (has_icon && icon_type == WBPROJECT &&
        default_tool && *default_tool) {
        snprintf(command, sizeof(command), "%s \"%s\" %s",
                 default_tool, full, profile.args);
    } else {
        snprintf(command, sizeof(command), "\"%s\" %s",
                 full, profile.args);
    }

    if (profile.mode == RUNMODE_TAKEOVER)
        return qrx_schedule_takeover(command);

    return launch_native(command);
}

static int qrx_copy_path(const char *source, const char *dest_full,
                         int is_dir, int has_icon) {
    char command[700];
    char source_info[FILE_PATH_LEN + 160];
    char dest_info[FILE_PATH_LEN + 160];

    snprintf(command, sizeof(command),
             "Copy \"%s\" TO \"%s\" %s QUIET",
             source, dest_full, is_dir ? "ALL" : "");
    if (!execute_capture(command)) return 0;

    if (has_icon) {
        snprintf(source_info, sizeof(source_info), "%s.info", source);
        snprintf(dest_info, sizeof(dest_info), "%s.info", dest_full);
        if (qrx_path_exists(source_info)) {
            snprintf(command, sizeof(command),
                     "Copy \"%s\" TO \"%s\" QUIET", source_info, dest_info);
            execute_capture(command);
        }
    }
    return 1;
}

static int qrx_delete_path(const char *full, int is_dir, int has_icon) {
    char command[700];

    if (is_dir) {
        snprintf(command, sizeof(command),
                 "Delete \"%s\" ALL QUIET", full);
        if (!execute_capture(command)) return 0;
    } else if (!DeleteFile((STRPTR)full)) {
        return 0;
    }

    if (has_icon && IconBase)
        DeleteDiskObject((UBYTE *)full);
    return 1;
}

static int qrx_rename_path(const char *old_full, const char *new_full,
                           int has_icon) {
    char old_info[FILE_PATH_LEN + 160];
    char new_info[FILE_PATH_LEN + 160];

    if (!Rename((STRPTR)old_full, (STRPTR)new_full)) return 0;

    if (has_icon) {
        snprintf(old_info, sizeof(old_info), "%s.info", old_full);
        snprintf(new_info, sizeof(new_info), "%s.info", new_full);
        if (qrx_path_exists(old_info))
            Rename((STRPTR)old_info, (STRPTR)new_info);
    }
    return 1;
}

static int qrx_clipboard_set(int mode, int index) {
    struct QRXFileEntry *e;
    if (index < 0 || index >= g_file_count) {
        qrx_set_status("Select an object first.");
        return 0;
    }

    e = &g_file_entries[index];
    qrx_join_path(g_clip_source, sizeof(g_clip_source),
                  g_files_path, e->name);
    strncpy(g_clip_name, e->name, sizeof(g_clip_name) - 1);
    g_clip_name[sizeof(g_clip_name) - 1] = 0;
    g_clip_is_dir = e->dir_type > 0;
    g_clip_has_icon = e->has_icon;
    g_clip_mode = mode;
    g_delete_armed = 0;
    qrx_set_status(mode == CLIP_MOVE
                   ? "Cut stored. Open destination and press PASTE."
                   : "Copy stored. Open destination and press PASTE.");
    return 1;
}

static int qrx_paste_clipboard(const char *dest_dir) {
    char dest_full[FILE_PATH_LEN + 256];

    if (g_clip_mode == CLIP_NONE || !g_clip_source[0]) {
        qrx_set_status("Clipboard is empty.");
        return 0;
    }

    if (g_clip_is_dir && qrx_path_prefix(g_clip_source, dest_dir)) {
        qrx_set_status("Cannot copy/move a drawer into itself.");
        return 0;
    }

    qrx_join_path(dest_full, sizeof(dest_full), dest_dir, g_clip_name);
    if (!qrx_ci_cmp(dest_full, g_clip_source)) {
        qrx_set_status("Source and destination are identical.");
        return 0;
    }
    if (qrx_path_exists(dest_full)) {
        qrx_set_status("Destination already contains that name.");
        return 0;
    }

    if (g_clip_mode == CLIP_MOVE) {
        if (!qrx_rename_path(g_clip_source, dest_full, g_clip_has_icon)) {
            if (!qrx_copy_path(g_clip_source, dest_full,
                               g_clip_is_dir, g_clip_has_icon)) {
                qrx_set_status("Move failed.");
                return 0;
            }
            if (!qrx_delete_path(g_clip_source,
                                 g_clip_is_dir, g_clip_has_icon)) {
                qrx_set_status("Copied, but source cleanup failed.");
                return 0;
            }
        }
        g_clip_mode = CLIP_NONE;
        g_clip_source[0] = 0;
        qrx_set_status("Move completed.");
    } else {
        if (!qrx_copy_path(g_clip_source, dest_full,
                           g_clip_is_dir, g_clip_has_icon)) {
            qrx_set_status("Copy failed.");
            return 0;
        }
        qrx_set_status("Copy completed.");
    }

    qrx_load_directory(g_files_path);
    qrx_scan_desktop();
    redraw_desktop();
    return 1;
}

static int qrx_move_index_to(int index, const char *dest_dir) {
    if (!qrx_clipboard_set(CLIP_MOVE, index)) return 0;
    return qrx_paste_clipboard(dest_dir);
}

static void qrx_begin_rename(void) {
    struct QRXFileEntry *e;
    if (g_file_selected < 0 || g_file_selected >= g_file_count) {
        qrx_set_status("Select an object before Rename.");
        return;
    }

    e = &g_file_entries[g_file_selected];
    strncpy(g_rename_text, e->name, sizeof(g_rename_text) - 1);
    g_rename_text[sizeof(g_rename_text) - 1] = 0;
    g_rename_len = strlen(g_rename_text);
    g_rename_active = 1;
    g_delete_armed = 0;
    qrx_set_status("Rename: type name, Enter commits, Esc cancels.");
}

static void qrx_commit_rename(void) {
    struct QRXFileEntry *e;
    char old_full[FILE_PATH_LEN + 160];
    char new_full[FILE_PATH_LEN + 160];

    if (!g_rename_active ||
        g_file_selected < 0 || g_file_selected >= g_file_count)
        return;

    if (!g_rename_text[0] ||
        strchr(g_rename_text, '/') || strchr(g_rename_text, ':')) {
        qrx_set_status("Invalid Amiga filename.");
        return;
    }

    e = &g_file_entries[g_file_selected];
    qrx_join_path(old_full, sizeof(old_full), g_files_path, e->name);
    qrx_join_path(new_full, sizeof(new_full), g_files_path, g_rename_text);

    if (qrx_path_exists(new_full)) {
        qrx_set_status("New name already exists.");
        return;
    }

    if (!qrx_rename_path(old_full, new_full, e->has_icon)) {
        qrx_set_status("Rename failed.");
        return;
    }

    g_rename_active = 0;
    qrx_load_directory(g_files_path);
    qrx_scan_desktop();
    qrx_set_status("Rename completed.");
    redraw_desktop();
}

static void qrx_delete_selected(void) {
    struct QRXFileEntry *e;
    char full[FILE_PATH_LEN + 160];

    if (g_file_selected < 0 || g_file_selected >= g_file_count) {
        qrx_set_status("Select an object before Delete.");
        g_delete_armed = 0;
        return;
    }

    if (!g_delete_armed) {
        g_delete_armed = 1;
        qrx_set_status("Delete armed. Press DEL again to confirm.");
        return;
    }

    e = &g_file_entries[g_file_selected];
    qrx_join_path(full, sizeof(full), g_files_path, e->name);
    if (qrx_delete_path(full, e->dir_type > 0, e->has_icon)) {
        qrx_load_directory(g_files_path);
        qrx_scan_desktop();
        qrx_set_status("Delete completed.");
        redraw_desktop();
    } else {
        qrx_set_status("Delete failed.");
    }
    g_delete_armed = 0;
}

static int qrx_foreign_window_count(void) {
    struct Window *w;
    int count = 0;

    if (!g_screen || g_screen_borrowed) return 0;
    for (w = g_screen->FirstWindow; w; w = w->NextWindow) {
        if (w != g_desktop && w != g_terminal && w != g_files)
            ++count;
    }
    return count;
}

static struct Window *qrx_foreign_window_at(int wanted) {
    struct Window *w;
    int index = 0;

    if (!g_screen || g_screen_borrowed || wanted < 0) return NULL;

    for (w = g_screen->FirstWindow; w; w = w->NextWindow) {
        if (w == g_desktop || w == g_terminal || w == g_files)
            continue;
        if (index == wanted) return w;
        ++index;
    }
    return NULL;
}

static void qrx_focus_foreign_window(int index) {
    struct Window *w = qrx_foreign_window_at(index);
    if (!w) {
        add_line("Visitor window index not found.");
        return;
    }
    WindowToFront(w);
    ActivateWindow(w);
}

static void qrx_list_foreign_windows(void) {
    struct Window *w;
    int index = 0;
    char buf[TERM_LINE_LEN + 1];

    if (!g_screen) return;

    for (w = g_screen->FirstWindow; w; w = w->NextWindow) {
        const char *title;
        if (w == g_desktop || w == g_terminal || w == g_files)
            continue;

        title = w->Title ? (const char *)w->Title : "(untitled)";
        snprintf(buf, sizeof(buf), "%d: %.42s [%d,%d %dx%d]",
                 index, title,
                 (int)w->LeftEdge, (int)w->TopEdge,
                 (int)w->Width, (int)w->Height);
        add_line(buf);
        ++index;
    }

    if (index == 0)
        add_line("No visitor Amiga windows on QRX.");
}

static void draw_foreign_window_tabs(struct RastPort *rp) {
    struct Window *w;
    int index = 0;
    WORD x = 8;
    char label[28];

    if (!g_screen || g_screen_borrowed || !rp) return;

    for (w = g_screen->FirstWindow; w && index < 4; w = w->NextWindow) {
        const char *title;
        if (w == g_desktop || w == g_terminal || w == g_files)
            continue;

        title = w->Title ? (const char *)w->Title : "Amiga App";
        snprintf(label, sizeof(label), "%d:%.15s", index, title);
        fill(rp, PEN_BUTTON, x, 28, x + 140, 46);
        outline(rp, PEN_EDGE, x, 28, x + 140, 46);
        text_at(rp, PEN_TEXT, x + 6, 41, label);
        x += 146;
        ++index;
    }
}

static int desktop_click_foreign_window(WORD x, WORD y) {
    int count, index;
    if (y < 28 || y > 46 || x < 8) return 0;

    index = (x - 8) / 146;
    if (index < 0 || index > 3) return 0;
    count = qrx_foreign_window_count();
    if (index >= count) return 0;

    qrx_focus_foreign_window(index);
    return 1;
}

static void draw_file_icon(struct RastPort *rp, int entry_index, WORD x, WORD y) {
    struct QRXFileEntry *e;
    struct DiskObject *dobj = NULL;
    struct Image *img = NULL;
    char full[FILE_PATH_LEN + 128];
    int drawn = 0;

    if (entry_index < 0 || entry_index >= g_file_count) return;
    e = &g_file_entries[entry_index];

    if (IconBase) {
        qrx_join_path(full, sizeof(full), g_files_path, e->name);
        dobj = e->has_icon
             ? GetDiskObject((STRPTR)full)
             : GetDiskObjectNew((STRPTR)full);
        if (dobj) {
            img = (struct Image *)dobj->do_Gadget.GadgetRender;
            if (img && img->Width <= FILE_ICON_W && img->Height <= FILE_ICON_H) {
                DrawImage(rp, img, x, y);
                drawn = 1;
            }
            FreeDiskObject(dobj);
        }
    }

    if (drawn) return;

    fill(rp, e->dir_type > 0 ? PEN_ACCENT : PEN_BUTTON,
         x, y + 2, x + 24, y + 24);
    outline(rp, PEN_EDGE, x, y + 2, x + 24, y + 24);
    text_at(rp, e->dir_type > 0 ? PEN_BLACK : PEN_TEXT,
            x + 8, y + 18, e->dir_type > 0 ? "D" :
            (e->has_icon ? "I" : "F"));
}

static void draw_desktop_disk_icon(struct RastPort *rp, WORD x, WORD y,
                                   const char *label, UBYTE pen) {
    fill(rp, pen, x, y, x + 42, y + 28);
    outline(rp, PEN_EDGE, x, y, x + 42, y + 28);
    fill(rp, PEN_PANEL_2, x + 7, y + 7, x + 35, y + 19);
    text_at(rp, PEN_TEXT, x - 2, y + 43, label);
}

static void draw_desktop_dynamic_icon(struct RastPort *rp, int index,
                                      WORD x, WORD y) {
    struct QRXDesktopEntry *e;
    struct DiskObject *dobj = NULL;
    struct Image *img = NULL;
    int drawn = 0;

    if (index < 0 || index >= g_desktop_count) return;
    e = &g_desktop_entries[index];

    if (IconBase) {
        dobj = e->has_icon
             ? GetDiskObject((STRPTR)e->full_path)
             : GetDiskObjectNew((STRPTR)e->full_path);
        if (dobj) {
            img = (struct Image *)dobj->do_Gadget.GadgetRender;
            if (img && img->Width <= 50 && img->Height <= 34) {
                DrawImage(rp, img, x, y);
                drawn = 1;
            }
            FreeDiskObject(dobj);
        }
    }

    if (!drawn) {
        const char *glyph = e->dir_type > 0 ? "D" : "*";
        if (qrx_ci_cmp(e->name, "Writer") == 0) glyph = "W";
        else if (qrx_ci_cmp(e->name, "Spreadsheet") == 0) glyph = "S";
        else if (qrx_ci_cmp(e->name, "Presenter") == 0) glyph = "P";
        else if (qrx_ci_cmp(e->name, "Hardware") == 0) glyph = "H";
        fill(rp, e->dir_type > 0 ? PEN_ACCENT : PEN_BUTTON, x, y, x + 36, y + 30);
        outline(rp, PEN_EDGE, x, y, x + 36, y + 30);
        fill(rp, PEN_PANEL_2, x + 5, y + 5, x + 31, y + 25);
        outline(rp, PEN_WHITE, x + 5, y + 5, x + 31, y + 25);
        text_at(rp, PEN_TEXT, x + 14, y + 20, glyph);
    }
    text_at(rp, PEN_TEXT, x - 2, y + 46, e->name);
}

static void draw_filemanager_chrome(struct RastPort *rp, WORD w, WORD h) {
    if (g_look == LOOK_WINDOWS) {
        fill(rp, PEN_PANEL, 0, 0, w - 1, h - 1);
        bevel(rp, 0, 0, w - 1, h - 1, 1);
        fill(rp, PEN_BAR, 3, 3, w - 4, 24);
        text_at(rp, PEN_WHITE, 9, 17, "QRX Drive");
        fill(rp, PEN_BUTTON, w - 27, 5, w - 7, 21);
        bevel(rp, w - 27, 5, w - 7, 21, 1);
        text_at(rp, PEN_BLACK, w - 20, 18, "X");
    } else if (g_look == LOOK_MACOS) {
        fill(rp, PEN_PANEL, 0, 0, w - 1, h - 1);
        outline(rp, PEN_EDGE, 0, 0, w - 1, h - 1);
        fill(rp, PEN_BAR, 1, 1, w - 2, 27);
        fill(rp, PEN_RED, 9, 9, 17, 17);
        fill(rp, PEN_YELLOW, 21, 9, 29, 17);
        fill(rp, PEN_GREEN, 33, 9, 41, 17);
        text_at(rp, PEN_BAR_TEXT, (w / 2) - 30, 18, "QRX Drive");
    } else if (g_look == LOOK_AMIGAOS) {
        fill(rp, PEN_PANEL, 0, 0, w - 1, h - 1);
        bevel(rp, 0, 0, w - 1, h - 1, 1);
        fill(rp, PEN_AMIGA, 3, 3, w - 4, 23);
        fill(rp, PEN_PANEL, 5, 5, 20, 20);
        bevel(rp, 5, 5, 20, 20, 1);
        text_at(rp, PEN_WHITE, 28, 17, "QRX Drive");
    } else {
        fill(rp, PEN_PANEL, 0, 0, w - 1, h - 1);
        outline(rp, PEN_EDGE, 0, 0, w - 1, h - 1);
        fill(rp, PEN_BAR, 1, 1, w - 2, 28);
        fill(rp, PEN_ACCENT, 1, 27, w - 2, 28);
        text_at(rp, PEN_ACCENT, 12, 18, "QRX");
        text_at(rp, PEN_BAR_TEXT, 43, 18, "Drive");
        fill(rp, PEN_BUTTON, w - 27, 6, w - 8, 22);
        outline(rp, PEN_EDGE, w - 27, 6, w - 8, 22);
        text_at(rp, PEN_TEXT, w - 21, 19, "X");
    }

    fill(rp, PEN_PANEL_2, 5, 31, w - 6, h - 6);
    draw_resize_handle(rp, w, h);
}

static int qrx_file_visible_count(void) {
    int content_h;
    if (!g_files) return 1;
    content_h = g_files->Height - FILE_LIST_TOP - 42;
    if (content_h < FILE_GRID_H) content_h = FILE_GRID_H;

    if (g_file_icon_view) {
        int cols = (g_files->Width - FILE_LEFT_W - 20) / FILE_GRID_W;
        int rows = content_h / FILE_GRID_H;
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        return cols * rows;
    }
    return content_h / FILE_ROW_H;
}

static int qrx_file_index_at(WORD x, WORD y) {
    int index;
    if (!g_files || x < FILE_LEFT_W + 8 ||
        y < FILE_LIST_TOP || y >= g_files->Height - 36)
        return -1;

    if (g_file_icon_view) {
        int cols = (g_files->Width - FILE_LEFT_W - 20) / FILE_GRID_W;
        int col, row;
        if (cols < 1) cols = 1;
        col = (x - (FILE_LEFT_W + 8)) / FILE_GRID_W;
        row = (y - FILE_LIST_TOP) / FILE_GRID_H;
        if (col < 0 || col >= cols || row < 0) return -1;
        index = g_file_scroll + row * cols + col;
    } else {
        index = g_file_scroll + (y - FILE_LIST_TOP) / FILE_ROW_H;
    }
    return index < g_file_count ? index : -1;
}

static int qrx_drive_drop_target(WORD x, WORD y, char *dest, size_t dest_size) {
    int row;
    if (x < 10 || x > 92) return 0;
    row = qrx_drive_row_from_y(y);
    return qrx_drive_path_for_row(row, dest, dest_size);
}

static void redraw_filemanager(void) {
    struct RastPort *rp;
    int visible, row, idx;
    WORD y;
    char buf[180];

    if (!g_files) return;
    rp = g_files->RPort;
    draw_filemanager_chrome(rp, g_files->Width, g_files->Height);

    fill(rp, PEN_BUTTON, 10, 38, 45, 58);
    outline(rp, PEN_EDGE, 10, 38, 45, 58);
    text_at(rp, PEN_TEXT, 20, 53, "<");
    fill(rp, PEN_BUTTON, 50, 38, 95, 58);
    outline(rp, PEN_EDGE, 50, 38, 95, 58);
    text_at(rp, PEN_TEXT, 58, 53, "REF");

    fill(rp, PEN_PANEL, 105, 38, g_files->Width - 12, 58);
    outline(rp, PEN_EDGE, 105, 38, g_files->Width - 12, 58);
    text_at(rp, PEN_TEXT, 112, 53, g_files_path);

    fill(rp, g_file_icon_view ? PEN_ACCENT : PEN_BUTTON, 105, 64, 158, 84);
    outline(rp, PEN_EDGE, 105, 64, 158, 84);
    text_at(rp, g_file_icon_view ? PEN_BLACK : PEN_TEXT, 113, 79,
            g_file_icon_view ? "ICON" : "LIST");
    fill(rp, PEN_BUTTON, 163, 64, 207, 84);
    outline(rp, PEN_EDGE, 163, 64, 207, 84);
    text_at(rp, PEN_TEXT, 168, 79, "COPY");
    fill(rp, PEN_BUTTON, 212, 64, 248, 84);
    outline(rp, PEN_EDGE, 212, 64, 248, 84);
    text_at(rp, PEN_TEXT, 218, 79, "CUT");
    fill(rp, g_clip_mode != CLIP_NONE ? PEN_ACCENT : PEN_BUTTON,
         253, 64, 301, 84);
    outline(rp, PEN_EDGE, 253, 64, 301, 84);
    text_at(rp, g_clip_mode != CLIP_NONE ? PEN_BLACK : PEN_TEXT,
            259, 79, "PASTE");
    fill(rp, PEN_BUTTON, 306, 64, 350, 84);
    outline(rp, PEN_EDGE, 306, 64, 350, 84);
    text_at(rp, PEN_TEXT, 312, 79, "REN");
    fill(rp, g_delete_armed ? PEN_RED : PEN_BUTTON, 355, 64, 397, 84);
    outline(rp, PEN_EDGE, 355, 64, 397, 84);
    text_at(rp, g_delete_armed ? PEN_WHITE : PEN_TEXT, 362, 79, "DEL");

    /* mounted volumes are refreshed on navigation/explicit REF only; never during paint */
    text_at(rp, PEN_MUTED, 14, 106, "LOCATIONS");
    {
        int drow;
        char dpath[QRX_MOUNTED_VOLUME_NAME + 2];
        char dlabel[12];
        for (drow = 0; drow < 4 + g_mounted_volume_count; ++drow) {
            WORD dy = 116 + drow * 26;
            if (!qrx_drive_path_for_row(drow, dpath, sizeof(dpath))) continue;
            if (drow < 4) {
                snprintf(dlabel, sizeof(dlabel), "%s", dpath);
            } else {
                snprintf(dlabel, sizeof(dlabel), "%.9s:",
                         g_mounted_volumes[drow - 4].name);
            }
            fill(rp, drow >= 4 ? PEN_ACCENT : PEN_BUTTON, 10, dy, 92, dy + 22);
            outline(rp, PEN_EDGE, 10, dy, 92, dy + 22);
            text_at(rp, drow >= 4 ? PEN_BLACK : PEN_TEXT, 16, dy + 15, dlabel);
        }
        if (g_mounted_volume_count == 0)
            text_at(rp, PEN_MUTED, 16, 237, "no media");
    }

    visible = qrx_file_visible_count();
    if (visible < 1) visible = 1;

    if (g_file_icon_view) {
        int cols = (g_files->Width - FILE_LEFT_W - 20) / FILE_GRID_W;
        if (cols < 1) cols = 1;
        for (row = 0; row < visible; ++row) {
            int col;
            WORD x;
            idx = g_file_scroll + row;
            if (idx >= g_file_count) break;
            col = row % cols;
            y = FILE_LIST_TOP + (row / cols) * FILE_GRID_H;
            x = FILE_LEFT_W + 8 + col * FILE_GRID_W;

            if (idx == g_file_selected) {
                fill(rp, PEN_BUTTON, x, y,
                     x + FILE_GRID_W - 5, y + FILE_GRID_H - 5);
                outline(rp, PEN_EDGE, x, y,
                        x + FILE_GRID_W - 5, y + FILE_GRID_H - 5);
            }
            draw_file_icon(rp, idx, x + 24, y + 5);
            snprintf(buf, sizeof(buf), "%.12s%s",
                     g_file_entries[idx].name,
                     g_file_entries[idx].dir_type > 0 ? "/" : "");
            text_at(rp, PEN_TEXT, x + 4, y + 55, buf);
            if (g_file_entries[idx].generated_icon)
                text_at(rp, PEN_MUTED, x + 4, y + 67, "auto icon");
        }
    } else {
        y = FILE_LIST_TOP;
        for (row = 0; row < visible; ++row) {
            idx = g_file_scroll + row;
            if (idx >= g_file_count) break;
            if (idx == g_file_selected)
                fill(rp, PEN_BUTTON, FILE_LEFT_W + 8, y,
                     g_files->Width - 12, y + FILE_ROW_H - 2);
            draw_file_icon(rp, idx, FILE_LEFT_W + 14, y + 2);
            snprintf(buf, sizeof(buf), "%s%s",
                     g_file_entries[idx].name,
                     g_file_entries[idx].dir_type > 0 ? "/" : "");
            text_at(rp, PEN_TEXT, FILE_LEFT_W + 68, y + 17, buf);
            text_at(rp, PEN_MUTED, g_files->Width - 85, y + 17,
                    g_file_entries[idx].has_icon
                      ? qrx_icon_type_name(g_file_entries[idx].icon_type)
                      : (g_file_entries[idx].generated_icon ? "AutoIcon" : ""));
            y += FILE_ROW_H;
        }
    }

    fill(rp, PEN_PANEL, 8, g_files->Height - 32,
         g_files->Width - 8, g_files->Height - 8);
    if (g_rename_active) {
        snprintf(buf, sizeof(buf), "Rename: %s_", g_rename_text);
        text_at(rp, PEN_ACCENT, 14, g_files->Height - 16, buf);
    } else if (g_file_status[0]) {
        text_at(rp, PEN_MUTED, 14, g_files->Height - 16, g_file_status);
    } else if (g_file_selected >= 0 && g_file_selected < g_file_count) {
        struct QRXFileEntry *e = &g_file_entries[g_file_selected];
        snprintf(buf, sizeof(buf), "%s | %ld bytes | %s",
                 e->dir_type > 0 ? "Drawer" : qrx_icon_type_name(e->icon_type),
                 e->size, e->has_icon ? ".info" :
                 (e->generated_icon ? "auto icon" : "no icon"));
        text_at(rp, PEN_MUTED, 14, g_files->Height - 16, buf);
    } else {
        snprintf(buf, sizeof(buf), "%d objects | %s view",
                 g_file_count, g_file_icon_view ? "icon" : "list");
        text_at(rp, PEN_MUTED, 14, g_files->Height - 16, buf);
    }

    if (g_object_dragging && g_file_selected >= 0 &&
        g_file_selected < g_file_count) {
        snprintf(buf, sizeof(buf), "MOVE %s -> drawer/drive",
                 g_file_entries[g_file_selected].name);
        text_at(rp, PEN_ACCENT, 110, 105, buf);
    }
}

static int filemanager_close_hit(WORD x, WORD y) {
    WORD w;
    if (!g_files) return 0;
    w = g_files->Width;

    if (g_look == LOOK_MACOS)
        return (x >= 7 && x <= 20 && y >= 6 && y <= 21);
    if (g_look == LOOK_AMIGAOS)
        return (x >= 3 && x <= 22 && y >= 3 && y <= 22);

    return (x >= w - 30 && x <= w - 5 && y >= 3 && y <= 25);
}

static int filemanager_title_hit(WORD x, WORD y) {
    if (!g_files || y < 1 || y > TITLE_H) return 0;
    if (g_look == LOOK_MACOS && x <= 48) return 0;
    if (g_look == LOOK_AMIGAOS && x <= 24) return 0;
    if ((g_look == LOOK_QRX || g_look == LOOK_WINDOWS) &&
        x >= g_files->Width - 32) return 0;
    return 1;
}

static int filemanager_resize_hit(WORD x, WORD y) {
    if (!g_files) return 0;
    return x >= g_files->Width - RESIZE_GRAB &&
           y >= g_files->Height - RESIZE_GRAB;
}

static void begin_file_gesture(WORD x, WORD y, int resize) {
    if (!g_files) return;

    g_files_gesture_abs_x = g_screen ? g_screen->MouseX : g_files->LeftEdge + x;
    g_files_gesture_abs_y = g_screen ? g_screen->MouseY : g_files->TopEdge + y;
    g_files_gesture_left = g_files->LeftEdge;
    g_files_gesture_top = g_files->TopEdge;
    g_files_gesture_width = g_files->Width;
    g_files_gesture_height = g_files->Height;
    g_files_resizing = resize;
    g_files_dragging = !resize;
}

static void update_file_gesture(WORD x, WORD y) {
    LONG abs_x, abs_y, dx, dy, left, top, width, height;

    if (!g_files || (!g_files_dragging && !g_files_resizing)) return;

    abs_x = g_screen ? g_screen->MouseX : g_files->LeftEdge + x;
    abs_y = g_screen ? g_screen->MouseY : g_files->TopEdge + y;
    dx = abs_x - g_files_gesture_abs_x;
    dy = abs_y - g_files_gesture_abs_y;

    if (g_files_dragging) {
        left = g_files_gesture_left + dx;
        top = g_files_gesture_top + dy;

        if (left < 0) left = 0;
        if (top < 22) top = 22;
        if (left > g_screen_width - 80) left = g_screen_width - 80;
        if (top > g_screen_height - 40) top = g_screen_height - 40;

        MoveWindow(g_files,
                   left - g_files->LeftEdge,
                   top - g_files->TopEdge);
    } else {
        width = g_files_gesture_width + dx;
        height = g_files_gesture_height + dy;

        if (width < FILE_MIN_W) width = FILE_MIN_W;
        if (height < FILE_MIN_H) height = FILE_MIN_H;
        if (g_files->LeftEdge + width > g_screen_width)
            width = g_screen_width - g_files->LeftEdge;
        if (g_files->TopEdge + height > g_screen_height)
            height = g_screen_height - g_files->TopEdge;

        SizeWindow(g_files,
                   width - g_files->Width,
                   height - g_files->Height);
    }
}

static void finish_file_gesture(void) {
    g_files_dragging = 0;
    g_files_resizing = 0;
    if (g_files) {
        g_files_left = g_files->LeftEdge;
        g_files_top = g_files->TopEdge;
        g_files_width = g_files->Width;
        g_files_height = g_files->Height;
        redraw_filemanager();
    }
}

static void close_filemanager(void) {
    if (!g_files) return;
    CloseWindow(g_files);
    g_files = NULL;
    g_files_dragging = 0;
    g_files_resizing = 0;
    g_object_drag_candidate = -1;
    g_object_dragging = 0;
    g_rename_active = 0;
    redraw_desktop();
}

static int open_filemanager(const char *path) {
    if (path && *path)
        qrx_load_directory(path);
    else if (g_file_count == 0)
        qrx_load_directory(g_files_path);

    if (g_files) {
        WindowToFront(g_files);
        ActivateWindow(g_files);
        redraw_filemanager();
        return 1;
    }

    g_files = OpenWindowTags(NULL,
        (g_screen_borrowed ? WA_PubScreen : WA_CustomScreen), (ULONG)g_screen,
        WA_Left, g_files_left,
        WA_Top, g_files_top,
        WA_Width, g_files_width,
        WA_Height, g_files_height,
        WA_MinWidth, FILE_MIN_W,
        WA_MinHeight, FILE_MIN_H,
        WA_MaxWidth, g_screen_width,
        WA_MaxHeight, g_screen_height,
        WA_Flags, WFLG_ACTIVATE | WFLG_BORDERLESS | WFLG_SMART_REFRESH |
                  WFLG_RMBTRAP | WFLG_REPORTMOUSE,
        WA_IDCMP, IDCMP_VANILLAKEY | IDCMP_RAWKEY | IDCMP_REFRESHWINDOW |
                  IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_NEWSIZE | IDCMP_RAWKEY,
        TAG_DONE);

    if (!g_files) {
        add_line("QRX Drive: could not open File Manager window.");
        return 0;
    }

    redraw_filemanager();
    return 1;
}

static LONG qrx_open_file_entry(int index) {
    struct QRXFileEntry *e;
    char full[FILE_PATH_LEN + 128];
    if (index < 0 || index >= g_file_count) return 0;
    e = &g_file_entries[index];
    qrx_join_path(full, sizeof(full), g_files_path, e->name);
    /* Drawers are navigation, not external objects: keep one browser window. */
    if (e->dir_type > 0) {
        if (qrx_load_directory(full)) { qrx_set_status(""); return 1; }
        qrx_set_status("Cannot enter drawer.");
        redraw_filemanager();
        return 0;
    }
    return qrx_open_path(full, e->dir_type, e->has_icon,
                         e->icon_type, e->default_tool);
}

static void qrx_file_object_click(int index) {
    ULONG sec = 0, micro = 0;
    int is_double = 0;
    if (index < 0 || index >= g_file_count) return;

    CurrentTime(&sec, &micro);
    if (index == g_last_file_click &&
        DoubleClick(g_last_file_sec, g_last_file_micro, sec, micro))
        is_double = 1;

    g_last_file_click = index;
    g_last_file_sec = sec;
    g_last_file_micro = micro;
    g_file_selected = index;
    g_delete_armed = 0;
    qrx_set_status("");
    redraw_filemanager();
    /* Drawers are navigation objects: one click opens them. Files retain double-click launch. */
    if (g_file_entries[index].dir_type > 0) qrx_open_file_entry(index);
    else if (is_double) qrx_open_file_entry(index);
}

static int qrx_drop_target_path(WORD x, WORD y, char *dest, size_t dest_size) {
    int target = qrx_file_index_at(x, y);
    if (qrx_drive_drop_target(x, y, dest, dest_size)) return 1;
    if (target >= 0 && target < g_file_count &&
        g_file_entries[target].dir_type > 0) {
        qrx_join_path(dest, dest_size, g_files_path,
                      g_file_entries[target].name);
        return 1;
    }
    return 0;
}

static void qrx_finish_object_drag(WORD x, WORD y) {
    char dest[FILE_PATH_LEN + 128];

    if (!g_object_dragging) {
        if (g_object_drag_candidate >= 0)
            qrx_file_object_click(g_object_drag_candidate);
    } else if (g_file_selected >= 0 &&
               qrx_drop_target_path(x, y, dest, sizeof(dest))) {
        qrx_move_index_to(g_file_selected, dest);
        qrx_load_directory(g_files_path);
        qrx_scan_desktop();
        redraw_desktop();
    } else {
        qrx_set_status("Drag cancelled: drop on a drawer or drive.");
    }

    g_object_drag_candidate = -1;
    g_object_dragging = 0;
    redraw_filemanager();
}

static void handle_filemanager_message(struct IntuiMessage *msg) {
    ULONG cls;
    UWORD code;
    WORD x, y;
    int index;

    if (!msg || !g_files) return;
    cls = msg->Class;
    code = msg->Code;
    x = msg->MouseX;
    y = msg->MouseY;
    ReplyMsg((struct Message *)msg);

    if (cls == IDCMP_REFRESHWINDOW) {
        BeginRefresh(g_files);
        redraw_filemanager();
        EndRefresh(g_files, TRUE);
        return;
    }

    if (cls == IDCMP_NEWSIZE) {
        finish_file_gesture();
        return;
    }

    if (cls == IDCMP_VANILLAKEY && g_rename_active) {
        if (code == 13 || code == 10) qrx_commit_rename();
        else if (code == 27) {
            g_rename_active = 0;
            qrx_set_status("Rename cancelled.");
        } else if (code == 8 || code == 127) {
            if (g_rename_len > 0) g_rename_text[--g_rename_len] = 0;
        } else if (code >= 32 && code <= 126 &&
                   g_rename_len < (int)sizeof(g_rename_text) - 1) {
            g_rename_text[g_rename_len++] = (char)code;
            g_rename_text[g_rename_len] = 0;
        }
        redraw_filemanager();
        return;
    }

    if (cls == IDCMP_MOUSEMOVE) {
        if (g_files_dragging || g_files_resizing) {
            update_file_gesture(x, y);
            return;
        }
        if (g_object_drag_candidate >= 0 && !g_object_dragging) {
            int dx = x - g_object_drag_start_x;
            int dy = y - g_object_drag_start_y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx > 5 || dy > 5) {
                g_object_dragging = 1;
                g_file_selected = g_object_drag_candidate;
                qrx_set_status("Drag object onto a drawer or drive.");
                redraw_filemanager();
            }
        }
        return;
    }

    if (cls != IDCMP_MOUSEBUTTONS) return;

    if (code == SELECTUP) {
        if (g_files_dragging || g_files_resizing) {
            finish_file_gesture();
            return;
        }
        if (g_object_drag_candidate >= 0 || g_object_dragging) {
            qrx_finish_object_drag(x, y);
            return;
        }
        return;
    }
    if (code != SELECTDOWN) return;
    if (g_rename_active) return;

    if (filemanager_close_hit(x, y)) { close_filemanager(); return; }
    if (filemanager_resize_hit(x, y)) { begin_file_gesture(x, y, 1); return; }
    if (filemanager_title_hit(x, y)) { begin_file_gesture(x, y, 0); return; }

    if (y >= 38 && y <= 58) {
        if (x >= 10 && x <= 45) {
            qrx_parent_directory(); qrx_set_status(""); redraw_filemanager(); return;
        }
        if (x >= 50 && x <= 95) {
            qrx_load_directory(g_files_path); qrx_set_status("Refreshed.");
            redraw_filemanager(); return;
        }
    }

    if (y >= 64 && y <= 84) {
        if (x >= 105 && x <= 158) {
            g_file_icon_view = !g_file_icon_view;
            g_file_scroll = 0;
            save_preferences();
            qrx_set_status(g_file_icon_view ? "Icon view." : "List view.");
        } else if (x >= 163 && x <= 207) {
            qrx_clipboard_set(CLIP_COPY, g_file_selected);
        } else if (x >= 212 && x <= 248) {
            qrx_clipboard_set(CLIP_MOVE, g_file_selected);
        } else if (x >= 253 && x <= 301) {
            qrx_paste_clipboard(g_files_path);
        } else if (x >= 306 && x <= 350) {
            qrx_begin_rename();
        } else if (x >= 355 && x <= 397) {
            qrx_delete_selected();
        }
        redraw_filemanager();
        return;
    }

    if (x >= 10 && x <= 92) {
        char drive_path[QRX_MOUNTED_VOLUME_NAME + 2];
        int drive_row = qrx_drive_row_from_y(y);
        if (qrx_drive_path_for_row(drive_row, drive_path, sizeof(drive_path))) {
            if (!qrx_load_directory(drive_path))
                qrx_set_status("Volume unavailable or media removed.");
            else
                qrx_set_status("");
            redraw_filemanager();
            return;
        }
    }

    index = qrx_file_index_at(x, y);
    if (index >= 0) {
        g_object_drag_candidate = index;
        g_object_drag_start_x = x;
        g_object_drag_start_y = y;
        g_file_selected = index;
        g_delete_armed = 0;
        qrx_set_status("");
        redraw_filemanager();
    }
}

static int qrx_desktop_dynamic_index_at(WORD x, WORD y) {
    int i;
    WORD ix = 100, iy = 72;
    for (i = 0; i < g_desktop_count; ++i) {
        if (x >= ix - 6 && x <= ix + 76 &&
            y >= iy - 6 && y <= iy + 52) return i;
        ix += 92;
        if (ix > g_screen_width - 220) { ix = 100; iy += 72; }
    }
    return -1;
}

static int qrx_desktop_object_click(WORD x, WORD y) {
    ULONG sec = 0, micro = 0;
    int index;

    if (x >= 18 && x <= 72) {
        if (y >= 66 && y <= 120) { open_filemanager("SYS:"); return 1; }
        if (y >= 126 && y <= 180) { open_filemanager("QRX:"); return 1; }
        if (y >= 186 && y <= 240) { open_filemanager("RAM:"); return 1; }
    }

    index = qrx_desktop_dynamic_index_at(x, y);
    if (index < 0) return 0;

    CurrentTime(&sec, &micro);
    if (index == g_desktop_last_click &&
        DoubleClick(g_desktop_last_sec, g_desktop_last_micro, sec, micro)) {
        struct QRXDesktopEntry *e = &g_desktop_entries[index];
        qrx_open_path(e->full_path, e->dir_type, e->has_icon,
                      e->icon_type, e->default_tool);
    }

    g_desktop_last_click = index;
    g_desktop_last_sec = sec;
    g_desktop_last_micro = micro;
    g_desktop_selected = index;
    redraw_desktop();
    return 1;
}

static int desktop_click_files_launcher(WORD x, WORD y) {
    if (g_look == LOOK_QRX) {
        WORD dock_x = (g_screen_width - 288) / 2;
        WORD dock_y = g_screen_height - 38;
        if (dock_x < 8) dock_x = 8;
        return x >= dock_x + 72 && x <= dock_x + 118 &&
               y >= dock_y + 8 && y <= dock_y + 24;
    }
    if (g_look == LOOK_MACOS) {
        WORD dock_x = (g_screen_width - 274) / 2;
        WORD dock_y = g_screen_height - 41;
        if (dock_x < 8) dock_x = 8;
        return x >= dock_x + 74 && x <= dock_x + 120 &&
               y >= dock_y + 9 && y <= dock_y + 26;
    }
    if (g_look == LOOK_WINDOWS) {
        WORD ty = g_screen_height - 28;
        return x >= 248 && x <= 324 && y >= ty + 4 && y <= ty + 23;
    }
    if (g_look == LOOK_AMIGAOS) {
        WORD ay = g_screen_height - 42;
        return x >= 184 && x <= 328 && y >= ay && y <= ay + 36;
    }
    return 0;
}

static void redraw_terminal(void);

static int desktop_click_dock(WORD x, WORD y) {
    WORD dx, dy;
    if (g_look == LOOK_QRX) {
        dx=(g_screen_width-288)/2; if(dx<8)dx=8; dy=g_screen_height-38;
        if(y>=dy+8 && y<=dy+25) {
            if(x>=dx+14 && x<=dx+62) { WindowToFront(g_terminal); ActivateWindow(g_terminal); return 1; }
            if(x>=dx+72 && x<=dx+122) { open_filemanager(g_files_path); return 1; }
            if(x>=dx+130 && x<=dx+180) { launch_native("QRX:C/QRXAI"); return 1; }
            if(x>=dx+188 && x<=dx+258) { launch_native("QRX:C/QRXSettings"); return 1; }
        }
    } else if (g_look == LOOK_MACOS) {
        dx=(g_screen_width-274)/2; if(dx<8)dx=8; dy=g_screen_height-41;
        if(y>=dy+8 && y<=dy+27) {
            if(x>=dx+14 && x<=dx+64) { WindowToFront(g_terminal); ActivateWindow(g_terminal); return 1; }
            if(x>=dx+72 && x<=dx+122) { open_filemanager(g_files_path); return 1; }
            if(x>=dx+130 && x<=dx+180) { launch_native("QRX:C/QRXAI"); return 1; }
        }
    }
    if(y>=g_screen_height-48){WORD cx=g_screen_width/2;if(x>=cx-150&&x<cx-80){WindowToFront(g_terminal);ActivateWindow(g_terminal);return 1;}if(x>=cx-80&&x<cx-10){open_filemanager(g_files_path);return 1;}if(x>=cx-10&&x<cx+60){launch_native("QRX:C/QRXAI");return 1;}if(x>=cx+60&&x<=cx+160){launch_native("QRX:C/QRXSettings");return 1;}}
    return 0;
}

static int desktop_click_productivity(WORD x,WORD y){
    WORD px=g_screen_width-112,py=58;if(px<520)px=520;if(x<px+7||x>g_screen_width-14)return 0;
    if(y>=py+26&&y<=py+48){launch_native("QRX:C/QRXWriter");return 1;} if(y>=py+52&&y<=py+74){launch_native("QRX:C/QRXSpreadsheet");return 1;}
    if(y>=py+78&&y<=py+100){launch_native("QRX:C/QRXPresenter");return 1;} if(y>=py+104&&y<=py+126){launch_native("QRX:C/QRXHardware");return 1;}
    if(y>=py+130&&y<=py+152){launch_native("QRX:C/QRXSettings");return 1;} if(y>=py+156&&y<=py+178){launch_native("QRX:C/QRXWelcome");return 1;}
    if(y>=py+182&&y<=py+204){launch_native("QRX:C/QRXWeb");return 1;} if(y>=py+208&&y<=py+230){launch_native("QRX:C/ARESAntivirus");return 1;}
    if(y>=py+234&&y<=py+256){open_filemanager(g_files_path);return 1;} return 0;
}
static void redraw_desktop(void) {
    struct RastPort *rp;
    if (!g_screen || !g_desktop) return;

    rp = g_desktop->RPort;
    if (!g_screen_borrowed) {
        draw_wallpaper(g_screen);
        apply_look_palette();
    } else {
        apply_look_palette();
        SetAPen(rp, qrx_pen(PEN_PANEL_2));
        RectFill(rp, 0, 0, g_desktop->Width - 1, g_desktop->Height - 1);
    }

    if (g_look == LOOK_WINDOWS) draw_windows_desktop(rp);
    else if (g_look == LOOK_MACOS) draw_macos_desktop(rp);
    else if (g_look == LOOK_AMIGAOS) draw_amiga_desktop(rp);
    else draw_qrx_desktop(rp);
    qrx_draw_datetime(rp);

    draw_desktop_disk_icon(rp, 24, 72, "SYS:", PEN_BLUE);
    draw_desktop_disk_icon(rp, 24, 132, "QRX:", PEN_ACCENT);
    draw_desktop_disk_icon(rp, 24, 192, "RAM:", PEN_GREEN);

    {
        int i;
        WORD x = 100;
        WORD y = 72;
        for (i = 0; i < g_desktop_count; ++i) {
            if (g_desktop_selected == i) {
                fill(rp, PEN_BUTTON, x - 6, y - 6, x + 76, y + 52);
                outline(rp, PEN_EDGE, x - 6, y - 6, x + 76, y + 52);
            }
            draw_desktop_dynamic_icon(rp, i, x, y);
            x += 92;
            if (x > g_screen_width - 220) { x = 100; y += 72; }
        }
    }

    draw_foreign_window_tabs(rp);
    draw_productivity_launcher(rp);
    qrx_draw_ai_float(rp);
    qrx_draw_win_start_menu(rp);
    if (g_context_open) {
        WORD x=g_context_x,y=g_context_y; if(x>g_screen_width-150)x=g_screen_width-150; if(y>g_screen_height-110)y=g_screen_height-110;
        fill(rp,PEN_PANEL,x,y,x+145,y+100); outline(rp,PEN_EDGE,x,y,x+145,y+100);
        text_at(rp,PEN_TEXT,x+10,y+20,"Open QRX Drive"); text_at(rp,PEN_TEXT,x+10,y+40,"Open Terminal");
        text_at(rp,PEN_TEXT,x+10,y+60,"Refresh Desktop"); text_at(rp,PEN_TEXT,x+10,y+80,"Settings");
    }
}

static void draw_resize_handle(struct RastPort *rp, WORD w, WORD h) {
    UBYTE pen = (g_look == LOOK_AMIGAOS || g_look == LOOK_WINDOWS) ? PEN_DARK : PEN_MUTED;
    SetAPen(rp, qrx_pen(pen));
    Move(rp, w - 13, h - 5); Draw(rp, w - 5, h - 13);
    Move(rp, w - 9, h - 5); Draw(rp, w - 5, h - 9);
    Move(rp, w - 5, h - 5); Draw(rp, w - 5, h - 5);
}

static void draw_terminal_chrome(struct RastPort *rp, WORD w, WORD h) {
    if (g_look == LOOK_WINDOWS) {
        fill(rp, PEN_PANEL, 0, 0, w - 1, h - 1);
        bevel(rp, 0, 0, w - 1, h - 1, 1);
        fill(rp, PEN_BAR, 3, 3, w - 4, 24);
        text_at(rp, PEN_WHITE, 9, 17, "QRX Terminal");
        fill(rp, PEN_BUTTON, w - 27, 5, w - 7, 21);
        bevel(rp, w - 27, 5, w - 7, 21, 1);
        text_at(rp, PEN_BLACK, w - 20, 18, "X");
    } else if (g_look == LOOK_MACOS) {
        fill(rp, PEN_PANEL, 0, 0, w - 1, h - 1);
        outline(rp, PEN_EDGE, 0, 0, w - 1, h - 1);
        fill(rp, PEN_BAR, 1, 1, w - 2, 27);
        fill(rp, PEN_RED,    9, 9, 17, 17);
        fill(rp, PEN_YELLOW,21, 9, 29, 17);
        fill(rp, PEN_GREEN, 33, 9, 41, 17);
        text_at(rp, PEN_BAR_TEXT, (w / 2) - 42, 18, "QRX Terminal");
    } else if (g_look == LOOK_AMIGAOS) {
        fill(rp, PEN_PANEL, 0, 0, w - 1, h - 1);
        bevel(rp, 0, 0, w - 1, h - 1, 1);
        fill(rp, PEN_AMIGA, 3, 3, w - 4, 23);
        fill(rp, PEN_PANEL, 5, 5, 20, 20);
        bevel(rp, 5, 5, 20, 20, 1);
        text_at(rp, PEN_WHITE, 28, 17, "QRX Terminal");
        fill(rp, PEN_PANEL, w - 22, 5, w - 7, 20);
        bevel(rp, w - 22, 5, w - 7, 20, 1);
    } else {
        fill(rp, PEN_PANEL, 0, 0, w - 1, h - 1);
        outline(rp, PEN_EDGE, 0, 0, w - 1, h - 1);
        fill(rp, PEN_BAR, 1, 1, w - 2, 28);
        fill(rp, PEN_ACCENT, 1, 27, w - 2, 28);
        text_at(rp, PEN_ACCENT, 12, 18, "QRX");
        text_at(rp, PEN_BAR_TEXT, 43, 18, "Terminal");
        fill(rp, PEN_BUTTON, w - 27, 6, w - 8, 22);
        outline(rp, PEN_EDGE, w - 27, 6, w - 8, 22);
        text_at(rp, PEN_TEXT, w - 21, 19, "X");
    }

    fill(rp, PEN_PANEL_2, 5, 31, w - 6, h - 6);
    draw_resize_handle(rp, w, h);
}

static void redraw_terminal(void) {
    struct RastPort *rp; int i,y,cols,startline,used=0; char prompt[INPUT_LEN+12],subtitle[96],part[96]; const char *q; int n;
    if(!g_terminal)return;rp=g_terminal->RPort;draw_terminal_chrome(rp,g_terminal->Width,g_terminal->Height);
    snprintf(subtitle,sizeof(subtitle),"native 68K / %s / QRX   Up/Down history  TAB complete",look_name(g_look));text_at(rp,PEN_MUTED,14,48,subtitle);
    cols=(g_terminal->Width-32)/8;if(cols<20)cols=20;if(cols>90)cols=90;startline=qrx_term_scroll;if(startline<0)startline=0;y=65;
    for(i=startline;i<line_count&&y<g_terminal->Height-30;i++){q=lines[i];if(!*q){y+=11;continue;}while(*q&&y<g_terminal->Height-30){n=strlen(q);if(n>cols)n=cols;if(q[n]&&n==cols){int c=n;while(c>8&&q[c]!=' ')c--;if(c>8)n=c;}memcpy(part,q,n);part[n]=0;text_at(rp,PEN_TEXT,14,y,part);q+=n;while(*q==' ')q++;y+=11;used++;}}
    snprintf(prompt,sizeof(prompt),"qrx:> %s_",input); if((int)strlen(prompt)>cols) text_at(rp,PEN_ACCENT,14,g_terminal->Height-15,prompt+strlen(prompt)-cols); else text_at(rp,PEN_ACCENT,14,g_terminal->Height-15,prompt);
}
static void redraw_terminal_input(void) {
    struct RastPort *rp;
    char prompt[INPUT_LEN + 12];
    WORD y;
    if (!g_terminal) return;
    rp = g_terminal->RPort;
    y = g_terminal->Height - 26;
    /* Only repaint the command line. Full-window redraw on every key caused
       visible flashes on OCS/ECS and under emulation. */
    fill(rp, PEN_PANEL_2, 6, y, g_terminal->Width - 7, g_terminal->Height - 7);
    snprintf(prompt, sizeof(prompt), "qrx:> %s_", input);
    text_at(rp, PEN_ACCENT, 14, g_terminal->Height - 15, prompt);
}

static void redraw_all(void) {
    redraw_desktop();
    redraw_terminal();
}

static void set_look(enum LookFeel look) {
    g_look = look;
    apply_look_palette();
    redraw_all();
    redraw_filemanager();
    save_preferences();
}

static void capture_file_into_terminal(const char *path) {
    BPTR fh;
    char buf[256];
    int any = 0;

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) return;

    while (FGets(fh, buf, sizeof(buf))) {
        trim_eol(buf);
        add_wrapped(buf);
        any = 1;
    }
    Close(fh);

    if (!any) add_line("(command produced no output)");
}

static LONG execute_capture(const char *command) {
    BPTR in_fh, out_fh;
    LONG ok;

    DeleteFile((STRPTR)DOS_CAPTURE);
    in_fh = Open((STRPTR)"NIL:", MODE_OLDFILE);
    out_fh = Open((STRPTR)DOS_CAPTURE, MODE_NEWFILE);

    if (!out_fh) {
        if (in_fh) Close(in_fh);
        add_line("AmigaDOS bridge: cannot create capture file.");
        return 0;
    }

    ok = Execute((STRPTR)command, in_fh, out_fh);

    if (in_fh) Close(in_fh);
    Close(out_fh);

    capture_file_into_terminal(DOS_CAPTURE);
    DeleteFile((STRPTR)DOS_CAPTURE);

    if (!ok) add_line("AmigaDOS Execute() reported failure.");
    return ok;
}

static LONG launch_native(const char *command) {
    LONG rc;
    if (!command || !*command) { add_line("Usage: run <Amiga executable> [arguments]"); return 0; }
    rc = SystemTags((STRPTR)command, SYS_Asynch, TRUE, SYS_Input, 0, SYS_Output, 0, TAG_DONE);
    if (rc != -1) { add_line("Native 68K process launched."); return 1; }
    add_line("Launch failed. Check path, executable and required libraries.");
    return 0;
}



static int qrx_schedule_takeover(const char *command) {
    if (g_screen_borrowed) {
        add_line("TAKEOVER unavailable in existing-public-screen host mode.");
        return 0;
    }
    if (!command || !*command) {
        add_line("TAKEOVER needs a command.");
        return 0;
    }
    if (has_foreign_windows()) {
        add_line("Close visitor Amiga windows before TAKEOVER.");
        return 0;
    }

    save_preferences();
    snprintf(g_session_command, sizeof(g_session_command), "%s", command);
    g_session_action = SESSION_TAKEOVER;
    add_line("TAKEOVER scheduled: QRX will release its AGA screen.");
    return 1;
}

static int qrx_schedule_whdload(const char *slave, const char *data_dir,
                                const char *options) {
    if (g_screen_borrowed) {
        add_line("WHDLoad unavailable in existing-public-screen host mode.");
        return 0;
    }
    if (!slave || !*slave) {
        add_line("WHDLOAD needs a Slave path.");
        return 0;
    }
    if (has_foreign_windows()) {
        add_line("Close visitor Amiga windows before WHDLoad.");
        return 0;
    }

    save_preferences();
    snprintf(g_session_slave, sizeof(g_session_slave), "%s", slave);
    snprintf(g_session_data, sizeof(g_session_data), "%s",
             data_dir && *data_dir ? data_dir : "SYS:");
    snprintf(g_session_options, sizeof(g_session_options), "%s",
             options ? options : "");
    g_session_action = SESSION_WHDLOAD;
    add_line("WHDLoad scheduled: QRX will release its AGA screen.");
    return 1;
}

static LONG qrx_execute_wait_no_ui(const char *command) {
    BPTR in_fh, out_fh;
    LONG ok;

    if (!command || !*command) return 0;

    in_fh = Open((STRPTR)"NIL:", MODE_OLDFILE);
    out_fh = Open((STRPTR)"NIL:", MODE_NEWFILE);
    ok = Execute((STRPTR)command, in_fh, out_fh);
    if (in_fh) Close(in_fh);
    if (out_fh) Close(out_fh);
    return ok;
}

static int qrx_find_whdload(char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = 0;

    if (qrx_path_exists("C:WHDLoad")) {
        snprintf(out, out_size, "C:WHDLoad");
        return 1;
    }
    if (qrx_path_exists("SYS:C/WHDLoad")) {
        snprintf(out, out_size, "SYS:C/WHDLoad");
        return 1;
    }
    if (qrx_path_exists("QRX:Legacy/WHDLoad")) {
        snprintf(out, out_size, "QRX:Legacy/WHDLoad");
        return 1;
    }
    return 0;
}

static void qrx_write_legacy_log(const char *mode, const char *command,
                                 LONG result) {
    BPTR fh;
    char buf[1024];

    fh = Open((STRPTR)LEGACY_LOG_PATH, MODE_NEWFILE);
    if (!fh) return;

    snprintf(buf, sizeof(buf),
             "QRX OS One Amiga 68K v0.20\n"
             "mode=%s\n"
             "command=%s\n"
             "result=%ld\n",
             mode ? mode : "UNKNOWN",
             command ? command : "",
             result);
    FPuts(fh, buf);
    Close(fh);
}

static LONG qrx_execute_takeover_session(void) {
    LONG result;

    result = qrx_execute_wait_no_ui(g_session_command);
    qrx_write_legacy_log("TAKEOVER", g_session_command, result);
    snprintf(g_resume_notice, sizeof(g_resume_notice),
             "Returned from TAKEOVER (result=%ld).", result);
    return result;
}

static LONG qrx_execute_whdload_session(void) {
    char whdload[128];
    char command[LEGACY_CMD_LEN];
    LONG result;

    if (!qrx_find_whdload(whdload, sizeof(whdload))) {
        snprintf(g_resume_notice, sizeof(g_resume_notice),
                 "WHDLoad missing. Install in C: or QRX:Legacy.");
        qrx_write_legacy_log("WHDLOAD", g_session_slave, 0);
        return 0;
    }

    snprintf(command, sizeof(command),
             "%s Slave=\"%s\" Data=\"%s\" Preload NoWriteInfo %s",
             whdload, g_session_slave, g_session_data, g_session_options);

    result = qrx_execute_wait_no_ui(command);
    qrx_write_legacy_log("WHDLOAD", command, result);
    snprintf(g_resume_notice, sizeof(g_resume_notice),
             "Returned from WHDLoad (result=%ld).", result);
    return result;
}

static int has_foreign_windows(void) {
    struct Window *w;

    if (!g_screen || g_screen_borrowed) return 0;

    for (w = g_screen->FirstWindow; w; w = w->NextWindow) {
        if (w != g_desktop && w != g_terminal && w != g_files)
            return 1;
    }
    return 0;
}

static void qrx_init_posix_environment(void) {
    SetVar((STRPTR)"HOME", (STRPTR)"/qrx/Home", 9, GVF_GLOBAL_ONLY);
    SetVar((STRPTR)"TMPDIR", (STRPTR)"/tmp", 4, GVF_GLOBAL_ONLY);
    SetVar((STRPTR)"QRX_POSIX_CWD", (STRPTR)"/", 1, GVF_GLOBAL_ONLY);
    SetVar((STRPTR)"QRX_POSIX_PATH", (STRPTR)"/qrx/POSIX/bin:/sys/C", 21, GVF_GLOBAL_ONLY);
}

static int execute_command(const char *cmd) {
    char buf[TERM_LINE_LEN + 1];
    char doscmd[512];
    ULONG allmem, chipmem, fastmem;
    enum LookFeel look;

    if (!cmd || !*cmd) return 1;

    snprintf(buf, sizeof(buf), "qrx:> %.68s", cmd);
    add_line(buf);

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        add_line("help version info mem htop clear about looks look <name> prefs");
        add_line("dos ! dir run apps files view copy cut paste rename delete");
        add_line("writer sheet presenter hardware usb display posix linux linuxabi linuxrun qrx86 x86run x86check win winrun wine wineprobe wineboot winedisplay wineservices dynarec winext msi winrt winscan winload wininit winbridge winjit wintrace notepadprobe reactoscalc reactosnotepad reactosthird reactossolitaire reactostest compat profile elf unix");
        add_line("pwd ls cat cp mv rm mkdir uname socket mmap");
        add_line("windows focus <n> desktop open legacy takeover runmode ai chat color quit");
        add_line("amigacheck <prog>  amigarun [ASYNC] [STACK=n] <prog> [args]  (AmigaOS 1.x-3.x HUNK)");
        add_line("i386 <prog> [args]  win32 <prog.exe>  rechner minesweeper uhr  (x86 via qrx86core)");
        add_line("welcome on|off  (Welcome dialog autostart)");
    } else if (!strcmp(cmd, "version") || !strcmp(cmd, "ver")) {
        add_line("QRX OS One Amiga 68K native desktop v0.20");
    } else if (!strcmp(cmd, "info")) {
        snprintf(buf, sizeof(buf), "Display: %s (%dx%dx%d)",
                 g_display_is_rtg ? "RTG/Enhanced" : "AGA/Classic",
                 (int)g_screen_width, (int)g_screen_height,
                 (int)g_screen_depth);
        add_line(buf);
        add_line("QRX screen is the default public Intuition screen.");
        add_line(PoseidonBase ? "USB: Poseidon library detected." :
                                "USB: Poseidon stack not detected.");
        add_line("Productivity: Writer / Spreadsheet / Presenter native 68K.");
        add_line("QRX86 Advanced: PT_INTERP, shared libs, TLS/futex groundwork, WineBoot probe.");
        add_line("Legacy: NORMAL / TAKEOVER / WHDLOAD on real Amiga hardware.");
    } else if (!strcmp(cmd, "mem")) {
        allmem  = AvailMem(MEMF_ANY);
        chipmem = AvailMem(MEMF_CHIP);
        fastmem = AvailMem(MEMF_FAST);
        snprintf(buf, sizeof(buf), "RAM all=%lu KB chip=%lu KB fast=%lu KB",
                 allmem/1024UL, chipmem/1024UL, fastmem/1024UL);
        add_line(buf);
    } else if (!strcmp(cmd, "htop") || !strcmp(cmd, "top")) {
        allmem  = AvailMem(MEMF_ANY);
        chipmem = AvailMem(MEMF_CHIP);
        fastmem = AvailMem(MEMF_FAST);
        add_line("QRX HTOP 68K  TASKS / MEMORY");
        snprintf(buf, sizeof(buf), "Free: %luK  Chip: %luK  Fast: %luK", allmem/1024UL, chipmem/1024UL, fastmem/1024UL);
        add_line(buf);
        execute_capture("Status");
    } else if (!strcmp(cmd, "clear") || !strcmp(cmd, "cls")) {
        clear_lines();
    } else if (!strcmp(cmd, "about")) {
        add_line("QRX native desktop: modern QRX shell on AmigaOS-compatible ABI.");
        add_line("Classic apps and QRX apps are intended to coexist.");
    } else if (!strcmp(cmd, "looks") || !strcmp(cmd, "look")) {
        snprintf(buf, sizeof(buf), "Current Look & Feel: %s", look_name(g_look));
        add_line(buf);
        add_line("Available: qrx windows macos amigaos");
    } else if (!strncmp(cmd, "look ", 5)) {
        if (parse_look(cmd + 5, &look)) {
            g_look = look;
            snprintf(buf, sizeof(buf), "Look & Feel -> %s", look_name(g_look));
            add_line(buf);
            apply_look_palette();
            redraw_desktop();
            save_preferences();
        } else {
            add_line("Unknown look. Use: qrx windows macos amigaos");
        }
    } else if (!strcmp(cmd, "prefs")) {
        sync_terminal_geometry();
        snprintf(buf, sizeof(buf), "Prefs: look=%s terminal=%d,%d,%d,%d view=%s",
                 look_key(g_look),
                 (int)g_term_left, (int)g_term_top,
                 (int)g_term_width, (int)g_term_height,
                 g_file_icon_view ? "icons" : "list");
        add_line(buf);
        add_line(PREFS_PATH);
    } else if (!strncmp(cmd, "dos ", 4)) {
        execute_capture(cmd + 4);
    } else if (cmd[0] == '!' && cmd[1]) {
        execute_capture(cmd + 1);
    } else if (!strcmp(cmd, "dir")) {
        execute_capture("List");
    } else if (!strncmp(cmd, "dir ", 4)) {
        snprintf(doscmd, sizeof(doscmd), "List %s", cmd + 4);
        execute_capture(doscmd);
    } else if (!strcmp(cmd, "pwd")) {
        add_line(g_term_cwd);
    } else if (!strcmp(cmd, "cd")) {
        snprintf(g_term_cwd, sizeof(g_term_cwd), "SYS:");
        add_line(g_term_cwd);
    } else if (!strncmp(cmd, "cd ", 3)) {
        char target[FILE_PATH_LEN + 1]; BPTR lk; const char *arg=cmd+3;
        while (*arg==' ') arg++;
        if (!strcmp(arg,"..")) {
            BPTR cur=Lock((STRPTR)g_term_cwd,ACCESS_READ), par=0;
            if(cur){ par=ParentDir(cur); UnLock(cur); }
            if(par){ if(NameFromLock(par,(STRPTR)target,sizeof(target))){strncpy(g_term_cwd,target,FILE_PATH_LEN);g_term_cwd[FILE_PATH_LEN]=0;} UnLock(par); }
        } else {
            qrx_join_path(target,sizeof(target),g_term_cwd,arg);
            lk=Lock((STRPTR)target,ACCESS_READ);
            if(!lk) lk=Lock((STRPTR)arg,ACCESS_READ), strncpy(target,arg,sizeof(target)-1), target[sizeof(target)-1]=0;
            if(lk){ UnLock(lk); strncpy(g_term_cwd,target,FILE_PATH_LEN);g_term_cwd[FILE_PATH_LEN]=0; }
            else { add_line("cd: drawer not found"); return 1; }
        }
        add_line(g_term_cwd);
    } else if (!strcmp(cmd, "ls")) {
        snprintf(doscmd,sizeof(doscmd),"List \"%s\"",g_term_cwd); execute_capture(doscmd);
    } else if (!strncmp(cmd, "ls ", 3)) {
        snprintf(doscmd,sizeof(doscmd),"List %s",cmd+3); execute_capture(doscmd);
    } else if (!strcmp(cmd, "files") || !strcmp(cmd, "drive")) {
        open_filemanager(g_files_path);
        add_line("QRX Drive opened.");
    } else if (!strncmp(cmd, "files ", 6)) {
        if (open_filemanager(cmd + 6))
            add_line("QRX Drive path opened.");
    } else if (!strncmp(cmd, "drive ", 6)) {
        if (open_filemanager(cmd + 6))
            add_line("QRX Drive path opened.");
    } else if (!strcmp(cmd, "view")) {
        g_file_icon_view = !g_file_icon_view;
        g_file_scroll = 0;
        save_preferences();
        qrx_set_status(g_file_icon_view ? "Icon view." : "List view.");
        redraw_filemanager();
        add_line(g_file_icon_view ? "QRX Drive -> icon view." : "QRX Drive -> list view.");
    } else if (!strcmp(cmd, "copy")) {
        if (qrx_clipboard_set(CLIP_COPY, g_file_selected))
            add_line("Selected object copied to QRX clipboard.");
        redraw_filemanager();
    } else if (!strcmp(cmd, "cut")) {
        if (qrx_clipboard_set(CLIP_MOVE, g_file_selected))
            add_line("Selected object cut to QRX clipboard.");
        redraw_filemanager();
    } else if (!strcmp(cmd, "paste")) {
        qrx_paste_clipboard(g_files_path);
        redraw_filemanager();
    } else if (!strncmp(cmd, "rename ", 7)) {
        if (g_file_selected >= 0 && g_file_selected < g_file_count) {
            strncpy(g_rename_text, cmd + 7, sizeof(g_rename_text) - 1);
            g_rename_text[sizeof(g_rename_text) - 1] = 0;
            g_rename_len = strlen(g_rename_text);
            g_rename_active = 1;
            qrx_commit_rename();
            redraw_filemanager();
        } else add_line("Select an object in QRX Drive first.");
    } else if (!strcmp(cmd, "delete")) {
        qrx_delete_selected();
        redraw_filemanager();
        add_line(g_file_status);
    } else if (!strcmp(cmd, "desktop") || !strcmp(cmd, "refreshdesktop")) {
        qrx_scan_desktop();
        redraw_desktop();
        add_line("QRX desktop objects refreshed.");
    } else if (!strcmp(cmd, "windows")) {
        qrx_list_foreign_windows();
        redraw_desktop();
    } else if (!strncmp(cmd, "focus ", 6)) {
        qrx_focus_foreign_window(atoi(cmd + 6));
        redraw_desktop();
    } else if (!strcmp(cmd, "apps")) {
        add_line("QRX native apps:");
        add_line(" writer       -> QRX Writer 68K");
        add_line(" sheet        -> QRX Spreadsheet 68K");
        add_line(" presenter    -> QRX Presenter 68K");
        add_line(" hardware     -> QRX Hardware Manager");
        add_line("Normal Amiga apps still open on the QRX public screen.");
    } else if (!strcmp(cmd, "writer")) {
        launch_native("QRX:C/QRXWriter");
    } else if (!strcmp(cmd, "sheet") || !strcmp(cmd, "spreadsheet")) {
        launch_native("QRX:C/QRXSpreadsheet");
    } else if (!strcmp(cmd, "presenter") || !strcmp(cmd, "present")) {
        launch_native("QRX:C/QRXPresenter");
    } else if (!strcmp(cmd, "web") || !strcmp(cmd, "browser")) {
        launch_native("QRX:C/QRXWeb");
    } else if (!strcmp(cmd, "antivirus") || !strcmp(cmd, "ares")) {
        launch_native("QRX:C/ARESAntivirus");
    } else if (!strcmp(cmd, "hardware")) {
        launch_native("QRX:C/QRXHardware");
    } else if (!strcmp(cmd, "display")) {
        snprintf(buf, sizeof(buf), "%s mode=%08lx %dx%dx%d",
                 g_display_label, g_screen_mode,
                 (int)g_screen_width, (int)g_screen_height,
                 (int)g_screen_depth);
        add_line(buf);
        add_line(g_display_is_rtg ?
                 "Policy: RTG/P96-class enhanced mode selected." :
                 "Policy: AGA fallback selected.");
    } else if (!strcmp(cmd, "usb")) {
        int i;
        add_line(PoseidonBase ? "Poseidon library: available" : "Poseidon library: not detected");
        qrx_refresh_mounted_volumes();
        if (g_mounted_volume_count == 0) {
            add_line("No additional mounted DOS volumes detected.");
        } else {
            for (i = 0; i < g_mounted_volume_count; ++i) {
                snprintf(buf, sizeof(buf), "%s mounted", g_mounted_volumes[i].path);
                add_line(buf);
            }
        }
        add_line("Only volumes already present in the DOS list are shown; no USB0: probing.");
    } else if (!strcmp(cmd, "posix") || !strcmp(cmd, "linux")) {
        execute_capture("QRX:C/QRXPosixTest");
    } else if (!strcmp(cmd, "unix")) {
        execute_capture("QRX:C/QRXUnix help");
    } else if (!strncmp(cmd, "unix ", 5)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXUnix %s", cmd + 5);
        execute_capture(doscmd);
    } else if (!strcmp(cmd, "linuxabi")) {
        execute_capture("QRX:C/QRXLinuxRun --abi");
    } else if (!strncmp(cmd, "linuxrun ", 9)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXLinuxRun %s", cmd + 9);
        execute_capture(doscmd);
    } else if (!strcmp(cmd, "qrx86")) {
        execute_capture("QRX:C/QRX86Run --abi");
    } else if (!strncmp(cmd, "x86run ", 7)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRX86Run %s", cmd + 7);
        execute_capture(doscmd);
    } else if (!strcmp(cmd, "win")) {
        execute_capture("QRX:C/QRXWinRun --wine-status");
    } else if (!strncmp(cmd, "wincheck ", 9)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXWinRun --check \"%s\"", cmd + 9);
        execute_capture(doscmd);
    } else if (!strncmp(cmd, "winrun ", 7)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXWinRun \"%s\"", cmd + 7);
        execute_capture(doscmd);
    } else if (!strncmp(cmd, "wine ", 5)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXWinRun --wine \"%s\"", cmd + 5);
        execute_capture(doscmd);
    } else if (!strncmp(cmd, "x86check ", 9)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRX86Run --check \"%s\"", cmd + 9);
        execute_capture(doscmd);
    } else if (!strcmp(cmd, "wineprobe")) {
        execute_capture("QRX:C/QRXWineBoot --probe");
    } else if (!strcmp(cmd, "wineboot")) {
        execute_capture("QRX:C/QRXWineBoot --boot");
    } else if (!strcmp(cmd, "winedisplay")) {
        execute_capture("QRX:C/QRXWineDisplayTest --probe");
    } else if (!strcmp(cmd, "wineservices")) {
        execute_capture("QRX:C/QRXWineServicesTest");
    } else if (!strcmp(cmd, "dynarec")) {
        execute_capture("QRX:C/QRXDynarecTest");
    } else if (!strcmp(cmd, "winext")) {
        execute_capture("QRX:C/QRXWinExtTest");
    } else if (!strncmp(cmd, "msi ", 4)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXMSI --probe \"%s\"", cmd + 4);
        execute_capture(doscmd);
    } else if (!strcmp(cmd, "winrt")) {
        execute_capture("QRX:C/QRXWinRuntimeTest");
    } else if (!strncmp(cmd, "winscan ", 8)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXWinScan \"%s\"", cmd + 8);
        execute_capture(doscmd);
    } else if (!strncmp(cmd, "winload ", 8)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXWinGuestLoader \"%s\"", cmd + 8);
        execute_capture(doscmd);
    } else if (!strncmp(cmd, "wininit ", 8)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXWinGuestRuntime --init-plan \"%s\"", cmd + 8);
        execute_capture(doscmd);
    } else if (!strncmp(cmd, "winbridge ", 10)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXWinGuestRuntime --init-plan --bridge-dump \"%s\"", cmd + 10);
        execute_capture(doscmd);
    } else if (!strncmp(cmd, "winjit ", 7)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXWinGuestRuntime --emit-handoff T:QRX-winjit.qrxgrt \"%s\"", cmd + 7);
        execute_capture(doscmd);
        execute_capture("QRX:C/QRX86GRTExec --jit=on T:QRX-winjit.qrxgrt");
    } else if (!strncmp(cmd, "wintrace ", 9)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXWinGuestRuntime --emit-handoff T:QRX-wintrace.qrxgrt \"%s\"", cmd + 9);
        execute_capture(doscmd);
        execute_capture("QRX:C/QRX86GRTExec --jit=off --trace --max-steps 100000 T:QRX-wintrace.qrxgrt");
    } else if (!strcmp(cmd, "notepadprobe")) {
        execute_capture("QRX:C/QRXWinRun QRX:Windows/samples/notepad-class-probe.exe");
    } else if (!strcmp(cmd, "reactoscalc")) {
        execute_capture("QRX:C/QRXWinGuestRuntime --init-plan --bridge-dump --emit-handoff T:QRX-calc.qrxgrt QRX:Windows/ReactOS/System32/calc.exe");
        execute_capture("QRX:C/QRX86GRTExec --jit=on T:QRX-calc.qrxgrt");
    } else if (!strcmp(cmd, "reactosnotepad")) {
        execute_capture("QRX:C/QRXWinGuestRuntime --init-plan --bridge-dump --emit-handoff T:QRX-notepad.qrxgrt QRX:Windows/ReactOS/System32/notepad.exe");
        execute_capture("QRX:C/QRX86GRTExec --jit=on T:QRX-notepad.qrxgrt");
    } else if (!strcmp(cmd, "reactosthird") || !strcmp(cmd, "reactosclock")) {
        execute_capture("QRX:C/QRXRunReactOSThird");
    } else if (!strcmp(cmd, "reactossolitaire")) {
        execute_capture("Execute QRX:S/RunReactOSSolitaire");
    } else if (!strcmp(cmd, "reactostest")) {
        execute_capture("Execute QRX:S/TestReactOS3");
    } else if (!strcmp(cmd, "compat")) {
        execute_capture("QRX:C/QRXCompatSuite");
    } else if (!strcmp(cmd, "compat full")) {
        execute_capture("QRX:C/QRXCompatSuite --full");
    } else if (!strncmp(cmd, "profile ", 8)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXCompatDB %s", cmd + 8);
        execute_capture(doscmd);
    } else if (!strncmp(cmd, "elf ", 4)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXELF \"%s\"", cmd + 4);
        execute_capture(doscmd);
    } else if (!strcmp(cmd, "pwd")) {
        execute_capture("QRX:C/QRXUnix pwd");
    } else if (!strcmp(cmd, "uname")) {
        execute_capture("QRX:C/QRXUnix uname");
    } else if (!strcmp(cmd, "socket")) {
        execute_capture("QRX:C/QRXUnix socket");
    } else if (!strncmp(cmd, "ls", 2) && (cmd[2] == 0 || cmd[2] == ' ')) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXUnix %s", cmd);
        execute_capture(doscmd);
    } else if (!strncmp(cmd, "cat ", 4) || !strncmp(cmd, "cp ", 3) ||
               !strncmp(cmd, "mv ", 3) || !strncmp(cmd, "rm ", 3) ||
               !strncmp(cmd, "mkdir ", 6) || !strncmp(cmd, "mmap ", 5)) {
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXUnix %s", cmd);
        execute_capture(doscmd);
    } else if (!strncmp(cmd, "open ", 5)) {
        const char *path = cmd + 5;
        BPTR lock = Lock((STRPTR)path, ACCESS_READ);
        struct FileInfoBlock *fib = NULL;
        LONG dir_type = -1;
        UBYTE has_icon = 0;
        UBYTE icon_type = 0;
        char default_tool[128];

        default_tool[0] = 0;
        if (!lock) {
            add_line("open: object not found.");
        } else {
            fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
            if (fib && Examine(lock, fib))
                dir_type = fib->fib_DirEntryType;
            if (fib) FreeDosObject(DOS_FIB, fib);
            UnLock(lock);

            if (IconBase) {
                struct DiskObject *dobj = GetDiskObject((STRPTR)path);
                if (dobj) {
                    has_icon = 1;
                    icon_type = dobj->do_Type;
                    if (dobj->do_DefaultTool)
                        snprintf(default_tool, sizeof(default_tool),
                                 "%s", dobj->do_DefaultTool);
                    FreeDiskObject(dobj);
                }
            }
            qrx_open_path(path, dir_type, has_icon, icon_type, default_tool);
            if (g_session_action == SESSION_TAKEOVER ||
                g_session_action == SESSION_WHDLOAD)
                return 0;
        }
    } else if (!strncmp(cmd, "takeover ", 9)) {
        if (qrx_schedule_takeover(cmd + 9))
            return 0;
    } else if (!strncmp(cmd, "legacy ", 7) ||
               !strncmp(cmd, "whdload ", 8)) {
        const char *spec = !strncmp(cmd, "legacy ", 7) ? cmd + 7 : cmd + 8;
        char slave[LEGACY_PATH_LEN];
        char data_dir[LEGACY_PATH_LEN];
        char opts[LEGACY_OPT_LEN];
        const char *p = spec;
        const char *start;
        size_t n = 0;

        while (*p == ' ') ++p;
        start = p;
        if (*p == '"') {
            ++start;
            ++p;
            while (*p && *p != '"') ++p;
            n = (size_t)(p - start);
            if (*p == '"') ++p;
        } else {
            while (*p && *p != ' ') ++p;
            n = (size_t)(p - start);
        }
        if (n >= sizeof(slave)) n = sizeof(slave) - 1;
        memcpy(slave, start, n);
        slave[n] = 0;
        while (*p == ' ') ++p;
        snprintf(opts, sizeof(opts), "%s", p);
        qrx_parent_path(slave, data_dir, sizeof(data_dir));
        if (!data_dir[0])
            snprintf(data_dir, sizeof(data_dir), "%s", g_files_path);

        if (qrx_schedule_whdload(slave, data_dir, opts))
            return 0;
    } else if (!strncmp(cmd, "runmode ", 8)) {
        struct QRXLaunchProfile profile;
        qrx_detect_launch_profile(cmd + 8, NULL, &profile);
        snprintf(buf, sizeof(buf), "Run mode: %s",
                 qrx_runmode_name(profile.mode));
        add_line(buf);
        if (profile.slave[0]) {
            snprintf(buf, sizeof(buf), "Slave: %.60s", profile.slave);
            add_line(buf);
        }
    } else if (!strcmp(cmd, "welcome off") || !strcmp(cmd, "welcome on")) {
        g_welcome_autostart = !strcmp(cmd, "welcome on");
        save_preferences();
        add_line(g_welcome_autostart ? "Welcome dialog will open at desktop start." :
                                       "Welcome dialog autostart off (saves a process and a window).");
    } else if (!strncmp(cmd, "i386 ", 5)) {
        /* 22.5.12: Linux i386 ELF programs through qrx-i386 (qrx86core interpreter, no native x86). */
        snprintf(doscmd, sizeof(doscmd), "QRX:C/qrx-i386 %s", cmd + 5);
        execute_capture(doscmd);
    } else if (!strncmp(cmd, "win32 ", 6)) {
        /* 22.5.12: Win32 GUI programs through qrx-win32 (HLE subsystem, own Intuition windows). */
        snprintf(doscmd, sizeof(doscmd), "QRX:C/qrx-win32 %s", cmd + 6);
        launch_native(doscmd);
    } else if (!strcmp(cmd, "rechner")) {
        launch_native("QRX:C/qrx-win32 QRX:Windows/ReactOS/System32/calc.exe");
    } else if (!strcmp(cmd, "minesweeper")) {
        launch_native("QRX:C/qrx-win32 QRX:Windows/ReactOS/System32/winmine.exe");
    } else if (!strcmp(cmd, "uhr")) {
        launch_native("QRX:C/qrx-win32 QRX:Windows/QRX/qrxclock.exe");
    } else if (!strncmp(cmd, "amigacheck ", 11)) {
        /* 22.5.11: classic HUNK check through QRXAmigaRun (AROS LoadSeg + library probe). */
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXAmigaRun CHECK %s", cmd + 11);
        execute_capture(doscmd);
    } else if (!strncmp(cmd, "amigarun ", 9)) {
        /* 22.5.11: synchronous DOS-process start with return code; ASYNC for GUI programs. */
        snprintf(doscmd, sizeof(doscmd), "QRX:C/QRXAmigaRun %s", cmd + 9);
        execute_capture(doscmd);
    } else if (!strncmp(cmd, "run ", 4)) {
        launch_native(cmd + 4);
    } else if (!strcmp(cmd, "ai") || !strcmp(cmd, "chat")) {
        launch_native("QRX:C/QRXAI");
    } else if (!strcmp(cmd, "color")) {
        add_line("Color theme: QRX Dark (fixed in v0.20).");
        add_line("Look & Feel remains independent from color themes.");
    } else if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
        if (has_foreign_windows()) {
            add_line("Close Amiga application windows on the QRX screen first.");
            add_line("QRX keeps the public screen alive while visitor windows exist.");
            return 1;
        }
        save_preferences();
        g_session_action = SESSION_EXIT;
        return 0;

    } else {
        /* Shell compatibility: unknown commands are passed directly to AmigaDOS. */
        execute_capture(cmd);
    }
    return 1;
}

static int desktop_click_look(WORD x, WORD y) {
    int i;
    WORD bx = 392;

    if (y < 4 || y > 18) return 0;

    for (i = 0; i < 4; ++i) {
        if (x >= bx && x <= bx + 54) {
            set_look((enum LookFeel)i);
            return 1;
        }
        bx += 58;
    }
    return 0;
}

static int terminal_close_hit(WORD x, WORD y) {
    WORD w;
    if (!g_terminal) return 0;
    w = g_terminal->Width;

    if (g_look == LOOK_MACOS)
        return (x >= 7 && x <= 20 && y >= 6 && y <= 21);
    if (g_look == LOOK_AMIGAOS)
        return (x >= 3 && x <= 22 && y >= 3 && y <= 22);

    return (x >= w - 30 && x <= w - 5 && y >= 3 && y <= 25);
}

static int terminal_resize_hit(WORD x, WORD y) {
    if (!g_terminal) return 0;
    return x >= g_terminal->Width - RESIZE_GRAB &&
           y >= g_terminal->Height - RESIZE_GRAB;
}

static int terminal_title_hit(WORD x, WORD y) {
    if (y < 1 || y > TITLE_H) return 0;

    if (g_look == LOOK_MACOS && x <= 48) return 0;
    if (g_look == LOOK_AMIGAOS && x <= 24) return 0;
    if ((g_look == LOOK_QRX || g_look == LOOK_WINDOWS) &&
        x >= g_terminal->Width - 32) return 0;

    return 1;
}

static void begin_window_gesture(WORD local_x, WORD local_y, int resize) {
    if (!g_terminal) return;

    g_gesture_abs_x = g_screen ? g_screen->MouseX : g_terminal->LeftEdge + local_x;
    g_gesture_abs_y = g_screen ? g_screen->MouseY : g_terminal->TopEdge + local_y;
    g_gesture_left = g_terminal->LeftEdge;
    g_gesture_top = g_terminal->TopEdge;
    g_gesture_width = g_terminal->Width;
    g_gesture_height = g_terminal->Height;

    g_resizing = resize;
    g_dragging = !resize;
}

static void update_window_gesture(WORD local_x, WORD local_y) {
    LONG abs_x, abs_y, dx, dy;
    LONG target_left, target_top, target_w, target_h;

    if (!g_terminal || (!g_dragging && !g_resizing)) return;

    abs_x = g_screen ? g_screen->MouseX : g_terminal->LeftEdge + local_x;
    abs_y = g_screen ? g_screen->MouseY : g_terminal->TopEdge + local_y;
    dx = abs_x - g_gesture_abs_x;
    dy = abs_y - g_gesture_abs_y;

    if (g_dragging) {
        target_left = g_gesture_left + dx;
        target_top = g_gesture_top + dy;

        if (target_left < 0) target_left = 0;
        if (target_top < 22) target_top = 22;
        if (target_left > g_screen_width - 80) target_left = g_screen_width - 80;
        if (target_top > g_screen_height - 40) target_top = g_screen_height - 40;

        MoveWindow(g_terminal,
                   target_left - g_terminal->LeftEdge,
                   target_top - g_terminal->TopEdge);
    } else {
        target_w = g_gesture_width + dx;
        target_h = g_gesture_height + dy;

        if (target_w < TERM_MIN_W) target_w = TERM_MIN_W;
        if (target_h < TERM_MIN_H) target_h = TERM_MIN_H;
        if (g_terminal->LeftEdge + target_w > g_screen_width)
            target_w = g_screen_width - g_terminal->LeftEdge;
        if (g_terminal->TopEdge + target_h > g_screen_height)
            target_h = g_screen_height - g_terminal->TopEdge;

        SizeWindow(g_terminal,
                   target_w - g_terminal->Width,
                   target_h - g_terminal->Height);
    }
}

static void finish_window_gesture(void) {
    if (g_dragging || g_resizing) {
        g_dragging = 0;
        g_resizing = 0;
        sync_terminal_geometry();
        save_preferences();
        redraw_terminal();
    }
}

static void qrx_cycle_window(void);
static void qrx_workspace_switch(int ws);
static void qrx_expose_toggle(void);

static int handle_terminal_message(struct IntuiMessage *msg) {
    ULONG cls;
    UWORD code;
    WORD mx, my;

    if (!msg) return 1;

    cls = msg->Class;
    code = msg->Code;
    mx = msg->MouseX;
    my = msg->MouseY;
    ReplyMsg((struct Message *)msg);

    if (cls == IDCMP_REFRESHWINDOW) {
        BeginRefresh(g_terminal);
        redraw_terminal();
        EndRefresh(g_terminal, TRUE);
    } else if (cls == IDCMP_NEWSIZE) {
        sync_terminal_geometry();
        redraw_terminal();
    } else if (cls == IDCMP_MOUSEBUTTONS) {
        if (code == SELECTDOWN) {
            if (terminal_close_hit(mx, my)) { WindowToBack(g_terminal); return 1; }

            WindowToFront(g_terminal); ActivateWindow(g_terminal);
            if (terminal_resize_hit(mx, my))
                begin_window_gesture(mx, my, 1);
            else if (terminal_title_hit(mx, my))
                begin_window_gesture(mx, my, 0);
        } else if (code == SELECTUP) {
            finish_window_gesture();
        }
    } else if (cls == IDCMP_MOUSEMOVE) {
        update_window_gesture(mx, my);
    } else if (cls == IDCMP_RAWKEY) {
        if ((msg->Qualifier & IEQUALIFIER_LALT) && code == 0x42) { qrx_cycle_window(); return 1; }
        if ((msg->Qualifier & IEQUALIFIER_LALT) && code >= 0x01 && code <= 0x04) { qrx_workspace_switch((int)code-1); return 1; /* 22.5.11: was an undefined return value; 0 closed the desktop */ }
        if ((msg->Qualifier & IEQUALIFIER_LALT) && code == 0x12) { qrx_expose_toggle(); return 1; }
        if (code == 0x4c && g_term_hist_count) { /* cursor up */
            if (g_term_hist_pos < g_term_hist_count) g_term_hist_pos++;
            strncpy(input,g_term_history[g_term_hist_pos-1],INPUT_LEN); input[INPUT_LEN]=0; input_len=strlen(input); redraw_terminal();
        } else if (code == 0x4d) { /* cursor down */
            if (g_term_hist_pos > 1) { g_term_hist_pos--; strncpy(input,g_term_history[g_term_hist_pos-1],INPUT_LEN); input[INPUT_LEN]=0; }
            else { g_term_hist_pos=0; input[0]=0; } input_len=strlen(input); redraw_terminal();
        } else if (code == 0x42) { /* TAB */
            qrx_complete();
            redraw_terminal_input();
        }
    } else if (cls == IDCMP_VANILLAKEY) {
        if (code == 13 || code == 10) {
            input[input_len] = 0;
            if (input_len) {
                int hi; if (g_term_hist_count < TERM_HISTORY) g_term_hist_count++;
                for (hi=g_term_hist_count-1; hi>0; --hi) strcpy(g_term_history[hi],g_term_history[hi-1]);
                strncpy(g_term_history[0],input,INPUT_LEN); g_term_history[0][INPUT_LEN]=0; g_term_hist_pos=0;
            }
            if (!execute_command(input)) return 0;
            input_len = 0;
            input[0] = 0;
            redraw_terminal();
        } else if (code == 8 || code == 127) {
            if (input_len > 0) input[--input_len] = 0;
            redraw_terminal_input();
        } else if (code == 27) {
            finish_window_gesture();
        } else if (code >= 32 && code <= 126 && input_len < INPUT_LEN) {
            input[input_len++] = (char)code;
            input[input_len] = 0;
            redraw_terminal_input();
        }
    }
    return 1;
}

static int g_ai_hover=0;
static void qrx_draw_ai_float(struct RastPort *rp){WORD x=g_screen_width-54,y=g_screen_height-64;fill(rp,PEN_RED,x,y,x+38,y+38);outline(rp,g_ai_hover?PEN_WHITE:PEN_EDGE,x,y,x+38,y+38);SetAPen(rp,PEN_WHITE);Move(rp,x+9,y+15);Draw(rp,x+15,y+15);Move(rp,x+23,y+15);Draw(rp,x+29,y+15);text_at(rp,PEN_WHITE,x+12,y+31,"AI");if(g_ai_hover){fill(rp,PEN_PANEL,x-68,y+8,x-4,y+31);outline(rp,PEN_EDGE,x-68,y+8,x-4,y+31);text_at(rp,PEN_TEXT,x-59,y+24,"QRXAI");}}
static int qrx_ai_float_hit(WORD x,WORD y){WORD ax=g_screen_width-54,ay=g_screen_height-64;return x>=ax&&x<=ax+38&&y>=ay&&y<=ay+38;}
static int qrx_top_menu_click(WORD x,WORD y){if(y>22)return 0;if(x>=8&&x<=58){launch_native("QRX:C/QRXWelcome");return 1;}if(x>=62&&x<=118){open_filemanager(g_files_path);return 1;}if(x>=122&&x<=194){WindowToFront(g_terminal);ActivateWindow(g_terminal);return 1;}if(x>=198&&x<=250){launch_native("QRX:C/QRXWelcome");return 1;}return desktop_click_look(x,y);}

static int g_workspace=0; static int g_expose=0;
static void qrx_cycle_window(void){struct Window*a[3];int n=0,i;a[n++]=g_terminal;if(g_files)a[n++]=g_files;for(i=0;i<n;i++){if(a[i]&&a[i]!=g_desktop){WindowToFront(a[i]);ActivateWindow(a[i]);break;}}}
static void qrx_workspace_switch(int ws){if(ws<0||ws>3)return;g_workspace=ws;qrx_set_status(ws==0?"Workspace 1":ws==1?"Workspace 2":ws==2?"Workspace 3":"Workspace 4");redraw_desktop();}
static void qrx_expose_toggle(void){g_expose=!g_expose;if(g_expose){if(g_terminal){MoveWindow(g_terminal,20-g_terminal->LeftEdge,45-g_terminal->TopEdge);SizeWindow(g_terminal,(g_screen_width/2-30)-g_terminal->Width,(g_screen_height/2-60)-g_terminal->Height);}if(g_files){MoveWindow(g_files,g_screen_width/2-g_files->LeftEdge,45-g_files->TopEdge);SizeWindow(g_files,(g_screen_width/2-20)-g_files->Width,(g_screen_height/2-60)-g_files->Height);}}redraw_desktop();}
static void handle_desktop_message(struct IntuiMessage *msg) {
    ULONG cls;UWORD code;WORD mx,my;int nh;if(!msg)return;cls=msg->Class;code=msg->Code;mx=msg->MouseX;my=msg->MouseY;ReplyMsg((struct Message*)msg);
    if(cls==IDCMP_REFRESHWINDOW){BeginRefresh(g_desktop);redraw_desktop();EndRefresh(g_desktop,TRUE);return;}
    if(cls==IDCMP_MOUSEMOVE){nh=qrx_ai_float_hit(mx,my);if(nh!=g_ai_hover){g_ai_hover=nh;qrx_draw_ai_float(g_desktop->RPort);}return;}
    if(cls!=IDCMP_MOUSEBUTTONS||code!=SELECTDOWN)return;
    if(qrx_ai_float_hit(mx,my)){launch_native("QRX:C/QRXAI");return;} if(qrx_top_menu_click(mx,my))return; if(qrx_windows_start_click(mx,my))return;
    if(desktop_click_dock(mx,my))return; if(desktop_click_productivity(mx,my))return; if(qrx_desktop_object_click(mx,my))return; if(desktop_click_foreign_window(mx,my))return;
    if(desktop_click_files_launcher(mx,my)){open_filemanager(g_files_path);return;}
}
static void qrx_save_host_palette(void) {
    ULONG count;
    if (!g_screen || !g_screen_borrowed || !g_screen->ViewPort.ColorMap) return;
    count = g_screen_depth >= 8 ? 256UL : (1UL << (g_screen_depth ? g_screen_depth : 1));
    if (count > 256UL) count = 256UL;
    GetRGB32(g_screen->ViewPort.ColorMap, 0, count, g_host_palette);
    g_host_palette_count = count;
    g_host_palette_saved = 1;
}

static void qrx_restore_host_palette(void) {
    ULONG i;
    if (!g_screen || !g_host_palette_saved) return;
    for (i = 0; i < g_host_palette_count; ++i) {
        SetRGB32(&g_screen->ViewPort, i,
                 g_host_palette[i * 3],
                 g_host_palette[i * 3 + 1],
                 g_host_palette[i * 3 + 2]);
    }
    g_host_palette_saved = 0;
    g_host_palette_count = 0;
}

static int qrx_adopt_public_screen(struct Screen *host) {
    ULONG host_depth;
    if (!host) return 0;
    host_depth = GetBitMapAttr(host->RastPort.BitMap, BMA_DEPTH);
    g_screen = host;
    g_screen_borrowed = 1;
    g_screen_mode = GetVPModeID(&host->ViewPort);
    g_screen_width = host->Width;
    g_screen_height = host->Height;
    g_screen_depth = (UBYTE)(host_depth ? host_depth : 1);
    g_display_is_rtg =
        (g_screen_width > QRX_W ||
         g_screen_height > QRX_H ||
         g_screen_depth > QRX_DEPTH);
    snprintf(g_display_label, sizeof(g_display_label),
             "AROS public host %dx%dx%d",
             (int)g_screen_width, (int)g_screen_height,
             (int)g_screen_depth);
    g_host_title_was_visible = (host->Flags & SHOWTITLE) ? 1 : 0;
    qrx_save_host_palette();
    apply_look_palette();
    if (g_host_title_was_visible) ShowTitle(host, FALSE);
    PutStr("QRX: clean host mode on existing AROS public screen.\n");
    return 1;
}

static void qrx_clamp_runtime_geometry(void) {
    if (g_screen_borrowed && (g_screen_width <= 700 || g_screen_height <= 550)) {
        g_term_left = 12;
        g_term_top = 34;
        g_term_width = g_screen_width - 24;
        g_term_height = g_screen_height - 46;
        g_files_left = 12;
        g_files_top = 34;
        g_files_width = g_screen_width - 24;
        g_files_height = g_screen_height - 46;
    }

    if (g_term_width > g_screen_width)
        g_term_width = g_screen_width;
    if (g_term_height > g_screen_height - 22)
        g_term_height = g_screen_height - 22;
    if (g_term_width < TERM_MIN_W)
        g_term_width = TERM_MIN_W;
    if (g_term_height < TERM_MIN_H)
        g_term_height = TERM_MIN_H;
    if (g_term_left < 0) g_term_left = 0;
    if (g_term_top < 22) g_term_top = 22;
    if (g_term_left + g_term_width > g_screen_width)
        g_term_left = g_screen_width - g_term_width;
    if (g_term_top + g_term_height > g_screen_height)
        g_term_top = g_screen_height - g_term_height;
    if (g_term_left < 0) g_term_left = 0;
    if (g_term_top < 22) g_term_top = 22;

    if (g_files_width > g_screen_width)
        g_files_width = g_screen_width;
    if (g_files_height > g_screen_height - 22)
        g_files_height = g_screen_height - 22;
    if (g_files_width < FILE_MIN_W)
        g_files_width = FILE_MIN_W;
    if (g_files_height < FILE_MIN_H)
        g_files_height = FILE_MIN_H;
    if (g_files_left < 0) g_files_left = 0;
    if (g_files_top < 22) g_files_top = 22;
    if (g_files_left + g_files_width > g_screen_width)
        g_files_left = g_screen_width - g_files_width;
    if (g_files_top + g_files_height > g_screen_height)
        g_files_top = g_screen_height - g_files_height;
    if (g_files_left < 0) g_files_left = 0;
    if (g_files_top < 22) g_files_top = 22;
}

static int run_desktop(void) {
    struct IntuiMessage *msg;
    ULONG signals, term_sig, desk_sig, files_sig, wait_mask;
    int running = 1;

    qrx_clamp_runtime_geometry();

    g_desktop = OpenWindowTags(NULL,
        (g_screen_borrowed ? WA_PubScreen : WA_CustomScreen), (ULONG)g_screen,
        WA_Left, 0,
        WA_Top, 0,
        WA_Width, g_screen_width,
        WA_Height, g_screen_height,
        WA_Flags, (g_screen_borrowed ? 0 : WFLG_BACKDROP) |
                  WFLG_BORDERLESS | WFLG_SMART_REFRESH | WFLG_RMBTRAP | WFLG_ACTIVATE | WFLG_REPORTMOUSE,
        WA_IDCMP, IDCMP_REFRESHWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE,
        TAG_DONE);
    if (!g_desktop) return 0;
    if (g_screen_borrowed) WindowToFront(g_desktop);

    g_terminal = OpenWindowTags(NULL,
        (g_screen_borrowed ? WA_PubScreen : WA_CustomScreen), (ULONG)g_screen,
        WA_Left, g_term_left,
        WA_Top, g_term_top,
        WA_Width, g_term_width,
        WA_Height, g_term_height,
        WA_MinWidth, TERM_MIN_W,
        WA_MinHeight, TERM_MIN_H,
        WA_MaxWidth, g_screen_width,
        WA_MaxHeight, g_screen_height,
        WA_Flags, WFLG_ACTIVATE | WFLG_BORDERLESS | WFLG_SMART_REFRESH |
                  WFLG_RMBTRAP | WFLG_REPORTMOUSE,
        WA_IDCMP, IDCMP_VANILLAKEY | IDCMP_RAWKEY | IDCMP_REFRESHWINDOW |
                  IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_NEWSIZE,
        TAG_DONE);
    if (!g_terminal) {
        CloseWindow(g_desktop);
        g_desktop = NULL;
        return 0;
    }

    qrx_scan_desktop();

    clear_lines();
    add_line("QRX native desktop v0.20 online.");
    add_line("Drag the title bar; resize from the lower-right corner.");
    add_line("AmigaDOS: 'dos List SYS:' or shorthand '!List SYS:'.");
    add_line("Native app: 'run SYS:Tools/Calculator'.");
    add_line("QRX Drive: list/icon view, clipboard ops, rename/delete, drag/drop.");
    add_line("Desktop: SYS:/QRX:/RAM: + QRX:Desktop objects.");
    {
        char bootline[TERM_LINE_LEN + 1];
        snprintf(bootline, sizeof(bootline), "Display: %s", g_display_label);
        add_line(bootline);
    }
    add_line(PoseidonBase ? "USB: Poseidon library available." : "USB: Poseidon library not detected.");
    add_line("Apps: Writer / Spreadsheet / Presenter / Hardware Manager.");
    add_line("Legacy: NORMAL / TAKEOVER / WHDLOAD; no nested UAE on real hardware.");
    add_line("Linux ABI: direct static/freestanding ELF32/m68k subset + POSIX source ports.");
    if (g_resume_notice[0]) {
        add_line(g_resume_notice);
        g_resume_notice[0] = 0;
    }
    redraw_all();
    if (g_welcome_autostart) launch_native("QRX:C/QRXWelcome");

    term_sig = 1UL << g_terminal->UserPort->mp_SigBit;
    desk_sig = 1UL << g_desktop->UserPort->mp_SigBit;

    while (running) {
        files_sig = g_files ? (1UL << g_files->UserPort->mp_SigBit) : 0;
        wait_mask = term_sig | desk_sig | files_sig;
        signals = Wait(wait_mask);

        if (g_files && files_sig && (signals & files_sig)) {
            while (g_files &&
                   (msg = (struct IntuiMessage *)GetMsg(g_files->UserPort)) != NULL)
                handle_filemanager_message(msg);
        }

        if (signals & desk_sig) {
            while ((msg = (struct IntuiMessage *)GetMsg(g_desktop->UserPort)) != NULL)
                handle_desktop_message(msg);
        }

        if (signals & term_sig) {
            while ((msg = (struct IntuiMessage *)GetMsg(g_terminal->UserPort)) != NULL) {
                if (!handle_terminal_message(msg)) {
                    running = 0;
                    break;
                }
            }
        }

        if (g_session_action == SESSION_TAKEOVER ||
            g_session_action == SESSION_WHDLOAD ||
            g_session_action == SESSION_EXIT)
            running = 0;
    }

    save_preferences();

    if (g_files) {
        CloseWindow(g_files);
        g_files = NULL;
    }

    CloseWindow(g_terminal);
    g_terminal = NULL;
    CloseWindow(g_desktop);
    g_desktop = NULL;
    return 1;
}

static int qrx_open_runtime_screen(void) {
    struct QRXDisplayCandidate {
        WORD width;
        WORD height;
        UBYTE depth;
        const char *label;
        UBYTE enhanced;
    };
    static const struct QRXDisplayCandidate candidates[] = {
        {1024, 768, 24, "RTG 1024x768x24", 1},
        {1024, 768, 16, "RTG 1024x768x16", 1},
        { 800, 600, 16, "RTG 800x600x16",  1},
        { 800, 600,  8, "RTG 800x600x8",   1},
        { 640, 512,  8, "AGA 640x512x8",   0}
    };
    int i;

    g_screen = NULL;
    g_screen_borrowed = 0;
    g_screen_mode = INVALID_ID;
    g_screen_width = QRX_W;
    g_screen_height = QRX_H;
    g_screen_depth = QRX_DEPTH;
    g_display_is_rtg = 0;
    snprintf(g_display_label, sizeof(g_display_label), "AGA 640x512x8");
    g_host_palette_saved = 0;
    g_host_palette_count = 0;
    g_host_title_was_visible = 0;

    {
        /* 22.5.11: ENV:QRXSCREEN=rtg (or own) opens QRX's own RTG/AGA screen instead of
         * borrowing the default public screen.  Default (unset) is unchanged: borrowing
         * costs no extra display memory and keeps the proven requester-free host. */
        char want[16];
        LONG n = GetVar((STRPTR)"QRXSCREEN", (STRPTR)want, sizeof(want), 0);
        int own = n > 0 && (!strcmp(want, "rtg") || !strcmp(want, "RTG") ||
                            !strcmp(want, "own") || !strcmp(want, "OWN"));
        if (!own) {
            struct Screen *host = LockPubScreen(NULL);
            if (host) qrx_adopt_public_screen(host);
        }
    }

    for (i = 0; !g_screen && i < (int)(sizeof(candidates) / sizeof(candidates[0])); ++i) {
        ULONG mode;
        ULONG depth;
        const struct QRXDisplayCandidate *c = &candidates[i];

        mode = BestModeID(
            BIDTAG_NominalWidth, c->width,
            BIDTAG_NominalHeight, c->height,
            BIDTAG_Depth, c->depth,
            TAG_DONE);

        if (mode == INVALID_ID)
            continue;

        g_screen = OpenScreenTags(NULL,
            SA_DisplayID, mode,
            SA_Width, c->width,
            SA_Height, c->height,
            SA_Depth, c->depth,
            SA_Type, CUSTOMSCREEN,
            SA_Title, (ULONG)"QRX OS One Amiga 68K v0.20",
            SA_PubName, (ULONG)"QRX",
            SA_ShowTitle, FALSE,
            SA_Quiet, TRUE,
            TAG_DONE);

        if (!g_screen)
            continue;

        depth = GetBitMapAttr(g_screen->RastPort.BitMap, BMA_DEPTH);

        /*
         * BestModeID may legally return a "best effort" lower mode.
         * Reject that for RTG candidates so AGA is never mislabeled as RTG.
         */
        if (c->enhanced &&
            (g_screen->Width < c->width ||
             g_screen->Height < c->height ||
             depth < c->depth)) {
            CloseScreen(g_screen);
            g_screen = NULL;
            continue;
        }

        g_screen_mode = mode;
        g_screen_width = g_screen->Width;
        g_screen_height = g_screen->Height;
        g_screen_depth = (UBYTE)depth;
        g_display_is_rtg =
            (c->enhanced ||
             g_screen_width > QRX_W ||
             g_screen_height > QRX_H ||
             g_screen_depth > QRX_DEPTH);

        snprintf(g_display_label, sizeof(g_display_label), "%s", c->label);
        break;
    }

    /* QRX v0.20.3.7.4.6.5.7 AROS display fallback */
    if (!g_screen) {
        struct Screen *active = LockPubScreen(NULL);
        if (active) {
            ULONG active_mode = GetVPModeID(&active->ViewPort);
            WORD active_width = active->Width;
            WORD active_height = active->Height;
            ULONG active_depth = GetBitMapAttr(active->RastPort.BitMap, BMA_DEPTH);
            ULONG requested_depth = active_depth;

            UnlockPubScreen(NULL, active);

            if (active_mode != INVALID_ID &&
                active_width > 0 && active_height > 0 && active_depth > 0) {
                g_screen = OpenScreenTags(NULL,
                    SA_DisplayID, active_mode,
                    SA_Width, active_width,
                    SA_Height, active_height,
                    SA_Depth, requested_depth,
                    SA_Type, CUSTOMSCREEN,
                    SA_Title, (ULONG)"QRX OS One Amiga 68K v0.20",
                    SA_PubName, (ULONG)"QRX",
                    SA_ShowTitle, FALSE,
                    SA_Quiet, TRUE,
                    TAG_DONE);

                if (g_screen) {
                    ULONG depth = GetBitMapAttr(g_screen->RastPort.BitMap, BMA_DEPTH);
                    if (g_screen) {
                    g_screen_mode = GetVPModeID(&g_screen->ViewPort);
                    g_screen_width = g_screen->Width;
                    g_screen_height = g_screen->Height;
                    g_screen_depth = (UBYTE)depth;
                    g_display_is_rtg =
                        (g_screen_width > QRX_W ||
                         g_screen_height > QRX_H ||
                         g_screen_depth > QRX_DEPTH);
                    snprintf(g_display_label, sizeof(g_display_label),
                             "AROS current %dx%dx%d",
                             (int)g_screen_width, (int)g_screen_height,
                             (int)g_screen_depth);
                    PutStr("QRX: using current AROS/Wanderer display mode fallback.\n");
                    }
                }
            }
        }
    }

    if (!g_screen) {
        g_screen = OpenScreenTags(NULL,
            SA_LikeWorkbench, TRUE,
            SA_Type, CUSTOMSCREEN,
            SA_Title, (ULONG)"QRX OS One Amiga 68K v0.20",
            SA_PubName, (ULONG)"QRX",
            SA_ShowTitle, FALSE,
            SA_Quiet, TRUE,
            TAG_DONE);

        if (g_screen) {
            ULONG depth = GetBitMapAttr(g_screen->RastPort.BitMap, BMA_DEPTH);
            if (g_screen) {
            g_screen_mode = GetVPModeID(&g_screen->ViewPort);
            g_screen_width = g_screen->Width;
            g_screen_height = g_screen->Height;
            g_screen_depth = (UBYTE)depth;
            g_display_is_rtg =
                (g_screen_width > QRX_W ||
                 g_screen_height > QRX_H ||
                 g_screen_depth > QRX_DEPTH);
            snprintf(g_display_label, sizeof(g_display_label),
                     "AROS Workbench-like %dx%dx%d",
                     (int)g_screen_width, (int)g_screen_height,
                     (int)g_screen_depth);
            PutStr("QRX: using SA_LikeWorkbench display fallback.\n");
            }
        }
    }

    if (!g_screen) {
        struct Screen *host = LockPubScreen(NULL);
        if (host) qrx_adopt_public_screen(host);
    }

    if (!g_screen) {
        PutStr("QRX: no usable RTG, AGA, Workbench-like, or public host screen found.\n");
        return 0;
    }

    if (!g_screen_borrowed) {
        PubScreenStatus(g_screen, 0);
        SetDefaultPubScreen((UBYTE *)"QRX");
        draw_wallpaper(g_screen);
        apply_look_palette();
    }
    ScreenToFront(g_screen);
    return 1;
}

static void qrx_close_runtime_screen(void) {
    finish_window_gesture();

    if (g_files) {
        CloseWindow(g_files);
        g_files = NULL;
    }
    if (g_terminal) {
        CloseWindow(g_terminal);
        g_terminal = NULL;
    }
    if (g_desktop) {
        CloseWindow(g_desktop);
        g_desktop = NULL;
    }

    if (g_screen) {
        if (g_screen_borrowed) {
            if (g_host_title_was_visible) ShowTitle(g_screen, TRUE);
            qrx_restore_host_palette();
            UnlockPubScreen(NULL, g_screen);
            g_host_title_was_visible = 0;
        } else {
            SetDefaultPubScreen(NULL);
            PubScreenStatus(g_screen, PSNF_PRIVATE);
            qrx_free_wallpaper_cache();
            CloseScreen(g_screen);
        }
        g_screen = NULL;
        g_screen_borrowed = 0;
    }
}

int main(void) {
    int rc = 20;
    int keep_running = 1;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 39);
    IconBase = OpenLibrary("icon.library", 37);
    PoseidonBase = OpenLibrary("poseidon.library", 0);

    if (!IntuitionBase || !GfxBase || !DOSBase)
        goto cleanup;

    qrx_disable_dos_requesters();
    load_preferences();
    qrx_init_posix_environment();
    rc = 0;

    while (keep_running) {
        int action;

        g_session_action = SESSION_NONE;

        if (!qrx_open_runtime_screen()) {
            rc = 20;
            break;
        }

        run_desktop();

        action = g_session_action;
        qrx_close_runtime_screen();

        if (action == SESSION_TAKEOVER) {
            qrx_execute_takeover_session();
            continue;
        }

        if (action == SESSION_WHDLOAD) {
            qrx_execute_whdload_session();
            continue;
        }

        keep_running = 0;
    }

cleanup:
    qrx_close_runtime_screen();
    qrx_restore_dos_requesters();

    if (PoseidonBase) CloseLibrary(PoseidonBase);
    if (IconBase) CloseLibrary(IconBase);
    if (DOSBase) CloseLibrary((struct Library *)DOSBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
