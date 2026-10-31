/* QRX OS One 22.5.12 - Linux i386 user-mode personality for qrx86core.
 * Loads static or dynamically linked (PT_INTERP) ELF32 i386 programs and
 * implements the int 0x80 system call ABI on top of portable POSIX host calls
 * (Linux host for tests, AROS posixc on the m68k target).
 *
 * Processes: fork/vfork/clone(SIGCHLD) create a child with a private copy of
 * the guest address space.  All processes of a session run under a cooperative
 * scheduler in one host thread (time slices via the instruction budget); pipe
 * reads/writes and wait4 block by re-executing their int 0x80 in a later slice.
 * Pipes are in-process 64 KiB buffers, so shell pipelines need no host fork().
 * Threads (clone with CLONE_VM) are not supported and return -ENOSYS. */
#ifndef QRX86LINUX_H
#define QRX86LINUX_H
#include "../core/qrx86core.h"

typedef struct lxopts {
    const char *root;      /* guest "/" maps to this host directory ("" = host /) */
    int trace;             /* 1 = log system calls to stderr */
    uint64_t budget;       /* per-process instruction budget, 0 = unlimited */
} lxopts;

/* Run a program.  'path' is a guest path (resolved under root).  Returns the
 * exit status (0-255), or 128+signal-like codes: 139 guest fault, 127 load error. */
int lx_run(const lxopts *o, const char *path, int argc, char **argv, char **envp);

#endif
