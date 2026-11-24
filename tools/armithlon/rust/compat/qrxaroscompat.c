/*
 * libqrxaroscompat, part 1: POSIX functions that Rust's std (and ported C code) calls and that
 * AROS's C library (libcrt) lacks, for m68k64-unknown-aros (QRX OS One 64). Built by
 * build-compat.sh, linked by the target spec. Sockets, poll and the descriptor functions are in
 * qrxnet.c.
 *
 *   strerror_r            real
 *   pread, pwrite         move the file position and put it back: not atomic towards other
 *                         threads using the same descriptor
 *   arc4random_buf,       the host's getentropy() through the board (network service 4, op 19):
 *   getentropy            cryptographic. Without the host door the program stops (abort) rather
 *                         than run on guessable numbers.
 *   pipe2                 pipe(); the flags are emulated (O_NONBLOCK) or not needed (O_CLOEXEC,
 *                         see qrx_spawn)
 *   qrx_spawn, waitpid,   start a program as an AROS process (LoadSeg, CreateNewProc), its return
 *   kill                  code through NP_ExitCode; see below
 *   mkfifo, lchown, chroot fail with ENOSYS (AROS has no FIFOs, no owners, no chroot)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

int qrx_host_random(void *buf, size_t n);      /* qrxnet.c */
int qrx_host_door(void);

