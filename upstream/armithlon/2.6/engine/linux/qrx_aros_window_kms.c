/* The AROS RTG screen of the 32-bit QRX OS One straight on the Linux console (linux/qrx_lio.c:
 * KMS/DRM, evdev, ALSA), without SDL: the Raspberry Pi appliance. Same contract as the Mac window
 * and the SDL one (macos/qrx_aros_window.h):
 *  - the guest CPU thread publishes the framebuffer (address, stride, size, format, CLUT);
 *  - the main thread converts only the rows that changed since the last frame (a shadow copy of
 *    the guest bytes tells which) into the frame buffer; the display hardware scales it to the
 *    screen and it is flipped at vertical blank. Nothing changed: no copy, no flip, the thread
 *    waits for input;
 *  - keys go to the guest as Amiga raw keys (Linux key codes; the Windows keys are the Amiga
 *    keys), the mouse as positions and buttons; the pointer is the display's hardware cursor;
 *  - AHI's sound ring (qrxhost.audio) plays through ALSA;
 *  - while a host-API program (Direct3D/Glide on the host GPU) presents, its frames are shown.
 * Unattended checks as with SDL: QRX_TEST_KEYS, QRX_TEST_CLICKS, QRX_TEST_KEY(_AT),
 * QRX_RTG_CAPTURE(_AT, _COUNT, _STEP); QRX_SCREEN_CAPTURE=path writes what the screen shows. */
#include "../macos/qrx_aros_window.h"
#include "../full68k/qrx86_door.h"
#include "qrx_hostgpu.h"
#include "qrx_hostfs.h"
#include "qrx_lio.h"
#include "qrxmedia.h"
#include <linux/input.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static QRXMediaDevice media;
static QRXHostFS apps_disk;
static int apps_mounted;
static QRXVM *guest_vm;
static uint8_t *ram_base;
static size_t ram_bytes;
static int running;
static atomic_uint_fast64_t present_fb, present_geom;
static const uint32_t *present_clut;
static atomic_uint pointer_want = 0;
static uint8_t keys_down[128];
static unsigned buttons;
static uint64_t audio_frames_heard;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

/* Linux key codes -> Amiga raw keys; 0xFF: none */
static unsigned rawkey(unsigned k)
{
    static uint8_t map[256];
    static int init;
    if (!init) {
        static const uint16_t pairs[][2] = {
            {KEY_GRAVE,0x00},{KEY_1,0x01},{KEY_2,0x02},{KEY_3,0x03},{KEY_4,0x04},{KEY_5,0x05},{KEY_6,0x06},{KEY_7,0x07},
            {KEY_8,0x08},{KEY_9,0x09},{KEY_0,0x0A},{KEY_MINUS,0x0B},{KEY_EQUAL,0x0C},{KEY_BACKSLASH,0x0D},
            {KEY_Q,0x10},{KEY_W,0x11},{KEY_E,0x12},{KEY_R,0x13},{KEY_T,0x14},{KEY_Y,0x15},{KEY_U,0x16},{KEY_I,0x17},
            {KEY_O,0x18},{KEY_P,0x19},{KEY_LEFTBRACE,0x1A},{KEY_RIGHTBRACE,0x1B},
            {KEY_A,0x20},{KEY_S,0x21},{KEY_D,0x22},{KEY_F,0x23},{KEY_G,0x24},{KEY_H,0x25},{KEY_J,0x26},{KEY_K,0x27},
            {KEY_L,0x28},{KEY_SEMICOLON,0x29},{KEY_APOSTROPHE,0x2A},
            {KEY_102ND,0x30},{KEY_Z,0x31},{KEY_X,0x32},{KEY_C,0x33},{KEY_V,0x34},{KEY_B,0x35},{KEY_N,0x36},{KEY_M,0x37},
            {KEY_COMMA,0x38},{KEY_DOT,0x39},{KEY_SLASH,0x3A},
            {KEY_SPACE,0x40},{KEY_BACKSPACE,0x41},{KEY_TAB,0x42},{KEY_KPENTER,0x43},{KEY_ENTER,0x44},{KEY_ESC,0x45},
            {KEY_DELETE,0x46},{KEY_INSERT,0x5F},{KEY_PAGEUP,0x48},{KEY_PAGEDOWN,0x49},{KEY_HOME,0x70},{KEY_END,0x71},
            {KEY_UP,0x4C},{KEY_DOWN,0x4D},{KEY_RIGHT,0x4E},{KEY_LEFT,0x4F},
            {KEY_F1,0x50},{KEY_F2,0x51},{KEY_F3,0x52},{KEY_F4,0x53},{KEY_F5,0x54},{KEY_F6,0x55},{KEY_F7,0x56},
            {KEY_F8,0x57},{KEY_F9,0x58},{KEY_F10,0x59},{KEY_F11,0x4B},{KEY_F12,0x6F},
            {KEY_KP0,0x0F},{KEY_KP1,0x1D},{KEY_KP2,0x1E},{KEY_KP3,0x1F},{KEY_KP4,0x2D},{KEY_KP5,0x2E},{KEY_KP6,0x2F},
            {KEY_KP7,0x3D},{KEY_KP8,0x3E},{KEY_KP9,0x3F},{KEY_KPDOT,0x3C},{KEY_KPMINUS,0x4A},{KEY_KPPLUS,0x5E},
            {KEY_KPASTERISK,0x5D},{KEY_KPSLASH,0x5C},{KEY_NUMLOCK,0x5A},
            {KEY_LEFTSHIFT,0x60},{KEY_RIGHTSHIFT,0x61},{KEY_CAPSLOCK,0x62},{KEY_LEFTCTRL,0x63},{KEY_RIGHTCTRL,0x63},
            {KEY_LEFTALT,0x64},{KEY_RIGHTALT,0x65},{KEY_LEFTMETA,0x66},{KEY_RIGHTMETA,0x67},   /* the Amiga keys */
        };
        memset(map, 0xFF, sizeof map);
        for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) map[pairs[i][0]] = (uint8_t)pairs[i][1];
        init = 1;
    }
    return k < 256 ? map[k] : 0xFFu;
}
static void key(unsigned code, int down)
{
    if (code > 127) return;
    keys_down[code] = (uint8_t)down;
    qrx_media_push_input(&media, ((down ? 1u : 2u) << 24) | (code << 16));
}
static void mouse_at(int x, int y)
{
    unsigned gx = x < 0 ? 0u : (unsigned)x, gy = y < 0 ? 0u : (unsigned)y;
    qrx_media_set_mouse(&media, gx, gy, buttons);
    qrx_media_push_input(&media, (3u << 24) | ((gx & 4095u) << 12) | (gy & 4095u));
}

