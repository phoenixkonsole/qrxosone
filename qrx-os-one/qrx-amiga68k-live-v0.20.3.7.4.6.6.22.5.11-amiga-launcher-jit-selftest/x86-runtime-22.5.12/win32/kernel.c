/* QRX OS One 22.5.12 - qrx-win32: kernel32, msvcrt, advapi32 and small DLLs. */
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../qmath/qmath.h"    /* msvcrt math -> qm_* (deterministic, host libm not trusted) */
#include "../qmath/qfloat.h"
#include <time.h>
#include <ctype.h>
#include <sys/time.h>
#include "qrxwin.h"

#define FN(name) static uint32_t name(qwin *w, uint32_t *a)
#define C (&w->cpu)
#define RD32(x) x86_rd32(C, (x))
#define WR32(x, v) x86_wr32(C, (x), (v))
#define UNUSED (void)w; (void)a

static const char *known_dlls[] = { "kernel32", "user32", "gdi32", "advapi32", "msvcrt", "comctl32", "shell32", "winmm", "ntdll", "ddraw", "dsound", "dinput", "opengl32", "glide2x", "glide3x", "d3d8", "d3d9", "vulkan-1", NULL };
#define QW_NKNOWN 18
#define QW_MODBASE 0x7E000000u

static double argd(qwin *w, uint32_t addr) { uint64_t u = (uint64_t)RD32(addr) | ((uint64_t)RD32(addr + 4) << 32); double d; memcpy(&d, &u, 8); return d; }
static double dbl(uint32_t *a, int i) { uint64_t u = (uint64_t)a[i] | ((uint64_t)a[i + 1] << 32); double d; memcpy(&d, &u, 8); return d; }
static uint32_t retd(qwin *w, double v) { x86_fpu_push(C, v); return 0; }

static uint64_t host_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

/* ====================================================================== printf / scanf */
typedef struct { wchar16 *b; int n, max; } wbuf;
static void put(wbuf *o, wchar16 ch) { if (o->n < o->max - 1) o->b[o->n] = ch; o->n++; }
static void puts8(wbuf *o, const char *s) { while (*s) put(o, (uint8_t)*s++); }

/* fmt: wide or narrow string in guest memory; ap: guest address of first vararg */
static int qfmt(qwin *w, wbuf *o, uint32_t fmt, int fwide, uint32_t ap)
{
    uint32_t f = fmt;
#define FCH() (fwide ? x86_rd16(C, f) : x86_rd8(C, f))
#define FADV() (f += fwide ? 2u : 1u)
    for (;;) {
        uint32_t ch = FCH();
        char spec[32], tmp[512];
        int sl = 0, width = -1, prec = -1, lng = 0, hflag = 0, wflag = 0, left = 0;
        if (!ch) break;
        FADV();
        if (ch != '%') { put(o, (wchar16)ch); continue; }
        spec[sl++] = '%';
        for (;;) {
            ch = FCH();
            if (ch == '-' || ch == '+' || ch == ' ' || ch == '#' || ch == '0') { if (ch == '-') left = 1; spec[sl++] = (char)ch; FADV(); continue; }
            break;
        }
        if (ch == '*') { width = (int32_t)RD32(ap); ap += 4; FADV(); if (width < 0) { left = 1; width = -width; spec[sl++] = '-'; } }
        else while (ch >= '0' && ch <= '9') { width = (width < 0 ? 0 : width * 10) + (int)(ch - '0'); FADV(); ch = FCH(); }
        ch = FCH();
        if (ch == '.') {
            FADV(); ch = FCH(); prec = 0;
            if (ch == '*') { prec = (int32_t)RD32(ap); ap += 4; FADV(); }
            else while (ch >= '0' && ch <= '9') { prec = prec * 10 + (int)(ch - '0'); FADV(); ch = FCH(); }
        }
        for (;;) {
            ch = FCH();
            if (ch == 'l') { lng++; FADV(); continue; }
            if (ch == 'h') { hflag++; FADV(); continue; }
            if (ch == 'w') { wflag = 1; FADV(); continue; }
            if (ch == 'L' || ch == 'z' || ch == 'j' || ch == 't') { if (ch == 'j') lng = 2; FADV(); continue; }
            if (ch == 'I') {
                FADV();
                if (FCH() == '6' && x86_rd8(C, f + (fwide ? 2 : 1)) == '4') { FADV(); FADV(); lng = 2; }
                else if (FCH() == '3' && x86_rd8(C, f + (fwide ? 2 : 1)) == '2') { FADV(); FADV(); }
                continue;
            }
            break;
        }
        ch = FCH();
        if (!ch) break;
        FADV();
        if (width >= 0) sl += snprintf(spec + sl, sizeof(spec) - (size_t)sl, "%d", width);
        if (prec >= 0) sl += snprintf(spec + sl, sizeof(spec) - (size_t)sl, ".%d", prec);
        switch (ch) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            long long v;
            if (lng >= 2) { v = (long long)((uint64_t)RD32(ap) | ((uint64_t)RD32(ap + 4) << 32)); ap += 8; }
            else {
                uint32_t u = RD32(ap); ap += 4;
                if (hflag) u &= 0xFFFF;
                v = (ch == 'd' || ch == 'i') ? (long long)(hflag ? (int16_t)u : (int32_t)u) : (long long)u;
            }
            snprintf(spec + sl, sizeof(spec) - (size_t)sl, "ll%c", (int)ch);
            if (ch == 'd' || ch == 'i') snprintf(tmp, sizeof(tmp), spec, v);
            else snprintf(tmp, sizeof(tmp), spec, (unsigned long long)v);
            puts8(o, tmp);
            break; }
        case 'p': snprintf(tmp, sizeof(tmp), "%08X", RD32(ap)); ap += 4; puts8(o, tmp); break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double d = argd(w, ap);
            int fl = 0, k;
            ap += 8;
            for (k = 1; k < sl && (spec[k] < '1' || spec[k] > '9') && spec[k] != '.'; k++) {
                if (spec[k] == '-') fl |= QF_LEFT; else if (spec[k] == '+') fl |= QF_PLUS; else if (spec[k] == ' ') fl |= QF_SPACE;
                else if (spec[k] == '#') fl |= QF_ALT; else if (spec[k] == '0') fl |= QF_ZERO;
            }
            /* exact digits independent of the host C library (AROS printf is not exact) */
            qf_format(tmp, sizeof(tmp), d, (int)ch, prec, fl, width < 0 ? 0 : width);
            puts8(o, tmp);
            break; }
        case 'a': case 'A': {
            double d = argd(w, ap); ap += 8;
            spec[sl++] = (char)ch; spec[sl] = 0;
            snprintf(tmp, sizeof(tmp), spec, d);
            puts8(o, tmp);
            break; }
        case 'c': case 'C': {
            uint32_t v = RD32(ap); ap += 4;
            int pad = width > 1 ? width - 1 : 0;
            if (!left) while (pad-- > 0) put(o, ' ');
            put(o, (wchar16)(((ch == 'c') == !fwide && !wflag && !lng) ? (v & 0xFF) : (v & 0xFFFF)));
            if (left) while (pad-- > 0) put(o, ' ');
            break; }
        case 's': case 'S': {
            uint32_t p = RD32(ap), i;
            int wide = (ch == 's') ? (fwide ? !hflag : (lng || wflag)) : !fwide;
            int len = 0, pad;
            ap += 4;
            if (!p) { static const char nul[] = "(null)"; for (i = 0; nul[i] && (prec < 0 || (int)i < prec); i++) put(o, (wchar16)nul[i]); break; }
            while ((prec < 0 || len < prec) && (wide ? x86_rd16(C, p + (uint32_t)len * 2) : x86_rd8(C, p + (uint32_t)len))) len++;
            pad = width > len ? width - len : 0;
            if (!left) while (pad-- > 0) put(o, ' ');
            for (i = 0; (int)i < len; i++) put(o, wide ? x86_rd16(C, p + i * 2) : x86_rd8(C, p + i));
            if (left) while (pad-- > 0) put(o, ' ');
            break; }
        case 'n': WR32(RD32(ap), (uint32_t)o->n); ap += 4; break;
        case '%': put(o, '%'); break;
        default: put(o, '%'); put(o, (wchar16)ch); break;
        }
    }
    if (o->max > 0) o->b[o->n < o->max ? o->n : o->max - 1] = 0;
    return o->n;
#undef FCH
#undef FADV
}

