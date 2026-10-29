/* QRX OS One 22.5.12 - qrx-win32: Win32 user-mode HLE on qrx86core.
 *
 * Runs unmodified 32-bit Windows GUI programs (PE/i386) without any Microsoft
 * or ReactOS DLL: the program's own code is interpreted by qrx86core, every
 * imported API function is a high-level-emulated host function (Wine-like
 * approach).  Windows, dialogs, controls and GDI drawing are implemented here
 * on a 32-bit software framebuffer per top-level window; a backend presents
 * the framebuffers (headless PPM renderer for tests, AROS Intuition/RTG on
 * the Amiga) and delivers input.
 *
 * Only the API surface needed by the target programs is implemented; every
 * unimplemented import is reported by name the moment it is called. */
#ifndef QRXWIN_H
#define QRXWIN_H
#include <stdint.h>
#include <stdio.h>
#include "../core/qrx86core.h"

typedef uint16_t wchar16;

/* ------------------------------------------------------------------ guest layout */
#define QW_THUNK_BASE   0x7F000000u   /* HLE thunks: never mapped */
#define QW_THUNK_MAX    4096u
#define QW_DATA_BASE    0x00010000u   /* HLE data exports (_iob, _wcmdln, ...) */
#define QW_DATA_SIZE    0x00010000u
#define QW_STACK_TOP    0x00300000u
#define QW_STACK_SIZE   0x00100000u
#define QW_HEAP_BASE    0x10000000u
#define QW_HEAP_LIMIT   0x30000000u
#define QW_TEB          0x7FFDE000u
#define QW_PEB          0x7FFD0000u

/* ------------------------------------------------------------------ GDI */
typedef struct qsurf {               /* 32-bit 0x00RRGGBB pixels */
    uint32_t *pix;
    int w, h;
} qsurf;

enum { GO_FREE = 0, GO_PEN, GO_BRUSH, GO_FONT, GO_BITMAP, GO_DC, GO_RGN, GO_ICON, GO_CURSOR, GO_MENU_UNUSED, GO_PALETTE };

typedef struct qobj {
    int type, stock;
    uint32_t color;                   /* pen/brush colour (COLORREF) */
    int style, width;                 /* pen style / brush style (0 solid, 1 null) */
    int font;                         /* index in qfont_table */
    qsurf bmp;                        /* bitmap pixels */
    int mono;                         /* 1-bpp bitmap / brush: 0x000000 -> text colour, 0xFFFFFF -> background colour */
    int x, y, w, h;                   /* rect region / icon size */
    void *dc;                         /* GO_DC: struct qdc */
} qobj;

#define QW_MAXEXCL 128
typedef struct qdc {
    struct qwin *w;                   /* owner HLE instance */
    uint32_t win;                     /* window handle (0 = memory DC) */
    qsurf *s;                         /* target surface */
    int ox, oy;                       /* origin of the DC in surface coords */
    int cx0, cy0, cx1, cy1;           /* clip rectangle in surface coords */
    uint32_t pen, brush, font, bitmap;
    uint32_t textcol, bkcol;
    int bkmode, rop2, ta;             /* bkmode 1 TRANSPARENT 2 OPAQUE */
    int px, py;                       /* current position */
    int vox, voy;                     /* SetViewportOrgEx */
    int bx0, by0, bx1, by1;           /* clip without IntersectClipRect/SelectClipRgn (surface coords) */
    int nex, ex[QW_MAXEXCL][4];       /* ExcludeClipRect rectangles (surface coords) */
    int brx, bry;                     /* SetBrushOrgEx */
    uint32_t pal;                     /* selected palette (32-bit surfaces: no effect) */
    struct qdc *saved[8];             /* SaveDC stack (copies of this struct) */
    int nsaved;
    qsurf membmp;                     /* default 1x1 bitmap of a memory DC */
} qdc;

