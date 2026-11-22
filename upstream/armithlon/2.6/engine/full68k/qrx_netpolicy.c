/* The guest's network through Tor or WireGuard (qrx_netpolicy.h).
 *
 * The settings live in ~/Library/Application Support/QRX OS One/net.cfg (see qrxnetd's main.rs for
 * the keys); the guest writes them with QRX_NET_CONFIG. With mode tor or wireguard the engine
 * starts qrxnetd (next to the engine binary, or QRX_NETD), and every guest connection goes through
 * it. Until qrxnetd says READY, and whenever it is not running, guest connections and lookups fail:
 * there is no path from the guest to the network that does not go through the tunnel. */
#include "qrx_netpolicy.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int mode = -1;                 /* -1 not loaded yet, 0 direct, 1 tor, 2 wireguard, 3 offline */
static volatile int ready;
static char sock_path[104];
static pid_t daemon_pid;
static unsigned generation;           /* bumps with every new configuration */
static pid_t at_exit_pid;
static void kill_at_exit(void) { if (daemon_pid) kill(daemon_pid, SIGTERM); }

static void cfg_path(char *out, size_t n)
{
    const char *h = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/QRX OS One/net.cfg", h ? h : ".");
}
static int mode_of(const char *text)
{
    const char *p = text;
    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "mode", 4)) {
            const char *v = strchr(p, '=');
            if (v) {
                v++;
                while (*v == ' ') v++;
                if (!strncmp(v, "tor", 3)) return 1;
                if (!strncmp(v, "wireguard", 9)) return 2;
                if (!strncmp(v, "offline", 7)) return 3;      /* no network at all */
                return 0;
            }
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}
static void netd_path(char *out, size_t n)
{
    const char *e = getenv("QRX_NETD");
    if (e) { snprintf(out, n, "%s", e); return; }
    char exe[PATH_MAX] = "";
#ifdef __APPLE__
    uint32_t sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) != 0) exe[0] = 0;
#endif
    char *slash = strrchr(exe, '/');
    if (slash) { slash[1] = 0; snprintf(out, n, "%sqrxnetd", exe); }
    else snprintf(out, n, "qrxnetd");
}

