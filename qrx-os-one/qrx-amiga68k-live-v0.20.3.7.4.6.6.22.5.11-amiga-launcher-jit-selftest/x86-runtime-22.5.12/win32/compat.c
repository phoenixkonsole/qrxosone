/* QRX OS One - qrx-win32: the rest of what Win9x games import.
 * Synchronisation with real waiting, a registry kept in a file next to the game, INI files,
 * file mappings, old-style file calls, multimedia files, waveOut and many small GDI and USER
 * functions. This table is searched first, so it replaces older single-thread versions. */
#include "qrxwin.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FN(name) static uint32_t name(qwin *w, uint32_t *a)
#define C (&w->cpu)
#define RD32(x) x86_rd32(C, (x))
#define WR32(x, v) x86_wr32(C, (x), (v))
#define RD16(x) x86_rd16(C, (x))
#define WR16(x, v) x86_wr16(C, (x), (v))
#define RD8(x) x86_rd8(C, (x))
#define WR8(x, v) x86_wr8(C, (x), (v))
#define UNUSED (void)w; (void)a

static void rd_str(qwin *w, uint32_t p, char *out, int max)
{
    int i = 0;
    if (p) for (; i < max - 1; i++) { out[i] = (char)RD8(p + (uint32_t)i); if (!out[i]) break; }
    out[i] = 0;
}
static uint32_t wr_str(qwin *w, uint32_t p, uint32_t max, const char *s)
{
    uint32_t i = 0;
    if (!max) return 0;
    for (; s[i] && i + 1 < max; i++) WR8(p + i, (uint8_t)s[i]);
    WR8(p + i, 0);
    return i;
}
extern const qhle qw_files[];
static uint32_t call_api(qwin *w, const char *dll, const char *name, uint32_t *a)
{
    const qhle *e = NULL;
    if (!strcmp(dll, "files-next")) {            /* the version this table wraps */
        for (e = qw_files; e->name; e++) if (!strcmp(e->name, name)) break;
        if (!e->name) e = NULL;
    } else e = qw_find_hle(dll, name);
    return e ? e->fn(w, a) : 0;
}
FN(c_ok0) { UNUSED; return 0; }
FN(c_ok1) { UNUSED; return 1; }
FN(c_arg0) { (void)w; return a[0]; }
FN(c_fail_mm) { UNUSED; return 2u; /* MMSYSERR_BADDEVICEID */ }

/* ================================================================ synchronisation */
#define MAXSYNC 256
#define SYNC_BASE 0x7E300000u
enum { S_FREE, S_EVENT, S_MUTEX, S_SEM };
typedef struct { int type, manual; int32_t count, max; uint32_t owner; int rec; char name[64]; } qsync;
static qsync g_s[MAXSYNC];

static uint32_t sync_new(qwin *w, int type, int32_t count, int32_t max, int manual, uint32_t nameptr)
{
    int i;
    char name[64];
    rd_str(w, nameptr, name, sizeof(name));
    if (name[0]) {
        for (i = 0; i < MAXSYNC; i++) if (g_s[i].type == type && !strcmp(g_s[i].name, name)) {
            w->last_error = 183;                 /* ERROR_ALREADY_EXISTS */
            return SYNC_BASE + (uint32_t)i * 4u;
        }
    }
    w->last_error = 0;
    for (i = 0; i < MAXSYNC; i++) if (g_s[i].type == S_FREE) {
        memset(&g_s[i], 0, sizeof(g_s[i]));
        g_s[i].type = type; g_s[i].count = count; g_s[i].max = max; g_s[i].manual = manual;
        snprintf(g_s[i].name, sizeof(g_s[i].name), "%s", name);
        return SYNC_BASE + (uint32_t)i * 4u;
    }
    return 0;
}
static qsync *sync_of(uint32_t h)
{
    uint32_t i = (h - SYNC_BASE) / 4u;
    return h >= SYNC_BASE && i < MAXSYNC && g_s[i].type != S_FREE ? &g_s[i] : NULL;
}
FN(c_CreateEventA) { return sync_new(w, S_EVENT, a[2] ? 1 : 0, 1, a[1] != 0, a[3]); }
FN(c_CreateMutexA) {
    uint32_t h = sync_new(w, S_MUTEX, 1, 1, 0, a[2]);
    qsync *s = sync_of(h);
    if (s && a[1] && !s->owner) { s->owner = qw_thread_current_id(); s->rec = 1; s->count = 0; }
    return h;
}
FN(c_CreateSemaphoreA) { return sync_new(w, S_SEM, (int32_t)a[1], (int32_t)a[2], 0, a[3]); }
FN(c_SetEvent) { qsync *s = sync_of(a[0]); UNUSED; if (!s) return 0; s->count = 1; return 1; }
FN(c_ResetEvent) { qsync *s = sync_of(a[0]); UNUSED; if (!s) return 0; s->count = 0; return 1; }
FN(c_PulseEvent) {
    /* Lets every thread already waiting see it once, then clears it. */
    qsync *s = sync_of(a[0]);
    if (!s) return 0;
    s->count = 1;
    qw_thread_yield(w);
    s->count = 0;
    return 1;
}
FN(c_ReleaseMutex) {
    qsync *s = sync_of(a[0]);
    UNUSED;
    if (!s || s->type != S_MUTEX || s->owner != qw_thread_current_id()) return 0;
    if (--s->rec <= 0) { s->rec = 0; s->owner = 0; s->count = 1; }
    return 1;
}
FN(c_ReleaseSemaphore) {
    qsync *s = sync_of(a[0]);
    if (!s || s->type != S_SEM) return 0;
    if (a[2]) WR32(a[2], (uint32_t)s->count);
    if (s->count + (int32_t)a[1] > s->max) return 0;
    s->count += (int32_t)a[1];
    return 1;
}
/* 1 if signalled, and takes it when take is set. Unknown handles count as signalled. */
static int sig_try(qwin *w, uint32_t h, int take)
{
    int t = qw_thread_handle_index(h);
    qsync *s;
    (void)w;
    if (t >= 0) return qw_thread_finished(t);
    s = sync_of(h);
    if (!s) return 1;
    if (s->type == S_MUTEX) {
        uint32_t me = qw_thread_current_id();
        if (s->owner && s->owner != me) return 0;
        if (take) { s->owner = me; s->rec++; s->count = 0; }
        return 1;
    }
    if (s->count <= 0) return 0;
    if (take && !(s->type == S_EVENT && s->manual)) s->count--;
    return 1;
}
static uint32_t wait_many(qwin *w, uint32_t n, uint32_t *hs, int all, uint32_t ms)
{
    uint64_t end = ms == 0xFFFFFFFFu ? 0 : w->be->now(w) + ms;
    uint32_t i;
    for (;;) {
        if (all) {
            for (i = 0; i < n; i++) if (!sig_try(w, hs[i], 0)) break;
            if (i == n) { for (i = 0; i < n; i++) sig_try(w, hs[i], 1); return 0; }
        } else {
            for (i = 0; i < n; i++) if (sig_try(w, hs[i], 1)) return i;
        }
        if (w->exiting) return 0;
        if (ms == 0xFFFFFFFFu) qw_thread_block(w, 2);
        else {
            uint64_t now = w->be->now(w);
            if (now >= end) return 0x102;      /* WAIT_TIMEOUT */
            qw_thread_block(w, (uint32_t)(end - now));
        }
    }
}
FN(c_WaitForSingleObject) { uint32_t h = a[0]; return wait_many(w, 1, &h, 0, a[1]); }
FN(c_WaitForMultipleObjects) {
    uint32_t h[64], n = a[0] < 64 ? a[0] : 64, i;
    for (i = 0; i < n; i++) h[i] = RD32(a[1] + i * 4);
    return wait_many(w, n, h, a[2] != 0, a[3]);
}
FN(c_CloseHandle) {
    qsync *s = sync_of(a[0]);
    if (s) { s->type = S_FREE; return 1; }
    if (qw_thread_handle_index(a[0]) >= 0) return 1;
    return qw_file_close(w, a[0]);
}
FN(c_Sleep) { qw_thread_sleep(w, a[0]); return 0; }
FN(c_SleepEx) { qw_thread_sleep(w, a[0]); return 0; }
FN(c_GetCurrentThreadId) { UNUSED; return qw_thread_current_id(); }
/* CRITICAL_SECTION: +8 RecursionCount, +0xC OwningThread */
FN(c_InitializeCriticalSection) { uint32_t i; for (i = 0; i < 24; i += 4) WR32(a[0] + i, 0); return 1; }
FN(c_EnterCriticalSection) {
    uint32_t me = qw_thread_current_id();
    for (;;) {
        uint32_t o = RD32(a[0] + 0xC);
        if (!o || o == me) break;
        qw_thread_block(w, 2);
        if (w->exiting) return 0;
    }
    WR32(a[0] + 0xC, me);
    WR32(a[0] + 8, RD32(a[0] + 8) + 1);
    return 0;
}
FN(c_TryEnterCriticalSection) {
    uint32_t me = qw_thread_current_id(), o = RD32(a[0] + 0xC);
    if (o && o != me) return 0;
    WR32(a[0] + 0xC, me);
    WR32(a[0] + 8, RD32(a[0] + 8) + 1);
    return 1;
}
FN(c_LeaveCriticalSection) {
    uint32_t n = RD32(a[0] + 8);
    if (n) n--;
    WR32(a[0] + 8, n);
    if (!n) WR32(a[0] + 0xC, 0);
    return 0;
}
FN(c_TlsGetValue) { return a[0] < 64 ? RD32(qw_thread_current_teb() + 0x1000 + a[0] * 4) : 0; }
FN(c_TlsSetValue) { if (a[0] < 64) WR32(qw_thread_current_teb() + 0x1000 + a[0] * 4, a[1]); return 1; }
FN(c_DuplicateHandle) { if (a[3]) WR32(a[3], a[1] == 0xFFFFFFFEu ? qw_thread_current_handle(w) : a[1]); return 1; }

/* ================================================================ registry */
#define REG_FILE "QRXREG.TXT"
typedef struct qval { char key[160], name[64]; uint32_t type, size; uint8_t *data; struct qval *next; } qval;
static qval *g_vals;
static struct { int used; char path[160]; } g_hk[64];
static char g_known[128][160];
static int g_loaded;