/* ------------------------------------------------------------------ USER */
typedef struct qmenuitem {
    uint32_t id, flags;               /* MF_* */
    wchar16 text[64];
    uint32_t sub;                     /* popup menu handle */
} qmenuitem;

typedef struct qmenu {
    int used;
    int n;
    qmenuitem it[48];
} qmenu;

typedef struct qclass {
    int used;
    wchar16 name[64];
    uint32_t atom, style, wndproc, cbcls, cbwnd, hinst, icon, cursor, brush;
    wchar16 menuname[64];
    uint32_t menuid;
    int builtin;                      /* 1 BUTTON 2 STATIC 3 EDIT 4 DIALOG 5 LISTBOX 6 SCROLLBAR */
} qclass;

typedef struct qwnd {
    int used;
    uint32_t h;                       /* handle */
    qclass *cls;
    uint32_t parent, owner, style, exstyle, id, menu, wndproc, userdata, hinst;
    int x, y, cw, ch;                 /* client rect: x/y relative to parent client (screen for top-level) */
    int nl, nt, nr, nb;               /* non-client margins (frame/caption/menu), top-level only */
    wchar16 text[256];
    uint8_t *extra;                   /* cbWndExtra bytes */
    int nextra;
    int visible, enabled, destroyed;
    int dirty, erase;                 /* needs WM_PAINT / WM_ERASEBKGND */
    int ux0, uy0, ux1, uy1;           /* update region bounds (client coords) */
    /* top-level */
    qsurf surf;
    void *be;                         /* backend window */
    int present;                      /* needs presentation */
    int px0, py0, px1, py1;           /* area to present (surface coords), valid while present */
    /* controls */
    int state;                        /* button: checked/pushed bits */
    int hover;
    uint32_t font;
    uint32_t dlgproc;                 /* dialogs: guest DLGPROC */
    int modal_done, modal_result, is_dialog;
    uint32_t focus_child;
    int caret;
    uint32_t tme;                     /* TrackMouseEvent flags */
    uint32_t icon;
    int fs_w, fs_h, fs_bpp;           /* DirectDraw exclusive mode: show on an own screen (backend permitting) */
} qwnd;

typedef struct qmsg { uint32_t hwnd, msg, wp, lp, time; int x, y; } qmsg;

typedef struct qtimer { int used; uint32_t hwnd, id, ms, proc; uint64_t next; } qtimer;

/* ------------------------------------------------------------------ backend */
typedef struct qevent {
    int type;                         /* QE_* */
    uint32_t win;                     /* target top-level handle (0 = active) */
    int x, y, button, vk, ch, id;
    char arg[256];
} qevent;
enum { QE_NONE = 0, QE_MOUSEDOWN, QE_MOUSEUP, QE_MOUSEMOVE, QE_KEYDOWN, QE_KEYUP, QE_CHAR, QE_MENU, QE_CLOSE,
       QE_CLICKID, QE_SNAP, QE_TEXT, QE_EXPECT, QE_QUIT, QE_WAIT, QE_ACTIVATE, QE_MENUBAR };

typedef struct qbackend {
    const char *name;
    int  (*init)(struct qwin *w);
    void (*create)(struct qwin *w, qwnd *top);
    void (*destroy)(struct qwin *w, qwnd *top);
    void (*present)(struct qwin *w, qwnd *top);
    void (*title)(struct qwin *w, qwnd *top);
    void (*menu)(struct qwin *w, qwnd *top);
    /* wait for input: returns 1 with *ev filled, 0 when timeout ms elapsed, -1 = shut down */
    int  (*wait)(struct qwin *w, uint32_t timeout_ms, qevent *ev);
    uint64_t (*now)(struct qwin *w);  /* milliseconds */
    void (*shutdown)(struct qwin *w);
    /* optional: show an 8-bit palettised frame (DirectDraw) directly, without the
     * 32-bit surface; pal = 256 x 0x00RRGGBB.  NULL: the caller converts. */
    void (*present8)(struct qwin *w, qwnd *top, const uint8_t *pix, int pitch, const uint32_t *pal, int width, int height);
    /* optional: re-open TOP's backend window (fs_w/fs_h set = own full-screen display) */
    void (*reopen)(struct qwin *w, qwnd *top);
} qbackend;

