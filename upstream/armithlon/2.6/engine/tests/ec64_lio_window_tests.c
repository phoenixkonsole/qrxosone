/* The 68100EC64 console window (linux/qrx_ec64_window_kms.c on linux/qrx_lio.c) without
 * hardware: QRX_LIO_TEST. The console shows text; the framebuffer replaces it in both formats with
 * the right colours (screen capture); evdev keys reach the board as macOS key codes and typed text
 * the console input; the mouse reaches the board in framebuffer pixels. */
#include "../macos/qrx_ec64_window.h"
#include <assert.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { W = 64, H = 40, FB = 0x1000 };
static uint8_t ram[1u << 20];
static qec64_board board;
static char typed_text[64];
static size_t typed_n;
static void typed(void *ctx, const char *t, size_t n)
{
    (void)ctx;
    if (typed_n + n < sizeof typed_text) { memcpy(typed_text + typed_n, t, n); typed_n += n; }
}
static uint32_t next_event(void)
{
    if (board.ev_head == board.ev_tail) return 0;
    uint32_t e = board.events[board.ev_head];
    board.ev_head = (board.ev_head + 1) % 256u;
    return e;
}
static unsigned cw, ch;
static uint8_t *shot;
static char cap[160];
static void capture(void)
{
    remove(cap);
    for (int i = 0; i < 100; i++) {
        qec64_window_pump();
        FILE *f = fopen(cap, "rb");
        if (!f) continue;
        char head[8];
        unsigned max;
        assert(fscanf(f, "%7s %u %u %u", head, &cw, &ch, &max) == 4);
        fgetc(f);
        free(shot);
        shot = malloc((size_t)cw * ch * 3);
        assert(fread(shot, 1, (size_t)cw * ch * 3, f) == (size_t)cw * ch * 3);
        fclose(f);
        return;
    }
    assert(!"no capture");
}
static const uint8_t *at(double fx, double fy) { return shot + ((size_t)(fy * ch) * cw + (size_t)(fx * cw)) * 3; }
static void ev(int fd, unsigned type, unsigned code, int value)
{
    struct input_event e = { .type = (uint16_t)type, .code = (uint16_t)code, .value = value };
    assert(write(fd, &e, sizeof e) == (ssize_t)sizeof e);
}

int main(void)
{
    char dir[] = "/tmp/qrx-ec64-lio-XXXXXX", dev[160];
    assert(mkdtemp(dir));
    snprintf(dev, sizeof dev, "%s/event0", dir);
    assert(mkfifo(dev, 0600) == 0);
    int in = open(dev, O_RDWR);
    assert(in >= 0);
    snprintf(cap, sizeof cap, "%s/screen.ppm", dir);
    setenv("QRX_LIO_TEST", dir, 1);
    setenv("QRX_LIO_TEST_SIZE", "640x400", 1);
    setenv("QRX_ALSA_DEVICE", "null", 0);
    setenv("QRX_EC64_CAPTURE", cap, 1);
    setenv("QRX_EC64_CAPTURE_AT", "0", 1);
    setenv("QRX_EC64_CAPTURE_REPEAT", "1", 1);
    qec64_board_init(&board, ram, sizeof ram);
    qec64_window_input inp = { typed, NULL };
    assert(qec64_window_open("ec64 test", &board, &inp));
    assert(board.host_view == (640u << 16 | 400u) && board.host_screen == board.host_view);

    /* console text: light glyph pixels on the dark background */
    qec64_window_console("QRX OS One 64\nconsole\n", 22);
    capture();
    int light = 0;
    for (unsigned i = 0; i < cw * ch; i++) if (shot[i * 3] > 0xC0 && shot[i * 3 + 1] > 0xC0) light++;
    assert(light > 20);
    puts("console text shown");

    /* XRGB32, big endian: left half red, right half blue */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = ram + FB + (size_t)y * W * 4 + x * 4;
            p[0] = 0; p[1] = x < W / 2 ? 0xFF : 0; p[2] = 0x20; p[3] = x < W / 2 ? 0 : 0xFF;
        }
    board.fb_addr = FB; board.fb_width = W; board.fb_height = H; board.fb_stride = W * 4;
    board.fb_format = QEC64_FB_FMT_XRGB32; board.fb_enable = 1; board.fb_frames = 1;
    capture();
    const uint8_t *l = at(0.3, 0.5), *r = at(0.7, 0.5);
    assert(l[0] == 0xFF && l[1] == 0x20 && l[2] == 0);
    assert(r[0] == 0 && r[1] == 0x20 && r[2] == 0xFF);
    /* RGB565, big endian: green */
    for (int i = 0; i < W * H; i++) { ram[FB + i * 2] = 0x07; ram[FB + i * 2 + 1] = 0xE0; }
    board.fb_stride = W * 2; board.fb_format = QEC64_FB_FMT_RGB565; board.fb_frames = 2;
    capture();
    const uint8_t *g = at(0.5, 0.5);
    assert(g[0] == 0 && g[1] == 0xFF && g[2] == 0);
    puts("framebuffer XRGB32 and RGB565 shown");

    /* keys: A as macOS key code 0, the Windows key as Command, typed text and Return */
    while (next_event()) {}
    ev(in, EV_KEY, KEY_A, 1); ev(in, EV_KEY, KEY_A, 0);
    ev(in, EV_KEY, KEY_LEFTMETA, 1);
    ev(in, EV_KEY, KEY_LEFTSHIFT, 1); ev(in, EV_KEY, KEY_H, 1); ev(in, EV_KEY, KEY_H, 0); ev(in, EV_KEY, KEY_LEFTSHIFT, 0);
    ev(in, EV_KEY, KEY_I, 1); ev(in, EV_KEY, KEY_ENTER, 1); ev(in, EV_SYN, SYN_REPORT, 0);
    for (int i = 0; i < 4; i++) qec64_window_pump();
    assert(next_event() == (QEC64_INPUT_KEY_DOWN << 24 | 0x00u << 16));
    assert(next_event() == (QEC64_INPUT_KEY_UP << 24 | 0x00u << 16));
    assert(next_event() == (QEC64_INPUT_KEY_DOWN << 24 | 0x37u << 16));
    assert(typed_n == 4 && !memcmp(typed_text, "aHi\n", 4));
    puts("keys as macOS key codes, typed text with Shift");

    /* the mouse in framebuffer pixels */
    while (next_event()) {}
    ev(in, EV_REL, REL_X, 10); ev(in, EV_REL, REL_Y, 20); ev(in, EV_SYN, SYN_REPORT, 0);
    ev(in, EV_KEY, BTN_LEFT, 1); ev(in, EV_SYN, SYN_REPORT, 0);
    for (int i = 0; i < 4; i++) qec64_window_pump();
    uint32_t mv = next_event(), mv2 = next_event(), bt = next_event();
    assert(mv == (QEC64_INPUT_MOVE << 24 | 10u << 12 | 20u));
    assert(mv2 == mv);
    assert(bt == (QEC64_INPUT_BUTTON << 24 | 0u << 16 | 1u));
    puts("mouse in framebuffer pixels");

    qec64_window_close();
    close(in);
    remove(dev); remove(cap); rmdir(dir);
    free(shot);
    puts("PASS");
    return 0;
}
