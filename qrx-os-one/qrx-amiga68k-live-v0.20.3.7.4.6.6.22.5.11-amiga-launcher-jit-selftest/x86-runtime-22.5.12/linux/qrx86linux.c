/* QRX OS One 22.5.12 - Linux i386 personality (see qrx86linux.h). */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include "qrx86linux.h"

/* ---------------------------------------------------------------- Linux ABI constants */
#define L_EPERM 1
#define L_ENOENT 2
#define L_EINTR 4
#define L_EIO 5
#define L_ENOEXEC 8
#define L_EBADF 9
#define L_ECHILD 10
#define L_EAGAIN 11
#define L_ENOMEM 12
#define L_EACCES 13
#define L_EFAULT 14
#define L_EEXIST 17
#define L_EXDEV 18
#define L_ENOTDIR 20
#define L_EISDIR 21
#define L_EINVAL 22
#define L_EMFILE 24
#define L_ENOTTY 25
#define L_ENOSPC 28
#define L_ESPIPE 29
#define L_EROFS 30
#define L_EPIPE 32
#define L_ERANGE 34
#define L_ENAMETOOLONG 36
#define L_ENOSYS 38
#define L_ENOTEMPTY 39
#define L_ELOOP 40

#define LX_MAXFD 256
#define LX_STACK_TOP  0xBFFFF000u
#define LX_STACK_SIZE (2u * 1024u * 1024u)
#define LX_MMAP_BASE  0x40000000u
#define LX_MMAP_END   0xB0000000u
#define LX_EXEC_BIAS  0x56555000u    /* ET_DYN main program */
#define LX_INTERP_BIAS 0xB7E00000u   /* program interpreter (ld-linux.so.2) */

enum { F_HOST = 1, F_DIR, F_PIPE_R, F_PIPE_W, F_NULL };

typedef struct lxpipe { uint8_t *buf; size_t len, cap, rpos; int readers, writers; } lxpipe;

typedef struct lxfile {
    int refs, kind, hfd, flags;
    DIR *dir;
    char *path;              /* host path for directories */
    lxpipe *pipe;
    struct dirent *pend;     /* readdir entry that did not fit into the last getdents buffer */
    uint32_t dpos;
} lxfile;

typedef struct lxzombie { int pid, status; struct lxzombie *next; } lxzombie;
typedef struct lxresv { uint32_t a, b; struct lxresv *next; } lxresv;

typedef struct lxproc {
    x86cpu cpu;
    const lxopts *o;
    lxfile *fd[LX_MAXFD];
    unsigned char cloexec[LX_MAXFD];
    char cwd[1024];
    char exe[1024];           /* guest path of the running image */
    uint32_t brk_start, brk_cur, mmap_next;
    int pid, ppid, exited, status;
    lxzombie *zombies;
    lxresv *resv;             /* PROT_NONE reservations (not backed by memory) */
    uint32_t umask_v;
    int blocked;              /* last syscall would block: re-executed on the next slice */
    struct lxproc *next;
} lxproc;

static int g_next_pid = 1000;
static lxproc *g_procs;       /* all live processes of the current lx_run session */
static int g_deadlock;        /* every process blocked: pipes read EOF, waits fail */
#define LX_SLICE 200000u      /* instructions per scheduling slice */
#define LX_PIPE_MAX 65536u

/* ---------------------------------------------------------------- helpers */
static int lerr(int e)
{
    switch (e) {
    case EPERM: return L_EPERM;
    case ENOENT: return L_ENOENT;
    case EINTR: return L_EINTR;
    case EIO: return L_EIO;
    case EBADF: return L_EBADF;
#ifdef ECHILD
    case ECHILD: return L_ECHILD;
#endif
    case EAGAIN: return L_EAGAIN;
    case ENOMEM: return L_ENOMEM;
    case EACCES: return L_EACCES;
#ifdef EFAULT
    case EFAULT: return L_EFAULT;
#endif
    case EEXIST: return L_EEXIST;
#ifdef EXDEV
    case EXDEV: return L_EXDEV;
#endif
    case ENOTDIR: return L_ENOTDIR;
    case EISDIR: return L_EISDIR;
    case EINVAL: return L_EINVAL;
    case EMFILE: return L_EMFILE;
#ifdef ENOTTY
    case ENOTTY: return L_ENOTTY;
#endif
    case ENOSPC: return L_ENOSPC;
#ifdef ESPIPE
    case ESPIPE: return L_ESPIPE;
#endif
#ifdef EROFS
    case EROFS: return L_EROFS;
#endif
    case EPIPE: return L_EPIPE;
    case ERANGE: return L_ERANGE;
#ifdef ENAMETOOLONG
    case ENAMETOOLONG: return L_ENAMETOOLONG;
#endif
#ifdef ENOTEMPTY
    case ENOTEMPTY: return L_ENOTEMPTY;
#endif
#ifdef ELOOP
    case ELOOP: return L_ELOOP;
#endif
    default: return L_EIO;
    }
}
#define NEG(e) ((uint32_t)-(int32_t)(e))
#define HERR() NEG(lerr(errno))

static int gstr(lxproc *p, uint32_t a, char *buf, size_t n)
{
    size_t i;
    if (!a) return 0;
    for (i = 0; i + 1 < n; i++) {
        uint8_t *h = x86_host_ptr(&p->cpu, a + (uint32_t)i);
        if (!h) return 0;
        buf[i] = (char)*h;
        if (!buf[i]) return 1;
    }
    buf[n - 1] = 0;
    return 1;
}

static void normalize(char *out, size_t n, const char *cwd, const char *path)
{
    char tmp[2048], *seg, *save = NULL;
    size_t len = 0;
    if (path[0] == '/') snprintf(tmp, sizeof(tmp), "%s", path);
    else snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);
    out[0] = 0;
    for (seg = strtok_r(tmp, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
        if (!strcmp(seg, ".")) continue;
        if (!strcmp(seg, "..")) { char *s = strrchr(out, '/'); if (s) { *s = 0; len = (size_t)(s - out); } continue; }
        if (len + strlen(seg) + 2 >= n) break;
        out[len++] = '/'; strcpy(out + len, seg); len += strlen(seg);
    }
    if (!out[0]) strcpy(out, "/");
}

/* join a host directory and a name (AmigaDOS: no '/' after "VOL:") */
static void hjoin(char *out, size_t n, const char *dir, const char *name)
{
    size_t L = strlen(dir);
    if (!L) snprintf(out, n, "%s", name);
    else if (dir[L - 1] == '/' || dir[L - 1] == ':') snprintf(out, n, "%s%s", dir, name);
    else snprintf(out, n, "%s/%s", dir, name);
}

/* absolute guest path -> host path.
 * POSIX hosts: identical (or below ROOT).
 * AROS: posixc uses AmigaDOS paths, so "/VOL/a/b" <-> "VOL:a/b"; with ROOT
 * (an AmigaDOS directory) "/a/b" -> "ROOT/a/b". */
static void guest_to_host(const lxopts *o, const char *ab, char *h, size_t n)
{
#ifdef __AROS__
    if (o->root && o->root[0]) { if (!ab[1]) snprintf(h, n, "%s", o->root); else hjoin(h, n, o->root, ab + 1); return; }
    {
        const char *v = ab + 1, *sl = strchr(v, '/');
        if (!*v) { snprintf(h, n, "SYS:"); return; }
        if (!sl) snprintf(h, n, "%s:", v);
        else snprintf(h, n, "%.*s:%s", (int)(sl - v), v, sl + 1);
    }
#else
    if (o->root && o->root[0]) snprintf(h, n, "%s%s", o->root, strcmp(ab, "/") ? ab : "/");
    else snprintf(h, n, "%s", ab);
#endif
}

/* host path (as returned by getcwd or stored for directories) -> absolute guest path */
static void host_to_guest(const lxopts *o, const char *h, char *g, size_t n)
{
#ifdef __AROS__
    const char *c = strchr(h, ':');
    if (o->root && o->root[0]) {
        size_t L = strlen(o->root);
        if (!strncmp(h, o->root, L)) { const char *r = h + L; while (*r == '/') r++; snprintf(g, n, "/%s", r); return; }
        snprintf(g, n, "/");
        return;
    }
    if (c) { if (c[1]) snprintf(g, n, "/%.*s/%s", (int)(c - h), h, c + 1); else snprintf(g, n, "/%.*s", (int)(c - h), h); }
    else snprintf(g, n, "/%s", h);
#else
    if (o->root && o->root[0] && !strncmp(h, o->root, strlen(o->root))) snprintf(g, n, "%s", h[strlen(o->root)] ? h + strlen(o->root) : "/");
    else snprintf(g, n, "%s", h);
#endif
}

