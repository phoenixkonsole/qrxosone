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
#ifdef __AROS__
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#endif

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
/* The NT security calls, as Windows 95 has them: they fail with ERROR_CALL_NOT_IMPLEMENTED, and
   setup programs take that as "no NT, no rights to check". */
FN(c_nosec) { (void)a; w->last_error = 120; return 0; }
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
    if (t >= 0) return qw_thread_finished(t);
    t = qw_proc_signalled(w, h);
    if (t >= 0) return t;
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
    if (qw_proc_close(a[0])) return 1;
    return qw_file_close(w, a[0]);
}
/* ------------------------------------------------------------------ child processes
 * A child is another qrx-win32 task with its own x86 memory and window. What it must know
 * goes through a file in T:, because quotes in a command line do not survive the shell; it
 * reports its exit code in a second file, the file name plus ".x". */
#define QP_MAX 32
#define QP_BASE 0x7E300000u
#define QP_THREADS 0x100u
typedef struct qproc { int used, started, done, open; uint32_t code; char file[64]; uint64_t next_poll; } qproc;
static qproc g_p[QP_MAX];
static qproc *proc_of(uint32_t h)
{
    uint32_t i = h - QP_BASE;
    if (i >= QP_THREADS) i -= QP_THREADS;
    return i < QP_MAX && g_p[i].used ? &g_p[i] : NULL;
}
static void proc_poll(qwin *w, qproc *p)
{
    uint64_t now;
    FILE *f;
    if (p->done || !p->started) return;
    now = w->be->now(w);
    if (now < p->next_poll) return;
    p->next_poll = now + 20;
    {
        char x[80];
        snprintf(x, sizeof(x), "%s.x", p->file);
        f = fopen(x, "r");
    }
    if (f) {
        unsigned code;
        if (fscanf(f, "%u\n", &code) == 1) { p->code = code; p->done = 1; }
        fclose(f);
    }
}
int qw_proc_signalled(qwin *w, uint32_t h)
{
    qproc *p = proc_of(h);
    if (!p) return -1;
    proc_poll(w, p);
    return p->done;
}
int qw_proc_close(uint32_t h)
{
    qproc *p = proc_of(h);
    if (!p) return 0;
    if (--p->open <= 0) {
        if (p->done) { char x[80]; snprintf(x, sizeof(x), "%s.x", p->file); remove(x); }
        p->used = 0;
    }
    return 1;
}
static int proc_start(qwin *w, qproc *p)
{
#ifdef __AROS__
    char cmd[120];
    BPTR in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE), out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    LONG r;
    (void)w;
    snprintf(cmd, sizeof(cmd), "C:qrx-win32 -P \"%s\"", p->file);
    r = SystemTags((CONST_STRPTR)cmd, SYS_Input, (IPTR)in, SYS_Output, (IPTR)out, SYS_Asynch, TRUE, TAG_END);
    if (r == -1) { Close(in); Close(out); return 0; }
    p->started = 1;
    return 1;
#else
    (void)w; (void)p;
    return 0;
