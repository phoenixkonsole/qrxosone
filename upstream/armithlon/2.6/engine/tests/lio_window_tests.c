/* The console window of the 32-bit QRX OS One (linux/qrx_aros_window_kms.c on linux/qrx_lio.c)
 * without hardware: QRX_LIO_TEST. Checks the RTG formats (guest capture), the scaling onto the
 * screen (screen capture: aspect kept, letterboxed), evdev keys as Amiga raw keys, relative mouse
 * motion and buttons, and a keyboard plugged in after the start (inotify). */
#include "../macos/qrx_aros_window.h"
#include "qrxmedia.h"
#include <assert.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

volatile uint32_t qrx_host_view_size;           /* qrx86_door.c in the engine */
enum { W = 64, H = 48, FB = 0x10000u };
static uint8_t ram[1u << 20];
static uint32_t clut[256];

static void wait_file(const char *path)
{
    for (int i = 0; i < 400; i++) {
        qrx_aros_window_pump();
        if (access(path, R_OK) == 0) return;
    }
    assert(!"no capture");
}
static uint8_t *read_ppm(const char *path, unsigned *w, unsigned *h)
{
    char head[8];
    unsigned max;
    FILE *f = fopen(path, "rb");
    assert(f && fscanf(f, "%7s %u %u %u", head, w, h, &max) == 4 && !strcmp(head, "P6") && max == 255);
    fgetc(f);
    uint8_t *p = malloc((size_t)*w * *h * 3);
    assert(p && fread(p, 1, (size_t)*w * *h * 3, f) == (size_t)*w * *h * 3);
    fclose(f);
    remove(path);
    return p;
}
static void expect(const uint8_t *rgb, unsigned w, unsigned x, unsigned y, unsigned r, unsigned g, unsigned b)
{
    const uint8_t *p = rgb + ((size_t)y * w + x) * 3;
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
static void ev(int fd, unsigned type, unsigned code, int value)
{
    struct input_event e = { .type = (uint16_t)type, .code = (uint16_t)code, .value = value };
    assert(write(fd, &e, sizeof e) == (ssize_t)sizeof e);
}

int main(void)
{
    char dir[] = "/tmp/qrx-lio-XXXXXX", path[128], name[160], dev0[160], dev1[160];
    assert(mkdtemp(dir));
    snprintf(dev0, sizeof dev0, "%s/event0", dir);
    assert(mkfifo(dev0, 0600) == 0);
    int in0 = open(dev0, O_RDWR);
    assert(in0 >= 0);
    snprintf(path, sizeof path, "%s/rtg", dir);
    setenv("QRX_LIO_TEST", dir, 1);
    setenv("QRX_LIO_TEST_SIZE", "320x200", 1);
    setenv("QRX_ALSA_DEVICE", "null", 0);
    setenv("QRX_RTG_CAPTURE", path, 1);
    setenv("QRX_RTG_CAPTURE_AT", "0", 1);
    setenv("QRX_RTG_CAPTURE_COUNT", "3", 1);
    setenv("QRX_RTG_CAPTURE_STEP", "0", 1);
    setenv("QRX_APPS_DIR", "/tmp", 1);
    QRXVM vm;
    qrx_vm_init(&vm, ram, sizeof ram);
    assert(qrx_aros_window_open(&vm, sizeof ram));
    assert(qrx_host_view_size == (320u << 16 | 200u));
    unsigned w, h;
    uint8_t *rgb;

    /* CLUT8: index = x + y, palette entry i = 0x00RRGGBB */
    for (int i = 0; i < 256; i++) clut[i] = (uint32_t)(i * 3 & 255) << 16 | (uint32_t)(i * 5 & 255) << 8 | (uint32_t)(255 - i);
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) ram[FB + y * W + x] = (uint8_t)(x + y);
    qrx_aros_window_publish(FB, W, W, H, 1, clut);
    snprintf(name, sizeof name, "%s.0", path);
    wait_file(name);
    rgb = read_ppm(name, &w, &h);
    assert(w == W && h == H);
    expect(rgb, W, 0, 0, 0, 0, 255);
    expect(rgb, W, 10, 3, (13 * 3) & 255, (13 * 5) & 255, 255 - 13);
    free(rgb);
    puts("CLUT8 frame matches the palette");

    /* RGB565 and ARGB32, both big endian */
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
        uint16_t v = (uint16_t)((x & 31) << 11 | (y & 63) << 5 | 31);
        ram[FB + (y * W + x) * 2] = (uint8_t)(v >> 8); ram[FB + (y * W + x) * 2 + 1] = (uint8_t)v;
    }
    qrx_aros_window_publish(FB, W * 2, W, H, 2, NULL);
    snprintf(name, sizeof name, "%s.1", path);
    wait_file(name);
    rgb = read_ppm(name, &w, &h);
    expect(rgb, W, 31, 47, 255, (47 << 2 | 47 >> 4), 255);
    expect(rgb, W, 0, 0, 0, 0, 255);
    free(rgb);
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
        uint8_t *p = ram + FB + (y * W + x) * 4;
        p[0] = 0xFF; p[1] = (uint8_t)(x * 4); p[2] = (uint8_t)(y * 5); p[3] = 0x80;
    }
    qrx_aros_window_publish(FB, W * 4, W, H, 5, NULL);
    snprintf(name, sizeof name, "%s.2", path);
    wait_file(name);
    rgb = read_ppm(name, &w, &h);
    expect(rgb, W, 12, 7, 48, 35, 0x80);
    free(rgb);
    puts("RGB565 and ARGB32 frames converted");

    /* the screen: 64x48 scaled by 200/48 to 267x200, 26 pixels of black at the sides */
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) ram[FB + y * W + x] = (uint8_t)(x + y);
    qrx_aros_window_publish(FB, W, W, H, 1, clut);
    snprintf(name, sizeof name, "%s/screen", dir);
    setenv("QRX_SCREEN_CAPTURE", name, 1);
    setenv("QRX_SCREEN_CAPTURE_AT", "0", 1);
    setenv("QRX_SCREEN_CAPTURE_REPEAT", "1", 1);
    for (int i = 0; i < 5; i++) qrx_aros_window_pump();
    wait_file(name);
    rgb = read_ppm(name, &w, &h);
    assert(w == 320 && h == 200);
    expect(rgb, w, 10, 100, 0, 0, 0);               /* letterbox */
    expect(rgb, w, 310, 100, 0, 0, 0);
    expect(rgb, w, 27, 0, 0, 0, 255);               /* frame pixel 0,0 */
    {   /* frame pixel 32,24 lands at 26 + 32 * 267 / 64 = 159, 24 * 200 / 48 = 100 */
        unsigned i = 32 + 24;
        expect(rgb, w, 160, 101, (i * 3) & 255, (i * 5) & 255, 255 - i);
    }
    free(rgb);
    unsetenv("QRX_SCREEN_CAPTURE");
    puts("screen: aspect kept, letterboxed");


    /* keyboard: Linux key codes -> Amiga raw keys, down and up; repeats are not passed on */
    while (next_input(&vm)) {}
    ev(in0, EV_KEY, KEY_A, 1); ev(in0, EV_KEY, KEY_A, 2); ev(in0, EV_KEY, KEY_A, 0);
    ev(in0, EV_KEY, KEY_LEFTMETA, 1); ev(in0, EV_KEY, KEY_UP, 1); ev(in0, EV_SYN, SYN_REPORT, 0);
    for (int i = 0; i < 3; i++) qrx_aros_window_pump();
    assert(next_input(&vm) == (1u << 24 | 0x20u << 16));     /* A down */
    assert(next_input(&vm) == (2u << 24 | 0x20u << 16));     /* A up */
    assert(next_input(&vm) == (1u << 24 | 0x66u << 16));     /* left Amiga */
    assert(next_input(&vm) == (1u << 24 | 0x4Cu << 16));     /* cursor up */
    assert(next_input(&vm) == 0);
    puts("evdev keys arrive as Amiga raw keys");

    /* mouse: relative motion in frame pixels, clamped to the frame; then the right button */
    ev(in0, EV_REL, REL_X, 20); ev(in0, EV_REL, REL_Y, 30); ev(in0, EV_SYN, SYN_REPORT, 0);
    ev(in0, EV_KEY, BTN_RIGHT, 1); ev(in0, EV_SYN, SYN_REPORT, 0);
    for (int i = 0; i < 3; i++) qrx_aros_window_pump();
    assert(next_input(&vm) == (3u << 24 | 20u << 12 | 30u));
    assert(next_input(&vm) == (3u << 24 | 20u << 12 | 30u));
    assert(next_input(&vm) == (4u << 24 | 1u << 16 | 1u));
    ev(in0, EV_REL, REL_X, 500); ev(in0, EV_REL, REL_Y, -500); ev(in0, EV_SYN, SYN_REPORT, 0);
    for (int i = 0; i < 3; i++) qrx_aros_window_pump();
    assert(next_input(&vm) == (3u << 24 | (unsigned)(W - 1) << 12 | 0u));
    puts("mouse moves in frame pixels, clamped; buttons arrive");

    /* a keyboard plugged in later */
    snprintf(dev1, sizeof dev1, "%s/event1", dir);
    assert(mkfifo(dev1, 0600) == 0);
    int in1 = open(dev1, O_RDWR);
    assert(in1 >= 0);
    for (int i = 0; i < 3; i++) qrx_aros_window_pump();
    ev(in1, EV_KEY, KEY_ESC, 1); ev(in1, EV_SYN, SYN_REPORT, 0);
    for (int i = 0; i < 3; i++) qrx_aros_window_pump();
    while (next_input(&vm) >> 24 == 3u) {}
    puts("a device plugged in later is found");
    {
        ev(in1, EV_KEY, KEY_ESC, 0); ev(in1, EV_SYN, SYN_REPORT, 0);
        for (int i = 0; i < 3; i++) qrx_aros_window_pump();
        uint32_t e = next_input(&vm);
        assert(e == (2u << 24 | 0x45u << 16));
    }

    qrx_aros_window_close();
    close(in0); close(in1);
    remove(dev0); remove(dev1); rmdir(dir);
    puts("PASS");
    return 0;
}