/* ---- pointer shape: chosen by the guest desktop through the door (3: crosshair) */
void qrx_host_set_pointer(uint32_t index) { atomic_store(&pointer_want, index); }

/* ---- framebuffer formats as published (see the Metal shader on the Mac) -> XRGB8888 */
typedef struct { uint32_t fb, stride, w, h, fmt; } Present;
static unsigned bytes_per_pixel(uint32_t fmt) { return fmt == 1 ? 1u : fmt == 2 || fmt == 3 ? 2u : fmt == 4 ? 3u : fmt == 5 ? 4u : 0u; }
static int present_ok(const Present *u)
{
    unsigned bpp = bytes_per_pixel(u->fmt);
    return u->w && u->h && bpp && u->stride >= u->w * bpp && (uint64_t)u->fb + (uint64_t)u->stride * u->h <= ram_bytes;
}
static void convert_row(const Present *u, const uint32_t *clut, const uint8_t *row, uint32_t *o)
{
    switch (u->fmt) {
    case 1: for (uint32_t x = 0; x < u->w; x++) o[x] = clut ? clut[row[x]] & 0xFFFFFFu : 0; break;
    case 2: for (uint32_t x = 0; x < u->w; x++) {
                uint32_t v = (uint32_t)row[x * 2] << 8 | row[x * 2 + 1];
                uint32_t r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
                o[x] = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2); }
            break;
    case 3: for (uint32_t x = 0; x < u->w; x++) {
                uint32_t v = (uint32_t)row[x * 2] << 8 | row[x * 2 + 1];
                uint32_t r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
                o[x] = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2); }
            break;
    case 4: for (uint32_t x = 0; x < u->w; x++) o[x] = (uint32_t)row[x * 3] << 16 | (uint32_t)row[x * 3 + 1] << 8 | row[x * 3 + 2]; break;
    case 5: for (uint32_t x = 0; x < u->w; x++) o[x] = (uint32_t)row[x * 4 + 1] << 16 | (uint32_t)row[x * 4 + 2] << 8 | row[x * 4 + 3]; break;
    default: break;
    }
}
static Present current_present(void)
{
    uint64_t a = atomic_load_explicit(&present_fb, memory_order_acquire), g = atomic_load_explicit(&present_geom, memory_order_acquire);
    Present u = {(uint32_t)(a >> 32), (uint32_t)a, (uint32_t)(g >> 32) >> 16, (uint32_t)(g >> 32) & 0xFFFFu, (uint32_t)g};
    return u;
}