int strerror_r(int errnum, char *buf, size_t buflen)
{
    const char *s = strerror(errnum);
    size_t n;
    if (!buflen) return ERANGE;
    if (!s) s = "Unknown error";
    n = strlen(s);
    if (n >= buflen) { memcpy(buf, s, buflen - 1); buf[buflen - 1] = 0; return ERANGE; }
    memcpy(buf, s, n + 1);
    return 0;
}

ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
    off_t old = lseek(fd, 0, SEEK_CUR);
    ssize_t r;
    int e;
    if (old < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
    r = read(fd, buf, n);
    e = errno;
    lseek(fd, old, SEEK_SET);
    errno = e;
    return r;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    off_t old = lseek(fd, 0, SEEK_CUR);
    ssize_t r;
    int e;
    if (old < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
    r = write(fd, buf, n);
    e = errno;
    lseek(fd, old, SEEK_SET);
    errno = e;
    return r;
}

int getentropy(void *buf, size_t n)
{
    if (n > 256) { errno = EIO; return -1; }
    if (qrx_host_random(buf, n)) { errno = EIO; return -1; }
    return 0;
}

void arc4random_buf(void *buf, size_t n)
{
    if (qrx_host_random(buf, n)) {
        static const char msg[] = "qrxaroscompat: no entropy source (the board's host door does not answer)\n";
        write(2, msg, sizeof msg - 1);
        abort();
    }
}

int pipe2(int fds[2], int flags)
{
    if (pipe(fds)) return -1;
    if (flags & O_NONBLOCK) {
        fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
        fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
    }
    return 0;
}

/*
 * qrx_spawn: runs path with argv as a new AROS process and returns its pid, or -1 with errno.
 * in/out/err: descriptors for the child's stdin/stdout/stderr, or -1 for the parent's. cwd: NULL
 * or the child's directory. envp is not passed: AROS processes share the variables (ENV:, the
 * shell's locals), there is no per-process environment to hand over.
 *
 * AROS's own vfork() does not work on the 68100EC64 (it returns 1 and never runs the child), so
 * this is AROS's native way: LoadSeg + CreateNewProc, the arguments as one quoted line, the
 * stdio as file handles of the child's own: a pipe end is opened again by its PIPE: name, a file
 * through DupLockFromFH, so the parent may close its descriptors right away (and a pipe's reader
 * sees the end of the data when the child ends). The child's return code arrives through
 * NP_ExitCode; waitpid and kill below serve these pids and pass all others to AROS.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <signal.h>

#define QRX_PID_BASE 0x51500000
#define NCHILD 64
struct qchild { volatile int used, done, code; struct Process *proc; };
static struct qchild children[NCHILD];
int __get_default_file(int fd, long *fh);
int qrx_pipe_name(BPTR fh, char *buf, size_t n);     /* qrxnet.c */
pid_t __waitpid_CrtBase_wrapper(pid_t, int *, int);
int __kill_CrtBase_wrapper(pid_t, int);

static void child_exit(IPTR result, IPTR data)
{
    struct qchild *c = (struct qchild *)data;
    c->code = (int)result;
    c->done = 1;
}

/* a handle of the child's own for descriptor fd: 1 when it is ours to close, 0 when shared */
static int child_handle(int fd, LONG mode, BPTR fallback, BPTR *fh)
{
    long h = 0;
    char name[256];
    if (fd < 0) { *fh = fallback; return 0; }
    if (__get_default_file(fd, &h) || !h) { errno = EBADF; return -1; }
    if (qrx_pipe_name((BPTR)h, name + 5, sizeof name - 5) == 0) {
        memcpy(name, "PIPE:", 5);
        *fh = Open((CONST_STRPTR)name, mode);
        if (*fh) return 1;
    } else {
        BPTR lock = DupLockFromFH((BPTR)h);
        if (lock) {
            *fh = OpenFromLock(lock);
            if (*fh) {
                if (mode == MODE_NEWFILE) Seek(*fh, 0, OFFSET_END);
                return 1;
            }
            UnLock(lock);
        }
    }
    *fh = (BPTR)h;          /* a console or the like: shared, the parent keeps it open */
    return 0;
}

static void add_arg(char *out, size_t cap, size_t *n, const char *a)
{
    int quote = !*a || strpbrk(a, " \t\"*\n;=") != NULL;
    if (quote && *n < cap) out[(*n)++] = '"';
    for (; *a && *n + 3 < cap; a++) {
        if (*a == '"' || *a == '*') { out[(*n)++] = '*'; out[(*n)++] = *a; }
        else if (*a == '\n') { out[(*n)++] = '*'; out[(*n)++] = 'N'; }
        else out[(*n)++] = *a;
    }
    if (quote && *n < cap) out[(*n)++] = '"';
}

pid_t qrx_spawn(const char *path, char *const argv[], char *const envp[], int in, int out, int err,
                const char *cwd, int *exec_errno)
{
    (void)envp;
    char prog[512];
    if (exec_errno) *exec_errno = 0;
    /* a Unix-style /C/Echo means C:Echo */
    if (path[0] == '/' && strchr(path + 1, '/')) {
        const char *s = strchr(path + 1, '/');
        snprintf(prog, sizeof prog, "%.*s:%s", (int)(s - path - 1), path + 1, s + 1);
    } else snprintf(prog, sizeof prog, "%s", path);
    BPTR seg = LoadSeg((CONST_STRPTR)prog);
    if (!seg && !strchr(prog, ':') && !strchr(prog, '/')) {
        char c[520];
        snprintf(c, sizeof c, "C:%s", prog);
        seg = LoadSeg((CONST_STRPTR)c);
    }
    if (!seg) { errno = ENOENT; return -1; }

    size_t n = 0, cap = 4096;
    char *args = malloc(cap + 2);
    if (!args) { UnLoadSeg(seg); errno = ENOMEM; return -1; }
    for (int i = 1; argv && argv[i]; i++) {
        if (i > 1 && n < cap) args[n++] = ' ';
        add_arg(args, cap, &n, argv[i]);
    }
    args[n++] = '\n'; args[n] = 0;

    int slot = -1;
    Forbid();
    for (int i = 0; i < NCHILD; i++) if (!children[i].used) { slot = i; children[i].used = 1; break; }
    Permit();
    if (slot < 0) { free(args); UnLoadSeg(seg); errno = EAGAIN; return -1; }
    struct qchild *c = &children[slot];
    c->done = 0; c->code = 0;

    BPTR fin, fout, ferr;
    int cin = child_handle(in, MODE_OLDFILE, Input(), &fin);
    int cout = child_handle(out, MODE_NEWFILE, Output(), &fout);
    struct Process *me = (struct Process *)FindTask(NULL);
    int cerr = child_handle(err, MODE_NEWFILE, me->pr_CES ? me->pr_CES : Output(), &ferr);
    BPTR dir = cwd ? Lock((CONST_STRPTR)cwd, SHARED_LOCK) : DupLock(me->pr_CurrentDir);
    if (cin < 0 || cout < 0 || cerr < 0 || (cwd && !dir)) {
        int e = cwd && !dir ? ENOENT : EBADF;
        if (cin > 0) Close(fin);
        if (cout > 0) Close(fout);
        if (cerr > 0) Close(ferr);
        if (dir) UnLock(dir);
        free(args); UnLoadSeg(seg); c->used = 0;
        errno = e;
        return -1;
    }
    const char *base = argv && argv[0] ? argv[0] : prog;
    struct Process *p = CreateNewProcTags(
        NP_Seglist, (IPTR)seg, NP_FreeSeglist, TRUE,
        NP_Name, (IPTR)base, NP_CommandName, (IPTR)base,
        NP_Arguments, (IPTR)args,
        NP_Input, (IPTR)fin, NP_CloseInput, cin > 0,
        NP_Output, (IPTR)fout, NP_CloseOutput, cout > 0,
        NP_Error, (IPTR)ferr, NP_CloseError, cerr > 0,
        NP_CurrentDir, (IPTR)dir,
        NP_Cli, TRUE,
        NP_StackSize, 262144,
        NP_ExitCode, (IPTR)child_exit, NP_ExitData, (IPTR)c,
        TAG_DONE);
    free(args);
    if (!p) {
        if (cin > 0) Close(fin);
        if (cout > 0) Close(fout);
        if (cerr > 0) Close(ferr);
        UnLock(dir); UnLoadSeg(seg); c->used = 0;
        errno = ENOMEM;
        return -1;
    }
    c->proc = p;
    return QRX_PID_BASE + slot;
}

static struct qchild *child_of(pid_t pid)
{
    long s = (long)pid - QRX_PID_BASE;
    return s >= 0 && s < NCHILD && children[s].used ? &children[s] : NULL;
}
pid_t waitpid(pid_t pid, int *status, int options)
{
    if (pid < QRX_PID_BASE || pid >= QRX_PID_BASE + NCHILD) return __waitpid_CrtBase_wrapper(pid, status, options);
    struct qchild *c = child_of(pid);
    if (!c) { errno = ECHILD; return -1; }
    unsigned step = 1;
    while (!c->done) {
        if (options & WNOHANG) return 0;
        usleep(step * 1000u);
        if (step < 20) step++;
    }
    if (status) *status = c->code & 0xff;
    c->used = 0;
    return pid;
}
int kill(pid_t pid, int sig)
{
    if (pid < QRX_PID_BASE || pid >= QRX_PID_BASE + NCHILD) return __kill_CrtBase_wrapper(pid, sig);
    struct qchild *c = child_of(pid);
    if (!c) { errno = ESRCH; return -1; }
    if (sig == 0) return 0;
    Forbid();
    if (!c->done) Signal(&c->proc->pr_Task, SIGBREAKF_CTRL_C);    /* AROS's way to ask a program to stop */
    Permit();
    return 0;
}

int mkfifo(const char *path, mode_t mode) { (void)path; (void)mode; errno = ENOSYS; return -1; }
int lchown(const char *path, uid_t o, gid_t g) { (void)path; (void)o; (void)g; errno = ENOSYS; return -1; }
int chroot(const char *path) { (void)path; errno = ENOSYS; return -1; }