/* guest path -> host path */
static void hostpath(lxproc *p, const char *g, char *h, size_t n)
{
    char ab[1536];
    normalize(ab, sizeof(ab), p->cwd, g);
    if (!strcmp(ab, "/proc/self/exe")) normalize(ab, sizeof(ab), "/", p->exe);
    guest_to_host(p->o, ab, h, n);
}

static lxfile *fnew(int kind)
{
    lxfile *f = (lxfile *)calloc(1, sizeof(lxfile));
    if (f) { f->refs = 1; f->kind = kind; f->hfd = -1; }
    return f;
}
static void fput(lxfile *f)
{
    if (!f || --f->refs > 0) return;
    if (f->kind == F_HOST && f->hfd > 2) close(f->hfd);
    if (f->kind == F_DIR && f->dir) closedir(f->dir);
    if (f->kind == F_PIPE_R && f->pipe) f->pipe->readers--;
    if (f->kind == F_PIPE_W && f->pipe) f->pipe->writers--;
    if (f->pipe && f->pipe->readers <= 0 && f->pipe->writers <= 0) { free(f->pipe->buf); free(f->pipe); }
    free(f->path);
    free(f);
}
static int falloc(lxproc *p, int from, lxfile *f)
{
    int i;
    for (i = from; i < LX_MAXFD; i++) if (!p->fd[i]) { p->fd[i] = f; p->cloexec[i] = 0; return i; }
    return -1;
}
static lxfile *fget(lxproc *p, uint32_t fd) { return fd < LX_MAXFD ? p->fd[fd] : NULL; }

/* ---------------------------------------------------------------- memory */
static int resv_hit(lxproc *p, uint32_t a, uint32_t b)
{
    lxresv *r;
    for (r = p->resv; r; r = r->next) if (a < r->b && b > r->a) return 1;
    return 0;
}
static void resv_drop(lxproc *p, uint32_t a, uint32_t b)
{
    lxresv **pp = &p->resv;
    while (*pp) {
        lxresv *r = *pp;
        if (a <= r->a && b >= r->b) { *pp = r->next; free(r); continue; }
        if (a > r->a && b < r->b) { lxresv *n = (lxresv *)malloc(sizeof(*n)); if (n) { n->a = b; n->b = r->b; n->next = r->next; r->next = n; } r->b = a; }
        else if (a <= r->a && b > r->a) r->a = b;
        else if (a < r->b && b >= r->b) r->b = a;
        pp = &r->next;
    }
}
static int range_free(lxproc *p, uint32_t a, uint32_t len)
{
    uint32_t x;
    if (resv_hit(p, a, a + len)) return 0;
    for (x = a; x - a < len; x += X86_PAGE_SIZE) if (x86_is_mapped(&p->cpu, x)) return 0;
    return 1;
}
static uint32_t find_space(lxproc *p, uint32_t len)
{
    uint32_t a = p->mmap_next;
    while ((uint64_t)a + len <= LX_MMAP_END) {
        if (range_free(p, a, len)) { p->mmap_next = a + len; return a; }
        a += X86_PAGE_SIZE;
    }
    for (a = LX_MMAP_BASE; (uint64_t)a + len <= LX_MMAP_END; a += X86_PAGE_SIZE)
        if (range_free(p, a, len)) return a;
    return 0;
}

/* ---------------------------------------------------------------- ELF loader */
typedef struct { uint32_t entry, phdr, phnum, base, end; } lximage;

static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t le32(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;
    long sz;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        b = (uint8_t *)malloc((size_t)sz + 1);
        if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
        if (b) *n = (size_t)sz;
    }
    fclose(f);
    return b;
}

static const char *load_elf(lxproc *p, const uint8_t *b, size_t n, uint32_t bias_dyn, lximage *im, char *interp, size_t interp_n)
{
    uint32_t phoff, i, lo = 0xFFFFFFFFu, hi = 0, bias = 0;
    uint16_t type, phnum, phentsz;
    if (n < 52 || memcmp(b, "\177ELF", 4)) return "not an ELF file";
    if (b[4] != 1 || b[5] != 1) return "not a 32-bit little-endian ELF";
    type = le16(b + 16);
    if (le16(b + 18) != 3) return "not an i386 ELF";
    if (type != 2 && type != 3) return "not an executable ELF";
    phoff = le32(b + 28); phentsz = le16(b + 42); phnum = le16(b + 44);
    if (phentsz != 32 || (uint64_t)phoff + (uint64_t)phnum * 32 > n) return "bad program headers";
    if (type == 3) bias = bias_dyn;
    if (interp) interp[0] = 0;
    im->phdr = 0;
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = b + phoff + i * 32;
        uint32_t pt = le32(ph), off = le32(ph + 4), va = le32(ph + 8), fsz = le32(ph + 16), msz = le32(ph + 20);
        if (pt == 3 && interp) {  /* PT_INTERP */
            size_t k = fsz < interp_n ? fsz : interp_n - 1;
            if ((uint64_t)off + fsz > n) return "bad PT_INTERP";
            memcpy(interp, b + off, k); interp[k] = 0;
        }
        if (pt == 6) im->phdr = va + bias;   /* PT_PHDR */
        if (pt != 1) continue;
        if ((uint64_t)off + fsz > n || fsz > msz) return "bad PT_LOAD";
        if (va + bias < lo) lo = va + bias;
        if (va + bias + msz > hi) hi = va + bias + msz;
        if (!im->phdr && off <= phoff && phoff < off + fsz) im->phdr = va + bias + (phoff - off);
    }
    if (lo >= hi) return "no loadable segments";
    if (!x86_map(&p->cpu, lo, hi - lo, 1)) return "out of memory";
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = b + phoff + i * 32;
        uint32_t pt = le32(ph), off = le32(ph + 4), va = le32(ph + 8), fsz = le32(ph + 16);
        if (pt == 1 && fsz) x86_write_mem(&p->cpu, va + bias, b + off, fsz);
    }
    /* read-only text pages (pages shared with a writable segment stay writable) */
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = b + phoff + i * 32;
        uint32_t va = le32(ph + 8) + bias, msz = le32(ph + 20), fl = le32(ph + 24);
        if (le32(ph) == 1 && !(fl & 2)) x86_protect(&p->cpu, va, msz, 0);
    }
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = b + phoff + i * 32;
        uint32_t va = le32(ph + 8) + bias, msz = le32(ph + 20), fl = le32(ph + 24);
        if (le32(ph) == 1 && (fl & 2)) x86_protect(&p->cpu, va, msz, 1);
    }
    im->entry = le32(b + 24) + bias;
    im->phnum = phnum;
    im->base = bias;
    im->end = hi;
    return NULL;
}

static int lx_int(x86cpu *c, int vec);

/* Build a fresh address space for 'gpath'. On error returns message, process untouched only
 * when the header check failed (caller decides). */