static int qscan(qwin *w, uint32_t in, int iwide, uint32_t fmt, int fwide, uint32_t ap)
{
    uint32_t s = in, f = fmt;
    int count = 0;
#define SCH() (iwide ? x86_rd16(C, s) : x86_rd8(C, s))
#define SADV() (s += iwide ? 2u : 1u)
#define FCH() (fwide ? x86_rd16(C, f) : x86_rd8(C, f))
#define FADV() (f += fwide ? 2u : 1u)
    for (;;) {
        uint32_t ch = FCH();
        int lng = 0, hflag = 0, width = 0, skip = 0;
        char tmp[128];
        int n = 0;
        if (!ch) break;
        FADV();
        if (ch == ' ' || ch == '\t' || ch == '\n') { while (SCH() == ' ' || SCH() == '\t' || SCH() == '\n') SADV(); continue; }
        if (ch != '%') { if (SCH() != ch) break; SADV(); continue; }
        if (FCH() == '*') { skip = 1; FADV(); }
        while (FCH() >= '0' && FCH() <= '9') { width = width * 10 + (int)(FCH() - '0'); FADV(); }
        for (;;) {
            ch = FCH();
            if (ch == 'l') { lng++; FADV(); continue; }
            if (ch == 'h') { hflag = 1; FADV(); continue; }
            if (ch == 'L') { lng = 2; FADV(); continue; }
            if (ch == 'I') { FADV(); if (FCH() == '6') { FADV(); FADV(); lng = 2; } continue; }
            break;
        }
        ch = FCH(); FADV();
        if (ch != 'c') while (SCH() == ' ' || SCH() == '\t' || SCH() == '\n') SADV();
        if (!width) width = 127;
        if (ch == 'd' || ch == 'i' || ch == 'u' || ch == 'x' || ch == 'X' || ch == 'o') {
            unsigned long long v;
            int base = (ch == 'x' || ch == 'X') ? 16 : ch == 'o' ? 8 : ch == 'i' ? 0 : 10;
            char *e;
            while (n < width && n < 120) {
                uint32_t k = SCH();
                if (!(isxdigit((int)(k & 0x7F)) || k == '-' || k == '+' || ((k == 'x' || k == 'X') && n == 1)) || k > 127) break;
                if (base == 10 && isalpha((int)k)) break;
                tmp[n++] = (char)k; SADV();
            }
            tmp[n] = 0;
            if (!n) break;
            v = (ch == 'd' || ch == 'i') ? (unsigned long long)strtoll(tmp, &e, base) : strtoull(tmp, &e, base);
            if (!skip) {
                uint32_t p = RD32(ap); ap += 4;
                if (lng >= 2) { WR32(p, (uint32_t)v); WR32(p + 4, (uint32_t)(v >> 32)); }
                else if (hflag) x86_wr16(C, p, (uint16_t)v);
                else WR32(p, (uint32_t)v);
                count++;
            }
        } else if (ch == 'f' || ch == 'e' || ch == 'g' || ch == 'E' || ch == 'G') {
            double d;
            while (n < width && n < 120) {
                uint32_t k = SCH();
                if (!(isdigit((int)(k & 0x7F)) || k == '-' || k == '+' || k == '.' || k == 'e' || k == 'E') || k > 127) break;
                tmp[n++] = (char)k; SADV();
            }
            tmp[n] = 0;
            if (!n) break;
            d = qf_strtod(tmp, NULL);
            if (!skip) {
                uint32_t p = RD32(ap); ap += 4;
                if (lng) { uint64_t u; memcpy(&u, &d, 8); WR32(p, (uint32_t)u); WR32(p + 4, (uint32_t)(u >> 32)); }
                else { float fl = (float)d; uint32_t u; memcpy(&u, &fl, 4); WR32(p, u); }
                count++;
            }
        } else if (ch == 's') {
            uint32_t p = skip ? 0 : RD32(ap);
            int wide = fwide ? !hflag : lng;
            if (!skip) ap += 4;
            while (n < width && SCH() && SCH() != ' ' && SCH() != '\t' && SCH() != '\n') {
                if (p) { if (wide) x86_wr16(C, p + (uint32_t)n * 2, (uint16_t)SCH()); else x86_wr8(C, p + (uint32_t)n, (uint8_t)SCH()); }
                n++; SADV();
            }
            if (p) { if (wide) x86_wr16(C, p + (uint32_t)n * 2, 0); else x86_wr8(C, p + (uint32_t)n, 0); }
            if (!n) break;
            if (!skip) count++;
        } else if (ch == 'c') {
            if (!SCH()) break;
            if (!skip) { uint32_t p = RD32(ap); ap += 4; if (fwide && !hflag) x86_wr16(C, p, (uint16_t)SCH()); else x86_wr8(C, p, (uint8_t)SCH()); count++; }
            SADV();
        } else break;
    }
    return count ? count : (SCH() ? 0 : -1);
#undef SCH
#undef SADV
#undef FCH
#undef FADV
}

/* write a wide result into guest buffer (wide or narrow) */
static int out_str(qwin *w, uint32_t dst, int max, const wchar16 *s, int n, int wide)
{
    int i;
    if (!dst || max <= 0) return n;
    for (i = 0; i < n && i < max; i++) { if (wide) x86_wr16(C, dst + (uint32_t)i * 2, s[i]); else x86_wr8(C, dst + (uint32_t)i, (uint8_t)s[i]); }
    if (i < max) { if (wide) x86_wr16(C, dst + (uint32_t)i * 2, 0); else x86_wr8(C, dst + (uint32_t)i, 0); }
    return n;
}

static wchar16 g_fbuf[16384];
static int fmt_to(qwin *w, uint32_t dst, int max, int dwide, uint32_t fmt, int fwide, uint32_t ap, int trunc_neg)
{
    wbuf o; int n;
    o.b = g_fbuf; o.n = 0; o.max = 16384;
    n = qfmt(w, &o, fmt, fwide, ap);
    if (n > 16383) n = 16383;
    if (max < 0) { out_str(w, dst, n + 1, g_fbuf, n, dwide); return n; }
    if (n >= max && trunc_neg) { out_str(w, dst, max, g_fbuf, max, dwide); return -1; }
    out_str(w, dst, max, g_fbuf, n < max ? n : max - 1, dwide);
    return n;
}

/* ====================================================================== msvcrt */
static uint32_t g_iob, g_wcmdln, g_acmdln, g_winitenv, g_initenv, g_fmode, g_commode, g_errno, g_argc, g_wargv, g_argv;

uint32_t qw_data_import(qwin *w, const char *dll, const char *name)
{
    (void)dll;
    if (!strcmp(name, "_iob")) { if (!g_iob) g_iob = qw_data(w, 32 * 20); return g_iob; }
    if (!strcmp(name, "_wcmdln")) {
        if (!g_wcmdln) { uint32_t s = qw_data(w, 1024); qw_wr_wstr(w, s, w->cmdline, 512); g_wcmdln = qw_data(w, 4); WR32(g_wcmdln, s); }
        return g_wcmdln;
    }
    if (!strcmp(name, "_acmdln")) {
        if (!g_acmdln) { uint32_t s = qw_data(w, 512), i; for (i = 0; w->cmdline[i] && i < 511; i++) x86_wr8(C, s + i, (uint8_t)w->cmdline[i]); g_acmdln = qw_data(w, 4); WR32(g_acmdln, s); }
        return g_acmdln;
    }
    if (!strcmp(name, "__winitenv")) { if (!g_winitenv) { uint32_t arr = qw_data(w, 8); g_winitenv = qw_data(w, 4); WR32(g_winitenv, arr); } return g_winitenv; }
    if (!strcmp(name, "__initenv") || !strcmp(name, "_environ")) { if (!g_initenv) { uint32_t arr = qw_data(w, 8); g_initenv = qw_data(w, 4); WR32(g_initenv, arr); } return g_initenv; }
    if (!strcmp(name, "__lc_codepage")) { static uint32_t g_codepage; if (!g_codepage) { g_codepage = qw_data(w, 4); WR32(g_codepage, 1252); } return g_codepage; }
    if (!strcmp(name, "__mb_cur_max")) { static uint32_t g_mbmax; if (!g_mbmax) { g_mbmax = qw_data(w, 4); WR32(g_mbmax, 1); } return g_mbmax; }
    if (!strcmp(name, "_fmode")) { if (!g_fmode) g_fmode = qw_data(w, 4); return g_fmode; }
    if (!strcmp(name, "_commode")) { if (!g_commode) g_commode = qw_data(w, 4); return g_commode; }
    return 0;
}

static void build_args(qwin *w)
{
    /* split the wide command line into argv (wide + narrow) */
    wchar16 *s = w->cmdline;
    uint32_t wv, av, i = 0, n = 0;
    uint32_t parts[32][2];
    int k = 0;
    if (g_wargv) return;
    while (s[k] && n < 31) {
        int st, q = 0;
        while (s[k] == ' ') k++;
        if (!s[k]) break;
        st = k;
        while (s[k] && (q || s[k] != ' ')) { if (s[k] == '"') q = !q; k++; }
        parts[n][0] = (uint32_t)st; parts[n][1] = (uint32_t)(k - st); n++;
    }
    wv = qw_data(w, (n + 1) * 4); av = qw_data(w, (n + 1) * 4);
    for (i = 0; i < n; i++) {
        uint32_t L = parts[i][1], j, ws, as, o = 0;
        ws = qw_data(w, (L + 1) * 2); as = qw_data(w, L + 1);
        for (j = 0; j < L; j++) { wchar16 ch = s[parts[i][0] + j]; if (ch == '"') continue; x86_wr16(C, ws + o * 2, ch); x86_wr8(C, as + o, (uint8_t)ch); o++; }
        WR32(wv + i * 4, ws); WR32(av + i * 4, as);
    }
    g_argc = n; g_wargv = wv; g_argv = av;
}

