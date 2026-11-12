/* QRX OS One 22.5.12 - qrx-win32: ANSI ("...A") entry points of user32/gdi32.
 * Each one copies its string arguments into temporary guest UTF-16 buffers and
 * calls the Unicode implementation, so there is only one implementation of
 * windows, dialogs and drawing.  Text coming back (GetWindowTextA, LoadStringA,
 * GetTextMetricsA ...) is converted to Latin-1.  Messages sent to ANSI window
 * procedures are not translated (programs see UTF-16 in WM_SETTEXT/WM_GETTEXT;
 * the target programs do not use those on their own windows). */
#include <stdlib.h>
#include <string.h>
#include "qrxwin.h"

#define FN(name) static uint32_t name(qwin *w, uint32_t *a)
#define C (&w->cpu)
#define RD32(x) x86_rd32(C, (x))
#define WR32(x, v) x86_wr32(C, (x), (v))

typedef struct { uint32_t p[6]; int n; } tmps;

/* guest UTF-16 copy of the ANSI string at s (n < 0: NUL-terminated); ids/atoms (< 0x10000) pass through */
static uint32_t tw(qwin *w, tmps *t, uint32_t s, int n)
{
    uint32_t g;
    int i;
    if (s < 0x10000u) return s;
    if (n < 0) { n = 0; while (x86_rd8(C, s + (uint32_t)n) && n < 65535) n++; }
    g = qw_alloc(w, (uint32_t)n * 2u + 2u);
    if (!g) return 0;
    for (i = 0; i < n; i++) x86_wr16(C, g + (uint32_t)i * 2u, x86_rd8(C, s + (uint32_t)i));
    x86_wr16(C, g + (uint32_t)n * 2u, 0);
    if (t->n < 6) t->p[t->n++] = g;
    return g;
}
static void tfree(qwin *w, tmps *t) { while (t->n) qw_free(w, t->p[--t->n]); }

/* UTF-16 guest buffer -> ANSI guest buffer (max bytes incl. NUL); returns chars written */
static uint32_t w2a_guest(qwin *w, uint32_t dst, uint32_t src, uint32_t max)
{
    uint32_t i;
    if (!max) return 0;
    for (i = 0; i + 1 < max; i++) {
        uint16_t ch = x86_rd16(C, src + i * 2u);
        if (!ch) break;
        x86_wr8(C, dst + i, ch < 256 ? (uint8_t)ch : '?');
    }
    x86_wr8(C, dst + i, 0);
    return i;
}

static uint32_t callw(qwin *w, const char *dll, const char *name, uint32_t *a)
{
    const qhle *e = qw_find_hle(dll, name);
    if (!e) { qw_log(w, "ansi: %s missing", name); return 0; }
    return e->fn(w, a);
}
#define CALL(dll, name) callw(w, dll, name, b)
#define COPYARGS uint32_t b[16]; tmps t = { {0}, 0 }; memcpy(b, a, sizeof(b))

/* ---------------------------------------------------------------- user32 */
FN(a_RegisterClassExA) {
    uint32_t ex = qw_alloc(w, 48), r, i;
    COPYARGS;
    for (i = 0; i < 48; i += 4) WR32(ex + i, RD32(a[0] + i));
    WR32(ex + 36, tw(w, &t, RD32(a[0] + 36), -1));
    WR32(ex + 40, tw(w, &t, RD32(a[0] + 40), -1));
    b[0] = ex; r = CALL("user32", "RegisterClassExW");
    tfree(w, &t); qw_free(w, ex);
    return r;
}
FN(a_RegisterClassA) {
    uint32_t ex = qw_alloc(w, 48), r, i;
    COPYARGS;
    WR32(ex, 48);
    for (i = 0; i < 40; i += 4) WR32(ex + 4 + i, RD32(a[0] + i));
    WR32(ex + 36, tw(w, &t, RD32(a[0] + 32), -1));
    WR32(ex + 40, tw(w, &t, RD32(a[0] + 36), -1));
    WR32(ex + 44, 0);
    b[0] = ex; r = CALL("user32", "RegisterClassExW");
    tfree(w, &t); qw_free(w, ex);
    return r;
}
FN(a_UnregisterClassA) { (void)w; (void)a; return 1; }
FN(a_CreateWindowExA) {
    uint32_t r;
    COPYARGS;
    b[1] = tw(w, &t, a[1], -1); b[2] = tw(w, &t, a[2], -1);
    r = CALL("user32", "CreateWindowExW");
    tfree(w, &t);
    return r;
}
/* text messages: ANSI <-> UTF-16 around the Unicode implementation */
/* A text message already bridged to wide can pass through a program's ANSI window procedure
 * and come back here in its DefWindowProcA: that buffer is wide and must stay so. */