static const char *exec_image(lxproc *p, const char *gpath, int argc, char **argv, int envc, char **envp)
{
    char hp[1600], interp[256], ihp[1600];
    size_t n = 0, in = 0;
    uint8_t *b, *ib = NULL;
    lximage im, ii;
    const char *err;
    uint32_t sp, strs[512], envs[512], rnd, execfn, plat, a;
    int i, k;
    lxresv *r;

    hostpath(p, gpath, hp, sizeof(hp));
    b = slurp(hp, &n);
    if (!b) return "cannot read program";
    if (n < 4 || memcmp(b, "\177ELF", 4)) { free(b); return "not an ELF file"; }
    /* destroy the old image */
    {
        /* execve runs inside x86_run: keep the run-loop context (jmp_buf, counters) */
        x86_int_fn ih = p->cpu.int_hook; void *u = p->cpu.user; uint64_t bud = p->cpu.budget, ic = p->cpu.icount;
        jmp_buf *jb = p->cpu.jb; int dep = p->cpu.depth;
        x86_free(&p->cpu);
        x86_init(&p->cpu);
        p->cpu.int_hook = ih; p->cpu.user = u; p->cpu.budget = bud; p->cpu.icount = ic; p->cpu.jb = jb; p->cpu.depth = dep;
    }
    while ((r = p->resv)) { p->resv = r->next; free(r); }
    p->mmap_next = LX_MMAP_BASE;
    memset(&im, 0, sizeof(im)); memset(&ii, 0, sizeof(ii));
    err = load_elf(p, b, n, LX_EXEC_BIAS, &im, interp, sizeof(interp));
    free(b);
    if (err) return err;
    if (interp[0]) {
        hostpath(p, interp, ihp, sizeof(ihp));
        ib = slurp(ihp, &in);
        if (!ib) return "cannot read program interpreter";
        err = load_elf(p, ib, in, LX_INTERP_BIAS, &ii, NULL, 0);
        free(ib);
        if (err) return err;
    }
    {   /* re-exec through /proc/self/exe keeps the image name (BusyBox standalone applets) */
        char ab[1024];
        normalize(ab, sizeof(ab), p->cwd, gpath);
        if (strcmp(ab, "/proc/self/exe") || !p->exe[0]) snprintf(p->exe, sizeof(p->exe), "%s", ab);
    }
    p->brk_start = p->brk_cur = (im.end + X86_PAGE_MASK) & ~X86_PAGE_MASK;
    /* stack */
    if (!x86_map(&p->cpu, LX_STACK_TOP - LX_STACK_SIZE, LX_STACK_SIZE, 1)) return "out of memory (stack)";
    sp = LX_STACK_TOP - 16;
    if (argc > 511) argc = 511;
    if (envc > 511) envc = 511;
#define PUTSTR(s, dst) do { size_t L_ = strlen(s) + 1; sp -= (uint32_t)L_; x86_write_mem(&p->cpu, sp, (s), (uint32_t)L_); (dst) = sp; } while (0)
    PUTSTR(gpath, execfn);
    PUTSTR("i686", plat);
    for (i = 0; i < envc; i++) PUTSTR(envp[i], envs[i]);
    for (i = 0; i < argc; i++) PUTSTR(argv[i], strs[i]);
    sp -= 16; rnd = sp;
    for (i = 0; i < 16; i++) x86_wr8(&p->cpu, rnd + (uint32_t)i, (uint8_t)(rand() >> 3));
    sp &= ~15u;
    k = 1 + argc + 1 + envc + 1 + 2 * 17;
    sp -= (uint32_t)k * 4;
    sp &= ~15u;
    a = sp;
    x86_wr32(&p->cpu, a, (uint32_t)argc); a += 4;
    for (i = 0; i < argc; i++) { x86_wr32(&p->cpu, a, strs[i]); a += 4; }
    x86_wr32(&p->cpu, a, 0); a += 4;
    for (i = 0; i < envc; i++) { x86_wr32(&p->cpu, a, envs[i]); a += 4; }
    x86_wr32(&p->cpu, a, 0); a += 4;
#define AUX(t, v) do { x86_wr32(&p->cpu, a, (t)); x86_wr32(&p->cpu, a + 4, (v)); a += 8; } while (0)
    AUX(3, im.phdr); AUX(4, 32); AUX(5, im.phnum); AUX(6, 4096); AUX(7, interp[0] ? ii.base : 0);
    AUX(8, 0); AUX(9, im.entry); AUX(11, 1000); AUX(12, 1000); AUX(13, 1000); AUX(14, 1000);
    AUX(15, plat); AUX(16, 0x00008111u); AUX(17, 100); AUX(23, 0); AUX(25, rnd); AUX(31, execfn);
    AUX(0, 0);
#undef AUX
#undef PUTSTR
    p->cpu.r[X86_ESP] = sp;
    p->cpu.eip = interp[0] ? ii.entry : im.entry;
    return NULL;
}

/* ---------------------------------------------------------------- stat translation */
static uint32_t lmode(const struct stat *st)
{
    uint32_t m = (uint32_t)st->st_mode & 07777u;
    if (S_ISDIR(st->st_mode)) m |= 0040000u;
    else if (S_ISREG(st->st_mode)) m |= 0100000u;
#ifdef S_ISLNK
    else if (S_ISLNK(st->st_mode)) m |= 0120000u;
#endif
    else if (S_ISCHR(st->st_mode)) m |= 0020000u;
#ifdef S_ISFIFO
    else if (S_ISFIFO(st->st_mode)) m |= 0010000u;
#endif
    else m |= 0100000u;
    return m;
}
static void put_stat64(lxproc *p, uint32_t a, const struct stat *st)
{
    uint8_t b[96];
    uint64_t ino = (uint64_t)st->st_ino, sz = (uint64_t)st->st_size, blocks = ((uint64_t)st->st_size + 511) / 512;
    uint32_t i;
    memset(b, 0, sizeof(b));
#define W32(o, v) do { uint32_t v_ = (uint32_t)(v); b[o] = (uint8_t)v_; b[(o) + 1] = (uint8_t)(v_ >> 8); b[(o) + 2] = (uint8_t)(v_ >> 16); b[(o) + 3] = (uint8_t)(v_ >> 24); } while (0)
#define W64(o, v) do { uint64_t w_ = (uint64_t)(v); W32(o, w_); W32((o) + 4, w_ >> 32); } while (0)
    W64(0, (uint64_t)st->st_dev); W32(12, ino); W32(16, lmode(st)); W32(20, st->st_nlink);
    W32(24, st->st_uid); W32(28, st->st_gid); W64(44, sz); W32(52, 4096); W64(56, blocks);
    W32(64, st->st_atime); W32(72, st->st_mtime); W32(80, st->st_ctime); W64(88, ino);
    for (i = 0; i < 96; i++) x86_wr8(&p->cpu, a + i, b[i]);
}
static void put_statx(lxproc *p, uint32_t a, const struct stat *st)
{
    uint8_t b[256];
    uint32_t i;
    memset(b, 0, sizeof(b));
    W32(0, 0x7FFu); W32(4, 4096); W32(16, st->st_nlink); W32(20, st->st_uid); W32(24, st->st_gid);
    b[28] = (uint8_t)lmode(st); b[29] = (uint8_t)(lmode(st) >> 8);
    W64(32, (uint64_t)st->st_ino); W64(40, (uint64_t)st->st_size); W64(48, ((uint64_t)st->st_size + 511) / 512);
    W64(64, (int64_t)st->st_atime); W64(96, (int64_t)st->st_ctime); W64(112, (int64_t)st->st_mtime); W64(80, (int64_t)st->st_mtime);
    W32(136, (uint32_t)(st->st_dev >> 8)); W32(140, (uint32_t)(st->st_dev & 0xFF));
    for (i = 0; i < 256; i++) x86_wr8(&p->cpu, a + i, b[i]);
}

/* ---------------------------------------------------------------- open */
static uint32_t do_open(lxproc *p, const char *g, uint32_t fl, uint32_t mode)
{
    char hp[1600], ab[1536];
    struct stat st;
    int hf, hfl = 0, fd;
    lxfile *f;
    normalize(ab, sizeof(ab), p->cwd, g);
    if (!strcmp(ab, "/dev/null") || !strcmp(ab, "/dev/zero")) {
        f = fnew(F_NULL); if (!f) return NEG(L_ENOMEM);
        f->flags = !strcmp(ab, "/dev/zero");
        fd = falloc(p, 0, f); if (fd < 0) { fput(f); return NEG(L_EMFILE); }
        return (uint32_t)fd;
    }
    if (!strcmp(ab, "/dev/tty") || !strcmp(ab, "/dev/stdout") || !strcmp(ab, "/dev/stderr") || !strcmp(ab, "/dev/stdin")) {
        int h = !strcmp(ab, "/dev/stdin") ? 0 : !strcmp(ab, "/dev/stderr") ? 2 : 1;
        f = fnew(F_HOST); if (!f) return NEG(L_ENOMEM);
        f->hfd = h; fd = falloc(p, 0, f); if (fd < 0) { fput(f); return NEG(L_EMFILE); }
        return (uint32_t)fd;
    }
    hostpath(p, g, hp, sizeof(hp));
    if (stat(hp, &st) == 0 && S_ISDIR(st.st_mode)) {
        if ((fl & 3) != 0) return NEG(L_EISDIR);
        f = fnew(F_DIR); if (!f) return NEG(L_ENOMEM);
        f->dir = opendir(hp);
        if (!f->dir) { int e = errno; fput(f); return NEG(lerr(e)); }
        f->path = strdup(hp);
    } else {
        if (fl & 0200000u) return NEG(L_ENOTDIR);
        switch (fl & 3) { case 0: hfl = O_RDONLY; break; case 1: hfl = O_WRONLY; break; default: hfl = O_RDWR; }
        if (fl & 0100u) hfl |= O_CREAT;
        if (fl & 0200u) hfl |= O_EXCL;
        if (fl & 01000u) hfl |= O_TRUNC;
        if (fl & 02000u) hfl |= O_APPEND;
        hf = open(hp, hfl, (mode_t)(mode & 0777u & ~p->umask_v));
        if (hf < 0) return HERR();
        f = fnew(F_HOST); if (!f) { close(hf); return NEG(L_ENOMEM); }
        f->hfd = hf;
    }
    fd = falloc(p, 0, f);
    if (fd < 0) { fput(f); return NEG(L_EMFILE); }
    if (fl & 02000000u) p->cloexec[fd] = 1;
    return (uint32_t)fd;
}