FN(m___wgetmainargs) { build_args(w); WR32(a[0], g_argc); WR32(a[1], g_wargv); qw_data_import(w, 0, "__winitenv"); WR32(a[2], RD32(g_winitenv)); return 0; }
FN(m___getmainargs) { build_args(w); WR32(a[0], g_argc); WR32(a[1], g_argv); qw_data_import(w, 0, "__initenv"); WR32(a[2], RD32(g_initenv)); return 0; }
FN(m_nop) { UNUSED; return 0; }
FN(m_initterm) {
    uint32_t p;
    for (p = a[0]; p < a[1]; p += 4) { uint32_t fn = RD32(p); if (fn) qw_call(w, fn, 0, 0, 0, 0, 0); if (w->exiting || C->fault) break; }
    return 0;
}
FN(m_initterm_e) {
    uint32_t p;
    for (p = a[0]; p < a[1]; p += 4) { uint32_t fn = RD32(p); if (fn) { uint32_t r = qw_call(w, fn, 0, 0, 0, 0, 0); if (r) return r; } if (w->exiting || C->fault) break; }
    return 0;
}
FN(m_onexit) { if (w->nonexit < 64) w->onexit[w->nonexit++] = a[0]; return a[0]; }
/* atexit reports success as 0, unlike _onexit, which returns the function. */
FN(m_atexit) { if (w->nonexit < 64) w->onexit[w->nonexit++] = a[0]; return 0; }
FN(m_dllonexit) { return m_onexit(w, a); }
FN(m_exit) {
    while (w->nonexit > 0) { uint32_t fn = w->onexit[--w->nonexit]; qw_call(w, fn, 0, 0, 0, 0, 0); }
    w->exit_code = a[0]; w->exiting = 1; return 0;
}
FN(m__exit) { w->exit_code = a[0]; w->exiting = 1; return 0; }
FN(m_abort) { qw_log(w, "abort() called by the program"); w->exit_code = 3; w->exiting = 1; return 0; }
FN(m_amsg_exit) { qw_log(w, "_amsg_exit(%u)", a[0]); w->exit_code = 255; w->exiting = 1; return 0; }
FN(m_malloc) { return qw_alloc(w, a[0]); }
FN(m_calloc) { return qw_alloc(w, a[0] * a[1]); }
FN(m_free) { qw_free(w, a[0]); return 0; }
FN(m_realloc) {
    uint32_t n, old;
    if (!a[0]) return qw_alloc(w, a[1]);
    if (!a[1]) { qw_free(w, a[0]); return 0; }
    old = qw_size(w, a[0]);
    if (old >= a[1]) return a[0];
    n = qw_alloc(w, a[1]);
    if (n) { uint32_t i; for (i = 0; i < old; i++) x86_wr8(C, n + i, x86_rd8(C, a[0] + i)); qw_free(w, a[0]); }
    return n;
}
FN(m_msize) { return qw_size(w, a[0]); }
FN(m_memcpy) { uint32_t i; for (i = 0; i < a[2]; i++) x86_wr8(C, a[0] + i, x86_rd8(C, a[1] + i)); return a[0]; }
FN(m_memmove) {
    uint32_t i;
    if (a[0] < a[1]) for (i = 0; i < a[2]; i++) x86_wr8(C, a[0] + i, x86_rd8(C, a[1] + i));
    else for (i = a[2]; i-- > 0;) x86_wr8(C, a[0] + i, x86_rd8(C, a[1] + i));
    return a[0];
}
FN(m_memset) { uint32_t i; for (i = 0; i < a[2]; i++) x86_wr8(C, a[0] + i, (uint8_t)a[1]); return a[0]; }
FN(m_memcmp) { uint32_t i; for (i = 0; i < a[2]; i++) { int d = (int)x86_rd8(C, a[0] + i) - (int)x86_rd8(C, a[1] + i); if (d) return (uint32_t)d; } return 0; }
FN(m_strlen) { uint32_t n = 0; while (x86_rd8(C, a[0] + n)) n++; return n; }
FN(m_wcslen) { uint32_t n = 0; while (x86_rd16(C, a[0] + n * 2)) n++; return n; }
FN(m_strcmp) { uint32_t i; for (i = 0;; i++) { int x = x86_rd8(C, a[0] + i), y = x86_rd8(C, a[1] + i); if (x != y) return (uint32_t)(x - y); if (!x) return 0; } }
FN(m_strncmp) { uint32_t i; for (i = 0; i < a[2]; i++) { int x = x86_rd8(C, a[0] + i), y = x86_rd8(C, a[1] + i); if (x != y) return (uint32_t)(x - y); if (!x) return 0; } return 0; }
FN(m_wcscmp) { uint32_t i; for (i = 0;; i++) { int x = x86_rd16(C, a[0] + i * 2), y = x86_rd16(C, a[1] + i * 2); if (x != y) return (uint32_t)(x - y); if (!x) return 0; } }
FN(m_wcsicmp) { uint32_t i; for (i = 0;; i++) { int x = x86_rd16(C, a[0] + i * 2), y = x86_rd16(C, a[1] + i * 2); if (x >= 65 && x <= 90) x += 32; if (y >= 65 && y <= 90) y += 32; if (x != y) return (uint32_t)(x - y); if (!x) return 0; } }
FN(m_wcsncmp) { uint32_t i; for (i = 0; i < a[2]; i++) { int x = x86_rd16(C, a[0] + i * 2), y = x86_rd16(C, a[1] + i * 2); if (x != y) return (uint32_t)(x - y); if (!x) return 0; } return 0; }
FN(m_strcpy) { uint32_t i = 0; uint8_t ch; do { ch = x86_rd8(C, a[1] + i); x86_wr8(C, a[0] + i, ch); i++; } while (ch); return a[0]; }
FN(m_wcscpy) { uint32_t i = 0; uint16_t ch; do { ch = x86_rd16(C, a[1] + i * 2); x86_wr16(C, a[0] + i * 2, ch); i++; } while (ch); return a[0]; }
FN(m_strncpy) { uint32_t i; int end = 0; for (i = 0; i < a[2]; i++) { uint8_t ch = end ? 0 : x86_rd8(C, a[1] + i); if (!ch) end = 1; x86_wr8(C, a[0] + i, ch); } return a[0]; }
FN(m_wcsncpy) { uint32_t i; int end = 0; for (i = 0; i < a[2]; i++) { uint16_t ch = end ? 0 : x86_rd16(C, a[1] + i * 2); if (!ch) end = 1; x86_wr16(C, a[0] + i * 2, ch); } return a[0]; }
FN(m_strcat) { uint32_t n = m_strlen(w, a); uint32_t b[2]; b[0] = a[0] + n; b[1] = a[1]; m_strcpy(w, b); return a[0]; }
FN(m_wcscat) { uint32_t n = m_wcslen(w, a); uint32_t b[2]; b[0] = a[0] + n * 2; b[1] = a[1]; m_wcscpy(w, b); return a[0]; }
FN(m_strchr) { uint32_t i; for (i = 0;; i++) { uint8_t ch = x86_rd8(C, a[0] + i); if (ch == (uint8_t)a[1]) return a[0] + i; if (!ch) return 0; } }
FN(m_strrchr) { uint32_t i, r = 0; for (i = 0;; i++) { uint8_t ch = x86_rd8(C, a[0] + i); if (ch == (uint8_t)a[1]) r = a[0] + i; if (!ch) return r; } }
FN(m_wcschr) { uint32_t i; for (i = 0;; i++) { uint16_t ch = x86_rd16(C, a[0] + i * 2); if (ch == (uint16_t)a[1]) return a[0] + i * 2; if (!ch) return 0; } }
FN(m_wcsrchr) { uint32_t i, r = 0; for (i = 0;; i++) { uint16_t ch = x86_rd16(C, a[0] + i * 2); if (ch == (uint16_t)a[1]) r = a[0] + i * 2; if (!ch) return r; } }
FN(m_strcspn) { uint32_t i, j; for (i = 0;; i++) { uint8_t ch = x86_rd8(C, a[0] + i); if (!ch) return i; for (j = 0;; j++) { uint8_t r = x86_rd8(C, a[1] + j); if (!r) break; if (r == ch) return i; } } }
FN(m_strdup) { uint32_t n = m_strlen(w, a) + 1, d = qw_alloc(w, n), i; for (i = 0; i < n; i++) x86_wr8(C, d + i, x86_rd8(C, a[0] + i)); return d; }
FN(m_wcsdup) { uint32_t n = m_wcslen(w, a) + 1, d = qw_alloc(w, n * 2), i; for (i = 0; i < n; i++) x86_wr16(C, d + i * 2, x86_rd16(C, a[0] + i * 2)); return d; }
FN(m_strupr) { uint32_t i; for (i = 0;; i++) { uint8_t ch = x86_rd8(C, a[0] + i); if (!ch) break; x86_wr8(C, a[0] + i, (uint8_t)toupper(ch)); } return a[0]; }
FN(m_strlwr) { uint32_t i; for (i = 0;; i++) { uint8_t ch = x86_rd8(C, a[0] + i); if (!ch) break; x86_wr8(C, a[0] + i, (uint8_t)tolower(ch)); } return a[0]; }
FN(m_toupper) { UNUSED; return (uint32_t)toupper((int)(a[0] & 0xFF)); }
FN(m_tolower) { UNUSED; return (uint32_t)tolower((int)(a[0] & 0xFF)); }
FN(m_towupper) { UNUSED; return a[0] < 128 ? (uint32_t)toupper((int)a[0]) : a[0]; }
FN(m_towlower) { UNUSED; return a[0] < 128 ? (uint32_t)tolower((int)a[0]) : a[0]; }
FN(m_isdigit) { UNUSED; return a[0] < 256 && isdigit((int)a[0]); }
FN(m_isspace) { UNUSED; return a[0] < 256 && isspace((int)a[0]); }
FN(m_isalpha) { UNUSED; return a[0] < 256 && isalpha((int)a[0]); }
FN(m_atoi) { char b[64]; qw_rd_str(w, a[0], b, 64); return (uint32_t)atoi(b); }
FN(m_wtoi) { wchar16 t[64]; char b[64]; qw_rd_wstr(w, a[0], t, 64); w2a(b, t, 64); return (uint32_t)atoi(b); }
FN(m_atof) { char b[128]; qw_rd_str(w, a[0], b, 128); return retd(w, qf_strtod(b, NULL)); }
FN(m_wcstod) {
    wchar16 t[128]; char b[128], *e; double d;
    qw_rd_wstr(w, a[0], t, 128); w2a(b, t, 128);
    d = qf_strtod(b, &e);
    if (a[1]) WR32(a[1], a[0] + (uint32_t)(e - b) * 2);
    return retd(w, d);
}
FN(m_rand) { w->rand_seed = w->rand_seed * 214013u + 2531011u; return (w->rand_seed >> 16) & 0x7FFF; }
FN(m_srand) { w->rand_seed = a[0]; return 0; }
FN(m_time) { uint32_t t = (uint32_t)time(NULL); if (a[0]) WR32(a[0], t); return t; }
FN(m_clock) { UNUSED; return (uint32_t)(w->be->now(w) & 0xFFFFFFFFu); }
FN(m_signal) { UNUSED; return 0; }
FN(m_sprintf) { return (uint32_t)fmt_to(w, a[0], -1, 0, a[1], 0, C->r[X86_ESP] + 12, 0); }
FN(m_swprintf) {   /* msvcrt swprintf(buf, fmt, ...) (no count) */
    return (uint32_t)fmt_to(w, a[0], -1, 1, a[1], 1, C->r[X86_ESP] + 12, 0);
}
FN(m_snprintf) { return (uint32_t)fmt_to(w, a[0], (int)a[1], 0, a[2], 0, C->r[X86_ESP] + 16, 1); }
FN(m_snwprintf) { return (uint32_t)fmt_to(w, a[0], (int)a[1], 1, a[2], 1, C->r[X86_ESP] + 16, 1); }
FN(m_vsnprintf) { return (uint32_t)fmt_to(w, a[0], (int)a[1], 0, a[2], 0, a[3], 1); }
FN(m_vsnwprintf) { return (uint32_t)fmt_to(w, a[0], (int)a[1], 1, a[2], 1, a[3], 1); }
FN(m_vsprintf) { return (uint32_t)fmt_to(w, a[0], -1, 0, a[1], 0, a[2], 0); }
FN(m_vswprintf) { return (uint32_t)fmt_to(w, a[0], -1, 1, a[1], 1, a[2], 0); }
static void file_out(qwin *w, uint32_t file, const wchar16 *s, int n)
{
    FILE *o = qw_stream_host(w, file);
    int i;
    if (!o) o = (g_iob && file == g_iob + 64) ? stderr : stdout;
    for (i = 0; i < n; i++) fputc(s[i] < 256 ? (int)s[i] : '?', o);
    fflush(o);
}
FN(m_vfprintf) { wbuf o; o.b = g_fbuf; o.n = 0; o.max = 16384; qfmt(w, &o, a[1], 0, a[2]); file_out(w, a[0], g_fbuf, o.n < 16383 ? o.n : 16383); return (uint32_t)o.n; }
FN(m_fprintf) { wbuf o; o.b = g_fbuf; o.n = 0; o.max = 16384; qfmt(w, &o, a[1], 0, C->r[X86_ESP] + 12); file_out(w, a[0], g_fbuf, o.n < 16383 ? o.n : 16383); return (uint32_t)o.n; }
FN(m_printf) { wbuf o; o.b = g_fbuf; o.n = 0; o.max = 16384; qfmt(w, &o, a[0], 0, C->r[X86_ESP] + 8); file_out(w, 0, g_fbuf, o.n < 16383 ? o.n : 16383); return (uint32_t)o.n; }
FN(m_fwrite) {
    uint32_t n = a[1] * a[2], i;
    FILE *o = qw_stream_host(w, a[3]);
    if (!o) o = (g_iob && a[3] == g_iob + 64) ? stderr : stdout;
    for (i = 0; i < n; i++) fputc(x86_rd8(C, a[0] + i), o);
    fflush(o);
    return a[2];
}
FN(m_fputc) { FILE *o = qw_stream_host(w, a[1]); if (!o) o = (g_iob && a[1] == g_iob + 64) ? stderr : stdout; fputc((int)(a[0] & 0xFF), o); return a[0]; }
FN(m_fflush) { FILE *o = qw_stream_host(w, a[0]); UNUSED; fflush(o ? o : stdout); return 0; }
FN(m_puts) { char b[1024]; qw_rd_str(w, a[0], b, sizeof(b)); puts(b); return 0; }
FN(m_swscanf) { return (uint32_t)qscan(w, a[0], 1, a[1], 1, C->r[X86_ESP] + 12); }
FN(m_sscanf) { return (uint32_t)qscan(w, a[0], 0, a[1], 0, C->r[X86_ESP] + 12); }
FN(m_finite) { double d = dbl(a, 0); UNUSED; return isfinite(d) ? 1 : 0; }
FN(m_isnan) { double d = dbl(a, 0); UNUSED; return isnan(d) ? 1 : 0; }
#define MATH1(nm, f) FN(nm) { return retd(w, f(dbl(a, 0))); }
MATH1(m_sin, sin) MATH1(m_cos, cos) MATH1(m_tan, tan) MATH1(m_asin, asin) MATH1(m_acos, acos) MATH1(m_atan, atan)
MATH1(m_sinh, sinh) MATH1(m_cosh, cosh) MATH1(m_tanh, tanh) MATH1(m_exp, exp) MATH1(m_log, log) MATH1(m_log10, log10)
static double q_sqrt(double x) { return qm_sqrt(x); }
static double q_fabs(double x) { return __builtin_fabs(x); }
MATH1(m_sqrt, q_sqrt) MATH1(m_floor, floor) MATH1(m_ceil, ceil) MATH1(m_fabs, q_fabs)
FN(m_pow) { return retd(w, pow(dbl(a, 0), dbl(a, 2))); }
FN(m_fmod) { return retd(w, fmod(dbl(a, 0), dbl(a, 2))); }
FN(m_ldexp) { return retd(w, ldexp(dbl(a, 0), (int)(int32_t)a[2])); }
FN(m_atan2) { return retd(w, atan2(dbl(a, 0), dbl(a, 2))); }
FN(m_modf) {
    double ip, fr = modf(dbl(a, 0), &ip); uint64_t u;
    memcpy(&u, &ip, 8); WR32(a[2], (uint32_t)u); WR32(a[2] + 4, (uint32_t)(u >> 32));
    return retd(w, fr);
}
FN(m_ftol) { double d = x86_fpu_pop(C); int64_t v = fabs(d) < 9.2e18 ? x86_d2i64(d) : INT64_MIN; C->r[X86_EDX] = (uint32_t)((uint64_t)v >> 32); return (uint32_t)v; }
FN(m_CIpow) { double y = x86_fpu_pop(C), x = x86_fpu_pop(C); return retd(w, pow(x, y)); }
FN(m_bsearch) {
    uint32_t lo = 0, hi = a[2];
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, el = a[1] + mid * a[3];
        int32_t r = (int32_t)qw_call(w, a[4], 2, a[0], el, 0, 0);
        if (!r) return el;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}
