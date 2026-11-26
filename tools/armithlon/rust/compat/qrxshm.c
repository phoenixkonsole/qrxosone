/*
 * libqrxaroscompat, part 5: POSIX shared memory for QRX OS One 64 (Gecko's IPC: shm_open + ftruncate
 * + mmap(MAP_SHARED)). AROS runs every program in one address space, so sharing is plain: an object
 * is one block of memory, and every mmap of it returns that same block.
 *
 *   shm_open/shm_unlink  named objects; a descriptor from QRX_SHM_FD up (not AROS's, like the
 *                        sockets of qrxnet.c)
 *   ftruncate            sets the size; growing keeps the contents, but only while nothing maps it
 *   fstat                size and mode of the object
 *   mmap(MAP_SHARED, fd) the object's block (plus the offset); munmap drops that reference
 *   dup/dup2/close/fcntl for these descriptors
 * The object is freed when it is unlinked and neither open nor mapped.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <proto/exec.h>
#include <exec/semaphores.h>

#define QRX_SHM_FD 0x50000
#define NSHMFD 256
#define NSHMOBJ 128
#define NSHMMAP 512

struct shmobj { int used, unlinked; char name[96]; void *mem; size_t size; int fds, maps; };
static struct shmobj objs[NSHMOBJ];
static struct { int used, obj, rdonly, cloexec; } fds[NSHMFD];
static struct { uintptr_t base; size_t len; int obj; } maps[NSHMMAP];
static struct SignalSemaphore shm_lock;
static volatile int shm_lock_ready;

static void shm_init(void)
{
    if (shm_lock_ready) return;
    Forbid();
    if (!shm_lock_ready) { InitSemaphore(&shm_lock); shm_lock_ready = 1; }
    Permit();
}
#define LOCK() (shm_init(), ObtainSemaphore(&shm_lock))
#define UNLOCK() ReleaseSemaphore(&shm_lock)

int qrx_is_shm_fd(int fd) { return fd >= QRX_SHM_FD && fd < QRX_SHM_FD + NSHMFD && fds[fd - QRX_SHM_FD].used; }

static void maybe_free(int o)    /* with the lock */
{
    struct shmobj *s = &objs[o];
    if (s->unlinked && !s->fds && !s->maps) { free(s->mem); memset(s, 0, sizeof *s); }
}
static int new_fd(int o, int rdonly)   /* with the lock */
{
    for (int i = 0; i < NSHMFD; i++) if (!fds[i].used) {
        fds[i].used = 1; fds[i].obj = o; fds[i].rdonly = rdonly; fds[i].cloexec = 0;
        objs[o].fds++;
        return QRX_SHM_FD + i;
    }
    errno = EMFILE;
    return -1;
}

int shm_open(const char *name, int oflag, mode_t mode)
{
    (void)mode;
    if (!name || strlen(name) >= sizeof objs[0].name) { errno = ENAMETOOLONG; return -1; }
    LOCK();
    int o = -1;
    for (int i = 0; i < NSHMOBJ; i++) if (objs[i].used && !objs[i].unlinked && !strcmp(objs[i].name, name)) { o = i; break; }
    if (o >= 0 && (oflag & O_CREAT) && (oflag & O_EXCL)) { UNLOCK(); errno = EEXIST; return -1; }
    if (o < 0) {
        if (!(oflag & O_CREAT)) { UNLOCK(); errno = ENOENT; return -1; }
        for (int i = 0; i < NSHMOBJ; i++) if (!objs[i].used) { o = i; break; }
        if (o < 0) { UNLOCK(); errno = ENFILE; return -1; }
        memset(&objs[o], 0, sizeof objs[o]);
        objs[o].used = 1;
        strcpy(objs[o].name, name);
    }
    if ((oflag & O_TRUNC) && objs[o].size && !objs[o].maps) { free(objs[o].mem); objs[o].mem = NULL; objs[o].size = 0; }
    int fd = new_fd(o, (oflag & O_ACCMODE) == O_RDONLY);
    if (fd < 0 && !objs[o].fds) maybe_free(o);
    UNLOCK();
    return fd;
}
int shm_unlink(const char *name)
{
    LOCK();
    for (int i = 0; i < NSHMOBJ; i++) if (objs[i].used && !objs[i].unlinked && !strcmp(objs[i].name, name)) {
        objs[i].unlinked = 1; objs[i].name[0] = 0;
        maybe_free(i);
        UNLOCK();
        return 0;
    }
    UNLOCK();
    errno = ENOENT;
    return -1;
}

/* the descriptor functions of AROS's C library, for all others */
int __ftruncate_CrtBase_wrapper(int, off_t);
int __fstat_CrtBase_wrapper(int, struct stat *);
int __dup_CrtBase_wrapper(int);
int __dup2_CrtBase_wrapper(int, int);

