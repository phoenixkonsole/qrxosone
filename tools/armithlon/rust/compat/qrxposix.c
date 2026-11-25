/*
 * libqrxaroscompat, part 6: POSIX functions that Gecko's crates (rustix, memmap2, std) and C++ code
 * call and that AROS's C library lacks. What AROS can do is done; what it cannot (device nodes,
 * setting the clock, process groups) fails with a POSIX error, never claims success.
 *
 *   *at functions          with AT_FDCWD or an absolute path ("/..." or "VOLUME:..."): AROS has no
 *                          directory descriptors; otherwise ENOSYS. The paths go to AROS's C library,
 *                          which converts Unix paths in its *nix mode
 *   utimensat, futimens    through utimes(); UTIME_NOW and UTIME_OMIT
 *   clock_getres           the resolution of clock_gettime (1 us: timer.device)
 *   clock_settime          EPERM: the clock is the host's
 *   dup3                   dup2 (O_CLOEXEC has no meaning without exec)
 *   fdatasync              fsync
 *   posix_fallocate        extends the file with ftruncate; posix_fadvise: no advice taken (0)
 *   preadv, pwritev        pread/pwrite per buffer
 *   waitid                 P_PID with WEXITED, through waitpid
 *   get/setpriority        the task priority of the calling process (PRIO_PROCESS, 0 = self)
 *   pthread_atfork         0: there is no fork() on AROS, so the handlers never run
 *   getpwuid_r             getpwuid copied into the caller's buffer
 *   dlopen, dlsym, ...     AROS has no dynamic linker for ELF objects: dlopen and dlsym return NULL
 *                          and dlerror says why; dladdr finds nothing (0)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* the values QRX OS One 64 uses (AROS's fcntl.h/sys/stat.h/sys/wait.h, the libc crate) */
#define QAT_FDCWD (-100)
#define QAT_EACCESS 0x100
#define QAT_SYMLINK_NOFOLLOW 0x200
#define QAT_REMOVEDIR 0x800
#define QUTIME_NOW ((1l << 30) - 1l)
#define QUTIME_OMIT ((1l << 30) - 2l)
#define QP_PID 1
#define QPRIO_PROCESS 0

int __get_default_file(int fd, long *fh);
ssize_t pread(int fd, void *buf, size_t n, off_t off);          /* qrxaroscompat.c */
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
int lchown(const char *path, uid_t owner, gid_t group);           /* qrxaroscompat.c */

/* A path the *at functions can take: relative to the current directory, or absolute */
static int at_ok(int dirfd, const char *path)
{
    if (dirfd == QAT_FDCWD || (path && (path[0] == '/' || strchr(path, ':')))) return 1;
    errno = ENOSYS;
    return 0;
}

