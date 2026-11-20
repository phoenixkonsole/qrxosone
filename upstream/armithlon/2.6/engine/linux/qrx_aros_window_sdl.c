/* Linux (Raspberry Pi) version of macos/qrx_aros_window.m: the AROS RTG framebuffer in an SDL2
 * window. SDL picks the display system: X11 or Wayland on the Pi desktop, KMS/DRM on the console.
 *
 * Same contract as on the Mac (qrx_aros_window.h):
 *  - the guest CPU thread publishes the framebuffer (address, stride, size, format, CLUT);
 *  - the main thread copies it once per refresh, converts it to XRGB and shows it scaled with the
 *    aspect kept (SDL's logical size, which also maps mouse coordinates back to guest pixels);
 *  - keys go to the guest as Amiga raw keys, the mouse as positions and buttons (qrxmedia);
 *  - AHI's sound ring (qrxhost.audio) plays through SDL audio;
 *  - while a host-API program (Direct3D/Glide on the host GPU) presents, its frames are shown.
 * Unattended checks, as on the Mac: QRX_TEST_KEYS, QRX_TEST_CLICKS, QRX_TEST_KEY(_AT),
 * QRX_RTG_CAPTURE(_AT, _COUNT, _STEP). QRX_FULLSCREEN=1 starts in full screen.
 * Not yet on Linux: the QRX3D accelerated stream (Metal renderer on the Mac), the Apps-folder
 * menu (set QRX_APPS_DIR), the drawn pointer shapes (system cursors stand in). */
#include "../macos/qrx_aros_window.h"
#include "../full68k/qrx86_door.h"
#include "qrx_hostgpu.h"
#include "qrx_hostfs.h"
#include "qrxmedia.h"
#include <SDL2/SDL.h>
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
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tex_w, tex_h;
static uint32_t *pixels;
static size_t pixels_cap;
static SDL_AudioDeviceID audio;
static uint64_t audio_frames_heard;
static int running;
static atomic_uint_fast64_t present_fb, present_geom;
static const uint32_t *present_clut;
static atomic_uint pointer_want;
static unsigned pointer_have = 0xFFFFFFFFu;
static uint8_t keys_down[128];
static unsigned guest_w = 1024, guest_h = 768;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

