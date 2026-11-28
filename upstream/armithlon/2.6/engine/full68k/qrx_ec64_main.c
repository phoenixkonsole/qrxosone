/* qrx_ec64_run: the 64-bit QRX OS One (AROS on the 68100EC64) in ARMithlon.
 *
 *   qrx_ec64_run [options] core.elf [kickstart module...]
 *     --cores N        cores, 1 to 8 (default 2)
 *     --ram MB         guest RAM (default 512)
 *     --disk IMAGE     a disk image for qrxdisk.device (an FFS image from mkffsimg.py)
 *     --disk-write     write the disk back to the file at the end
 *     --hostfs DIR     share the host directory DIR (host service 12, qrx_ec64_hostfs.h): with the
 *                      qrxsys kickstart module the system boots from it as QRXSYS: (no --disk needed);
 *                      L:qrxhostfs-handler mounts it as HOST: otherwise
 *     --hostfs-ro      the shared directory read-only
 *     --cmdline TEXT   the kernel command line (default "econsole")
 *     --window         a window (Cocoa on the Mac, SDL2 on Linux): the console, and the board's
 *                      framebuffer once it is on
 *     --stdin          console input from stdin (without --window)
 *     --input TEXT     console input to start with, \n for a new line
 *     --seconds S      stop after S seconds (default: run until the system ends or the window closes)
 *     --capture-at T:FILE[,T:FILE...]  the framebuffer as PPM after T seconds too (several in one run)
 *     --events FILE    keyboard and mouse as from the window, at set times (tests without a window):
 *                      one per line, "SECONDS move X Y | down B | up B | click B | wheel N |
 *                      key MACKEY | keydown MACKEY | keyup MACKEY | text WORDS"; B 0 left, 1 right,
 *                      2 middle; X, Y framebuffer pixels; text types US layout characters
 *     --map FILE       "address name" of every function of the kickstart
 *     --stats          instructions and instructions per second per core at the end
 *     --capture FILE   the board's framebuffer at the end as a PPM, if it is on
 *     --audio-wav FILE without a window: play the board's sound into a WAV file in real time
 *     --host-view [T:]WxH  without a window: the host window's size the guest sees (after T seconds),
 *                      as when the window is resized or goes full screen; also sets the host screen
 *
 * The system ends the run with a value through the test console's end register; 0x101 and 0x102
 * (a reboot) load and start it again with " rebooted" added to the command line, as a reset would.
 * The 32-bit QRX OS One is qrx68100_full_run; this program does not change it. */
#include "qrx68100ec64.h"
#include "qrx_ec64_boot.h"
#include "qrx_ec64_board.h"
#include "qrx_ec64_hostfs.h"
#include "qrx_gl3.h"
#if defined(__APPLE__) || defined(QRX_SDL_FRONTEND)
#define QEC64_WINDOW 1
#include "../macos/qrx_ec64_window.h"
#endif
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_MODULES 64
#define MAX_BOOTS 8

static int use_window, use_stdin, show_stats;
static const char *profile_path;
static const char *capture_path;