static void key_lower(char *s) { for (; *s; s++) *s = *s == '/' ? '\\' : (char)tolower((unsigned char)*s); }
static void key_known(const char *p)
{
    int i;
    for (i = 0; i < 128; i++) {
        if (!strcmp(g_known[i], p)) return;
        if (!g_known[i][0]) { snprintf(g_known[i], sizeof(g_known[i]), "%s", p); return; }
    }
}
static qval *val_find(const char *k, const char *n)
{
    qval *v;
    for (v = g_vals; v; v = v->next) if (!strcmp(v->key, k) && !strcmp(v->name, n)) return v;
    return NULL;
}
static void val_set(const char *k, const char *n, uint32_t type, const uint8_t *d, uint32_t size)
{
    qval *v = val_find(k, n);
    if (!v) {
        v = (qval *)calloc(1, sizeof(*v));
        snprintf(v->key, sizeof(v->key), "%s", k);
        snprintf(v->name, sizeof(v->name), "%s", n);
        v->next = g_vals; g_vals = v;
    }
    free(v->data);
    v->data = (uint8_t *)malloc(size ? size : 1);
    if (size) memcpy(v->data, d, size);
    v->type = type; v->size = size;
    key_known(k);
}
static void reg_load(void)
{
    FILE *f;
    char line[1024];
    if (g_loaded) return;
    g_loaded = 1;
    f = fopen(REG_FILE, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *k = line, *n = strchr(line, '|'), *t, *h;
        uint8_t buf[256];
        uint32_t size = 0;
        if (!n) continue;
        *n++ = 0;
        t = strchr(n, '|');
        if (!t) continue;
        *t++ = 0;
        h = strchr(t, '|');
        if (!h) continue;
        *h++ = 0;
        while (isxdigit((unsigned char)h[0]) && isxdigit((unsigned char)h[1]) && size < sizeof(buf)) {
            unsigned b = 0;
            sscanf(h, "%2x", &b);
            buf[size++] = (uint8_t)b;
            h += 2;
        }
        key_lower(k);
        val_set(k, n, (uint32_t)strtoul(t, NULL, 10), buf, size);
    }
    fclose(f);
}
static void reg_save(void)
{
    FILE *f = fopen(REG_FILE, "w");
    qval *v;
    if (!f) return;
    for (v = g_vals; v; v = v->next) {
        uint32_t i;
        if (v->size > 256) continue;
        fprintf(f, "%s|%s|%u|", v->key, v->name, (unsigned)v->type);
        for (i = 0; i < v->size; i++) fprintf(f, "%02x", v->data[i]);
        fputc('\n', f);
    }
    fclose(f);
}
static void reg_path(qwin *w, uint32_t hk, uint32_t sub, char *out, size_t n)
{
    const char *root = hk == 0x80000000u ? "hkcr" : hk == 0x80000001u ? "hkcu" : hk == 0x80000002u ? "hklm" : hk == 0x80000003u ? "hku" : NULL;
    char s[160];
    uint32_t i = hk - 0x7E400000u;
    if (!root && i < 64 && g_hk[i].used) root = g_hk[i].path;
    if (!root) root = "?";
    rd_str(w, sub, s, sizeof(s));
    if (s[0]) snprintf(out, n, "%s\\%s", root, s); else snprintf(out, n, "%s", root);
    key_lower(out);
}
static uint32_t hk_new(const char *p)
{
    int i;
    for (i = 0; i < 64; i++) if (!g_hk[i].used) {
        g_hk[i].used = 1;
        snprintf(g_hk[i].path, sizeof(g_hk[i].path), "%s", p);
        return 0x7E400000u + (uint32_t)i;
    }
    return 0;
}
static int key_exists(const char *p)
{
    size_t L = strlen(p);
    int i;
    for (i = 0; i < 128 && g_known[i][0]; i++) {
        if (!strncmp(g_known[i], p, L) && (g_known[i][L] == 0 || g_known[i][L] == '\\')) return 1;
    }
    return 0;
}
static uint32_t reg_open(qwin *w, uint32_t hk, uint32_t sub, uint32_t out, int create, uint32_t disp)
{
    char p[160];
    reg_load();
    reg_path(w, hk, sub, p, sizeof(p));
    if (!key_exists(p)) {
        if (!create) { if (out) WR32(out, 0); return 2; }
        key_known(p);
        if (disp) WR32(disp, 1);
        reg_save();
    } else if (disp) WR32(disp, 2);
    if (out) WR32(out, hk_new(p));
    return 0;
}
FN(c_RegOpenKeyA) { return reg_open(w, a[0], a[1], a[2], 0, 0); }
FN(c_RegOpenKeyExA) { return reg_open(w, a[0], a[1], a[4], 0, 0); }
FN(c_RegCreateKeyA) { return reg_open(w, a[0], a[1], a[2], 1, 0); }
FN(c_RegCreateKeyExA) { return reg_open(w, a[0], a[1], a[7], 1, a[8]); }
FN(c_RegCloseKey) { uint32_t i = a[0] - 0x7E400000u; UNUSED; if (i < 64) g_hk[i].used = 0; return 0; }
FN(c_RegQueryValueExA) {
    char p[160], n[64];
    qval *v;
    reg_load();
    reg_path(w, a[0], 0, p, sizeof(p));
    rd_str(w, a[1], n, sizeof(n));
    v = val_find(p, n);
    if (!v) return 2;
    if (a[3]) WR32(a[3], v->type);
    if (a[5]) {
        uint32_t cap = RD32(a[5]);
        WR32(a[5], v->size);
        if (a[4]) {
            if (cap < v->size) return 234;     /* ERROR_MORE_DATA */
            x86_write_mem(C, a[4], v->data, v->size);
        }
    }
    return 0;
}
FN(c_RegSetValueExA) {
    char p[160], n[64];
    uint8_t *d;
    uint32_t size = a[5];
    reg_load();
    reg_path(w, a[0], 0, p, sizeof(p));
    rd_str(w, a[1], n, sizeof(n));
    d = (uint8_t *)malloc(size ? size : 1);
    if (size) x86_read_mem(C, a[4], d, size);
    val_set(p, n, a[3], d, size);
    free(d);
    reg_save();
    return 0;
}
FN(c_RegDeleteValueA) {
    char p[160], n[64];
    qval **pv, *v;
    reg_load();
    reg_path(w, a[0], 0, p, sizeof(p));
    rd_str(w, a[1], n, sizeof(n));
    for (pv = &g_vals; (v = *pv) != NULL; pv = &v->next) {
        if (!strcmp(v->key, p) && !strcmp(v->name, n)) { *pv = v->next; free(v->data); free(v); reg_save(); return 0; }
    }
    return 2;
}
FN(c_RegEnumValueA) {
    char p[160];
    qval *v;
    uint32_t i = 0;
    reg_load();
    reg_path(w, a[0], 0, p, sizeof(p));
    for (v = g_vals; v; v = v->next) {
        if (strcmp(v->key, p)) continue;
        if (i++ != a[1]) continue;
        wr_str(w, a[2], RD32(a[3]), v->name);
        WR32(a[3], (uint32_t)strlen(v->name));
        if (a[5]) WR32(a[5], v->type);
        if (a[7]) {
            if (a[6]) x86_write_mem(C, a[6], v->data, v->size < RD32(a[7]) ? v->size : RD32(a[7]));
            WR32(a[7], v->size);
        }
        return 0;
    }
    return 259;   /* ERROR_NO_MORE_ITEMS */
}
FN(c_RegNoMore) { UNUSED; return 259; }
FN(c_RegQueryInfoKeyA) { uint32_t i; for (i = 4; i < 11; i++) if (a[i]) WR32(a[i], 0); if (a[2]) WR32(a[2], 0); (void)w; return 0; }

/* ================================================================ INI files */
static FILE *ini_open(qwin *w, uint32_t path)
{
    char p[300], *s;
    FILE *f;
    (void)w;
    rd_str(w, path, p, sizeof(p));
    /* Windows paths from a game: a plain name lives in \WINDOWS, here beside the game. */
    for (s = p; *s; s++) if (*s == '\\') *s = '/';
    s = p;
    if (isalpha((unsigned char)s[0]) && s[1] == ':') s += 2;
    while (*s == '/') s++;
    f = fopen(s, "r");
    if (!f) { char *b = strrchr(s, '/'); if (b) f = fopen(b + 1, "r"); }
    return f;
}
static int ini_find(FILE *f, const char *sec, const char *key, char *out, size_t max)
{
    char line[512];
    int in = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = line, *e;
        while (*s == ' ' || *s == '\t') s++;
        for (e = s + strlen(s); e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
        if (*s == '[') {
            e = strchr(s, ']');
            if (e) *e = 0;
            in = !strcasecmp(s + 1, sec);
            continue;
        }
        if (!in || *s == ';') continue;
        e = strchr(s, '=');
        if (!e) continue;
        *e = 0;
        { char *t = e; while (t > s && (t[-1] == ' ' || t[-1] == '\t')) *--t = 0; }
        if (!strcasecmp(s, key)) {
            e++;
            while (*e == ' ' || *e == '\t') e++;
            snprintf(out, max, "%s", e);
            return 1;
        }
    }
    return 0;
}
FN(c_GetPrivateProfileStringA) {
    char sec[128], key[128], val[512];
    FILE *f = ini_open(w, a[5]);
    int found = 0;
    rd_str(w, a[0], sec, sizeof(sec));
    rd_str(w, a[1], key, sizeof(key));
    if (f) { found = a[1] && ini_find(f, sec, key, val, sizeof(val)); fclose(f); }
    if (!found) rd_str(w, a[2], val, sizeof(val));
    return wr_str(w, a[3], a[4], val);
}
FN(c_GetPrivateProfileIntA) {
    char sec[128], key[128], val[64];
    FILE *f = ini_open(w, a[3]);
    int found = 0;
    rd_str(w, a[0], sec, sizeof(sec));
    rd_str(w, a[1], key, sizeof(key));
    if (f) { found = ini_find(f, sec, key, val, sizeof(val)); fclose(f); }
    return found ? (uint32_t)strtol(val, NULL, 10) : a[2];
}
FN(c_GetProfileStringA) { char v[256]; rd_str(w, a[2], v, sizeof(v)); return wr_str(w, a[3], a[4], v); }
FN(c_GetProfileIntA) { (void)w; return a[2]; }