static uint32_t g_wide_buf;
static uint32_t text_msg(qwin *w, const char *fn, uint32_t *a, int nargs_before)
{
    uint32_t r, msg = a[nargs_before + 1], outer = g_wide_buf;
    COPYARGS;
    if ((msg == WM_SETTEXT || msg == WM_GETTEXT) && g_wide_buf && a[nargs_before + 3] == g_wide_buf) {
        r = callw(w, "user32", fn, b);
    } else if (msg == WM_SETTEXT) {
        b[nargs_before + 3] = g_wide_buf = tw(w, &t, a[nargs_before + 3], -1);
        r = callw(w, "user32", fn, b);
        g_wide_buf = outer;
    } else if (msg == 0x0180 || msg == 0x0181 || msg == 0x018C || msg == 0x018F || msg == 0x01A2 ||
               msg == 0x0143 || msg == 0x014A || msg == 0x014C || msg == 0x014D || msg == 0x0158) {
        /* list and combo box entries */
        b[nargs_before + 3] = tw(w, &t, a[nargs_before + 3], -1);
        r = callw(w, "user32", fn, b);
    } else if (msg == 0x0189 || msg == 0x0148) {           /* LB_GETTEXT, CB_GETLBTEXT */
        uint32_t g = qw_alloc(w, 512);
        b[nargs_before + 3] = g;
        r = callw(w, "user32", fn, b);
        if (r != 0xFFFFFFFFu) r = w2a_guest(w, a[nargs_before + 3], g, 256);
        qw_free(w, g);
    } else if (msg == WM_GETTEXT && a[nargs_before + 2]) {
        uint32_t max = a[nargs_before + 2], g = qw_alloc(w, max * 2u + 2u);
        b[nargs_before + 3] = g_wide_buf = g;
        callw(w, "user32", fn, b);
        g_wide_buf = outer;
        r = w2a_guest(w, a[nargs_before + 3], g, max);
        qw_free(w, g);
    } else r = callw(w, "user32", fn, b);
    tfree(w, &t);
    return r;
}
FN(a_DefWindowProcA) { return text_msg(w, "DefWindowProcW", a, 0); }
FN(a_SendMessageA) { return text_msg(w, "SendMessageW", a, 0); }
FN(a_SendDlgItemMessageA) {
    /* (hDlg, id, msg, wp, lp) -> GetDlgItem + SendMessageA */
    uint32_t b[16], h;
    memset(b, 0, sizeof(b)); b[0] = a[0]; b[1] = a[1];
    h = callw(w, "user32", "GetDlgItem", b);
    b[0] = h; b[1] = a[2]; b[2] = a[3]; b[3] = a[4];
    return text_msg(w, "SendMessageW", b, 0);
}
/* A program that subclasses one of our controls (Delphi's VCL does) hands its ANSI messages on
 * to our window procedure, which works in UTF-16: text messages need the same bridge as
 * SendMessageA. Its own procedures get them as they are. Inno Setup's wizard overran an
 * 8-byte buffer with UTF-16 here and broke its heap. */