static uint8_t *shadow;
static size_t shadow_cap;
static Present shown;
static uint32_t shown_clut[256];
static int shown_valid;
/* 1 when a frame was shown (paced by the vertical blank), 0 when nothing changed */
static int present_guest(void)
{
    unsigned w, h, pitch;
    if (qrx_hg_active() && qrx_hg_read_frame(NULL, &w, &h)) {
        static uint8_t *rgba;
        static size_t cap;
        if ((size_t)w * h * 4 > cap) { free(rgba); rgba = malloc((size_t)w * h * 4); cap = rgba ? (size_t)w * h * 4 : 0; }
        uint32_t *px;
        if (rgba && qrx_hg_read_frame(rgba, &w, &h) && (px = qrx_lio_frame_begin(w, h, &pitch))) {
            for (size_t i = 0; i < (size_t)w * h; i++)
                px[i] = (uint32_t)rgba[i * 4] << 16 | (uint32_t)rgba[i * 4 + 1] << 8 | rgba[i * 4 + 2];
            qrx_lio_frame_end(0, h);
            shown_valid = 0;
            return 1;
        }
    }
    Present u = current_present();
    if (!present_ok(&u)) { shown_valid = 0; return 0; }
    size_t row_bytes = (size_t)u.w * bytes_per_pixel(u.fmt), total = (size_t)u.stride * u.h;
    if (total > shadow_cap) {
        uint8_t *n = realloc(shadow, total);
        if (!n) { shown_valid = 0; return 0; }
        shadow = n; shadow_cap = total;
    }
    uint32_t *px = qrx_lio_frame_begin(u.w, u.h, &pitch);
    if (!px) return 0;
    const uint8_t *fb = ram_base + u.fb;
    int full = !shown_valid || memcmp(&u, &shown, sizeof u) ||
               (u.fmt == 1 && present_clut && memcmp(present_clut, shown_clut, sizeof shown_clut));
    unsigned y0 = u.h, y1 = 0;
    if (full) {
        memcpy(shadow, fb, total);
        if (u.fmt == 1 && present_clut) memcpy(shown_clut, present_clut, sizeof shown_clut);
        for (unsigned y = 0; y < u.h; y++) convert_row(&u, present_clut, shadow + (size_t)y * u.stride, px + (size_t)y * u.w);
        y0 = 0; y1 = u.h;
        shown = u; shown_valid = 1;
    } else {
        for (unsigned y = 0; y < u.h; y++) {
            const uint8_t *row = fb + (size_t)y * u.stride;
            uint8_t *old = shadow + (size_t)y * u.stride;
            if (!memcmp(row, old, row_bytes)) continue;
            memcpy(old, row, row_bytes);
            convert_row(&u, present_clut, old, px + (size_t)y * u.w);
            if (y < y0) y0 = y;
            y1 = y + 1;
        }
        if (y0 >= y1) return 0;
    }
    qrx_lio_frame_end(y0, y1);
    return 1;
}
static void capture_guest(const char *path)
{
    Present u = current_present();
    if (!present_ok(&u)) { fprintf(stderr, "[RTG] nothing to capture %ux%u fmt=%u\n", u.w, u.h, u.fmt); return; }
    FILE *f = fopen(path, "wb");
    uint32_t *row = malloc((size_t)u.w * 4);
    if (!f || !row) { if (f) fclose(f); free(row); return; }
    fprintf(f, "P6\n%u %u\n255\n", u.w, u.h);
    for (unsigned y = 0; y < u.h; y++) {
        convert_row(&u, present_clut, ram_base + u.fb + (size_t)y * u.stride, row);
        for (unsigned x = 0; x < u.w; x++) { uint8_t rgb[3] = {(uint8_t)(row[x] >> 16), (uint8_t)(row[x] >> 8), (uint8_t)row[x]}; fwrite(rgb, 1, 3, f); }
    }
    free(row);
    fclose(f);
    fprintf(stderr, "[RTG] captured %ux%u fmt=%u to %s\n", u.w, u.h, u.fmt, path);
    if (audio_frames_heard) fprintf(stderr, "[AUDIO] %llu frames with sound so far\n", (unsigned long long)audio_frames_heard);
}