/* ================================================================ old-style file calls */
static uint32_t lopen_mode(qwin *w, uint32_t path, uint32_t access, uint32_t disp)
{
    uint32_t b[7];
    b[0] = path; b[1] = access; b[2] = 3; b[3] = 0; b[4] = disp; b[5] = 0x80; b[6] = 0;
    return call_api(w, "kernel32", "CreateFileA", b);
}
FN(c_lopen) { return lopen_mode(w, a[0], (a[1] & 3u) == 0 ? 0x80000000u : (a[1] & 3u) == 1 ? 0x40000000u : 0xC0000000u, 3); }
FN(c_lcreat) { return lopen_mode(w, a[0], 0xC0000000u, 2); }
FN(c_lclose) { return qw_file_close(w, a[0]) ? 0 : 0xFFFFFFFFu; }
extern int qw_is_hle_dll(const char *dll);
static uint32_t scratch_u32(qwin *w)
{
    static uint32_t p;
    if (!p) p = qw_alloc(w, 16);
    return p;
}
FN(c_lread) {
    uint32_t got = scratch_u32(w), b[5];
    b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; b[3] = got; b[4] = 0;
    return call_api(w, "kernel32", "ReadFile", b) ? RD32(got) : 0xFFFFFFFFu;
}
FN(c_lwrite) {
    uint32_t got = scratch_u32(w), b[5];
    b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; b[3] = got; b[4] = 0;
    return call_api(w, "kernel32", "WriteFile", b) ? RD32(got) : 0xFFFFFFFFu;
}
FN(c_llseek) {
    uint32_t b[4];
    b[0] = a[0]; b[1] = a[1]; b[2] = 0; b[3] = a[2];
    return call_api(w, "kernel32", "SetFilePointer", b);
}
/* OFSTRUCT: +0 cBytes, +8 szPathName */
FN(c_OpenFile) {
    uint32_t mode = a[2], acc = (mode & 3u) == 0 ? 0x80000000u : (mode & 3u) == 1 ? 0x40000000u : 0xC0000000u;
    uint32_t h;
    char p[128];
    rd_str(w, a[0], p, sizeof(p));
    if (a[1]) { WR8(a[1], 136); wr_str(w, a[1] + 8, 128, p); }
    if (mode & 0x1000u) {                 /* OF_CREATE */
        h = lopen_mode(w, a[0], 0xC0000000u, 2);
    } else {
        h = lopen_mode(w, a[0], acc, 3);
    }
    if (h == 0xFFFFFFFFu) return 0xFFFFFFFFu;
    if (mode & 0x4000u) { qw_file_close(w, h); return 1; }  /* OF_EXIST */
    return h;
}
FN(c_GetSystemDirectoryA) { return wr_str(w, a[0], a[1], "C:\\WINDOWS\\SYSTEM"); }
FN(c_GetWindowsDirectoryA) { return wr_str(w, a[0], a[1], "C:\\WINDOWS"); }
FN(c_GetTempFileNameA) {
    static unsigned n;
    char p[200], pre[8], out[260];
    rd_str(w, a[0], p, sizeof(p));
    rd_str(w, a[1], pre, sizeof(pre));
    n++;
    snprintf(out, sizeof(out), "%s\\%.3s%04x.TMP", p[0] ? p : ".", pre, a[2] ? a[2] : n);
    wr_str(w, a[3], 260, out);
    return a[2] ? a[2] : n;
}
FN(c_CopyFileA) {
    char s[300], d[300], buf[4096];
    uint32_t b[7], got = scratch_u32(w), hs, hd;
    (void)s; (void)d;
    hs = lopen_mode(w, a[0], 0x80000000u, 3);
    if (hs == 0xFFFFFFFFu) return 0;
    hd = lopen_mode(w, a[1], 0xC0000000u, a[2] ? 1 : 2);
    if (hd == 0xFFFFFFFFu) { qw_file_close(w, hs); return 0; }
    {
        uint32_t tmp = qw_alloc(w, sizeof(buf));
        for (;;) {
            b[0] = hs; b[1] = tmp; b[2] = sizeof(buf); b[3] = got; b[4] = 0;
            if (!call_api(w, "kernel32", "ReadFile", b) || !RD32(got)) break;
            b[0] = hd; b[2] = RD32(got);
            call_api(w, "kernel32", "WriteFile", b);
        }
    }
    qw_file_close(w, hs);
    qw_file_close(w, hd);
    return 1;
}

/* ================================================================ memory */
FN(c_GlobalMemoryStatus) {
    uint32_t p = a[0];
    WR32(p, 32); WR32(p + 4, 20);
    WR32(p + 8, 128u << 20); WR32(p + 12, 96u << 20);
    WR32(p + 16, 256u << 20); WR32(p + 20, 200u << 20);
    WR32(p + 24, 0x7FFE0000u); WR32(p + 28, 0x7F000000u);
    return 0;
}
/* Global and Local memory is a plain pointer here, so a handle is its own address. */
FN(c_GlobalReAlloc) {
    uint32_t b[4];
    if (!a[0]) { b[0] = a[2]; b[1] = a[1]; return call_api(w, "kernel32", "GlobalAlloc", b); }
    b[0] = 0x00150000u; b[1] = (a[2] & 0x40u) ? 8u : 0u; b[2] = a[0]; b[3] = a[1];
    return call_api(w, "kernel32", "HeapReAlloc", b);
}
FN(c_LocalSize) { uint32_t b[1]; b[0] = a[0]; return call_api(w, "kernel32", "GlobalSize", b); }

/* File mappings: read into memory. Writes do not reach the file. */
#define MAXMAP 32
#define MAP_BASE 0x7E500000u
static struct { int used; uint32_t file, size; char name[64]; uint32_t view; } g_map[MAXMAP];
FN(c_CreateFileMappingA) {
    int i;
    char name[64];
    rd_str(w, a[5], name, sizeof(name));
    if (name[0]) for (i = 0; i < MAXMAP; i++) if (g_map[i].used && !strcmp(g_map[i].name, name)) {
        w->last_error = 183;
        return MAP_BASE + (uint32_t)i;
    }
    w->last_error = 0;
    for (i = 0; i < MAXMAP; i++) if (!g_map[i].used) {
        g_map[i].used = 1;
        g_map[i].file = a[0];
        g_map[i].size = a[4];
        g_map[i].view = 0;
        snprintf(g_map[i].name, sizeof(g_map[i].name), "%s", name);
        if (a[0] != 0xFFFFFFFFu && !a[4]) {
            uint32_t b[2];
            b[0] = a[0]; b[1] = 0;
            g_map[i].size = call_api(w, "kernel32", "GetFileSize", b);
        }
        return MAP_BASE + (uint32_t)i;
    }
    return 0;
}
FN(c_OpenFileMappingA) {
    int i;
    char name[64];
    rd_str(w, a[2], name, sizeof(name));
    for (i = 0; i < MAXMAP; i++) if (name[0] && g_map[i].used && !strcmp(g_map[i].name, name)) return MAP_BASE + (uint32_t)i;
    return 0;
}
FN(c_MapViewOfFile) {
    uint32_t i = a[0] - MAP_BASE, off = a[3], size = a[4], p, b[5];
    if (i >= MAXMAP || !g_map[i].used) return 0;
    if (g_map[i].view && g_map[i].file == 0xFFFFFFFFu) return g_map[i].view + off;
    if (!size) size = g_map[i].size - off;
    p = qw_alloc(w, size + 16);
    if (!p) return 0;
    if (g_map[i].file == 0xFFFFFFFFu) {
        uint8_t *h = x86_host_ptr(C, p);
        if (h) memset(h, 0, size);
        g_map[i].view = p;
        return p + off;
    }
    b[0] = g_map[i].file; b[1] = off; b[2] = 0; b[3] = 0;
    call_api(w, "kernel32", "SetFilePointer", b);
    b[0] = g_map[i].file; b[1] = p; b[2] = size; b[3] = scratch_u32(w); b[4] = 0;
    call_api(w, "kernel32", "ReadFile", b);
    return p;
}

/* ================================================================ strings, numbers */
FN(c_lstrcmpA) {
    uint32_t i;
    for (i = 0;; i++) {
        int x = RD8(a[0] + i), y = RD8(a[1] + i);
        if (x != y) return x < y ? 0xFFFFFFFFu : 1u;
        if (!x) return 0;
    }
}
FN(c_lstrcmpiA) {
    uint32_t i;
    for (i = 0;; i++) {
        int x = tolower(RD8(a[0] + i)), y = tolower(RD8(a[1] + i));
        if (x != y) return x < y ? 0xFFFFFFFFu : 1u;
        if (!x) return 0;
    }
}
FN(c_lstrcpynA) {
    uint32_t i, n = a[2];
    if (!n) return a[0];
    for (i = 0; i + 1 < n; i++) {
        uint8_t ch = (uint8_t)RD8(a[1] + i);
        if (!ch) break;
        WR8(a[0] + i, ch);
    }
    WR8(a[0] + i, 0);
    return a[0];
}
FN(c_MulDiv) {
    int64_t r;
    (void)w;
    if (!a[2]) return 0xFFFFFFFFu;
    r = (int64_t)(int32_t)a[0] * (int32_t)a[1];
    r = (r >= 0) == ((int32_t)a[2] >= 0) ? (r + (int32_t)a[2] / 2) / (int32_t)a[2] : (r - (int32_t)a[2] / 2) / (int32_t)a[2];
    return (uint32_t)r;
}
FN(c_GetComputerNameA) { uint32_t n = wr_str(w, a[0], RD32(a[1]), "QRXOSONE"); WR32(a[1], n); return 1; }
FN(c_LangId) { UNUSED; return 0x409; }
FN(c_SystemTimeToFileTime) { WR32(a[1], 0xD53E8000u); WR32(a[1] + 4, 0x019DB1DEu); return 1; }
FN(c_GetExitCodeProcess) { if (a[1]) WR32(a[1], 0); return 1; }
FN(c_FormatDT) { uint32_t cap = a[5]; return cap ? wr_str(w, a[4], cap, "1998-01-01") + 1 : 11; }
FN(c_CharUpperBuffA) { uint32_t i; for (i = 0; i < a[1]; i++) WR8(a[0] + i, (uint8_t)toupper(RD8(a[0] + i))); return a[1]; }
FN(c_CharToOemA) {
    uint32_t i = 0;
    for (;; i++) { uint8_t ch = (uint8_t)RD8(a[0] + i); WR8(a[1] + i, ch); if (!ch) break; }
    return 1;
}
FN(c_GetKeyNameTextA) { return wr_str(w, a[1], a[2], "KEY"); }
FN(c_GetWindowThreadProcessId) { if (a[1]) WR32(a[1], 0x100); (void)w; return 0x104; }