FN(m_qsort) {
    /* insertion sort through the guest comparator (n is small in practice) */
    uint32_t n = a[1], sz = a[2], i, j, k, tmp = qw_alloc(w, sz);
    for (i = 1; i < n; i++) {
        for (k = 0; k < sz; k++) x86_wr8(C, tmp + k, x86_rd8(C, a[0] + i * sz + k));
        j = i;
        while (j > 0 && (int32_t)qw_call(w, a[3], 2, a[0] + (j - 1) * sz, tmp, 0, 0) > 0) {
            for (k = 0; k < sz; k++) x86_wr8(C, a[0] + j * sz + k, x86_rd8(C, a[0] + (j - 1) * sz + k));
            j--;
        }
        for (k = 0; k < sz; k++) x86_wr8(C, a[0] + j * sz + k, x86_rd8(C, tmp + k));
    }
    qw_free(w, tmp);
    return 0;
}
FN(m_errno) { if (!g_errno) g_errno = qw_data(w, 4); UNUSED; return g_errno; }
FN(m_p_fmode) { return qw_data_import(w, 0, "_fmode"); (void)a; }
FN(m_p_initenv) { return qw_data_import(w, 0, "__initenv"); (void)a; }
FN(m_p_mb_cur_max) { return qw_data_import(w, 0, "__mb_cur_max"); (void)a; }
FN(m_islower) { UNUSED; return (uint32_t)islower((int)(a[0] & 0xFF)); }
FN(m_isupper) { UNUSED; return (uint32_t)isupper((int)(a[0] & 0xFF)); }
FN(m_get_osfhandle) { UNUSED; return a[0]; }
FN(m_setmode) { UNUSED; return 0x8000u; }
FN(m_p_commode) { return qw_data_import(w, 0, "_commode"); (void)a; }
FN(m_p_acmdln) { (void)a; return qw_data_import(w, 0, "_acmdln"); }
FN(m_p_wcmdln) { (void)a; return qw_data_import(w, 0, "_wcmdln"); }
FN(m_ismbblead) { UNUSED; return 0; }
FN(m_iob_func) { return qw_data_import(w, 0, "_iob"); (void)a; }
FN(m_controlfp) { UNUSED; return 0x0009001Fu; }

