/*
 * libqrxaroscompat, part 4: sys/mman.h for QRX OS One 64 (the header is in the AROS patch,
 * compiler/crt/posixc/include/aros/posixc/sys/mman.h).
 *
 * AROS runs every program in one address space without paging, so a "mapping" is a block of
 * memory: anonymous private mappings come zeroed and page-aligned (from 1 MB up aligned to 1 MB,
 * what SpiderMonkey's GC asks for), munmap of a whole mapping frees it. Unmapping part of a mapping
 * keeps the memory (it cannot be given back in pieces); MADV_DONTNEED zeroes the range, as Linux
 * does for anonymous memory. mprotect, mlock: accepted, no effect (no MMU protection here).
 * File mappings are copies: the block is filled from the file with pread (beyond the end of the
 * file it stays zero). A MAP_SHARED mapping with PROT_WRITE keeps a duplicate of the descriptor and
 * writes its bytes back to the file on msync and munmap; between those, the mapping and the file
 * (write(), other mappings) do not see each other's changes, as there is no MMU to share pages.
 */
#define _GNU_SOURCE   /* posix_memalign in AROS's stdlib.h */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <proto/exec.h>
#include <exec/semaphores.h>

struct region {
    uintptr_t base; size_t len;
    int wfd;                     /* MAP_SHARED|PROT_WRITE file mapping: where to write back, else -1 */
    off_t off; size_t flen;      /* file offset and the bytes that came from the file */
};
ssize_t pread(int fd, void *buf, size_t n, off_t off);          /* qrxaroscompat.c */
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
int dup(int fd);
int close(int fd);
static struct region *regions;
static size_t nregions, cap;
static struct SignalSemaphore lock;
static volatile int lock_ready;

static void lock_init(void)
{
    if (lock_ready) return;
    Forbid();
    if (!lock_ready) { InitSemaphore(&lock); lock_ready = 1; }
    Permit();
}

int qrx_is_shm_fd(int fd);
void *qrx_shm_mmap(size_t len, int prot, int fd, off_t off);
int qrx_shm_munmap(void *addr, size_t len);

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (!len) { errno = EINVAL; return MAP_FAILED; }
    if ((flags & MAP_SHARED) && !(flags & MAP_ANON) && qrx_is_shm_fd(fd)) return qrx_shm_mmap(len, prot, fd, off);
    int file = !(flags & MAP_ANON);
    if (!file && fd != -1) { errno = EINVAL; return MAP_FAILED; }
    if (file && (off & 4095)) { errno = EINVAL; return MAP_FAILED; }
    size_t want = len;
    len = (len + 4095) & ~(size_t)4095;
    lock_init();
    if (file && (flags & MAP_FIXED)) { errno = EINVAL; return MAP_FAILED; }
    if (flags & MAP_FIXED) {
        /* only inside a mapping already handed out: then it is a re-zeroing of that range */
        ObtainSemaphore(&lock);
        for (size_t i = 0; i < nregions; i++) {
            struct region *r = &regions[i];
            if ((uintptr_t)addr >= r->base && (uintptr_t)addr + len <= r->base + r->len) {
                ReleaseSemaphore(&lock);
                memset(addr, 0, len);
                return addr;
            }
        }
        ReleaseSemaphore(&lock);
        errno = EINVAL;
        return MAP_FAILED;
    }
    size_t align = len >= (1u << 20) ? (1u << 20) : 4096;
    void *p = NULL;
    if (posix_memalign(&p, align, len) || !p) { errno = ENOMEM; return MAP_FAILED; }
    memset(p, 0, len);
    size_t got = 0;
    int wfd = -1;
    if (file) {
        while (got < want) {
            ssize_t n = pread(fd, (char *)p + got, want - got, off + (off_t)got);
            if (n < 0) { int e = errno; free(p); errno = e == EBADF ? EBADF : EACCES; return MAP_FAILED; }
            if (n == 0) break;
            got += (size_t)n;
        }
        if ((flags & MAP_SHARED) && (prot & PROT_WRITE)) {
            wfd = dup(fd);
            if (wfd < 0) { free(p); errno = EBADF; return MAP_FAILED; }
        }
    }
    ObtainSemaphore(&lock);
    if (nregions == cap) {
        size_t ncap = cap ? cap * 2 : 64;
        struct region *n = realloc(regions, ncap * sizeof *n);
        if (!n) { ReleaseSemaphore(&lock); free(p); errno = ENOMEM; return MAP_FAILED; }
        regions = n; cap = ncap;
    }
    regions[nregions].base = (uintptr_t)p;
    regions[nregions].len = len;
    regions[nregions].wfd = wfd;
    regions[nregions].off = off;
    regions[nregions].flen = wfd >= 0 ? want : got;
    nregions++;
    ReleaseSemaphore(&lock);
    return p;
}