#endif
}
int qw_proc_resume(qwin *w, uint32_t h)
{
    qproc *p = proc_of(h);
    if (!p) return 0;
    if (!p->started && !proc_start(w, p)) { p->done = 1; p->code = 1; }
    return 1;
}
/* 0 no such file, 1 a 32-bit Windows program, 2 anything else */
static int exe_kind(const char *host)
{
    uint8_t b[64], pe[4];
    FILE *f = fopen(host, "rb");
    int k = 2;
    if (!f) return 0;
    if (fread(b, 1, 64, f) == 64 && b[0] == 'M' && b[1] == 'Z') {
        long off = (long)b[60] | (long)b[61] << 8 | (long)b[62] << 16 | (long)b[63] << 24;
        if (off > 0 && !fseek(f, off, SEEK_SET) && fread(pe, 1, 4, f) == 4 && !memcmp(pe, "PE\0\0", 4)) k = 1;
    }
    fclose(f);
    return k;
}
static int try_exe(qwin *w, const char *dir, const char *name, char *host, size_t hmax, char *win, size_t wmax)
{
    const char *base = strrchr(name, '\\');
    int ext = strchr(base ? base : name, '.') != NULL, round;
    size_t L = strlen(dir);
    for (round = ext ? 1 : 0; round < 2; round++) {
        int k;
        if (L) snprintf(win, wmax, "%s%s%s%s", dir, dir[L - 1] == '\\' ? "" : "\\", name, round ? "" : ".exe");
        else snprintf(win, wmax, "%s%s", name, round ? "" : ".exe");
        qw_fs_map(w, win, host, hmax);
        k = exe_kind(host);
        if (k == 2) {                            /* a 16-bit starter for a 32-bit program? */
            extern int qw_ne_redirect(qwin *w, const char *path, char *out, size_t max);
            char alt[1200];
            if (qw_ne_redirect(w, host, alt, sizeof(alt))) {
                const char *ab = strrchr(alt, '/') ? strrchr(alt, '/') + 1 : alt;
                char *wb = strrchr(win, '\\');
                snprintf(host, hmax, "%s", alt);
                if (wb && (size_t)(wb + 1 - win) + strlen(ab) < wmax) strcpy(wb + 1, ab);
                k = 1;
            }
        }
        if (k) return k;
    }
    return 0;
}
static int find_exe(qwin *w, const char *name, char *host, size_t hmax, char *win, size_t wmax)
{
    int k;
    char dir[600];
    uint32_t n;
    if (strchr(name, ':') || name[0] == '\\' || name[0] == '/') return try_exe(w, "", name, host, hmax, win, wmax);
    qw_fs_cwd_win(w, dir, sizeof(dir));
    if ((k = try_exe(w, dir, name, host, hmax, win, wmax))) return k;
    if (w->exe_win[0]) {
        snprintf(dir, sizeof(dir), "%s", w->exe_win);
        n = (uint32_t)(strrchr(dir, '\\') ? strrchr(dir, '\\') - dir : 0);
        dir[n] = 0;
    } else snprintf(dir, sizeof(dir), "C:\\");
    if ((k = try_exe(w, dir, name, host, hmax, win, wmax))) return k;
    if ((k = try_exe(w, "C:\\WINDOWS\\SYSTEM", name, host, hmax, win, wmax))) return k;
    return try_exe(w, "C:\\WINDOWS", name, host, hmax, win, wmax);
}
/* The program a command line names: an unquoted one may contain spaces, as on Windows. */
static int find_cmd(qwin *w, const char *cmd, char *host, size_t hmax, char *win, size_t wmax)
{
    char name[600];
    size_t i = 0;
    int k;
    while (*cmd == ' ') cmd++;
    if (*cmd == '"') {
        for (cmd++; cmd[i] && cmd[i] != '"' && i + 1 < sizeof(name); i++) name[i] = cmd[i];
        name[i] = 0;
        return find_exe(w, name, host, hmax, win, wmax);
    }
    for (;; i++) {
        if (i + 1 >= sizeof(name)) return 0;
        if (!cmd[i] || cmd[i] == ' ') {
            name[i] = 0;
            if (i && (k = find_exe(w, name, host, hmax, win, wmax))) return k;
            if (!cmd[i]) return 0;
        }
        name[i] = cmd[i];
    }
}
/* Returns the slot, or -1 with last_error set. */
static int proc_create(qwin *w, const char *app, const char *cmd, const char *cwd, int suspended, int report)
{
    char host[1200], win[600], root[1024], cur[512], rel[1024], line[1100];
    static int seq;
    int i, kind;
    qproc *p;
    FILE *f;
    kind = app && app[0] ? find_exe(w, app, host, sizeof(host), win, sizeof(win)) : cmd ? find_cmd(w, cmd, host, sizeof(host), win, sizeof(win)) : 0;
    if (kind != 1) {
        qw_log(w, "CreateProcess(%s, %s): %s", app ? app : "", cmd ? cmd : "", kind ? "not a 32-bit Windows program" : "not found");
        w->last_error = kind ? 193 : 2;
        return -1;
    }
    for (i = 0; i < QP_MAX; i++) if (!g_p[i].used) break;
    if (i >= QP_MAX) { w->last_error = 8; return -1; }
    p = &g_p[i];
    memset(p, 0, sizeof(*p));
    if (cmd && cmd[0]) snprintf(line, sizeof(line), "%s", cmd);
    else snprintf(line, sizeof(line), "\"%s\"", win);
    qw_fs_state(w, root, sizeof(root), cur, sizeof(cur));
    if (cwd && cwd[0]) { qw_fs_rel(w, cwd, rel, sizeof(rel)); snprintf(cur, sizeof(cur), "%s", rel); }
#ifdef __AROS__
    snprintf(p->file, sizeof(p->file), "T:qrxp.%lx.%d", (unsigned long)(uintptr_t)FindTask(NULL), ++seq);
#else
    snprintf(p->file, sizeof(p->file), "qrxp.%d", ++seq);
#endif
    f = fopen(p->file, "w");
    if (!f) { w->last_error = 5; return -1; }
    fprintf(f, "%s\n%s\n%s\n%s\n%s\n", host, win, root, cur, line);
    if (w->log_path[0]) fprintf(f, "%s.%d\n", w->log_path, seq); else fprintf(f, "\n");
    fprintf(f, "%d\n%d\n%d\n", w->windowed, w->verbose, report);
    fclose(f);
    p->used = 1;
    p->open = 2;
    if (!suspended && !proc_start(w, p)) { remove(p->file); p->used = 0; w->last_error = 8; return -1; }
    qw_log(w, "started %s: %s", win, line);
    return i;
}
static uint32_t create_process(qwin *w, uint32_t *a, const char *app, const char *cmd, const char *cwd)
{
    int i = proc_create(w, app, cmd, cwd, (a[5] & 4u) != 0, 1);    /* CREATE_SUSPENDED */
    if (i < 0) return 0;
    if (a[9]) {
        WR32(a[9], QP_BASE + (uint32_t)i);
        WR32(a[9] + 4, QP_BASE + QP_THREADS + (uint32_t)i);
        WR32(a[9] + 8, 0x1000u + (uint32_t)i * 4u);
        WR32(a[9] + 12, 0x1004u + (uint32_t)i * 4u);
    }
    return 1;
}
FN(c_CreateProcessA) {
    char app[600], cmd[1024], cwd[600];
    if (a[0]) rd_str(w, a[0], app, sizeof(app));
    if (a[1]) rd_str(w, a[1], cmd, sizeof(cmd));
    if (a[7]) rd_str(w, a[7], cwd, sizeof(cwd));
    return create_process(w, a, a[0] ? app : NULL, a[1] ? cmd : NULL, a[7] ? cwd : NULL);
}
FN(c_CreateProcessW) {
    char app[600], cmd[1024], cwd[600];
    wchar16 t[1024];
    if (a[0]) { qw_rd_wstr(w, a[0], t, 1024); w2a(app, t, sizeof(app)); }
    if (a[1]) { qw_rd_wstr(w, a[1], t, 1024); w2a(cmd, t, sizeof(cmd)); }
    if (a[7]) { qw_rd_wstr(w, a[7], t, 1024); w2a(cwd, t, sizeof(cwd)); }
    return create_process(w, a, a[0] ? app : NULL, a[1] ? cmd : NULL, a[7] ? cwd : NULL);
}
FN(c_WinExec) {
    char cmd[1024];
    int i;
    rd_str(w, a[0], cmd, sizeof(cmd));
    i = proc_create(w, NULL, cmd, NULL, 0, 0);
    if (i < 0) return w->last_error == 2 ? 2 : 11;                 /* file not found, bad format */
    g_p[i].used = 0;                                               /* no handle: nobody can ask */
    return 33;
}
FN(c_ShellExecuteA) {                    /* (hwnd, verb, file, params, dir, show) */
    char file[600], params[400], dir[600], cmd[1024], host[1200], win[600];
    int i, k;
    rd_str(w, a[2], file, sizeof(file));
    params[0] = dir[0] = 0;
    if (a[3]) rd_str(w, a[3], params, sizeof(params));
    if (a[4]) rd_str(w, a[4], dir, sizeof(dir));
    k = find_exe(w, file, host, sizeof(host), win, sizeof(win));
    if (!k) return 2;                                              /* file not found */
    if (k != 1) return 31;                                         /* documents have no application here */
    snprintf(cmd, sizeof(cmd), "\"%s\"%s%s", win, params[0] ? " " : "", params);
    i = proc_create(w, NULL, cmd, dir[0] ? dir : NULL, 0, 0);
    if (i < 0) return 2;
    g_p[i].used = 0;
    return 33;
}
FN(c_GetExitCodeProcess) {
    qproc *p = proc_of(a[0]);
    uint32_t code = 259;                                           /* STILL_ACTIVE */
    if (p) { proc_poll(w, p); if (p->done) code = p->code; }
    if (a[1]) WR32(a[1], code);
    return 1;
}
FN(c_TerminateProcess) {
    qproc *p = proc_of(a[0]);
    if (p) {
        /* The task cannot be stopped from here; the program that asked goes on as if it had. */
        qw_log(w, "TerminateProcess on a child: it keeps running");
        p->done = 1;
        p->code = a[1];
        return 1;
    }
    w->exit_code = a[1];
    w->exiting = 1;
    return 1;
}
/* (locale, flags, value, format, out, size) */
FN(c_GetNumberFormatA) {
    char v[64], res[128], dec[8] = ".", sep[8] = ",";
    int digits = 2, group = 3, o = 0, i, L, intlen, have;
    const char *dot;
    rd_str(w, a[2], v, sizeof(v));
    if (a[3]) {
        digits = (int)RD32(a[3]); group = (int)RD32(a[3] + 8);
        rd_str(w, RD32(a[3] + 12), dec, sizeof(dec)); rd_str(w, RD32(a[3] + 16), sep, sizeof(sep));
    }
    i = v[0] == '-' ? 1 : 0;
    if (i) res[o++] = '-';
    dot = strchr(v, '.');
    L = (int)strlen(v);
    intlen = (dot ? (int)(dot - v) : L) - i;
    for (have = 0; have < intlen && o < 100; have++) {
        res[o++] = v[i + have];
        if (group > 0 && have + 1 < intlen && (intlen - have - 1) % group == 0) { int k; for (k = 0; sep[k]; k++) res[o++] = sep[k]; }
    }
    if (digits > 0) {
        const char *f = dot ? dot + 1 : "";
        int k;
        for (k = 0; dec[k]; k++) res[o++] = dec[k];
        for (k = 0; k < digits && o < 120; k++) res[o++] = *f ? *f++ : '0';
    }
    res[o] = 0;
    if (!a[5]) return (uint32_t)o + 1;
    if ((uint32_t)o + 1 > a[5]) { w->last_error = 122; return 0; }
    return wr_str(w, a[4], a[5], res) + 1;
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
/* One registry for all programs, as on Windows: an installer's entries must reach the
   game it installs. Older versions kept a file per program folder; it is merged once. */
#define REG_FILE_OLD "QRXREG.TXT"
#ifdef __AROS__
#define REG_FILE "SYS:QRXREG.TXT"
#else
#define REG_FILE REG_FILE_OLD
#endif
#define REG_KEYS 512
typedef struct qval { char key[160], name[64]; uint32_t type, size; uint8_t *data; struct qval *next; } qval;
static qval *g_vals;
static struct { int used; char path[160]; } g_hk[64];
static char g_known[REG_KEYS][160];
static int g_loaded;

static void key_lower(char *s) { for (; *s; s++) *s = *s == '/' ? '\\' : (char)tolower((unsigned char)*s); }
static void key_known(const char *p)
{
    int i;
    for (i = 0; i < REG_KEYS; i++) {
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
static void reg_read(const char *file)
{
    FILE *f = fopen(file, "r");
    char line[1024];
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
        if (t[0] == 'k') key_known(k);                     /* a key without values */
        else val_set(k, n, (uint32_t)strtoul(t, NULL, 10), buf, size);
    }
    fclose(f);
}
static int reg_write(const char *file)
{
    FILE *f = fopen(file, "w");
    qval *v;
    int i;
    if (!f) return 0;
    for (i = 0; i < REG_KEYS && g_known[i][0]; i++) fprintf(f, "%s||k|\n", g_known[i]);
    for (v = g_vals; v; v = v->next) {
        uint32_t j;
        if (v->size > 256) continue;
        fprintf(f, "%s|%s|%u|", v->key, v->name, (unsigned)v->type);
        for (j = 0; j < v->size; j++) fprintf(f, "%02x", v->data[j]);
        fputc('\n', f);
    }
    fclose(f);
    return 1;
}
static void reg_save(void)
{
    if (!reg_write(REG_FILE)) reg_write(REG_FILE_OLD);     /* a read-only system disk */
}
static void reg_load(void)
{
    FILE *old;
    if (g_loaded) return;
    g_loaded = 1;
    reg_read(REG_FILE);
    {   /* Programs read the DirectX version from here. */
        static const char ver[] = "4.09.00.0904";
        static const uint8_t inst[] = { 0, 0, 0, 4, 0, 9, 0, 0 };  
        val_set("hklm\\software\\microsoft\\directx", "Version", 1, (const uint8_t *)ver, sizeof(ver));
        val_set("hklm\\software\\microsoft\\directx", "InstalledVersion", 3, inst, sizeof(inst));
    }
    {   /* What every Windows has there: installers (InstallShield) build their default folder
         * from ProgramFilesDir and show the owner on their user information page. Only where
         * the registry file has no value of its own. */
        static const char *const cv[][2] = {
            { "ProgramFilesDir", "C:\\Program Files" }, { "CommonFilesDir", "C:\\Program Files\\Common Files" },
            { "RegisteredOwner", "QRX User" }, { "RegisteredOrganization", "" },
        };
        const char *k = "hklm\\software\\microsoft\\windows\\currentversion";
        size_t i;
        for (i = 0; i < sizeof(cv) / sizeof(cv[0]); i++)
            if (!val_find(k, cv[i][0])) val_set(k, cv[i][0], 1, (const uint8_t *)cv[i][1], (uint32_t)strlen(cv[i][1]) + 1u);
    }
    if (strcmp(REG_FILE, REG_FILE_OLD) && (old = fopen(REG_FILE_OLD, "r")) != NULL) {
        fclose(old);
        reg_read(REG_FILE_OLD);
        if (reg_write(REG_FILE)) remove(REG_FILE_OLD);
    }
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
    for (i = 0; i < REG_KEYS && g_known[i][0]; i++) {
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
    if (!v) { if (w->verbose) qw_log(w, "RegQueryValueEx(%s, %s): not found", p, n); return 2; }
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
/* The index-th direct child of key p, also when only a deeper key was ever created. */
static int key_child(const char *p, uint32_t index, char *out, size_t max)
{
    size_t L = strlen(p);
    uint32_t n = 0;
    int i, j;
    for (i = 0; i < REG_KEYS && g_known[i][0]; i++) {
        const char *c = g_known[i] + L + 1, *e;
        size_t cl;
        int seen = 0;
        if (strncmp(g_known[i], p, L) || g_known[i][L] != '\\') continue;
        e = strchr(c, '\\');
        cl = e ? (size_t)(e - c) : strlen(c);
        for (j = 0; j < i && !seen; j++)
            if (!strncmp(g_known[j], p, L) && g_known[j][L] == '\\' && !strncmp(g_known[j] + L + 1, c, cl)
                && (g_known[j][L + 1 + cl] == 0 || g_known[j][L + 1 + cl] == '\\')) seen = 1;
        if (seen) continue;
        if (n++ == index) {
            if (cl >= max) cl = max - 1;
            memcpy(out, c, cl); out[cl] = 0;
            return 1;
        }
    }
    return 0;
}
FN(c_RegEnumKeyExA) {
    char p[160], c[160];
    uint32_t cap = RD32(a[3]), L, i;
    reg_load();
    reg_path(w, a[0], 0, p, sizeof(p));
    if (!key_child(p, a[1], c, sizeof(c))) return 259;     /* ERROR_NO_MORE_ITEMS */
    L = (uint32_t)strlen(c);
    if (L + 1 > cap) return 234;                           /* ERROR_MORE_DATA */
    wr_str(w, a[2], cap, c);
    WR32(a[3], L);
    if (a[6]) WR32(a[6], 0);
    if (a[7]) for (i = 0; i < 8; i++) WR8(a[7] + i, 0);
    return 0;
}
FN(c_RegEnumKeyA) {
    char p[160], c[160];
    reg_load();
    reg_path(w, a[0], 0, p, sizeof(p));
    if (!key_child(p, a[1], c, sizeof(c))) return 259;
    if (strlen(c) + 1 > a[3]) return 234;
    wr_str(w, a[2], a[3], c);
    return 0;
}
/* Removes the whole subtree, as Windows 95 does. */
static int key_within(const char *k, const char *p, size_t L) { return !strncmp(k, p, L) && (k[L] == 0 || k[L] == '\\'); }
FN(c_RegDeleteKeyA) {
    char p[160];
    qval **pv, *v;
    int i, n = 0;
    size_t L;
    reg_load();
    reg_path(w, a[0], a[1], p, sizeof(p));
    if (!key_exists(p)) return 2;
    L = strlen(p);
    for (pv = &g_vals; (v = *pv) != NULL; ) {
        if (key_within(v->key, p, L)) { *pv = v->next; free(v->data); free(v); }
        else pv = &v->next;
    }
    for (i = 0; i < REG_KEYS && g_known[i][0]; i++) {
        if (key_within(g_known[i], p, L)) continue;
        if (n != i) memcpy(g_known[n], g_known[i], sizeof(g_known[n]));
        n++;
    }
    if (n < REG_KEYS) g_known[n][0] = 0;
    reg_save();
    return 0;
}
FN(c_RegQueryInfoKeyA) {
    char p[160], c[160];
    uint32_t i, subs = 0, maxsub = 0, vals = 0, maxname = 0, maxdata = 0;
    qval *v;
    reg_load();
    reg_path(w, a[0], 0, p, sizeof(p));
    while (key_child(p, subs, c, sizeof(c))) { if (strlen(c) > maxsub) maxsub = (uint32_t)strlen(c); subs++; }
    for (v = g_vals; v; v = v->next) if (!strcmp(v->key, p)) {
        vals++;
        if (strlen(v->name) > maxname) maxname = (uint32_t)strlen(v->name);
        if (v->size > maxdata) maxdata = v->size;
    }
    if (a[2]) WR32(a[2], 0);
    for (i = 4; i < 11; i++) if (a[i]) WR32(a[i], 0);
    if (a[4]) WR32(a[4], subs);
    if (a[5]) WR32(a[5], maxsub);
    if (a[7]) WR32(a[7], vals);
    if (a[8]) WR32(a[8], maxname);
    if (a[9]) WR32(a[9], maxdata);
    return 0;
}

/* ================================================================ INI files */
static void ini_path(qwin *w, uint32_t path, char *out, size_t max)
{
    char p[300], *s;
    rd_str(w, path, p, sizeof(p));
    /* A full or relative Windows path goes where CreateFile puts it (C:\ = the program's
     * drive, C:\WINDOWS shared): InstallShield writes C:\_ISTMP0.DIR\~reg.tmp. */
    if (p[0] && (strchr(p, '\\') || (isalpha((unsigned char)p[0]) && p[1] == ':'))) { qw_fs_map(w, p, out, max); return; }
    for (s = p; *s; s++) if (*s == '\\') *s = '/';
    s = p;
    if (isalpha((unsigned char)s[0]) && s[1] == ':') s += 2;
    while (*s == '/') s++;
#ifdef __AROS__
    if (!strncasecmp(s, "windows/", 8)) { snprintf(out, max, "SYS:%s", s); return; }   /* one C:\WINDOWS for all programs */
#endif
    snprintf(out, max, "%s", s);
}
static FILE *ini_open(qwin *w, uint32_t path)
{
    char s[310];
    FILE *f;
    ini_path(w, path, s, sizeof(s));
    if (!s[0]) return NULL;                                 /* "" is no file (AROS: the current directory) */
    /* Windows paths from a game: a plain name lives in \WINDOWS, here beside the game. */
    f = fopen(s, "r");
    if (!f) { char *b = strrchr(s, '/'); if (b) f = fopen(b + 1, "r"); }
#ifdef __AROS__
    if (!f) {
        char g[330], *b = strrchr(s, '/');
        snprintf(g, sizeof(g), "SYS:WINDOWS/%s", b ? b + 1 : s);
        f = fopen(g, "r");
    }
#endif
    return f;
}
/* Rewrites file: key = val in section sec. val NULL removes the key, key NULL the section. */
static int ini_update(const char *file, const char *sec, const char *key, const char *val)
{
    FILE *f = fopen(file, "r");
    char *old = NULL, *neu, *p;
    long n = 0;
    size_t k = 0, room;
    int in = 0, seen = 0, done = 0;
    if (f) {
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n < 0) n = 0;
        old = (char *)malloc((size_t)n + 1);
        if (!old) { fclose(f); return 0; }
        n = (long)fread(old, 1, (size_t)n, f);
        old[n] = 0;
        fclose(f);
    }
    room = (size_t)n + strlen(sec) + (key ? strlen(key) : 0) + (val ? strlen(val) : 0) + 16;
    neu = (char *)malloc(room);
    if (!neu) { free(old); return 0; }
    for (p = old; p && *p; ) {
        char *e = strchr(p, '\n'), *s = p, *t;
        size_t L = e ? (size_t)(e - p) + 1 : strlen(p);
        int drop = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[' && (t = strchr(s, ']')) != NULL && t < p + L) {
            if (in && !done && key && val) { k += (size_t)sprintf(neu + k, "%s=%s\r\n", key, val); done = 1; }
            in = (size_t)(t - s - 1) == strlen(sec) && !strncasecmp(s + 1, sec, strlen(sec));
            if (in) seen = 1;
            if (in && !key) drop = 1;
        } else if (in && !key) drop = 1;
        else if (in && !done && *s != ';' && (t = strchr(s, '=')) != NULL && t < p + L) {
            char *q = t;
            while (q > s && (q[-1] == ' ' || q[-1] == '\t')) q--;
            if ((size_t)(q - s) == strlen(key) && !strncasecmp(s, key, strlen(key))) {
                drop = 1; done = 1;
                if (val) k += (size_t)sprintf(neu + k, "%s=%s\r\n", key, val);
            }
        }
        if (!drop) { memcpy(neu + k, p, L); k += L; }
        p += L;
    }
    if (!done && key && val) {
        if (!seen) k += (size_t)sprintf(neu + k, "[%s]\r\n", sec);
        k += (size_t)sprintf(neu + k, "%s=%s\r\n", key, val);
    }
    f = fopen(file, "w");
    if (f) { fwrite(neu, 1, k, f); fclose(f); }
    free(old); free(neu);
    return f != NULL;
}
FN(c_WritePrivateProfileStringA) {
    char file[310], sec[128], key[128], val[512];
    if (!a[3] || !a[0]) return 1;                           /* flush (no file, or no section) */
    ini_path(w, a[3], file, sizeof(file));
    /* An empty name is no file (on AROS "" is the current directory, and opening that to
     * write blocks): fail as Windows does. */
    if (!file[0]) { w->last_error = 2; return 0; }
    rd_str(w, a[0], sec, sizeof(sec));
    rd_str(w, a[1], key, sizeof(key));
    rd_str(w, a[2], val, sizeof(val));
    if (w->verbose) { char raw[300]; rd_str(w, a[3], raw, sizeof(raw)); qw_log(w, "WritePrivateProfileString(%s -> %s) [%s] %s", raw, file, sec, a[1] ? key : "(section)"); }
    return (uint32_t)ini_update(file, sec, a[1] ? key : NULL, a[2] ? val : NULL);
}
FN(c_GetPrivateProfileSectionA) {
    char sec[128], line[512];
    FILE *f = ini_open(w, a[3]);
    uint32_t k = 0, cap = a[2];
    int in = 0;
    rd_str(w, a[0], sec, sizeof(sec));
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *s = line, *e;
            uint32_t L, i;
            while (*s == ' ' || *s == '\t') s++;
            for (e = s + strlen(s); e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
            if (*s == '[') { e = strchr(s, ']'); if (e) *e = 0; in = !strcasecmp(s + 1, sec); continue; }
            if (!in || !*s || *s == ';') continue;
            L = (uint32_t)strlen(s);
            if (k + L + 2 > cap) break;
            for (i = 0; i <= L; i++) WR8(a[1] + k + i, (uint8_t)s[i]);
            k += L + 1;
        }
        fclose(f);
    }
    if (cap) WR8(a[1] + k, 0);
    return k;
}
FN(c_WritePrivateProfileSectionA) {
    char file[310], sec[128], line[512];
    uint32_t p = a[1];
    ini_path(w, a[2], file, sizeof(file));
    if (!file[0]) { w->last_error = 2; return 0; }
    rd_str(w, a[0], sec, sizeof(sec));
    ini_update(file, sec, NULL, NULL);
    for (; p && RD8(p); ) {
        char *e;
        uint32_t L;
        rd_str(w, p, line, sizeof(line));
        L = (uint32_t)strlen(line);
        p += L + 1;
        e = strchr(line, '=');
        if (!e) continue;
        *e++ = 0;
        ini_update(file, sec, line, e);
    }
    return 1;
}
/* DirectX is built in: the setup a game ships finds it installed. */
FN(c_DirectXSetupGetVersion) { if (a[0]) WR32(a[0], 0x00040009u); if (a[1]) WR32(a[1], 904u); return 1; }
FN(c_VerLanguageNameA) { return wr_str(w, a[1], a[2], "English (United States)"); }
FN(c_efail) { (void)w; (void)a; return 0x80004005u; }
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
    WR32(p + 8, 256u << 20); WR32(p + 12, 192u << 20);
    /* Demos of the time refuse to start below some amount of free page file. */
    WR32(p + 16, 1024u << 20); WR32(p + 20, 900u << 20);
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
        g_map[i].view = p;                      /* qw_alloc has zeroed it */
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
FN(c_FormatDT) { uint32_t cap = a[5]; return cap ? wr_str(w, a[4], cap, "1998-01-01") + 1 : 11; }
FN(c_CharUpperBuffA) { uint32_t i; for (i = 0; i < a[1]; i++) WR8(a[0] + i, (uint8_t)toupper(RD8(a[0] + i))); return a[1]; }
FN(c_CharLowerBuffA) { uint32_t i; for (i = 0; i < a[1]; i++) WR8(a[0] + i, (uint8_t)tolower(RD8(a[0] + i))); return a[1]; }
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
static void wo_start(qwin *w, uint32_t i)
{
    qwo *o = &g_wo[i];
    o->playing = 1;
    o->clock0 = w->be->now(w) - (o->played * 1000u) / (o->rate * o->ba);
    o->lastpos = (uint32_t)(o->played & (WO_RING - 1u));
    qw_ds_voice(w, DSV_SET, WO_VOICE_BASE + i, o->ring, WO_RING, o->rate, o->ch, o->bits, 1, o->lastpos + 1u, NULL, NULL);
    qw_ds_voice(w, DSV_START, WO_VOICE_BASE + i, o->ring, WO_RING, o->rate, o->ch, o->bits, 1, 0, NULL, NULL);
}
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
    /* Guest pages do not lie next to each other in our memory: never copy past one. */
    for (n = 0; n < len; ) {
        uint8_t chunk[1024];
        uint32_t k = len - n, room = WO_RING - wp;
        if (k > sizeof(chunk)) k = (uint32_t)sizeof(chunk);
        if (k > room) k = room;
        x86_read_mem(C, data + n, chunk, k);
        x86_write_mem(C, o->ring + wp, chunk, k);
        n += k;
        wp = (wp + k) & (WO_RING - 1u);
    }
    o->written += len;
    WR32(hdr + 16, (RD32(hdr + 16) | 0x10u) & ~1u);
    o->q[o->qn] = hdr;
    o->qend[o->qn] = o->written;
    o->qn++;
    if (!o->playing && !o->paused) wo_start(w, i);
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
/* What was written during the pause starts now: Freddi Fish tests the sound card this way. */
FN(c_waveOutRestart) {
    uint32_t i = a[0] - WO_HANDLE;
    if (i < 2) {
        g_wo[i].paused = 0;
        if (g_wo[i].open && !g_wo[i].playing && g_wo[i].qn) wo_start(w, i);
    }
    return 0;
}
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

/* (hdc, family, callback, lparam): the one font there is, under the name asked for. EnumFontsA
   takes the same arguments and calls back the same way. */
FN(c_EnumFontFamiliesA) {
    uint32_t lf = qw_alloc(w, 60 + 32), tm = qw_alloc(w, 80), i, r;
    char face[32];
    if (!lf || !tm) return 0;
    for (i = 0; i < 92u; i += 4u) WR32(lf + i, 0);
    for (i = 0; i < 80u; i += 4u) WR32(tm + i, 0);
    snprintf(face, sizeof(face), "Arial");
    if (a[1]) rd_str(w, a[1], face, sizeof(face));
    WR32(lf, 16); WR32(lf + 16, 400);        /* lfHeight, lfWeight */
    for (i = 0; face[i]; i++) x86_wr8(C, lf + 28 + i, (uint8_t)face[i]);
    WR32(tm, 16); WR32(tm + 4, 13); WR32(tm + 8, 3); WR32(tm + 20, 7); WR32(tm + 24, 14); WR32(tm + 28, 400);
    r = qw_call(w, a[2], 4, lf, tm, 4 /* TRUETYPE_FONTTYPE */, a[3]);
    qw_free(w, lf); qw_free(w, tm);
    return r;
}

/* ================================================================ BSTR and VARIANT */
/* A BSTR is UTF-16 with its length in bytes in the dword before it. */
static uint32_t bstr_new(qwin *w, uint32_t from, uint32_t chars)
{
    uint32_t m = qw_alloc(w, chars * 2u + 6u), i;
    if (!m) return 0;
    WR32(m, chars * 2u);
    for (i = 0; i < chars; i++) x86_wr16(C, m + 4u + i * 2u, from ? x86_rd16(C, from + i * 2u) : 0);
    x86_wr16(C, m + 4u + chars * 2u, 0);
    return m + 4u;
}
FN(ole_SysAllocStringLen) { return bstr_new(w, a[0], a[1]); }
FN(ole_SysAllocString) {
    uint32_t n = 0;
    if (!a[0]) return 0;
    while (x86_rd16(C, a[0] + n * 2u)) n++;
    return bstr_new(w, a[0], n);
}
FN(ole_SysFreeString) { if (a[0]) qw_free(w, a[0] - 4u); return 0; }
FN(ole_SysStringLen) { UNUSED; return a[0] ? RD32(a[0] - 4u) / 2u : 0; }
FN(ole_SysReAllocStringLen) {
    uint32_t n = bstr_new(w, a[1], a[2]), old = RD32(a[0]);
    if (!n) return 0;
    if (old) qw_free(w, old - 4u);
    WR32(a[0], n);
    return 1;
}
FN(ole_VariantInit) { UNUSED; WR32(a[0], 0); WR32(a[0] + 4, 0); WR32(a[0] + 8, 0); WR32(a[0] + 12, 0); return 0; }
FN(ole_VariantClear) {
    if ((x86_rd16(C, a[0]) & 0xFFFu) == 8u && RD32(a[0] + 8)) qw_free(w, RD32(a[0] + 8) - 4u);   /* VT_BSTR */
    WR32(a[0], 0);
    return 0;
}
FN(ole_VariantCopy) {
    uint32_t i;
    for (i = 0; i < 16u; i += 4u) WR32(a[0] + i, RD32(a[1] + i));
    if ((x86_rd16(C, a[1]) & 0xFFFu) == 8u && RD32(a[1] + 8)) WR32(a[0] + 8, bstr_new(w, RD32(a[1] + 8), RD32(RD32(a[1] + 8) - 4u) / 2u));
    return 0;
}
FN(ole_VariantChangeType) { UNUSED; return 0x80020005u; }   /* DISP_E_TYPEMISMATCH: no conversions here */

/* ================================================================ system DLLs as files */
/* Our system DLLs have a version too: games refuse to start below the DirectX they need, and
 * The Gungan Frontier reads the answer without checking it. As in a real block, the fixed part
 * follows a header of 0x28 bytes. Other files report the version resource they carry: Inno
 * Setup stops with "Internal error S104" when the SHFOLDR.DLL it unpacked has none. */
#define VER_BLOCK 0x100u
#define VER_FIXED 0x28u
static int version_is_system(qwin *w, uint32_t path)
{
    char p[300];
    const char *b;
    rd_str(w, path, p, sizeof(p));
    b = strrchr(p, '\\');
    /* DDHELP.EXE and such are asked for too: all of the system folder has our version. */
    if (!strncasecmp(p, "C:\\WINDOWS\\SYSTEM", 17)) return 1;
    return qw_is_hle_dll(b ? b + 1 : p);
}
static uint32_t le32at(const uint8_t *d, uint32_t n, uint32_t o) { return o + 4u <= n && o + 4u > o ? (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24 : 0u; }
/* The file offset of an address, from the section that holds it; 0 if none does. */
static uint32_t rva_off(const uint8_t *d, uint32_t n, uint32_t sec, uint32_t nsec, uint32_t r)
{
    uint32_t k;
    for (k = 0; k < nsec; k++) {
        uint32_t s = sec + k * 40u, va = le32at(d, n, s + 12), vs = le32at(d, n, s + 8), rs = le32at(d, n, s + 16);
        if (r >= va && r < va + (vs > rs ? vs : rs)) return le32at(d, n, s + 20) + (r - va);
    }
    return 0;
}
/* The RT_VERSION resource of a PE file (its first name and language), copied to *out.
   Returns its size, 0 when the file has none. */
static uint32_t version_of_file(qwin *w, uint32_t path, uint8_t **out)
{
    char p[300], host[1200];
    FILE *f;
    uint8_t *d = NULL;
    long fl;
    uint32_t n, pe, nsec, opt, rva, sec, dir = 0, lvl, at = 0, size = 0, off = 0;
    *out = NULL;
    rd_str(w, path, p, sizeof(p));
    qw_fs_map(w, p, host, sizeof(host));
    if (!(f = fopen(host, "rb"))) return 0;
    fseek(f, 0, SEEK_END); fl = ftell(f); fseek(f, 0, SEEK_SET);
    if (fl < 0x100 || fl > (64L << 20) || !(d = (uint8_t *)malloc((size_t)fl)) || fread(d, 1, (size_t)fl, f) != (size_t)fl) { fclose(f); free(d); return 0; }
    fclose(f);
    n = (uint32_t)fl;
#define OFF(r) rva_off(d, n, sec, nsec, r)
    pe = le32at(d, n, 60);
    if (d[0] != 'M' || d[1] != 'Z' || pe + 0x80u > n || memcmp(d + pe, "PE\0\0", 4)) goto done;
    nsec = (uint32_t)(d[pe + 6] | d[pe + 7] << 8);
    opt = (uint32_t)(d[pe + 20] | d[pe + 21] << 8);
    sec = pe + 24u + opt;
    if (opt < 0x78u || sec + nsec * 40u > n) goto done;
    rva = le32at(d, n, pe + 24 + 0x70);
    if (!rva || !(off = OFF(rva))) goto done;
    /* Three levels: type 16, then the first name, then the first language's data entry. */
    for (dir = 0, lvl = 0; lvl < 2; lvl++) {
        uint32_t e, cnt = (le32at(d, n, off + dir + 12) & 0xFFFFu) + (le32at(d, n, off + dir + 12) >> 16);
        for (e = 0, at = 0; e < cnt && e < 4096u; e++)
            if (lvl || le32at(d, n, off + dir + 16 + e * 8) == 16u) { at = le32at(d, n, off + dir + 20 + e * 8); break; }
        if (!(at & 0x80000000u)) goto done;
        dir = at & 0x7FFFFFFFu;
    }
    {
        uint32_t cnt = (le32at(d, n, off + dir + 12) & 0xFFFFu) + (le32at(d, n, off + dir + 12) >> 16), data, from;
        if (!cnt) goto done;
        at = le32at(d, n, off + dir + 20);
        if (at & 0x80000000u) goto done;
        data = le32at(d, n, off + at);
        size = le32at(d, n, off + at + 4);
        from = OFF(data);
        if (!size || size > (1u << 20) || !from || from + size > n || !(*out = (uint8_t *)malloc(size))) { size = 0; goto done; }
        memcpy(*out, d + from, size);
    }
#undef OFF
done:
    free(d);
    return size;
}
/* A real block holds UTF-16 text; VerQueryValueA answers in ANSI, which it writes into the
   second half of the buffer, so the size asked for is twice the resource. */
FN(c_GetFileVersionInfoSizeA) {
    uint8_t *v;
    uint32_t k;
    if (a[1]) WR32(a[1], 0);
    if (version_is_system(w, a[0])) return VER_BLOCK;
    k = version_of_file(w, a[0], &v);
    free(v);
    return k ? k * 2u : 0;
}
FN(c_GetFileVersionInfoA) {
    uint32_t i;
    uint8_t *v;
    if (!version_is_system(w, a[0])) {
        uint32_t k = version_of_file(w, a[0], &v);
        if (!k) return 0;
        for (i = 0; i < a[2]; i++) x86_wr8(C, a[3] + i, i < k ? v[i] : 0);
        free(v);
        return 1;
    }
    if (a[2] < VER_BLOCK) return 0;
    for (i = 0; i < VER_BLOCK; i += 4) WR32(a[3] + i, 0);
    WR32(a[3], VER_BLOCK | (0x34u << 16));
    WR32(a[3] + VER_FIXED, 0xFEEF04BDu);
    WR32(a[3] + VER_FIXED + 4, 0x00010000u);
    /* 4.08.00.400, DirectX 8 */
    WR32(a[3] + VER_FIXED + 8, 0x00040008u); WR32(a[3] + VER_FIXED + 12, 0x00000190u);
    WR32(a[3] + VER_FIXED + 16, 0x00040008u); WR32(a[3] + VER_FIXED + 20, 0x00000190u);
    return 1;
}
/* Walks the blocks: wLength, wValueLength, wType, the key in UTF-16, the value, the children,
   each on a 4-byte boundary. "\" is the fixed part; text values come back as ANSI. */
FN(c_VerQueryValueA) {
    char q[200], *part, *next;
    uint32_t blk = a[0], total = x86_rd16(C, a[0]), val, vlen;
    rd_str(w, a[1], q, sizeof(q));
    if (a[2]) WR32(a[2], 0);
    if (a[3]) WR32(a[3], 0);
    for (part = q; *part; part = next) {
        while (*part == '\\') part++;
        if (!*part) break;
        for (next = part; *next && *next != '\\'; next++) ;
        if (*next) *next++ = 0;
        uint32_t len = x86_rd16(C, blk), key = blk + 6, c = key, child, found = 0;
        while (x86_rd16(C, c)) c += 2;
        child = ((c + 2 + 3) & ~3u) + ((x86_rd16(C, blk + 2) * (x86_rd16(C, blk + 4) == 1 ? 2u : 1u) + 3) & ~3u);
        while (child + 6 < blk + len) {
            uint32_t cl = x86_rd16(C, child), k = 0;
            char name[100];
            if (cl < 6) break;
            for (c = child + 6; k + 1 < sizeof(name) && x86_rd16(C, c); c += 2) name[k++] = (char)x86_rd16(C, c);
            name[k] = 0;
            if (!strcasecmp(name, part)) { blk = child; found = 1; break; }
            child = (child + cl + 3) & ~3u;
        }
        if (!found) return 0;
    }
    {
        uint32_t c = blk + 6;
        while (x86_rd16(C, c)) c += 2;
        val = (c + 2 + 3) & ~3u;
        vlen = x86_rd16(C, blk + 2);
    }
    if (blk == a[0]) {                                     /* "\": VS_FIXEDFILEINFO */
        if (RD32(val) != 0xFEEF04BDu) val = a[0] + VER_FIXED;   /* our own block has no key */
        if (RD32(val) != 0xFEEF04BDu) return 0;
        vlen = 0x34u;
    } else if (x86_rd16(C, blk + 4) == 1) {                /* text: an ANSI copy in the second half */
        uint32_t to = a[0] + total + (val - a[0]), i;
        for (i = 0; i < vlen; i++) x86_wr8(C, to + i, (uint8_t)x86_rd16(C, val + i * 2));
        val = to;
    }
    if (a[2]) WR32(a[2], val);
    if (a[3]) WR32(a[3], vlen);
    return 1;
}
/* Some engines look for ddraw.dll in the system directory before loading it. */
#define FIND_FAKE 0x7E700001u
static int is_system_dll_path(const char *p)
{
    const char *b = strrchr(p, '\\');
    static const char *const helpers[] = { "ddhelp.exe", "ddraw16.dll", "dsound.vxd", "d3dim.dll", "d3drm.dll", NULL };   /* the rest of DirectX */
    int i;
    if (strncasecmp(p, "C:\\WINDOWS\\SYSTEM", 17) || !b) return 0;
    for (i = 0; helpers[i]; i++) if (!strcasecmp(b + 1, helpers[i])) return 1;
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

/* ================================================================ small answers */
FN(c_GetPaletteEntries) { UNUSED; if (a[3]) { uint32_t i; for (i = 0; i < a[2]; i++) WR32(a[3] + i * 4, 0); } return a[2]; }
FN(c_GetDIBColorTable) { UNUSED; return 0; }
FN(c_GetShortPathNameA) { char p[300]; rd_str(w, a[0], p, sizeof(p)); return wr_str(w, a[1], a[2], p); }
FN(c_FatalAppExitA) { char m[200]; rd_str(w, a[1], m, sizeof(m)); qw_log(w, "FatalAppExit: %s", m); w->exiting = 1; C->stop = 1; return 0; }
FN(c_GetClipBox) { if (a[1]) { WR32(a[1], 0); WR32(a[1] + 4, 0); WR32(a[1] + 8, 640); WR32(a[1] + 12, 480); } return 2; /* SIMPLEREGION */ }
FN(c_GetProcessVersion) { UNUSED; return 0x00040000u; }
FN(c_RegQueryValueA) { UNUSED; return 2; }
/* No codecs: only metric asked by games at start, the largest format size, gets an answer. */
FN(c_LoadLibraryExA) {
    const qhle *e = qw_find_hle("kernel32", "LoadLibraryA");
    uint32_t b[16];
    memset(b, 0, sizeof(b));
    b[0] = a[0];
    return e ? e->fn(w, b) : 0;
}
/* CoLoadLibrary (name, autofree) and CoFreeLibrary: InstallShield loads a DLL that way to call
 * its DllRegisterServer. Same as LoadLibraryW and FreeLibrary. */
FN(c_CoLoadLibrary) {
    const qhle *e = qw_find_hle("kernel32", "LoadLibraryW");
    uint32_t b[16];
    memset(b, 0, sizeof(b));
    b[0] = a[0];
    return e ? e->fn(w, b) : 0;
}
FN(c_CoFreeLibrary) {
    const qhle *e = qw_find_hle("kernel32", "FreeLibrary");
    uint32_t b[16];
    memset(b, 0, sizeof(b));
    b[0] = a[0];
    if (e) (void)e->fn(w, b);
    return 0;
}
FN(c_StringFromGUID2) {
    char t[64];
    int i, n;
    uint8_t g[16];
    for (i = 0; i < 16; i++) g[i] = x86_rd8(C, a[0] + (uint32_t)i);
    n = snprintf(t, sizeof(t), "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                 g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    if ((int)a[2] < n + 1) return 0;
    for (i = 0; i <= n; i++) x86_wr16(C, a[1] + (uint32_t)i * 2, (uint16_t)(uint8_t)t[i]);
    return (uint32_t)n + 1;
}
/* An event or mutex nobody created: not found, what a program checks for its running twin. */
FN(c_OpenEventA) { UNUSED; w->last_error = 2; return 0; }
FN(c_DdeInitializeA) { if (a[0]) WR32(a[0], 1); return 0; }
FN(c_GetKeyboardLayoutList) { if (a[0] > 0 && a[1]) WR32(a[1], 0x04070407u); return 1; }
/* ---- lz32: files packed with COMPRESS.EXE ("SZDD"), and plain ones, read through one call */
#define LZ_MAX 16
#define LZ_BASE 0x7A000000u
static struct { uint8_t *data; uint32_t size, pos; } g_lz[LZ_MAX];
static uint8_t *lz_expand(const uint8_t *src, uint32_t n, uint32_t *out_size)
{
    static const uint8_t magic[8] = { 'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33 };
    uint8_t win[4096], *out;
    uint32_t want, i = 14, o = 0, w = 4096 - 16, k;
    if (n < 14 || memcmp(src, magic, 8)) return NULL;
    want = (uint32_t)src[10] | ((uint32_t)src[11] << 8) | ((uint32_t)src[12] << 16) | ((uint32_t)src[13] << 24);
    out = (uint8_t *)malloc(want ? want : 1);
    if (!out) return NULL;
    memset(win, ' ', sizeof(win));
    while (i < n && o < want) {
        int flags = src[i++], b;
        for (b = 0; b < 8 && i < n && o < want; b++, flags >>= 1) {
            if (flags & 1) {
                out[o++] = win[w] = src[i++];
                w = (w + 1) & 4095;
            } else {
                uint32_t pos, len;
                if (i + 1 >= n) break;
                pos = src[i] | ((src[i + 1] & 0xF0u) << 4);
                len = (src[i + 1] & 0x0Fu) + 3;
                i += 2;
                for (k = 0; k < len && o < want; k++) {
                    out[o++] = win[w] = win[(pos + k) & 4095];
                    w = (w + 1) & 4095;
                }
            }
        }
    }
    *out_size = o;
    return out;
}
/* Takes the whole of an open file into memory, unpacked. Returns an LZ handle. */
static uint32_t lz_take(qwin *w, uint32_t h)
{
    const qhle *sk = qw_find_hle("kernel32", "_llseek"), *rd = qw_find_hle("kernel32", "_lread"), *cl = qw_find_hle("kernel32", "_lclose");
    uint32_t b[16], size, tmp;
    int i;
    uint8_t *data, *x;
    if (!sk || !rd || !cl) return 0xFFFFFFFFu;
    for (i = 0; i < LZ_MAX; i++) if (!g_lz[i].data) break;
    if (i == LZ_MAX) return 0xFFFFFFFFu;
    memset(b, 0, sizeof(b)); b[0] = h; b[1] = 0; b[2] = 2;
    size = sk->fn(w, b);
    memset(b, 0, sizeof(b)); b[0] = h; b[1] = 0; b[2] = 0;
    sk->fn(w, b);
    tmp = qw_alloc(w, size ? size : 1);
    data = (uint8_t *)malloc(size ? size : 1);
    memset(b, 0, sizeof(b)); b[0] = h; b[1] = tmp; b[2] = size;
    rd->fn(w, b);
    if (data) x86_read_mem(C, tmp, data, size);
    qw_free(w, tmp);
    memset(b, 0, sizeof(b)); b[0] = h;
    cl->fn(w, b);
    if (!data) return 0xFFFFFFFFu;
    x = lz_expand(data, size, &tmp);
    if (x) { free(data); data = x; size = tmp; }
    g_lz[i].data = data; g_lz[i].size = size; g_lz[i].pos = 0;
    return LZ_BASE + (uint32_t)i;
}
FN(c_LZOpenFileA) {
    const qhle *op = qw_find_hle("kernel32", "_lopen");
    uint32_t b[16], h;
    if (!op) return 0xFFFFFFFFu;
    memset(b, 0, sizeof(b)); b[0] = a[0]; b[1] = 0;
    h = op->fn(w, b);
    if (h == 0xFFFFFFFFu) return 0xFFFFFFFFu;
    return lz_take(w, h);
}
/* The handle of a file opened otherwise, or one of ours, which stays what it is. */
FN(c_LZInit) {
    if (a[0] - LZ_BASE < LZ_MAX) return a[0];
    return lz_take(w, a[0]);
}
FN(c_LZRead) {
    uint32_t i = a[0] - LZ_BASE, n = a[2];
    if (i >= LZ_MAX || !g_lz[i].data) return 0xFFFFFFFFu;
    if (n > g_lz[i].size - g_lz[i].pos) n = g_lz[i].size - g_lz[i].pos;
    x86_write_mem(C, a[1], g_lz[i].data + g_lz[i].pos, n);
    g_lz[i].pos += n;
    return n;
}
FN(c_LZSeek) {
    uint32_t i = a[0] - LZ_BASE;
    int64_t p;
    (void)w;
    if (i >= LZ_MAX || !g_lz[i].data) return 0xFFFFFFFFu;
    p = a[2] == 0 ? (int64_t)(int32_t)a[1] : a[2] == 1 ? (int64_t)g_lz[i].pos + (int32_t)a[1] : (int64_t)g_lz[i].size + (int32_t)a[1];
    if (p < 0 || p > (int64_t)g_lz[i].size) return 0xFFFFFFFFu;
    g_lz[i].pos = (uint32_t)p;
    return g_lz[i].pos;
}
FN(c_LZClose) {
    uint32_t i = a[0] - LZ_BASE;
    (void)w;
    if (i < LZ_MAX && g_lz[i].data) { free(g_lz[i].data); g_lz[i].data = NULL; }
    return 0;
}
FN(c_RegisterClipboardFormatA) { static uint32_t next = 0xC000u; UNUSED; return next++; }
FN(c_CoRegisterClassObject) { if (a[4]) WR32(a[4], 1); return 0; }
FN(c_acmMetrics) { if (a[2]) WR32(a[2], a[1] == 50 ? 50u : 0u); return 0; }
static uint32_t g_prio_class = 0x20, g_thread_prio;
FN(c_SetPriorityClass) { (void)w; g_prio_class = a[1]; return 1; }
FN(c_GetPriorityClass) { UNUSED; return g_prio_class; }
/* A process handle is not a thread: real Windows fails, and Fury3, which passes
 * GetCurrentProcess() by mistake, reads success here as an error. */
FN(c_SetThreadPriority) { (void)w; if (a[0] == 0xFFFFFFFFu) return 0; g_thread_prio = a[1]; return 1; }
FN(c_GetThreadPriority) { UNUSED; return g_thread_prio; }
/* A movable block that is no longer locked: 0, and no error. */
FN(c_GlobalUnlock) { UNUSED; w->last_error = 0; return 0; }
FN(c_acmNoDriver) { UNUSED; return 0x200u + 11u; /* ACMERR: not possible */ }
FN(c_FileVersionNone) { UNUSED; return 0; }
FN(c_GetCharWidthA) { uint32_t i; for (i = a[1]; i <= a[2] && i - a[1] < 256; i++) WR32(a[3] + (i - a[1]) * 4, 8); return 1; }
FN(c_CharPrevA) { (void)w; return a[1] > a[0] ? a[1] - 1 : a[0]; }
FN(c_atom_add) { static uint32_t n = 0xC100; UNUSED; return n++; }
FN(c_BeginDeferWindowPos) { UNUSED; return 0x7E900001u; }

/* ================================================================ COM */
FN(c_CoCreateInstance) { return qw_dx_cocreate(w, a); }   /* DirectX classes; else REGDB_E_CLASSNOTREG */

/* ------------------------------------------------------------------ the C runtime of
 * Visual C++ 2015 and later (CrystalMark Retro): pointer encoding, interlocked singly linked
 * lists (one thread at a time runs x86 code here, so plain reads and writes are atomic),
 * GetModuleHandleExW, the 64-bit file size and position calls, console output in UTF-16 and
 * CommandLineToArgvW. */
FN(c_EncodePointer) { (void)w; return a[0]; }
FN(c_InitializeSListHead) { WR32(a[0], 0); WR32(a[0] + 4, 0); return 0; }
FN(c_InterlockedPushEntrySList) {
    uint32_t first = RD32(a[0]), depth = RD32(a[0] + 4);
    WR32(a[1], first); WR32(a[0], a[1]);
    WR32(a[0] + 4, ((depth & 0xFFFFu) + 1u) | ((depth + 0x10000u) & 0xFFFF0000u));
    return first;
}
FN(c_InterlockedPopEntrySList) {
    uint32_t first = RD32(a[0]), depth = RD32(a[0] + 4);
    if (!first) return 0;
    WR32(a[0], RD32(first));
    WR32(a[0] + 4, ((depth - 1u) & 0xFFFFu) | ((depth + 0x10000u) & 0xFFFF0000u));
    return first;
}
FN(c_InterlockedFlushSList) { uint32_t first = RD32(a[0]); WR32(a[0], 0); WR32(a[0] + 4, RD32(a[0] + 4) & 0xFFFF0000u); return first; }
FN(c_QueryDepthSList) { return RD32(a[0] + 4) & 0xFFFFu; }
FN(c_GetModuleHandleExW) {             /* (flags, name or address, HMODULE *) */
    uint32_t h;
    if (!a[2]) return 0;
    if (a[0] & 4u) h = w->image_base;  /* FROM_ADDRESS: the program's own code asks */
    else { uint32_t b[1]; b[0] = a[1]; h = call_api(w, "kernel32", "GetModuleHandleW", b); }
    WR32(a[2], h);
    return h != 0;
}
FN(c_FindFirstFileExW) { uint32_t b[2]; b[0] = a[0]; b[1] = a[2]; return call_api(w, "kernel32", "FindFirstFileW", b); }
FN(c_GetFileSizeEx) {
    uint32_t b[2], lo;
    b[0] = a[0]; b[1] = 0;
    lo = call_api(w, "kernel32", "GetFileSize", b);
    if (lo == 0xFFFFFFFFu) return 0;
    WR32(a[1], lo); WR32(a[1] + 4, 0);
    return 1;
}
FN(c_SetFilePointerEx) {               /* (h, distance low, distance high, LARGE_INTEGER *new, method) */
    uint32_t b[4], r;
    b[0] = a[0]; b[1] = a[1]; b[2] = 0; b[3] = a[4];
    r = call_api(w, "kernel32", "SetFilePointer", b);
    if (r == 0xFFFFFFFFu) return 0;
    if (a[3]) { WR32(a[3], r); WR32(a[3] + 4, 0); }
    return 1;
}
FN(c_GetConsoleOutputCP) { UNUSED; return 437; }
FN(c_WriteConsoleW) {                  /* (h, const WCHAR *, n, DWORD *written, reserved) */
    uint32_t n = a[2], tmp, i, b[5];
    if (!n) { if (a[3]) WR32(a[3], 0); return 1; }
    tmp = qw_alloc(w, n);
    if (!tmp) return 0;
    for (i = 0; i < n; i++) { uint16_t ch = RD16(a[1] + 2u * i); WR8(tmp + i, ch < 256 ? ch : '?'); }
    b[0] = a[0]; b[1] = tmp; b[2] = n; b[3] = a[3]; b[4] = 0;
    i = call_api(w, "kernel32", "WriteFile", b);
    qw_free(w, tmp);
    return i;
}
FN(c_FreeLibraryAndExitThread) { uint32_t b[1]; b[0] = a[1]; return call_api(w, "kernel32", "ExitThread", b); }
FN(c_CommandLineToArgvW) {             /* the Windows rules for quotes and backslashes */
    static wchar16 cmd[2048], out[2048];
    uint32_t starts[128], argc = 0, n = 0, blk, i, p = 0, len = (uint32_t)qw_rd_wstr(w, a[0], cmd, 2048);
    for (;;) {
        int quote = 0;
        while (p < len && (cmd[p] == ' ' || cmd[p] == '\t')) p++;
        if (p >= len || argc >= 127 || n >= 2040) break;
        starts[argc++] = n;
        while (p < len && n < 2040) {
            wchar16 ch = cmd[p];
            if (ch == '\\') {
                uint32_t bs = 0;
                while (p < len && cmd[p] == '\\') { bs++; p++; }
                if (p < len && cmd[p] == '"') {
                    for (i = 0; i < bs / 2 && n < 2040; i++) out[n++] = '\\';
                    if (bs & 1) { out[n++] = '"'; p++; }
                } else for (i = 0; i < bs && n < 2040; i++) out[n++] = '\\';
                continue;
            }
            if (ch == '"') { quote = !quote; p++; continue; }
            if (!quote && (ch == ' ' || ch == '\t')) break;
            out[n++] = ch;
            p++;
        }
        out[n++] = 0;
    }
    blk = qw_alloc(w, (argc + 1u) * 4u + n * 2u);
    if (!blk) return 0;
    for (i = 0; i < argc; i++) WR32(blk + i * 4u, blk + (argc + 1u) * 4u + starts[i] * 2u);
    WR32(blk + argc * 4u, 0);
    for (i = 0; i < n; i++) WR16(blk + (argc + 1u) * 4u + i * 2u, out[i]);
    if (a[1]) WR32(a[1], argc);
    return blk;
}

#define K(n, f, c) { "kernel32", n, c, f }
#define U(n, f, c) { "user32", n, c, f }
#define G(n, f, c) { "gdi32", n, c, f }
#define ADV(n, f, c) { "advapi32", n, c, f }
#define WM(n, f, c) { "winmm", n, c, f }
const qhle qw_compat[] = {
    /* the C runtime of Visual C++ 2015 and later */
    K("EncodePointer", c_EncodePointer, 1), K("DecodePointer", c_EncodePointer, 1),
    K("InitializeSListHead", c_InitializeSListHead, 1), K("InterlockedPushEntrySList", c_InterlockedPushEntrySList, 2),
    K("InterlockedPopEntrySList", c_InterlockedPopEntrySList, 1), K("InterlockedFlushSList", c_InterlockedFlushSList, 1),
    K("QueryDepthSList", c_QueryDepthSList, 1), K("GetModuleHandleExW", c_GetModuleHandleExW, 3),
    K("FindFirstFileExW", c_FindFirstFileExW, 6), K("GetFileSizeEx", c_GetFileSizeEx, 2),
    K("SetFilePointerEx", c_SetFilePointerEx, 5), K("GetConsoleOutputCP", c_GetConsoleOutputCP, 0),
    K("WriteConsoleW", c_WriteConsoleW, 5), K("FreeLibraryAndExitThread", c_FreeLibraryAndExitThread, 2),
    { "shell32", "CommandLineToArgvW", 2, c_CommandLineToArgvW },

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
    ADV("RegEnumKeyExA", c_RegEnumKeyExA, 8), ADV("RegEnumKeyA", c_RegEnumKeyA, 4), ADV("RegFlushKey", c_ok0, 1), ADV("RegDeleteKeyA", c_RegDeleteKeyA, 2),
    ADV("RegQueryInfoKeyA", c_RegQueryInfoKeyA, 12),
    /* INI files */
    K("GetPrivateProfileStringA", c_GetPrivateProfileStringA, 6), K("GetPrivateProfileIntA", c_GetPrivateProfileIntA, 4),
    K("WritePrivateProfileStringA", c_WritePrivateProfileStringA, 4), K("WriteProfileStringA", c_ok1, 3), K("GetPrivateProfileSectionA", c_GetPrivateProfileSectionA, 4), K("WritePrivateProfileSectionA", c_WritePrivateProfileSectionA, 3), K("GetProfileStringA", c_GetProfileStringA, 5), K("GetProfileIntA", c_GetProfileIntA, 3),
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
    K("GetUserDefaultLangID", c_LangId, 0), K("GetSystemDefaultLCID", c_LangId, 0), K("GetThreadLocale", c_LangId, 0), K("SystemTimeToFileTime", c_SystemTimeToFileTime, 2),
    K("GetExitCodeProcess", c_GetExitCodeProcess, 2), K("WinExec", c_WinExec, 2), K("CreateProcessA", c_CreateProcessA, 10), K("GetNumberFormatA", c_GetNumberFormatA, 6), K("CreateProcessW", c_CreateProcessW, 10), K("TerminateProcess", c_TerminateProcess, 2), K("GetDateFormatA", c_FormatDT, 6), K("GetTimeFormatA", c_FormatDT, 6),
    K("IsValidLocale", c_ok1, 2), K("IsValidCodePage", c_ok1, 1), K("SetEnvironmentVariableW", c_ok1, 2), K("UTRegister", c_ok0, 7), K("UTUnRegister", c_ok0, 1),
    K("GetOverlappedResult", c_ok0, 4), K("PeekNamedPipe", c_ok0, 6), K("GetCommState", c_ok0, 2), K("SetCommState", c_ok0, 2), K("SetCommTimeouts", c_ok0, 2),
    K("GetCommModemStatus", c_ok0, 2), K("GetCommProperties", c_ok0, 2), K("PurgeComm", c_ok0, 2), K("EscapeCommFunction", c_ok0, 2),
    K("LoadModule", c_ok0, 2),
    /* user32 */
    U("WaitForInputIdle", c_ok0, 2), U("GetUpdateRect", c_ok0, 3), U("GetUpdateRgn", c_ok0, 3), U("GetWindowThreadProcessId", c_GetWindowThreadProcessId, 2),
    U("GetTopWindow", c_ok0, 1), U("EnumThreadWindows", c_ok1, 3), U("EnumWindows", c_ok1, 2), U("SetSysColors", c_ok1, 3),
    U("GetKeyNameTextA", c_GetKeyNameTextA, 3), U("CharUpperBuffA", c_CharUpperBuffA, 2), U("CharLowerBuffA", c_CharLowerBuffA, 2), U("CharToOemA", c_CharToOemA, 2), U("OemToCharA", c_CharToOemA, 2),
    U("mouse_event", c_ok0, 5), U("keybd_event", c_ok0, 4), U("GetCaretBlinkTime", c_ok1, 0), U("GetDoubleClickTime", c_ok1, 0),
    U("ShowWindowAsync", c_ok1, 2), U("CloseWindow", c_ok1, 1), U("SetScrollPos", c_ok0, 4), U("SetScrollRange", c_ok1, 5), U("GetScrollPos", c_ok0, 2),
    U("ShowScrollBar", c_ok1, 3), U("SetScrollInfo", c_ok0, 4), U("GetKeyboardType", c_ok0, 1), U("GetInputState", c_ok0, 0),
    U("SetCaretPos", c_ok1, 2), U("CreateCaret", c_ok1, 4), U("DestroyCaret", c_ok1, 0), U("HideCaret", c_ok1, 1), U("ShowCaret", c_ok1, 1),
    U("DestroyAcceleratorTable", c_ok1, 1), U("CreateAcceleratorTableA", c_ok1, 2), U("GetClipCursor", c_ok1, 1), U("AnyPopup", c_ok0, 0), U("InsertMenuA", c_ok1, 5),
    /* gdi32 */
    G("SetStretchBltMode", c_ok1, 2), G("GetStretchBltMode", c_ok1, 1), G("GdiFlush", c_ok1, 0), G("SetMapMode", c_ok1, 2), G("GetMapMode", c_ok1, 1),
    G("SetWindowOrgEx", c_ok1, 4), G("SetViewportOrgEx", c_ok1, 4), G("GetBkMode", c_ok1, 1), G("ResizePalette", c_ok1, 2), G("AnimatePalette", qw_gdi_AnimatePalette, 4),
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
    { "comctl32", "#17", 0, c_ok0 }, { "comctl32", "InitCommonControls", 0, c_ok0 },
    G("GetPaletteEntries", qw_gdi_GetPaletteEntries, 4), G("GetDIBColorTable", c_GetDIBColorTable, 4), G("RectVisible", c_ok1, 2), G("PtVisible", c_ok1, 3),
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
    U("FindWindowExA", c_ok0, 4), U("DrawTextExA", c_ok0, 6),
    K("GetShortPathNameA", c_GetShortPathNameA, 3), K("FatalAppExitA", c_FatalAppExitA, 2), K("FatalExit", c_FatalAppExitA, 1), K("GlobalFlags", c_ok0, 1),
    K("GlobalAddAtomA", c_atom_add, 1), K("GlobalDeleteAtom", c_ok0, 1), K("GlobalGetAtomNameA", c_ok0, 3), K("GetProcessVersion", c_GetProcessVersion, 1),
    K("GetStringTypeExA", c_ok1, 5), K("LockFile", c_ok1, 5), K("UnlockFile", c_ok1, 5), K("GetProcessAffinityMask", c_ok0, 3), K("IsProcessorFeaturePresent", c_ok0, 1),
    ADV("RegQueryValueA", c_RegQueryValueA, 4), ADV("RegSetValueA", c_ok0, 5), ADV("SetFileSecurityA", c_ok1, 3), ADV("GetFileSecurityA", c_ok0, 5),
    { "shell32", "ShellExecuteA", 6, c_ShellExecuteA }, { "shell32", "DragFinish", 1, c_ok0 }, { "shell32", "DragQueryFileA", 4, c_ok0 }, { "shell32", "DragAcceptFiles", 2, c_ok0 },
    { "shell32", "SHGetFileInfoA", 5, c_ok0 }, { "shell32", "SHChangeNotify", 4, c_ok0 }, { "gdi32", "GetClipRgn", 2, c_ok0 }, { "gdi32", "OffsetClipRgn", 3, c_ok1 }, { "shell32", "SHBrowseForFolder", 1, c_ok0 }, { "shell32", "SHGetMalloc", 1, c_efail }, { "shell32", "SHGetDesktopFolder", 1, c_efail }, { "shell32", "SHGetSpecialFolderLocation", 3, c_efail }, { "shell32", "SHGetPathFromIDList", 2, c_ok0 },
    { "version", "VerLanguageNameA", 3, c_VerLanguageNameA }, { "dsetup", "DirectXSetupGetVersion", 2, c_DirectXSetupGetVersion },
    { "dsetup", "DirectXSetupA", 3, c_ok0 }, { "dsetup", "DirectXSetupW", 3, c_ok0 }, { "dsetup", "DirectXSetup", 3, c_ok0 }, { "dsetup", "DirectXDeviceDriverSetupA", 4, c_ok0 },
    { "dsetup", "DirectXRegisterApplicationA", 2, c_ok1 }, { "dsetup", "DirectXRegisterApplicationW", 2, c_ok1 }, { "dsetup", "DirectXUnRegisterApplication", 2, c_ok1 },
    { "dsetup", "DirectXSetupSetCallback", 1, c_ok0 }, { "dsetup", "DirectXSetupGetEULAA", 3, c_ok1 }, { "user32", "ExitWindowsEx", 2, c_ok0 },
    { "user32", "DdeDisconnect", 1, c_ok1 }, { "user32", "DdeClientTransaction", 8, c_ok0 }, { "user32", "DdeCreateDataHandle", 7, c_ok0 },
    { "comctl32", "ImageList_Create", 5, c_ok1 }, { "comctl32", "ImageList_Add", 3, c_ok0 }, { "comctl32", "ImageList_Destroy", 1, c_ok1 },
    { "imm32", "ImmGetContext", 1, c_ok0 }, { "imm32", "ImmReleaseContext", 2, c_ok1 }, { "imm32", "ImmSetOpenStatus", 2, c_ok1 }, { "imm32", "ImmGetOpenStatus", 1, c_ok0 },
    { "imm32", "ImmAssociateContext", 2, c_ok0 }, { "imm32", "ImmNotifyIME", 4, c_ok1 }, { "imm32", "ImmGetDefaultIMEWnd", 1, c_ok0 },
    { "oleaut32", "SysAllocString", 1, ole_SysAllocString }, { "oleaut32", "#2", 1, ole_SysAllocString }, { "oleaut32", "SysAllocStringLen", 2, ole_SysAllocStringLen }, { "oleaut32", "#4", 2, ole_SysAllocStringLen },
    { "oleaut32", "SysReAllocStringLen", 3, ole_SysReAllocStringLen }, { "oleaut32", "#5", 3, ole_SysReAllocStringLen }, { "oleaut32", "SysFreeString", 1, ole_SysFreeString }, { "oleaut32", "#6", 1, ole_SysFreeString },
    { "oleaut32", "SysStringLen", 1, ole_SysStringLen }, { "oleaut32", "#7", 1, ole_SysStringLen }, { "oleaut32", "VariantInit", 1, ole_VariantInit }, { "oleaut32", "#8", 1, ole_VariantInit },
    { "oleaut32", "VariantClear", 1, ole_VariantClear }, { "oleaut32", "#9", 1, ole_VariantClear }, { "oleaut32", "VariantCopy", 2, ole_VariantCopy }, { "oleaut32", "#10", 2, ole_VariantCopy },
    { "oleaut32", "VariantCopyInd", 2, ole_VariantCopy }, { "oleaut32", "VariantChangeTypeEx", 5, ole_VariantChangeType }, { "oleaut32", "VariantChangeType", 4, ole_VariantChangeType },
    { "gdi32", "EnumFontFamiliesA", 4, c_EnumFontFamiliesA }, { "gdi32", "EnumFontsA", 4, c_EnumFontFamiliesA }, { "advapi32", "InitializeSecurityDescriptor", 2, c_ok1 }, { "advapi32", "SetSecurityDescriptorDacl", 4, c_ok1 }, { "advapi32", "OpenProcessToken", 3, c_nosec }, { "advapi32", "LookupPrivilegeValueA", 3, c_nosec }, { "advapi32", "AdjustTokenPrivileges", 6, c_nosec },
    { "advapi32", "OpenThreadToken", 4, c_nosec }, { "advapi32", "GetTokenInformation", 5, c_nosec }, { "advapi32", "AllocateAndInitializeSid", 11, c_nosec },
    { "advapi32", "FreeSid", 1, c_ok0 }, { "advapi32", "EqualSid", 2, c_nosec }, { "advapi32", "IsValidSid", 1, c_nosec }, { "advapi32", "GetLengthSid", 1, c_nosec },
    { "advapi32", "InitializeAcl", 3, c_nosec }, { "advapi32", "AddAccessAllowedAce", 4, c_nosec }, { "advapi32", "AccessCheck", 8, c_nosec },
    { "version", "GetFileVersionInfoSizeA", 2, c_GetFileVersionInfoSizeA }, { "version", "GetFileVersionInfoA", 4, c_GetFileVersionInfoA }, { "version", "VerQueryValueA", 4, c_VerQueryValueA },
    { "msacm32", "acmMetrics", 3, c_acmMetrics }, { "msacm32", "acmStreamOpen", 8, c_acmNoDriver }, { "msacm32", "acmStreamClose", 2, c_ok0 }, { "msacm32", "acmStreamSize", 4, c_acmNoDriver },
    { "msacm32", "acmStreamPrepareHeader", 3, c_acmNoDriver }, { "msacm32", "acmStreamUnprepareHeader", 3, c_ok0 }, { "msacm32", "acmStreamConvert", 3, c_acmNoDriver },
    { "kernel32", "LoadLibraryExA", 3, c_LoadLibraryExA }, { "kernel32", "DisableThreadLibraryCalls", 1, c_ok1 },
    { "user32", "RegisterClipboardFormatA", 1, c_RegisterClipboardFormatA },
    { "lz32", "LZOpenFileA", 3, c_LZOpenFileA }, { "lz32", "LZRead", 3, c_LZRead }, { "lz32", "LZSeek", 3, c_LZSeek },
    { "lz32", "LZClose", 1, c_LZClose }, { "lz32", "LZInit", 1, c_LZInit }, { "lz32", "LZStart", 0, c_ok1 }, { "lz32", "LZDone", 0, c_ok0 },
    { "user32", "SetMessageQueue", 1, c_ok1 }, { "user32", "GetKeyboardLayoutList", 2, c_GetKeyboardLayoutList },
    { "kernel32", "OpenMutexA", 3, c_OpenEventA },
    { "kernel32", "OpenEventA", 3, c_OpenEventA }, { "msacm32", "acmFormatSuggest", 5, c_acmNoDriver },
    { "user32", "DdeInitializeA", 4, c_DdeInitializeA }, { "user32", "DdeUninitialize", 1, c_ok1 },
    { "user32", "DdeCreateStringHandleA", 3, c_ok0 }, { "user32", "DdeFreeStringHandle", 2, c_ok1 },
    { "user32", "DdeNameService", 4, c_ok0 }, { "user32", "DdeConnect", 4, c_ok0 }, { "user32", "DdeGetLastError", 1, c_ok0 },
    { "comctl32", "#6", 4, c_ok0 }, { "comctl32", "#16", 12, c_ok0 }, { "comctl32", "#8", 13, c_ok0 },
    { "ole32", "StringFromGUID2", 3, c_StringFromGUID2 }, { "ole32", "CoRegisterClassObject", 5, c_CoRegisterClassObject },
    { "ole32", "CoRevokeClassObject", 1, c_ok0 },
    /* ole32 */
    { "ole32", "CoLoadLibrary", 2, c_CoLoadLibrary }, { "ole32", "CoFreeLibrary", 1, c_CoFreeLibrary },
    { "ole32", "CoInitialize", 1, c_ok0 }, { "ole32", "CoUninitialize", 0, c_ok0 }, { "ole32", "CoCreateInstance", 5, c_CoCreateInstance },
    { NULL, NULL, 0, NULL }
};