/* ====================================================================== kernel32 */
FN(k_ExitProcess) { return m_exit(w, a); }
FN(k_TerminateProcess) { w->exit_code = a[1]; w->exiting = 1; return 1; }
FN(k_GetLastError) { UNUSED; return w->last_error; }
FN(k_SetLastError) { w->last_error = a[0]; return 0; }
FN(k_GetCurrentProcess) { UNUSED; return 0xFFFFFFFFu; }
FN(k_GetCurrentThread) { UNUSED; return 0xFFFFFFFEu; }
FN(k_GetCurrentProcessId) { UNUSED; return 0x100; }
FN(k_GetCurrentThreadId) { UNUSED; return 0x104; }
FN(k_one) { UNUSED; return 1; }
FN(k_zero) { UNUSED; return 0; }
FN(k_GetTickCount) { UNUSED; return (uint32_t)w->be->now(w); }
FN(k_Sleep) { (void)a; if (w->be->wait) { qevent ev; (void)ev; } return 0; }
FN(k_QueryPerformanceCounter) { uint64_t t = w->be->now(w) * 1000u; WR32(a[0], (uint32_t)t); WR32(a[0] + 4, (uint32_t)(t >> 32)); return 1; }
FN(k_QueryPerformanceFrequency) { WR32(a[0], 1000000u); WR32(a[0] + 4, 0); return 1; }
FN(k_GetSystemTimeAsFileTime) {
    uint64_t ft = ((uint64_t)time(NULL) + 11644473600ull) * 10000000ull;
    WR32(a[0], (uint32_t)ft); WR32(a[0] + 4, (uint32_t)(ft >> 32)); return 0;
}
static void systime(qwin *w, uint32_t p, int local)
{
    time_t t = time(NULL);
    struct tm tmv, *tm = local ? localtime_r(&t, &tmv) : gmtime_r(&t, &tmv);
    x86_wr16(C, p, (uint16_t)(tm->tm_year + 1900)); x86_wr16(C, p + 2, (uint16_t)(tm->tm_mon + 1));
    x86_wr16(C, p + 4, (uint16_t)tm->tm_wday); x86_wr16(C, p + 6, (uint16_t)tm->tm_mday);
    x86_wr16(C, p + 8, (uint16_t)tm->tm_hour); x86_wr16(C, p + 10, (uint16_t)tm->tm_min);
    x86_wr16(C, p + 12, (uint16_t)tm->tm_sec); x86_wr16(C, p + 14, (uint16_t)(w->be->now(w) % 1000));
}
FN(k_GetLocalTime) { systime(w, a[0], 1); return 0; }
FN(k_GetSystemTime) { systime(w, a[0], 0); return 0; }
FN(k_GetStartupInfo) { uint32_t i; for (i = 0; i < 68; i += 4) WR32(a[0] + i, 0); WR32(a[0], 68); return 0; }
FN(k_GetVersion) { UNUSED; return 0x0A280105u; }    /* 5.1 build 2600 */
FN(k_GetVersionEx) {
    static const char sp[] = "Service Pack 3";
    uint32_t i, wide = RD32(a[0]) >= 276;
    WR32(a[0] + 4, 5); WR32(a[0] + 8, 1); WR32(a[0] + 12, 2600); WR32(a[0] + 16, 2);
    for (i = 0; i < sizeof(sp); i++) { if (wide) x86_wr16(C, a[0] + 20 + i * 2, (uint8_t)sp[i]); else x86_wr8(C, a[0] + 20 + i, (uint8_t)sp[i]); }
    return 1;
}
static uint32_t modhandle_for(const char *name)
{
    int i;
    char low[64];
    size_t k;
    for (k = 0; name[k] && k < 63; k++) low[k] = (char)tolower((unsigned char)name[k]);
    low[k] = 0;
    for (i = 0; known_dlls[i]; i++) {
        size_t L = strlen(known_dlls[i]);
        if (!strncmp(low, known_dlls[i], L) && (low[L] == 0 || low[L] == '.')) return QW_MODBASE + (uint32_t)i * 0x10000u;
    }
    return 0;
}
/* module handles: HLE DLLs -> QW_MODBASE + i*0x10000; program and its own DLLs -> image base */
static uint32_t module_handle(qwin *w, const char *n, int load)
{
    uint32_t h = modhandle_for(n);
    qmod *m;
    if (h) return h;
    m = load ? pe_load_dll(w, n) : pe_module_by_name(w, n);
    if (m) return m->base;
    if (load) { qw_log(w, "LoadLibrary(%s): not available in QRX (program continues without it)", n); w->last_error = 126; }
    return 0;
}
FN(k_GetModuleHandleA) { char n[64]; if (!a[0]) return w->image_base; qw_rd_str(w, a[0], n, 64); return module_handle(w, n, 0); }
FN(k_GetModuleHandleW) { wchar16 t[64]; char n[64]; if (!a[0]) return w->image_base; qw_rd_wstr(w, a[0], t, 64); w2a(n, t, 64); return module_handle(w, n, 0); }
FN(k_LoadLibraryA) { char n[128]; qw_rd_str(w, a[0], n, 128); return module_handle(w, n, 1); }
FN(k_LoadLibraryW) { wchar16 t[128]; char n[128]; qw_rd_wstr(w, a[0], t, 128); w2a(n, t, 128); return module_handle(w, n, 1); }
FN(k_QrxLauncherPlay) { UNUSED; return 1; }
FN(k_GetProcAddress) {
    char n[96];
    uint32_t idx = (a[0] - QW_MODBASE) / 0x10000u;
    const qhle *e;
    qmod *m = pe_find_module(w, a[0]);
    if (m) {
        if (a[1] < 0x10000u) return pe_export(w, m, NULL, a[1]);
        qw_rd_str(w, a[1], n, sizeof(n));
        /* Doom95's launcher is a property-sheet dialog the game only needs to
         * have closed with "play": the command line carries the settings. */
        if (!strcmp(n, "_Launcher@4")) return qw_thunk(w, "kernel32", "QrxLauncherPlay");
        return pe_export(w, m, n, 0);
    }
    if (a[0] < QW_MODBASE || idx >= QW_NKNOWN) return 0;
    if (a[1] < 0x10000u) return 0;
    qw_rd_str(w, a[1], n, 64);
    e = qw_find_hle(known_dlls[idx], n);
    if (!e && qw_host_has(w, n)) return qw_thunk(w, known_dlls[idx], n);
    if (!e) { if (w->verbose) qw_log(w, "GetProcAddress(%s, %s): not implemented", known_dlls[idx], n); return 0; }
    return qw_thunk(w, known_dlls[idx], n);
}
FN(k_GetModuleFileNameW) {
    const char *b = strrchr(w->exe_path, '/');
    char p[300]; wchar16 t[300];
    int n;
    snprintf(p, sizeof(p), "C:\\%s", b ? b + 1 : w->exe_path);
    a2w(t, p, 300);
    n = qw_wr_wstr(w, a[1], t, (int)a[2]);
    return (uint32_t)n;
}
FN(k_GetModuleFileNameA) {
    const char *b = strrchr(w->exe_path, '/');
    char p[300]; uint32_t i;
    snprintf(p, sizeof(p), "C:\\%s", b ? b + 1 : w->exe_path);
    for (i = 0; p[i] && i + 1 < a[2]; i++) x86_wr8(C, a[1] + i, (uint8_t)p[i]);
    if (a[2]) x86_wr8(C, a[1] + i, 0);
    return i;
}
FN(k_GetCommandLineW) { qw_data_import(w, 0, "_wcmdln"); return RD32(g_wcmdln); }
FN(k_GetCommandLineA) { qw_data_import(w, 0, "_acmdln"); return RD32(g_acmdln); }
FN(k_GetLocaleInfoW) {
    const char *v = "";
    int de = (w->lang & 0x3FF) == 7;
    wchar16 t[64];
    switch (a[1] & 0xFFFF) {
    case 0x0E: v = de ? "," : "."; break;          /* LOCALE_SDECIMAL */
    case 0x0F: v = de ? "." : ","; break;          /* LOCALE_STHOUSAND */
    case 0x10: v = "3;0"; break;
    case 0x0C: v = ";"; break;                      /* LOCALE_SLIST */
    case 0x1001: v = de ? "Deutsch" : "English"; break;
    case 0x0059: v = de ? "de" : "en"; break;
    case 0x005A: v = de ? "DE" : "US"; break;
    default: v = "0"; break;
    }
    a2w(t, v, 64);
    if (!a[3]) return (uint32_t)wlen16(t) + 1;
    return (uint32_t)qw_wr_wstr(w, a[2], t, (int)a[3]) + 1;
}
FN(k_GetUserDefaultLCID) { UNUSED; return w->lang; }
FN(k_GetACP) { UNUSED; return 1252; }
FN(k_GetProfileIntW) { UNUSED; return a[2]; }
FN(k_GetProfileStringW) { wchar16 t[256]; int n = qw_rd_wstr(w, a[2], t, 256); qw_wr_wstr(w, a[3], t, (int)a[4]); return (uint32_t)n; }
FN(k_GlobalAlloc) { return qw_alloc(w, a[1] ? a[1] : 1); }
FN(k_LocalAlloc) { return qw_alloc(w, a[1] ? a[1] : 1); }
FN(k_GlobalFree) { qw_free(w, a[0]); return 0; }
FN(k_GlobalLock) { UNUSED; return a[0]; }
FN(k_GlobalUnlock) { UNUSED; return 1; }
FN(k_GlobalSize) { return qw_size(w, a[0]); }
FN(k_LocalReAlloc) { uint32_t b[2]; b[0] = a[0]; b[1] = a[1]; return m_realloc(w, b); }
FN(k_HeapCreate) { UNUSED; return 0x00150000u; }
FN(k_GetProcessHeap) { UNUSED; return 0x00150000u; }
FN(k_HeapAlloc) { return qw_alloc(w, a[2]); }
FN(k_HeapFree) { qw_free(w, a[2]); return 1; }
FN(k_HeapReAlloc) { uint32_t b[2]; b[0] = a[2]; b[1] = a[3]; return m_realloc(w, b); }
FN(k_HeapSize) { return qw_size(w, a[2]); }
FN(k_VirtualAlloc) {
    uint32_t n = (a[1] + 0xFFFu) & ~0xFFFu, r = qw_alloc(w, n + 0x1000);
    return (r + 0xFFFu) & ~0xFFFu;
}
FN(k_VirtualFree) { UNUSED; return 1; }
FN(k_VirtualProtect) { if (a[3]) WR32(a[3], 0x04); UNUSED; return 1; }
FN(k_VirtualQuery) {
    uint32_t p = a[0] & ~0xFFFu, i;
    for (i = 0; i < 28; i += 4) WR32(a[1] + i, 0);
    WR32(a[1], p); WR32(a[1] + 4, w->image_base); WR32(a[1] + 8, 0x04); WR32(a[1] + 12, 0x1000);
    WR32(a[1] + 16, x86_is_mapped(C, p) ? 0x1000u : 0x10000u); WR32(a[1] + 20, 0x04); WR32(a[1] + 24, 0x1000000u);
    return 28;
}
FN(k_lstrlenW) { return a[0] ? m_wcslen(w, a) : 0; }
FN(k_lstrlenA) { return a[0] ? m_strlen(w, a) : 0; }
FN(k_lstrcpyW) { return m_wcscpy(w, a); }
FN(k_lstrcpyA) { return m_strcpy(w, a); }
FN(k_lstrcatW) { return m_wcscat(w, a); }
FN(k_lstrcmpW) { return m_wcscmp(w, a); }
FN(k_lstrcmpiW) { return m_wcsicmp(w, a); }
FN(k_lstrcpynW) {
    uint32_t i;
    if (!a[2]) return a[0];
    for (i = 0; i + 1 < a[2]; i++) { uint16_t ch = x86_rd16(C, a[1] + i * 2); if (!ch) break; x86_wr16(C, a[0] + i * 2, ch); }
    x86_wr16(C, a[0] + i * 2, 0);
    return a[0];
}
FN(k_MultiByteToWideChar) {
    int32_t n = (int32_t)a[3];
    uint32_t i, cnt = 0;
    if (n < 0) { n = 0; while (x86_rd8(C, a[2] + (uint32_t)n)) n++; n++; }
    for (i = 0; i < (uint32_t)n; i++) { if (a[5] && cnt < a[5]) x86_wr16(C, a[4] + cnt * 2, x86_rd8(C, a[2] + i)); cnt++; }
    return a[5] && cnt > a[5] ? 0 : cnt;
}
FN(k_WideCharToMultiByte) {
    int32_t n = (int32_t)a[3];
    uint32_t i, cnt = 0;
    if (n < 0) { n = 0; while (x86_rd16(C, a[2] + (uint32_t)n * 2)) n++; n++; }
    for (i = 0; i < (uint32_t)n; i++) { uint16_t ch = x86_rd16(C, a[2] + i * 2); if (a[5] && cnt < a[5]) x86_wr8(C, a[4] + cnt, ch < 256 ? (uint8_t)ch : '?'); cnt++; }
    return a[5] && cnt > a[5] ? 0 : cnt;
}
FN(k_GetEnvironmentVariableA) { UNUSED; return 0; }
FN(k_GetEnvironmentVariableW) { UNUSED; return 0; }
FN(k_SetUnhandledExceptionFilter) { uint32_t o = w->unhandled_filter; w->unhandled_filter = a[0]; return o; }
FN(k_UnhandledExceptionFilter) { UNUSED; return 1; }
FN(k_TlsAlloc) { static uint32_t n; UNUSED; return n < 64 ? n++ : 0xFFFFFFFFu; }
FN(k_TlsGetValue) { return a[0] < 64 ? RD32(QW_TEB + 0x1000 + a[0] * 4) : 0; }
FN(k_TlsSetValue) { if (a[0] < 64) WR32(QW_TEB + 0x1000 + a[0] * 4, a[1]); return 1; }
FN(k_GetStdHandle) { UNUSED; return 0x10 + (a[0] & 0xFF); }
FN(k_WriteFile) {
    uint32_t i;
    for (i = 0; i < a[2]; i++) fputc(x86_rd8(C, a[1] + i), a[0] == 0x10 + 0xF4 ? stderr : stdout);
    if (a[3]) WR32(a[3], a[2]);
    return 1;
}
FN(k_OutputDebugStringA) { char b[512]; qw_rd_str(w, a[0], b, 512); if (w->verbose) qw_log(w, "debug: %s", b); return 0; }
FN(k_OutputDebugStringW) { wchar16 t[512]; char b[512]; qw_rd_wstr(w, a[0], t, 512); w2a(b, t, 512); if (w->verbose) qw_log(w, "debug: %s", b); return 0; }
FN(k_IsDebuggerPresent) { UNUSED; return 0; }
FN(k_GetSystemInfo) {
    uint32_t i;
    for (i = 0; i < 36; i += 4) WR32(a[0] + i, 0);
    WR32(a[0] + 4, 0x1000); WR32(a[0] + 8, 0x10000); WR32(a[0] + 12, 0x7FFEFFFF); WR32(a[0] + 16, 1); WR32(a[0] + 20, 1);
    WR32(a[0] + 24, 586); WR32(a[0] + 28, 0x10000); x86_wr16(C, a[0] + 32, 6);
    return 0;
}
FN(k_RaiseException) { qw_log(w, "RaiseException(%08x): structured exceptions are not supported", a[0]); w->exit_code = a[0]; w->exiting = 1; return 0; }
FN(k_FreeLibrary) { UNUSED; return 1; }
FN(k_IsBadReadPtr) { UNUSED; return !x86_is_mapped(C, a[0]); }
FN(k_FindResourceW) { uint32_t d, s; if (!res_find_in(w, a[0], a[2], a[1], &d, &s)) return 0; { uint32_t h = qw_alloc(w, 8); WR32(h, d); WR32(h + 4, s); return h; } }
FN(k_LoadResource) { UNUSED; return a[1] ? RD32(a[1]) : 0; }
FN(k_LockResource) { UNUSED; return a[0]; }
FN(k_SizeofResource) { UNUSED; return a[1] ? RD32(a[1] + 4) : 0; }
FN(k_InterlockedIncrement) { uint32_t v = RD32(a[0]) + 1; WR32(a[0], v); return v; }
FN(k_InterlockedDecrement) { uint32_t v = RD32(a[0]) - 1; WR32(a[0], v); return v; }
FN(k_InterlockedExchange) { uint32_t v = RD32(a[0]); WR32(a[0], a[1]); return v; }
FN(k_InterlockedCompareExchange) { uint32_t v = RD32(a[0]); if (v == a[2]) WR32(a[0], a[1]); return v; }
FN(k_GetTimeZoneInformation) { uint32_t i; for (i = 0; i < 172; i += 4) WR32(a[0] + i, 0); UNUSED; return 0; }
/* synchronisation objects.  qrx-win32 runs one guest thread, so nothing can ever
 * release an object while the thread waits: a wait that cannot be satisfied
 * returns WAIT_TIMEOUT for a finite timeout and is reported (and treated as
 * signalled) for INFINITE instead of hanging. */