/* ---------------------------------------------------------------- getdents */
static uint32_t do_getdents(lxproc *p, lxfile *f, uint32_t buf, uint32_t cnt, int is64)
{
    uint32_t used = 0;
    if (!f || f->kind != F_DIR) return NEG(L_ENOTDIR);
    for (;;) {
        struct dirent *de = f->pend ? f->pend : readdir(f->dir);
        uint32_t nl, rl, o;
        uint8_t dtype = 0;
        char full[2200];
        struct stat st;
        f->pend = NULL;
        if (!de) break;
        nl = (uint32_t)strlen(de->d_name);
        rl = is64 ? ((19 + nl + 1 + 7) & ~7u) : ((10 + nl + 2 + 3) & ~3u);
        if (used + rl > cnt) { f->pend = de; if (!used) return NEG(L_EINVAL); break; }
        hjoin(full, sizeof(full), f->path, de->d_name);
        if (lstat(full, &st) == 0) {
            uint32_t m = lmode(&st) & 0170000u;
            dtype = (uint8_t)(m >> 12);
        }
        f->dpos++;
        o = buf + used;
        for (nl = 0; nl < rl; nl++) x86_wr8(&p->cpu, o + nl, 0);
        nl = (uint32_t)strlen(de->d_name);
        if (is64) {
            x86_wr32(&p->cpu, o, f->dpos + 2); x86_wr32(&p->cpu, o + 8, f->dpos); x86_wr16(&p->cpu, o + 16, (uint16_t)rl);
            x86_wr8(&p->cpu, o + 18, dtype); x86_write_mem(&p->cpu, o + 19, de->d_name, nl);
        } else {
            x86_wr32(&p->cpu, o, f->dpos + 2); x86_wr32(&p->cpu, o + 4, f->dpos); x86_wr16(&p->cpu, o + 8, (uint16_t)rl);
            x86_write_mem(&p->cpu, o + 10, de->d_name, nl); x86_wr8(&p->cpu, o + rl - 1, dtype);
        }
        used += rl;
    }
    return used;
}

static void proc_exit(struct lxproc *p, int status);

/* ---------------------------------------------------------------- read/write */
static uint32_t do_read(lxproc *p, lxfile *f, uint32_t buf, uint32_t n)
{
    uint32_t done = 0;
    if (!f) return NEG(L_EBADF);
    if (f->kind == F_DIR) return NEG(L_EISDIR);
    if (f->kind == F_NULL) {
        if (!f->flags) return 0;
        while (done < n) { if (!x86_wr8(&p->cpu, buf + done, 0)) return NEG(L_EFAULT); done++; }
        return n;
    }
    if (f->kind == F_PIPE_R) {
        lxpipe *pp = f->pipe;
        uint32_t k = (uint32_t)(pp->len - pp->rpos);
        if (!k && pp->writers > 0 && !g_deadlock) { p->blocked = 1; return 0; }
        if (k > n) k = n;
        if (!x86_write_mem(&p->cpu, buf, pp->buf + pp->rpos, k)) return NEG(L_EFAULT);
        pp->rpos += k;
        if (pp->rpos == pp->len) pp->rpos = pp->len = 0;
        return k;      /* empty pipe without writers: EOF */
    }
    if (f->kind != F_HOST) return NEG(L_EBADF);
    while (done < n) {
        uint8_t *h = x86_host_ptr(&p->cpu, buf + done);
        uint32_t k = X86_PAGE_SIZE - ((buf + done) & X86_PAGE_MASK);
        ssize_t r;
        if (!h) return done ? done : NEG(L_EFAULT);
        if (k > n - done) k = n - done;
        r = read(f->hfd, h, k);
        if (r < 0) return done ? done : HERR();
        done += (uint32_t)r;
        if ((uint32_t)r < k) break;
        if (isatty(f->hfd)) break;
    }
    return done;
}

static uint32_t do_write(lxproc *p, lxfile *f, uint32_t buf, uint32_t n)
{
    uint32_t done = 0;
    if (!f) return NEG(L_EBADF);
    if (f->kind == F_NULL) return n;
    if (f->kind == F_PIPE_W) {
        lxpipe *pp = f->pipe;
        if (pp->readers <= 0) { proc_exit(p, 13); return NEG(L_EPIPE); }      /* SIGPIPE */
        if (pp->rpos) { memmove(pp->buf, pp->buf + pp->rpos, pp->len - pp->rpos); pp->len -= pp->rpos; pp->rpos = 0; }
        if (pp->len >= LX_PIPE_MAX) { if (!g_deadlock) { p->blocked = 1; return 0; } return NEG(L_EAGAIN); }
        if (n > LX_PIPE_MAX - pp->len) n = (uint32_t)(LX_PIPE_MAX - pp->len);
        if (pp->len + n > pp->cap) {
            size_t nc = pp->cap ? pp->cap : 4096;
            uint8_t *nb;
            while (nc < pp->len + n) nc *= 2;
            nb = (uint8_t *)realloc(pp->buf, nc);
            if (!nb) return NEG(L_ENOMEM);
            pp->buf = nb; pp->cap = nc;
        }
        if (!x86_read_mem(&p->cpu, buf, pp->buf + pp->len, n)) return NEG(L_EFAULT);
        pp->len += n;
        return n;
    }
    if (f->kind != F_HOST) return NEG(L_EBADF);
    while (done < n) {
        uint8_t *h = x86_host_ptr(&p->cpu, buf + done);
        uint32_t k = X86_PAGE_SIZE - ((buf + done) & X86_PAGE_MASK);
        ssize_t r;
        if (!h) return done ? done : NEG(L_EFAULT);
        if (k > n - done) k = n - done;
        r = write(f->hfd, h, k);
        if (r < 0) return done ? done : HERR();
        done += (uint32_t)r;
        if ((uint32_t)r < k) break;
    }
    return done;
}

/* ---------------------------------------------------------------- process model */
static lxproc *proc_new(const lxopts *o)
{
    lxproc *p = (lxproc *)calloc(1, sizeof(lxproc));
    if (!p) return NULL;
    x86_init(&p->cpu);
    p->o = o;
    p->cpu.user = p;
    p->cpu.int_hook = lx_int;
    p->cpu.budget = o->budget;
    p->pid = g_next_pid++;
    p->umask_v = 022;
    strcpy(p->cwd, "/");
    p->mmap_next = LX_MMAP_BASE;
    return p;
}
static void proc_free(lxproc *p)
{
    int i;
    lxzombie *z;
    lxresv *r;
    for (i = 0; i < LX_MAXFD; i++) if (p->fd[i]) fput(p->fd[i]);
    while ((z = p->zombies)) { p->zombies = z->next; free(z); }
    while ((r = p->resv)) { p->resv = r->next; free(r); }
    x86_free(&p->cpu);
    free(p);
}

static lxproc *proc_fork(lxproc *p)
{
    lxproc *c = proc_new(p->o);
    int i, j;
    lxresv *r, **tail;
    if (!c) return NULL;
    {   /* copy CPU state but not the page directory pointers */
        x86page *keep[1024];
        memcpy(keep, c->cpu.dir, sizeof(keep));
        c->cpu = p->cpu;
        memcpy(c->cpu.dir, keep, sizeof(keep));
        c->cpu.user = c; c->cpu.jb = NULL; c->cpu.depth = 0; c->cpu.stop = 0; c->cpu.fault = 0;
    }
    for (i = 0; i < 1024; i++) {
        if (!p->cpu.dir[i]) continue;
        for (j = 0; j < 1024; j++) {
            x86page *s = &p->cpu.dir[i][j];
            uint32_t a = ((uint32_t)i << 22) | ((uint32_t)j << 12);
            if (!s->mem) continue;
            if (!x86_map(&c->cpu, a, X86_PAGE_SIZE, 1)) { proc_free(c); return NULL; }
            memcpy(x86_host_ptr(&c->cpu, a), s->mem, X86_PAGE_SIZE);
            c->cpu.dir[i][j].flags = s->flags;
        }
    }
    for (i = 0; i < LX_MAXFD; i++) if (p->fd[i]) { c->fd[i] = p->fd[i]; c->fd[i]->refs++; c->cloexec[i] = p->cloexec[i]; }
    tail = &c->resv;
    for (r = p->resv; r; r = r->next) { lxresv *n = (lxresv *)malloc(sizeof(*n)); if (!n) break; *n = *r; n->next = NULL; *tail = n; tail = &n->next; }
    memcpy(c->cwd, p->cwd, sizeof(c->cwd));
    memcpy(c->exe, p->exe, sizeof(c->exe));
    c->brk_start = p->brk_start; c->brk_cur = p->brk_cur; c->mmap_next = p->mmap_next;
    c->ppid = p->pid; c->umask_v = p->umask_v;
    return c;
}