FN(a_CallWindowProcA) {
    if (a[0] - QW_THUNK_BASE < (uint32_t)w->nthunk) return text_msg(w, "CallWindowProcW", a, 1);
    return callw(w, "user32", "CallWindowProcW", a);
}
FN(a_SetWindowTextA) { uint32_t r, outer = g_wide_buf; COPYARGS; b[1] = g_wide_buf = tw(w, &t, a[1], -1); r = CALL("user32", "SetWindowTextW"); g_wide_buf = outer; tfree(w, &t); return r; }
FN(a_GetWindowTextA) {
    uint32_t g, r;
    COPYARGS;
    if (!a[2]) return 0;
    g = qw_alloc(w, a[2] * 2u + 2u);
    {
        uint32_t outer = g_wide_buf;
        b[1] = g_wide_buf = g;
        CALL("user32", "GetWindowTextW");
        g_wide_buf = outer;
    }
    r = w2a_guest(w, a[1], g, a[2]);
    qw_free(w, g);
    (void)t;
    return r;
}
FN(a_SetDlgItemTextA) { uint32_t r; COPYARGS; b[2] = tw(w, &t, a[2], -1); r = CALL("user32", "SetDlgItemTextW"); tfree(w, &t); return r; }
FN(a_GetDlgItemTextA) {
    uint32_t g, r;
    COPYARGS;
    if (!a[3]) return 0;
    g = qw_alloc(w, a[3] * 2u + 2u);
    b[2] = g; CALL("user32", "GetDlgItemTextW");
    r = w2a_guest(w, a[2], g, a[3]);
    qw_free(w, g);
    (void)t;
    return r;
}
FN(a_LoadStringA) {
    uint32_t g, r;
    COPYARGS;
    if (!a[3]) return 0;                              /* ANSI has no read-only pointer form */
    g = qw_alloc(w, a[3] * 2u + 2u);
    b[2] = g; r = CALL("user32", "LoadStringW");
    r = r ? w2a_guest(w, a[2], g, a[3]) : (x86_wr8(C, a[2], 0), 0u);
    qw_free(w, g);
    (void)t;
    return r;
}
FN(a_MessageBoxA) { uint32_t r; COPYARGS;
    {   /* Always in the log. Benchmarks and unattended runs set QRX_WIN_AUTO_OK:
         * nobody is there to click the box a timedemo ends with. */
        char text[256];
        qw_rd_str(w, a[1], text, sizeof(text));
        qw_log(w, "MessageBox: %s", text);
        if (getenv("QRX_WIN_AUTO_OK")) return 1;
    }
    b[1] = tw(w, &t, a[1], -1); b[2] = tw(w, &t, a[2], -1); r = CALL("user32", "MessageBoxW"); tfree(w, &t); return r; }