#define QW_SYNC_BASE 0x00005000u
#define QW_MAXSYNC 64
enum { SY_FREE, SY_MUTEX, SY_SEM, SY_EVENT };
static struct { int type; int32_t count, max; int manual; } g_sync[QW_MAXSYNC];
static uint32_t sync_new(int type, int32_t count, int32_t max, int manual)
{
    int i;
    for (i = 0; i < QW_MAXSYNC; i++) if (g_sync[i].type == SY_FREE) {
        g_sync[i].type = type; g_sync[i].count = count; g_sync[i].max = max; g_sync[i].manual = manual;
        return QW_SYNC_BASE + (uint32_t)i * 4u;
    }
    return 0;
}
static int sync_idx(uint32_t h) { uint32_t i = (h - QW_SYNC_BASE) / 4u; return (h >= QW_SYNC_BASE && i < QW_MAXSYNC && g_sync[i].type != SY_FREE && !(h & 3)) ? (int)i : -1; }
FN(k_CreateMutex) { UNUSED; return sync_new(SY_MUTEX, a[1] ? 0 : 1, 1, 0); }          /* count 1 = free */
FN(k_CreateSemaphore) { UNUSED; return sync_new(SY_SEM, (int32_t)a[1], (int32_t)a[2], 0); }
FN(k_CreateEvent) { UNUSED; return sync_new(SY_EVENT, a[2] ? 1 : 0, 1, a[1] != 0); }
FN(k_ReleaseMutex) { int i = sync_idx(a[0]); UNUSED; if (i < 0 || g_sync[i].type != SY_MUTEX) return 0; g_sync[i].count = 1; return 1; }
FN(k_ReleaseSemaphore) {
    int i = sync_idx(a[0]);
    if (i < 0 || g_sync[i].type != SY_SEM) return 0;
    if (a[2]) WR32(a[2], (uint32_t)g_sync[i].count);
    if (g_sync[i].count + (int32_t)a[1] > g_sync[i].max) return 0;
    g_sync[i].count += (int32_t)a[1];
    return 1;
}
FN(k_SetEvent) { int i = sync_idx(a[0]); UNUSED; if (i < 0) return 0; g_sync[i].count = 1; return 1; }
FN(k_ResetEvent) { int i = sync_idx(a[0]); UNUSED; if (i < 0) return 0; g_sync[i].count = 0; return 1; }
FN(k_PulseEvent) { int i = sync_idx(a[0]); UNUSED; if (i < 0) return 0; g_sync[i].count = 0; return 1; }
static uint32_t sync_wait(qwin *w, uint32_t h, uint32_t ms)
{
    int i = sync_idx(h);
    if (i < 0) return 0;                                /* unknown handles (threads, processes...): signalled */
    if (g_sync[i].count > 0) {
        if (g_sync[i].type != SY_EVENT || !g_sync[i].manual) g_sync[i].count--;
        if (g_sync[i].type == SY_MUTEX) g_sync[i].count = 0;
        return 0;                                       /* WAIT_OBJECT_0 */
    }
    if (g_sync[i].type == SY_MUTEX) return 0;           /* recursive acquire by the only thread */
    if (ms != 0xFFFFFFFFu) return 0x102;                /* WAIT_TIMEOUT */
    qw_log(w, "WaitForSingleObject(%08x, INFINITE) on an unsignalled object: single-threaded, treated as signalled", h);
    return 0;
}
FN(k_WaitForSingleObject) { return sync_wait(w, a[0], a[1]); }
FN(k_WaitForMultipleObjects) {
    uint32_t i;
    for (i = 0; i < a[0] && i < 64; i++) {
        int k = sync_idx(RD32(a[1] + i * 4));
        if (k < 0 || g_sync[k].count > 0 || g_sync[k].type == SY_MUTEX) {
            if (!a[2]) return sync_wait(w, RD32(a[1] + i * 4), a[3]) == 0 ? i : 0x102;
        } else if (a[2]) return a[3] != 0xFFFFFFFFu ? 0x102 : 0;
    }
    if (a[2]) { for (i = 0; i < a[0] && i < 64; i++) sync_wait(w, RD32(a[1] + i * 4), 0); return 0; }
    return a[3] != 0xFFFFFFFFu ? 0x102 : 0;
}
FN(k_CloseHandle) { int i = sync_idx(a[0]); UNUSED; if (i >= 0) g_sync[i].type = SY_FREE; else qw_file_close(w, a[0]); return 1; }

/* ====================================================================== advapi32: in-memory registry */
typedef struct qreg { char path[200]; char name[64]; uint32_t type, size; uint8_t data[512]; struct qreg *next; } qreg;
typedef struct qkey { int used; char path[200]; } qkey;
static qkey g_keys[64];