static void proc_exit(lxproc *p, int status)
{
    lxproc *q;
    int i;
    if (p->exited) return;
    p->exited = 1;
    p->status = status;
    p->cpu.stop = 1;
    for (i = 0; i < LX_MAXFD; i++) if (p->fd[i]) { fput(p->fd[i]); p->fd[i] = NULL; }   /* pipe EOF for readers */
    for (q = g_procs; q; q = q->next) {
        if (q->pid == p->ppid && !q->exited) {
            lxzombie *z = (lxzombie *)malloc(sizeof(*z));
            if (z) { z->pid = p->pid; z->status = status; z->next = q->zombies; q->zombies = z; }
        }
        if (q->ppid == p->pid) q->ppid = 1;
    }
}

static uint32_t do_fork(lxproc *p)
{
    lxproc *c = proc_fork(p), **pp;
    if (!c) return NEG(L_ENOMEM);
    c->cpu.r[X86_EAX] = 0;
    for (pp = &g_procs; *pp; pp = &(*pp)->next) ;
    *pp = c;
    if (p->o->trace) fprintf(stderr, "[qrx-i386 %d] fork -> child %d\n", p->pid, c->pid);
    return (uint32_t)c->pid;
}

static uint32_t do_wait(lxproc *p, int32_t pid, uint32_t statp, uint32_t opt)
{
    lxzombie **pp = &p->zombies;
    lxproc *q;
    for (; *pp; pp = &(*pp)->next) {
        lxzombie *z = *pp;
        if (pid == -1 || pid == 0 || pid == z->pid) {
            int id = z->pid;
            if (statp) x86_wr32(&p->cpu, statp, (uint32_t)z->status);
            *pp = z->next; free(z);
            return (uint32_t)id;
        }
    }
    for (q = g_procs; q; q = q->next)
        if (q->ppid == p->pid && !q->exited && (pid <= 0 || pid == q->pid)) {
            if (opt & 1u) return 0;                         /* WNOHANG */
            if (g_deadlock) return NEG(L_ECHILD);
            p->blocked = 1;
            return 0;
        }
    return NEG(L_ECHILD);
}

static int read_strv(lxproc *p, uint32_t a, char ***out)
{
    int n = 0, cap = 16;
    char **v = (char **)malloc(sizeof(char *) * (size_t)cap);
    if (!v) return -1;
    while (a) {
        uint32_t s = x86_rd32(&p->cpu, a + (uint32_t)n * 4);
        char tmp[4096];
        if (!s) break;
        if (!gstr(p, s, tmp, sizeof(tmp))) break;
        if (n + 2 >= cap) { char **nv; cap *= 2; nv = (char **)realloc(v, sizeof(char *) * (size_t)cap); if (!nv) break; v = nv; }
        v[n++] = strdup(tmp);
    }
    v[n] = NULL;
    *out = v;
    return n;
}
static void free_strv(char **v) { int i; if (!v) return; for (i = 0; v[i]; i++) free(v[i]); free(v); }

static uint32_t do_execve(lxproc *p, uint32_t pa, uint32_t av, uint32_t ev)
{
    char path[1024], hp[1600], **argv = NULL, **envp = NULL;
    int argc, envc, i;
    uint8_t head[256];
    FILE *f;
    size_t got;
    const char *err;
    if (!gstr(p, pa, path, sizeof(path))) return NEG(L_EFAULT);
    hostpath(p, path, hp, sizeof(hp));
    f = fopen(hp, "rb");
    if (!f) return HERR();
    got = fread(head, 1, sizeof(head) - 1, f);
    fclose(f);
    head[got] = 0;
    argc = read_strv(p, av, &argv);
    envc = read_strv(p, ev, &envp);
    if (argc < 0 || envc < 0) { free_strv(argv); free_strv(envp); return NEG(L_ENOMEM); }
    if (got > 2 && head[0] == '#' && head[1] == '!') {     /* script: #!interp [arg] */
        char *s = (char *)head + 2, *e, *arg = NULL, **nv;
        while (*s == ' ' || *s == '\t') s++;
        e = s; while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
        if (*e == ' ' || *e == '\t') { *e++ = 0; while (*e == ' ' || *e == '\t') e++; arg = e; while (*e && *e != '\n') e++; *e = 0; if (!*arg) arg = NULL; }
        else *e = 0;
        nv = (char **)calloc((size_t)argc + 4, sizeof(char *));
        if (!nv) { free_strv(argv); free_strv(envp); return NEG(L_ENOMEM); }
        i = 0; nv[i++] = strdup(s); if (arg) nv[i++] = strdup(arg); nv[i++] = strdup(path);
        { int j; for (j = 1; j < argc; j++) nv[i++] = argv[j]; if (argc > 0) free(argv[0]); }
        nv[i] = NULL; free(argv); argv = nv; argc = i;
        snprintf(path, sizeof(path), "%s", s);
        hostpath(p, path, hp, sizeof(hp));
        f = fopen(hp, "rb");
        if (!f) { free_strv(argv); free_strv(envp); return NEG(L_ENOENT); }
        got = fread(head, 1, 4, f); fclose(f);
    }
    if (got < 4 || memcmp(head, "\177ELF", 4) || head[4] != 1 || head[18] != 3) { free_strv(argv); free_strv(envp); return NEG(L_ENOEXEC); }
    for (i = 0; i < LX_MAXFD; i++) if (p->fd[i] && p->cloexec[i]) { fput(p->fd[i]); p->fd[i] = NULL; p->cloexec[i] = 0; }
    if (p->o->trace) fprintf(stderr, "[qrx-i386 %d] execve %s\n", p->pid, path);
    err = exec_image(p, path, argc, argv, envc, envp);
    free_strv(argv); free_strv(envp);
    if (err) {
        fprintf(stderr, "qrx-i386: execve %s: %s\n", path, err);
        proc_exit(p, 127 << 8);
        return 0;
    }
    return p->cpu.r[X86_EAX] = 0;
}

/* ---------------------------------------------------------------- syscall dispatcher */
static uint32_t sys_uname(lxproc *p, uint32_t a)
{
    static const char *f[6] = { "Linux", "qrx", "5.15.0-qrx86", "#1 QRX OS One 22.5.12 qrx86core", "i686", "(none)" };
    int i;
    for (i = 0; i < 6; i++) {
        char b[65];
        memset(b, 0, sizeof(b));
        strncpy(b, f[i], 64);
        if (!x86_write_mem(&p->cpu, a + (uint32_t)i * 65, b, 65)) return NEG(L_EFAULT);
    }
    return 0;
}