/* ================================================================ multimedia */
FN(c_timeGetDevCaps) { WR32(a[0], 1); WR32(a[0] + 4, 1000000); return 0; }
FN(c_mciError) { UNUSED; return 275u; /* MCIERR_FILE_NOT_FOUND: no CD audio device here */ }
FN(c_mciGetErrorStringA) { wr_str(w, a[1], a[2], "No MCI device"); return 1; }

/* mmio on file handles, with RIFF chunk navigation */
FN(c_mmioOpenA) {
    uint32_t h = lopen_mode(w, a[0], (a[2] & 3u) == 0 ? 0x80000000u : 0xC0000000u, (a[2] & 0x1000u) ? 2u : 3u);
    return h == 0xFFFFFFFFu ? 0 : h;
}
FN(c_mmioClose) { qw_file_close(w, a[0]); return 0; }
FN(c_mmioRead) { return c_lread(w, a); }
FN(c_mmioWrite) { return c_lwrite(w, a); }
FN(c_mmioSeek) { return c_llseek(w, a); }
/* MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags */
static uint32_t mm_tell(qwin *w, uint32_t h) { uint32_t b[4]; b[0] = h; b[1] = 0; b[2] = 0; b[3] = 1; return call_api(w, "kernel32", "SetFilePointer", b); }
static void mm_seek(qwin *w, uint32_t h, uint32_t pos) { uint32_t b[4]; b[0] = h; b[1] = pos; b[2] = 0; b[3] = 0; call_api(w, "kernel32", "SetFilePointer", b); }
static uint32_t mm_read(qwin *w, uint32_t h, uint32_t p, uint32_t n) { uint32_t b[3]; b[0] = h; b[1] = p; b[2] = n; return c_lread(w, b); }
FN(c_mmioDescend) {
    uint32_t h = a[0], ck = a[1], par = a[2], fl = a[3], end = par ? RD32(par + 12) + RD32(par + 4) : 0xFFFFFFFFu;
    uint32_t want_id = fl & 0x10u ? RD32(ck) : 0, want_ty = fl & 0x60u ? RD32(ck + 8) : 0;
    uint32_t find_riff = fl & 0x20u, find_list = fl & 0x40u;
    for (;;) {
        uint32_t pos = mm_tell(w, h), id, size, ty = 0;
        if (pos + 8 > end) return 0x103;        /* MMIOERR_CHUNKNOTFOUND */
        if (mm_read(w, h, ck, 8) != 8) return 0x103;
        id = RD32(ck); size = RD32(ck + 4);
        WR32(ck + 12, pos + 8);
        WR32(ck + 16, 0);
        if (id == 0x46464952u || id == 0x5453494Cu) {       /* RIFF, LIST */
            if (mm_read(w, h, ck + 8, 4) != 4) return 0x103;
            ty = RD32(ck + 8);
            WR32(ck + 12, pos + 12);
        }
        if (!(fl & 0x70u)) return 0;
        if ((fl & 0x10u) && id == want_id) return 0;
        if (find_riff && id == 0x46464952u && ty == want_ty) return 0;
        if (find_list && id == 0x5453494Cu && ty == want_ty) return 0;
        mm_seek(w, h, pos + 8 + ((size + 1u) & ~1u));
    }
}
FN(c_mmioAscend) {
    uint32_t off = RD32(a[1] + 12), size = RD32(a[1] + 4), id = RD32(a[1]);
    uint32_t base = (id == 0x46464952u || id == 0x5453494Cu) ? off - 4u : off;
    mm_seek(w, a[0], base + ((size + 1u) & ~1u));
    return 0;
}

/* waveOut goes to the host mixer as one looping voice over a ring in guest memory. */
#define WO_RING (1u << 19)
#define WO_MAXQ 64
typedef struct {
    int open;
    uint32_t cbtype, cb, inst, rate, ch, bits, ba, ring;
    uint64_t written, played;
    uint32_t q[WO_MAXQ];
    uint64_t qend[WO_MAXQ];
    int qn, playing, paused;
    uint32_t lastpos;
    uint64_t clock0;
} qwo;
static qwo g_wo[2];
#define WO_HANDLE 0x7E600000u
#define WO_VOICE_BASE 124u
int qw_ds_voice(qwin *w, uint32_t op, uint32_t voice, uint32_t mem, uint32_t size, uint32_t rate, uint32_t ch, uint32_t bits, int loop, uint32_t arg, uint32_t *pos, int *playing);
enum { DSV_START = 1, DSV_STOP, DSV_SET, DSV_POS, DSV_FREE };

static void wo_notify(qwin *w, uint32_t h, qwo *o, uint32_t msg, uint32_t p1)
{
    uint32_t args[5];
    int ok;
    switch (o->cbtype) {
    case 0x30000u:
        args[0] = h; args[1] = msg; args[2] = o->inst; args[3] = p1; args[4] = 0;
        x86_call(C, o->cb, 5, args, &ok);
        break;
    case 0x10000u: win_post(w, o->cb, msg, h, p1); break;
    case 0x50000u: { qsync *s = sync_of(o->cb); if (s) s->count = 1; break; }
    default: break;
    }
}
static int g_service_up;
static void service_main(qwin *w);
static void service_up(qwin *w) { if (!g_service_up) { g_service_up = 1; qw_thread_spawn_service(w, service_main); } }

static void wo_tick(qwin *w, int idx)
{
    qwo *o = &g_wo[idx];
    uint32_t pos = 0;
    int on = 0;
    if (!o->open || !o->playing) return;
    if (qw_ds_voice(w, DSV_POS, WO_VOICE_BASE + (uint32_t)idx, 0, 0, 0, 0, 0, 0, 0, &pos, &on)) {
        o->played += (uint64_t)((pos - o->lastpos) & (WO_RING - 1u));
        o->lastpos = pos;
    } else {
        o->played = (w->be->now(w) - o->clock0) * (uint64_t)o->rate * o->ba / 1000u;
    }
    while (o->qn && o->played >= o->qend[0]) {
        uint32_t hdr = o->q[0];
        int i;
        WR32(hdr + 16, (RD32(hdr + 16) & ~0x10u) | 1u);    /* DONE, not INQUEUE */
        for (i = 1; i < o->qn; i++) { o->q[i - 1] = o->q[i]; o->qend[i - 1] = o->qend[i]; }
        o->qn--;
        wo_notify(w, WO_HANDLE + (uint32_t)idx, o, 0x3BD, hdr);
    }
    if (!o->qn && o->played >= o->written) {
        qw_ds_voice(w, DSV_STOP, WO_VOICE_BASE + (uint32_t)idx, 0, 0, 0, 0, 0, 0, 0, NULL, NULL);
        o->playing = 0;
    }
}
FN(c_waveOutOpen) {
    uint32_t fmt = a[2], tag = RD16(fmt), flags = a[5], rate = RD32(fmt + 4), ch = RD16(fmt + 2), bits = RD16(fmt + 14);
    int i;
    qwo *o;
    if (tag != 1 || (bits != 8 && bits != 16) || (ch != 1 && ch != 2)) return 32;   /* WAVERR_BADFORMAT */
    if (flags & 1u) return 0;                                                       /* query only */
    for (i = 0; i < 2; i++) if (!g_wo[i].open) break;
    if (i == 2) return 4;                                                           /* MMSYSERR_ALLOCATED */
    o = &g_wo[i];
    if (!o->ring) o->ring = qw_alloc(w, WO_RING + 16);
    if (!o->ring) return 7;
    o->open = 1; o->cbtype = flags & 0x70000u; o->cb = a[3]; o->inst = a[4];
    o->rate = rate; o->ch = ch; o->bits = bits; o->ba = ch * bits / 8u;
    o->written = o->played = 0; o->qn = 0; o->playing = 0; o->lastpos = 0;
    if (a[0]) WR32(a[0], WO_HANDLE + (uint32_t)i);
    service_up(w);
    wo_notify(w, WO_HANDLE + (uint32_t)i, o, 0x3BB, 0);
    return 0;
}
FN(c_waveOutClose) {
    uint32_t i = a[0] - WO_HANDLE;
    if (i >= 2 || !g_wo[i].open) return 5;
    qw_ds_voice(w, DSV_FREE, WO_VOICE_BASE + i, 0, 0, 0, 0, 0, 0, 0, NULL, NULL);
    g_wo[i].open = 0;
    wo_notify(w, a[0], &g_wo[i], 0x3BC, 0);
    return 0;
}
FN(c_waveOutPrepareHeader) { WR32(a[1] + 16, RD32(a[1] + 16) | 2u); return 0; }
FN(c_waveOutUnprepareHeader) { WR32(a[1] + 16, RD32(a[1] + 16) & ~2u); return 0; }
FN(c_waveOutWrite) {
    uint32_t i = a[0] - WO_HANDLE, hdr = a[1], data, len, wp, n;
    qwo *o;
    if (i >= 2 || !g_wo[i].open) return 5;
    o = &g_wo[i];
    data = RD32(hdr); len = RD32(hdr + 4);
    if (o->qn >= WO_MAXQ || o->written + len > o->played + WO_RING - o->ba) {
        /* The ring holds half a second or more of data; beyond that the caller waits. */
        while (!w->exiting && (o->qn >= WO_MAXQ || o->written + len > o->played + WO_RING - o->ba)) {
            qw_thread_block(w, 2);
            wo_tick(w, (int)i);
        }
    }
    wp = (uint32_t)(o->written & (WO_RING - 1u));
    n = len < WO_RING - wp ? len : WO_RING - wp;
    {
        uint8_t *dst = x86_host_ptr(C, o->ring + wp);
        if (dst) x86_read_mem(C, data, dst, n);
        if (n < len) { dst = x86_host_ptr(C, o->ring); if (dst) x86_read_mem(C, data + n, dst, len - n); }
    }
    o->written += len;
    WR32(hdr + 16, (RD32(hdr + 16) | 0x10u) & ~1u);
    o->q[o->qn] = hdr;
    o->qend[o->qn] = o->written;
    o->qn++;
    if (!o->playing && !o->paused) {
        o->playing = 1;
        o->clock0 = w->be->now(w) - (o->played * 1000u) / (o->rate * o->ba);
        o->lastpos = (uint32_t)(o->played & (WO_RING - 1u));
        qw_ds_voice(w, DSV_SET, WO_VOICE_BASE + i, o->ring, WO_RING, o->rate, o->ch, o->bits, 1, o->lastpos + 1u, NULL, NULL);
        qw_ds_voice(w, DSV_START, WO_VOICE_BASE + i, o->ring, WO_RING, o->rate, o->ch, o->bits, 1, 0, NULL, NULL);
    }
    return 0;
}
FN(c_waveOutReset) {
    uint32_t i = a[0] - WO_HANDLE;
    qwo *o;
    if (i >= 2 || !g_wo[i].open) return 5;
    o = &g_wo[i];
    qw_ds_voice(w, DSV_STOP, WO_VOICE_BASE + i, 0, 0, 0, 0, 0, 0, 0, NULL, NULL);
    o->playing = 0;
    o->played = o->written;
    while (o->qn) {
        uint32_t hdr = o->q[0];
        int k;
        WR32(hdr + 16, (RD32(hdr + 16) & ~0x10u) | 1u);
        for (k = 1; k < o->qn; k++) o->q[k - 1] = o->q[k];
        o->qn--;
        wo_notify(w, a[0], o, 0x3BD, hdr);
    }
    return 0;
}
FN(c_waveOutPause) { uint32_t i = a[0] - WO_HANDLE; if (i < 2) { g_wo[i].paused = 1; if (g_wo[i].playing) { qw_ds_voice(w, DSV_STOP, WO_VOICE_BASE + i, 0, 0, 0, 0, 0, 0, 0, NULL, NULL); g_wo[i].playing = 0; } } return 0; }
FN(c_waveOutRestart) { uint32_t i = a[0] - WO_HANDLE; (void)w; if (i < 2) g_wo[i].paused = 0; return 0; }
/* MMTIME: wType, then the value */
FN(c_waveOutGetPosition) {
    uint32_t i = a[0] - WO_HANDLE;
    qwo *o;
    if (i >= 2) return 5;
    o = &g_wo[i];
    wo_tick(w, (int)i);
    if (RD32(a[1]) == 2) WR32(a[1] + 4, (uint32_t)(o->played / o->ba));      /* TIME_SAMPLES */
    else { WR32(a[1], 4); WR32(a[1] + 4, (uint32_t)o->played); }             /* TIME_BYTES */
    return 0;
}
FN(c_waveOutGetDevCapsA) {
    uint32_t p = a[1], k;
    for (k = 0; k < a[2] && k < 52; k += 4) WR32(p + k, 0);
    WR16(p, 1); WR16(p + 2, 1); WR32(p + 4, 0x400);
    wr_str(w, p + 8, 32, "QRX Wave Out");
    WR32(p + 40, 0xFFF);                     /* every format up to 44 kHz 16 bit stereo */
    WR16(p + 44, 2);
    WR32(p + 48, 0x0C);
    return 0;
}
FN(c_waveOutGetID) { WR32(a[1], 0); (void)w; return 0; }