/* USB HID scancodes (SDL) -> Amiga raw keys; 0xFF: none */
static unsigned rawkey(SDL_Scancode s)
{
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const unsigned char az[26] = {0x20,0x35,0x33,0x22,0x12,0x23,0x24,0x25,0x17,0x26,0x27,0x28,0x37,
                                             0x36,0x18,0x19,0x10,0x13,0x21,0x14,0x16,0x34,0x11,0x32,0x15,0x31};
        return az[s - SDL_SCANCODE_A];
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0) return 0x01u + (unsigned)(s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return 0x50u + (unsigned)(s - SDL_SCANCODE_F1);
    switch (s) {
    case SDL_SCANCODE_GRAVE: return 0x00;          case SDL_SCANCODE_MINUS: return 0x0B;
    case SDL_SCANCODE_EQUALS: return 0x0C;         case SDL_SCANCODE_BACKSLASH: return 0x0D;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1A;    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_SEMICOLON: return 0x29;      case SDL_SCANCODE_APOSTROPHE: return 0x2A;
    case SDL_SCANCODE_NONUSHASH: return 0x2B;      case SDL_SCANCODE_NONUSBACKSLASH: return 0x30;
    case SDL_SCANCODE_COMMA: return 0x38;          case SDL_SCANCODE_PERIOD: return 0x39;
    case SDL_SCANCODE_SLASH: return 0x3A;          case SDL_SCANCODE_SPACE: return 0x40;
    case SDL_SCANCODE_BACKSPACE: return 0x41;      case SDL_SCANCODE_TAB: return 0x42;
    case SDL_SCANCODE_KP_ENTER: return 0x43;       case SDL_SCANCODE_RETURN: return 0x44;
    case SDL_SCANCODE_ESCAPE: return 0x45;         case SDL_SCANCODE_DELETE: return 0x46;
    case SDL_SCANCODE_INSERT: return 0x5F;         /* Help, where the Mac maps its Help/Insert key */
    case SDL_SCANCODE_PAGEUP: return 0x48;         case SDL_SCANCODE_PAGEDOWN: return 0x49;
    case SDL_SCANCODE_HOME: return 0x70;           case SDL_SCANCODE_END: return 0x71;
    case SDL_SCANCODE_UP: return 0x4C;             case SDL_SCANCODE_DOWN: return 0x4D;
    case SDL_SCANCODE_RIGHT: return 0x4E;          case SDL_SCANCODE_LEFT: return 0x4F;
    case SDL_SCANCODE_F11: return 0x4B;            case SDL_SCANCODE_F12: return 0x6F;
    case SDL_SCANCODE_KP_0: return 0x0F;           case SDL_SCANCODE_KP_1: return 0x1D;
    case SDL_SCANCODE_KP_2: return 0x1E;           case SDL_SCANCODE_KP_3: return 0x1F;
    case SDL_SCANCODE_KP_4: return 0x2D;           case SDL_SCANCODE_KP_5: return 0x2E;
    case SDL_SCANCODE_KP_6: return 0x2F;           case SDL_SCANCODE_KP_7: return 0x3D;
    case SDL_SCANCODE_KP_8: return 0x3E;           case SDL_SCANCODE_KP_9: return 0x3F;
    case SDL_SCANCODE_KP_PERIOD: return 0x3C;      case SDL_SCANCODE_KP_MINUS: return 0x4A;
    case SDL_SCANCODE_KP_PLUS: return 0x5E;        case SDL_SCANCODE_KP_MULTIPLY: return 0x5D;
    case SDL_SCANCODE_KP_DIVIDE: return 0x5C;      case SDL_SCANCODE_NUMLOCKCLEAR: return 0x5A;
    case SDL_SCANCODE_LSHIFT: return 0x60;         case SDL_SCANCODE_RSHIFT: return 0x61;
    case SDL_SCANCODE_CAPSLOCK: return 0x62;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x63;
    case SDL_SCANCODE_LALT: return 0x64;           case SDL_SCANCODE_RALT: return 0x65;
    case SDL_SCANCODE_LGUI: return 0x66;           case SDL_SCANCODE_RGUI: return 0x67;   /* the Amiga keys */
    default: return 0xFF;
    }
}
static void key(unsigned code, int down, int repeat)
{
    if (code > 127) return;
    keys_down[code] = (uint8_t)down;
    qrx_media_push_input(&media, ((down ? 1u : 2u) << 24) | (code << 16) | (repeat ? 1u : 0u));
}
static void release_all_keys(void)
{
    for (unsigned k = 0; k < 128; k++) if (keys_down[k]) key(k, 0, 0);
}
static unsigned clampu(int v, unsigned hi) { return v < 0 ? 0u : (unsigned)v >= hi ? hi - 1u : (unsigned)v; }
static void mouse_at(int x, int y)
{
    unsigned gx = clampu(x, guest_w), gy = clampu(y, guest_h);
    uint32_t b = SDL_GetMouseState(NULL, NULL), mb = 0;
    if (b & SDL_BUTTON_LMASK) mb |= 1u;
    if (b & SDL_BUTTON_RMASK) mb |= 2u;
    if (b & SDL_BUTTON_MMASK) mb |= 4u;
    qrx_media_set_mouse(&media, gx, gy, mb);
    qrx_media_push_input(&media, (3u << 24) | ((gx & 4095u) << 12) | (gy & 4095u));
}

/* ---- pointer shape: chosen by the guest desktop through the door; set on the main thread */
void qrx_host_set_pointer(uint32_t index) { atomic_store(&pointer_want, index); }
static void apply_pointer(void)
{
    static SDL_Cursor *arrow, *cross;
    unsigned want = atomic_load(&pointer_want);
    if (want == pointer_have) return;
    pointer_have = want;
    if (!arrow) { arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW); cross = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR); }
    SDL_SetCursor(want == 3 && cross ? cross : arrow);
}

