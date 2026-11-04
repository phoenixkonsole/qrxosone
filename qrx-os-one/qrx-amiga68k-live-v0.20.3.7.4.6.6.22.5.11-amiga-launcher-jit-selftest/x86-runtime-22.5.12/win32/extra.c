/* QRX OS One 22.5.12 - qrx-win32: further kernel32/user32/gdi32/msvcrt/winmm functions
 * needed by SDL 1.2 programs (PrBoom-plus): environment (shared by getenv,
 * GetEnvironmentVariable, _putenv, SetEnvironmentVariable and the -e option),
 * winmm time functions, C string helpers, display-mode queries, cursors and
 * palette/DIB functions.  Functions that need threads or processes report failure. */
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "qrxwin.h"

#define FN(name) static uint32_t name(qwin *w, uint32_t *a)
#define C (&w->cpu)
#define RD32(x) x86_rd32(C, (x))
#define WR32(x, v) x86_wr32(C, (x), (v))
#define UNUSED (void)w; (void)a

/* ------------------------------------------------------------------ environment */
#define QW_MAXENV 64
static char g_env[QW_MAXENV][256];
static uint32_t g_envguest[QW_MAXENV];     /* getenv() results live in guest memory */

static int env_find(const char *name)
{
    int i;
    size_t n = strlen(name);
    for (i = 0; i < QW_MAXENV; i++)
        if (g_env[i][0] && !strncasecmp(g_env[i], name, n) && g_env[i][n] == '=') return i;
    return -1;
}
void qw_env_set(const char *name, const char *value)      /* value NULL: remove */
{
    int i = env_find(name);
    if (i < 0) for (i = 0; i < QW_MAXENV && g_env[i][0]; i++) {}
    if (i >= QW_MAXENV) return;
    if (!value) { g_env[i][0] = 0; return; }
    snprintf(g_env[i], sizeof(g_env[i]), "%s=%s", name, value);
    g_envguest[i] = 0;
}
static const char *env_get(const char *name) { int i = env_find(name); return i < 0 ? NULL : strchr(g_env[i], '=') + 1; }

FN(x_getenv) {
    char n[128];
    int i;
    qw_rd_str(w, a[0], n, sizeof(n));
    i = env_find(n);
    if (i < 0) return 0;
    if (!g_envguest[i]) {
        const char *v = strchr(g_env[i], '=') + 1;
        uint32_t k, L = (uint32_t)strlen(v);
        g_envguest[i] = qw_alloc(w, L + 1);
        for (k = 0; k <= L; k++) x86_wr8(C, g_envguest[i] + k, (uint8_t)v[k]);
    }
    return g_envguest[i];
}
FN(x_putenv) {
    char s[300], *eq;
    qw_rd_str(w, a[0], s, sizeof(s));
    eq = strchr(s, '=');
    if (!eq) return 0xFFFFFFFFu;
    *eq = 0;
    qw_env_set(s, eq[1] ? eq + 1 : NULL);
    return 0;
}
FN(x_GetEnvironmentVariableA) {
    char n[128];
    const char *v;
    uint32_t L, k;
    qw_rd_str(w, a[0], n, sizeof(n));
    v = env_get(n);
    if (!v) { w->last_error = 203; return 0; }
    L = (uint32_t)strlen(v);
    if (L + 1 > a[2]) return L + 1;
    for (k = 0; k <= L; k++) x86_wr8(C, a[1] + k, (uint8_t)v[k]);
    return L;
}
FN(x_GetEnvironmentVariableW) {
    wchar16 t[128];
    char n[128];
    const char *v;
    uint32_t L, k;
    qw_rd_wstr(w, a[0], t, 128); w2a(n, t, 128);
    v = env_get(n);
    if (!v) { w->last_error = 203; return 0; }
    L = (uint32_t)strlen(v);
    if (L + 1 > a[2]) return L + 1;
    for (k = 0; k <= L; k++) x86_wr16(C, a[1] + k * 2, (uint8_t)v[k]);
    return L;
}
FN(x_SetEnvironmentVariableA) {
    char n[128], v[256];
    qw_rd_str(w, a[0], n, sizeof(n));
    if (a[1]) qw_rd_str(w, a[1], v, sizeof(v));
    qw_env_set(n, a[1] ? v : NULL);
    return 1;
}