int faccessat(int dirfd, const char *path, int mode, int flags)
{
    if (!at_ok(dirfd, path)) return -1;
    (void)flags;           /* AT_EACCESS: AROS has one user, effective = real */
    return access(path, mode);
}
static int to_timeval(const struct timespec *ts, struct timeval *tv, const char *path, int fd)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    struct stat st;
    int have = 0;
    for (int i = 0; i < 2; i++) {
        if (!ts || ts[i].tv_nsec == QUTIME_NOW) tv[i] = now;
        else if (ts[i].tv_nsec == QUTIME_OMIT) {
            if (!have) { if ((path ? stat(path, &st) : fstat(fd, &st))) return -1; have = 1; }
            tv[i].tv_sec = i ? st.st_mtime : st.st_atime; tv[i].tv_usec = 0;
        } else { tv[i].tv_sec = ts[i].tv_sec; tv[i].tv_usec = ts[i].tv_nsec / 1000; }
    }
    return 0;
}
int utimensat(int dirfd, const char *path, const struct timespec ts[2], int flags)
{
    (void)flags;
    if (!at_ok(dirfd, path)) return -1;
    if (ts && ts[0].tv_nsec == QUTIME_OMIT && ts[1].tv_nsec == QUTIME_OMIT) return 0;
    struct timeval tv[2];
    if (to_timeval(ts, tv, path, -1)) return -1;
    return utimes(path, tv);
}
int futimens(int fd, const struct timespec ts[2])
{
    long fh = 0;
    char name[1024];
    if (__get_default_file(fd, &fh) || !fh) { errno = EBADF; return -1; }
    if (!NameFromFH((BPTR)fh, (STRPTR)name, sizeof name)) { errno = EBADF; return -1; }
    if (ts && ts[0].tv_nsec == QUTIME_OMIT && ts[1].tv_nsec == QUTIME_OMIT) return 0;
    struct timeval tv[2];
    if (to_timeval(ts, tv, NULL, fd)) return -1;
    return utimes(name, tv);
}
int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev)
{
    (void)dirfd; (void)path; (void)mode; (void)dev;
    errno = ENOSYS;        /* AROS has no device nodes */
    return -1;
}
int clock_getres(clockid_t clk, struct timespec *res)
{
    (void)clk;
    if (res) { res->tv_sec = 0; res->tv_nsec = 1000; }
    return 0;
}
int clock_settime(clockid_t clk, const struct timespec *ts)
{
    (void)clk; (void)ts;
    errno = EPERM;         /* the guest's clock is the host's */
    return -1;
}
int dup3(int fd, int to, int flags)
{
    (void)flags;
    if (fd == to) { errno = EINVAL; return -1; }
    return dup2(fd, to);
}
int fdatasync(int fd) { return fsync(fd); }
int posix_fallocate(int fd, off_t off, off_t len)
{
    struct stat st;
    if (off < 0 || len <= 0) return EINVAL;
    if (fstat(fd, &st)) return errno;
    if (st.st_size >= off + len) return 0;
    return ftruncate(fd, off + len) ? errno : 0;
}
int posix_fadvise(int fd, off_t off, off_t len, int advice) { (void)fd; (void)off; (void)len; (void)advice; return 0; }
ssize_t preadv(int fd, const struct iovec *iov, int cnt, off_t off)
{
    ssize_t total = 0;
    for (int i = 0; i < cnt; i++) {
        ssize_t r = pread(fd, iov[i].iov_base, iov[i].iov_len, off + total);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].iov_len) break;
    }
    return total;
}
ssize_t pwritev(int fd, const struct iovec *iov, int cnt, off_t off)
{
    ssize_t total = 0;
    for (int i = 0; i < cnt; i++) {
        ssize_t r = pwrite(fd, iov[i].iov_base, iov[i].iov_len, off + total);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].iov_len) break;
    }
    return total;
}
int waitid(idtype_t idtype, id_t id, siginfo_t *info, int options)
{
    if (idtype != QP_PID) { errno = EINVAL; return -1; }
    int status = 0;
    pid_t r = waitpid((pid_t)id, &status, options & WNOHANG);
    if (r < 0) return -1;
    if (info) {
        memset(info, 0, sizeof *info);
        if (r > 0) { info->si_pid = r; info->si_signo = SIGCHLD; info->si_status = status & 0xff; }
    }
    return 0;
}
int getpriority(int which, id_t who)
{
    if (which != QPRIO_PROCESS || who != 0) { errno = ESRCH; return -1; }
    errno = 0;
    return -(int)FindTask(NULL)->tc_Node.ln_Pri;    /* nice: higher value, lower priority */
}
int setpriority(int which, id_t who, int prio)
{
    if (which != QPRIO_PROCESS || who != 0) { errno = ESRCH; return -1; }
    if (prio < -20) prio = -20;
    if (prio > 19) prio = 19;
    SetTaskPri(FindTask(NULL), -prio);
    return 0;
}
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
    (void)prepare; (void)parent; (void)child;
    return 0;
}
int getpwuid_r(uid_t uid, struct passwd *pw, char *buf, size_t len, struct passwd **res)
{
    struct passwd *p = getpwuid(uid);
    *res = NULL;
    if (!p) return 0;      /* no such user: not an error */
    size_t need = 0;
    const char *f[5] = { p->pw_name, p->pw_passwd, p->pw_gecos, p->pw_dir, p->pw_shell };
    for (int i = 0; i < 5; i++) need += (f[i] ? strlen(f[i]) : 0) + 1;
    if (need > len) return ERANGE;
    *pw = *p;
    char *o = buf;
    char **to[5] = { &pw->pw_name, &pw->pw_passwd, &pw->pw_gecos, &pw->pw_dir, &pw->pw_shell };
    for (int i = 0; i < 5; i++) {
        size_t k = f[i] ? strlen(f[i]) : 0;
        memcpy(o, f[i] ? f[i] : "", k); o[k] = 0;
        *to[i] = o; o += k + 1;
    }
    *res = pw;
    return 0;
}