static uint32_t syscall_dispatch(lxproc *p, uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)
{
    x86cpu *c = &p->cpu;
    char path[1024], hp[1600], hp2[1600];
    struct stat st;
    lxfile *f;
    switch (nr) {
    case 1: case 252: proc_exit(p, (int)((a1 & 0xFF) << 8)); return 0;              /* exit, exit_group */
    case 2: case 190: return do_fork(p);                                           /* fork, vfork */
    case 120:                                                                      /* clone */
        if (a1 & 0x100u) return NEG(L_ENOSYS);                                     /* CLONE_VM: threads unsupported */
        { uint32_t pid = do_fork(p);
          if ((int32_t)pid > 0 && (a1 & 0x01000000u) && a5) x86_wr32(c, a5, 0);    /* CLONE_CHILD_SETTID in child only */
          return pid; }
    case 3: return do_read(p, fget(p, a1), a2, a3);
    case 4: return do_write(p, fget(p, a1), a2, a3);
    case 5: if (!gstr(p, a1, path, sizeof(path))) return NEG(L_EFAULT); return do_open(p, path, a2, a3);
    case 295: /* openat: dirfd other than AT_FDCWD only matters for relative paths (rare in practice) */
        if (!gstr(p, a2, path, sizeof(path))) return NEG(L_EFAULT);
        return do_open(p, path, a3, a4);
    case 6: f = fget(p, a1); if (!f) return NEG(L_EBADF); fput(f); p->fd[a1] = NULL; return 0;
    case 7: return do_wait(p, (int32_t)a1, a2, a3);
    case 114: return do_wait(p, (int32_t)a1, a2, a3);
    case 11: return do_execve(p, a1, a2, a3);
    case 10: if (!gstr(p, a1, path, sizeof(path))) return NEG(L_EFAULT); hostpath(p, path, hp, sizeof(hp)); return unlink(hp) ? HERR() : 0;
    case 301: if (!gstr(p, a2, path, sizeof(path))) return NEG(L_EFAULT); hostpath(p, path, hp, sizeof(hp));
        return ((a3 & 0x200u) ? rmdir(hp) : unlink(hp)) ? HERR() : 0;
    case 12: if (!gstr(p, a1, path, sizeof(path))) return NEG(L_EFAULT);
        hostpath(p, path, hp, sizeof(hp));
        if (stat(hp, &st)) return HERR();
        if (!S_ISDIR(st.st_mode)) return NEG(L_ENOTDIR);
        { char ab[1024]; normalize(ab, sizeof(ab), p->cwd, path); strcpy(p->cwd, ab); }
        return 0;
    case 133: f = fget(p, a1); if (!f || f->kind != F_DIR) return NEG(L_ENOTDIR);
        host_to_guest(p->o, f->path, p->cwd, sizeof(p->cwd));
        if (!p->cwd[0]) strcpy(p->cwd, "/");
        return 0;
    case 13: { uint32_t t = (uint32_t)time(NULL); if (a1) x86_wr32(c, a1, t); return t; }
    case 19: f = fget(p, a1); if (!f) return NEG(L_EBADF); if (f->kind != F_HOST) return NEG(L_ESPIPE);
        { off_t r = lseek(f->hfd, (off_t)(int32_t)a2, (int)a3); return r < 0 ? HERR() : (uint32_t)r; }
    case 140: f = fget(p, a1); if (!f) return NEG(L_EBADF); if (f->kind != F_HOST) return NEG(L_ESPIPE);
        { off_t r = lseek(f->hfd, (off_t)(((uint64_t)a2 << 32) | a3), (int)a5); if (r < 0) return HERR();
          x86_wr32(c, a4, (uint32_t)r); x86_wr32(c, a4 + 4, (uint32_t)((uint64_t)r >> 32)); return 0; }
    case 20: return (uint32_t)p->pid;
    case 64: return (uint32_t)(p->ppid ? p->ppid : 1);
    case 224: return (uint32_t)p->pid;
    case 24: case 47: case 49: case 50: case 199: case 200: case 201: case 202: return 1000;
    case 33: case 307: {
        uint32_t pa = nr == 33 ? a1 : a2, md = nr == 33 ? a2 : a3;
        if (!gstr(p, pa, path, sizeof(path))) return NEG(L_EFAULT);
        hostpath(p, path, hp, sizeof(hp));
        if (stat(hp, &st)) return HERR();
        if ((md & 2) && access(hp, W_OK)) return HERR();
        return 0; }
    case 37: if ((int)a1 == p->pid || a1 == 0) { if (a2) proc_exit(p, (int)(a2 & 0x7F)); } return 0;
    case 38:
        if (!gstr(p, a1, path, sizeof(path))) return NEG(L_EFAULT);
        hostpath(p, path, hp, sizeof(hp));
        if (!gstr(p, a2, path, sizeof(path))) return NEG(L_EFAULT);
        hostpath(p, path, hp2, sizeof(hp2));
        return rename(hp, hp2) ? HERR() : 0;
    case 39: case 296: if (!gstr(p, nr == 39 ? a1 : a2, path, sizeof(path))) return NEG(L_EFAULT); hostpath(p, path, hp, sizeof(hp));
        return mkdir(hp, (mode_t)((nr == 39 ? a2 : a3) & 0777u)) ? HERR() : 0;
    case 40: if (!gstr(p, a1, path, sizeof(path))) return NEG(L_EFAULT); hostpath(p, path, hp, sizeof(hp)); return rmdir(hp) ? HERR() : 0;
    case 41: f = fget(p, a1); if (!f) return NEG(L_EBADF); { int n = falloc(p, 0, f); if (n < 0) return NEG(L_EMFILE); f->refs++; return (uint32_t)n; }
    case 63: case 330: f = fget(p, a1); if (!f) return NEG(L_EBADF);
        if (a2 >= LX_MAXFD) return NEG(L_EBADF);
        if (a1 == a2) return a2;
        if (p->fd[a2]) fput(p->fd[a2]);
        p->fd[a2] = f; f->refs++; p->cloexec[a2] = (nr == 330 && (a3 & 02000000u)) ? 1 : 0;
        return a2;
    case 42: case 331: {
        lxpipe *pp = (lxpipe *)calloc(1, sizeof(lxpipe));
        lxfile *r = fnew(F_PIPE_R), *w = fnew(F_PIPE_W);
        int fr, fw;
        if (!pp || !r || !w) { free(pp); free(r); free(w); return NEG(L_ENOMEM); }
        r->pipe = w->pipe = pp; pp->readers = 1; pp->writers = 1;
        fr = falloc(p, 0, r); fw = fr >= 0 ? falloc(p, 0, w) : -1;
        if (fr < 0 || fw < 0) { if (fr >= 0) { p->fd[fr] = NULL; } free(r); free(w); free(pp); return NEG(L_EMFILE); }
        x86_wr32(c, a1, (uint32_t)fr); x86_wr32(c, a1 + 4, (uint32_t)fw);
        if (nr == 331 && (a2 & 02000000u)) p->cloexec[fr] = p->cloexec[fw] = 1;
        return 0; }
    case 43: { uint32_t t = (uint32_t)(c->icount / 1000000u); if (a1) { x86_wr32(c, a1, t); x86_wr32(c, a1 + 4, 0); x86_wr32(c, a1 + 8, 0); x86_wr32(c, a1 + 12, 0); } return t; }
    case 45: {
        uint32_t want = a1;
        if (!want || want < p->brk_start) return p->brk_cur;
        if (want > p->brk_cur) {
            uint32_t from = (p->brk_cur + X86_PAGE_MASK) & ~X86_PAGE_MASK;
            if (want > from && (!range_free(p, from, ((want - from) + X86_PAGE_MASK) & ~X86_PAGE_MASK) || !x86_map(c, from, want - from, 1))) return p->brk_cur;
        }
        p->brk_cur = want;
        return want; }
    case 54: {
        f = fget(p, a1);
        if (!f) return NEG(L_EBADF);
        if (f->kind != F_HOST || !isatty(f->hfd)) return NEG(L_ENOTTY);
        if (a2 == 0x5401u) { uint32_t i; for (i = 0; i < 60; i++) x86_wr8(c, a3 + i, 0); x86_wr32(c, a3 + 12, 0x8A3Bu); x86_wr32(c, a3 + 8, 0xBFu); x86_wr32(c, a3 + 4, 0x5u); x86_wr32(c, a3, 0x500u); return 0; }
        if (a2 == 0x5413u) { x86_wr16(c, a3, 24); x86_wr16(c, a3 + 2, 80); x86_wr16(c, a3 + 4, 0); x86_wr16(c, a3 + 6, 0); return 0; }
        if (a2 == 0x5402u || a2 == 0x5403u || a2 == 0x5404u) return 0;
        if (a2 == 0x540Fu) { x86_wr32(c, a3, (uint32_t)p->pid); return 0; }
        return NEG(L_ENOTTY); }
    case 55: case 221:
        f = fget(p, a1); if (!f) return NEG(L_EBADF);
        switch (a2) {
        case 0: case 1030: { int n = falloc(p, (int)a3, f); if (n < 0) return NEG(L_EMFILE); f->refs++; if (a2 == 1030) p->cloexec[n] = 1; return (uint32_t)n; }
        case 1: return p->cloexec[a1];
        case 2: p->cloexec[a1] = (unsigned char)(a3 & 1); return 0;
        case 3: return f->kind == F_PIPE_W ? 1u : f->kind == F_PIPE_R ? 0u : (uint32_t)(f->kind == F_HOST && f->hfd == 1 ? 1 : 2);
        case 4: return 0;
        default: return NEG(L_EINVAL);
        }
    case 60: { uint32_t o = p->umask_v; p->umask_v = a1 & 0777u; return o; }
    case 78: { struct timeval tv; gettimeofday(&tv, NULL); if (a1) { x86_wr32(c, a1, (uint32_t)tv.tv_sec); x86_wr32(c, a1 + 4, (uint32_t)tv.tv_usec); } if (a2) { x86_wr32(c, a2, 0); x86_wr32(c, a2 + 4, 0); } return 0; }
    case 85: case 305: {
        uint32_t pa = nr == 85 ? a1 : a2, buf = nr == 85 ? a2 : a3, n = nr == 85 ? a3 : a4;
        char ab[1024], tgt[1024];
        if (!gstr(p, pa, path, sizeof(path))) return NEG(L_EFAULT);
        normalize(ab, sizeof(ab), p->cwd, path);
        if (!strcmp(ab, "/proc/self/exe")) snprintf(tgt, sizeof(tgt), "%s", p->exe);
        else {
#if defined(__AROS__)
            return NEG(L_EINVAL);
#else
            ssize_t r;
            hostpath(p, path, hp, sizeof(hp));
            r = readlink(hp, tgt, sizeof(tgt) - 1);
            if (r < 0) return HERR();
            tgt[r] = 0;
#endif
        }
        { uint32_t L = (uint32_t)strlen(tgt); if (L > n) L = n; x86_write_mem(c, buf, tgt, L); return L; } }
    case 90: case 192: {                 /* old_mmap / mmap2 */
        uint32_t addr = a1, len = a2, prot = a3, fl = a4, fd = a5, off = a6;
        uint32_t base;
        if (nr == 90) { addr = x86_rd32(c, a1); len = x86_rd32(c, a1 + 4); prot = x86_rd32(c, a1 + 8); fl = x86_rd32(c, a1 + 12); fd = x86_rd32(c, a1 + 16); off = x86_rd32(c, a1 + 20) >> 12; }
        if (!len) return NEG(L_EINVAL);
        len = (len + X86_PAGE_MASK) & ~X86_PAGE_MASK;
        if (fl & 0x10u) { base = addr & ~X86_PAGE_MASK; x86_unmap(c, base, len); resv_drop(p, base, base + len); }
        else if (addr && !(addr & X86_PAGE_MASK) && range_free(p, addr, len)) base = addr;
        else base = find_space(p, len);
        if (!base) return NEG(L_ENOMEM);
        if (!prot && (fl & 0x20u)) {
            lxresv *r = (lxresv *)malloc(sizeof(*r));
            if (!r) return NEG(L_ENOMEM);
            r->a = base; r->b = base + len; r->next = p->resv; p->resv = r;
            return base;
        }
        if (!x86_map(c, base, len, 1)) return NEG(L_ENOMEM);
        { uint32_t x; for (x = 0; x < len; x += X86_PAGE_SIZE) memset(x86_host_ptr(c, base + x), 0, X86_PAGE_SIZE); }
        if (!(fl & 0x20u)) {
            f = fget(p, fd);
            if (!f || f->kind != F_HOST) { x86_unmap(c, base, len); return NEG(L_EBADF); }
            {
                off_t save = lseek(f->hfd, 0, SEEK_CUR);
                uint32_t done = 0;
                if (lseek(f->hfd, (off_t)off * 4096, SEEK_SET) < 0) { x86_unmap(c, base, len); return HERR(); }
                while (done < len) {
                    ssize_t r = read(f->hfd, x86_host_ptr(c, base + done), X86_PAGE_SIZE);
                    if (r <= 0) break;
                    done += (uint32_t)r;
                    if (r < (ssize_t)X86_PAGE_SIZE) break;
                }
                lseek(f->hfd, save, SEEK_SET);
            }
        }
        if (!(prot & 2u)) x86_protect(c, base, len, 0);
        return base; }
    case 91: { uint32_t a = a1 & ~X86_PAGE_MASK, l = (a2 + X86_PAGE_MASK) & ~X86_PAGE_MASK; x86_unmap(c, a, l); resv_drop(p, a, a + l); return 0; }
    case 125: {
        uint32_t a = a1 & ~X86_PAGE_MASK, l = (a2 + X86_PAGE_MASK) & ~X86_PAGE_MASK, x;
        if (a3 && resv_hit(p, a, a + l)) { resv_drop(p, a, a + l); for (x = 0; x < l; x += X86_PAGE_SIZE) if (!x86_is_mapped(c, a + x)) x86_map(c, a + x, X86_PAGE_SIZE, 1); }
        x86_protect(c, a, l, (a3 & 2u) != 0);
        return 0; }
    case 219: return 0;
    case 93: case 194: f = fget(p, a1); if (!f || f->kind != F_HOST) return NEG(L_EBADF); return ftruncate(f->hfd, (off_t)a2) ? HERR() : 0;
    case 94: case 15: case 306: case 212: case 182: case 198: case 207: case 95: case 298: return 0;   /* chmod/chown family: accepted */
    case 106: case 107: case 195: case 196: case 108: case 197: case 300: case 383: {
        int r;
        uint32_t out;
        if (nr == 108 || nr == 197) {
            f = fget(p, a1); if (!f) return NEG(L_EBADF);
            if (f->kind == F_HOST) r = fstat(f->hfd, &st);
            else if (f->kind == F_DIR) r = stat(f->path, &st);
            else { memset(&st, 0, sizeof(st)); st.st_mode = S_IFIFO | 0600; r = 0; }
            out = a2;
        } else {
            uint32_t pa = (nr == 300 || nr == 383) ? a2 : a1;
            uint32_t flg = nr == 300 ? a4 : nr == 383 ? a3 : 0;
            out = nr == 300 ? a3 : nr == 383 ? a5 : a2;
            if (!gstr(p, pa, path, sizeof(path))) return NEG(L_EFAULT);
            if (!path[0] && (flg & 0x1000u)) {  /* AT_EMPTY_PATH: fstat on dirfd */
                f = fget(p, a1); if (!f) return NEG(L_EBADF);
                r = f->kind == F_HOST ? fstat(f->hfd, &st) : f->kind == F_DIR ? stat(f->path, &st) : -1;
            } else {
                char ab[1024];
                normalize(ab, sizeof(ab), p->cwd, path);
                if (!strcmp(ab, "/dev/null") || !strcmp(ab, "/dev/tty")) { memset(&st, 0, sizeof(st)); st.st_mode = S_IFCHR | 0666; r = 0; }
                else {
                    hostpath(p, path, hp, sizeof(hp));
                    r = (nr == 107 || nr == 196 || (flg & 0x100u)) ? lstat(hp, &st) : stat(hp, &st);
                }
            }
        }
        if (r) return HERR();
        if (nr == 383) put_statx(p, out, &st); else put_stat64(p, out, &st);
        return 0; }
    case 99: case 100: case 268: case 269: {                     /* statfs family */
        uint32_t out = (nr == 268 || nr == 269) ? a3 : a2, i;
        for (i = 0; i < 84; i++) x86_wr8(c, out + i, 0);
        x86_wr32(c, out, 0xEF53u); x86_wr32(c, out + 4, 4096);
        return 0; }
    case 116: { uint32_t i; for (i = 0; i < 64; i++) x86_wr8(c, a1 + i, 0); x86_wr32(c, a1 + 16, 256u << 20); x86_wr32(c, a1 + 20, 128u << 20); x86_wr16(c, a1 + 40, 1); x86_wr32(c, a1 + 52, 1); return 0; }
    case 122: return sys_uname(p, a1);
    case 126: case 175: if ((nr == 175 ? a3 : a3) && a3) { uint32_t i; for (i = 0; i < 8; i++) x86_wr8(c, a3 + i, 0); } return 0;
    case 67: case 174: if (a3) { uint32_t i; for (i = 0; i < 20; i++) x86_wr8(c, a3 + i, 0); } return 0;
    case 186: case 173: case 179: return 0;
    case 141: return do_getdents(p, fget(p, a1), a2, a3, 0);
    case 220: return do_getdents(p, fget(p, a1), a2, a3, 1);
    case 142: case 168: case 308: case 309: {                   /* select/poll: everything ready */
        if (nr == 168 || nr == 309) { uint32_t i; for (i = 0; i < a2; i++) { uint16_t ev = x86_rd16(c, a1 + i * 8 + 4); x86_wr16(c, a1 + i * 8 + 6, (uint16_t)(ev & 5u)); } return a2; }
        return a1; }
    case 145: case 146: {
        uint32_t i, tot = 0, r;
        f = fget(p, a1);
        for (i = 0; i < a3; i++) {
            uint32_t b = x86_rd32(c, a2 + i * 8), l = x86_rd32(c, a2 + i * 8 + 4);
            if (!l) continue;
            r = nr == 145 ? do_read(p, f, b, l) : do_write(p, f, b, l);
            if ((int32_t)r < 0) return tot ? tot : r;
            tot += r;
            if (r < l) break;
        }
        return tot; }
    case 162: case 407: {
        uint32_t s = x86_rd32(c, a1), ns = nr == 407 ? x86_rd32(c, a1 + 8) : x86_rd32(c, a1 + 4);
        if (nr == 407) { s = x86_rd32(c, a2); ns = x86_rd32(c, a2 + 8); }
        if (s > 5) s = 5;   /* never stall the emulator for long */
        if (s) sleep(s);
        if (ns >= 1000) usleep(ns / 1000 > 999999 ? 999999 : ns / 1000);
        return 0; }
    case 183: {
        uint32_t L = (uint32_t)strlen(p->cwd) + 1;
        if (L > a2) return NEG(L_ERANGE);
        x86_write_mem(c, a1, p->cwd, L);
        return L; }
    case 191: case 76: x86_wr32(c, a2, a1 == 3 ? 8u << 20 : 0xFFFFFFFFu); x86_wr32(c, a2 + 4, 0xFFFFFFFFu); if (a1 == 7) { x86_wr32(c, a2, 1024); x86_wr32(c, a2 + 4, 1024); } return 0;
    case 340:                                     /* prlimit64(pid, resource, new, old) */
        if (a3) {
            x86_wr32(c, a3, 0); x86_wr32(c, a3 + 4, 0); x86_wr32(c, a3 + 8, 0xFFFFFFFFu); x86_wr32(c, a3 + 12, 0xFFFFFFFFu);
            if (a2 == 3) x86_wr32(c, a3, 8u << 20);
            if (a2 == 7) { x86_wr32(c, a3, 1024); x86_wr32(c, a3 + 8, 1024); x86_wr32(c, a3 + 12, 0); }
        }
        return 0;
    case 243: {                                  /* set_thread_area */
        int32_t e = (int32_t)x86_rd32(c, a1);
        uint32_t base = x86_rd32(c, a1 + 4), lim = x86_rd32(c, a1 + 8);
        int k;
        if (e == -1) { for (k = 0; k < 3 && c->tls[k].used; k++) ; if (k == 3) return NEG(L_ESPIPE); e = 6 + k; x86_wr32(c, a1, (uint32_t)e); }
        if (e < 6 || e > 8) return NEG(L_EINVAL);
        c->tls[e - 6].base = base; c->tls[e - 6].limit = lim; c->tls[e - 6].used = 1;
        for (k = X86_FS; k <= X86_GS; k++) if ((c->sreg[k] >> 3) == (uint16_t)e) c->sbase[k] = base;
        return 0; }
    case 258: return (uint32_t)p->pid;            /* set_tid_address */
    case 311: case 386: return nr == 386 ? NEG(L_ENOSYS) : 0;   /* set_robust_list ok, rseq unsupported */
    case 240: case 422: return (a2 & 127u) == 0 ? NEG(L_EAGAIN) : 0;  /* futex: no other threads */
    case 265: case 403: case 266: case 406: {
        uint64_t s = (uint64_t)time(NULL), ns = 0;
#if defined(CLOCK_REALTIME) && !defined(__AROS__)
        struct timespec ts;
        if (clock_gettime((a1 == 1 || a1 == 4 || a1 == 7) ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts) == 0) { s = (uint64_t)ts.tv_sec; ns = (uint64_t)ts.tv_nsec; }
#else
        struct timeval tv; gettimeofday(&tv, NULL); s = (uint64_t)tv.tv_sec; ns = (uint64_t)tv.tv_usec * 1000u;
#endif
        if (nr == 266 || nr == 406) { s = 0; ns = 1000; }
        if (!a2) return 0;
        if (nr == 265 || nr == 266) { x86_wr32(c, a2, (uint32_t)s); x86_wr32(c, a2 + 4, (uint32_t)ns); }
        else { x86_wr32(c, a2, (uint32_t)s); x86_wr32(c, a2 + 4, (uint32_t)(s >> 32)); x86_wr32(c, a2 + 8, (uint32_t)ns); x86_wr32(c, a2 + 12, 0); }
        return 0; }
    case 355: { uint32_t i; for (i = 0; i < a2; i++) x86_wr8(c, a1 + i, (uint8_t)(rand() >> 5)); return a2; }
    case 271: case 320: case 412: return 0;       /* utimes / utimensat: accepted */
    case 66: return (uint32_t)p->pid;            /* setsid */
    case 57: case 132: case 65: return (uint32_t)p->pid;  /* setpgid / getpgid / getpgrp */
    case 147: return (uint32_t)p->pid;            /* getsid */
    case 213: case 214: case 23: case 46: case 203: case 204: case 208: case 210: return 0;  /* set*id */
    case 9: case 83: case 304: return NEG(L_EPERM);  /* link / symlink */
    case 36: return 0;                           /* sync */
    case 118: case 148: return 0;                /* fsync / fdatasync */
    case 96: return 0;                           /* getpriority */
    case 29: return NEG(L_EINTR);                /* pause */
    case 27: return 0;                           /* alarm */
    case 77: { uint32_t i; for (i = 0; i < 72; i++) x86_wr8(c, a2 + i, 0); return 0; }  /* getrusage */
    case 104: case 105: return 0;                /* setitimer/getitimer */
    case 318: if (a1) x86_wr32(c, a1, 0); if (a2) x86_wr32(c, a2, 0); return 0;    /* getcpu */
    case 384: return NEG(L_EINVAL);              /* arch_prctl */
    case 172: return 0;                           /* prctl */
    default:
        if (p->o->trace) fprintf(stderr, "[qrx-i386 %d] unsupported syscall %u\n", p->pid, nr);
        return NEG(L_ENOSYS);
    }
}