/* ------------------------------------------------------------------ winmm */
FN(x_timeGetTime) { UNUSED; return (uint32_t)w->be->now(w); }
FN(x_ok0) { UNUSED; return 0; }
/* CPINFO: MaxCharSize, DefaultChar[2], LeadByte[12]: single-byte code page. */
FN(x_GetCPInfo) {
    uint32_t i;
    for (i = 0; i < 20; i += 4) x86_wr32(C, a[1] + i, 0);
    x86_wr32(C, a[1], 1);
    x86_wr8(C, a[1] + 4, '?');
    (void)w;
    return 1;
}
FN(x_ok1) { UNUSED; return 1; }
FN(x_timeSetEvent) { UNUSED; qw_log(w, "timeSetEvent: periodic callbacks need a thread - not available"); return 0; }

/* ------------------------------------------------------------------ msvcrt strings */
static int rd_c(qwin *w, uint32_t p) { return x86_rd8(C, p); }
FN(x_stricmp) {
    uint32_t i;
    for (i = 0; ; i++) {
        int x = tolower(rd_c(w, a[0] + i)), y = tolower(rd_c(w, a[1] + i));
        if (x != y) return (uint32_t)(x - y);
        if (!x) return 0;
    }
}
FN(x_strnicmp) {
    uint32_t i;
    for (i = 0; i < a[2]; i++) {
        int x = tolower(rd_c(w, a[0] + i)), y = tolower(rd_c(w, a[1] + i));
        if (x != y) return (uint32_t)(x - y);
        if (!x) return 0;
    }
    return 0;
}
FN(x_strstr) {
    uint32_t i, j;
    if (!rd_c(w, a[1])) return a[0];
    for (i = 0; rd_c(w, a[0] + i); i++) {
        for (j = 0; ; j++) {
            int n = rd_c(w, a[1] + j);
            if (!n) return a[0] + i;
            if (rd_c(w, a[0] + i + j) != n) break;
        }
    }
    return 0;
}
FN(x_strtok) {                          /* static position, like msvcrt (single thread) */
    static uint32_t pos;
    uint32_t s = a[0] ? a[0] : pos, start;
    char delim[64];
    qw_rd_str(w, a[1], delim, sizeof(delim));
    if (!s) return 0;
    while (rd_c(w, s) && strchr(delim, rd_c(w, s))) s++;
    if (!rd_c(w, s)) { pos = 0; return 0; }
    start = s;
    while (rd_c(w, s) && !strchr(delim, rd_c(w, s))) s++;
    if (rd_c(w, s)) { x86_wr8(C, s, 0); pos = s + 1; } else pos = 0;
    return start;
}
static void rd_num(qwin *w, uint32_t p, char *b, int max) { qw_rd_str(w, p, b, max); }
FN(x_strtol) { char b[128], *e; long v; rd_num(w, a[0], b, sizeof(b)); v = strtol(b, &e, (int)a[2]); if (a[1]) WR32(a[1], a[0] + (uint32_t)(e - b)); return (uint32_t)v; }
FN(x_strtoul) { char b[128], *e; unsigned long v; rd_num(w, a[0], b, sizeof(b)); v = strtoul(b, &e, (int)a[2]); if (a[1]) WR32(a[1], a[0] + (uint32_t)(e - b)); return (uint32_t)v; }
FN(x_strtoi64) { char b[128], *e; long long v; rd_num(w, a[0], b, sizeof(b)); v = strtoll(b, &e, (int)a[2]); if (a[1]) WR32(a[1], a[0] + (uint32_t)(e - b)); C->r[X86_EDX] = (uint32_t)((unsigned long long)v >> 32); return (uint32_t)v; }
FN(x_strtoui64) { char b[128], *e; unsigned long long v; rd_num(w, a[0], b, sizeof(b)); v = strtoull(b, &e, (int)a[2]); if (a[1]) WR32(a[1], a[0] + (uint32_t)(e - b)); C->r[X86_EDX] = (uint32_t)(v >> 32); return (uint32_t)v; }
FN(x_memchr) { uint32_t i; for (i = 0; i < a[2]; i++) if (rd_c(w, a[0] + i) == (int)(a[1] & 0xFF)) return a[0] + i; return 0; }
FN(x_strncat) {
    uint32_t d = a[0], i;
    while (rd_c(w, d)) d++;
    for (i = 0; i < a[2] && rd_c(w, a[1] + i); i++) x86_wr8(C, d + i, (uint8_t)rd_c(w, a[1] + i));
    x86_wr8(C, d + i, 0);
    return a[0];
}
FN(x_isalnum) { UNUSED; return isalnum((int)(a[0] & 0xFF)) ? 1 : 0; }
FN(x_isxdigit) { UNUSED; return isxdigit((int)(a[0] & 0xFF)) ? 1 : 0; }
static uint32_t put_s(qwin *w, uint32_t d, const char *s) { uint32_t i; for (i = 0; ; i++) { x86_wr8(C, d + i, (uint8_t)s[i]); if (!s[i]) break; } return d; }
static void utoa_r(unsigned long v, int radix, char *b)
{
    char t[40];
    int n = 0, i;
    if (radix < 2 || radix > 36) radix = 10;
    do { int dgt = (int)(v % (unsigned)radix); t[n++] = (char)(dgt < 10 ? '0' + dgt : 'a' + dgt - 10); v /= (unsigned)radix; } while (v);
    for (i = 0; i < n; i++) b[i] = t[n - 1 - i];
    b[n] = 0;
}
FN(x_ltoa) { char b[48]; long v = (int32_t)a[0]; if (v < 0 && a[2] == 10) { b[0] = '-'; utoa_r((unsigned long)-v, 10, b + 1); } else utoa_r((uint32_t)a[0], (int)a[2], b); return put_s(w, a[1], b); }
FN(x_ultoa) { char b[48]; utoa_r(a[0], (int)a[2], b); return put_s(w, a[1], b); }
FN(x_strrev) {
    uint32_t n = 0, i;
    while (rd_c(w, a[0] + n)) n++;
    for (i = 0; i < n / 2; i++) { int x = rd_c(w, a[0] + i); x86_wr8(C, a[0] + i, (uint8_t)rd_c(w, a[0] + n - 1 - i)); x86_wr8(C, a[0] + n - 1 - i, (uint8_t)x); }
    return a[0];
}
FN(x_strerror) { static uint32_t buf; char b[64]; if (!buf) buf = qw_alloc(w, 64); snprintf(b, sizeof(b), "error %d", (int)a[0]); return put_s(w, buf, b); }
FN(x_assert) {
    char e[200], f[200];
    qw_rd_str(w, a[0], e, sizeof(e)); qw_rd_str(w, a[1], f, sizeof(f));
    qw_log(w, "assertion failed: %s (%s:%u)", e, f, a[2]);
    w->exiting = 1; w->exit_code = 3;
    return 0;
}
FN(x_localeconv) { static uint32_t lc; UNUSED; if (!lc) { uint32_t i; lc = qw_alloc(w, 64); put_s(w, lc + 48, "."); for (i = 0; i < 48; i += 4) WR32(lc + i, lc + 49); WR32(lc, lc + 48); } return lc; }
FN(x_setlocale) { static uint32_t c; UNUSED; if (!c) { c = qw_alloc(w, 4); put_s(w, c, "C"); } return c; }
FN(x_fail_m1) { UNUSED; return 0xFFFFFFFFu; }
FN(x_mktemp) { UNUSED; return a[0]; }

