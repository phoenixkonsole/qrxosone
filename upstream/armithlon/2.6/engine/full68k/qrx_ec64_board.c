/* The ARMithlon 68100EC64 board's devices, see qrx_ec64_board.h */
#include "qrx_ec64_board.h"
#include "qrx_hostnet.h"
#ifdef __APPLE__
#include "qrx_gl68k.h"
#include <pthread.h>
/* One GL stream at a time: the contexts belong to whichever core's thread calls. */
static pthread_mutex_t gl_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

static uint64_t clock_us(void);
void qec64_board_init(qec64_board *b, uint8_t *ram, uint64_t ram_size)
{
    memset(b, 0, sizeof *b);
    b->ram = ram;
    b->ram_size = ram_size;
    b->cores = 1;
    b->start_us = clock_us();
    snprintf(b->host_cpu, sizeof b->host_cpu, "unknown");
#ifdef __APPLE__
    size_t len = sizeof b->host_cpu - 1;
    if (sysctlbyname("machdep.cpu.brand_string", b->host_cpu, &len, NULL, 0)) snprintf(b->host_cpu, sizeof b->host_cpu, "Apple");
#endif
}

void qec64_board_push_event(qec64_board *b, uint32_t event)
{
    unsigned tail = __atomic_load_n(&b->ev_tail, __ATOMIC_ACQUIRE);
    unsigned next = (tail + 1) % 256u;
    if (next == __atomic_load_n(&b->ev_head, __ATOMIC_ACQUIRE)) return;    /* full: dropped */
    b->events[tail] = event;
    __atomic_store_n(&b->ev_tail, next, __ATOMIC_RELEASE);
}

static uint32_t take_event(qec64_board *b)
{
    unsigned head = __atomic_load_n(&b->ev_head, __ATOMIC_ACQUIRE);
    if (head == __atomic_load_n(&b->ev_tail, __ATOMIC_ACQUIRE)) return 0;
    uint32_t e = b->events[head];
    __atomic_store_n(&b->ev_head, (head + 1) % 256u, __ATOMIC_RELEASE);
    return e;
}

