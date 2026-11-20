/* Door service 11: host control for the appliance (Wi-Fi, Bluetooth, power) from QRX OS One's
 * settings, see qrx_hostctl.h. The request names a command and its arguments; the engine runs
 * the control program ($QRX_NETCTL, /qrx/netctl on the Raspberry Pi image) directly, without a
 * shell, so guest text never becomes shell syntax. Its output goes back into the guest buffer.
 *
 * Request block (big endian longs, as the other services): q[0] = 11, q[4] guest address of the
 * arguments (NUL-separated strings, ending in an empty one), q[5] output buffer, q[6] its size.
 * Result: q[12] = bytes written, or 0xFFFFFFFF (no control program, bad request, failed);
 * q[13] = the program's exit status. */
#include "qrx_hostctl.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_ARGS 8
#define MAX_ARG_BYTES 512
#define TIMEOUT_MS 8000

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }

/* runs prog with args, output into out (at most cap bytes); returns bytes or -1 */
static long run(const char *prog, char *const argv[], uint8_t *out, uint32_t cap, int *status)
{
    int fd[2];
    if (pipe(fd)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(fd[1], 1); dup2(fd[1], 2);
        close(fd[0]); close(fd[1]);
        execv(prog, argv);
        _exit(127);
    }
    close(fd[1]);
    uint32_t got = 0;
    uint64_t end = now_ms() + TIMEOUT_MS;
    for (;;) {
        uint64_t t = now_ms();
        if (t >= end) { kill(pid, SIGKILL); break; }
        struct pollfd p = {fd[0], POLLIN, 0};
        if (poll(&p, 1, (int)(end - t)) <= 0) continue;
        uint8_t tmp[1024];
        ssize_t n = read(fd[0], tmp, sizeof tmp);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        uint32_t k = (uint32_t)n < cap - got ? (uint32_t)n : cap - got;
        memcpy(out + got, tmp, k);
        got += k;
    }
    close(fd[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    *status = WIFEXITED(st) ? WEXITSTATUS(st) : 255;
    return got;
}

void qrx_hostctl_call(uint8_t *ram, uint64_t ram_size, uint8_t *q)
{
    const char *prog = getenv("QRX_NETCTL");
    uint32_t args = be32(q + 16), out = be32(q + 20), cap = be32(q + 24);
    char buf[MAX_ARG_BYTES + 1], *argv[MAX_ARGS + 2];
    int argc = 1, status = 255;
    put_be32(q + 48, UINT32_MAX);
    put_be32(q + 52, 255);
    if (!prog || !*prog || access(prog, X_OK)) return;
    if ((uint64_t)args + MAX_ARG_BYTES > ram_size || (uint64_t)out + cap > ram_size || cap < 1 || cap > (1u << 20)) return;
    memcpy(buf, ram + args, MAX_ARG_BYTES);
    buf[MAX_ARG_BYTES] = 0;
    argv[0] = (char *)prog;
    for (size_t i = 0; argc <= MAX_ARGS && i < MAX_ARG_BYTES && buf[i]; argc++) {
        argv[argc] = buf + i;
        i += strlen(buf + i) + 1;
    }
    argv[argc] = NULL;
    if (argc < 2) return;
    long n = run(prog, argv, ram + out, cap - 1, &status);
    if (n < 0) return;
    ram[out + (uint32_t)n] = 0;
    put_be32(q + 48, (uint32_t)n);
    put_be32(q + 52, (uint32_t)status);
}