/* the bytes [a, a+n) of a shared writable file mapping back to the file */
static int writeback(const struct region *r, uintptr_t a, size_t n)
{
    uintptr_t end = r->base + r->flen;
    if (a < r->base) { n -= r->base - a; a = r->base; }
    if (a >= end) return 0;
    if (a + n > end) n = end - a;
    size_t done = 0;
    while (done < n) {
        ssize_t w = pwrite(r->wfd, (const char *)(a + done), n - done, r->off + (off_t)(a - r->base + done));
        if (w <= 0) { errno = EIO; return -1; }
        done += (size_t)w;
    }
    return 0;
}

int munmap(void *addr, size_t len)
{
    if (qrx_shm_munmap(addr, len)) return 0;
    if (!len) { errno = EINVAL; return -1; }
    len = (len + 4095) & ~(size_t)4095;    /* as mmap rounded it: munmap(p, the length asked) */
    lock_init();
    ObtainSemaphore(&lock);
    for (size_t i = 0; i < nregions; i++) {
        struct region *r = &regions[i];
        if ((uintptr_t)addr == r->base && len >= r->len) {
            struct region g = *r;
            regions[i] = regions[--nregions];
            ReleaseSemaphore(&lock);
            int rc = 0;
            if (g.wfd >= 0) {
                if (writeback(&g, g.base, g.flen)) rc = -1;
                close(g.wfd);
            }
            free((void *)g.base);
            return rc;
        }
        if ((uintptr_t)addr >= r->base && (uintptr_t)addr < r->base + r->len) {
            ReleaseSemaphore(&lock);       /* a part of a mapping: kept, see above */
            return 0;
        }
    }
    ReleaseSemaphore(&lock);
    return 0;                              /* nothing mapped there: not an error in POSIX */
}

int madvise(void *addr, size_t len, int advice)
{
    if (advice == MADV_DONTNEED) memset(addr, 0, len);
    return 0;
}
int posix_madvise(void *addr, size_t len, int advice) { return madvise(addr, len, advice) ? errno : 0; }
int mprotect(void *addr, size_t len, int prot) { (void)addr; (void)len; (void)prot; return 0; }
int msync(void *addr, size_t len, int flags)
{
    (void)flags;           /* MS_ASYNC and MS_SYNC: written now either way */
    lock_init();
    ObtainSemaphore(&lock);
    for (size_t i = 0; i < nregions; i++) {
        struct region *r = &regions[i];
        if ((uintptr_t)addr >= r->base && (uintptr_t)addr < r->base + r->len) {
            struct region g = *r;
            ReleaseSemaphore(&lock);
            return g.wfd >= 0 ? writeback(&g, (uintptr_t)addr, len) : 0;
        }
    }
    ReleaseSemaphore(&lock);
    return 0;
}
int mlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
int munlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }

/* time.h: AROS keeps the time zone in locale.library; the C library's localtime uses it */
void tzset(void) {}