FN(a_LoadBitmapA) { uint32_t r; COPYARGS; b[1] = tw(w, &t, a[1], -1); r = CALL("user32", "LoadBitmapW"); tfree(w, &t); return r; }
FN(a_LoadImageA) { uint32_t r; COPYARGS; b[1] = tw(w, &t, a[1], -1); r = CALL("user32", "LoadImageW"); tfree(w, &t); return r; }
FN(a_LoadMenuA) { uint32_t r; COPYARGS; b[1] = tw(w, &t, a[1], -1); r = CALL("user32", "LoadMenuW"); tfree(w, &t); return r; }
FN(a_LoadAcceleratorsA) { uint32_t r; COPYARGS; b[1] = tw(w, &t, a[1], -1); r = CALL("user32", "LoadAcceleratorsW"); tfree(w, &t); return r; }
FN(a_DialogBoxParamA) { uint32_t r; COPYARGS; b[1] = tw(w, &t, a[1], -1); r = CALL("user32", "DialogBoxParamW"); tfree(w, &t); return r; }
FN(a_CreateDialogParamA) { uint32_t r; COPYARGS; b[1] = tw(w, &t, a[1], -1); r = CALL("user32", "CreateDialogParamW"); tfree(w, &t); return r; }
FN(a_FindWindowA) { uint32_t r; COPYARGS; b[0] = tw(w, &t, a[0], -1); b[1] = tw(w, &t, a[1], -1); r = CALL("user32", "FindWindowW"); tfree(w, &t); return r; }
FN(a_RegisterWindowMessageA) { uint32_t r; COPYARGS; b[0] = tw(w, &t, a[0], -1); r = CALL("user32", "RegisterWindowMessageW"); tfree(w, &t); return r; }
FN(a_GetClassNameA) {
    uint32_t g, r;
    COPYARGS;
    if (!a[2]) return 0;
    g = qw_alloc(w, a[2] * 2u + 2u);
    b[1] = g; CALL("user32", "GetClassNameW");
    r = w2a_guest(w, a[1], g, a[2]);
    qw_free(w, g);
    (void)t;
    return r;
}
FN(a_AppendMenuA) { uint32_t r; COPYARGS; if (!(a[1] & 0x0904u)) b[3] = tw(w, &t, a[3], -1); r = CALL("user32", "AppendMenuW"); tfree(w, &t); return r; }  /* not MF_BITMAP/OWNERDRAW/SEPARATOR */
FN(a_ModifyMenuA) { uint32_t r; COPYARGS; if (!(a[2] & 0x0904u)) b[4] = tw(w, &t, a[4], -1); r = CALL("user32", "ModifyMenuW"); tfree(w, &t); return r; }
FN(a_DrawTextA) { uint32_t r; COPYARGS; b[1] = tw(w, &t, a[1], (int32_t)a[2]); r = CALL("user32", "DrawTextW"); tfree(w, &t); return r; }
FN(a_wvsprintfA) { const qhle *e = qw_find_hle("user32", "wsprintfA"); (void)e; return (uint32_t)qw_format(w, a[0], -1, 0, a[1], 0, a[2]); }
FN(a_CharUpperA) {
    if (a[0] < 0x10000u) { uint32_t c = a[0] & 0xFF; return (c >= 'a' && c <= 'z') ? c - 32 : c; }
    { uint32_t p; for (p = a[0]; ; p++) { uint8_t c = x86_rd8(C, p); if (!c) break; if (c >= 'a' && c <= 'z') x86_wr8(C, p, (uint8_t)(c - 32)); } }
    return a[0];
}
FN(a_CharLowerA) {
    if (a[0] < 0x10000u) { uint32_t c = a[0] & 0xFF; return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
    { uint32_t p; for (p = a[0]; ; p++) { uint8_t c = x86_rd8(C, p); if (!c) break; if (c >= 'A' && c <= 'Z') x86_wr8(C, p, (uint8_t)(c + 32)); } }
    return a[0];
}
FN(a_CharNextA) { (void)w; return x86_rd8(C, a[0]) ? a[0] + 1 : a[0]; }
FN(a_GetClassInfoExA) { uint32_t r; COPYARGS; b[1] = tw(w, &t, a[1], -1); r = CALL("user32", "GetClassInfoExW"); tfree(w, &t); return r; }

/* ---------------------------------------------------------------- gdi32 */
FN(a_TextOutA) { uint32_t r; COPYARGS; b[3] = tw(w, &t, a[3], (int32_t)a[4]); r = CALL("gdi32", "TextOutW"); tfree(w, &t); return r; }
FN(a_ExtTextOutA) { uint32_t r; COPYARGS; b[5] = tw(w, &t, a[5], (int32_t)a[6]); r = CALL("gdi32", "ExtTextOutW"); tfree(w, &t); return r; }
FN(a_GetTextExtentExPointA) { uint32_t r; COPYARGS; b[1] = tw(w, &t, a[1], (int32_t)a[2]); r = CALL("gdi32", "GetTextExtentExPointW"); tfree(w, &t); return r; }
FN(a_GetTextExtentPoint32A) { uint32_t r; COPYARGS; b[1] = tw(w, &t, a[1], (int32_t)a[2]); r = CALL("gdi32", "GetTextExtentPoint32W"); tfree(w, &t); return r; }
FN(a_FindResourceA) { uint32_t r; COPYARGS; b[1] = tw(w, &t, a[1], -1); b[2] = tw(w, &t, a[2], -1); r = CALL("kernel32", "FindResourceW"); tfree(w, &t); return r; }
FN(a_GetObjectA) { return callw(w, "gdi32", "GetObjectW", a); }    /* bitmaps: identical layout */
FN(a_CreateFontA) { return callw(w, "gdi32", "CreateFontW", a); }  /* face name is not used */
FN(a_CreateFontIndirectA) { return callw(w, "gdi32", "CreateFontIndirectW", a); }   /* first 28 bytes identical */
FN(a_GetTextMetricsA) {
    /* TEXTMETRICW: 11 LONGs, 4 WCHARs, 5 BYTEs; TEXTMETRICA: 11 LONGs, 4 BYTEs, 5 BYTEs */
    uint32_t g = qw_alloc(w, 64), r, i;
    COPYARGS;
    b[1] = g; r = CALL("gdi32", "GetTextMetricsW");
    for (i = 0; i < 44; i += 4) WR32(a[1] + i, RD32(g + i));
    for (i = 0; i < 4; i++) x86_wr8(C, a[1] + 44 + i, (uint8_t)x86_rd16(C, g + 44 + i * 2));
    for (i = 0; i < 5; i++) x86_wr8(C, a[1] + 48 + i, x86_rd8(C, g + 52 + i));
    qw_free(w, g);
    (void)t;
    return r;
}

#define U(n, f, c) { "user32", n, c, f }
#define G(n, f, c) { "gdi32", n, c, f }
const qhle qw_ansi[] = {
    U("RegisterClassExA", a_RegisterClassExA, 1), U("RegisterClassA", a_RegisterClassA, 1), U("UnregisterClassA", a_UnregisterClassA, 2),
    U("CreateWindowExA", a_CreateWindowExA, 12), U("DefWindowProcA", a_DefWindowProcA, 4), U("SendMessageA", a_SendMessageA, 4),
    U("SendDlgItemMessageA", a_SendDlgItemMessageA, 5), U("CallWindowProcA", a_CallWindowProcA, 5),
    U("SetWindowTextA", a_SetWindowTextA, 2), U("GetWindowTextA", a_GetWindowTextA, 3), U("SetDlgItemTextA", a_SetDlgItemTextA, 3),
    U("GetDlgItemTextA", a_GetDlgItemTextA, 4), U("LoadStringA", a_LoadStringA, 4), U("MessageBoxA", a_MessageBoxA, 4),
    U("LoadBitmapA", a_LoadBitmapA, 2), U("LoadImageA", a_LoadImageA, 6), U("LoadMenuA", a_LoadMenuA, 2),
    { "kernel32", "FindResourceA", 3, a_FindResourceA },
    U("LoadAcceleratorsA", a_LoadAcceleratorsA, 2), U("DialogBoxParamA", a_DialogBoxParamA, 5), U("CreateDialogParamA", a_CreateDialogParamA, 5),
    U("FindWindowA", a_FindWindowA, 2), U("RegisterWindowMessageA", a_RegisterWindowMessageA, 1), U("GetClassNameA", a_GetClassNameA, 3),
    U("AppendMenuA", a_AppendMenuA, 4), U("ModifyMenuA", a_ModifyMenuA, 5), U("DrawTextA", a_DrawTextA, 5),
    U("wvsprintfA", a_wvsprintfA, 3), U("CharUpperA", a_CharUpperA, 1), U("CharLowerA", a_CharLowerA, 1), U("CharNextA", a_CharNextA, 1),
    U("GetClassInfoExA", a_GetClassInfoExA, 3),
    G("TextOutA", a_TextOutA, 5), G("ExtTextOutA", a_ExtTextOutA, 8), G("GetTextExtentPoint32A", a_GetTextExtentPoint32A, 4), G("GetTextExtentExPointA", a_GetTextExtentExPointA, 7),
    G("GetTextExtentPointA", a_GetTextExtentPoint32A, 4), G("GetObjectA", a_GetObjectA, 3), G("CreateFontA", a_CreateFontA, 14),
    G("CreateFontIndirectA", a_CreateFontIndirectA, 1), G("GetTextMetricsA", a_GetTextMetricsA, 2),
    { NULL, NULL, 0, NULL }
};
