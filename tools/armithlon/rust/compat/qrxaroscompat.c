/*
 * libqrxaroscompat: POSIX functions that Rust's std calls and that AROS's C library (libcrt) lacks,
 * for m68k64-unknown-aros (QRX OS One 64). Built by build-compat.sh, linked by the target spec.
 *
 * What is real here and what is not:
 *   strerror_r, pread, pwrite        real (pread/pwrite move the file position and put it back:
 *                                    not atomic towards other threads using the same descriptor)
 *   poll                             reports every requested event as ready (AROS's libcrt has no
 *                                    select either); right for files, wrong for an empty pipe/console
 *   arc4random_buf                   NOT cryptographic: AROS has no entropy source. Mixes the clock,
 *                                    the task address and a counter through splitmix64. Enough for
 *                                    HashMap seeds, not for keys.
 *   sockets, getaddrinfo, gethostname, mkfifo, lchown, chroot
 *                                    fail with ENOSYS / EAI_FAIL: std::net does not work yet
 *                                    (AROS sockets live in bsdsocket.library with their own descriptors)
 */
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>

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

struct pollfd { int fd; short events; short revents; };
int poll(struct pollfd *fds, unsigned long nfds, int timeout)
{
    unsigned long i;
    int ready = 0;
    for (i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        fds[i].revents = fds[i].events & (0x1 | 0x4 | 0x40);     /* POLLIN | POLLOUT | POLLRDNORM */
        if (fds[i].revents) ready++;
    }
    if (!ready && timeout > 0) usleep((useconds_t)timeout * 1000);
    return ready;
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

static uint64_t qrx_rng_state;
static uint64_t splitmix64(void)
{
    uint64_t z = (qrx_rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
void arc4random_buf(void *buf, size_t n)
{
    struct timespec ts;
    unsigned char *p = buf;
    clock_gettime(CLOCK_REALTIME, &ts);
    qrx_rng_state ^= ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec ^ (uint64_t)(uintptr_t)&ts
                     ^ ((uint64_t)getpid() << 17);
    while (n) {
        uint64_t v = splitmix64();
        size_t k = n < 8 ? n : 8;
        memcpy(p, &v, k);
        p += k; n -= k;
    }
}

#define QRX_NOSYS(decl) decl { errno = ENOSYS; return -1; }
QRX_NOSYS(int socket(int d, int t, int p))
QRX_NOSYS(int socketpair(int d, int t, int p, int *sv))
QRX_NOSYS(int bind(int s, const void *a, unsigned l))
QRX_NOSYS(int connect(int s, const void *a, unsigned l))
QRX_NOSYS(int listen(int s, int b))
QRX_NOSYS(int accept(int s, void *a, unsigned *l))
QRX_NOSYS(int shutdown(int s, int how))
QRX_NOSYS(int getsockname(int s, void *a, unsigned *l))
QRX_NOSYS(int getpeername(int s, void *a, unsigned *l))
QRX_NOSYS(int getsockopt(int s, int lv, int o, void *v, unsigned *l))
QRX_NOSYS(int setsockopt(int s, int lv, int o, const void *v, unsigned l))
QRX_NOSYS(ssize_t send(int s, const void *b, size_t n, int f))
QRX_NOSYS(ssize_t recv(int s, void *b, size_t n, int f))
QRX_NOSYS(ssize_t sendto(int s, const void *b, size_t n, int f, const void *a, unsigned l))
QRX_NOSYS(ssize_t recvfrom(int s, void *b, size_t n, int f, void *a, unsigned *l))
QRX_NOSYS(int gethostname(char *name, size_t len))
QRX_NOSYS(int mkfifo(const char *path, mode_t mode))
QRX_NOSYS(int lchown(const char *path, uid_t o, gid_t g))
QRX_NOSYS(int chroot(const char *path))

int getaddrinfo(const char *node, const char *service, const void *hints, void **res)
{
    (void)node; (void)service; (void)hints;
    *res = 0;
    return 4;   /* EAI_FAIL in AROS's netdb.h */
}
void freeaddrinfo(void *ai) { (void)ai; }
