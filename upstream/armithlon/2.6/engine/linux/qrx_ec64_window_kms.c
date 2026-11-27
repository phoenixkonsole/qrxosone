/* The 68100EC64 machine (QRX OS One 64) straight on the Linux console (linux/qrx_lio.c: KMS/DRM,
 * evdev, ALSA), without SDL; same interface as the Mac window (macos/qrx_ec64_window.h):
 *   - the console (an 8x8 font drawn at twice its size) until the guest turns the framebuffer on,
 *   - then the board's framebuffer, scaled to the screen by the display hardware,
 *   - keys as macOS virtual key codes (what the board's input register carries; the Windows keys
 *     are the Amiga keys), typed text (US layout) for the console, the mouse in framebuffer
 *     pixels, sound from the board's ring.
 * Only a new frame or new console text is drawn. QRX_TEST_KEYS=seconds:linuxkey,... and
 * QRX_EC64_CAPTURE=path (the screen at QRX_EC64_CAPTURE_AT seconds; _REPEAT) for checks. */
#include "../macos/qrx_ec64_window.h"
#include "qrx_font8x8.h"
#include "qrx_lio.h"
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static qec64_board *board;
static qec64_window_input input;
static int closed, showing_fb, console_dirty = 1;
static uint32_t shown_frames = UINT32_MAX;
static double t_open;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

/* ---- console */
#define CELL 16
#define COLS 112
#define ROWS 45
static char grid[ROWS][COLS];
static int cur_x, cur_y;
static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;
static char *pending;
static size_t pending_n, pending_cap;

void qec64_window_console(const char *text, size_t len)
{
    pthread_mutex_lock(&pending_lock);
    if (pending_n + len > pending_cap) {
        size_t cap = (pending_n + len) * 2 + 256;
        char *p = realloc(pending, cap);
        if (!p) { pthread_mutex_unlock(&pending_lock); return; }
        pending = p; pending_cap = cap;
    }
    memcpy(pending + pending_n, text, len);
    pending_n += len;
    pthread_mutex_unlock(&pending_lock);
}
static void scroll(void) { memmove(grid[0], grid[1], sizeof grid - sizeof grid[0]); memset(grid[ROWS - 1], ' ', COLS); cur_y = ROWS - 1; }
static void console_put(unsigned char ch)
{
    if (ch == '\r') return;
    if (ch == '\n') { cur_x = 0; if (++cur_y >= ROWS) scroll(); return; }
    if (ch == '\b') { if (cur_x) grid[cur_y][--cur_x] = ' '; return; }
    if (ch == '\t') { do console_put(' '); while (cur_x % 8); return; }
    if (cur_x >= COLS) { cur_x = 0; if (++cur_y >= ROWS) scroll(); }
    grid[cur_y][cur_x++] = (char)ch;
}
static void take_console_text(void)
{
    pthread_mutex_lock(&pending_lock);
    for (size_t i = 0; i < pending_n; i++) console_put((unsigned char)pending[i]);
    if (pending_n) console_dirty = 1;
    pending_n = 0;
    pthread_mutex_unlock(&pending_lock);
}
static void draw_console(void)
{
    unsigned pitch;
    uint32_t *px = qrx_lio_frame_begin(COLS * CELL, ROWS * CELL, &pitch);
    if (!px) return;
    const uint32_t bg = 0x141A29u, fg = 0xEBEBEBu, cursor = 0x5FA8FFu;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            unsigned char ch = (unsigned char)grid[r][c];
            const uint8_t *g = ch >= QRX_FONT_FIRST && ch <= QRX_FONT_LAST ? qrx_glyph8x8[ch - QRX_FONT_FIRST] : qrx_glyph8x8[0];
            for (int y = 0; y < CELL; y++) {
                uint32_t *o = px + (size_t)(r * CELL + y) * (COLS * CELL) + c * CELL;
                uint8_t bits = g[y / 2];
                for (int x = 0; x < CELL; x++) o[x] = (bits >> (x / 2) & 1) ? fg : bg;
            }
        }
    for (int y = CELL - 3; y < CELL; y++)
        for (int x = 0; x < CELL; x++) px[(size_t)(cur_y * CELL + y) * (COLS * CELL) + cur_x * CELL + x] = cursor;
    qrx_lio_frame_end(0, ROWS * CELL);
}