/* ------------------------------------------------------------------ kernel32 */
FN(x_GetLocaleInfoA) { const char *v = "1252"; uint32_t L = 4; UNUSED; if (!a[3]) return L + 1; if (a[3] < L + 1) return 0; put_s(w, a[2], v); return L + 1; }

/* ------------------------------------------------------------------ user32 */
/* DEVMODEA: dmBitsPerPel 104, dmPelsWidth 108, dmPelsHeight 112, dmDisplayFrequency 120, dmSize 36, dmFields 40 */
FN(x_EnumDisplaySettingsA) {
    static const int modes[][3] = { { 640, 480, 16 }, { 640, 480, 8 }, { 320, 200, 8 }, { 320, 240, 8 }, { 800, 600, 16 }, { 800, 600, 8 } };
    uint32_t i = a[1] == 0xFFFFFFFFu || a[1] == 0xFFFFFFFEu ? 0 : a[1];
    if (i >= sizeof(modes) / sizeof(modes[0])) return 0;
    x86_wr16(C, a[2] + 36, 156);
    WR32(a[2] + 40, 0x00040000u | 0x00080000u | 0x00100000u | 0x00400000u);
    WR32(a[2] + 104, (uint32_t)modes[i][2]); WR32(a[2] + 108, (uint32_t)modes[i][0]); WR32(a[2] + 112, (uint32_t)modes[i][1]); WR32(a[2] + 120, 60);
    return 1;
}
FN(x_CreateCursor) { UNUSED; return gdi_new(w, GO_CURSOR); }
FN(x_CreateIconFromResourceEx) { UNUSED; return gdi_new(w, GO_ICON); }
FN(x_GetClassInfoA) {                    /* (hinst, name, WNDCLASSA*) via GetClassInfoExW */
    const qhle *ex = qw_find_hle("user32", "GetClassInfoExA");
    uint32_t b[16], tmp = qw_alloc(w, 48), r, i;
    memset(b, 0, sizeof(b));
    b[0] = a[0]; b[1] = a[1]; b[2] = tmp;
    r = ex ? ex->fn(w, b) : 0;
    if (r) for (i = 0; i < 40; i += 4) WR32(a[2] + i, RD32(tmp + 4 + i));
    qw_free(w, tmp);
    return r;
}
FN(x_GetKeyboardLayoutNameA) { UNUSED; put_s(w, a[0], "00000409"); return 1; }
FN(x_LoadKeyboardLayoutA) { UNUSED; return 0x04090409u; }
FN(x_MsgWaitForMultipleObjects) {        /* (n, handles, waitall, ms, mask): single thread */
    qevent ev;
    uint32_t ms = a[3] == 0xFFFFFFFFu ? 100 : a[3];
    int r, k = qw_dinput_ready(w, a[1], a[0]);
    if (k >= 0) return (uint32_t)k;                          /* DirectInput device has data */
    memset(&ev, 0, sizeof(ev));
    r = w->be->wait(w, ms, &ev);
    if (r > 0) {
        user_input_event(w, &ev);
        k = qw_dinput_ready(w, a[1], a[0]);
        return k >= 0 ? (uint32_t)k : a[0];                  /* device data, else "window message" */
    }
    return 0x102;
}
FN(x_ToUnicode) { const qhle *t = qw_find_hle("user32", "ToAsciiEx"); return t ? t->fn(w, a) : 0; }
FN(x_WindowFromPoint) { UNUSED; return w->main_wnd; }
FN(x_GetLastActivePopup) { UNUSED; return a[0]; }
FN(x_GetCursor) { static uint32_t c; UNUSED; if (!c) c = gdi_new(w, GO_CURSOR); return c; }