static void key_path(qwin *w, uint32_t hk, uint32_t sub, char *out, int n)
{
    wchar16 t[200]; char s[200];
    const char *root = hk == 0x80000001u ? "HKCU" : hk == 0x80000002u ? "HKLM" : hk == 0x80000000u ? "HKCR" : NULL;
    if (!root && hk >= 0x1000 && hk < 0x1000 + 64 && g_keys[hk - 0x1000].used) root = g_keys[hk - 0x1000].path;
    if (!root) root = "?";
    qw_rd_wstr(w, sub, t, 200); w2a(s, t, 200);
    if (s[0]) snprintf(out, (size_t)n, "%s\\%s", root, s); else snprintf(out, (size_t)n, "%s", root);
    { char *p; for (p = out; *p; p++) *p = (char)tolower((unsigned char)*p); }
}
static uint32_t key_open(const char *p)
{
    int i;
    for (i = 0; i < 64; i++) if (!g_keys[i].used) { g_keys[i].used = 1; snprintf(g_keys[i].path, sizeof(g_keys[i].path), "%s", p); return 0x1000u + (uint32_t)i; }
    return 0;
}
static int key_exists(qwin *w, const char *p)
{
    qreg *r;
    size_t L = strlen(p);
    for (r = w->reg; r; r = r->next) if (!strncmp(r->path, p, L) && (r->path[L] == 0 || r->path[L] == '\\')) return 1;
    return 0;
}
FN(a_RegOpenKeyExW) { char p[200]; key_path(w, a[0], a[1], p, 200); if (!key_exists(w, p)) return 2; WR32(a[4], key_open(p)); return 0; }
FN(a_RegCreateKeyExW) {
    char p[200]; qreg *r;
    key_path(w, a[0], a[1], p, 200);
    if (a[8]) WR32(a[8], key_exists(w, p) ? 2 : 1);
    if (!key_exists(w, p)) { r = (qreg *)calloc(1, sizeof(qreg)); if (r) { snprintf(r->path, sizeof(r->path), "%s", p); r->next = w->reg; w->reg = r; } }
    WR32(a[7], key_open(p));
    return 0;
}
FN(a_RegCloseKey) { if (a[0] >= 0x1000 && a[0] < 0x1000 + 64) g_keys[a[0] - 0x1000].used = 0; UNUSED; return 0; }
static qreg *val_find(qwin *w, uint32_t hk, uint32_t name)
{
    char p[200], n[64]; wchar16 t[64]; qreg *r;
    key_path(w, hk, 0, p, 200);
    qw_rd_wstr(w, name, t, 64); w2a(n, t, 64);
    { char *q; for (q = n; *q; q++) *q = (char)tolower((unsigned char)*q); }
    for (r = w->reg; r; r = r->next) if (!strcmp(r->path, p) && !strcmp(r->name, n) && r->type) return r;
    return NULL;
}
FN(a_RegQueryValueExW) {
    qreg *r = val_find(w, a[0], a[1]);
    uint32_t i;
    if (!r) return 2;
    if (a[3]) WR32(a[3], r->type);
    if (a[5]) {
        uint32_t cap = RD32(a[5]);
        WR32(a[5], r->size);
        if (a[4]) { if (cap < r->size) return 234; for (i = 0; i < r->size; i++) x86_wr8(C, a[4] + i, r->data[i]); }
    }
    return 0;
}
FN(a_RegSetValueExW) {
    qreg *r = val_find(w, a[0], a[1]);
    uint32_t i;
    if (!r) {
        char n[64]; wchar16 t[64];
        r = (qreg *)calloc(1, sizeof(qreg));
        if (!r) return 8;
        key_path(w, a[0], 0, r->path, 200);
        qw_rd_wstr(w, a[1], t, 64); w2a(n, t, 64);
        { char *q; for (q = n; *q; q++) *q = (char)tolower((unsigned char)*q); }
        snprintf(r->name, sizeof(r->name), "%s", n);
        r->next = w->reg; w->reg = r;
    }
    r->type = a[3] ? a[3] : 1;
    r->size = a[5] > sizeof(r->data) ? (uint32_t)sizeof(r->data) : a[5];
    for (i = 0; i < r->size; i++) r->data[i] = x86_rd8(C, a[4] + i);
    return 0;
}

/* ====================================================================== tables */
#define K(n, f, c) { "kernel32", n, c, f }
const qhle qw_kernel32[] = {
    K("ExitProcess", k_ExitProcess, 1), K("TerminateProcess", k_TerminateProcess, 2),
    K("GetLastError", k_GetLastError, 0), K("SetLastError", k_SetLastError, 1),
    K("GetCurrentProcess", k_GetCurrentProcess, 0), K("GetCurrentThread", k_GetCurrentThread, 0),
    K("GetCurrentProcessId", k_GetCurrentProcessId, 0), K("GetCurrentThreadId", k_GetCurrentThreadId, 0),
    K("InitializeCriticalSection", k_zero, 1), K("InitializeCriticalSectionAndSpinCount", k_one, 2), K("DeleteCriticalSection", k_zero, 1),
    K("EnterCriticalSection", k_zero, 1), K("LeaveCriticalSection", k_zero, 1), K("TryEnterCriticalSection", k_one, 1),
    K("GetTickCount", k_GetTickCount, 0), K("Sleep", k_Sleep, 1), K("SleepEx", k_zero, 2),
    K("QueryPerformanceCounter", k_QueryPerformanceCounter, 1), K("QueryPerformanceFrequency", k_QueryPerformanceFrequency, 1),
    K("GetSystemTimeAsFileTime", k_GetSystemTimeAsFileTime, 1), K("GetLocalTime", k_GetLocalTime, 1), K("GetSystemTime", k_GetSystemTime, 1),
    K("GetStartupInfoW", k_GetStartupInfo, 1), K("GetStartupInfoA", k_GetStartupInfo, 1),
    K("GetVersion", k_GetVersion, 0), K("GetVersionExW", k_GetVersionEx, 1), K("GetVersionExA", k_GetVersionEx, 1),
    K("GetModuleHandleA", k_GetModuleHandleA, 1), K("GetModuleHandleW", k_GetModuleHandleW, 1),
    K("LoadLibraryA", k_LoadLibraryA, 1), K("LoadLibraryW", k_LoadLibraryW, 1), K("LoadLibraryExW", k_LoadLibraryW, 3), K("FreeLibrary", k_FreeLibrary, 1),
    K("GetProcAddress", k_GetProcAddress, 2),
    K("GetModuleFileNameW", k_GetModuleFileNameW, 3), K("GetModuleFileNameA", k_GetModuleFileNameA, 3),
    K("GetCommandLineW", k_GetCommandLineW, 0), K("GetCommandLineA", k_GetCommandLineA, 0),
    K("GetLocaleInfoW", k_GetLocaleInfoW, 4), K("GetUserDefaultLCID", k_GetUserDefaultLCID, 0), K("GetUserDefaultLangID", k_GetUserDefaultLCID, 0),
    K("GetACP", k_GetACP, 0),
    K("GetProfileIntW", k_GetProfileIntW, 3), K("GetProfileStringW", k_GetProfileStringW, 5), K("WriteProfileStringW", k_one, 3),
    K("GlobalAlloc", k_GlobalAlloc, 2), K("GlobalFree", k_GlobalFree, 1), K("GlobalLock", k_GlobalLock, 1), K("GlobalUnlock", k_GlobalUnlock, 1), K("GlobalSize", k_GlobalSize, 1),
    K("LocalAlloc", k_LocalAlloc, 2), K("LocalFree", k_GlobalFree, 1), K("LocalReAlloc", k_LocalReAlloc, 3), K("LocalLock", k_GlobalLock, 1), K("LocalUnlock", k_GlobalUnlock, 1),
    K("HeapCreate", k_HeapCreate, 3), K("GetProcessHeap", k_GetProcessHeap, 0), K("HeapAlloc", k_HeapAlloc, 3), K("HeapFree", k_HeapFree, 3),
    K("HeapReAlloc", k_HeapReAlloc, 4), K("HeapSize", k_HeapSize, 3), K("HeapDestroy", k_one, 1),
    K("VirtualAlloc", k_VirtualAlloc, 4), K("VirtualFree", k_VirtualFree, 3), K("VirtualProtect", k_VirtualProtect, 4), K("VirtualQuery", k_VirtualQuery, 3),
    K("lstrlenW", k_lstrlenW, 1), K("lstrlenA", k_lstrlenA, 1), K("lstrcpyW", k_lstrcpyW, 2), K("lstrcpyA", k_lstrcpyA, 2),
    K("lstrcatW", k_lstrcatW, 2), K("lstrcmpW", k_lstrcmpW, 2), K("lstrcmpiW", k_lstrcmpiW, 2), K("lstrcpynW", k_lstrcpynW, 3),
    K("MultiByteToWideChar", k_MultiByteToWideChar, 6), K("WideCharToMultiByte", k_WideCharToMultiByte, 8),
    K("GetEnvironmentVariableA", k_GetEnvironmentVariableA, 3), K("GetEnvironmentVariableW", k_GetEnvironmentVariableW, 3),
    K("SetUnhandledExceptionFilter", k_SetUnhandledExceptionFilter, 1), K("UnhandledExceptionFilter", k_UnhandledExceptionFilter, 1),
    K("TlsAlloc", k_TlsAlloc, 0), K("TlsGetValue", k_TlsGetValue, 1), K("TlsSetValue", k_TlsSetValue, 2), K("TlsFree", k_one, 1),
    K("CreateMutexA", k_CreateMutex, 3), K("CreateMutexW", k_CreateMutex, 3), K("ReleaseMutex", k_ReleaseMutex, 1),
    K("CreateSemaphoreA", k_CreateSemaphore, 4), K("CreateSemaphoreW", k_CreateSemaphore, 4), K("ReleaseSemaphore", k_ReleaseSemaphore, 3),
    K("CreateEventA", k_CreateEvent, 4), K("CreateEventW", k_CreateEvent, 4), K("SetEvent", k_SetEvent, 1), K("ResetEvent", k_ResetEvent, 1),
    K("PulseEvent", k_PulseEvent, 1), K("WaitForSingleObject", k_WaitForSingleObject, 2), K("WaitForMultipleObjects", k_WaitForMultipleObjects, 4),
    K("GetStdHandle", k_GetStdHandle, 1), K("WriteFile", k_WriteFile, 5), K("CloseHandle", k_CloseHandle, 1),
    K("OutputDebugStringA", k_OutputDebugStringA, 1), K("OutputDebugStringW", k_OutputDebugStringW, 1), K("IsDebuggerPresent", k_IsDebuggerPresent, 0),
    K("GetSystemInfo", k_GetSystemInfo, 1), K("RaiseException", k_RaiseException, 4), K("IsBadReadPtr", k_IsBadReadPtr, 2),
    K("QrxLauncherPlay", k_QrxLauncherPlay, 1),
    K("FindResourceW", k_FindResourceW, 3), K("LoadResource", k_LoadResource, 2), K("LockResource", k_LockResource, 1), K("SizeofResource", k_SizeofResource, 2),
    K("InterlockedIncrement", k_InterlockedIncrement, 1), K("InterlockedDecrement", k_InterlockedDecrement, 1),
    K("InterlockedExchange", k_InterlockedExchange, 2), K("InterlockedCompareExchange", k_InterlockedCompareExchange, 3),
    K("GetTimeZoneInformation", k_GetTimeZoneInformation, 1), K("SetErrorMode", k_zero, 1),
    { NULL, NULL, 0, NULL }
};