/* Timers run on the service thread, which has a guest stack for their callbacks. */
#define MAXTIMER 16
static struct { int used, periodic; uint32_t delay, cb, user, flags; uint64_t next; } g_tm[MAXTIMER];
FN(c_timeSetEvent) {
    int i;
    for (i = 0; i < MAXTIMER; i++) if (!g_tm[i].used) {
        g_tm[i].used = 1;
        g_tm[i].delay = a[0] ? a[0] : 1;
        g_tm[i].cb = a[2];
        g_tm[i].user = a[3];
        g_tm[i].flags = a[4];
        g_tm[i].periodic = (a[4] & 1u) != 0;
        g_tm[i].next = w->be->now(w) + g_tm[i].delay;
        service_up(w);
        return (uint32_t)i + 1u;
    }
    return 0;
}
FN(c_timeKillEvent) { uint32_t i = a[0] - 1u; UNUSED; if (i < MAXTIMER && g_tm[i].used) { g_tm[i].used = 0; return 0; } return 97; }
void qw_midi_tick(qwin *w);
static void service_main(qwin *w)
{
    while (!w->exiting) {
        uint64_t now = w->be->now(w);
        int i;
        for (i = 0; i < MAXTIMER; i++) {
            if (!g_tm[i].used || now < g_tm[i].next) continue;
            if (g_tm[i].periodic) g_tm[i].next = now + g_tm[i].delay; else g_tm[i].used = 0;
            if (g_tm[i].flags & 0x30u) {          /* TIME_CALLBACK_EVENT_SET / PULSE */
                qsync *s = sync_of(g_tm[i].cb);
                if (s) s->count = 1;
            } else {
                uint32_t args[5];
                int ok;
                args[0] = (uint32_t)i + 1u; args[1] = 0; args[2] = g_tm[i].user; args[3] = 0; args[4] = 0;
                x86_call(C, g_tm[i].cb, 5, args, &ok);
            }
        }
        wo_tick(w, 0);
        wo_tick(w, 1);
        qw_midi_tick(w);
        qw_thread_block(w, 2);
    }
}
void qw_service_start(qwin *w) { service_up(w); }