static void capture_to(const qec64_board *b, const char *capture_path)
{
    const uint8_t *px; uint32_t w, h, stride, fmt;
    if (!capture_path) return;
    if (!qec64_board_framebuffer(b, &px, &w, &h, &stride, &fmt)) { fprintf(stderr, "[EC64] no framebuffer to capture\n"); return; }
    FILE *f = fopen(capture_path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = px + (uint64_t)y * stride;
            uint8_t rgb[3];
            if (fmt == QEC64_FB_FMT_XRGB32) { rgb[0] = p[x * 4 + 1]; rgb[1] = p[x * 4 + 2]; rgb[2] = p[x * 4 + 3]; }
            else { uint32_t v = (uint32_t)p[x * 2] << 8 | p[x * 2 + 1];
                   rgb[0] = (uint8_t)((v >> 11 & 31) * 255 / 31); rgb[1] = (uint8_t)((v >> 5 & 63) * 255 / 63); rgb[2] = (uint8_t)((v & 31) * 255 / 31); }
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    fprintf(stderr, "[EC64] framebuffer %u x %u written to %s\n", w, h, capture_path);
}
static void capture(const qec64_board *b) { capture_to(b, capture_path); }

/* --capture-at T:FILE[,T:FILE...]: the framebuffer as PPM also after T seconds (several pictures in one run) */
typedef struct { const qec64_board *board; char *spec; } capture_run;
static void *capture_later(void *arg)
{
    capture_run *c = arg;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (char *item = strtok(c->spec, ","); item; item = strtok(NULL, ",")) {
        char *colon = strchr(item, ':');
        if (!colon) continue;
        *colon = 0;
        double at = atof(item);
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        double left = at - ((now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9);
        if (left > 0) { struct timespec ts = { (time_t)left, (long)((left - (time_t)left) * 1e9) }; nanosleep(&ts, NULL); }
        capture_to(c->board, colon + 1);
    }
    return NULL;
}

/* QRX_EC64_DUMP=<hex address>,<hex length>,<file>: that part of guest RAM at the end, e.g. the code
 * around a fault's pc to find the library it belongs to (aros-qrx/qrxbt.py, a fuzzy match) */
static void dump_ram(const qec64_board *b)
{
    const char *e = getenv("QRX_EC64_DUMP");
    if (!e) return;
    char *p;
    unsigned long long a = strtoull(e, &p, 16), n = *p == ',' ? strtoull(p + 1, &p, 16) : 0;
    if (*p != ',' || !n || a >= b->ram_size || n > b->ram_size - a) { fprintf(stderr, "[EC64] QRX_EC64_DUMP: bad range\n"); return; }
    FILE *f = fopen(p + 1, "wb");
    if (!f) return;
    fwrite(b->ram + a, 1, (size_t)n, f);
    fclose(f);
    fprintf(stderr, "[EC64] RAM %llx+%llx written to %s\n", a, n, p + 1);
}

static double wall(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static void console_out(void *ctx, unsigned ch)
{
    (void)ctx;
#ifdef QEC64_WINDOW
    if (use_window) { char c = (char)ch; qec64_window_console(&c, 1); return; }
#endif
    fputc((int)ch, stdout);
    if (ch == '\n') fflush(stdout);
}

#ifdef QEC64_WINDOW
static qec64_machine *volatile current;     /* the machine of the boot that runs now */
static void typed(void *ctx, const char *text, size_t len)
{
    (void)ctx;
    qec64_machine *m = current;
    if (m) qec64_machine_console_push(m, text, len);
}
#endif

static void *stdin_reader(void *arg)
{
    qec64_machine *m = arg;
    char buf[256];
    ssize_t n;
    while ((n = read(0, buf, sizeof buf)) > 0) qec64_machine_console_push(m, buf, (size_t)n);
    return NULL;
}

/* --profile FILE: every 2 ms of wall time each core's pc goes to FILE as a record of
 * 16 bytes (little endian): u32 milliseconds since the start, u8 core, u8 flags (1 supervisor,
 * 2 waiting after STOP, 4 owns the global Forbid, 8 stands still for it), u16 0, u64 pc. tools/armithlon/ec64prof.py sums them up by symbol. */
struct prof_args { qec64_machine *m; unsigned cores; FILE *f; int stop; const char *tasks_path; };
/* The task a core runs: ThisTask of its per-core data (A8, AROS's tls.h: offset 16), and the task's
 * name (its Node's ln_Name at offset 24). Profile records carry an index into the tasks seen so far;
 * PROFILE.tasks names them ("index address name"), written when a task is first seen. */
#define PROF_TASKS 4096
static uint64_t prof_task_ptr[PROF_TASKS];
static unsigned prof_ntasks;
static uint64_t be64(const uint8_t *b) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | b[i]; return v; }
static unsigned prof_task(qec64_cpu *c, FILE *names)
{
    uint64_t tls = c->a[8], task, name;
    if (tls + 24 > c->ram_size) return 0xFFFF;
    task = be64(c->ram + tls + 16);
    for (unsigned i = 0; i < prof_ntasks; i++)
        if (prof_task_ptr[i] == task) return i;
    if (prof_ntasks >= PROF_TASKS || !names) return 0xFFFF;
    char buf[64] = "?";
    if (task && task + 32 <= c->ram_size && (name = be64(c->ram + task + 24)) && name < c->ram_size) {
        unsigned n = 0;
        while (n < sizeof buf - 1 && name + n < c->ram_size && c->ram[name + n] >= 32 && c->ram[name + n] < 127) { buf[n] = (char)c->ram[name + n]; n++; }
        buf[n] = 0;
    }
    prof_task_ptr[prof_ntasks] = task;
    fprintf(names, "%u %016llx %s\n", prof_ntasks, (unsigned long long)task, buf);
    fflush(names);
    return prof_ntasks++;
}

static void *profile_thread(void *arg)
{
    struct prof_args *p = arg;
    double t0 = wall();
    FILE *names = p->tasks_path ? fopen(p->tasks_path, "w") : NULL;
    while (!__atomic_load_n(&p->stop, __ATOMIC_ACQUIRE)) {
        struct timespec ts = { 0, 2000000 };
        nanosleep(&ts, NULL);
        uint32_t ms = (uint32_t)((wall() - t0) * 1000.0);
        int gowner; uint32_t gstopped;
        qec64_machine_gforbid_state(p->m, &gowner, &gstopped);
        for (unsigned k = 0; k < p->cores; k++) {
            qec64_cpu *c = qec64_machine_core(p->m, k);
            uint8_t rec[16] = { 0 };
            uint64_t pc = __atomic_load_n(&c->pc, __ATOMIC_RELAXED);
            for (int i = 0; i < 4; i++) rec[i] = (uint8_t)(ms >> (8 * i));
            rec[4] = (uint8_t)k;
            rec[5] = (uint8_t)((c->supervisor ? 1 : 0) | (c->waiting ? 2 : 0) | (gowner == (int)k ? 4 : 0) | (gstopped >> k & 1 ? 8 : 0));
            unsigned ti = prof_task(c, names);
            rec[6] = (uint8_t)ti; rec[7] = (uint8_t)(ti >> 8);
            for (int i = 0; i < 8; i++) rec[8 + i] = (uint8_t)(pc >> (8 * i));
            fwrite(rec, 1, sizeof rec, p->f);
        }
    }
    if (names) fclose(names);
    return NULL;
}

struct run_args { qec64_machine *m; uint64_t pc, sp; double seconds; unsigned who; enum qec64_status st; int done; };
static void *machine_thread(void *arg)
{
    struct run_args *r = arg;
    r->st = qec64_machine_run(r->m, r->pc, r->sp, r->seconds, &r->who);
    __atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static uint8_t *read_file(const char *path, uint64_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *size = (uint64_t)n;
    return b;
}

static char *unescape(const char *e)
{
    char *o, *s = o = malloc(strlen(e) + 1);
    for (; *e; e++) {
        if (e[0] == '\\' && e[1] == 'n') { *o++ = '\n'; e++; }
        else *o++ = *e;
    }
    *o = 0;
    return s;
}

/* --host-view T:WxH: the window changes its size after T seconds */
typedef struct { qec64_board *board; double t; uint32_t size; } view_at;
static void *view_later(void *arg)
{
    view_at *v = arg;
    struct timespec ts = { (time_t)v->t, (long)((v->t - (time_t)v->t) * 1e9) };
    nanosleep(&ts, NULL);
    v->board->host_view = v->size;
    if ((v->size >> 16) > (v->board->host_screen >> 16)) v->board->host_screen = v->size;
    return NULL;
}

/* --events FILE: input as the host window would send it, at set times (see the top) */
typedef struct { qec64_board *board; char *path; } events_run;
static int mac_of(char c, int *shift)
{
    static const char lower[] = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.\0 `";
    static const char upper[] = "ASDFHGZXCV\0BQWERYT!@#$^%+(&_*)}OU{IP\0LJ\"K:|<?NM>\0 ~";
    *shift = 0;
    for (int i = 0; i < (int)sizeof lower - 1; i++) {
        if (lower[i] && lower[i] == c) return i;
        if (upper[i] && upper[i] == c) { *shift = 1; return i; }
    }
    return -1;
}
static void ev_key(qec64_board *b, int k, int down) { qec64_board_push_event(b, (down ? 1u : 2u) << 24 | (uint32_t)(k & 0xff) << 16); }
static void ev_pause(double s) { struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) }; nanosleep(&ts, NULL); }
static void *events_thread(void *arg)
{
    events_run *e = arg;
    FILE *f = fopen(e->path, "r");
    if (!f) { fprintf(stderr, "--events: cannot read %s\n", e->path); return NULL; }
    char line[512];
    double t0 = 0, now = 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); t0 = ts.tv_sec + ts.tv_nsec / 1e9;
    while (fgets(line, sizeof line, f)) {
        double at; char cmd[16]; int n = 0;
        if (line[0] == '#' || sscanf(line, "%lf %15s %n", &at, cmd, &n) < 2) continue;
        char *rest = line + n;
        rest[strcspn(rest, "\n")] = 0;
        clock_gettime(CLOCK_MONOTONIC, &ts); now = ts.tv_sec + ts.tv_nsec / 1e9 - t0;
        if (at > now) ev_pause(at - now);
        qec64_board *b = e->board;
        int x = 0, y = 0;
        if (!strcmp(cmd, "move") && sscanf(rest, "%d %d", &x, &y) == 2)
            qec64_board_push_event(b, 3u << 24 | (uint32_t)(x & 0xfff) << 12 | (uint32_t)(y & 0xfff));
        else if (!strcmp(cmd, "down") || !strcmp(cmd, "up") || !strcmp(cmd, "click")) {
            uint32_t btn = (uint32_t)atoi(rest) & 0xff;
            if (strcmp(cmd, "up")) qec64_board_push_event(b, 4u << 24 | btn << 16 | 1u);
            if (!strcmp(cmd, "click")) ev_pause(0.08);
            if (strcmp(cmd, "down")) qec64_board_push_event(b, 4u << 24 | btn << 16);
        } else if (!strcmp(cmd, "wheel"))
            qec64_board_push_event(b, 5u << 24 | ((uint32_t)atoi(rest) & 0xffff));
        else if (!strcmp(cmd, "key")) { int k = (int)strtol(rest, NULL, 0); ev_key(b, k, 1); ev_pause(0.05); ev_key(b, k, 0); }
        else if (!strcmp(cmd, "keydown")) ev_key(b, (int)strtol(rest, NULL, 0), 1);
        else if (!strcmp(cmd, "keyup")) ev_key(b, (int)strtol(rest, NULL, 0), 0);
        else if (!strcmp(cmd, "text"))
            for (char *c = rest; *c; c++) {
                int shift, k = mac_of(*c, &shift);
                if (k < 0) continue;
                if (shift) ev_key(b, 0x38, 1);
                ev_key(b, k, 1); ev_pause(0.03); ev_key(b, k, 0);
                if (shift) ev_key(b, 0x38, 0);
                ev_pause(0.05);
            }
        else fprintf(stderr, "--events: unknown: %s", line);
    }
    fclose(f);
    return NULL;
}

/* --audio-wav: drains the board's sound ring at 48 kHz, as the Mac's audio output would, into a file */
typedef struct { qec64_board *board; FILE *f; } wav_out;
static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t bytes = frames * 4u;
    uint8_t h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x80\xbb\0\0\0\xee\x02\0\x04\0\x10\0data";
    uint32_t riff = 36 + bytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}
static void *wav_writer(void *arg)
{
    wav_out *w = arg;
    int16_t pcm[480 * 2];
    uint32_t frames = 0;
    wav_header(w->f, 0);
    for (;;) {
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        size_t n = qec64_board_pull_audio(w->board, pcm, 480);
        if (n < 480) memset(pcm + n * 2, 0, (480 - n) * 4);   /* an underrun plays as silence */
        fwrite(pcm, 4, 480, w->f);
        frames += 480;
        if (frames % 48000 == 0) { wav_header(w->f, frames); fflush(w->f); }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    unsigned cores = 2;
    uint64_t ram_mb = 512, disk_size = 0;
    double seconds = 1e9;
    const char *disk_path = NULL, *map_path = NULL, *kernel = NULL;
    const char *modules[MAX_MODULES];
    unsigned nmodules = 0;
    char cmdline[400] = "econsole", *input = NULL, *events_path = NULL, *capture_at = NULL;
    const char *wav_path = NULL, *views[4];
    int n_views = 0;
    int disk_write = 0, hostfs_ro = 0;
    const char *hostfs_dir = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--cores") && i + 1 < argc) cores = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--ram") && i + 1 < argc) ram_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--disk") && i + 1 < argc) disk_path = argv[++i];
        else if (!strcmp(a, "--disk-write")) disk_write = 1;
        else if (!strcmp(a, "--hostfs") && i + 1 < argc) hostfs_dir = argv[++i];
        else if (!strcmp(a, "--hostfs-ro")) hostfs_ro = 1;
        else if (!strcmp(a, "--cmdline") && i + 1 < argc) snprintf(cmdline, sizeof cmdline, "%s", argv[++i]);
        else if (!strcmp(a, "--window")) use_window = 1;
        else if (!strcmp(a, "--stdin")) use_stdin = 1;
        else if (!strcmp(a, "--input") && i + 1 < argc) input = unescape(argv[++i]);
        else if (!strcmp(a, "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(a, "--events") && i + 1 < argc) events_path = argv[++i];
        else if (!strcmp(a, "--capture-at") && i + 1 < argc) capture_at = argv[++i];
        else if (!strcmp(a, "--map") && i + 1 < argc) map_path = argv[++i];
        else if (!strcmp(a, "--stats")) show_stats = 1;
        else if (!strcmp(a, "--profile") && i + 1 < argc) profile_path = argv[++i];
        else if (!strcmp(a, "--capture") && i + 1 < argc) capture_path = argv[++i];
        else if (!strcmp(a, "--audio-wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(a, "--host-view") && i + 1 < argc && n_views < 4) views[n_views++] = argv[++i];
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s\n", a); return 2; }
        else if (!kernel) kernel = a;
        else if (nmodules < MAX_MODULES) modules[nmodules++] = a;
    }
    if (!kernel || cores < 1 || cores > QEC64_MAX_CORES || ram_mb < 64) {
        fprintf(stderr, "usage: %s [--cores N] [--ram MB] [--disk IMAGE [--disk-write]] [--hostfs DIR [--hostfs-ro]] [--cmdline TEXT] [--window | --stdin]\n"
                        "       [--input TEXT] [--seconds S] [--map FILE] [--stats] [--profile FILE] core.elf [module...]\n", argv[0]);
        return 2;
    }
#ifndef QEC64_WINDOW
    if (use_window) { fprintf(stderr, "--window needs macOS or a build with SDL2\n"); return 2; }
#endif
    if (hostfs_dir && !qrx_ec64_hostfs_mount(hostfs_dir, hostfs_ro)) {
        fprintf(stderr, "%s: not a directory\n", hostfs_dir);
        return 2;
    }
    uint8_t *disk = NULL;
    if (disk_path) {
        disk = read_file(disk_path, &disk_size);
        if (!disk) { fprintf(stderr, "%s: cannot read\n", disk_path); return 2; }
        disk_size &= ~UINT64_C(511);
    }
    uint64_t ram_size = ram_mb << 20;
    long long code = -1;
    int window_closed = 0;
    static qec64_board board_store;              /* one board over all boots: the window keeps a pointer to it */
    qec64_board *board = &board_store;

    for (int boot = 0; boot < MAX_BOOTS && !window_closed; boot++) {
        uint8_t *ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (ram == MAP_FAILED) { fprintf(stderr, "no memory for %" PRIu64 " MB of guest RAM\n", ram_mb); return 2; }
        FILE *map = map_path && boot == 0 ? fopen(map_path, "w") : NULL;
        qec64_aros_boot b = { .kernel = kernel, .modules = modules, .nmodules = nmodules, .cmdline = cmdline,
                              .disk = disk, .disk_size = disk_size, .map = map, .log = boot == 0 ? stderr : NULL };
        qec64_aros_loaded L;
        char err[256];
        if (qec64_aros_load(ram, ram_size, &b, &L, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 2; }
        if (map) fclose(map);
        fprintf(stderr, "[EC64] %u cores, %" PRIu64 " MB, kickstart up to %016" PRIx64 "%s\n", cores, ram_mb, L.kernel_end,
                disk ? ", disk in RAM" : "");

        qec64_machine *m = qec64_machine_new(ram, ram_size, cores);
        if (!m) return 2;
        qec64_board_init(board, ram, ram_size);
        board->cores = cores;
        qec64_machine_set_mmio(m, qec64_board_mmio, board);
        qec64_machine_console_sink(m, console_out, NULL);
        if (input) qec64_machine_console_input(m, input, strlen(input));
        if (!use_window && boot == 0) for (int v = 0; v < n_views; v++) {
            static view_at va[4];
            unsigned w = 0, h = 0;
            double t = 0;
            const char *spec = views[v], *colon = strchr(spec, ':');
            if (colon) { t = atof(spec); spec = colon + 1; }
            if (sscanf(spec, "%ux%u", &w, &h) != 2) continue;
            va[v] = (view_at){ board, t, (uint32_t)w << 16 | h };
            if (t <= 0) { board->host_view = board->host_screen = va[v].size; continue; }
            if (!board->host_screen) board->host_screen = va[v].size;
            pthread_t th;
            pthread_create(&th, NULL, view_later, &va[v]);
        }
        static wav_out wav;
        if (wav_path && !use_window && boot == 0) {
            wav.board = board;
            wav.f = fopen(wav_path, "wb");
            if (wav.f) { pthread_t w; pthread_create(&w, NULL, wav_writer, &wav); }
        }
        static capture_run car;
        if (capture_at && !use_window && boot == 0) {
            car = (capture_run){ board, strdup(capture_at) };
            pthread_t ct; pthread_create(&ct, NULL, capture_later, &car);
        }
        static events_run evr;
        if (events_path && !use_window && boot == 0) {
            evr = (events_run){ board, events_path };
            pthread_t et; pthread_create(&et, NULL, events_thread, &evr);
        }
        pthread_t reader;
        if (use_stdin && !use_window && boot == 0) pthread_create(&reader, NULL, stdin_reader, m);

        struct run_args r = { m, L.pc, L.sp, seconds, 0, QEC64_RUNNING, 0 };
        static char tasks_path[1100];
        if (profile_path) snprintf(tasks_path, sizeof tasks_path, "%s.tasks", profile_path);
        struct prof_args pa = { m, cores, NULL, 0, profile_path && boot == 0 ? tasks_path : NULL };
        pthread_t pt;
        if (profile_path && (pa.f = fopen(profile_path, boot ? "ab" : "wb")) != NULL) pthread_create(&pt, NULL, profile_thread, &pa);
        else if (profile_path) fprintf(stderr, "[EC64] cannot write %s\n", profile_path);
        double t0 = wall();
#ifdef QEC64_WINDOW
        if (use_window) {
            qec64_window_input in = { typed, NULL };
            current = m;
            if (boot == 0 && !qec64_window_open("QRX OS One 64 (68100EC64)", board, &in)) { fprintf(stderr, "no window\n"); return 2; }
            pthread_t t;
            pthread_create(&t, NULL, machine_thread, &r);
            while (!__atomic_load_n(&r.done, __ATOMIC_ACQUIRE)) {
                if (!qec64_window_pump()) { window_closed = 1; qec64_machine_stop(m); }
            }
            pthread_join(t, NULL);
            current = NULL;
            qec64_window_pump();
        } else
#endif
            machine_thread(&r);
        double t1 = wall();
        if (pa.f) { __atomic_store_n(&pa.stop, 1, __ATOMIC_RELEASE); pthread_join(pt, NULL); fclose(pa.f); }

        qec64_cpu *ender = qec64_machine_core(m, r.who);
        code = r.st == QEC64_HALTED ? (long long)ender->exit_code : -1;
        fflush(stdout);
        fprintf(stderr, "\n[EC64] end: status %d on core %u, value %#llx, %.2f s\n", r.st, r.who, code, t1 - t0);
        qrx_gl3_report();   /* [QRX-GL3] streams=... calls=...: what programs drew on the host's OpenGL 3.3 */
        if (show_stats || (r.st != QEC64_HALTED && r.st != QEC64_RUNNING)) {
            uint64_t total = 0;
            for (unsigned k = 0; k < cores; k++) {
                qec64_cpu *c = qec64_machine_core(m, k);
                total += c->instructions;
                fprintf(stderr, "[EC64] core %u: %" PRIu64 " instructions (%.1f million a second), pc %016" PRIx64 " %s\n", k,
                        c->instructions, c->instructions / (t1 - t0) / 1e6, c->pc, c->supervisor ? "supervisor" : "user");
                if (show_stats)
                    for (unsigned r = 0; r < 8; r++)
                        fprintf(stderr, "[EC64]   d%u %016" PRIx64 "  a%u %016" PRIx64 "  a%u %016" PRIx64 "\n", r, c->d[r], r, c->a[r], r + 8, c->a[r + 8]);
            }
            fprintf(stderr, "[EC64] all cores: %.1f million instructions a second\n", total / (t1 - t0) / 1e6);
        }
        capture(board);
        dump_ram(board);
        if (disk) memcpy(disk, ram + L.disk_at, disk_size);    /* the disk keeps what the system wrote */
        qec64_machine_free(m);
        munmap(ram, ram_size);
        if (code == QEC64_END_COLDBOOT || code == QEC64_END_WARMBOOT) {
            fprintf(stderr, "[EC64] reboot\n");
            if (!strstr(cmdline, "rebooted")) strncat(cmdline, " rebooted", sizeof cmdline - strlen(cmdline) - 1);
            continue;
        }
        break;
    }
#ifdef QEC64_WINDOW
    if (use_window) qec64_window_close();
#endif
    if (disk && disk_write) {
        FILE *f = fopen(disk_path, "r+b");
        if (f) { fwrite(disk, 1, disk_size, f); fclose(f); }
    }
    return code == 0 || code == QEC64_END_POWEROFF ? 0 : 1;
}
