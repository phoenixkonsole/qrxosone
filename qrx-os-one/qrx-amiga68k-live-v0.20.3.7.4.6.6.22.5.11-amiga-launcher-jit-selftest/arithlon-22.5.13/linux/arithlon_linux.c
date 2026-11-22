/* Arithlon 22.5.13 - m68k Linux programs on an x86-64 Linux host.
 *
 * Like Amithlon, the 68k code runs on the x86 machine with the host kernel
 * underneath and no Amiga chipset: a static big-endian m68k ELF is loaded into
 * the flat guest space, TRAP #0 system calls are translated to the host
 * (argument structures converted between big- and little-endian layouts), and
 * the CPU is either the reference interpreter (-i) or the x86-64 dynamic
 * recompiler (default).
 *
 *   arithlon [-i] [-s] [-E VAR=VAL] PROGRAM [ARGS...]
 *     -i  interpreter only        -s  statistics on exit
 * Not implemented: fork/clone/execve (single process), signal delivery, sockets. */
#define _GNU_SOURCE
#include "../core/m68k.h"
#include "../core/jit_x64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#include <poll.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/times.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <termios.h>

#define GUEST_SPAN   0x100000000ull
#define PAGE         4096u
#define MMAP_BASE    0x40000000u
#define STACK_TOP    0x7FFF0000u
#define STACK_SIZE   (8u << 20)

typedef struct {
    m68k cpu;
    uint8_t *mem;
    uint8_t *pagemap;                   /* 1 = mapped (one byte per guest page) */
    uint32_t brk_start, brk_cur, mmap_next;
    uint32_t tp;                        /* thread pointer (set_thread_area) */
    int exited, status, use_jit, stats;
    char exe[4096];
    jit_t *jit;
} proc_t;

static proc_t P;

/* ------------------------------------------------------------------ guest memory */
static uint8_t *G(uint32_t a) { return P.mem + a; }
static void put32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(G(a), &v, 4); }
static void put16(uint32_t a, uint32_t v) { uint16_t t = __builtin_bswap16((uint16_t)v); memcpy(G(a), &t, 2); }
static void put64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(G(a), &v, 8); }
static uint32_t get32(uint32_t a) { uint32_t v; memcpy(&v, G(a), 4); return __builtin_bswap32(v); }
static uint16_t get16(uint32_t a) { uint16_t v; memcpy(&v, G(a), 2); return __builtin_bswap16(v); }