static int lx_int(x86cpu *c, int vec)
{
    lxproc *p = (lxproc *)c->user;
    uint32_t nr, r;
    if (vec != 0x80) return 0;
    nr = c->r[X86_EAX];
    p->blocked = 0;
    r = syscall_dispatch(p, nr, c->r[X86_EBX], c->r[X86_ECX], c->r[X86_EDX], c->r[X86_ESI], c->r[X86_EDI], c->r[X86_EBP]);
    if (p->o->trace && !p->blocked) fprintf(stderr, "[qrx-i386 %d] sys %u(%08lx, %08lx, %08lx) = %ld\n", p->pid, nr,
                             (unsigned long)c->r[X86_EBX], (unsigned long)c->r[X86_ECX], (unsigned long)c->r[X86_EDX], (long)(int32_t)r);
    if (p->blocked && !p->exited) {          /* retry this int 0x80 in a later slice */
        c->eip -= 2;
        c->stop = 1;
        return 1;
    }
    if (!p->exited && nr != 11) c->r[X86_EAX] = r;
    else if (nr == 11 && (int32_t)r < 0) c->r[X86_EAX] = r;
    return 1;
}

/* run one slice; returns 1 if the process made progress */
static int run_slice(lxproc *p)
{
    x86cpu *c = &p->cpu;
    int f;
    uint64_t lim = c->icount + LX_SLICE;
    if (p->o->budget && lim > p->o->budget) lim = p->o->budget;
    c->stop = 0;
    c->budget = lim;
    p->blocked = 0;
    f = x86_run(c, 0xFFFFFFFFu);
    if (f == X86_FAULT_BUDGET && !(p->o->budget && c->icount >= p->o->budget)) { c->fault = X86_OK; return 1; }  /* preempted */
    if (f && !p->exited) {
        char d[400];
        x86_describe_fault(c, d, sizeof(d));
        fprintf(stderr, "qrx-i386: process %d (%s) stopped: %s\n", p->pid, p->exe, d);
        proc_exit(p, f == X86_FAULT_BUDGET ? 9 : 11);    /* reported as SIGKILL / SIGSEGV */
        return 1;
    }
    return p->exited || !p->blocked;
}