#define MS(n, f) { "msvcrt", n, -1, f }
#define AD(n, f, c) { "advapi32", n, c, f }
static uint32_t c_InitCommonControls(qwin *w, uint32_t *a) { UNUSED; return 0; }
static uint32_t c_InitCommonControlsEx(qwin *w, uint32_t *a) { UNUSED; return 1; }
/* status bar: a STATIC child along the bottom of the parent (SB_* messages are not interpreted) */
static uint32_t status_window(qwin *w, uint32_t style, uint32_t wtext, uint32_t parent, uint32_t id)
{
    const qhle *cw = qw_find_hle("user32", "CreateWindowExW"), *gr = qw_find_hle("user32", "GetClientRect");
    uint32_t cls = qw_alloc(w, 16), rc = qw_alloc(w, 16), b[16], h;
    static const wchar16 st[] = { 'S', 'T', 'A', 'T', 'I', 'C', 0 };
    int pw = 300, ph = 200;
    memset(b, 0, sizeof(b));
    qw_wr_wstr(w, cls, st, 8);
    b[0] = parent; b[1] = rc;
    if (gr && gr->fn(w, b)) { pw = (int32_t)RD32(rc + 8); ph = (int32_t)RD32(rc + 12); }
    memset(b, 0, sizeof(b));
    b[0] = 0x00020000u;                               /* WS_EX_STATICEDGE */
    b[1] = cls; b[2] = wtext; b[3] = (style | 0x40000000u | 0x10000000u) & ~0x00C00000u;   /* WS_CHILD|WS_VISIBLE */
    b[4] = 0; b[5] = (uint32_t)(ph - 20); b[6] = (uint32_t)pw; b[7] = 20; b[8] = parent; b[9] = id; b[10] = w->image_base;
    h = cw ? cw->fn(w, b) : 0;
    qw_free(w, cls); qw_free(w, rc);
    return h;
}
static uint32_t c_CreateStatusWindowW(qwin *w, uint32_t *a) { return status_window(w, a[0], a[1], a[2], a[3]); }
static uint32_t c_CreateStatusWindowA(qwin *w, uint32_t *a)
{
    char t[256]; wchar16 u[256]; uint32_t g = qw_alloc(w, 520), h;
    qw_rd_str(w, a[1], t, 256); a2w(u, t, 256); qw_wr_wstr(w, g, u, 256);
    h = status_window(w, a[0], g, a[2], a[3]);
    qw_free(w, g);
    return h;
}
static uint32_t s_ShellAboutW(qwin *w, uint32_t *a);
static uint32_t mm_PlaySound(qwin *w, uint32_t *a) { UNUSED; return 1; }
static uint32_t nt_DbgPrint(qwin *w, uint32_t *a) { UNUSED; return 0; }
uint32_t user_message_box(qwin *w, uint32_t owner, const wchar16 *text, const wchar16 *title, uint32_t type);
static uint32_t s_ShellAboutW(qwin *w, uint32_t *a)
{
    wchar16 app[128], other[256], text[512];
    int n;
    qw_rd_wstr(w, a[1], app, 128);
    qw_rd_wstr(w, a[2], other, 256);
    { int i; for (i = 0; app[i]; i++) if (app[i] == '#') { app[i] = 0; break; } }
    a2w(text, "", 512);
    wcpy16(text, app, 512);
    n = wlen16(text);
    { static const char ver[] = "\nQRX OS One 22.5.12 Win32 subsystem (qrx86core)\n"; int i; for (i = 0; ver[i] && n < 500; i++) text[n++] = (wchar16)(uint8_t)ver[i]; text[n] = 0; }
    { int i; for (i = 0; other[i] && n < 510; i++) text[n++] = other[i]; text[n] = 0; }
    user_message_box(w, a[0], text, app, 0);
    return 1;
}
const qhle qw_misc[] = {
    MS("__wgetmainargs", m___wgetmainargs), MS("__getmainargs", m___getmainargs),
    MS("__set_app_type", m_nop), MS("__setusermatherr", m_nop), MS("__lconv_init", m_nop), MS("_fpreset", m_nop),
    MS("_initterm", m_initterm), MS("_initterm_e", m_initterm_e), MS("_onexit", m_onexit), MS("__dllonexit", m_dllonexit), MS("atexit", m_atexit),
    MS("exit", m_exit), MS("_exit", m__exit), MS("_cexit", m_nop), MS("_c_exit", m_nop), MS("abort", m_abort), MS("_amsg_exit", m_amsg_exit),
    MS("_lock", m_nop), MS("_unlock", m_nop), MS("signal", m_signal), MS("_controlfp", m_controlfp), MS("_control87", m_controlfp),
    MS("malloc", m_malloc), MS("calloc", m_calloc), MS("free", m_free), MS("realloc", m_realloc), MS("_msize", m_msize),
    MS("memcpy", m_memcpy), MS("memmove", m_memmove), MS("memset", m_memset), MS("memcmp", m_memcmp),
    MS("strlen", m_strlen), MS("wcslen", m_wcslen), MS("strcmp", m_strcmp), MS("strncmp", m_strncmp), MS("wcscmp", m_wcscmp),
    MS("_wcsicmp", m_wcsicmp), MS("wcsncmp", m_wcsncmp), MS("strcpy", m_strcpy), MS("wcscpy", m_wcscpy), MS("strncpy", m_strncpy), MS("wcsncpy", m_wcsncpy),
    MS("strcat", m_strcat), MS("wcscat", m_wcscat), MS("strchr", m_strchr), MS("strrchr", m_strrchr), MS("wcschr", m_wcschr), MS("wcsrchr", m_wcsrchr),
    MS("strcspn", m_strcspn), MS("_strdup", m_strdup), MS("_wcsdup", m_wcsdup), MS("_strupr", m_strupr), MS("_strlwr", m_strlwr),
    MS("toupper", m_toupper), MS("tolower", m_tolower), MS("towupper", m_towupper), MS("towlower", m_towlower),
    MS("isdigit", m_isdigit), MS("isspace", m_isspace), MS("isalpha", m_isalpha), MS("iswdigit", m_isdigit), MS("iswspace", m_isspace),
    MS("atoi", m_atoi), MS("_wtoi", m_wtoi), MS("atof", m_atof), MS("wcstod", m_wcstod),
    MS("rand", m_rand), MS("srand", m_srand), MS("time", m_time), MS("clock", m_clock),
    MS("sprintf", m_sprintf), MS("swprintf", m_swprintf), MS("_snprintf", m_snprintf), MS("_snwprintf", m_snwprintf),
    MS("_vsnprintf", m_vsnprintf), MS("_vsnwprintf", m_vsnwprintf), MS("vsprintf", m_vsprintf), MS("vswprintf", m_vswprintf),
    MS("vfprintf", m_vfprintf), MS("fprintf", m_fprintf), MS("printf", m_printf), MS("fwrite", m_fwrite), MS("fputc", m_fputc),
    MS("fflush", m_fflush), MS("puts", m_puts), MS("swscanf", m_swscanf), MS("sscanf", m_sscanf),
    MS("_finite", m_finite), MS("_isnan", m_isnan),
    MS("ldexp", m_ldexp), MS("sin", m_sin), MS("cos", m_cos), MS("tan", m_tan), MS("asin", m_asin), MS("acos", m_acos), MS("atan", m_atan), MS("atan2", m_atan2),
    MS("sinh", m_sinh), MS("cosh", m_cosh), MS("tanh", m_tanh), MS("exp", m_exp), MS("log", m_log), MS("log10", m_log10),
    MS("sqrt", m_sqrt), MS("floor", m_floor), MS("ceil", m_ceil), MS("fabs", m_fabs), MS("pow", m_pow), MS("fmod", m_fmod), MS("modf", m_modf),
    MS("_ftol", m_ftol), MS("_CIpow", m_CIpow),
    MS("bsearch", m_bsearch), MS("qsort", m_qsort), MS("_errno", m_errno),
    MS("__p__fmode", m_p_fmode), MS("__p__acmdln", m_p_acmdln), MS("__p__wcmdln", m_p_wcmdln), MS("_ismbblead", m_ismbblead), MS("__p__commode", m_p_commode), MS("__p__iob", m_iob_func), MS("__p___initenv", m_p_initenv), MS("__p___mb_cur_max", m_p_mb_cur_max), MS("islower", m_islower), MS("isupper", m_isupper), MS("_get_osfhandle", m_get_osfhandle), MS("_setmode", m_setmode), MS("__iob_func", m_iob_func),
    AD("RegOpenKeyExW", a_RegOpenKeyExW, 5), AD("RegCreateKeyExW", a_RegCreateKeyExW, 9), AD("RegCloseKey", a_RegCloseKey, 1),
    AD("RegQueryValueExW", a_RegQueryValueExW, 6), AD("RegSetValueExW", a_RegSetValueExW, 6),
    { "comctl32", "CreateStatusWindowW", 4, c_CreateStatusWindowW }, { "comctl32", "CreateStatusWindowA", 4, c_CreateStatusWindowA },
    { "comctl32", "InitCommonControls", 0, c_InitCommonControls }, { "comctl32", "InitCommonControlsEx", 1, c_InitCommonControlsEx },
    { "shell32", "ShellAboutW", 4, s_ShellAboutW },
    { "winmm", "PlaySoundW", 3, mm_PlaySound }, { "winmm", "PlaySoundA", 3, mm_PlaySound },
    { "ntdll", "vDbgPrintExWithPrefix", -1, nt_DbgPrint }, { "ntdll", "DbgPrint", -1, nt_DbgPrint },
    { NULL, NULL, 0, NULL }
};

/* exported for user32 (wsprintfW / wvsprintfW) */
int qw_format(qwin *w, uint32_t dst, int max, int dwide, uint32_t fmt, int fwide, uint32_t ap)
{
    return fmt_to(w, dst, max, dwide, fmt, fwide, ap, 0);
}