/* ---- AHI sound ring -> ALSA */
static size_t audio_pull(void *ctx, int16_t *out, size_t n)
{
    (void)ctx;
    size_t got = qrx_media_pull_audio(&media, out, n);
    for (size_t i = 0; i < got * 2u; i++) if (out[i]) { audio_frames_heard += got; break; }
    return got;
}

static void apps_open(QRXVM *vm)
{
    char path[1024];
    const char *env = getenv("QRX_APPS_DIR");
    if (env) snprintf(path, sizeof path, "%s", env);
    else {
        const char *home = getenv("HOME");
        snprintf(path, sizeof path, "%s/.local/share/armithlon", home ? home : ".");
        mkdir(path, 0755);
        strncat(path, "/Apps", sizeof path - strlen(path) - 1);
        mkdir(path, 0755);
    }
    if (qrx_hostfs_mount(&apps_disk, path, false)) {
        apps_mounted = 1;
        qrx_hostfs_attach2(vm, &apps_disk);
        fprintf(stderr, "[APPS] writable second disk: %s\n", path);
    } else fprintf(stderr, "[APPS] not available: %s\n", path);
}

int qrx_aros_window_open(QRXVM *vm, size_t ram_size)
{
    qrx_media_init(&media, NULL, NULL);
    qrx_vm_bind_media(vm, &media);
    guest_vm = vm; ram_base = vm->ram.data; ram_bytes = ram_size;
    if (!qrx_lio_open()) return 0;
    unsigned sw, sh;
    qrx_lio_screen(&sw, &sh);
    qrx_host_view_size = sw << 16 | (sh & 0xFFFFu);      /* the guest picks a matching screen mode */
    apps_open(vm);
    qrx_hg_set_layer(&media);                            /* frames are read back for this screen */
    qrx_lio_audio_open(audio_pull, NULL);
    running = 1;
    return 1;
}

/* QRX_TEST_* unattended input and the captures, timed from the opening */
static void test_hooks(void)
{
    static double t0;
    if (!t0) t0 = now_s();
    double t = now_s() - t0;
    {   /* QRX_TEST_KEYS=seconds:rawkey[:hold],... */
        static const char *next;
        static int init;
        static struct { unsigned key; double at; } held[8];
        if (!init) { init = 1; next = getenv("QRX_TEST_KEYS"); }
        for (int i = 0; i < 8; i++) if (held[i].key && t >= held[i].at) { qrx_media_push_input(&media, (2u << 24) | (held[i].key << 16)); held[i].key = 0; }
        while (next && *next) {
            char *e;
            double at = strtod(next, &e);
            if (*e != ':' || t < at) break;
            unsigned k = (unsigned)strtoul(e + 1, &e, 0);
            double hold = 0.15;
            if (*e == ':') hold = strtod(e + 1, &e);
            qrx_media_push_input(&media, (1u << 24) | (k << 16));
            for (int i = 0; i < 8; i++) if (!held[i].key) { held[i].key = k; held[i].at = t + hold; break; }
            next = *e == ',' ? e + 1 : "";
        }
    }
    {   /* QRX_TEST_CLICKS=seconds:x:y,... */
        static const char *next;
        static int init, pressed;
        if (!init) { init = 1; next = getenv("QRX_TEST_CLICKS"); }
        if (pressed && --pressed == 0) qrx_media_push_input(&media, (4u << 24) | 0u);
        if (!pressed && next && *next) {
            char *e;
            double at = strtod(next, &e);
            if (*e == ':' && t >= at) {
                unsigned x = (unsigned)strtoul(e + 1, &e, 0), y = 0;
                if (*e == ':') y = (unsigned)strtoul(e + 1, &e, 0);
                next = *e == ',' ? e + 1 : "";
                qrx_media_set_mouse(&media, x, y, 0);
                qrx_media_push_input(&media, (3u << 24) | ((x & 4095u) << 12) | (y & 4095u));
                qrx_media_push_input(&media, (4u << 24) | 1u);
                pressed = 3;
            }
        }
    }
    {   /* QRX_TEST_KEY=rawkey at QRX_TEST_KEY_AT seconds */
        static int stage;
        static double at = -1;
        const char *k = getenv("QRX_TEST_KEY");
        if (k && stage < 2) {
            if (at < 0) { const char *a = getenv("QRX_TEST_KEY_AT"); at = a ? atof(a) : 20.0; }
            if (t >= at) { qrx_media_push_input(&media, ((stage == 0 ? 1u : 2u) << 24) | ((unsigned)strtoul(k, NULL, 0) << 16)); stage++; at = t + 0.15; }
        }
    }
    {   /* QRX_RTG_CAPTURE=path at QRX_RTG_CAPTURE_AT; _COUNT=n as path.0..n-1, _STEP apart */
        static int left = -1, index;
        static double at;
        const char *path = getenv("QRX_RTG_CAPTURE");
        if (path && left) {
            if (left < 0) { const char *a = getenv("QRX_RTG_CAPTURE_AT"), *n = getenv("QRX_RTG_CAPTURE_COUNT"); at = a ? atof(a) : 20.0; left = n ? atoi(n) : 1; }
            if (t >= at) {
                char name[1024];
                if (getenv("QRX_RTG_CAPTURE_COUNT")) snprintf(name, sizeof name, "%s.%d", path, index++);
                else snprintf(name, sizeof name, "%s", path);
                capture_guest(name);
                left--;
                at = t + (getenv("QRX_RTG_CAPTURE_STEP") ? atof(getenv("QRX_RTG_CAPTURE_STEP")) : 0.067);
            }
        }
    }
    {   /* QRX_SCREEN_CAPTURE=path: the whole screen as shown (scaled, letterboxed), once, at
         * QRX_SCREEN_CAPTURE_AT; QRX_SCREEN_CAPTURE_REPEAT: whenever the file is gone */
        static int done;
        const char *path = getenv("QRX_SCREEN_CAPTURE");
        const char *a = getenv("QRX_SCREEN_CAPTURE_AT");
        if (path && t >= (a ? atof(a) : 20.0)) {
            if (getenv("QRX_SCREEN_CAPTURE_REPEAT")) { FILE *f = fopen(path, "rb"); if (f) fclose(f); else done = 0; }
            if (!done) {
                char tmp[1024];
                snprintf(tmp, sizeof tmp, "%s.part", path);
                if (qrx_lio_capture(tmp)) rename(tmp, path);
                done = 1;
            }
        }
    }
}