/* unistd.h: there is no fork on AROS (one address space, nothing to copy); qrx_spawn starts programs */
#include <sys/types.h>
pid_t fork(void) { errno = ENOSYS; return -1; }

/* -fstack-protector: the guard value, from the host's random bytes before main, and the report */
#include <stdio.h>
#include <unistd.h>
int qrx_host_random(void *buf, size_t n);   /* qrxnet.c */
uintptr_t __stack_chk_guard = 0x5152583634a5c3e1UL;
__attribute__((constructor)) static void qrx_stack_guard_init(void)
{
    uintptr_t g;
    if (qrx_host_random(&g, sizeof g) == 0) __stack_chk_guard = g & ~(uintptr_t)0xFF;  /* a 0 byte stops string overflows */
}
__attribute__((noreturn)) void __stack_chk_fail(void)
{
    static const char msg[] = "*** stack smashing detected: terminated\n";
    write(2, msg, sizeof msg - 1);
    abort();
}

/* sysconf: AROS's knows only _SC_ARG_MAX and fails with -1 for the rest; programs that size their
 * memory by the page size (SpiderMonkey's GC) then compute with (size_t)-1. The 68100EC64 has
 * 4 KB pages (no paging is done, it is the unit mmap rounds to above). Everything else goes to
 * AROS. */
long __sysconf_CrtBase_wrapper(int name);
long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE:
#if _SC_PAGE_SIZE != _SC_PAGESIZE
    case _SC_PAGE_SIZE:
#endif
        return 4096;
    default:
        return __sysconf_CrtBase_wrapper(name);
    }
}

/* QRX_TEXTBASE set (a shell variable: Set QRX_TEXTBASE 1): the program says where its code was
 * loaded (the address of this function), so pcs from the engine's QRX_EC64_PCSAMPLE profile can be
 * mapped to its symbols: offset = pc - printed address + this function's value in the ELF file. */
void qrx_report_textbase(void);
__attribute__((constructor)) void qrx_report_textbase(void)
{
    if (getenv("QRX_TEXTBASE")) {
        char b[96];
        int n = snprintf(b, sizeof b, "qrx_report_textbase at %p, nregions at %p\n", (void *)&qrx_report_textbase,
                         (void *)&nregions);
        write(2, b, (size_t)n);
    }
}

/* getpagesize (BSD; SQLite asks for it): the page of sysconf above */
int getpagesize(void) { return 4096; }
/* fchown: AROS has no owners (as lchown in qrxaroscompat.c) */
int fchown(int fd, uid_t o, gid_t g) { (void)fd; (void)o; (void)g; errno = ENOSYS; return -1; }

/* syslog: AROS has no system log; the message goes to the error output with its priority */
#include <stdarg.h>
void syslog(int priority, const char *format, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, format);
    int n = snprintf(b, sizeof b, "syslog <%d>: ", priority);
    if (n > 0 && n < (int)sizeof b) vsnprintf(b + n, sizeof b - (size_t)n, format, ap);
    va_end(ap);
    size_t l = strlen(b);
    if (l < sizeof b - 1) { b[l++] = '\n'; b[l] = 0; }
    write(2, b, l);
}

/* getrusage (AROS keeps no CPU accounting per task): AROS does not page, so the fault counts are
 * exactly 0; the times and sizes are not measured and reported as 0 too. */
#include <sys/resource.h>
int getrusage(int who, struct rusage *u)
{
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN) { errno = EINVAL; return -1; }
    memset(u, 0, sizeof *u);
    return 0;
}

/* pthread_sigmask: AROS's signals are per task, as a thread here is; sigprocmask serves */
#include <signal.h>
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    return sigprocmask(how, set, old) ? errno : 0;
}
/* alarm: AROS's C library delivers no SIGALRM; no alarm is set, there was none before (0) */
unsigned alarm(unsigned seconds) { (void)seconds; return 0; }