/* ------------------------------------------------------------------ instance */
#define QW_MAXWND 512
#define QW_MAXOBJ 2048
#define QW_MAXCLS 64
#define QW_MAXMENU 64
#define QW_MAXQ 512
#define QW_MAXTIMER 32

typedef struct qhle {
    const char *dll, *name;
    int nargs;                        /* stdcall argument count, -1 = cdecl */
    uint32_t (*fn)(struct qwin *w, uint32_t *a);
} qhle;

/* a loaded PE image: the program (mod[0]) or a program-supplied DLL */
#define QW_MAXMOD 16
#define QW_DLL_BASE  0x40000000u      /* relocated DLLs go here */
#define QW_DLL_LIMIT 0x7C000000u
typedef struct qmod {
    char name[64];                    /* lower-case file name, e.g. "cards.dll" */
    uint32_t base, size, entry, res_rva, exp_rva, exp_size;
    int is_dll, inited;
} qmod;

typedef struct qwin {
    x86cpu cpu;
    const qbackend *be;
    void *beh;                        /* backend private */
    FILE *log;
    int trace, verbose;
    uint16_t lang;
    /* image */
    uint32_t image_base, image_size, entry, res_rva, res_size;
    char exe_path[1024];
    qmod mod[QW_MAXMOD];
    int nmod;
    int init_order[QW_MAXMOD], ninit;   /* DLLs in dependency order (DllMain) */
    uint32_t dll_next;
    int started;                      /* entry point running: LoadLibrary calls DllMain at once */
    wchar16 cmdline[512];
    /* thunks */
    const qhle *thunk[QW_THUNK_MAX];
    char thunk_name[QW_THUNK_MAX][48];
    uint32_t nthunk;
    uint32_t data_next;
    /* heap */
    uint32_t heap_top, heap_mapped;
    struct qblk *blocks;              /* allocation map */
    struct qblk *freel;
    /* kernel */
    uint32_t last_error, exit_code;
    int exiting;
    uint32_t unhandled_filter;
    uint32_t onexit[64];
    int nonexit;
    uint32_t rand_seed;
    /* registry (in memory) */
    struct qreg *reg;
    /* USER */
    qwnd wnd[QW_MAXWND];
    qclass cls[QW_MAXCLS];
    qmenu menu[QW_MAXMENU];
    qobj obj[QW_MAXOBJ];
    qmsg q[QW_MAXQ];
    int qh, qt;
    qtimer timer[QW_MAXTIMER];
    uint32_t focus, capture, active, main_wnd;
    int quit_posted, quit_code;
    int mouse_x, mouse_y;             /* last mouse position, screen coords */
    uint8_t keystate[256];
    uint32_t clip_fmt, clip_data;     /* clipboard */
    uint32_t host_wndproc[8];         /* thunk addresses of builtin window procs */
    uint32_t defwndproc_thunk, defdlgproc_thunk;
    int modal_depth;
    /* script/test bookkeeping */
    int expect_fail, expect_ok;
    char outdir[512];
    int menu_bar_h, caption_h, frame_w;
    int draw_nc;                      /* headless: draw a caption/menu bar into snapshots */
    uint32_t menu_popup, menu_owner;  /* open popup menu window and its owner */
    int menu_open_index;              /* menu bar item whose popup is open (-1 none) */
    char grab_path[256];              /* -G: capture the display after grab_at DirectDraw frames */
    int grab_at, frames;
} qwin;