/* ---- the board's framebuffer: XRGB32 and RGB565, both big endian -> XRGB8888 */
static int show_framebuffer(void)
{
    const uint8_t *src;
    uint32_t w, h, stride, fmt;
    unsigned pitch;
    if (!board || !qec64_board_framebuffer(board, &src, &w, &h, &stride, &fmt)) {
        if (showing_fb) { showing_fb = 0; console_dirty = 1; }
        return 0;
    }
    uint32_t frames = board->fb_frames;
    if (showing_fb && frames == shown_frames) return 1;
    uint32_t *px = qrx_lio_frame_begin(w, h, &pitch);
    if (!px) return 1;
    showing_fb = 1;
    shown_frames = frames;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * stride;
        uint32_t *o = px + (size_t)y * w;
        if (fmt == QEC64_FB_FMT_XRGB32)
            for (uint32_t x = 0; x < w; x++) o[x] = (uint32_t)row[x * 4 + 1] << 16 | (uint32_t)row[x * 4 + 2] << 8 | row[x * 4 + 3];
        else
            for (uint32_t x = 0; x < w; x++) {
                uint32_t v = (uint32_t)row[x * 2] << 8 | row[x * 2 + 1], r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
                o[x] = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
            }
    }
    qrx_lio_frame_end(0, h);
    return 2;
}

/* ---- keys: Linux key codes -> macOS virtual key codes (Carbon kVK_*) */
static unsigned mac_key(unsigned k)
{
    static uint8_t map[256];
    static int init;
    if (!init) {
        static const uint16_t m[][2] = {
            {KEY_A,0x00},{KEY_S,0x01},{KEY_D,0x02},{KEY_F,0x03},{KEY_H,0x04},{KEY_G,0x05},{KEY_Z,0x06},{KEY_X,0x07},
            {KEY_C,0x08},{KEY_V,0x09},{KEY_102ND,0x0A},{KEY_B,0x0B},{KEY_Q,0x0C},{KEY_W,0x0D},{KEY_E,0x0E},{KEY_R,0x0F},
            {KEY_Y,0x10},{KEY_T,0x11},{KEY_1,0x12},{KEY_2,0x13},{KEY_3,0x14},{KEY_4,0x15},{KEY_6,0x16},{KEY_5,0x17},
            {KEY_EQUAL,0x18},{KEY_9,0x19},{KEY_7,0x1A},{KEY_MINUS,0x1B},{KEY_8,0x1C},{KEY_0,0x1D},{KEY_RIGHTBRACE,0x1E},
            {KEY_O,0x1F},{KEY_U,0x20},{KEY_LEFTBRACE,0x21},{KEY_I,0x22},{KEY_P,0x23},{KEY_ENTER,0x24},{KEY_L,0x25},
            {KEY_J,0x26},{KEY_APOSTROPHE,0x27},{KEY_K,0x28},{KEY_SEMICOLON,0x29},{KEY_BACKSLASH,0x2A},{KEY_COMMA,0x2B},
            {KEY_SLASH,0x2C},{KEY_N,0x2D},{KEY_M,0x2E},{KEY_DOT,0x2F},{KEY_TAB,0x30},{KEY_SPACE,0x31},{KEY_GRAVE,0x32},
            {KEY_BACKSPACE,0x33},{KEY_ESC,0x35},{KEY_RIGHTMETA,0x36},{KEY_LEFTMETA,0x37},{KEY_LEFTSHIFT,0x38},
            {KEY_CAPSLOCK,0x39},{KEY_LEFTALT,0x3A},{KEY_LEFTCTRL,0x3B},{KEY_RIGHTSHIFT,0x3C},{KEY_RIGHTALT,0x3D},
            {KEY_RIGHTCTRL,0x3E},{KEY_KPDOT,0x41},{KEY_KPASTERISK,0x43},{KEY_KPPLUS,0x45},{KEY_NUMLOCK,0x47},
            {KEY_KPSLASH,0x4B},{KEY_KPENTER,0x4C},{KEY_KPMINUS,0x4E},{KEY_KPEQUAL,0x51},{KEY_KP0,0x52},{KEY_KP1,0x53},
            {KEY_KP2,0x54},{KEY_KP3,0x55},{KEY_KP4,0x56},{KEY_KP5,0x57},{KEY_KP6,0x58},{KEY_KP7,0x59},{KEY_KP8,0x5B},
            {KEY_KP9,0x5C},{KEY_F5,0x60},{KEY_F6,0x61},{KEY_F7,0x62},{KEY_F3,0x63},{KEY_F8,0x64},{KEY_F9,0x65},
            {KEY_F11,0x67},{KEY_F10,0x6D},{KEY_F12,0x6F},{KEY_INSERT,0x72},{KEY_HOME,0x73},{KEY_PAGEUP,0x74},
            {KEY_DELETE,0x75},{KEY_F4,0x76},{KEY_END,0x77},{KEY_F2,0x78},{KEY_PAGEDOWN,0x79},{KEY_F1,0x7A},
            {KEY_LEFT,0x7B},{KEY_RIGHT,0x7C},{KEY_DOWN,0x7D},{KEY_UP,0x7E},
        };
        memset(map, 0xFF, sizeof map);
        for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) map[m[i][0]] = (uint8_t)m[i][1];
        init = 1;
    }
    return k < 256 ? map[k] : 0xFFu;
}
static uint8_t keys_down[128];
static void key_event(unsigned kind, unsigned code)
{
    if (code >= 128) return;
    if (kind == QEC64_INPUT_KEY_DOWN) keys_down[code] = 1;
    else if (!keys_down[code]) return;
    else keys_down[code] = 0;
    if (board) qec64_board_push_event(board, (kind << 24) | (code << 16));
}
static void key(unsigned linux_code, int down, int shift)
{
    unsigned mac = mac_key(linux_code);
    if (mac != 0xFF) key_event(down ? QEC64_INPUT_KEY_DOWN : QEC64_INPUT_KEY_UP, mac);
    if (!down || !input.typed) return;
    int ch = qrx_lio_key_char(linux_code, shift);
    if (ch) { char c = (char)ch; input.typed(input.ctx, &c, 1); }
}