/* ================================================================ MIDI, played by the host */
#define DOOR_SVC_MIDI 3u
#define MIDI_HANDLE 0x7E800000u
#define MIDI_STREAM 0x7E800010u
#define MIDI_MAXQ 32
static uint32_t midi_send(qwin *w, uint32_t op, uint32_t msg, uint32_t data, uint32_t len)
{
    static uint32_t q[16];
    if (!C->door) return 0;
    q[0] = DOOR_SVC_MIDI; q[3] = op; q[4] = msg; q[5] = data; q[6] = len;
    return (uint32_t)x86_door_call(C, q);
}
enum { MD_SHORT = 1, MD_LONG, MD_RESET };
static struct {
    int open, playing;
    uint32_t cbtype, cb, inst, ppqn, tempo;    /* tempo: microseconds per quarter note */
    uint32_t q[MIDI_MAXQ];
    int qn;
    uint32_t off;                              /* in the first buffer */
    uint64_t now_ticks_x, last_ms;             /* ticks scaled by 1000 */
    uint64_t due;                              /* tick of the next event, scaled */
    int have_due;
    uint32_t volume;
} g_st;
static uint32_t g_mo_cbtype, g_mo_cb, g_mo_inst;
static void midi_notify(qwin *w, uint32_t cbtype, uint32_t cb, uint32_t inst, uint32_t h, uint32_t msg, uint32_t p1)
{
    uint32_t args[5];
    int ok;
    switch (cbtype) {
    case 0x30000u:
        args[0] = h; args[1] = msg; args[2] = inst; args[3] = p1; args[4] = 0;
        x86_call(C, cb, 5, args, &ok);
        break;
    case 0x10000u: win_post(w, cb, msg, h, p1); break;
    case 0x50000u: { qsync *s = sync_of(cb); if (s) s->count = 1; break; }
    default: break;
    }
}
FN(c_midiOutGetNumDevs) { UNUSED; return C->door ? 1u : 0u; }
FN(c_midiOutGetDevCapsA) {
    uint32_t p = a[1], k;
    for (k = 0; k < a[2] && k < 52; k += 4) WR32(p + k, 0);
    WR16(p, 1); WR16(p + 2, 1); WR32(p + 4, 0x100);
    wr_str(w, p + 8, 32, "QRX MIDI Synthesizer");
    WR16(p + 40, 4);                 /* MOD_SYNTH */
    WR16(p + 42, 128); WR16(p + 44, 16); WR16(p + 46, 0xFFFF);
    WR32(p + 48, 1);                 /* MIDICAPS_VOLUME */
    return 0;
}
FN(c_midiOutOpen) {
    if (!C->door) return 2;
    g_mo_cbtype = a[4] & 0x70000u; g_mo_cb = a[2]; g_mo_inst = a[3];
    if (a[0]) WR32(a[0], MIDI_HANDLE);
    midi_notify(w, g_mo_cbtype, g_mo_cb, g_mo_inst, MIDI_HANDLE, 0x3C7, 0);
    return 0;
}
FN(c_midiOutClose) {
    midi_send(w, MD_RESET, 0, 0, 0);
    midi_notify(w, g_mo_cbtype, g_mo_cb, g_mo_inst, MIDI_HANDLE, 0x3C8, 0);
    return 0;
}
FN(c_midiOutShortMsg) { midi_send(w, MD_SHORT, a[1], 0, 0); return 0; }
FN(c_midiOutLongMsg) {
    uint32_t len = RD32(a[1] + 4);
    midi_send(w, MD_LONG, 0, RD32(a[1]), len);
    WR32(a[1] + 16, RD32(a[1] + 16) | 1u);
    midi_notify(w, g_mo_cbtype, g_mo_cb, g_mo_inst, a[0], 0x3C9, a[1]);
    return 0;
}
FN(c_midiOutReset) { midi_send(w, MD_RESET, 0, 0, 0); return 0; }
FN(c_midiPrepare) { WR32(a[1] + 16, RD32(a[1] + 16) | 2u); return 0; }
FN(c_midiUnprepare) { WR32(a[1] + 16, RD32(a[1] + 16) & ~2u); return 0; }
FN(c_midiOutGetVolume) { if (a[1]) WR32(a[1], g_st.volume); return 0; }
FN(c_midiOutSetVolume) {
    /* Device volume as channel volume on all 16 channels. */
    uint32_t ch, v = ((a[1] & 0xFFFFu) * 127u) / 0xFFFFu;
    g_st.volume = a[1];
    for (ch = 0; ch < 16; ch++) midi_send(w, MD_SHORT, 0xB0u | ch | (7u << 8) | (v << 16), 0, 0);
    return 0;
}
FN(c_midiStreamOpen) {
    if (!C->door) return 2;
    memset(&g_st.q, 0, sizeof(g_st.q));
    g_st.open = 1; g_st.playing = 0; g_st.qn = 0; g_st.off = 0; g_st.have_due = 0;
    g_st.cbtype = a[5] & 0x70000u; g_st.cb = a[3]; g_st.inst = a[4];
    g_st.ppqn = 96; g_st.tempo = 500000;
    g_st.now_ticks_x = 0;
    if (a[0]) WR32(a[0], MIDI_STREAM);
    service_up(w);
    midi_notify(w, g_st.cbtype, g_st.cb, g_st.inst, MIDI_STREAM, 0x3C7, 0);
    return 0;
}
FN(c_midiStreamClose) {
    g_st.open = 0; g_st.qn = 0;
    midi_send(w, MD_RESET, 0, 0, 0);
    midi_notify(w, g_st.cbtype, g_st.cb, g_st.inst, MIDI_STREAM, 0x3C8, 0);
    return 0;
}
FN(c_midiStreamOut) {
    if (!g_st.open || g_st.qn >= MIDI_MAXQ) return 3;
    g_st.q[g_st.qn++] = a[1];
    WR32(a[1] + 16, (RD32(a[1] + 16) | 0x10u) & ~1u);
    return 0;
}
FN(c_midiStreamRestart) { g_st.playing = 1; g_st.last_ms = w->be->now(w); return 0; }
FN(c_midiStreamPause) { g_st.playing = 0; (void)w; (void)a; return 0; }
static void stream_done_first(qwin *w)
{
    uint32_t hdr = g_st.q[0];
    int i;
    WR32(hdr + 16, (RD32(hdr + 16) & ~0x10u) | 1u);
    for (i = 1; i < g_st.qn; i++) g_st.q[i - 1] = g_st.q[i];
    g_st.qn--;
    g_st.off = 0;
    g_st.have_due = 0;
    midi_notify(w, g_st.cbtype, g_st.cb, g_st.inst, MIDI_STREAM, 0x3C9, hdr);
}
FN(c_midiStreamStop) {
    g_st.playing = 0;
    midi_send(w, MD_RESET, 0, 0, 0);
    while (g_st.qn) stream_done_first(w);
    (void)a;
    return 0;
}
FN(c_midiStreamProperty) {
    uint32_t fl = a[2];
    if (fl & 2u) {                                  /* GET */
        if (fl & 0x40000000u) WR32(a[1] + 4, g_st.tempo);
        if (fl & 0x80000000u) WR32(a[1] + 4, g_st.ppqn);
    } else if (fl & 1u) {                           /* SET */
        if (fl & 0x40000000u) g_st.tempo = RD32(a[1] + 4);
        if (fl & 0x80000000u) g_st.ppqn = RD32(a[1] + 4);
    }
    return 0;
}
FN(c_midiStreamPosition) {
    uint32_t t = g_st.ppqn ? (uint32_t)(g_st.now_ticks_x / 1000u) : 0;
    WR32(a[1], 0x10);       /* TIME_TICKS */
    WR32(a[1] + 4, t);
    return 0;
}
void qw_midi_tick(qwin *w)
{
    uint64_t now;
    if (!g_st.open || !g_st.playing) return;
    now = w->be->now(w);
    if (g_st.tempo) g_st.now_ticks_x += (now - g_st.last_ms) * 1000u * 1000u * (uint64_t)g_st.ppqn / g_st.tempo;
    g_st.last_ms = now;
    while (g_st.qn) {
        uint32_t hdr = g_st.q[0], data = RD32(hdr), used = RD32(hdr + 8) ? RD32(hdr + 8) : RD32(hdr + 4);
        uint32_t ev;
        if (g_st.off + 12u > used) { stream_done_first(w); continue; }
        if (!g_st.have_due) {
            g_st.due = (g_st.qn && g_st.off == 0 ? g_st.now_ticks_x : g_st.due) + (uint64_t)RD32(data + g_st.off) * 1000u;
            g_st.have_due = 1;
        }
        if (g_st.now_ticks_x < g_st.due) return;
        ev = RD32(data + g_st.off + 8);
        if (ev & 0x80000000u) {                    /* MEVT_F_LONG */
            uint32_t len = ev & 0xFFFFFFu;
            if ((ev >> 24 & 0x7Fu) == 0x80u) midi_send(w, MD_LONG, 0, data + g_st.off + 12u, len);
            g_st.off += 12u + ((len + 3u) & ~3u);
        } else {
            uint32_t type = ev >> 24 & 0x3Fu;
            if (type == 0) midi_send(w, MD_SHORT, ev & 0xFFFFFFu, 0, 0);
            else if (type == 1) g_st.tempo = ev & 0xFFFFFFu;
            g_st.off += 12u;
        }
        g_st.have_due = 0;
        { uint64_t d = g_st.due; if (g_st.off + 12u <= used) { g_st.due = d + (uint64_t)RD32(data + g_st.off) * 1000u; g_st.have_due = 1; } else g_st.due = d; }
    }
}

/* ================================================================ system DLLs as files */
/* Some engines look for ddraw.dll in the system directory before loading it. */
#define FIND_FAKE 0x7E700001u
static int is_system_dll_path(const char *p)
{
    const char *b = strrchr(p, '\\');
    if (strncasecmp(p, "C:\\WINDOWS\\SYSTEM", 17) || !b) return 0;
    return qw_is_hle_dll(b + 1);
}
FN(c_FindFirstFileA) {
    char p[300];
    rd_str(w, a[0], p, sizeof(p));
    if (is_system_dll_path(p)) {
        uint32_t k;
        const char *b = strrchr(p, '\\') + 1;
        for (k = 0; k < 320; k += 4) WR32(a[1] + k, 0);
        WR32(a[1], 0x20);
        WR32(a[1] + 32, 100000);
        wr_str(w, a[1] + 44, 260, b);
        return FIND_FAKE;
    }
    return call_api(w, "files-next", "FindFirstFileA", a);
}
FN(c_FindClose) {
    if (a[0] == FIND_FAKE) return 1;
    return call_api(w, "files-next", "FindClose", a);
}
FN(c_FindNextFileA) {
    if (a[0] == FIND_FAKE) { w->last_error = 18; return 0; }
    return call_api(w, "files-next", "FindNextFileA", a);
}

/* ================================================================ DirectPlay */
/* Without a network session object, programs report that multiplayer is unavailable. */
FN(c_DirectPlayCreate) { if (a[1]) WR32(a[1], 0); UNUSED; return 0x88770078u; /* DPERR_UNAVAILABLE */ }

/* ================================================================ small answers */
FN(c_GetPaletteEntries) { UNUSED; if (a[3]) { uint32_t i; for (i = 0; i < a[2]; i++) WR32(a[3] + i * 4, 0); } return a[2]; }
FN(c_GetDIBColorTable) { UNUSED; return 0; }
FN(c_GetShortPathNameA) { char p[300]; rd_str(w, a[0], p, sizeof(p)); return wr_str(w, a[1], a[2], p); }
FN(c_FatalAppExitA) { char m[200]; rd_str(w, a[1], m, sizeof(m)); qw_log(w, "FatalAppExit: %s", m); w->exiting = 1; C->stop = 1; return 0; }
FN(c_GetClipBox) { if (a[1]) { WR32(a[1], 0); WR32(a[1] + 4, 0); WR32(a[1] + 8, 640); WR32(a[1] + 12, 480); } return 2; /* SIMPLEREGION */ }
FN(c_GetProcessVersion) { UNUSED; return 0x00040000u; }
FN(c_RegQueryValueA) { UNUSED; return 2; }
/* No codecs: only metric asked by games at start, the largest format size, gets an answer. */
FN(c_acmMetrics) { if (a[2]) WR32(a[2], a[1] == 50 ? 50u : 0u); return 0; }
static uint32_t g_prio_class = 0x20, g_thread_prio;
FN(c_SetPriorityClass) { (void)w; g_prio_class = a[1]; return 1; }
FN(c_GetPriorityClass) { UNUSED; return g_prio_class; }
FN(c_SetThreadPriority) { (void)w; g_thread_prio = a[1]; return 1; }
FN(c_GetThreadPriority) { UNUSED; return g_thread_prio; }
/* A movable block that is no longer locked: 0, and no error. */
FN(c_GlobalUnlock) { UNUSED; w->last_error = 0; return 0; }
FN(c_acmNoDriver) { UNUSED; return 0x200u + 11u; /* ACMERR: not possible */ }
FN(c_FileVersionNone) { UNUSED; return 0; }
FN(c_GetCharWidthA) { uint32_t i; for (i = a[1]; i <= a[2] && i - a[1] < 256; i++) WR32(a[3] + (i - a[1]) * 4, 8); return 1; }
FN(c_CharPrevA) { (void)w; return a[1] > a[0] ? a[1] - 1 : a[0]; }
FN(c_ShellExecuteA) { UNUSED; return 2; /* file not found */ }
FN(c_atom_add) { static uint32_t n = 0xC100; UNUSED; return n++; }
FN(c_BeginDeferWindowPos) { UNUSED; return 0x7E900001u; }

/* ================================================================ COM */
FN(c_CoCreateInstance) { if (a[4]) WR32(a[4], 0); UNUSED; return 0x80040154u; /* REGDB_E_CLASSNOTREG */ }

