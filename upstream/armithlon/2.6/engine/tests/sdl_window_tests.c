/* The Linux SDL window (linux/qrx_aros_window_sdl.c) with no display: SDL's offscreen driver.
 * Checks the RTG formats against the expected colours through the capture hook, and that SDL
 * keyboard and mouse events reach the guest's input queue as Amiga raw keys and buttons. */
#include "../macos/qrx_aros_window.h"
#include "qrxmedia.h"
#include <SDL2/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

volatile uint32_t qrx_host_view_size;           /* qrx86_door.c in the engine */
enum { W = 64, H = 48, FB = 0x10000u };
static uint8_t ram[1u << 20];
static uint32_t clut[256];

static void capture(const char *path, uint8_t *rgb)
{
    char head[32];
    unsigned w = 0, h = 0, max = 0;
    for (int i = 0; i < 400 && !getenv("QRX_RTG_CAPTURE_DONE"); i++) {
        qrx_aros_window_pump();
        FILE *f = fopen(path, "rb");
        if (f) {
            assert(fscanf(f, "%31s %u %u %u", head, &w, &h, &max) == 4 && !strcmp(head, "P6"));
            fgetc(f);
            assert(w == W && h == H && max == 255);
            assert(fread(rgb, 1, (size_t)W * H * 3, f) == (size_t)W * H * 3);
            fclose(f);
            remove(path);
            return;
        }
        SDL_Delay(5);
    }
    assert(!"no capture");
}
static void expect(const uint8_t *rgb, unsigned x, unsigned y, unsigned r, unsigned g, unsigned b)
{
    const uint8_t *p = rgb + ((size_t)y * W + x) * 3;
    if (p[0] != r || p[1] != g || p[2] != b) {
        fprintf(stderr, "pixel %u,%u = %02x%02x%02x, want %02x%02x%02x\n", x, y, p[0], p[1], p[2], r, g, b);
        assert(0);
    }
}
static uint32_t next_input(QRXVM *vm)
{
    QRXMediaDevice *m = vm->media;
    unsigned r = atomic_load(&m->input_read), w = atomic_load(&m->input_write);
    if (r == w) return 0;
    uint32_t e = m->input_events[r % QRX_INPUT_QUEUE_SIZE];
    atomic_store(&m->input_read, r + 1);
    return e;
}
int main(void)
{
    static uint8_t rgb[W * H * 3];
    char path[] = "/tmp/qrx-sdl-window-XXXXXX";
    QRXVM vm;
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    remove(path);
    setenv("SDL_VIDEODRIVER", "offscreen", 0);
    setenv("SDL_AUDIODRIVER", "dummy", 0);
    setenv("QRX_RTG_CAPTURE", path, 1);
    setenv("QRX_RTG_CAPTURE_AT", "0", 1);
    setenv("QRX_RTG_CAPTURE_COUNT", "3", 1);
    setenv("QRX_RTG_CAPTURE_STEP", "0", 1);
    setenv("QRX_APPS_DIR", "/tmp", 1);
    qrx_vm_init(&vm, ram, sizeof ram);
    assert(qrx_aros_window_open(&vm, sizeof ram));
    char name[64];

    /* CLUT8: index = x, palette entry i = 0x00RRGGBB */
    for (int i = 0; i < 256; i++) clut[i] = (uint32_t)(i * 3 & 255) << 16 | (uint32_t)(i * 5 & 255) << 8 | (uint32_t)(255 - i);
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) ram[FB + y * W + x] = (uint8_t)(x + y);
    qrx_aros_window_publish(FB, W, W, H, 1, clut);
    snprintf(name, sizeof name, "%s.0", path);
    capture(name, rgb);
    expect(rgb, 0, 0, 0, 0, 255);
    expect(rgb, 10, 3, (13 * 3) & 255, (13 * 5) & 255, 255 - 13);
    puts("CLUT8 frame matches the palette");

    /* RGB565 big endian */
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
        uint16_t v = (uint16_t)((x & 31) << 11 | (y & 63) << 5 | 31);
        ram[FB + (y * W + x) * 2] = (uint8_t)(v >> 8); ram[FB + (y * W + x) * 2 + 1] = (uint8_t)v;
    }
    qrx_aros_window_publish(FB, W * 2, W, H, 2, NULL);
    snprintf(name, sizeof name, "%s.1", path);
    capture(name, rgb);
    expect(rgb, 31, 63 > H - 1 ? 47 : 63, 255, 47 * 255 / 63, 255);
    expect(rgb, 0, 0, 0, 0, 255);
    puts("RGB565 frame converted");

    /* ARGB32 big endian */
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
        uint8_t *p = ram + FB + (y * W + x) * 4;
        p[0] = 0xFF; p[1] = (uint8_t)(x * 4); p[2] = (uint8_t)(y * 5); p[3] = 0x80;
    }
    qrx_aros_window_publish(FB, W * 4, W, H, 5, NULL);
    snprintf(name, sizeof name, "%s.2", path);
    capture(name, rgb);
    expect(rgb, 12, 7, 48, 35, 0x80);
    puts("ARGB32 frame converted");

    /* keyboard: SDL scancodes -> Amiga raw keys, down and up */
    while (next_input(&vm)) {}
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN; e.key.keysym.scancode = SDL_SCANCODE_A; SDL_PushEvent(&e);
    e.type = SDL_KEYUP; SDL_PushEvent(&e);
    e.type = SDL_KEYDOWN; e.key.keysym.scancode = SDL_SCANCODE_LGUI; SDL_PushEvent(&e);
    e.type = SDL_KEYDOWN; e.key.keysym.scancode = SDL_SCANCODE_UP; SDL_PushEvent(&e);
    qrx_aros_window_pump();
    assert(next_input(&vm) == (1u << 24 | 0x20u << 16));     /* A down */
    assert(next_input(&vm) == (2u << 24 | 0x20u << 16));     /* A up */
    assert(next_input(&vm) == (1u << 24 | 0x66u << 16));     /* left Amiga */
    assert(next_input(&vm) == (1u << 24 | 0x4Cu << 16));     /* cursor up */
    puts("keys arrive as Amiga raw keys");

    /* mouse: a button press reports the position, then the button (0 = left) */
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEBUTTONDOWN; e.button.button = SDL_BUTTON_RIGHT; e.button.x = 20; e.button.y = 30; SDL_PushEvent(&e);
    qrx_aros_window_pump();
    assert(next_input(&vm) == (3u << 24 | 20u << 12 | 30u));
    assert(next_input(&vm) == (4u << 24 | 1u << 16 | 1u));
    puts("mouse position and right button arrive");

    qrx_aros_window_close();
    puts("PASS");
    return 0;
}