/* ------------------------------------------------------------------ gdi32 */
/* CreateDIBSection: the pixel memory is guest memory the program writes to; a
 * bitmap object mirrors it when it is selected and blitted (copied on use) */
FN(x_CreateDIBSection) {                 /* (hdc, BITMAPINFO*, usage, void **bits, hsection, offset) */
    uint32_t bi = a[1];
    int32_t bw = (int32_t)RD32(bi + 4), bh = (int32_t)RD32(bi + 8);
    int bpp = x86_rd16(C, bi + 14);
    uint32_t stride, mem, h;
    qobj *o;
    if (bw <= 0 || bw > 4096 || bh == 0 || bh > 4096 || bh < -4096) return 0;
    stride = (uint32_t)(((bw * bpp + 31) / 32) * 4);
    mem = qw_alloc(w, stride * (uint32_t)(bh < 0 ? -bh : bh));
    if (!mem) return 0;
    h = gdi_new(w, GO_BITMAP);
    o = O(w, h, GO_BITMAP);
    if (o) surf_resize(&o->bmp, bw, bh < 0 ? -bh : bh);
    if (a[3]) WR32(a[3], mem);
    qw_dib_register(w, h, mem, bi, (int)bw, (int)bh, bpp, stride);
    return h;
}
FN(x_GetDIBits) { UNUSED; return 0; }
FN(x_SetDIBColorTable) { return qw_dib_colortable(w, a[0], a[1], a[2], a[3]); }
FN(x_GetSystemPaletteEntries) {
    uint32_t i;
    UNUSED;
    if (!a[3]) return 256;
    for (i = 0; i < a[2]; i++) { uint32_t k = a[1] + i; WR32(a[3] + i * 4, (k & 0xFF) | ((k & 0xFF) << 8) | ((k & 0xFF) << 16)); }
    return a[2];
}
FN(x_SetPaletteEntries) { UNUSED; return a[2]; }