#define K(n, f, c) { "kernel32", n, c, f }
#define U(n, f, c) { "user32", n, c, f }
#define G(n, f, c) { "gdi32", n, c, f }
#define ADV(n, f, c) { "advapi32", n, c, f }
#define WM(n, f, c) { "winmm", n, c, f }
const qhle qw_compat[] = {
    /* threads and synchronisation */
    K("CreateThread", qw_api_CreateThread, 6), K("ExitThread", qw_api_ExitThread, 1), K("ResumeThread", qw_api_ResumeThread, 1),
    K("SuspendThread", qw_api_SuspendThread, 1), K("TerminateThread", qw_api_TerminateThread, 2), K("GetExitCodeThread", qw_api_GetExitCodeThread, 2),
    K("SetThreadPriority", c_SetThreadPriority, 2), K("GetThreadPriority", c_GetThreadPriority, 1), K("GetPriorityClass", c_GetPriorityClass, 1), K("SetPriorityClass", c_SetPriorityClass, 2), K("GlobalUnlock", c_GlobalUnlock, 1),
    K("CreateEventA", c_CreateEventA, 4), K("CreateMutexA", c_CreateMutexA, 3), K("CreateSemaphoreA", c_CreateSemaphoreA, 4),
    K("SetEvent", c_SetEvent, 1), K("ResetEvent", c_ResetEvent, 1), K("PulseEvent", c_PulseEvent, 1),
    K("ReleaseMutex", c_ReleaseMutex, 1), K("ReleaseSemaphore", c_ReleaseSemaphore, 3),
    K("WaitForSingleObject", c_WaitForSingleObject, 2), K("WaitForMultipleObjects", c_WaitForMultipleObjects, 4),
    K("WaitForSingleObjectEx", c_WaitForSingleObject, 3), K("WaitForMultipleObjectsEx", c_WaitForMultipleObjects, 5),
    K("CloseHandle", c_CloseHandle, 1), K("Sleep", c_Sleep, 1), K("SleepEx", c_SleepEx, 2),
    K("GetCurrentThreadId", c_GetCurrentThreadId, 0),
    K("InitializeCriticalSection", c_InitializeCriticalSection, 1), K("InitializeCriticalSectionAndSpinCount", c_InitializeCriticalSection, 2),
    K("EnterCriticalSection", c_EnterCriticalSection, 1), K("LeaveCriticalSection", c_LeaveCriticalSection, 1),
    K("TryEnterCriticalSection", c_TryEnterCriticalSection, 1),
    K("TlsGetValue", c_TlsGetValue, 1), K("TlsSetValue", c_TlsSetValue, 2), K("DuplicateHandle", c_DuplicateHandle, 7),
    /* registry */
    ADV("RegOpenKeyA", c_RegOpenKeyA, 3), ADV("RegOpenKeyExA", c_RegOpenKeyExA, 5), ADV("RegCreateKeyA", c_RegCreateKeyA, 3),
    ADV("RegCreateKeyExA", c_RegCreateKeyExA, 9), ADV("RegCloseKey", c_RegCloseKey, 1), ADV("RegQueryValueExA", c_RegQueryValueExA, 6),
    ADV("RegSetValueExA", c_RegSetValueExA, 6), ADV("RegDeleteValueA", c_RegDeleteValueA, 2), ADV("RegEnumValueA", c_RegEnumValueA, 8),
    ADV("RegEnumKeyExA", c_RegNoMore, 8), ADV("RegEnumKeyA", c_RegNoMore, 4), ADV("RegFlushKey", c_ok0, 1), ADV("RegDeleteKeyA", c_ok0, 2),
    ADV("RegQueryInfoKeyA", c_RegQueryInfoKeyA, 12),
    /* INI files */
    K("GetPrivateProfileStringA", c_GetPrivateProfileStringA, 6), K("GetPrivateProfileIntA", c_GetPrivateProfileIntA, 4),
    K("WritePrivateProfileStringA", c_ok1, 4), K("GetProfileStringA", c_GetProfileStringA, 5), K("GetProfileIntA", c_GetProfileIntA, 3),
    /* old-style files */
    K("_lopen", c_lopen, 2), K("_lcreat", c_lcreat, 2), K("_lclose", c_lclose, 1), K("_lread", c_lread, 3), K("_hread", c_lread, 3),
    K("_lwrite", c_lwrite, 3), K("_hwrite", c_lwrite, 3), K("_llseek", c_llseek, 3), K("OpenFile", c_OpenFile, 3),
    K("GetSystemDirectoryA", c_GetSystemDirectoryA, 2), K("GetWindowsDirectoryA", c_GetWindowsDirectoryA, 2),
    K("GetTempFileNameA", c_GetTempFileNameA, 4), K("CopyFileA", c_CopyFileA, 3), K("SetFileTime", c_ok1, 4),
    /* memory */
    K("GlobalMemoryStatus", c_GlobalMemoryStatus, 1), K("GlobalHandle", c_arg0, 1), K("LocalHandle", c_arg0, 1), K("GlobalReAlloc", c_GlobalReAlloc, 3),
    K("LocalSize", c_LocalSize, 1), K("VirtualLock", c_ok1, 2), K("VirtualUnlock", c_ok1, 2), K("HeapCompact", c_ok0, 2), K("HeapValidate", c_ok1, 3),
    K("CreateFileMappingA", c_CreateFileMappingA, 6), K("OpenFileMappingA", c_OpenFileMappingA, 3), K("MapViewOfFile", c_MapViewOfFile, 5),
    K("UnmapViewOfFile", c_ok1, 1), K("FlushViewOfFile", c_ok1, 2), K("FreeResource", c_ok0, 1),
    /* strings and misc */
    K("lstrcmpA", c_lstrcmpA, 2), K("lstrcmpiA", c_lstrcmpiA, 2), K("lstrcpynA", c_lstrcpynA, 3), K("MulDiv", c_MulDiv, 3),
    K("SetConsoleCtrlHandler", c_ok1, 2), K("GetComputerNameA", c_GetComputerNameA, 2), K("GetSystemDefaultLangID", c_LangId, 0),
    K("GetUserDefaultLangID", c_LangId, 0), K("GetThreadLocale", c_LangId, 0), K("SystemTimeToFileTime", c_SystemTimeToFileTime, 2),
    K("GetExitCodeProcess", c_GetExitCodeProcess, 2), K("WinExec", c_ok0, 2), K("GetDateFormatA", c_FormatDT, 6), K("GetTimeFormatA", c_FormatDT, 6),
    K("IsValidLocale", c_ok1, 2), K("IsValidCodePage", c_ok1, 1), K("SetEnvironmentVariableW", c_ok1, 2), K("UTRegister", c_ok0, 7), K("UTUnRegister", c_ok0, 1),
    K("GetOverlappedResult", c_ok0, 4), K("PeekNamedPipe", c_ok0, 6), K("GetCommState", c_ok0, 2), K("SetCommState", c_ok0, 2), K("SetCommTimeouts", c_ok0, 2),
    K("GetCommModemStatus", c_ok0, 2), K("GetCommProperties", c_ok0, 2), K("PurgeComm", c_ok0, 2), K("EscapeCommFunction", c_ok0, 2),
    K("LoadModule", c_ok0, 2),
    /* user32 */
    U("WaitForInputIdle", c_ok0, 2), U("GetUpdateRect", c_ok0, 3), U("GetUpdateRgn", c_ok0, 3), U("GetWindowThreadProcessId", c_GetWindowThreadProcessId, 2),
    U("GetTopWindow", c_ok0, 1), U("EnumThreadWindows", c_ok1, 3), U("EnumWindows", c_ok1, 2), U("SetSysColors", c_ok1, 3),
    U("GetKeyNameTextA", c_GetKeyNameTextA, 3), U("CharUpperBuffA", c_CharUpperBuffA, 2), U("CharToOemA", c_CharToOemA, 2), U("OemToCharA", c_CharToOemA, 2),
    U("mouse_event", c_ok0, 5), U("keybd_event", c_ok0, 4), U("GetCaretBlinkTime", c_ok1, 0), U("GetDoubleClickTime", c_ok1, 0),
    U("ShowWindowAsync", c_ok1, 2), U("CloseWindow", c_ok1, 1), U("SetScrollPos", c_ok0, 4), U("SetScrollRange", c_ok1, 5), U("GetScrollPos", c_ok0, 2),
    U("ShowScrollBar", c_ok1, 3), U("SetScrollInfo", c_ok0, 4), U("GetKeyboardType", c_ok0, 1), U("GetInputState", c_ok0, 0),
    U("SetCaretPos", c_ok1, 2), U("CreateCaret", c_ok1, 4), U("DestroyCaret", c_ok1, 0), U("HideCaret", c_ok1, 1), U("ShowCaret", c_ok1, 1),
    U("DestroyAcceleratorTable", c_ok1, 1), U("CreateAcceleratorTableA", c_ok1, 2), U("GetClipCursor", c_ok1, 1), U("AnyPopup", c_ok0, 0), U("InsertMenuA", c_ok1, 5),
    /* gdi32 */
    G("SetStretchBltMode", c_ok1, 2), G("GetStretchBltMode", c_ok1, 1), G("GdiFlush", c_ok1, 0), G("SetMapMode", c_ok1, 2), G("GetMapMode", c_ok1, 1),
    G("SetWindowOrgEx", c_ok1, 4), G("SetViewportOrgEx", c_ok1, 4), G("GetBkMode", c_ok1, 1), G("ResizePalette", c_ok1, 2), G("AnimatePalette", c_ok1, 4),
    G("GetNearestPaletteIndex", c_ok0, 2), G("CreateRectRgnIndirect", c_ok1, 1), G("CreateHalftonePalette", c_ok1, 1), G("AddFontResourceA", c_ok1, 1),
    G("RemoveFontResourceA", c_ok1, 1), G("GetRegionData", c_ok0, 3),
    /* winmm */
    WM("timeGetDevCaps", c_timeGetDevCaps, 2), WM("timeSetEvent", c_timeSetEvent, 5), WM("timeKillEvent", c_timeKillEvent, 1),
    WM("mciSendCommandA", c_mciError, 4), WM("mciSendStringA", c_mciError, 4), WM("mciGetErrorStringA", c_mciGetErrorStringA, 3), WM("mciGetDeviceIDA", c_ok0, 1),
    WM("mmioOpenA", c_mmioOpenA, 3), WM("mmioClose", c_mmioClose, 2), WM("mmioRead", c_mmioRead, 3), WM("mmioWrite", c_mmioWrite, 3), WM("mmioSeek", c_mmioSeek, 3),
    WM("mmioDescend", c_mmioDescend, 4), WM("mmioAscend", c_mmioAscend, 3), WM("mmioGetInfo", c_ok1, 3), WM("mmioSetInfo", c_ok0, 3), WM("mmioAdvance", c_ok1, 3),
    WM("joyGetPos", c_fail_mm, 2), WM("joySetCapture", c_fail_mm, 4), WM("joyReleaseCapture", c_ok0, 1),
    WM("auxGetNumDevs", c_ok0, 0), WM("auxGetDevCapsA", c_fail_mm, 3), WM("auxSetVolume", c_fail_mm, 2), WM("auxGetVolume", c_fail_mm, 2),
    WM("mixerGetNumDevs", c_ok0, 0), WM("mixerOpen", c_fail_mm, 5), WM("mixerClose", c_ok0, 1), WM("mixerGetDevCapsA", c_fail_mm, 3),
    WM("mixerGetLineInfoA", c_fail_mm, 3), WM("mixerGetLineControlsA", c_fail_mm, 3), WM("mixerGetControlDetailsA", c_fail_mm, 3), WM("mixerSetControlDetails", c_fail_mm, 3),
    WM("sndPlaySoundA", c_ok1, 2),
    WM("midiOutGetNumDevs", c_midiOutGetNumDevs, 0), WM("midiOutGetDevCapsA", c_midiOutGetDevCapsA, 3), WM("midiOutOpen", c_midiOutOpen, 5),
    WM("midiOutClose", c_midiOutClose, 1), WM("midiOutShortMsg", c_midiOutShortMsg, 2), WM("midiOutLongMsg", c_midiOutLongMsg, 3),
    WM("midiOutReset", c_midiOutReset, 1), WM("midiOutPrepareHeader", c_midiPrepare, 3), WM("midiOutUnprepareHeader", c_midiUnprepare, 3),
    WM("midiOutGetVolume", c_midiOutGetVolume, 2), WM("midiOutSetVolume", c_midiOutSetVolume, 2),
    WM("midiStreamOpen", c_midiStreamOpen, 6), WM("midiStreamClose", c_midiStreamClose, 1), WM("midiStreamOut", c_midiStreamOut, 3),
    WM("midiStreamRestart", c_midiStreamRestart, 1), WM("midiStreamPause", c_midiStreamPause, 1), WM("midiStreamStop", c_midiStreamStop, 1),
    WM("midiStreamProperty", c_midiStreamProperty, 3), WM("midiStreamPosition", c_midiStreamPosition, 3),
    WM("waveOutGetNumDevs", c_ok1, 0), WM("waveOutOpen", c_waveOutOpen, 6), WM("waveOutClose", c_waveOutClose, 1),
    WM("waveOutPrepareHeader", c_waveOutPrepareHeader, 3), WM("waveOutUnprepareHeader", c_waveOutUnprepareHeader, 3), WM("waveOutWrite", c_waveOutWrite, 3),
    WM("waveOutReset", c_waveOutReset, 1), WM("waveOutPause", c_waveOutPause, 1), WM("waveOutRestart", c_waveOutRestart, 1),
    WM("waveOutGetPosition", c_waveOutGetPosition, 3), WM("waveOutGetDevCapsA", c_waveOutGetDevCapsA, 3), WM("waveOutGetID", c_waveOutGetID, 2),
    WM("waveOutSetVolume", c_ok0, 2), WM("waveOutGetVolume", c_ok0, 2),
    K("FindFirstFileA", c_FindFirstFileA, 2), K("FindNextFileA", c_FindNextFileA, 2), K("FindClose", c_FindClose, 1),
    { "dplayx", "DirectPlayCreate", 3, c_DirectPlayCreate }, { "dplayx", "#1", 3, c_DirectPlayCreate },
    { "dplayx", "DirectPlayEnumerateA", 2, c_ok0 }, { "dplayx", "#2", 2, c_ok0 },
    { "dplayx", "DirectPlayEnumerate", 2, c_ok0 }, { "dplayx", "#9", 2, c_ok0 },
    { "dplayx", "DirectPlayLobbyCreateA", 5, c_DirectPlayCreate }, { "dplayx", "#4", 5, c_DirectPlayCreate },
    { "dplay", "DirectPlayCreate", 3, c_DirectPlayCreate }, { "dplay", "DirectPlayEnumerateA", 2, c_ok0 }, { "dplay", "DirectPlayEnumerate", 2, c_ok0 },
    { "comctl32", "#17", 0, c_ok0 }, { "comctl32", "InitCommonControls", 0, c_ok0 },
    G("GetPaletteEntries", c_GetPaletteEntries, 4), G("GetDIBColorTable", c_GetDIBColorTable, 4), G("RectVisible", c_ok1, 2), G("PtVisible", c_ok1, 3),
    G("GetClipBox", c_GetClipBox, 2), G("SetWindowExtEx", c_ok1, 4), G("SetViewportExtEx", c_ok1, 4), G("OffsetViewportOrgEx", c_ok1, 4),
    G("ScaleViewportExtEx", c_ok1, 6), G("ScaleWindowExtEx", c_ok1, 6), G("GetCharWidthA", c_GetCharWidthA, 4), G("DPtoLP", c_ok1, 3), G("LPtoDP", c_ok1, 3),
    G("SetRectRgn", c_ok1, 5), G("CombineRgn", c_ok1, 4), G("CreateICA", c_ok0, 4), G("GetTextAlign", c_ok0, 1), G("GetNearestColor", c_ok0, 2),
    G("StartDocA", c_ok0, 2), G("StartPage", c_ok0, 1), G("EndPage", c_ok0, 1), G("EndDoc", c_ok0, 1), G("AbortDoc", c_ok0, 1), G("SetAbortProc", c_ok0, 2),
    G("PlayMetaFile", c_ok1, 2), G("GetCurrentPositionEx", c_ok1, 2), G("GetWindowOrgEx", c_ok1, 2), G("TabbedTextOutA", c_ok0, 8),
    U("GetScrollRange", c_ok1, 4), U("ScrollWindow", c_ok1, 5), U("BeginDeferWindowPos", c_BeginDeferWindowPos, 1), U("DeferWindowPos", c_arg0, 8), U("EndDeferWindowPos", c_ok1, 1),
    U("ShowOwnedPopups", c_ok1, 2), U("GrayStringA", c_ok1, 9), U("DefMDIChildProcA", c_ok0, 4), U("DefFrameProcA", c_ok0, 5), U("TranslateMDISysAccel", c_ok0, 2),
    U("ReuseDDElParam", c_ok0, 5), U("UnpackDDElParam", c_ok1, 4), U("GetMenuStringA", c_ok0, 5), U("GetDCEx", c_ok0, 3), U("GetMenuCheckMarkDimensions", c_ok0, 0),
    U("SetMenuItemBitmaps", c_ok1, 5), U("LockWindowUpdate", c_ok1, 1), U("SetParent", c_ok0, 2), U("GetScrollInfo", c_ok0, 3), U("CharPrevA", c_CharPrevA, 2),
    U("SetPropA", c_ok1, 3), U("GetPropA", c_ok0, 2), U("RemovePropA", c_ok0, 2), U("SendNotifyMessageA", c_ok1, 4), U("GetWindowWord", c_ok0, 2), U("SetWindowWord", c_ok0, 3),
    U("FindWindowExA", c_ok0, 4), U("DrawTextExA", c_ok0, 6), U("DialogBoxIndirectParamA", c_ok0, 5),
    K("GetShortPathNameA", c_GetShortPathNameA, 3), K("FatalAppExitA", c_FatalAppExitA, 2), K("FatalExit", c_FatalAppExitA, 1), K("GlobalFlags", c_ok0, 1),
    K("GlobalAddAtomA", c_atom_add, 1), K("GlobalDeleteAtom", c_ok0, 1), K("GlobalGetAtomNameA", c_ok0, 3), K("GetProcessVersion", c_GetProcessVersion, 1),
    K("GetStringTypeExA", c_ok1, 5), K("LockFile", c_ok1, 5), K("UnlockFile", c_ok1, 5), K("GetProcessAffinityMask", c_ok0, 3), K("IsProcessorFeaturePresent", c_ok0, 1),
    ADV("RegQueryValueA", c_RegQueryValueA, 4), ADV("RegSetValueA", c_ok0, 5), ADV("SetFileSecurityA", c_ok1, 3), ADV("GetFileSecurityA", c_ok0, 5),
    { "shell32", "ShellExecuteA", 6, c_ShellExecuteA }, { "shell32", "DragFinish", 1, c_ok0 }, { "shell32", "DragQueryFileA", 4, c_ok0 }, { "shell32", "DragAcceptFiles", 2, c_ok0 },
    { "shell32", "SHGetFileInfoA", 5, c_ok0 },
    { "comctl32", "ImageList_Create", 5, c_ok1 }, { "comctl32", "ImageList_Add", 3, c_ok0 }, { "comctl32", "ImageList_Destroy", 1, c_ok1 },
    { "imm32", "ImmGetContext", 1, c_ok0 }, { "imm32", "ImmReleaseContext", 2, c_ok1 }, { "imm32", "ImmSetOpenStatus", 2, c_ok1 }, { "imm32", "ImmGetOpenStatus", 1, c_ok0 },
    { "imm32", "ImmAssociateContext", 2, c_ok0 }, { "imm32", "ImmNotifyIME", 4, c_ok1 }, { "imm32", "ImmGetDefaultIMEWnd", 1, c_ok0 },
    { "version", "GetFileVersionInfoSizeA", 2, c_FileVersionNone }, { "version", "GetFileVersionInfoA", 4, c_FileVersionNone }, { "version", "VerQueryValueA", 4, c_FileVersionNone },
    { "msacm32", "acmMetrics", 3, c_acmMetrics }, { "msacm32", "acmStreamOpen", 8, c_acmNoDriver }, { "msacm32", "acmStreamClose", 2, c_ok0 }, { "msacm32", "acmStreamSize", 4, c_acmNoDriver },
    { "msacm32", "acmStreamPrepareHeader", 3, c_acmNoDriver }, { "msacm32", "acmStreamUnprepareHeader", 3, c_ok0 }, { "msacm32", "acmStreamConvert", 3, c_acmNoDriver },
    /* ole32 */
    { "ole32", "CoInitialize", 1, c_ok0 }, { "ole32", "CoUninitialize", 0, c_ok0 }, { "ole32", "CoCreateInstance", 5, c_CoCreateInstance },
    { NULL, NULL, 0, NULL }
};
