/* QRX OS One 22.5.12 - qrx-win32: files for Windows programs.
 *
 * Drive model: C: (and D:, reported as a CD-ROM drive for CD checks) is the
 * directory of the program; the current directory starts there.  Windows paths
 * ("C:\GTA\data\x.dat", "\data\x.dat", "data\x.dat", forward slashes too) are
 * mapped below that root, and every path component is looked up without regard
 * to case, as on Windows (Linux host file systems are case-sensitive).
 *
 * kernel32: CreateFile/ReadFile/WriteFile/SetFilePointer/GetFileSize/
 * FindFirstFile/directories/drives; msvcrt: FILE streams (fopen...), low-level
 * _open/_read/..., _findfirst, _stat, _getcwd.  Console handles and the _iob
 * streams keep their existing meaning (stdout/stderr). */
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "qrxwin.h"

#define FN(name) static uint32_t name(qwin *w, uint32_t *a)
#define C (&w->cpu)
#define RD32(x) x86_rd32(C, (x))
#define WR32(x, v) x86_wr32(C, (x), (v))
#define UNUSED (void)w; (void)a

#define QF_MAX 64
#define QF_HBASE 0x00006000u                  /* kernel32 file / find handles */
enum { QF_FREE, QF_FILE, QF_FIND };
typedef struct qfile {
    int type;
    FILE *f;
    int text, eof;                            /* msvcrt stream state */
    uint32_t gfile;                           /* guest FILE * (fopen) */
    int fd;                                   /* msvcrt low-level descriptor (_open), -1 if none */
    /* FindFirstFile */
    DIR *d;
    char dir[1024], pat[256];
    int wide;
} qfile;
static qfile g_f[QF_MAX];
static char g_root[1024];                     /* host directory of drive C: */
static char g_cwd[512];                       /* current directory relative to the root ("" = root) */
static int g_init;