/* dlfcn.h: AROS loads libraries as Exec libraries, not as ELF shared objects. */
static const char *qrx_dl_err;
void *dlopen(const char *file, int mode)
{
    (void)file; (void)mode;
    qrx_dl_err = "dlopen: AROS has no dynamic linker for ELF shared objects";
    return NULL;
}
void *dlsym(void *handle, const char *name)
{
    (void)handle; (void)name;
    qrx_dl_err = "dlsym: AROS has no dynamic linker for ELF shared objects";
    return NULL;
}
int dlclose(void *handle)
{
    (void)handle;
    qrx_dl_err = "dlclose: no such handle";
    return -1;
}
char *dlerror(void)
{
    const char *e = qrx_dl_err;
    qrx_dl_err = NULL;
    return (char *)e;
}
int dladdr(const void *addr, void *info)
{
    (void)addr; (void)info;
    return 0;
}

/* the rest of the *at family */
int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, unsigned int); va_end(ap); }
    if (!at_ok(dirfd, path)) return -1;
    return open(path, flags & ~(O_DIRECTORY | O_NOFOLLOW), mode);
}
int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    if (!at_ok(dirfd, path)) return -1;
    return (flags & QAT_SYMLINK_NOFOLLOW) ? lstat(path, st) : stat(path, st);
}
int mkdirat(int dirfd, const char *path, mode_t mode)
{
    if (!at_ok(dirfd, path)) return -1;
    return mkdir(path, mode);
}
int unlinkat(int dirfd, const char *path, int flags)
{
    if (!at_ok(dirfd, path)) return -1;
    return (flags & QAT_REMOVEDIR) ? rmdir(path) : unlink(path);
}
int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
{
    if (!at_ok(olddirfd, oldpath) || !at_ok(newdirfd, newpath)) return -1;
    return rename(oldpath, newpath);
}
int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags)
{
    (void)flags;
    if (!at_ok(olddirfd, oldpath) || !at_ok(newdirfd, newpath)) return -1;
    return link(oldpath, newpath);
}
int symlinkat(const char *target, int newdirfd, const char *linkpath)
{
    if (!at_ok(newdirfd, linkpath)) return -1;
    return symlink(target, linkpath);
}
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t len)
{
    if (!at_ok(dirfd, path)) return -1;
    return readlink(path, buf, len);
}
int fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
    (void)flags;
    if (!at_ok(dirfd, path)) return -1;
    return chmod(path, mode);
}
int fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flags)
{
    if (!at_ok(dirfd, path)) return -1;
    return (flags & QAT_SYMLINK_NOFOLLOW) ? lchown(path, owner, group) : chown(path, owner, group);
}

/* sigaltstack: AROS delivers signals on the task's own stack; there is no alternate stack */
int sigaltstack(const stack_t *ss, stack_t *old)
{
    if (old) { memset(old, 0, sizeof *old); old->ss_flags = SS_DISABLE; }
    if (ss) { errno = ENOSYS; return -1; }
    return 0;
}

/* pthread_mutexattr_setpshared: every AROS task shares one address space, so a mutex in shared
 * memory works between processes as it is; both values are accepted.
 * pthread_condattr_setclock: AROS's pthread_cond_timedwait measures against the real-time clock
 * (gettimeofday); only CLOCK_REALTIME can be honoured. */
#include <pthread.h>
int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared)
{
    (void)attr;
    return (pshared == PTHREAD_PROCESS_PRIVATE || pshared == PTHREAD_PROCESS_SHARED) ? 0 : EINVAL;
}
int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock)
{
    (void)attr;
    return clock == CLOCK_REALTIME ? 0 : EINVAL;
}
