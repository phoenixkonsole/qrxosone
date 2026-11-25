/*
 * libqrxaroscompat, part 4: sys/mman.h for QRX OS One 64 (the header is in the AROS patch,
 * compiler/crt/posixc/include/aros/posixc/sys/mman.h).
 *
 * AROS runs every program in one address space without paging, so a "mapping" is a block of
 * memory: anonymous private mappings come zeroed and page-aligned (from 1 MB up aligned to 1 MB,
 * what SpiderMonkey's GC asks for), munmap of a whole mapping frees it. Unmapping part of a mapping
 * keeps the memory (it cannot be given back in pieces); MADV_DONTNEED zeroes the range, as Linux
 * does for anonymous memory. mprotect, msync, mlock: accepted, no effect (no MMU protection here).
 * File mappings fail with ENODEV.
 */
#define _GNU_SOURCE   /* posix_memalign in AROS's stdlib.h */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <proto/exec.h>
#include <exec/semaphores.h>

struct region { uintptr_t base; size_t len; };
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

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    (void)prot; (void)off;
    if (!len) { errno = EINVAL; return MAP_FAILED; }
    if (!(flags & MAP_ANON) || fd != -1) { errno = ENODEV; return MAP_FAILED; }
    len = (len + 4095) & ~(size_t)4095;
    lock_init();
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
    ObtainSemaphore(&lock);
    if (nregions == cap) {
        size_t ncap = cap ? cap * 2 : 64;
        struct region *n = realloc(regions, ncap * sizeof *n);
        if (!n) { ReleaseSemaphore(&lock); free(p); errno = ENOMEM; return MAP_FAILED; }
        regions = n; cap = ncap;
    }
    regions[nregions].base = (uintptr_t)p;
    regions[nregions].len = len;
    nregions++;
    ReleaseSemaphore(&lock);
    return p;
}

int munmap(void *addr, size_t len)
{
    if (!len) { errno = EINVAL; return -1; }
    lock_init();
    ObtainSemaphore(&lock);
    for (size_t i = 0; i < nregions; i++) {
        struct region *r = &regions[i];
        if ((uintptr_t)addr == r->base && len >= r->len) {
            void *p = (void *)r->base;
            regions[i] = regions[--nregions];
            ReleaseSemaphore(&lock);
            free(p);
            return 0;
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
int msync(void *addr, size_t len, int flags) { (void)addr; (void)len; (void)flags; return 0; }
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