struct watch { pid_t pid; int out; unsigned gen; };
static void start_daemon(void);
/* Reads qrxnetd's stdout: READY opens the network, the end of it closes it again */
static void *watch_daemon(void *arg)
{
    struct watch w = *(struct watch *)arg;
    free(arg);
    char buf[256];
    size_t have = 0;
    for (;;) {
        ssize_t n = read(w.out, buf + have, sizeof buf - 1 - have);
        if (n <= 0) break;
        have += (size_t)n;
        buf[have] = 0;
        if (strstr(buf, "READY")) {
            pthread_mutex_lock(&lock);
            if (w.gen == generation) { chmod(sock_path, 0600); ready = 1; }
            pthread_mutex_unlock(&lock);
            fprintf(stderr, "[NET] %s ready\n", mode == 1 ? "Tor" : "WireGuard");
        }
        if (have > 200) have = 0;
    }
    close(w.out);
    int st;
    waitpid(w.pid, &st, 0);
    pthread_mutex_lock(&lock);
    int again = w.gen == generation && (mode == 1 || mode == 2);
    if (w.gen == generation) { ready = 0; daemon_pid = 0; }
    pthread_mutex_unlock(&lock);
    if (again) {
        fprintf(stderr, "[NET] qrxnetd ended; the guest has no network until it is back\n");
        sleep(3);
        pthread_mutex_lock(&lock);
        if (w.gen == generation && (mode == 1 || mode == 2) && !daemon_pid) start_daemon();
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}
/* with the lock held */
static void start_daemon(void)
{
    char cfg[PATH_MAX], netd[PATH_MAX];
    cfg_path(cfg, sizeof cfg);
    netd_path(netd, sizeof netd);
    snprintf(sock_path, sizeof sock_path, "/tmp/qrxnetd-%d-%d.sock", (int)getuid(), (int)getpid());
    int pipefd[2];
    if (pipe(pipefd)) return;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    char *argv[] = { netd, cfg, sock_path, NULL };
    pid_t pid;
    int e = posix_spawn(&pid, netd, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (e) { close(pipefd[0]); fprintf(stderr, "[NET] cannot start %s: the guest has no network\n", netd); return; }
    daemon_pid = pid;
    if (!at_exit_pid) { at_exit_pid = pid; atexit(kill_at_exit); }   /* qrxnetd ends with the engine */
    struct watch *w = malloc(sizeof *w);
    w->pid = pid; w->out = pipefd[0]; w->gen = generation;
    pthread_t t;
    pthread_create(&t, NULL, watch_daemon, w);
    pthread_detach(t);
}
static void stop_daemon(void)
{
    if (daemon_pid) kill(daemon_pid, SIGTERM);
    daemon_pid = 0;
    ready = 0;
}
static void load(void)
{
    char path[PATH_MAX], text[4096];
    if (mode >= 0) return;
    mode = 0;
    cfg_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    mode = mode_of(text);
    if (mode == 1 || mode == 2) start_daemon();
}

int qrx_net_mode(void)
{
    pthread_mutex_lock(&lock);
    load();
    int m = mode;
    pthread_mutex_unlock(&lock);
    return m;
}
int qrx_net_ready(void) { return qrx_net_mode() == 0 || ready; }

int qrx_net_configure(const char *text, size_t len)
{
    char path[PATH_MAX], dir[PATH_MAX], line[512];
    static const char *allowed[] = { "mode", "private_key", "address", "dns", "peer_public_key",
                                     "preshared_key", "endpoint", "keepalive", NULL };
    cfg_path(path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0;
    mkdir(dir, 0700);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    /* Only the known keys from the guest; Tor's directory is the engine's choice, not the guest's */
    for (size_t i = 0; i < len;) {
        size_t j = i;
        while (j < len && text[j] != '\n') j++;
        size_t n = j - i < sizeof line - 1 ? j - i : sizeof line - 1;
        memcpy(line, text + i, n);
        line[n] = 0;
        i = j + 1;
        const char *k = line;
        while (*k == ' ' || *k == '\t') k++;
        for (int a = 0; allowed[a]; a++) {
            size_t al = strlen(allowed[a]);
            if (!strncmp(k, allowed[a], al) && (k[al] == ' ' || k[al] == '=' || k[al] == '\t')) {
                if (!strchr(line, '\r')) fprintf(f, "%s\n", line);
                break;
            }
        }
    }
    fprintf(f, "state_dir = %s/tor\n", dir);
    fclose(f);
    chmod(path, 0600);                /* holds the WireGuard key */
    char copy[4096];
    if (len >= sizeof copy) len = sizeof copy - 1;
    memcpy(copy, text, len);
    copy[len] = 0;
    for (char *c = copy; *c; c++) if (*c == '\r') *c = ' ';
    pthread_mutex_lock(&lock);
    generation++;
    stop_daemon();
    mode = mode_of(copy);
    if (mode == 1 || mode == 2) start_daemon();
    pthread_mutex_unlock(&lock);
    return 0;
}

static int unix_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un su;
    memset(&su, 0, sizeof su);
    su.sun_family = AF_UNIX;
    snprintf(su.sun_path, sizeof su.sun_path, "%s", sock_path);
    if (connect(fd, (struct sockaddr *)&su, sizeof su) < 0) { close(fd); return -1; }
    return fd;
}
static size_t socks_request(uint8_t *q, uint8_t cmd, uint32_t ip, const char *name, uint16_t port)
{
    size_t n = 0;
    q[n++] = 5; q[n++] = 1; q[n++] = 0;          /* greeting: no authentication */
    q[n++] = 5; q[n++] = cmd; q[n++] = 0;
    if (name) {
        size_t l = strlen(name);
        if (l > 255) l = 255;
        q[n++] = 3; q[n++] = (uint8_t)l;
        memcpy(q + n, name, l); n += l;
    } else {
        q[n++] = 1;
        q[n++] = (uint8_t)(ip >> 24); q[n++] = (uint8_t)(ip >> 16); q[n++] = (uint8_t)(ip >> 8); q[n++] = (uint8_t)ip;
    }
    q[n++] = (uint8_t)(port >> 8); q[n++] = (uint8_t)port;
    return n;
}

int qrx_net_socks_connect(uint32_t ip, uint16_t port)
{
    if (!qrx_net_ready()) { errno = ENETUNREACH; return -1; }
    int fd = unix_connect();
    if (fd < 0) { errno = ENETUNREACH; return -1; }
    uint8_t q[300];
    size_t n = socks_request(q, 1, ip, NULL, port);
    if (write(fd, q, n) != (ssize_t)n) { close(fd); errno = ENETUNREACH; return -1; }
    return fd;
}

int qrx_net_socks_reply(const uint8_t *r, size_t have)
{
    if (have < QRX_SOCKS_REPLY) return 0;
    if (r[0] != 5 || r[1] != 0 || r[2] != 5) return -ECONNREFUSED;
    switch (r[3]) {
    case 0: return 1;
    case 3: return -ENETUNREACH;
    case 4: return -EHOSTUNREACH;
    case 6: return -ETIMEDOUT;
    default: return -ECONNREFUSED;
    }
}

int qrx_net_resolve(const char *name, uint32_t *ip)
{
    if (!qrx_net_ready()) return -1;
    int fd = unix_connect();
    if (fd < 0) return -1;
    uint8_t q[300], r[QRX_SOCKS_REPLY];
    size_t n = socks_request(q, 0xF0, 0, name, 0), have = 0;
    if (write(fd, q, n) != (ssize_t)n) { close(fd); return -1; }
    while (have < sizeof r) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 20000) <= 0) break;
        ssize_t k = read(fd, r + have, sizeof r - have);
        if (k <= 0) break;
        have += (size_t)k;
    }
    close(fd);
    if (qrx_net_socks_reply(r, have) != 1) return -1;
    *ip = (uint32_t)r[6] << 24 | (uint32_t)r[7] << 16 | (uint32_t)r[8] << 8 | r[9];
    return 0;
}