static int map_range(uint32_t a, uint32_t len)
{
    uint32_t s = a & ~(PAGE - 1), e;
    uint64_t end = ((uint64_t)a + len + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    if (end > GUEST_SPAN) return 0;
    e = (uint32_t)(end >> 12);
    if (mprotect(G(s), (size_t)(end - s), PROT_READ | PROT_WRITE)) return 0;
    memset(P.pagemap + (s >> 12), 1, e - (s >> 12));
    return 1;
}
static void unmap_range(uint32_t a, uint32_t len)
{
    uint32_t s = a & ~(PAGE - 1);
    uint64_t end = ((uint64_t)a + len + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    if (end > GUEST_SPAN || end <= s) return;
    madvise(G(s), (size_t)(end - s), MADV_DONTNEED);
    mprotect(G(s), (size_t)(end - s), PROT_NONE);
    memset(P.pagemap + (s >> 12), 0, (size_t)((end - s) >> 12));
    if (P.jit) jit_invalidate(P.jit, s, (uint32_t)(end - s));
}
static uint32_t find_free(uint32_t len)
{
    uint32_t n = (len + PAGE - 1) >> 12, run = 0, p;
    for (p = P.mmap_next >> 12; p < (STACK_TOP - STACK_SIZE) >> 12; p++) {
        if (P.pagemap[p]) { run = 0; continue; }
        if (++run == n) return (p - n + 1) << 12;
    }
    return 0;
}

/* ------------------------------------------------------------------ faults */
static void on_segv(int sig, siginfo_t *si, void *uc)
{
    uint8_t *a = (uint8_t *)si->si_addr;
    m68k *c = &P.cpu;
    (void)uc;
    if (a >= P.mem && a < P.mem + GUEST_SPAN + 65536 && c->jb) {
        c->fault = M68K_FAULT_MEM;
        c->fault_addr = (uint32_t)(a - P.mem);
        snprintf(c->fault_msg, sizeof(c->fault_msg), "access to unmapped guest memory");
        if (P.jit) jit_sync_pc(P.jit, c);
        siglongjmp(*(sigjmp_buf *)c->jb, 1);
    }
    signal(sig, SIG_DFL);
}

/* ------------------------------------------------------------------ ELF loading */
static int load_elf(const char *path, uint32_t *entry, uint32_t *phdr, uint32_t *phnum)
{
    FILE *f = fopen(path, "rb");
    uint8_t eh[52], ph[32];
    uint32_t phoff, i, top = 0;
    uint16_t n, phentsize;
    if (!f) return 0;
    if (fread(eh, 1, 52, f) != 52 || memcmp(eh, "\177ELF\001\002", 6) || ((eh[18] << 8) | eh[19]) != 4) {
        fprintf(stderr, "arithlon: %s is not a 32-bit big-endian m68k ELF\n", path);
        fclose(f); return 0;
    }
#define B32(p) ((uint32_t)(p)[0] << 24 | (uint32_t)(p)[1] << 16 | (uint32_t)(p)[2] << 8 | (p)[3])
#define B16(p) ((uint16_t)((p)[0] << 8 | (p)[1]))
    if (B16(eh + 16) != 2) { fprintf(stderr, "arithlon: only static executables (ET_EXEC) are supported\n"); fclose(f); return 0; }
    *entry = B32(eh + 24); phoff = B32(eh + 28); phentsize = B16(eh + 42); n = B16(eh + 44);
    *phnum = n; *phdr = 0;
    for (i = 0; i < n; i++) {
        uint32_t type, off, va, fsz, msz;
        if (fseek(f, (long)(phoff + i * phentsize), SEEK_SET) || fread(ph, 1, 32, f) != 32) break;
        type = B32(ph); off = B32(ph + 4); va = B32(ph + 8); fsz = B32(ph + 16); msz = B32(ph + 20);
        if (type == 6) *phdr = va;                                       /* PT_PHDR */
        if (type != 1) continue;
        if (!map_range(va, msz)) { fclose(f); return 0; }
        if (fsz && (fseek(f, (long)off, SEEK_SET) || fread(G(va), 1, fsz, f) != fsz)) { fclose(f); return 0; }
        if (!*phdr && off == 0) *phdr = va + phoff;
        if (va + msz > top) top = va + msz;
    }
    fclose(f);
    P.brk_start = P.brk_cur = (top + PAGE - 1) & ~(PAGE - 1);
    return 1;
}

static uint32_t push_str(uint32_t *sp, const char *s)
{
    size_t n = strlen(s) + 1;
    *sp -= (uint32_t)n;
    memcpy(G(*sp), s, n);
    return *sp;
}
static void setup_stack(m68k *c, int argc, char **argv, char **envp, uint32_t entry, uint32_t phdr, uint32_t phnum)
{
    uint32_t sp = STACK_TOP, *av, *ev, rnd, i;
    int envc = 0;
    while (envp[envc]) envc++;
    map_range(STACK_TOP - STACK_SIZE, STACK_SIZE);
    av = calloc((size_t)argc + 1, 4); ev = calloc((size_t)envc + 1, 4);
    push_str(&sp, P.exe);
    for (i = 0; i < (uint32_t)envc; i++) ev[i] = push_str(&sp, envp[i]);
    for (i = 0; i < (uint32_t)argc; i++) av[i] = push_str(&sp, argv[i]);
    sp -= 16; rnd = sp;
    for (i = 0; i < 16; i++) G(rnd)[i] = (uint8_t)(rand() & 0xFF);
    sp &= ~15u;
    {
        uint32_t aux[][2] = { { 3, phdr }, { 4, 32 }, { 5, phnum }, { 6, PAGE }, { 9, entry }, { 11, (uint32_t)getuid() },
                              { 12, (uint32_t)geteuid() }, { 13, (uint32_t)getgid() }, { 14, (uint32_t)getegid() },
                              { 16, 0 }, { 17, 100 }, { 23, 0 }, { 25, rnd }, { 0, 0 } };
        uint32_t words = 1 + (uint32_t)argc + 1 + (uint32_t)envc + 1 + (uint32_t)(sizeof(aux) / 4);
        sp -= words * 4; sp &= ~15u;
        c->a[7] = sp;
        put32(sp, (uint32_t)argc); sp += 4;
        for (i = 0; i < (uint32_t)argc; i++, sp += 4) put32(sp, av[i]);
        put32(sp, 0); sp += 4;
        for (i = 0; i < (uint32_t)envc; i++, sp += 4) put32(sp, ev[i]);
        put32(sp, 0); sp += 4;
        for (i = 0; i < sizeof(aux) / 8; i++, sp += 8) { put32(sp, aux[i][0]); put32(sp + 4, aux[i][1]); }
    }
    free(av); free(ev);
}

/* ------------------------------------------------------------------ system calls */
static int32_t E(long r) { return r < 0 ? -errno : (int32_t)r; }
static int open_flags(uint32_t f)
{
    int h = (int)(f & ~0740000u);
    if (f & 040000) h |= O_DIRECTORY;
    if (f & 0100000) h |= O_NOFOLLOW;
    return h;
}
static uint32_t open_flags_back(int h)
{
    uint32_t f = (uint32_t)h & ~(uint32_t)(O_DIRECTORY | O_NOFOLLOW | O_DIRECT | O_LARGEFILE);
    if (h & O_DIRECTORY) f |= 040000;
    if (h & O_NOFOLLOW) f |= 0100000;
    return f;
}
static const char *gpath(uint32_t p) { return (const char *)G(p); }
static void put_stat64(uint32_t b, const struct stat *st)
{
    memset(G(b), 0, 92);
    put64(b, st->st_dev); put32(b + 10, (uint32_t)st->st_ino); put32(b + 14, st->st_mode); put32(b + 18, (uint32_t)st->st_nlink);
    put32(b + 22, st->st_uid); put32(b + 26, st->st_gid); put64(b + 30, st->st_rdev); put64(b + 40, (uint64_t)st->st_size);
    put32(b + 48, (uint32_t)st->st_blksize); put64(b + 52, (uint64_t)st->st_blocks);
    put32(b + 60, (uint32_t)st->st_atim.tv_sec); put32(b + 64, (uint32_t)st->st_atim.tv_nsec);
    put32(b + 68, (uint32_t)st->st_mtim.tv_sec); put32(b + 72, (uint32_t)st->st_mtim.tv_nsec);
    put32(b + 76, (uint32_t)st->st_ctim.tv_sec); put32(b + 80, (uint32_t)st->st_ctim.tv_nsec);
    put64(b + 84, st->st_ino);
}
static int32_t sys_statx(uint32_t dirfd, uint32_t path, uint32_t flags, uint32_t mask, uint32_t buf)
{
    struct statx s;
    uint32_t i;
    if (syscall(SYS_statx, (int)dirfd, gpath(path), (int)flags, mask, &s) < 0) return -errno;
    memset(G(buf), 0, 256);
    put32(buf, s.stx_mask); put32(buf + 4, s.stx_blksize); put64(buf + 8, s.stx_attributes);
    put32(buf + 16, s.stx_nlink); put32(buf + 20, s.stx_uid); put32(buf + 24, s.stx_gid); put16(buf + 28, s.stx_mode);
    put64(buf + 32, s.stx_ino); put64(buf + 40, s.stx_size); put64(buf + 48, s.stx_blocks); put64(buf + 56, s.stx_attributes_mask);
    {
        struct statx_timestamp *t[4] = { &s.stx_atime, &s.stx_btime, &s.stx_ctime, &s.stx_mtime };
        for (i = 0; i < 4; i++) { put64(buf + 64 + i * 16, (uint64_t)t[i]->tv_sec); put32(buf + 72 + i * 16, t[i]->tv_nsec); }
    }
    put32(buf + 128, s.stx_rdev_major); put32(buf + 132, s.stx_rdev_minor); put32(buf + 136, s.stx_dev_major); put32(buf + 140, s.stx_dev_minor);
    return 0;
}
static int32_t sys_getdents64(int fd, uint32_t buf, uint32_t cnt)
{
    char *t = malloc(cnt);
    long n = syscall(SYS_getdents64, fd, t, cnt), o = 0;
    if (n < 0) { free(t); return -errno; }
    memcpy(G(buf), t, (size_t)n);
    while (o < n) {                                  /* same layout: swap ino, off, reclen */
        uint64_t ino, off; uint16_t rl;
        memcpy(&ino, t + o, 8); memcpy(&off, t + o + 8, 8); memcpy(&rl, t + o + 16, 2);
        put64(buf + (uint32_t)o, ino); put64(buf + (uint32_t)o + 8, off); put16(buf + (uint32_t)o + 16, rl);
        o += rl;
    }
    free(t);
    return (int32_t)n;
}
static uint32_t do_brk(uint32_t a)
{
    if (!a || a < P.brk_start) return P.brk_cur;
    if (a > P.brk_cur) {
        uint32_t s = (P.brk_cur + PAGE - 1) & ~(PAGE - 1);
        if (a > s && !map_range(s, a - s)) return P.brk_cur;
    }
    P.brk_cur = a;
    return a;
}
static int32_t do_mmap(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags, int32_t fd, uint32_t pgoff)
{
    uint32_t a;
    (void)prot;
    if (!len) return -EINVAL;
    len = (len + PAGE - 1) & ~(PAGE - 1);
    if (flags & 0x10) { a = addr & ~(PAGE - 1); unmap_range(a, len); }   /* MAP_FIXED */
    else if (!(a = find_free(len))) return -ENOMEM;
    if (!map_range(a, len)) return -ENOMEM;
    if (!(flags & 0x20) && fd >= 0) {                                     /* file-backed: private copy */
        ssize_t r = pread(fd, G(a), len, (off_t)pgoff * 4096);
        if (r < 0) { int e = errno; unmap_range(a, len); return -e; }
    }
    return (int32_t)a;
}
static int32_t do_writev(int fd, uint32_t iov, uint32_t n)
{
    struct iovec v[64];
    uint32_t i;
    if (n > 64) n = 64;
    for (i = 0; i < n; i++) { v[i].iov_base = G(get32(iov + i * 8)); v[i].iov_len = get32(iov + i * 8 + 4); }
    return E(writev(fd, v, (int)n));
}

static int32_t do_syscall(m68k *c, uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)
{
    switch (nr) {
    case 1: case 247: P.exited = 1; P.status = (int)a1 & 0xFF; c->stop = 1; return 0;     /* exit, exit_group */
    case 3: return E(read((int)a1, G(a2), a3));
    case 4: return E(write((int)a1, G(a2), a3));
    case 5: return E(open(gpath(a1), open_flags(a2), a3));
    case 6: return E(close((int)a1));
    case 10: return E(unlink(gpath(a1)));
    case 12: return E(chdir(gpath(a1)));
    case 13: { time_t t = time(NULL); if (a1) put32(a1, (uint32_t)t); return (int32_t)t; }
    case 19: return E(lseek((int)a1, (int32_t)a2, (int)a3));
    case 20: return getpid();
    case 33: return E(access(gpath(a1), (int)a2));
    case 38: return E(rename(gpath(a1), gpath(a2)));
    case 39: return E(mkdir(gpath(a1), a2));
    case 41: return E(dup((int)a1));
    case 42: case 327: { int fds[2]; if ((nr == 42 ? pipe(fds) : pipe2(fds, open_flags(a2))) < 0) return -errno; put32(a1, (uint32_t)fds[0]); put32(a1 + 4, (uint32_t)fds[1]); return 0; }
    case 43: { struct tms t; clock_t r = times(&t); if (a1) { put32(a1, (uint32_t)t.tms_utime); put32(a1 + 4, (uint32_t)t.tms_stime); put32(a1 + 8, (uint32_t)t.tms_cutime); put32(a1 + 12, (uint32_t)t.tms_cstime); } return (int32_t)r; }
    case 45: return (int32_t)do_brk(a1);
    case 54:                                                       /* ioctl */
        if (a2 == 0x5401) { if (!isatty((int)a1)) return -ENOTTY; memset(G(a3), 0, 36); return 0; }   /* TCGETS */
        if (a2 == 0x5413) { struct winsize ws; if (ioctl((int)a1, TIOCGWINSZ, &ws) < 0) return -errno;
                            put16(a3, ws.ws_row); put16(a3 + 2, ws.ws_col); put16(a3 + 4, ws.ws_xpixel); put16(a3 + 6, ws.ws_ypixel); return 0; }
        return -EINVAL;
    case 60: return (int32_t)umask(a1);
    case 63: return E(dup2((int)a1, (int)a2));
    case 64: return getppid();
    case 65: return getpgrp();
    case 78: { struct timeval tv; gettimeofday(&tv, NULL); if (a1) { put32(a1, (uint32_t)tv.tv_sec); put32(a1 + 4, (uint32_t)tv.tv_usec); } return 0; }
    case 85: case 298: {                                           /* readlink, readlinkat */
        const char *p = gpath(nr == 85 ? a1 : a2);
        uint32_t buf = nr == 85 ? a2 : a3, sz = nr == 85 ? a3 : a4;
        if (!strcmp(p, "/proc/self/exe")) { size_t n = strlen(P.exe); if (n > sz) n = sz; memcpy(G(buf), P.exe, n); return (int32_t)n; }
        return nr == 85 ? E(readlink(p, (char *)G(buf), sz)) : E(readlinkat((int)a1, p, (char *)G(buf), sz)); }
    case 91: unmap_range(a1, a2); return 0;
    case 116: memset(G(a1), 0, 64); return 0;                     /* sysinfo */
    case 122: {                                                    /* uname */
        static const char *f[6] = { "Linux", "arithlon", "6.1.0", "#1 Arithlon 22.5.13", "m68k", "(none)" };
        int i;
        memset(G(a1), 0, 6 * 65);
        for (i = 0; i < 6; i++) strcpy((char *)G(a1 + (uint32_t)i * 65), f[i]);
        return 0; }
    case 125: case 238: case 158: case 336: return 0;              /* mprotect, madvise, sched_yield, atomic_barrier */
    case 140: {                                                    /* _llseek */
        off_t r = lseek((int)a1, (off_t)(((uint64_t)a2 << 32) | a3), (int)a5);
        if (r < 0) return -errno;
        put64(a4, (uint64_t)r);
        return 0; }
    case 146: return do_writev((int)a1, a2, a3);
    case 162: {                                    /* nanosleep */
        struct timespec t = { (time_t)(int32_t)get32(a1), (long)get32(a1 + 4) };
        return E(nanosleep(&t, NULL)); }
    case 168: {                                                    /* poll */
        struct pollfd pf[64]; uint32_t i, n = a2 > 64 ? 64 : a2; int r;
        for (i = 0; i < n; i++) { pf[i].fd = (int)get32(a1 + i * 8); pf[i].events = (short)get16(a1 + i * 8 + 4); pf[i].revents = 0; }
        r = poll(pf, n, (int)a3);
        if (r < 0) return -errno;
        for (i = 0; i < n; i++) put16(a1 + i * 8 + 6, (uint16_t)pf[i].revents);
        return r; }
    case 174: if (a3) memset(G(a3), 0, 20); return 0;              /* rt_sigaction */
    case 175: if (a3) memset(G(a3), 0, 8); return 0;               /* rt_sigprocmask */
    case 183: { if (!getcwd((char *)G(a1), a2)) return -errno; return (int32_t)strlen((char *)G(a1)) + 1; }
    case 191: put32(a2, a1 == 3 ? STACK_SIZE : 0xFFFFFFFFu); put32(a2 + 4, 0xFFFFFFFFu); return 0;   /* ugetrlimit */
    case 192: return do_mmap(a1, a2, a3, a4, (int32_t)a5, a6);
    case 194: return E(ftruncate((int)a1, (off_t)(((uint64_t)a2 << 32) | a3)));
    case 195: case 196: case 197: {                                /* stat64, lstat64, fstat64 */
        struct stat st; int r = nr == 197 ? fstat((int)a1, &st) : nr == 195 ? stat(gpath(a1), &st) : lstat(gpath(a1), &st);
        if (r < 0) return -errno;
        put_stat64(a2, &st); return 0; }
    case 199: return (int32_t)getuid();
    case 200: return (int32_t)getgid();
    case 201: return (int32_t)geteuid();
    case 202: return (int32_t)getegid();
    case 220: return sys_getdents64((int)a1, a2, a3);
    case 235: return (a2 & 127) == 1 ? 0 : -EAGAIN;              /* futex: single thread */
    case 239: {                                                    /* fcntl64 */
        int r;
        if (a2 == 4) return E(fcntl((int)a1, F_SETFL, open_flags(a3)));
        r = fcntl((int)a1, (int)a2, a3);
        if (r < 0) return -errno;
        return a2 == 3 ? (int32_t)open_flags_back(r) : r; }
    case 253: return getpid();                                     /* set_tid_address */
    case 260: case 403: {                                          /* clock_gettime(64) */
        struct timespec t;
        if (clock_gettime((clockid_t)a1, &t) < 0) return -errno;
        if (nr == 403) { put64(a2, (uint64_t)t.tv_sec); put64(a2 + 8, (uint64_t)t.tv_nsec); }
        else { put32(a2, (uint32_t)t.tv_sec); put32(a2 + 4, (uint32_t)t.tv_nsec); }
        return 0; }
    case 288: return E(openat((int)a1, gpath(a2), open_flags(a3), a4));
    case 289: return E(mkdirat((int)a1, gpath(a2), a3));
    case 293: { struct stat st; if (fstatat((int)a1, gpath(a2), &st, (int)a4) < 0) return -errno; put_stat64(a3, &st); return 0; }
    case 294: return E(unlinkat((int)a1, gpath(a2), (int)a3));
    case 295: return E(renameat((int)a1, gpath(a2), (int)a3, gpath(a4)));
    case 300: return E(faccessat((int)a1, gpath(a2), (int)a3, 0));
    case 326: return E(dup3((int)a1, (int)a2, open_flags(a3)));
    case 333: return (int32_t)P.tp;                                /* get_thread_area */
    case 334: P.tp = a1; return 0;                                 /* set_thread_area */
    case 335: { uint32_t v = get32(a6); if (v == a2) put32(a6, a1); return (int32_t)v; }   /* atomic_cmpxchg_32 */
    case 339: {                                                    /* prlimit64 */
        if (a4) { put64(a4, a2 == 3 ? STACK_SIZE : ~0ull); put64(a4 + 8, ~0ull); }
        return 0; }
    case 352: return E(getrandom(G(a1), a2, a3));
    case 379: return sys_statx(a1, a2, a3, a4, a5);
    case 265: case 222:                                            /* tgkill / tkill (abort) */
        fprintf(stderr, "arithlon: program sent signal %u to itself\n", nr == 265 ? a3 : a2);
        P.exited = 1; P.status = 128 + (int)(nr == 265 ? a3 : a2); c->stop = 1; return 0;
    default: break;
    }
    if (getenv("ARITHLON_TRACE")) fprintf(stderr, "arithlon: system call %u not implemented\n", nr);
    return -ENOSYS;
}

static int on_trap(m68k *c, int vec)
{
    if (vec != 0) return 0;
    c->d[0] = (uint32_t)do_syscall(c, c->d[0], c->d[1], c->d[2], c->d[3], c->d[4], c->d[5], c->a[0]);
    return 1;
}

int main(int argc, char **argv)
{
    extern char **environ;
    m68k *c = &P.cpu;
    uint32_t entry, phdr, phnum;
    int i = 1, f;
    struct sigaction sa;
    char **envp = environ;
    P.use_jit = 1;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-i")) P.use_jit = 0;
        else if (!strcmp(argv[i], "-s")) P.stats = 1;
        else break;
        i++;
    }
    if (i >= argc) { fprintf(stderr, "usage: arithlon [-i] [-s] M68K-PROGRAM [ARGS...]\n"); return 2; }
    P.mem = mmap(NULL, GUEST_SPAN + 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    P.pagemap = calloc(1u << 20, 1);
    if (P.mem == MAP_FAILED || !P.pagemap) { fprintf(stderr, "arithlon: cannot reserve the 4 GiB guest space\n"); return 1; }
    if (!realpath(argv[i], P.exe)) snprintf(P.exe, sizeof(P.exe), "%s", argv[i]);
    m68k_init(c, P.mem);
    c->trap = on_trap;
    P.mmap_next = MMAP_BASE;
    if (!load_elf(argv[i], &entry, &phdr, &phnum)) { fprintf(stderr, "arithlon: cannot load %s\n", argv[i]); return 127; }
    setup_stack(c, argc - i, argv + i, envp, entry, phdr, phnum);
    c->pc = entry;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    if (P.use_jit) {
        P.jit = jit_new(c);
        if (!P.jit) { fprintf(stderr, "arithlon: JIT unavailable, interpreting\n"); P.use_jit = 0; }
    }
    f = P.use_jit ? jit_run(P.jit, c) : m68k_run(c);
    fflush(stdout);
    if (f && !P.exited) {
        char d[512];
        m68k_describe(c, d, sizeof(d));
        fprintf(stderr, "arithlon: program stopped: %s\n", d);
        return 139;
    }
    if (P.stats) {
        if (P.jit) jit_stats(P.jit, stderr);
        fprintf(stderr, "arithlon: %llu instructions interpreted\n", (unsigned long long)c->icount);
    }
    return P.status;
}