/* ---- framebuffer formats as published (see the Metal shader on the Mac) -> XRGB8888 */
typedef struct { uint32_t fb, stride, w, h, fmt; } Present;
static int convert(const Present *u, const uint32_t *clut, uint32_t *out)
{
    size_t bytes = (size_t)u->stride * u->h;
    if (!u->w || !u->h || !u->fmt || (uint64_t)u->fb + bytes > ram_bytes) return 0;
    for (uint32_t y = 0; y < u->h; y++) {
        const uint8_t *row = ram_base + u->fb + (size_t)y * u->stride;
        uint32_t *o = out + (size_t)y * u->w;
        switch (u->fmt) {
        case 1: for (uint32_t x = 0; x < u->w; x++) o[x] = clut ? clut[row[x]] & 0xFFFFFFu : 0; break;
        case 2: for (uint32_t x = 0; x < u->w; x++) {
                    uint32_t v = (uint32_t)row[x * 2] << 8 | row[x * 2 + 1];
                    o[x] = (((v >> 11) & 31) * 255 / 31) << 16 | (((v >> 5) & 63) * 255 / 63) << 8 | (v & 31) * 255 / 31; }
                break;
        case 3: for (uint32_t x = 0; x < u->w; x++) {
                    uint32_t v = (uint32_t)row[x * 2] << 8 | row[x * 2 + 1];
                    o[x] = (((v >> 10) & 31) * 255 / 31) << 16 | (((v >> 5) & 31) * 255 / 31) << 8 | (v & 31) * 255 / 31; }
                break;
        case 4: for (uint32_t x = 0; x < u->w; x++) o[x] = (uint32_t)row[x * 3] << 16 | (uint32_t)row[x * 3 + 1] << 8 | row[x * 3 + 2]; break;
        case 5: for (uint32_t x = 0; x < u->w; x++) o[x] = (uint32_t)row[x * 4 + 1] << 16 | (uint32_t)row[x * 4 + 2] << 8 | row[x * 4 + 3]; break;
        default: return 0;
        }
    }
    return 1;
}
static Present current_present(void)
{
    uint64_t a = atomic_load_explicit(&present_fb, memory_order_acquire), g = atomic_load_explicit(&present_geom, memory_order_acquire);
    Present u = {(uint32_t)(a >> 32), (uint32_t)a, (uint32_t)(g >> 32) >> 16, (uint32_t)(g >> 32) & 0xFFFFu, (uint32_t)g};
    return u;
}
static int ensure_pixels(size_t n)
{
    if (n <= pixels_cap) return 1;
    uint32_t *p = realloc(pixels, n * 4);
    if (!p) return 0;
    pixels = p; pixels_cap = n;
    return 1;
}
static void show(unsigned w, unsigned h)
{
    if (!tex || tex_w != (int)w || tex_h != (int)h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
        tex_w = (int)w; tex_h = (int)h;
        SDL_RenderSetLogicalSize(ren, (int)w, (int)h);
    }
    if (!tex) return;
    SDL_UpdateTexture(tex, NULL, pixels, (int)w * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);                          /* paced by vsync */
}
static void present_guest(void)
{
    unsigned w, h;
    if (qrx_hg_active() && qrx_hg_read_frame(NULL, &w, &h) && ensure_pixels((size_t)w * h)) {
        static uint8_t *rgba;
        static size_t cap;
        if ((size_t)w * h * 4 > cap) { free(rgba); rgba = malloc((size_t)w * h * 4); cap = rgba ? (size_t)w * h * 4 : 0; }
        if (rgba && qrx_hg_read_frame(rgba, &w, &h)) {
            for (size_t i = 0; i < (size_t)w * h; i++)
                pixels[i] = (uint32_t)rgba[i * 4] << 16 | (uint32_t)rgba[i * 4 + 1] << 8 | rgba[i * 4 + 2];
            guest_w = w; guest_h = h;
            show(w, h);
            return;
        }
    }
    Present u = current_present();
    if (!u.w || !u.h || !ensure_pixels((size_t)u.w * u.h) || !convert(&u, present_clut, pixels)) {
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
        return;
    }
    guest_w = u.w; guest_h = u.h;
    show(u.w, u.h);
}
static void capture_guest(const char *path)
{
    Present u = current_present();
    if (!u.w || !u.h || !ensure_pixels((size_t)u.w * u.h) || !convert(&u, present_clut, pixels)) {
        fprintf(stderr, "[RTG] nothing to capture %ux%u fmt=%u\n", u.w, u.h, u.fmt);
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", u.w, u.h);
    for (size_t i = 0; i < (size_t)u.w * u.h; i++) { uint8_t rgb[3] = {pixels[i] >> 16, pixels[i] >> 8, pixels[i]}; fwrite(rgb, 1, 3, f); }
    fclose(f);
    fprintf(stderr, "[RTG] captured %ux%u fmt=%u to %s\n", u.w, u.h, u.fmt, path);
    if (audio_frames_heard) fprintf(stderr, "[AUDIO] %llu frames with sound so far\n", (unsigned long long)audio_frames_heard);
}

/* ---- AHI sound ring -> SDL audio */
static void audio_callback(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    size_t frames = (size_t)len / 4u, got = qrx_media_pull_audio(&media, (int16_t *)stream, frames);
    const int16_t *pcm = (const int16_t *)stream;
    for (size_t i = 0; i < got * 2u; i++) if (pcm[i]) { audio_frames_heard += got; break; }
    if (got < frames) memset(stream + got * 4u, 0, (frames - got) * 4u);
}
static void audio_open(void)
{
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = QRX_AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    want.callback = audio_callback;
    audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!audio) { fprintf(stderr, "[AHI] no audio device: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(audio, 0);
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
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0 && SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[RTG] SDL: %s\n", SDL_GetError());
        return 0;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (getenv("QRX_FULLSCREEN")) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    win = SDL_CreateWindow("QRX OS One \xe2\x80\x94 ARMithlon / AROS RTG", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768, flags);
    if (!win) { fprintf(stderr, "[RTG] window: %s\n", SDL_GetError()); return 0; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "[RTG] renderer: %s\n", SDL_GetError()); return 0; }
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    {
        int w, h;
        SDL_GetWindowSize(win, &w, &h);
        qrx_host_view_size = (uint32_t)w << 16 | ((uint32_t)h & 0xFFFFu);
    }
    apps_open(vm);
    qrx_hg_set_layer(win);                           /* frames are read back for this window */
    audio_open();
    running = 1;
    return 1;
}

/* QRX_TEST_* unattended input and QRX_RTG_CAPTURE, timed from the window's opening */
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
}