int qrx_aros_window_pump(void)
{
    qrx_lio_event e;
    int x, y;
    int shown_now = present_guest();
    test_hooks();                          /* after drawing: a screen capture shows this frame */
    /* input: everything waiting; when nothing was shown, wait up to 8 ms for some */
    for (int wait = shown_now ? 0 : 8; qrx_lio_poll(&e, wait); wait = 0) {
        switch (e.type) {
        case QRX_LIO_KEY: { unsigned code = rawkey(e.code); if (code != 0xFF) key(code, e.value); break; }
        case QRX_LIO_MOVE: mouse_at(e.x, e.y); break;
        case QRX_LIO_BUTTON:
            buttons = e.value ? buttons | (1u << e.code) : buttons & ~(1u << e.code);
            qrx_lio_pointer_pos(&x, &y);
            mouse_at(x, y);
            qrx_media_push_input(&media, (4u << 24) | (e.code << 16) | (e.value ? 1u : 0u));
            break;
        case QRX_LIO_QUIT: running = 0; break;
        default: break;
        }
    }
    qrx_lio_pointer_pos(&x, &y);
    unsigned pw = atomic_load(&pointer_want);
    qrx_lio_pointer(pw > 4 ? 0 : (int)pw, x, y);
    return running;
}

void qrx_aros_window_publish(uint32_t fb, uint32_t stride, uint32_t w, uint32_t h, uint32_t fmt, const uint32_t *clut)
{
    present_clut = clut;
    atomic_store_explicit(&present_fb, ((uint64_t)fb << 32) | stride, memory_order_release);
    atomic_store_explicit(&present_geom, ((uint64_t)((w << 16) | (h & 0xFFFFu)) << 32) | fmt, memory_order_release);
}

void qrx_aros_window_close(void)
{
    if (audio_frames_heard) fprintf(stderr, "[AUDIO] %llu frames with sound played\n", (unsigned long long)audio_frames_heard);
    for (unsigned k = 0; k < 128; k++) if (keys_down[k]) key(k, 0);
    qrx_hg_set_layer(NULL);
    qrx_lio_close();
    running = 0;
    if (guest_vm) qrx_hostfs_attach2(guest_vm, NULL);
    if (apps_mounted) qrx_hostfs_unmount(&apps_disk);
    apps_mounted = 0;
    guest_vm = NULL;
    free(shadow); shadow = NULL; shadow_cap = 0; shown_valid = 0;
}