static uint64_t clock_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
/* The network service's buffers: the host would otherwise read and write outside guest RAM. */
static int net_args_ok(const qec64_board *b, uint32_t op, const uint32_t *a)
{
    uint64_t lim = b->ram_size < UINT64_C(0x100000000) ? b->ram_size : UINT64_C(0x100000000);
    switch (op) {
    case 6: case 7: case 8: case 9: case 18: case 19:   /* send, recv, sendto, recvfrom, hostname, random */
        return (uint64_t)a[1] + a[2] <= lim;
    case 17: {                                           /* resolve: a string */
        for (uint64_t p = a[1]; p < lim && p < (uint64_t)a[1] + 1024; p++) if (!b->ram[p]) return 1;
        return 0;
    }
    default: return 1;
    }
}
static void host_call(qec64_board *b, uint64_t addr)
{
    if (addr >= b->ram_size || b->ram_size - addr < 17 * 4 || (addr & 3)) return;
    uint8_t *q = b->ram + addr;
#ifdef __APPLE__
    if (be32(q) == QEC64_HOST_GL) {
        pthread_mutex_lock(&gl_lock);
        qrx_gl68k_service(b->ram, b->ram_size, q);
        pthread_mutex_unlock(&gl_lock);
        return;
    }
#endif
    uint32_t a[13];
    for (unsigned i = 0; i < 13; i++) a[i] = be32(q + 16 + i * 4);
    if (be32(q) != QEC64_HOST_NET) return;
    uint32_t op = be32(q + 12);
    if (!net_args_ok(b, op, a)) { a[8] = UINT32_MAX; a[9] = 10014; }
    else qrx_hostnet_call(op, a, b->ram);
    for (unsigned i = 8; i < 13; i++) put_be32(q + 16 + i * 4, a[i]);
}
/* The ring is single producer (the guest, under its own lock) and single consumer (the audio thread). */
static unsigned audio_queued(const qec64_board *b)
{
    return (__atomic_load_n(&b->audio_write, __ATOMIC_ACQUIRE) - __atomic_load_n(&b->audio_read, __ATOMIC_ACQUIRE)) % QEC64_AUDIO_RING;
}
static uint32_t audio_go(qec64_board *b)
{
    uint64_t n = b->audio_frames, addr = b->audio_addr;
    if (n > QEC64_AUDIO_RING || addr >= b->ram_size || n * 4 > b->ram_size - addr) return 0;
    unsigned room = QEC64_AUDIO_RING - 1 - audio_queued(b);
    if (n > room) n = room;
    unsigned w = __atomic_load_n(&b->audio_write, __ATOMIC_RELAXED);
    for (uint64_t i = 0; i < n; i++) b->audio_ring[(w + i) % QEC64_AUDIO_RING] = be32(b->ram + addr + i * 4);
    __atomic_store_n(&b->audio_write, (unsigned)((w + n) % QEC64_AUDIO_RING), __ATOMIC_RELEASE);
    return (uint32_t)n;
}
size_t qec64_board_pull_audio(qec64_board *b, int16_t *out, size_t n)
{
    unsigned r = __atomic_load_n(&b->audio_read, __ATOMIC_RELAXED);
    unsigned have = audio_queued(b);
    if (n > have) n = have;
    for (size_t i = 0; i < n; i++) {
        uint32_t f = b->audio_ring[(r + i) % QEC64_AUDIO_RING];
        out[i * 2] = (int16_t)(f >> 16);
        out[i * 2 + 1] = (int16_t)f;
    }
    __atomic_store_n(&b->audio_read, (unsigned)((r + n) % QEC64_AUDIO_RING), __ATOMIC_RELEASE);
    return n;
}
bool qec64_board_mmio(qec64_cpu *c, uint64_t addr, unsigned size, bool write, uint64_t *value)
{
    qec64_board *b = qec64_machine_user(c);
    uint64_t off = addr - QEC64_MMIO_BASE;
    if (!b || off >= 0x1000u) return false;
    switch ((unsigned)off) {
    case QEC64_BOARD_ID:
        if (write || size != 8) return false;
        *value = QEC64_BOARD_MAGIC; return true;
    case QEC64_BOARD_VERSION:
        if (write || size != 4) return false;
        *value = 1; return true;
    case QEC64_FB_ADDR:
        if (size != 8) return false;
        if (write) b->fb_addr = *value; else *value = b->fb_addr;
        return true;
    case QEC64_FB_WIDTH: case QEC64_FB_HEIGHT: case QEC64_FB_STRIDE: case QEC64_FB_FORMAT: case QEC64_FB_ENABLE: {
        volatile uint32_t *r = off == QEC64_FB_WIDTH ? &b->fb_width : off == QEC64_FB_HEIGHT ? &b->fb_height :
                               off == QEC64_FB_STRIDE ? &b->fb_stride : off == QEC64_FB_FORMAT ? &b->fb_format : &b->fb_enable;
        if (size != 4) return false;
        if (write) *r = (uint32_t)*value; else *value = *r;
        return true;
    }
    case QEC64_FB_FRAMES:
        if (size != 4) return false;
        if (write) __atomic_fetch_add(&b->fb_frames, 1u, __ATOMIC_RELEASE); else *value = b->fb_frames;
        return true;
    case QEC64_INPUT_EVENT:
        if (write || size != 4) return false;
        *value = take_event(b); return true;
    case QEC64_CLOCK_US:
        if (write || size != 8) return false;
        *value = clock_us(); return true;
    case QEC64_CPU_INSTRUCTIONS:
        if (write || size != 8) return false;
        *value = c->instructions; return true;
    case QEC64_HOST_CALL:
        if (!write || size != 8) return false;
        host_call(b, *value); return true;
    case QEC64_AUDIO_ADDR:
        if (size != 8) return false;
        if (write) b->audio_addr = *value; else *value = b->audio_addr;
        return true;
    case QEC64_AUDIO_FRAMES:
        if (size != 4) return false;
        if (write) b->audio_frames = (uint32_t)*value; else *value = b->audio_frames;
        return true;
    case QEC64_AUDIO_GO:
        if (size != 4) return false;
        if (write) b->audio_taken = audio_go(b); else *value = b->audio_taken;
        return true;
    case QEC64_AUDIO_QUEUED:
        if (write || size != 4) return false;
        *value = audio_queued(b); return true;
    case QEC64_AUDIO_RATE:
        if (write || size != 4) return false;
        *value = 48000; return true;
    case QEC64_HOST_FEATURES:
        if (write || size != 4) return false;
#ifdef __APPLE__
        *value = QEC64_FEATURE_NET | QEC64_FEATURE_GL;
#else
        *value = QEC64_FEATURE_NET;
#endif
        return true;
    case QEC64_IDLE: {
        /* The idle task writes this in a loop. The time between two such writes close together is
         * idle time. Sleeping here (QRX_EC64_IDLE_SLEEP=1) spares the host but still exposes an
         * SMP race in AROS 64, so by default the core keeps spinning. */
        static int sleep_on = -1;
        if (!write || size != 4) return false;
        if (sleep_on < 0) sleep_on = getenv("QRX_EC64_IDLE_SLEEP") != NULL;
        unsigned id = c->core_id < 8 ? c->core_id : 7;
        if (sleep_on) { b->idle_us[id] += qec64_machine_idle(c, 1000); return true; }
        uint64_t now = clock_us(), last = b->idle_last[id];
        if (last && now - last < 500) b->idle_us[id] += now - last;
        b->idle_last[id] = now;
        return true;
    }
    case QEC64_CORES:
        if (write || size != 4) return false;
        *value = b->cores; return true;
    case QEC64_RAM_SIZE:
        if (write || size != 8) return false;
        *value = b->ram_size; return true;
    case QEC64_UPTIME_US:
        if (write || size != 8) return false;
        *value = clock_us() - b->start_us; return true;
    case QEC64_CLOCK_UNIX:
        if (write || size != 8) return false;
        *value = (uint64_t)time(NULL); return true;
    default:
        if (!write && size == 8 && off >= QEC64_IDLE_US && off < QEC64_IDLE_US + 64 && !(off & 7)) {
            *value = b->idle_us[(off - QEC64_IDLE_US) / 8]; return true;
        }
        if (!write && size == 8 && off >= QEC64_HOST_CPU && off < QEC64_HOST_CPU + 64 && !(off & 7)) {
            uint64_t v = 0;
            for (unsigned i = 0; i < 8; i++) v = v << 8 | (uint8_t)b->host_cpu[off - QEC64_HOST_CPU + i];
            *value = v; return true;
        }
        return false;
    }
}

int qec64_board_framebuffer(const qec64_board *b, const uint8_t **pixels, uint32_t *w, uint32_t *h,
                            uint32_t *stride, uint32_t *format)
{
    uint64_t addr = b->fb_addr;
    uint32_t W = b->fb_width, H = b->fb_height, S = b->fb_stride, F = b->fb_format;
    uint32_t bpp = F == QEC64_FB_FMT_XRGB32 ? 4u : F == QEC64_FB_FMT_RGB565 ? 2u : 0u;
    if (!b->fb_enable || !bpp || !W || !H || W > 4096 || H > 4096 || S < W * bpp) return 0;
    if (addr >= b->ram_size || (uint64_t)S * H > b->ram_size - addr) return 0;
    *pixels = b->ram + addr;
    *w = W; *h = H; *stride = S; *format = F;
    return 1;
}