static size_t audio_pull(void *ctx, int16_t *out, size_t n) { (void)ctx; return board ? qec64_board_pull_audio(board, out, n) : 0; }

int qec64_window_open(const char *title, qec64_board *b, const qec64_window_input *in)
{
    (void)title;
    board = b;
    if (in) input = *in;
    memset(grid, ' ', sizeof grid);
    if (!qrx_lio_open()) return 0;
    unsigned sw, sh;
    qrx_lio_screen(&sw, &sh);
    board->host_view = board->host_screen = sw << 16 | (sh & 0xFFFF);   /* the guest's display follows */
    qrx_lio_audio_open(audio_pull, NULL);
    t_open = now_s();
    return 1;
}

static void test_hooks(void)
{
    double t = now_s() - t_open;
    static const char *next;
    static int init;
    static struct { unsigned k; double at; } held[8];
    if (!init) { init = 1; next = getenv("QRX_TEST_KEYS"); }
    for (int i = 0; i < 8; i++) if (held[i].k && t >= held[i].at) { key(held[i].k, 0, 0); held[i].k = 0; }
    while (next && *next) {
        char *e;
        double at = strtod(next, &e);
        if (*e != ':' || t < at) break;
        unsigned k = (unsigned)strtoul(e + 1, &e, 0);
        key(k, 1, 0);
        for (int i = 0; i < 8; i++) if (!held[i].k) { held[i].k = k; held[i].at = t + 0.15; break; }
        next = *e == ',' ? e + 1 : "";
    }
    static int done;
    const char *path = getenv("QRX_EC64_CAPTURE"), *a = getenv("QRX_EC64_CAPTURE_AT");
    if (path && t >= (a ? atof(a) : 20.0)) {
        if (getenv("QRX_EC64_CAPTURE_REPEAT")) { FILE *f = fopen(path, "rb"); if (f) fclose(f); else done = 0; }
        if (!done) {
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%s.part", path);
            if (qrx_lio_capture(tmp)) rename(tmp, path);
            done = 1;
        }
    }
}

int qec64_window_pump(void)
{
    qrx_lio_event e;
    take_console_text();
    int drawn = show_framebuffer() == 2;
    if (!showing_fb && console_dirty) { draw_console(); console_dirty = 0; drawn = 1; }
    test_hooks();
    for (int wait = drawn ? 0 : 16; qrx_lio_poll(&e, wait); wait = 0) {
        switch (e.type) {
        case QRX_LIO_KEY: key(e.code, e.value, e.shift); break;
        case QRX_LIO_MOVE:
            if (showing_fb && board) qec64_board_push_event(board, (QEC64_INPUT_MOVE << 24) | ((uint32_t)e.x & 0xFFF) << 12 | ((uint32_t)e.y & 0xFFF));
            break;
        case QRX_LIO_BUTTON:
            if (showing_fb && board) {
                int x, y;
                qrx_lio_pointer_pos(&x, &y);
                qec64_board_push_event(board, (QEC64_INPUT_MOVE << 24) | ((uint32_t)x & 0xFFF) << 12 | ((uint32_t)y & 0xFFF));
                qec64_board_push_event(board, (QEC64_INPUT_BUTTON << 24) | (e.code << 16) | (e.value ? 1u : 0u));
            }
            break;
        case QRX_LIO_WHEEL:
            if (showing_fb && board) qec64_board_push_event(board, (QEC64_INPUT_WHEEL << 24) | ((uint32_t)e.value & 0xFFFF));
            break;
        case QRX_LIO_QUIT: closed = 1; break;
        default: break;
        }
    }
    return !closed;
}

void qec64_window_close(void)
{
    for (unsigned k = 0; k < 128; k++) if (keys_down[k]) key_event(QEC64_INPUT_KEY_UP, k);
    qrx_lio_close();
    board = NULL;
}