/* hle.c */
int  qw_load(qwin *w, const char *path, int argc, char **argv);
int  qw_run(qwin *w);
uint32_t qw_thunk(qwin *w, const char *dll, const char *name);
uint32_t qw_alloc(qwin *w, uint32_t n);
void qw_free(qwin *w, uint32_t a);
uint32_t qw_size(qwin *w, uint32_t a);
uint32_t qw_data(qwin *w, uint32_t n);
void qw_log(qwin *w, const char *fmt, ...);
int  qw_rd_wstr(qwin *w, uint32_t a, wchar16 *out, int max);
int  qw_rd_str(qwin *w, uint32_t a, char *out, int max);
int  qw_wr_wstr(qwin *w, uint32_t a, const wchar16 *s, int max);   /* returns chars written (no NUL) */
int  wlen16(const wchar16 *s);
void wcpy16(wchar16 *d, const wchar16 *s, int max);
void a2w(wchar16 *d, const char *s, int max);
void w2a(char *d, const wchar16 *s, int max);
int  wicmp16(const wchar16 *a, const wchar16 *b);
uint32_t qw_call(qwin *w, uint32_t fn, int n, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
const qhle *qw_find_hle(const char *dll, const char *name);

/* pe.c */
int  pe_load(qwin *w, const uint8_t *b, size_t n);
/* program-supplied DLLs (searched next to the program, case-insensitive) */
qmod *pe_find_module(qwin *w, uint32_t hmod);            /* by handle (= base) */
qmod *pe_module_by_name(qwin *w, const char *name);
qmod *pe_load_dll(qwin *w, const char *name);            /* load or return loaded; NULL if absent */
uint32_t pe_export(qwin *w, qmod *m, const char *name, uint32_t ordinal);   /* 0 if missing */
void pe_init_dlls(qwin *w);                              /* DllMain(PROCESS_ATTACH) of pending DLLs */
int  qw_is_hle_dll(const char *dll);
/* find resource; type/name: id if < 0x10000 else guest pointer to string */
int  res_find(qwin *w, uint32_t type, uint32_t name, uint32_t *data, uint32_t *size);
int  res_find_in(qwin *w, uint32_t hmod, uint32_t type, uint32_t name, uint32_t *data, uint32_t *size);
int  res_find_id(qwin *w, int type, int id, uint32_t *data, uint32_t *size);

/* user.c */
extern const qhle qw_user32[];
void user_init(qwin *w);
qwnd *W(qwin *w, uint32_t h);
uint32_t win_send(qwin *w, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp);
void win_post(qwin *w, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp);
void win_invalidate(qwin *w, qwnd *x, int erase);
void win_abs_origin(qwin *w, qwnd *x, int *ax, int *ay);
qwnd *win_top(qwin *w, qwnd *x);
int  user_pump(qwin *w, int block, qmsg *out, uint32_t filt);
void user_render_all(qwin *w);
int  user_snapshot(qwin *w, const char *path);
qmenu *M(qwin *w, uint32_t h);
int  user_menubar_height(qwin *w, qwnd *top);
void user_render_menubar(qwin *w, qwnd *top, qsurf *s);   /* s: cw x menubar height */
uint32_t menu_load(qwin *w, uint32_t data);

/* gdi.c */
extern const qhle qw_gdi32[];
void gdi_init(qwin *w);
qobj *O(qwin *w, uint32_t h, int type);
uint32_t gdi_new(qwin *w, int type);
uint32_t dc_for_window(qwin *w, qwnd *x, int client);
void dc_release(qwin *w, uint32_t hdc);
qdc *DCP(qwin *w, uint32_t hdc);
void gdi_fill(qdc *d, int x0, int y0, int x1, int y1, uint32_t rgb);   /* logical coords */
void gdi_frame3d(qdc *d, int x0, int y0, int x1, int y1, int sunken, int thick);
int  gdi_text(qdc *d, int x, int y, const wchar16 *s, int n, uint32_t col);
int  gdi_text_width(int font, const wchar16 *s, int n);
int  gdi_font_height(int font);
int  gdi_drawtext(qdc *d, const wchar16 *s, int n, int *rc, uint32_t fmt);
uint32_t sys_color(int idx);
uint32_t qw_bitmap_from_dib(qwin *w, uint32_t dib, uint32_t size);
void surf_resize(qsurf *s, int w, int h);
uint32_t rgb_of(uint32_t colorref);

/* kernel.c (kernel32, msvcrt, advapi32, misc) */
extern const qhle qw_kernel32[];
int qw_format(qwin *w, uint32_t dst, int max, int dwide, uint32_t fmt, int fwide, uint32_t ap);
uint32_t qw_data_import(qwin *w, const char *dll, const char *name);
uint32_t user_message_box(qwin *w, uint32_t owner, const wchar16 *text, const wchar16 *title, uint32_t type);
extern const qhle qw_misc[];
extern const qhle qw_ansi[];   /* ansi.c */
extern const qhle qw_files[];  /* files.c */
extern const qhle qw_dx[];     /* dx.c: DirectDraw/DirectSound/DirectInput */
extern const qhle qw_extra[];  /* extra.c */
void qw_env_set(const char *name, const char *value);
int qw_dinput_ready(qwin *w, uint32_t handles, uint32_t n);   /* dx.c */
void user_input_event(qwin *w, qevent *ev);       /* feed one backend event into the window system */
void qw_dib_register(qwin *w, uint32_t hbm, uint32_t mem, uint32_t bmi, int bw, int bh, int bpp, uint32_t stride);
uint32_t qw_dib_colortable(qwin *w, uint32_t hdc, uint32_t start, uint32_t n, uint32_t rgbq);
const qhle *qw_find_dx(const char *name);   /* COM method thunks "IDirectDraw::CreateSurface" */
FILE *qw_stream_host(qwin *w, uint32_t gfile);    /* host FILE of a guest fopen() stream, NULL otherwise */
int  qw_file_close(qwin *w, uint32_t h);          /* CloseHandle for file/find handles */

/* backends */
extern const qbackend qw_headless;
#ifdef __AROS__
extern const qbackend qw_aros;
#endif
int headless_set_script(qwin *w, const char *path);

/* window messages used internally */
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_GETMINMAXINFO 0x0024
#define WM_DRAWITEM 0x002B
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_NOTIFY 0x004E
#define WM_SETICON 0x0080
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCPAINT 0x0085
#define WM_NCACTIVATE 0x0086
#define WM_GETDLGCODE 0x0087
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_INITMENU 0x0116
#define WM_INITMENUPOPUP 0x0117
#define WM_MENUSELECT 0x011F
#define WM_ENTERIDLE 0x0121
#define WM_CTLCOLORMSGBOX 0x0132
#define WM_CTLCOLOREDIT 0x0133
#define WM_CTLCOLORLISTBOX 0x0134
#define WM_CTLCOLORBTN 0x0135
#define WM_CTLCOLORDLG 0x0136
#define WM_CTLCOLORSCROLLBAR 0x0137
#define WM_CTLCOLORSTATIC 0x0138
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_PARENTNOTIFY 0x0210
#define WM_ENTERMENULOOP 0x0211
#define WM_EXITMENULOOP 0x0212
#define WM_CAPTURECHANGED 0x0215
#define WM_MOUSEHOVER 0x02A1
#define WM_MOUSELEAVE 0x02A3
#define WM_USER 0x0400

#define BM_GETCHECK 0x00F0
#define BM_SETCHECK 0x00F1
#define BM_GETSTATE 0x00F2
#define BM_SETSTATE 0x00F3
#define BM_SETSTYLE 0x00F4
#define BM_CLICK 0x00F5

#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_CAPTION 0x00C00000u
#define WS_BORDER 0x00800000u
#define WS_DLGFRAME 0x00400000u
#define WS_SYSMENU 0x00080000u
#define WS_THICKFRAME 0x00040000u
#define WS_GROUP 0x00020000u
#define WS_TABSTOP 0x00010000u
#define WS_EX_CLIENTEDGE 0x00000200u
#define WS_EX_STATICEDGE 0x00020000u
#define WS_EX_DLGMODALFRAME 0x00000001u

#endif