int qrx_aros_window_pump(void)
{
    SDL_Event e;
    test_hooks();
    apply_pointer();
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: running = 0; break;
        case SDL_KEYDOWN: case SDL_KEYUP: {
            unsigned code = rawkey(e.key.keysym.scancode);
            if (code != 0xFF) key(code, e.type == SDL_KEYDOWN, e.key.repeat);
            break; }
        case SDL_MOUSEMOTION: mouse_at(e.motion.x, e.motion.y); break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            unsigned b = e.button.button == SDL_BUTTON_LEFT ? 0u : e.button.button == SDL_BUTTON_RIGHT ? 1u : e.button.button == SDL_BUTTON_MIDDLE ? 2u : 9u;
            mouse_at(e.button.x, e.button.y);
            if (b < 9u) qrx_media_push_input(&media, (4u << 24) | (b << 16) | (e.type == SDL_MOUSEBUTTONDOWN ? 1u : 0u));
            break; }
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) release_all_keys();
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                qrx_host_view_size = (uint32_t)e.window.data1 << 16 | ((uint32_t)e.window.data2 & 0xFFFFu);
            break;
        default: break;
        }
    }
    present_guest();
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
    if (audio) { SDL_CloseAudioDevice(audio); audio = 0; }
    qrx_hg_set_layer(NULL);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    tex = NULL; ren = NULL; win = NULL;
    running = 0;
    if (guest_vm) qrx_hostfs_attach2(guest_vm, NULL);
    if (apps_mounted) qrx_hostfs_unmount(&apps_disk);
    apps_mounted = 0;
    guest_vm = NULL;
    SDL_Quit();
}
