/* qrx_ec64_run: the 64-bit QRX OS One (AROS on the 68100EC64) in ARMithlon.
 *
 *   qrx_ec64_run [options] core.elf [kickstart module...]
 *     --cores N        cores, 1 to 8 (default 2)
 *     --ram MB         guest RAM (default 512)
 *     --disk IMAGE     a disk image for qrxdisk.device (an FFS image from mkffsimg.py)
 *     --disk-write     write the disk back to the file at the end
 *     --cmdline TEXT   the kernel command line (default "econsole")
 *     --window         a Mac window: the console, and the board's framebuffer once it is on
 *     --stdin          console input from stdin (without --window)
 *     --input TEXT     console input to start with, \n for a new line
 *     --seconds S      stop after S seconds (default: run until the system ends or the window closes)
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
#ifdef __APPLE__
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
static const char *capture_path;

static void capture(const qec64_board *b)
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

static double wall(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static void console_out(void *ctx, unsigned ch)
{
    (void)ctx;
#ifdef __APPLE__
    if (use_window) { char c = (char)ch; qec64_window_console(&c, 1); return; }
#endif
    fputc((int)ch, stdout);
    if (ch == '\n') fflush(stdout);
}

#ifdef __APPLE__
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
    char cmdline[400] = "econsole", *input = NULL;
    const char *wav_path = NULL, *views[4];
    int n_views = 0;
    int disk_write = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--cores") && i + 1 < argc) cores = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--ram") && i + 1 < argc) ram_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--disk") && i + 1 < argc) disk_path = argv[++i];
        else if (!strcmp(a, "--disk-write")) disk_write = 1;
        else if (!strcmp(a, "--cmdline") && i + 1 < argc) snprintf(cmdline, sizeof cmdline, "%s", argv[++i]);
        else if (!strcmp(a, "--window")) use_window = 1;
        else if (!strcmp(a, "--stdin")) use_stdin = 1;
        else if (!strcmp(a, "--input") && i + 1 < argc) input = unescape(argv[++i]);
        else if (!strcmp(a, "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(a, "--map") && i + 1 < argc) map_path = argv[++i];
        else if (!strcmp(a, "--stats")) show_stats = 1;
        else if (!strcmp(a, "--capture") && i + 1 < argc) capture_path = argv[++i];
        else if (!strcmp(a, "--audio-wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(a, "--host-view") && i + 1 < argc && n_views < 4) views[n_views++] = argv[++i];
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s\n", a); return 2; }
        else if (!kernel) kernel = a;
        else if (nmodules < MAX_MODULES) modules[nmodules++] = a;
    }
    if (!kernel || cores < 1 || cores > QEC64_MAX_CORES || ram_mb < 64) {
        fprintf(stderr, "usage: %s [--cores N] [--ram MB] [--disk IMAGE [--disk-write]] [--cmdline TEXT] [--window | --stdin]\n"
                        "       [--input TEXT] [--seconds S] [--map FILE] [--stats] core.elf [module...]\n", argv[0]);
        return 2;
    }
#ifndef __APPLE__
    if (use_window) { fprintf(stderr, "--window needs macOS\n"); return 2; }
#endif
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
        pthread_t reader;
        if (use_stdin && !use_window && boot == 0) pthread_create(&reader, NULL, stdin_reader, m);

        struct run_args r = { m, L.pc, L.sp, seconds, 0, QEC64_RUNNING, 0 };
        double t0 = wall();
#ifdef __APPLE__
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

        qec64_cpu *ender = qec64_machine_core(m, r.who);
        code = r.st == QEC64_HALTED ? (long long)ender->exit_code : -1;
        fflush(stdout);
        fprintf(stderr, "\n[EC64] end: status %d on core %u, value %#llx, %.2f s\n", r.st, r.who, code, t1 - t0);
        if (show_stats || (r.st != QEC64_HALTED && r.st != QEC64_RUNNING)) {
            uint64_t total = 0;
            for (unsigned k = 0; k < cores; k++) {
                qec64_cpu *c = qec64_machine_core(m, k);
                total += c->instructions;
                fprintf(stderr, "[EC64] core %u: %" PRIu64 " instructions (%.1f million a second), pc %016" PRIx64 " %s\n", k,
                        c->instructions, c->instructions / (t1 - t0) / 1e6, c->pc, c->supervisor ? "supervisor" : "user");
            }
            fprintf(stderr, "[EC64] all cores: %.1f million instructions a second\n", total / (t1 - t0) / 1e6);
        }
        capture(board);
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
#ifdef __APPLE__
    if (use_window) qec64_window_close();
#endif
    if (disk && disk_write) {
        FILE *f = fopen(disk_path, "r+b");
        if (f) { fwrite(disk, 1, disk_size, f); fclose(f); }
    }
    return code == 0 || code == QEC64_END_POWEROFF ? 0 : 1;
}