int lx_run(const lxopts *o, const char *path, int argc, char **argv, char **envp)
{
    lxproc *p = proc_new(o), *q, **pp;
    const char *err;
    int envc = 0, st, i;
    if (!p) return 127;
    /* without a root the guest sees the host file system: start in the host cwd */
    if (!o->root || !o->root[0]) { char cw[1024]; if (getcwd(cw, sizeof(cw))) host_to_guest(o, cw, p->cwd, sizeof(p->cwd)); }
#ifdef __AROS__
    {   /* an AmigaDOS program path (VOL:dir/file) given on the command line */
        static char gp[1100];
        if (strchr(path, ':') && (!o->root || !o->root[0])) { host_to_guest(o, path, gp, sizeof(gp)); path = gp; }
    }
#endif
    for (i = 0; i < 3; i++) { lxfile *f = fnew(F_HOST); if (f) { f->hfd = i; p->fd[i] = f; } }
    while (envp && envp[envc]) envc++;
    err = exec_image(p, path, argc, argv, envc, envp);
    if (err) { fprintf(stderr, "qrx-i386: %s: %s\n", path, err); proc_free(p); return 127; }
    g_procs = p;
    g_deadlock = 0;
    while (!p->exited) {
        int progress = 0;
        for (q = g_procs; q; q = q->next) if (!q->exited) progress |= run_slice(q);
        /* reap exited processes except the initial one */
        for (pp = &g_procs; *pp; ) {
            q = *pp;
            if (q->exited && q != p) { *pp = q->next; proc_free(q); } else pp = &q->next;
        }
        if (!progress) {
            if (g_deadlock) { fprintf(stderr, "qrx-i386: all processes blocked - terminating\n"); break; }
            g_deadlock = 1;        /* one round in which blocking calls fail instead of waiting */
        } else g_deadlock = 0;
    }
    if (getenv("QRX_I386_STATS")) fprintf(stderr, "qrx-i386: pid %d executed %llu guest instructions\n", p->pid, (unsigned long long)p->cpu.icount);
    st = p->exited ? p->status : 9;
    for (q = g_procs; q; ) { lxproc *n = q->next; if (q != p) { proc_exit(q, 9); proc_free(q); } q = n; }
    g_procs = NULL;
    proc_free(p);
    if (st & 0x7F) return 128 + (st & 0x7F);
    return (st >> 8) & 0xFF;
}