/* Environment block for GetEnvironmentStrings: one empty variable list. */
FN(x_GetEnvironmentStrings) { UNUSED; return qw_data(w, 8); }
FN(x_WriteConsoleA) {
    char buf[512]; uint32_t i, n = a[2] < sizeof(buf) - 1 ? a[2] : (uint32_t)sizeof(buf) - 1;
    for (i = 0; i < n; i++) buf[i] = (char)x86_rd8(C, a[1] + i);
    buf[n] = 0; qw_log(w, "console: %s", buf);
    if (a[3]) x86_wr32(C, a[3], a[2]);
    return 1;
}
FN(x_FileTimeToDosDateTime) { if (a[1]) x86_wr16(C, a[1], 0x2821); if (a[2]) x86_wr16(C, a[2], 0); return 1; }
FN(x_DosDateTimeToFileTime) { if (a[2]) { x86_wr32(C, a[2], 0); x86_wr32(C, a[2] + 4, 0); } return 1; }
FN(x_FileTimeCopy) { x86_wr32(C, a[1], x86_rd32(C, a[0])); x86_wr32(C, a[1] + 4, x86_rd32(C, a[0] + 4)); return 1; }
FN(x_GetEnvironmentStringsW) { UNUSED; return qw_data(w, 8); }
FN(x_SetHandleCount) { UNUSED; return a[0]; }
FN(x_GetOEMCP) { UNUSED; return 437; }
/* GetStringTypeA(lcid, type, src, n, out) / W(type, src, n, out): C1 classes for plain ASCII */
static uint16_t ctype1(unsigned c)
{
    uint16_t t = 0;
    if (c >= '0' && c <= '9') t |= 0x04;
    if (c >= 'a' && c <= 'z') t |= 0x02 | 0x100;
    if (c >= 'A' && c <= 'Z') t |= 0x01 | 0x100;
    if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x08;
    if (c == ' ') t |= 0x40;
    if (c < 32 || c == 127) t |= 0x20;
    if (c > 32 && c < 127 && !(t & 0x107)) t |= 0x10;
    return t;
}
FN(x_GetStringTypeA) {
    uint32_t i, n = a[3];
    if (n == 0xFFFFFFFFu) { for (n = 0; x86_rd8(C, a[2] + n); n++) {} n++; }
    for (i = 0; i < n; i++) x86_wr16(C, a[4] + i * 2, ctype1(x86_rd8(C, a[2] + i)));
    (void)w;
    return 1;
}
FN(x_GetStringTypeW) {
    uint32_t i, n = a[2];
    if (n == 0xFFFFFFFFu) { for (n = 0; x86_rd16(C, a[1] + n * 2); n++) {} n++; }
    for (i = 0; i < n; i++) x86_wr16(C, a[3] + i * 2, ctype1(x86_rd16(C, a[1] + i * 2)));
    (void)w;
    return 1;
}
/* LCMapStringA(lcid, flags, src, n, dst, dstn): case maps ASCII, flags 0x100 lower 0x200 upper */
FN(x_LCMapStringA) {
    uint32_t i, n = a[3];
    if (n == 0xFFFFFFFFu) { for (n = 0; x86_rd8(C, a[2] + n); n++) {} n++; }
    if (!a[5]) return n;
    if (n > a[5]) return 0;
    for (i = 0; i < n; i++) {
        uint8_t c = x86_rd8(C, a[2] + i);
        if ((a[1] & 0x100) && c >= 'A' && c <= 'Z') c += 32;
        if ((a[1] & 0x200) && c >= 'a' && c <= 'z') c -= 32;
        x86_wr8(C, a[4] + i, c);
    }
    (void)w;
    return n;
}
FN(x_LCMapStringW) {
    uint32_t i, n = a[3];
    if (n == 0xFFFFFFFFu) { for (n = 0; x86_rd16(C, a[2] + n * 2); n++) {} n++; }
    if (!a[5]) return n;
    if (n > a[5]) return 0;
    for (i = 0; i < n; i++) {
        uint16_t c = x86_rd16(C, a[2] + i * 2);
        if ((a[1] & 0x100) && c >= 'A' && c <= 'Z') c += 32;
        if ((a[1] & 0x200) && c >= 'a' && c <= 'z') c -= 32;
        x86_wr16(C, a[4] + i * 2, c);
    }
    (void)w;
    return n;
}
/* CompareStringA(lcid, flags, s1, n1, s2, n2): returns 1 less, 2 equal, 3 greater */
FN(x_CompareStringA) {
    uint32_t n1 = a[3], n2 = a[5], i;
    if (n1 == 0xFFFFFFFFu) for (n1 = 0; x86_rd8(C, a[2] + n1); n1++) {}
    if (n2 == 0xFFFFFFFFu) for (n2 = 0; x86_rd8(C, a[4] + n2); n2++) {}
    for (i = 0; i < n1 && i < n2; i++) {
        unsigned x = x86_rd8(C, a[2] + i), y = x86_rd8(C, a[4] + i);
        if (a[1] & 1) { if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; }
        if (x != y) return x < y ? 1 : 3;
    }
    (void)w;
    return n1 == n2 ? 2 : (n1 < n2 ? 1 : 3);
}
FN(x_lstrcatA) {
    uint32_t e = a[0], i = 0;
    uint8_t c;
    while (x86_rd8(C, e)) e++;
    do { c = x86_rd8(C, a[1] + i); x86_wr8(C, e + i, c); i++; } while (c);
    (void)w;
    return a[0];
}
FN(x_FileTimeToSystemTime) { uint32_t i; for (i = 0; i < 16; i += 2) x86_wr16(C, a[1] + i, i == 0 ? 1996 : 1); (void)w; return 1; }
FN(x_GetUserNameA) {
    static const char n[] = "QRX";
    uint32_t i;
    for (i = 0; i < sizeof(n); i++) x86_wr8(C, a[0] + i, (uint8_t)n[i]);
    if (a[1]) x86_wr32(C, a[1], (uint32_t)sizeof(n));
    (void)w;
    return 1;
}
/* No cooperative scheduler yet: the thread is recorded and never runs. */
FN(x_CreateThread) {
    qw_log(w, "CreateThread start=%08x param=%08x flags=%08x: not run", a[2], a[3], a[4]);
    if (a[5]) x86_wr32(C, a[5], 0x200);
    return 0x7E200001u;
}
FN(x_ExitThread) { qw_log(w, "ExitThread(%u)", a[0]); w->exit_code = (int)a[0]; w->exiting = 1; C->stop = 1; return 0; }
/* No registry on this board: games fall back to their defaults. */
#define REG_NOT_FOUND 2u
FN(x_RegNotFound) { UNUSED; return REG_NOT_FOUND; }
FN(x_RegCreateKeyA) { if (a[2]) x86_wr32(C, a[2], 0x7E000001u); return 0; }
FN(x_MidiFail) { UNUSED; return 2u; /* MMSYSERR_BADDEVICEID */ }
FN(x_DeviceIoControl) { UNUSED; return 0; }
FN(x_StretchDIBits) { UNUSED; return a[6]; /* lines copied; the RTG surface is the real output */ }
FN(x_CreateDCA) { UNUSED; return 0x7E100001u; }
FN(x_CreateDIBitmap) { UNUSED; return 0x7E100002u; }
#define K(n, f, c) { "kernel32", n, c, f }
#define U(n, f, c) { "user32", n, c, f }
#define G(n, f, c) { "gdi32", n, c, f }
#define ADV(n, f, c) { "advapi32", n, c, f }
#define M(n, f) { "msvcrt", n, -1, f }
#define W_(n, f, c) { "winmm", n, c, f }
const qhle qw_extra[] = {
    K("GetEnvironmentStringsW", x_GetEnvironmentStringsW, 0), K("FreeEnvironmentStringsW", x_ok1, 1), K("FreeEnvironmentStringsA", x_ok1, 1),
    K("SetHandleCount", x_SetHandleCount, 1), K("GetOEMCP", x_GetOEMCP, 0), K("GetStringTypeA", x_GetStringTypeA, 5),
    K("GetStringTypeW", x_GetStringTypeW, 4), K("LCMapStringA", x_LCMapStringA, 6), K("LCMapStringW", x_LCMapStringW, 6),
    K("CompareStringA", x_CompareStringA, 6), K("CompareStringW", x_ok0, 6), K("IsBadCodePtr", x_ok0, 1), K("IsBadWritePtr", x_ok0, 2),
    K("DebugBreak", x_ok0, 0), K("lstrcatA", x_lstrcatA, 2), K("FileTimeToSystemTime", x_FileTimeToSystemTime, 2),
    K("RtlUnwind", x_ok0, 4), K("CreateThread", x_CreateThread, 6), K("ExitThread", x_ExitThread, 1),
    ADV("GetUserNameA", x_GetUserNameA, 2), ADV("RegDeleteKeyA", x_ok0, 2), ADV("RegEnumKeyExA", x_RegNotFound, 8),
    U("WinHelpA", x_ok1, 4), U("PostThreadMessageA", x_ok1, 4), U("GetWindowPlacement", x_ok0, 2), U("SetWindowPlacement", x_ok1, 2),
    U("CreateDialogIndirectParamA", x_ok0, 5),
    { "comctl32", "PropertySheetA", 1, x_fail_m1 },
    { "comdlg32", "GetOpenFileNameA", 1, x_ok0 }, { "comdlg32", "GetSaveFileNameA", 1, x_ok0 }, { "comdlg32", "CommDlgExtendedError", 0, x_ok0 },
    K("GetEnvironmentStrings", x_GetEnvironmentStrings, 0), K("SetStdHandle", x_ok1, 2), K("GetConsoleMode", x_ok0, 2),
    K("SetConsoleMode", x_ok1, 2), K("WriteConsoleA", x_WriteConsoleA, 5), K("ReadConsoleInputA", x_ok0, 4),
    K("DosDateTimeToFileTime", x_DosDateTimeToFileTime, 3), K("FileTimeToDosDateTime", x_FileTimeToDosDateTime, 3),
    K("FileTimeToLocalFileTime", x_FileTimeCopy, 2), K("LocalFileTimeToFileTime", x_FileTimeCopy, 2),
    K("DeviceIoControl", x_DeviceIoControl, 8), 
    ADV("RegOpenKeyA", x_RegNotFound, 3), ADV("RegCreateKeyA", x_RegCreateKeyA, 3),
    ADV("RegQueryValueExA", x_RegNotFound, 6), ADV("RegSetValueExA", x_ok0, 6),
    W_("joyGetNumDevs", x_ok0, 0), W_("joyGetDevCapsA", x_MidiFail, 3), W_("joyGetPosEx", x_MidiFail, 2),
    W_("midiStreamOpen", x_MidiFail, 6), W_("midiStreamClose", x_ok0, 1), W_("midiStreamOut", x_MidiFail, 3),
    W_("midiStreamPause", x_ok0, 1), W_("midiStreamRestart", x_ok0, 1), W_("midiStreamProperty", x_MidiFail, 3),
    W_("midiOutPrepareHeader", x_MidiFail, 3), W_("midiOutUnprepareHeader", x_ok0, 3), W_("midiOutReset", x_ok0, 1),
    G("CreateDCA", x_CreateDCA, 4), G("CreateDIBitmap", x_CreateDIBitmap, 6), G("StretchDIBits", x_StretchDIBits, 13),
    K("GetEnvironmentVariableA", x_GetEnvironmentVariableA, 3), K("GetEnvironmentVariableW", x_GetEnvironmentVariableW, 3),
    K("SetEnvironmentVariableA", x_SetEnvironmentVariableA, 2), K("FormatMessageA", x_ok0, 7), K("GetLocaleInfoA", x_GetLocaleInfoA, 4),
    K("GetCPInfo", x_GetCPInfo, 2), K("IsDBCSLeadByte", x_ok0, 1), K("GetFileInformationByHandle", x_ok0, 2),
    U("MapVirtualKeyExA", x_ok0, 3),
    K("IsDBCSLeadByteEx", x_ok0, 2), K("SetHandleInformation", x_ok1, 3), K("SetPriorityClass", x_ok1, 2),
    K("CreatePipe", x_ok0, 4), K("CreateProcessA", x_ok0, 10),
    M("getenv", x_getenv), M("_putenv", x_putenv), M("_stricmp", x_stricmp), M("_strnicmp", x_strnicmp), M("strstr", x_strstr),
    M("strtok", x_strtok), M("strtol", x_strtol), M("strtoul", x_strtoul), M("_strtoi64", x_strtoi64), M("_strtoui64", x_strtoui64),
    M("memchr", x_memchr), M("strncat", x_strncat), M("isalnum", x_isalnum), M("isxdigit", x_isxdigit), M("_ltoa", x_ltoa),
    M("_ultoa", x_ultoa), M("_strrev", x_strrev), M("strerror", x_strerror), M("_assert", x_assert), M("localeconv", x_localeconv),
    M("setlocale", x_setlocale), M("_fdopen", x_ok0), M("_open_osfhandle", x_fail_m1), M("freopen", x_ok0), M("setbuf", x_ok0),
    M("_isatty", x_ok0), M("_fstat", x_fail_m1), M("_mktemp", x_mktemp), M("system", x_fail_m1), M("raise", x_ok0),
    U("ChangeDisplaySettingsA", x_ok0, 2), U("EnumDisplaySettingsA", x_EnumDisplaySettingsA, 3), U("ClipCursor", x_ok1, 1),
    U("CreateCursor", x_CreateCursor, 7), U("CreateIconFromResourceEx", x_CreateIconFromResourceEx, 7), U("GetClassInfoA", x_GetClassInfoA, 3),
    U("GetCursor", x_GetCursor, 0), U("GetKeyboardLayoutNameA", x_GetKeyboardLayoutNameA, 1), U("LoadKeyboardLayoutA", x_LoadKeyboardLayoutA, 2),
    U("MsgWaitForMultipleObjects", x_MsgWaitForMultipleObjects, 5), U("ScrollWindowEx", x_ok0, 8), U("SetCursorPos", x_ok1, 2),
    U("ToUnicode", x_ToUnicode, 6), U("WindowFromPoint", x_WindowFromPoint, 2), U("GetLastActivePopup", x_GetLastActivePopup, 1),
    G("CreateDIBSection", x_CreateDIBSection, 6), G("GetDIBits", x_GetDIBits, 7), G("GetDeviceGammaRamp", x_ok0, 2),
    G("SetDeviceGammaRamp", x_ok0, 2), G("GetSystemPaletteEntries", x_GetSystemPaletteEntries, 4), G("GetSystemPaletteUse", x_ok1, 1),
    G("SetSystemPaletteUse", x_ok1, 2), G("SetDIBColorTable", x_SetDIBColorTable, 4), G("SetPaletteEntries", x_SetPaletteEntries, 4),
    G("UnrealizeObject", x_ok1, 1),
    W_("timeGetTime", x_timeGetTime, 0), W_("timeBeginPeriod", x_ok0, 1), W_("timeEndPeriod", x_ok0, 1), W_("timeSetEvent", x_timeSetEvent, 5),
    W_("timeKillEvent", x_ok0, 1), W_("midiOutGetNumDevs", x_ok0, 0), W_("midiOutGetDevCapsA", x_fail_m1, 3), W_("midiOutSetVolume", x_ok0, 2),
    { NULL, NULL, 0, NULL }
};