static void init_root(qwin *w)
{
    const char *sl;
    if (g_init) return;
    g_init = 1;
    sl = strrchr(w->exe_path, '/');
    if (!sl) sl = strrchr(w->exe_path, ':');
    if (sl) {
        size_t L = (size_t)(sl - w->exe_path) + (*sl == ':' ? 1 : 0);
        if (L >= sizeof(g_root)) L = sizeof(g_root) - 1;
        memcpy(g_root, w->exe_path, L); g_root[L] = 0;
    } else if (!getcwd(g_root, sizeof(g_root))) g_root[0] = 0;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* Levels above the program's folder that the last path climbed: games often keep their data
 * in ..\\data, and the program's folder is the root of the Windows paths here. */
static int g_above;

/* Windows path -> path relative to the root ("a/b/c"), with . and .. resolved */
static void win_rel(const char *in, char *out, size_t max)
{
    char tmp[1024], *comp[64];
    int n = 0, i;
    size_t k = 0;
    const char *p = in;
    g_above = 0;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') p += 2;                     /* drive letter */
    if (p[0] == '\\' || p[0] == '/') snprintf(tmp, sizeof(tmp), "%s", p + 1);
    else snprintf(tmp, sizeof(tmp), "%s%s%s", g_cwd, g_cwd[0] ? "/" : "", p);
    for (i = 0; tmp[i]; i++) if (tmp[i] == '\\') tmp[i] = '/';
    {
        char *s = tmp, *t;
        while (s && *s) {
            t = strchr(s, '/');
            if (t) *t = 0;
            if (!*s || !strcmp(s, ".")) {}
            else if (!strcmp(s, "..")) { if (n) n--; else g_above++; }
            else if (n < 64) comp[n++] = s;
            s = t ? t + 1 : NULL;
        }
    }
    out[0] = 0;
    for (i = 0; i < n; i++) {
        size_t L = strlen(comp[i]);
        if (k + L + 2 >= max) break;
        if (i) out[k++] = '/';
        memcpy(out + k, comp[i], L); k += L; out[k] = 0;
    }
}

/* host path for a root-relative path; each component matched case-insensitively.
 * Components that do not exist are kept as written (for files being created). */
static void host_path(const char *rel, char *out, size_t max)
{
    char cur[1200], part[256];
    const char *s = rel;
    snprintf(cur, sizeof(cur), "%s", g_root[0] ? g_root : ".");
    {
        int up;
        for (up = 0; up < g_above; up++) {
            char *b = strrchr(cur, '/');
            if (b) *b = 0;
            else if ((b = strchr(cur, ':')) != NULL) b[1] = 0;
        }
    }
    while (*s) {
        const char *t = strchr(s, '/');
        size_t L = t ? (size_t)(t - s) : strlen(s);
        size_t cl = strlen(cur);
        struct stat st;
        if (L >= sizeof(part)) L = sizeof(part) - 1;
        memcpy(part, s, L); part[L] = 0;
        snprintf(cur + cl, sizeof(cur) - cl, "%s%s", (cl && cur[cl - 1] != '/' && cur[cl - 1] != ':') ? "/" : "", part);
        if (stat(cur, &st) != 0) {                   /* try a case-insensitive match */
            DIR *d;
            struct dirent *e;
            cur[cl] = 0;
            d = opendir(cl ? cur : ".");
            if (d) {
                while ((e = readdir(d))) if (ieq(e->d_name, part)) { snprintf(part, sizeof(part), "%s", e->d_name); break; }
                closedir(d);
            }
            snprintf(cur + cl, sizeof(cur) - cl, "%s%s", (cl && cur[cl - 1] != '/' && cur[cl - 1] != ':') ? "/" : "", part);
        }
        s += L;
        while (*s == '/') s++;
    }
    snprintf(out, max, "%s", cur);
}

static void map_path(qwin *w, const char *win, char *host, size_t max)
{
    char rel[1024];
    init_root(w);
    win_rel(win, rel, sizeof(rel));
#ifdef __AROS__
    /* C:\ is the program's folder, but C:\WINDOWS is one place for all of them: what an
       installer puts there has to reach the game it installed. */
    if (!g_above && !strncasecmp(rel, "windows", 7) && (rel[7] == 0 || rel[7] == '/') &&
        ((isalpha((unsigned char)win[0]) && win[1] == ':') || win[0] == '\\' || win[0] == '/')) {
        char keep[sizeof(g_root)];
        static int made;
        if (!made) { made = 1; mkdir("SYS:WINDOWS", 0777); mkdir("SYS:WINDOWS/SYSTEM", 0777); }
        memcpy(keep, g_root, sizeof(keep));
        strcpy(g_root, "SYS:");
        host_path(rel, host, max);
        memcpy(g_root, keep, sizeof(keep));
        return;
    }
#endif
    /* An installer running elsewhere stored C:\\GAMES\\X, and C:\\ here is GAMES\\X itself:
       drop what the program's own folder already says. */
    if (!g_above && ((isalpha((unsigned char)win[0]) && win[1] == ':') || win[0] == '\\')) {
        size_t rl = strlen(g_root), k;
        for (k = strlen(rel); k > 0; k--) {
            if (rel[k] != 0 && rel[k] != '/') continue;
            if (k < rl && (g_root[rl - k - 1] == '/' || g_root[rl - k - 1] == ':') && !strncasecmp(g_root + rl - k, rel, k)) {
                memmove(rel, rel + k + (rel[k] == '/'), strlen(rel + k + (rel[k] == '/')) + 1);
                break;
            }
        }
    }
    host_path(rel, host, max);
}

static void rd_path(qwin *w, uint32_t p, int wide, char *out, int max)
{
    if (wide) { wchar16 t[600]; qw_rd_wstr(w, p, t, 600); w2a(out, t, max); }
    else qw_rd_str(w, p, out, max);
}

static int slot_new(int type)
{
    int i;
    for (i = 0; i < QF_MAX; i++) if (g_f[i].type == QF_FREE) { memset(&g_f[i], 0, sizeof(g_f[i])); g_f[i].type = type; g_f[i].fd = -1; return i; }
    return -1;
}
static qfile *by_handle(uint32_t h, int type)
{
    uint32_t i = (h - QF_HBASE) / 4u;
    if (h < QF_HBASE || (h & 3) || i >= QF_MAX || g_f[i].type != type) return NULL;
    return &g_f[i];
}
static uint32_t handle_of(int i) { return QF_HBASE + (uint32_t)i * 4u; }

/* --------------------------------------------------------------- shared with kernel.c */
FILE *qw_stream_host(qwin *w, uint32_t gfile)
{
    int i;
    (void)w;
    if (!gfile) return NULL;
    for (i = 0; i < QF_MAX; i++) if (g_f[i].type == QF_FILE && g_f[i].gfile == gfile) return g_f[i].f;
    return NULL;
}
int qw_file_close(qwin *w, uint32_t h)
{
    qfile *f = by_handle(h, QF_FILE);
    (void)w;
    if (f) { if (f->f) fclose(f->f); f->type = QF_FREE; return 1; }
    f = by_handle(h, QF_FIND);
    if (f) { if (f->d) closedir(f->d); f->type = QF_FREE; return 1; }
    return 0;
}

/* --------------------------------------------------------------- kernel32 files */
#define INVALID_HANDLE 0xFFFFFFFFu
static uint32_t create_file(qwin *w, uint32_t *a, int wide)
{
    char win[600], host[1200];
    uint32_t access = a[1], disp = a[4];
    const char *mode;
    struct stat st;
    int exists, i;
    FILE *fp;
    rd_path(w, a[0], wide, win, sizeof(win));
    if (!strncmp(win, "\\\\.\\", 4) || ieq(win, "CON") || ieq(win, "NUL")) { w->last_error = 2; return INVALID_HANDLE; }
    map_path(w, win, host, sizeof(host));
    exists = stat(host, &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) { w->last_error = 5; return INVALID_HANDLE; }
    switch (disp) {
    case 1: if (exists) { w->last_error = 80; return INVALID_HANDLE; } mode = "w+b"; break;     /* CREATE_NEW */
    case 2: mode = "w+b"; break;                                                               /* CREATE_ALWAYS */
    case 3: if (!exists) { w->last_error = 2; return INVALID_HANDLE; }                         /* OPEN_EXISTING */
            mode = (access & 0x40000000u) ? "r+b" : "rb"; break;
    case 4: mode = exists ? ((access & 0x40000000u) ? "r+b" : "rb") : "w+b"; break;            /* OPEN_ALWAYS */
    case 5: if (!exists) { w->last_error = 2; return INVALID_HANDLE; } mode = "w+b"; break;     /* TRUNCATE_EXISTING */
    default: w->last_error = 87; return INVALID_HANDLE;
    }
    fp = fopen(host, mode);
    if (!fp && !strcmp(mode, "r+b")) fp = fopen(host, "rb");     /* read-only media: still readable */
    if (!fp) { w->last_error = exists ? 5 : 3; if (w->verbose) qw_log(w, "CreateFile(%s -> %s): failed", win, host); return INVALID_HANDLE; }
    i = slot_new(QF_FILE);
    if (i < 0) { fclose(fp); w->last_error = 4; return INVALID_HANDLE; }
    g_f[i].f = fp;
    w->last_error = (disp == 2 || disp == 4) && exists ? 183 : 0;
    if (w->verbose) qw_log(w, "CreateFile(%s) -> %s", win, host);
    return handle_of(i);
}
FN(f_CreateFileA) { return create_file(w, a, 0); }
FN(f_CreateFileW) { return create_file(w, a, 1); }
FN(f_ReadFile) {
    qfile *f = by_handle(a[0], QF_FILE);
    uint32_t n = 0, chunk;
    uint8_t buf[4096];
    if (!f) { if (a[3]) WR32(a[3], 0); return 0; }
    while (n < a[2]) {
        size_t got;
        chunk = a[2] - n < sizeof(buf) ? a[2] - n : (uint32_t)sizeof(buf);
        got = fread(buf, 1, chunk, f->f);
        if (got) x86_write_mem(C, a[1] + n, buf, (uint32_t)got);
        n += (uint32_t)got;
        if (got < chunk) break;
    }
    if (a[3]) WR32(a[3], n);
    return 1;
}
FN(f_WriteFile) {
    qfile *f = by_handle(a[0], QF_FILE);
    uint32_t i;
    if (!f) {                                  /* console handles from GetStdHandle */
        for (i = 0; i < a[2]; i++) fputc(x86_rd8(C, a[1] + i), a[0] == 0x10 + 0xF4 ? stderr : stdout);
        if (a[3]) WR32(a[3], a[2]);
        return 1;
    }
    for (i = 0; i < a[2]; i++) fputc(x86_rd8(C, a[1] + i), f->f);
    if (a[3]) WR32(a[3], a[2]);
    return 1;
}
FN(f_SetFilePointer) {
    qfile *f = by_handle(a[0], QF_FILE);
    int64_t off = (int32_t)a[1];
    if (!f) return INVALID_HANDLE;
    if (a[2]) off = (int64_t)(((uint64_t)RD32(a[2]) << 32) | a[1]);
    if (fseek(f->f, (long)off, a[3] == 1 ? SEEK_CUR : a[3] == 2 ? SEEK_END : SEEK_SET)) { w->last_error = 131; return INVALID_HANDLE; }
    off = ftell(f->f);
    if (a[2]) WR32(a[2], (uint32_t)((uint64_t)off >> 32));
    return (uint32_t)off;
}
FN(f_GetFileSize) {
    qfile *f = by_handle(a[0], QF_FILE);
    long cur, end;
    if (!f) return INVALID_HANDLE;
    cur = ftell(f->f); fseek(f->f, 0, SEEK_END); end = ftell(f->f); fseek(f->f, cur, SEEK_SET);
    if (a[1]) WR32(a[1], 0);
    return (uint32_t)end;
}
FN(f_GetFileType) { UNUSED; return by_handle(a[0], QF_FILE) ? 1u : 2u; }             /* DISK / CHAR (console) */
FN(f_FlushFileBuffers) { qfile *f = by_handle(a[0], QF_FILE); UNUSED; if (f) fflush(f->f); return 1; }
FN(f_SetEndOfFile) { qfile *f = by_handle(a[0], QF_FILE); UNUSED; if (f) { fflush(f->f); if (ftruncate(fileno(f->f), ftell(f->f))) return 0; } return f != NULL; }
FN(f_GetFileTime) { UNUSED; if (a[1]) { WR32(a[1], 0); WR32(a[1] + 4, 0); } if (a[2]) { WR32(a[2], 0); WR32(a[2] + 4, 0); } if (a[3]) { WR32(a[3], 0); WR32(a[3] + 4, 0); } return 1; }

static uint32_t file_attrs(qwin *w, uint32_t p, int wide)
{
    char win[600], host[1200];
    struct stat st;
    rd_path(w, p, wide, win, sizeof(win));
    map_path(w, win, host, sizeof(host));
    if (stat(host, &st)) { w->last_error = 2; return INVALID_HANDLE; }
    return S_ISDIR(st.st_mode) ? 0x10u : 0x80u;               /* DIRECTORY / NORMAL */
}
FN(f_GetFileAttributesA) { return file_attrs(w, a[0], 0); }
/* SearchPathA(path, file, ext, buflen, buf, filepart): the program's directory only. */
FN(f_SearchPathA) {
    char name[300], ext[32], host[1200], full[340];
    struct stat st;
    uint32_t i, n, part;
    qw_rd_str(w, a[1], name, sizeof(name));
    ext[0] = 0;
    if (a[2] && !strchr(name, '.')) qw_rd_str(w, a[2], ext, sizeof(ext));
    snprintf(full, sizeof(full), "%s%s", name, ext);
    map_path(w, full, host, sizeof(host));
    if (stat(host, &st)) return 0;
    n = (uint32_t)strlen(full) + 3;
    if (n + 1 > a[3]) return n + 1;
    x86_wr8(C, a[4], 'C'); x86_wr8(C, a[4] + 1, ':'); x86_wr8(C, a[4] + 2, '\\');
    for (i = 0; i <= strlen(full); i++) x86_wr8(C, a[4] + 3 + i, (uint8_t)full[i]);
    part = a[4] + 3;
    if (a[5]) WR32(a[5], part);
    return n;
}
FN(f_GetFileAttributesW) { return file_attrs(w, a[0], 1); }
FN(f_SetFileAttributesA) { UNUSED; return 1; }
static uint32_t del_file(qwin *w, uint32_t p, int wide) { char win[600], host[1200]; rd_path(w, p, wide, win, sizeof(win)); map_path(w, win, host, sizeof(host)); return remove(host) == 0; }
FN(f_DeleteFileA) { return del_file(w, a[0], 0); }
FN(f_DeleteFileW) { return del_file(w, a[0], 1); }
static uint32_t mk_dir(qwin *w, uint32_t p, int wide) { char win[600], host[1200]; rd_path(w, p, wide, win, sizeof(win)); map_path(w, win, host, sizeof(host)); return mkdir(host, 0777) == 0; }
FN(f_CreateDirectoryA) { return mk_dir(w, a[0], 0); }
FN(f_CreateDirectoryW) { return mk_dir(w, a[0], 1); }
FN(f_RemoveDirectoryA) { char win[600], host[1200]; rd_path(w, a[0], 0, win, sizeof(win)); map_path(w, win, host, sizeof(host)); return rmdir(host) == 0; }
FN(f_MoveFileA) {
    char w1[600], w2[600], h1[1200], h2[1200];
    rd_path(w, a[0], 0, w1, sizeof(w1)); rd_path(w, a[1], 0, w2, sizeof(w2));
    map_path(w, w1, h1, sizeof(h1)); map_path(w, w2, h2, sizeof(h2));
    return rename(h1, h2) == 0;
}

/* current directory and full paths ("C:\" + relative path) */
static int full_of(const char *rel, char *out, int max)
{
    int k = snprintf(out, (size_t)max, "C:\\%s", rel), i;
    for (i = 0; out[i]; i++) if (out[i] == '/') out[i] = '\\';
    return k;
}
static uint32_t put_str(qwin *w, uint32_t dst, uint32_t max, const char *s, int wide)
{
    uint32_t n = (uint32_t)strlen(s), i;
    if (n + 1 > max) return n + 1;                 /* buffer too small: required size */
    for (i = 0; i <= n; i++) { if (wide) x86_wr16(C, dst + i * 2, (uint8_t)s[i]); else x86_wr8(C, dst + i, (uint8_t)s[i]); }
    return n;
}
FN(f_GetCurrentDirectoryA) { char b[600]; init_root(w); full_of(g_cwd, b, sizeof(b)); return put_str(w, a[1], a[0], b, 0); }
FN(f_GetCurrentDirectoryW) { char b[600]; init_root(w); full_of(g_cwd, b, sizeof(b)); return put_str(w, a[1], a[0], b, 1); }
static uint32_t set_cwd(qwin *w, uint32_t p, int wide)
{
    char win[600], rel[1024], host[1200];
    struct stat st;
    init_root(w);
    rd_path(w, p, wide, win, sizeof(win));
    win_rel(win, rel, sizeof(rel));
    host_path(rel, host, sizeof(host));
    if (stat(host, &st) || !S_ISDIR(st.st_mode)) { w->last_error = 3; return 0; }
    snprintf(g_cwd, sizeof(g_cwd), "%s", rel);
    return 1;
}
FN(f_SetCurrentDirectoryA) { return set_cwd(w, a[0], 0); }
FN(f_SetCurrentDirectoryW) { return set_cwd(w, a[0], 1); }
static uint32_t full_path(qwin *w, uint32_t *a, int wide)
{
    char win[600], rel[1024], b[1100];
    uint32_t r;
    init_root(w);
    rd_path(w, a[0], wide, win, sizeof(win));
    win_rel(win, rel, sizeof(rel));
    full_of(rel, b, sizeof(b));
    r = put_str(w, a[2], a[1], b, wide);
    if (a[3] && r < a[1]) {                           /* lpFilePart */
        const char *sl = strrchr(b, '\\');
        WR32(a[3], a[2] + (uint32_t)((sl ? sl + 1 : b) - b) * (wide ? 2u : 1u));
    }
    return r;
}
FN(f_GetFullPathNameA) { return full_path(w, a, 0); }
FN(f_GetFullPathNameW) { return full_path(w, a, 1); }

/* drives: C: = program directory (fixed), D: = the same directory as a CD-ROM drive */
static int drive_of(qwin *w, uint32_t p, int wide)
{
    char b[8];
    if (!p) return 'C';
    rd_path(w, p, wide, b, sizeof(b));
    return toupper((unsigned char)b[0]);
}
FN(f_GetDriveTypeA) { int d = drive_of(w, a[0], 0); return d == 'C' ? 3u : d == 'D' ? 5u : 1u; }
FN(f_GetDriveTypeW) { int d = drive_of(w, a[0], 1); return d == 'C' ? 3u : d == 'D' ? 5u : 1u; }
FN(f_GetLogicalDrives) { UNUSED; return 0x0Cu; }
FN(f_GetLogicalDriveStringsA) {
    static const char s[] = "C:\\\0D:\\\0";
    uint32_t i;
    UNUSED;
    if (a[0] < sizeof(s)) return sizeof(s);
    for (i = 0; i < sizeof(s); i++) x86_wr8(C, a[1] + i, (uint8_t)s[i]);
    return sizeof(s) - 1;
}
FN(f_GetVolumeInformationA) {
    const char *label = getenv("QRX_CDLABEL");
    int d = drive_of(w, a[0], 0);
    if (!label) label = d == 'D' ? "QRXCD" : "QRX";
    if (a[1] && a[2]) put_str(w, a[1], a[2], label, 0);
    if (a[3]) WR32(a[3], 0x51525831u);
    if (a[4]) WR32(a[4], 255);
    if (a[5]) WR32(a[5], 0);
    if (a[6] && a[7]) put_str(w, a[6], a[7], d == 'D' ? "CDFS" : "FAT", 0);
    return 1;
}
FN(f_GetDiskFreeSpaceA) { UNUSED; if (a[1]) WR32(a[1], 8); if (a[2]) WR32(a[2], 512); if (a[3]) WR32(a[3], 200000); if (a[4]) WR32(a[4], 400000); return 1; }
FN(f_GetTempPathA) { init_root(w); return put_str(w, a[1], a[0], "C:\\", 0); }

/* FindFirstFile / FindNextFile */
static int wild(const char *p, const char *s)
{
    if (!strcmp(p, "*.*") || !strcmp(p, "*")) return 1;
    for (; *p; p++, s++) {
        if (*p == '*') { for (p++; ; s++) { if (wild(p, s)) return 1; if (!*s) return 0; } }
        if (!*s) return *p == '.' && p[1] == '*' && !p[2] ? 1 : 0;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
    }
    return !*s;
}
static int find_next(qwin *w, qfile *f, uint32_t out)
{
    struct dirent *e;
    while ((e = readdir(f->d))) {
        char full[1400];
        struct stat st;
        uint32_t i, n;
        if (!wild(f->pat, e->d_name)) continue;
        snprintf(full, sizeof(full), "%s/%s", f->dir, e->d_name);
        memset(&st, 0, sizeof(st));
        stat(full, &st);
        for (i = 0; i < 44; i += 4) WR32(out + i, 0);
        WR32(out, S_ISDIR(st.st_mode) ? 0x10u : 0x80u);
        WR32(out + 32, (uint32_t)st.st_size);
        n = (uint32_t)strlen(e->d_name);
        if (n > 259) n = 259;
        for (i = 0; i <= n; i++) {
            uint8_t ch = i < n ? (uint8_t)e->d_name[i] : 0;
            if (f->wide) x86_wr16(C, out + 44 + i * 2, ch); else x86_wr8(C, out + 44 + i, ch);
        }
        if (f->wide) x86_wr16(C, out + 44 + 520, 0); else x86_wr8(C, out + 44 + 260, 0);
        return 1;
    }
    return 0;
}
static uint32_t find_first(qwin *w, uint32_t pat, uint32_t out, int wide)
{
    char win[600], rel[1024], host[1200], *sl;
    int i = slot_new(QF_FIND);
    qfile *f;
    if (i < 0) return INVALID_HANDLE;
    f = &g_f[i];
    f->wide = wide;
    init_root(w);
    rd_path(w, pat, wide, win, sizeof(win));
    win_rel(win, rel, sizeof(rel));
    sl = strrchr(rel, '/');
    snprintf(f->pat, sizeof(f->pat), "%s", sl ? sl + 1 : rel);
    if (sl) *sl = 0; else rel[0] = 0;
    host_path(rel, host, sizeof(host));
    snprintf(f->dir, sizeof(f->dir), "%s", host);
    f->d = opendir(host);
    if (!f->d || !find_next(w, f, out)) { if (f->d) closedir(f->d); f->type = QF_FREE; w->last_error = 2; return INVALID_HANDLE; }
    return handle_of(i);
}
FN(f_FindFirstFileA) { return find_first(w, a[0], a[1], 0); }
FN(f_FindFirstFileW) { return find_first(w, a[0], a[1], 1); }
FN(f_FindNextFileA) { qfile *f = by_handle(a[0], QF_FIND); if (!f || !find_next(w, f, a[1])) { w->last_error = 18; return 0; } return 1; }
FN(f_FindClose) { return qw_file_close(w, a[0]); }

/* --------------------------------------------------------------- msvcrt streams */
static qfile *stream(uint32_t gfile)
{
    int i;
    if (!gfile) return NULL;
    for (i = 0; i < QF_MAX; i++) if (g_f[i].type == QF_FILE && g_f[i].gfile == gfile) return &g_f[i];
    return NULL;
}
static uint32_t do_fopen(qwin *w, const char *win, const char *mode)
{
    char host[1200], m[8];
    int i, k = 0;
    const char *p;
    FILE *fp;
    map_path(w, win, host, sizeof(host));
    for (p = mode; *p && k < 6; p++) if (*p != 't' && *p != 'b') m[k++] = *p;
    m[k++] = 'b'; m[k] = 0;
    fp = fopen(host, m);
    if (!fp) { if (w->verbose) qw_log(w, "fopen(%s, %s) -> %s: failed", win, mode, host); return 0; }
    i = slot_new(QF_FILE);
    if (i < 0) { fclose(fp); return 0; }
    g_f[i].f = fp;
    g_f[i].text = !strchr(mode, 'b');
    g_f[i].gfile = qw_alloc(w, 32);           /* opaque guest FILE (its fields are not used) */
    if (w->verbose) qw_log(w, "fopen(%s, %s) -> %s", win, mode, host);
    return g_f[i].gfile;
}
FN(f_fopen) { char n[600], m[16]; qw_rd_str(w, a[0], n, sizeof(n)); qw_rd_str(w, a[1], m, sizeof(m)); return do_fopen(w, n, m); }
FN(f_wfopen) { char n[600], m[16]; rd_path(w, a[0], 1, n, sizeof(n)); rd_path(w, a[1], 1, m, sizeof(m)); return do_fopen(w, n, m); }
FN(f_fclose) {
    qfile *f = stream(a[0]);
    if (!f) return 0;
    fclose(f->f);
    qw_free(w, f->gfile);
    f->type = QF_FREE;
    return 0;
}
FN(f_fread) {
    qfile *f = stream(a[3]);
    uint32_t total = a[1] * a[2], n = 0;
    uint8_t buf[4096];
    if (!f || !a[1]) return 0;
    while (n < total) {
        uint32_t chunk = total - n < sizeof(buf) ? total - n : (uint32_t)sizeof(buf);
        size_t got = fread(buf, 1, chunk, f->f);
        if (got) x86_write_mem(C, a[0] + n, buf, (uint32_t)got);
        n += (uint32_t)got;
        if (got < chunk) { f->eof = 1; break; }
    }
    return n / a[1];
}
FN(f_fseek) { qfile *f = stream(a[0]); UNUSED; if (!f) return 0xFFFFFFFFu; f->eof = 0; return fseek(f->f, (long)(int32_t)a[1], a[2] == 1 ? SEEK_CUR : a[2] == 2 ? SEEK_END : SEEK_SET) ? 0xFFFFFFFFu : 0; }
FN(f_ftell) { qfile *f = stream(a[0]); UNUSED; return f ? (uint32_t)ftell(f->f) : 0xFFFFFFFFu; }
FN(f_rewind) { qfile *f = stream(a[0]); UNUSED; if (f) { rewind(f->f); f->eof = 0; } return 0; }
FN(f_feof) { qfile *f = stream(a[0]); UNUSED; return f ? (uint32_t)(f->eof || feof(f->f)) : 0; }
FN(f_ferror) { qfile *f = stream(a[0]); UNUSED; return f ? (uint32_t)ferror(f->f) : 0; }
FN(f_clearerr) { qfile *f = stream(a[0]); UNUSED; if (f) { clearerr(f->f); f->eof = 0; } return 0; }
static int s_getc(qfile *f)
{
    int ch = fgetc(f->f);
    if (ch == '\r' && f->text) { int n = fgetc(f->f); if (n == '\n') ch = '\n'; else if (n != EOF) ungetc(n, f->f); }
    if (ch == EOF) f->eof = 1;
    return ch;
}
FN(f_fgetc) { qfile *f = stream(a[0]); UNUSED; if (!f) return 0xFFFFFFFFu; return (uint32_t)s_getc(f); }
FN(f_ungetc) { qfile *f = stream(a[1]); UNUSED; if (!f || (int32_t)a[0] == -1) return 0xFFFFFFFFu; f->eof = 0; return (uint32_t)ungetc((int)(a[0] & 0xFF), f->f); }
FN(f_fgets) {
    qfile *f = stream(a[2]);
    uint32_t i = 0;
    int ch = 0;
    if (!f || (int32_t)a[1] <= 0) return 0;
    while (i + 1 < a[1]) {
        ch = s_getc(f);
        if (ch == EOF) break;
        x86_wr8(C, a[0] + i++, (uint8_t)ch);
        if (ch == '\n') break;
    }
    x86_wr8(C, a[0] + i, 0);
    return (i == 0 && ch == EOF) ? 0 : a[0];
}
FN(f_fputs) { qfile *f = stream(a[1]); char b[4096]; qw_rd_str(w, a[0], b, sizeof(b)); fputs(b, f ? f->f : (a[1] ? stderr : stdout)); return 0; }
FN(f_setvbuf) { UNUSED; return 0; }
FN(f_fileno) { int i; UNUSED; for (i = 0; i < QF_MAX; i++) if (g_f[i].type == QF_FILE && g_f[i].gfile == a[0]) return (uint32_t)(i + 3); return 1; }

/* low-level descriptors: fd = slot + 3 (0..2 are the console) */
static qfile *fdf(uint32_t fd) { uint32_t i = fd - 3; return (fd >= 3 && i < QF_MAX && g_f[i].type == QF_FILE) ? &g_f[i] : NULL; }
static uint32_t do_open(qwin *w, const char *win, uint32_t flags)
{
    char host[1200];
    const char *m;
    struct stat st;
    int i, exists;
    FILE *fp;
    map_path(w, win, host, sizeof(host));
    exists = stat(host, &st) == 0;
    if ((flags & 0x0500u) == 0x0500u && exists) return 0xFFFFFFFFu;            /* O_CREAT|O_EXCL */
    if ((flags & 0x0200u) || (!exists && (flags & 0x0100u))) m = (flags & 3) == 0 ? "w+b" : "w+b";   /* O_TRUNC / create */
    else if (!exists) return 0xFFFFFFFFu;
    else m = (flags & 3) == 0 ? "rb" : "r+b";
    fp = fopen(host, m);
    if (!fp) return 0xFFFFFFFFu;
    if (flags & 8) fseek(fp, 0, SEEK_END);                                     /* O_APPEND */
    i = slot_new(QF_FILE);
    if (i < 0) { fclose(fp); return 0xFFFFFFFFu; }
    g_f[i].f = fp;
    return (uint32_t)(i + 3);
}
FN(f_open) { char n[600]; qw_rd_str(w, a[0], n, sizeof(n)); return do_open(w, n, a[1]); }
FN(f_wopen) { char n[600]; rd_path(w, a[0], 1, n, sizeof(n)); return do_open(w, n, a[1]); }
FN(f_read) {
    qfile *f = fdf(a[0]);
    uint8_t buf[4096];
    uint32_t n = 0;
    if (!f) return 0xFFFFFFFFu;
    while (n < a[2]) {
        uint32_t chunk = a[2] - n < sizeof(buf) ? a[2] - n : (uint32_t)sizeof(buf);
        size_t got = fread(buf, 1, chunk, f->f);
        if (got) x86_write_mem(C, a[1] + n, buf, (uint32_t)got);
        n += (uint32_t)got;
        if (got < chunk) break;
    }
    return n;
}
FN(f_write) {
    qfile *f = fdf(a[0]);
    uint32_t i;
    for (i = 0; i < a[2]; i++) fputc(x86_rd8(C, a[1] + i), f ? f->f : (a[0] == 2 ? stderr : stdout));
    return a[2];
}
FN(f_close) { qfile *f = fdf(a[0]); UNUSED; if (!f) return 0xFFFFFFFFu; fclose(f->f); f->type = QF_FREE; return 0; }
FN(f_lseek) { qfile *f = fdf(a[0]); UNUSED; if (!f) return 0xFFFFFFFFu; if (fseek(f->f, (long)(int32_t)a[1], a[2] == 1 ? SEEK_CUR : a[2] == 2 ? SEEK_END : SEEK_SET)) return 0xFFFFFFFFu; return (uint32_t)ftell(f->f); }
FN(f_tell) { qfile *f = fdf(a[0]); UNUSED; return f ? (uint32_t)ftell(f->f) : 0xFFFFFFFFu; }
FN(f_eof) { qfile *f = fdf(a[0]); long cur, end; UNUSED; if (!f) return 0xFFFFFFFFu; cur = ftell(f->f); fseek(f->f, 0, SEEK_END); end = ftell(f->f); fseek(f->f, cur, SEEK_SET); return cur >= end; }
FN(f_filelength) { qfile *f = fdf(a[0]); long cur, end; UNUSED; if (!f) return 0xFFFFFFFFu; cur = ftell(f->f); fseek(f->f, 0, SEEK_END); end = ftell(f->f); fseek(f->f, cur, SEEK_SET); return (uint32_t)end; }
FN(f_access) { char n[600], host[1200]; struct stat st; qw_rd_str(w, a[0], n, sizeof(n)); map_path(w, n, host, sizeof(host)); return stat(host, &st) == 0 ? 0 : 0xFFFFFFFFu; }
FN(f_remove) { return del_file(w, a[0], 0) ? 0 : 0xFFFFFFFFu; }
FN(f_rename) { uint32_t b[2]; b[0] = a[0]; b[1] = a[1]; return f_MoveFileA(w, b) ? 0 : 0xFFFFFFFFu; }
FN(f_mkdir) { return mk_dir(w, a[0], 0) ? 0 : 0xFFFFFFFFu; }
FN(f_chdir) { return set_cwd(w, a[0], 0) ? 0 : 0xFFFFFFFFu; }
FN(f_getcwd) {
    char b[600];
    init_root(w);
    full_of(g_cwd, b, sizeof(b));
    if (!a[0]) { uint32_t g = qw_alloc(w, (uint32_t)strlen(b) + 1); put_str(w, g, (uint32_t)strlen(b) + 1, b, 0); return g; }
    return put_str(w, a[0], a[1], b, 0) < a[1] ? a[0] : 0;
}
/* struct _stat (32-bit time): dev 0, ino 4, mode 6, nlink 8, uid 10, gid 12, rdev 16, size 20, atime 24, mtime 28, ctime 32 */
FN(f_stat) {
    char n[600], host[1200];
    struct stat st;
    uint32_t i;
    qw_rd_str(w, a[0], n, sizeof(n));
    map_path(w, n, host, sizeof(host));
    if (stat(host, &st)) return 0xFFFFFFFFu;
    for (i = 0; i < 36; i += 4) WR32(a[1] + i, 0);
    x86_wr16(C, a[1] + 6, S_ISDIR(st.st_mode) ? 0x41FFu : 0x81B6u);
    x86_wr16(C, a[1] + 8, 1);
    WR32(a[1] + 20, (uint32_t)st.st_size);
    WR32(a[1] + 24, (uint32_t)st.st_mtime); WR32(a[1] + 28, (uint32_t)st.st_mtime); WR32(a[1] + 32, (uint32_t)st.st_mtime);
    return 0;
}
/* struct _stati64: mode 6, nlink 8, size 24 (64-bit), atime 32, mtime 36, ctime 40 */
static void fill_stati64(qwin *w, uint32_t out, const struct stat *st)
{
    uint32_t i;
    for (i = 0; i < 48; i += 4) WR32(out + i, 0);
    x86_wr16(C, out + 6, S_ISDIR(st->st_mode) ? 0x41FFu : 0x81B6u);
    x86_wr16(C, out + 8, 1);
    WR32(out + 24, (uint32_t)st->st_size);
    WR32(out + 28, (uint32_t)((uint64_t)st->st_size >> 32));
    WR32(out + 32, (uint32_t)st->st_mtime); WR32(out + 36, (uint32_t)st->st_mtime); WR32(out + 40, (uint32_t)st->st_mtime);
}
FN(f_stati64) {
    char n[600], host[1200];
    struct stat st;
    qw_rd_str(w, a[0], n, sizeof(n));
    map_path(w, n, host, sizeof(host));
    if (stat(host, &st)) return 0xFFFFFFFFu;
    fill_stati64(w, a[1], &st);
    return 0;
}
FN(f_fstati64) {
    qfile *f = fdf(a[0]);
    struct stat st;
    if (!f || fstat(fileno(f->f), &st)) return 0xFFFFFFFFu;
    fill_stati64(w, a[1], &st);
    return 0;
}
/* _findfirst: struct _finddata_t: attrib 0, time_create 4, time_access 8, time_write 12, size 16, name 20 */
static int fd_conv(qwin *w, uint32_t tmp, uint32_t out)
{
    uint32_t i;
    WR32(out, RD32(tmp) & 0x10u ? 0x10u : 0x20u);
    WR32(out + 4, 0); WR32(out + 8, 0); WR32(out + 12, 0);
    WR32(out + 16, RD32(tmp + 32));
    for (i = 0; i < 260; i++) { uint8_t ch = x86_rd8(C, tmp + 44 + i); x86_wr8(C, out + 20 + i, ch); if (!ch) break; }
    return 1;
}
FN(f_findfirst) {
    uint32_t tmp = qw_alloc(w, 320), h = find_first(w, a[0], tmp, 0);
    if (h != INVALID_HANDLE) fd_conv(w, tmp, a[1]);
    qw_free(w, tmp);
    return h;
}
FN(f_findnext) {
    uint32_t tmp = qw_alloc(w, 320), r = 0xFFFFFFFFu;
    qfile *f = by_handle(a[0], QF_FIND);
    if (f && find_next(w, f, tmp)) { fd_conv(w, tmp, a[1]); r = 0; }
    qw_free(w, tmp);
    return r;
}
FN(f_findclose) { return qw_file_close(w, a[0]) ? 0 : 0xFFFFFFFFu; }

#define K(n, f, c) { "kernel32", n, c, f }
#define M(n, f) { "msvcrt", n, -1, f }
const qhle qw_files[] = {
    K("CreateFileA", f_CreateFileA, 7), K("CreateFileW", f_CreateFileW, 7), K("ReadFile", f_ReadFile, 5), K("WriteFile", f_WriteFile, 5),
    K("SetFilePointer", f_SetFilePointer, 4), K("GetFileSize", f_GetFileSize, 2), K("GetFileType", f_GetFileType, 1),
    K("FlushFileBuffers", f_FlushFileBuffers, 1), K("SetEndOfFile", f_SetEndOfFile, 1), K("GetFileTime", f_GetFileTime, 4),
    K("SearchPathA", f_SearchPathA, 6),
    K("GetFileAttributesA", f_GetFileAttributesA, 1), K("GetFileAttributesW", f_GetFileAttributesW, 1), K("SetFileAttributesA", f_SetFileAttributesA, 2),
    K("DeleteFileA", f_DeleteFileA, 1), K("DeleteFileW", f_DeleteFileW, 1), K("CreateDirectoryA", f_CreateDirectoryA, 2),
    K("CreateDirectoryW", f_CreateDirectoryW, 2), K("RemoveDirectoryA", f_RemoveDirectoryA, 1), K("MoveFileA", f_MoveFileA, 2),
    K("GetCurrentDirectoryA", f_GetCurrentDirectoryA, 2), K("GetCurrentDirectoryW", f_GetCurrentDirectoryW, 2),
    K("SetCurrentDirectoryA", f_SetCurrentDirectoryA, 1), K("SetCurrentDirectoryW", f_SetCurrentDirectoryW, 1),
    K("GetFullPathNameA", f_GetFullPathNameA, 4), K("GetFullPathNameW", f_GetFullPathNameW, 4),
    K("GetDriveTypeA", f_GetDriveTypeA, 1), K("GetDriveTypeW", f_GetDriveTypeW, 1), K("GetLogicalDrives", f_GetLogicalDrives, 0),
    K("GetLogicalDriveStringsA", f_GetLogicalDriveStringsA, 2), K("GetVolumeInformationA", f_GetVolumeInformationA, 8),
    K("GetDiskFreeSpaceA", f_GetDiskFreeSpaceA, 5), K("GetTempPathA", f_GetTempPathA, 2),
    K("FindFirstFileA", f_FindFirstFileA, 2), K("FindFirstFileW", f_FindFirstFileW, 2), K("FindNextFileA", f_FindNextFileA, 2),
    K("FindNextFileW", f_FindNextFileA, 2), K("FindClose", f_FindClose, 1),
    M("fopen", f_fopen), M("_wfopen", f_wfopen), M("fclose", f_fclose), M("fread", f_fread), M("fseek", f_fseek), M("ftell", f_ftell),
    M("rewind", f_rewind), M("feof", f_feof), M("ferror", f_ferror), M("clearerr", f_clearerr), M("fgetc", f_fgetc), M("getc", f_fgetc),
    M("ungetc", f_ungetc), M("fgets", f_fgets), M("fputs", f_fputs), M("setvbuf", f_setvbuf), M("_fileno", f_fileno),
    M("_open", f_open), M("_wopen", f_wopen), M("_read", f_read), M("_write", f_write), M("_close", f_close), M("_lseek", f_lseek),
    M("_tell", f_tell), M("_eof", f_eof), M("_filelength", f_filelength), M("_access", f_access), M("remove", f_remove),
    M("_unlink", f_remove), M("rename", f_rename), M("_mkdir", f_mkdir), M("_chdir", f_chdir), M("_getcwd", f_getcwd),
    M("_stati64", f_stati64), M("_fstati64", f_fstati64),
    M("_stat", f_stat), M("_findfirst", f_findfirst), M("_findnext", f_findnext), M("_findclose", f_findclose),
    { NULL, NULL, 0, NULL }
};
