/*
 * libqrxaroscompat, part 7: one lock around AROS's descriptor table.
 *
 * AROS's C library keeps the descriptors (fdesc, fcb) of all the program's tasks in one table
 * without any locking ("TODO: Add locking to make filedesc usage thread safe", posixc/__fdesc.c),
 * and frees them into an exec memory pool that has no semaphore either. A program whose threads
 * open and close files at the same time (Gecko: its I/O threads, SQLite) runs on QRX OS One 64's
 * cores truly in parallel and corrupts both: a freed fcb comes back as another thread's memory,
 * and a file system then gets a lock or handle that is now someone's string.
 *
 * So everything that adds or removes a descriptor goes through one SignalSemaphore here: open,
 * creat, close, dup, dup2, pipe, mkstemp, opendir, closedir, fopen, fdopen, fclose and fcntl's
 * F_DUPFD (qrxnet.c, qrxshm.c call qrx_fd_lock). Reads, writes and seeks on descriptors that
 * exist are not serialized: they only look a descriptor up, and the table's blocks never move.
 * The semaphore nests, so a wrapper may call another.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <dirent.h>
#include <exec/semaphores.h>
#include <proto/exec.h>

static struct SignalSemaphore fd_sem;
volatile int qrx_fd_sem_ready;   /* 1 once the constructor ran */

/* before main (one task then): the semaphore exists before any thread can want it */
__attribute__((constructor)) static void fd_sem_init(void)
{
    InitSemaphore(&fd_sem);
    qrx_fd_sem_ready = 1;
}
void qrx_fd_lock(void) { if (qrx_fd_sem_ready) ObtainSemaphore(&fd_sem); }
void qrx_fd_unlock(void) { if (qrx_fd_sem_ready) ReleaseSemaphore(&fd_sem); }

int __open_CrtBase_wrapper(const char *, int, ...);
int __creat_CrtBase_wrapper(const char *, int);
int __pipe_CrtBase_wrapper(int *);
int __mkstemp_CrtBase_wrapper(char *);
DIR *__opendir_CrtBase_wrapper(const char *);
int __closedir_CrtBase_wrapper(DIR *);
FILE *__fopen_CrtBase_wrapper(const char *, const char *);
FILE *__fdopen_CrtBase_wrapper(int, const char *);
int __fclose_CrtBase_wrapper(FILE *);

#define LOCKED(T, call) do { qrx_fd_lock(); T r_ = call; int e_ = errno; qrx_fd_unlock(); errno = e_; return r_; } while (0)

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, unsigned int); va_end(ap); }
    LOCKED(int, __open_CrtBase_wrapper(path, flags, mode));
}
int creat(const char *path, int mode) { LOCKED(int, __creat_CrtBase_wrapper(path, mode)); }
int pipe(int fds[2]) { LOCKED(int, __pipe_CrtBase_wrapper(fds)); }
int mkstemp(char *tmpl) { LOCKED(int, __mkstemp_CrtBase_wrapper(tmpl)); }
DIR *opendir(const char *path) { LOCKED(DIR *, __opendir_CrtBase_wrapper(path)); }
int closedir(DIR *d) { LOCKED(int, __closedir_CrtBase_wrapper(d)); }
FILE *fopen(const char *path, const char *mode) { LOCKED(FILE *, __fopen_CrtBase_wrapper(path, mode)); }
FILE *fdopen(int fd, const char *mode) { LOCKED(FILE *, __fdopen_CrtBase_wrapper(fd, mode)); }
int fclose(FILE *f) { LOCKED(int, __fclose_CrtBase_wrapper(f)); }
