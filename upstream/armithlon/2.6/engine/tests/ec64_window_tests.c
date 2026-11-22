/* The Linux window of the 68100EC64 machine (linux/qrx_ec64_window_sdl.c) with no display: SDL's
 * offscreen driver. The console shows text; the framebuffer replaces it in both formats with the
 * right colours (checked on a capture of the window); keys reach the board as macOS key codes,
 * typed text the console input, the mouse the board in framebuffer pixels. */
#include "../macos/qrx_ec64_window.h"
#include <SDL2/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
/* the window as captured: pixel at a fraction of its size */
static int cw, ch;
static uint8_t *shot;
static void capture(const char *path)
{
    setenv("QRX_EC64_CAPTURE", path, 1);
    setenv("QRX_EC64_CAPTURE_REPEAT", "1", 1);
    setenv("QRX_EC64_CAPTURE_AT", "0", 1);
    remove(path);
    for (int i = 0; i < 200; i++) {
        qec64_window_pump();
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        char head[8];
        int max;
        assert(fscanf(f, "%7s %d %d %d", head, &cw, &ch, &max) == 4);
        fgetc(f);
        free(shot);
        shot = malloc((size_t)cw * ch * 3);
        assert(fread(shot, 1, (size_t)cw * ch * 3, f) == (size_t)cw * ch * 3);
        fclose(f);
        remove(path);
        return;
    }
    assert(!"no capture");
}
static const uint8_t *at(double fx, double fy) { return shot + ((size_t)(fy * ch) * cw + (size_t)(fx * cw)) * 3; }

int main(void)
{
    char path[] = "/tmp/qrx-ec64-window-XXXXXX";
    close(mkstemp(path));
    qec64_board_init(&board, ram, sizeof ram);
    qec64_window_input in = { typed, NULL };
    assert(qec64_window_open("ec64 test", &board, &in));
    assert(board.host_view != 0 && board.host_screen != 0);

    /* console text: the captured console has light glyph pixels on the dark background */
    qec64_window_console("QRX OS One 64\nconsole\n", 22);
    capture(path);
    int light = 0;
    for (int i = 0; i < cw * ch; i++) if (shot[i * 3] > 0xC0 && shot[i * 3 + 1] > 0xC0) light++;
    assert(light > 50);

    /* XRGB32, big endian: left half red, right half blue */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = ram + FB + (size_t)y * W * 4 + x * 4;
            p[0] = 0; p[1] = x < W / 2 ? 0xFF : 0; p[2] = 0x20; p[3] = x < W / 2 ? 0 : 0xFF;
        }
    board.fb_addr = FB; board.fb_width = W; board.fb_height = H; board.fb_stride = W * 4;
    board.fb_format = QEC64_FB_FMT_XRGB32; board.fb_enable = 1; board.fb_frames = 1;
    capture(path);
    const uint8_t *l = at(0.25, 0.5), *r = at(0.75, 0.5);
    fprintf(stderr, "xrgb32 left %02x%02x%02x right %02x%02x%02x\n", l[0], l[1], l[2], r[0], r[1], r[2]);
    assert(l[0] > 0xE0 && l[1] > 0x10 && l[1] < 0x30 && l[2] < 0x20);
    assert(r[0] < 0x20 && r[1] > 0x10 && r[1] < 0x30 && r[2] > 0xE0);

    /* RGB565, big endian: green */
    for (int i = 0; i < W * H; i++) { ram[FB + i * 2] = 0x07; ram[FB + i * 2 + 1] = 0xE0; }
    board.fb_stride = W * 2; board.fb_format = QEC64_FB_FMT_RGB565; board.fb_frames = 2;
    capture(path);
    const uint8_t *g = at(0.5, 0.5);
    fprintf(stderr, "rgb565 %02x%02x%02x\n", g[0], g[1], g[2]);
    assert(g[0] < 0x10 && g[1] > 0xF0 && g[2] < 0x10);

    /* keys: A down and up as macOS key code 0, typed text and Return to the console */
    while (next_event()) {}
    SDL_Event e = {0};
    e.type = SDL_KEYDOWN; e.key.keysym.scancode = SDL_SCANCODE_A; SDL_PushEvent(&e);
    e.type = SDL_KEYUP; SDL_PushEvent(&e);
    e.type = SDL_KEYDOWN; e.key.keysym.scancode = SDL_SCANCODE_LGUI; SDL_PushEvent(&e);
    SDL_Event t = {0};
    t.type = SDL_TEXTINPUT; strcpy(t.text.text, "hi"); SDL_PushEvent(&t);
    e.type = SDL_KEYDOWN; e.key.keysym.scancode = SDL_SCANCODE_RETURN; SDL_PushEvent(&e);
    for (int i = 0; i < 5; i++) qec64_window_pump();
    uint32_t ev[8] = {0};
    for (int i = 0; i < 8; i++) ev[i] = next_event();
    assert(ev[0] == (QEC64_INPUT_KEY_DOWN << 24 | 0x00u << 16));
    assert(ev[1] == (QEC64_INPUT_KEY_UP << 24 | 0x00u << 16));
    assert(ev[2] == (QEC64_INPUT_KEY_DOWN << 24 | 0x37u << 16));     /* Windows key = Command = Amiga */
    assert(ev[3] == (QEC64_INPUT_KEY_DOWN << 24 | 0x24u << 16));     /* Return */
    assert(typed_n == 3 && !memcmp(typed_text, "hi\n", 3));

    /* the mouse: SDL reports framebuffer pixels (logical size) */
    while (next_event()) {}
    SDL_Event m = {0};
    m.type = SDL_MOUSEMOTION; m.motion.x = 10; m.motion.y = 20; SDL_PushEvent(&m);
    m.type = SDL_MOUSEBUTTONDOWN; m.button.button = SDL_BUTTON_RIGHT; m.button.x = 11; m.button.y = 21; SDL_PushEvent(&m);
    for (int i = 0; i < 5; i++) qec64_window_pump();
    uint32_t mv = next_event(), mv2 = next_event(), bt = next_event();
    assert(mv == (QEC64_INPUT_MOVE << 24 | 10u << 12 | 20u));
    assert(mv2 == (QEC64_INPUT_MOVE << 24 | 11u << 12 | 21u));
    assert(bt == (QEC64_INPUT_BUTTON << 24 | 1u << 16 | 1u));

    /* sound: the board's ring is drained by the audio device */
    qec64_window_close();
    free(shot);
    puts("ec64 window: console, XRGB32, RGB565, keys, text, mouse ok");
    return 0;
}
