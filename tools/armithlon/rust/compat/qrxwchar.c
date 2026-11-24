/*
 * libqrxaroscompat, part 3: the wide-character functions AROS's C library declares (aros/stdc/wchar.h,
 * wctype.h) but does not implement, for the "C" locale, which is the only one QRX OS One 64 has.
 * libc++ with localization (std::locale, iostreams, std::wstring) and the code on top of it need them.
 *
 * Characters: wchar_t is 32 bits (UTF-32); the multibyte side is AROS's own mbrtowc/wcrtomb.
 * Classification and case follow ASCII; every other code point is "printable/graphic" when it is
 * not a control character (C0, C1) and nothing else, as in a minimal C locale.
 * Conversions (wcsto*, wcsftime, swprintf) go through the narrow functions: code points beyond
 * ASCII in a numeric string end it, as they would in the C locale.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

/* AROS's headers hide the functions they do not implement (STDC_*_NOTIMPL) and leave wctype_t and
 * wctrans_t undefined (aros/types/wctype_t.h: "#if 0"); these are the types this file defines them with */
typedef unsigned int qrx_wctype_t;
#define wctype_t qrx_wctype_t

wint_t btowc(int c) { return c == EOF ? WEOF : ((unsigned)c < 0x80 ? (wint_t)c : WEOF); }
int wctob(wint_t c) { return c < 0x80 ? (int)c : EOF; }