int ftruncate(int fd, off_t len)
{
    if (!qrx_is_shm_fd(fd)) return __ftruncate_CrtBase_wrapper(fd, len);
    if (len < 0) { errno = EINVAL; return -1; }
    LOCK();
    int f = fd - QRX_SHM_FD;
    struct shmobj *s = &objs[fds[f].obj];
    if (fds[f].rdonly) { UNLOCK(); errno = EBADF; return -1; }
    size_t n = (size_t)len;
    if (n == s->size) { UNLOCK(); return 0; }
    if (s->maps) { UNLOCK(); errno = EBUSY; return -1; }      /* the block cannot move while mapped */
    void *m = NULL;
    if (n && posix_memalign(&m, 4096, (n + 4095) & ~(size_t)4095)) { UNLOCK(); errno = ENOMEM; return -1; }
    if (m) {
        memset(m, 0, (n + 4095) & ~(size_t)4095);
        if (s->mem) memcpy(m, s->mem, n < s->size ? n : s->size);
    }
    free(s->mem);
    s->mem = m; s->size = n;
    UNLOCK();
    return 0;
}
int fstat(int fd, struct stat *st)
{
    if (!qrx_is_shm_fd(fd)) return __fstat_CrtBase_wrapper(fd, st);
    memset(st, 0, sizeof *st);
    LOCK();
    st->st_size = (off_t)objs[fds[fd - QRX_SHM_FD].obj].size;
    UNLOCK();
    st->st_mode = S_IFREG | 0600;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    return 0;
}
void qrx_fd_lock(void);           /* qrxfd.c */
void qrx_fd_unlock(void);
int dup(int fd)
{
    if (!qrx_is_shm_fd(fd)) { qrx_fd_lock(); int r = __dup_CrtBase_wrapper(fd), e = errno; qrx_fd_unlock(); errno = e; return r; }
    LOCK();
    int f = fd - QRX_SHM_FD;
    int r = new_fd(fds[f].obj, fds[f].rdonly);
    UNLOCK();
    return r;
}
int dup2(int fd, int to)
{
    if (!qrx_is_shm_fd(fd)) { qrx_fd_lock(); int r = __dup2_CrtBase_wrapper(fd, to), e = errno; qrx_fd_unlock(); errno = e; return r; }
    errno = EINVAL;     /* a shared memory descriptor goes only to a new number (dup) */
    return -1;
}
/* close and fcntl of these descriptors (qrxnet.c's close and fcntl call these first) */
int qrx_shm_close(int fd)
{
    LOCK();
    int f = fd - QRX_SHM_FD;
    int o = fds[f].obj;
    fds[f].used = 0;
    objs[o].fds--;
    maybe_free(o);
    UNLOCK();
    return 0;
}
int qrx_shm_fcntl(int fd, int cmd, long arg)
{
    int f = fd - QRX_SHM_FD;
    switch (cmd) {
    case F_GETFD: return fds[f].cloexec ? FD_CLOEXEC : 0;
    case F_SETFD: fds[f].cloexec = (arg & FD_CLOEXEC) != 0; return 0;
    case F_GETFL: return fds[f].rdonly ? O_RDONLY : O_RDWR;
    case F_SETFL: return 0;
    case F_DUPFD: return dup(fd);
    default: errno = EINVAL; return -1;
    }
}
/* mmap/munmap of the objects (qrxmman.c's mmap and munmap call these) */
void *qrx_shm_mmap(size_t len, int prot, int fd, off_t off)
{
    LOCK();
    int f = fd - QRX_SHM_FD, o = fds[f].obj;
    struct shmobj *s = &objs[o];
    if ((prot & PROT_WRITE) && fds[f].rdonly) { UNLOCK(); errno = EACCES; return MAP_FAILED; }
    if (off < 0 || (size_t)off + len > ((s->size + 4095) & ~(size_t)4095) || !s->mem) { UNLOCK(); errno = ENXIO; return MAP_FAILED; }
    int m = -1;
    for (int i = 0; i < NSHMMAP; i++) if (!maps[i].len) { m = i; break; }
    if (m < 0) { UNLOCK(); errno = ENOMEM; return MAP_FAILED; }
    maps[m].base = (uintptr_t)s->mem + (uintptr_t)off; maps[m].len = len; maps[m].obj = o;
    s->maps++;
    void *p = (char *)s->mem + off;
    UNLOCK();
    return p;
}
/* 1 when addr..len was a mapping of an object (then dropped), 0 when not ours */
int qrx_shm_munmap(void *addr, size_t len)
{
    (void)len;
    LOCK();
    for (int i = 0; i < NSHMMAP; i++) if (maps[i].len && maps[i].base == (uintptr_t)addr) {
        int o = maps[i].obj;
        maps[i].len = 0;
        objs[o].maps--;
        maybe_free(o);
        UNLOCK();
        return 1;
    }
    UNLOCK();
    return 0;
}