int iswcntrl(wint_t c) { return c < 0x20 || (c >= 0x7F && c < 0xA0); }
int iswdigit(wint_t c) { return c >= '0' && c <= '9'; }
int iswxdigit(wint_t c) { return iswdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int iswupper(wint_t c) { return c >= 'A' && c <= 'Z'; }
int iswlower(wint_t c) { return c >= 'a' && c <= 'z'; }
int iswalpha(wint_t c) { return iswupper(c) || iswlower(c); }
int iswalnum(wint_t c) { return iswalpha(c) || iswdigit(c); }
int iswblank(wint_t c) { return c == ' ' || c == '\t'; }
int iswspace(wint_t c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int iswpunct(wint_t c) { return c > 0x20 && c < 0x7F && !iswalnum(c); }
int iswgraph(wint_t c) { return c != WEOF && c > 0x20 && !iswcntrl(c) && c <= 0x10FFFF; }
int iswprint(wint_t c) { return c == ' ' || iswgraph(c); }

enum { WT_NONE, WT_ALNUM, WT_ALPHA, WT_BLANK, WT_CNTRL, WT_DIGIT, WT_GRAPH, WT_LOWER, WT_PRINT, WT_PUNCT,
       WT_SPACE, WT_UPPER, WT_XDIGIT };
wctype_t wctype(const char *n)
{
    static const char *names[] = { "", "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print",
                                   "punct", "space", "upper", "xdigit" };
    for (unsigned i = 1; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(n, names[i])) return (wctype_t)i;
    return 0;
}
int iswctype(wint_t c, wctype_t t)
{
    switch ((int)t) {
    case WT_ALNUM: return iswalnum(c);   case WT_ALPHA: return iswalpha(c);   case WT_BLANK: return iswblank(c);
    case WT_CNTRL: return iswcntrl(c);   case WT_DIGIT: return iswdigit(c);   case WT_GRAPH: return iswgraph(c);
    case WT_LOWER: return iswlower(c);   case WT_PRINT: return iswprint(c);   case WT_PUNCT: return iswpunct(c);
    case WT_SPACE: return iswspace(c);   case WT_UPPER: return iswupper(c);   case WT_XDIGIT: return iswxdigit(c);
    default: return 0;
    }
}

/* the ASCII prefix of a wide string as a narrow one (numbers, formats) */
static char *narrow(const wchar_t *s, size_t *n)
{
    size_t k = 0;
    while (s[k] && s[k] < 0x80) k++;
    char *b = malloc(k + 1);
    if (!b) return NULL;
    for (size_t i = 0; i < k; i++) b[i] = (char)s[i];
    b[k] = 0;
    if (n) *n = k;
    return b;
}
#define WCSTO(name, type, call)                                         \
    type name(const wchar_t *restrict s, wchar_t **restrict end)        \
    {                                                                   \
        char *b = narrow(s, NULL), *e;                                  \
        if (!b) { if (end) *end = (wchar_t *)s; return 0; }             \
        type r = call;                                                  \
        if (end) *end = (wchar_t *)s + (e - b);                         \
        free(b);                                                        \
        return r;                                                       \
    }
WCSTO(wcstod, double, strtod(b, &e))
WCSTO(wcstof, float, strtof(b, &e))
WCSTO(wcstold, long double, strtold(b, &e))
#define WCSTOI(name, type, call)                                                  \
    type name(const wchar_t *restrict s, wchar_t **restrict end, int base)        \
    {                                                                             \
        char *b = narrow(s, NULL), *e;                                            \
        if (!b) { if (end) *end = (wchar_t *)s; return 0; }                       \
        type r = call;                                                            \
        if (end) *end = (wchar_t *)s + (e - b);                                   \
        free(b);                                                                  \
        return r;                                                                 \
    }
WCSTOI(wcstol, long, strtol(b, &e, base))
WCSTOI(wcstoll, long long, strtoll(b, &e, base))
WCSTOI(wcstoul, unsigned long, strtoul(b, &e, base))
WCSTOI(wcstoull, unsigned long long, strtoull(b, &e, base))

int wcscoll(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }
size_t wcsxfrm(wchar_t *restrict d, const wchar_t *restrict s, size_t n)
{
    size_t l = wcslen(s);
    if (l < n) wmemcpy(d, s, l + 1);
    return l;
}

size_t wcsftime(wchar_t *restrict d, size_t n, const wchar_t *restrict fmt, const struct tm *restrict tm)
{
    char *f = narrow(fmt, NULL);
    if (!f || !n) { free(f); return 0; }
    char *buf = malloc(n);
    if (!buf) { free(f); return 0; }
    size_t r = strftime(buf, n, f, tm);
    for (size_t i = 0; i < r; i++) d[i] = (unsigned char)buf[i];
    if (r < n) d[r] = 0;
    free(buf); free(f);
    return r;
}

/* swprintf: the format is taken apart conversion by conversion. %ls and %lc are converted here
 * (AROS's printf has no wide strings), every other conversion goes to snprintf on its own with its
 * one argument, and the narrow result is widened through mbrtowc. */
static int wput(wchar_t *d, size_t n, size_t *o, wchar_t c)
{
    if (*o + 1 >= n) return -1;
    d[(*o)++] = c;
    return 0;
}
static int wput_mb(wchar_t *d, size_t n, size_t *o, const char *p)
{
    mbstate_t st;
    memset(&st, 0, sizeof st);
    while (*p) {
        wchar_t wc;
        size_t k = mbrtowc(&wc, p, strlen(p), &st);
        if (k == (size_t)-1 || k == (size_t)-2 || k == 0) { wc = (unsigned char)*p; k = 1; memset(&st, 0, sizeof st); }
        if (wput(d, n, o, wc)) return -1;
        p += k;
    }
    return 0;
}
int vswprintf(wchar_t *restrict d, size_t n, const wchar_t *restrict fmt, va_list ap)
{
    size_t o = 0;
    char spec[64], buf[512];
    if (!n) { errno = EOVERFLOW; return -1; }
    for (const wchar_t *f = fmt; *f; f++) {
        if (*f != L'%') { if (wput(d, n, &o, *f)) goto full; continue; }
        if (f[1] == L'%') { if (wput(d, n, &o, L'%')) goto full; f++; continue; }
        /* %[flags][width][.prec][length]conv */
        size_t k = 0;
        spec[k++] = '%';
        const wchar_t *q = f + 1;
        int width = -1, prec = -1, lng = 0, hh = 0;
        while (*q && wcschr(L"-+ #0", *q) && k < 40) spec[k++] = (char)*q++;
        if (*q == L'*') { width = va_arg(ap, int); q++; k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", width); }
        else while (*q >= L'0' && *q <= L'9' && k < 40) spec[k++] = (char)*q++;
        if (*q == L'.') {
            spec[k++] = '.'; q++;
            if (*q == L'*') { prec = va_arg(ap, int); q++; k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", prec); }
            else { prec = 0; while (*q >= L'0' && *q <= L'9' && k < 48) { prec = prec * 10 + (int)(*q - L'0'); spec[k++] = (char)*q++; } }
        }
        while (*q && wcschr(L"hlLqjzt", *q) && k < 56) {
            if (*q == L'l') lng++;
            if (*q == L'h') hh++;
            spec[k++] = (char)*q++;
        }
        wchar_t conv = *q;
        if (!conv) break;
        spec[k++] = (char)conv; spec[k] = 0;
        f = q;
        if ((conv == L's' || conv == L'c') && lng) {
            if (conv == L'c') { if (wput(d, n, &o, (wchar_t)va_arg(ap, wint_t))) goto full; continue; }
            const wchar_t *ws = va_arg(ap, const wchar_t *);
            if (!ws) ws = L"(null)";
            size_t len = wcslen(ws);
            if (prec >= 0 && (size_t)prec < len) len = (size_t)prec;
            for (size_t i = 0; i < len; i++) if (wput(d, n, &o, ws[i])) goto full;
            continue;
        }
        int r;
        switch (conv) {
        case L'd': case L'i': case L'o': case L'u': case L'x': case L'X':
            if (lng >= 2 || wcschr(L"jqz", *(q - 1))) r = snprintf(buf, sizeof buf, spec, va_arg(ap, long long));
            else if (lng == 1 || *(q - 1) == L't') r = snprintf(buf, sizeof buf, spec, va_arg(ap, long));
            else r = snprintf(buf, sizeof buf, spec, va_arg(ap, int));
            break;
        case L'e': case L'E': case L'f': case L'F': case L'g': case L'G': case L'a': case L'A':
            if (*(q - 1) == L'L') r = snprintf(buf, sizeof buf, spec, va_arg(ap, long double));
            else r = snprintf(buf, sizeof buf, spec, va_arg(ap, double));
            break;
        case L'c': r = snprintf(buf, sizeof buf, spec, va_arg(ap, int)); break;
        case L's': r = snprintf(buf, sizeof buf, spec, va_arg(ap, const char *)); break;
        case L'p': r = snprintf(buf, sizeof buf, spec, va_arg(ap, void *)); break;
        case L'n': *va_arg(ap, int *) = (int)o; continue;
        default: errno = EINVAL; return -1;
        }
        (void)hh;
        if (r < 0) return -1;
        if (wput_mb(d, n, &o, buf)) goto full;
    }
    d[o] = 0;
    return (int)o;
full:
    d[n - 1] = 0;
    errno = EOVERFLOW;
    return -1;
}
int swprintf(wchar_t *restrict d, size_t n, const wchar_t *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vswprintf(d, n, fmt, ap);
    va_end(ap);
    return r;
}

/* byte-oriented streams: a wide character is read as its multibyte sequence */
wint_t getwc(FILE *f)
{
    char b[8];
    mbstate_t st;
    memset(&st, 0, sizeof st);
    for (size_t k = 0; k < sizeof b; k++) {
        int c = getc(f);
        if (c == EOF) return WEOF;
        b[k] = (char)c;
        wchar_t wc;
        size_t r = mbrtowc(&wc, b, k + 1, &st);
        if (r == (size_t)-2) continue;
        if (r == (size_t)-1) { errno = EILSEQ; return WEOF; }
        return (wint_t)wc;
    }
    errno = EILSEQ;
    return WEOF;
}
wint_t ungetwc(wint_t c, FILE *f)
{
    char b[8];
    mbstate_t st;
    if (c == WEOF) return WEOF;
    memset(&st, 0, sizeof st);
    size_t k = wcrtomb(b, (wchar_t)c, &st);
    if (k == (size_t)-1) return WEOF;
    /* the C library takes back one byte only: enough for the ASCII the C locale reads */
    if (k != 1 || ungetc((unsigned char)b[0], f) == EOF) return WEOF;
    return c;
}
int fwide(FILE *f, int mode) { (void)f; return mode; }
