/* The Linux window of the 68100EC64 machine (QRX OS One 64), the SDL2 counterpart of
 * macos/qrx_ec64_window.m and the same interface (macos/qrx_ec64_window.h):
 *   - the console (text out, typed text in) until the guest turns the framebuffer on,
 *   - then the board's framebuffer, scaled to the window with its aspect kept,
 *   - keys as macOS virtual key codes (what the board's input register carries and the guest
 *     maps to Amiga raw keys), the mouse in framebuffer pixels, sound from the board's ring.
 * On the Raspberry Pi console SDL runs on KMS/DRM; QRX_FULLSCREEN=1 opens it full screen.
 * Unattended checks: QRX_TEST_KEYS=seconds:sdlscancode,... and QRX_EC64_CAPTURE=path (PPM of
 * what the window shows, at QRX_EC64_CAPTURE_AT seconds, default 20; QRX_EC64_CAPTURE_REPEAT). */
#include "../macos/qrx_ec64_window.h"
#include "qrx_font8x8.h"
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static qec64_board *board;
static qec64_window_input input;
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int tex_w, tex_h;
static uint32_t tex_format;
static SDL_AudioDeviceID audio;
static int closed, showing_fb, redraw = 1;
static uint32_t shown_frames = UINT32_MAX;
static uint16_t *swap565;                      /* RGB565 big endian -> host order */
static size_t swap_cap;
static double t_open;

/* ---- console: a grid of 8x8 glyphs drawn at twice their size */
#define CELL 16
#define COLS 112
#define ROWS 45
static char grid[ROWS][COLS];
static int cur_x, cur_y, console_dirty = 1;
static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;
static char *pending;
static size_t pending_n, pending_cap;
static uint32_t console_px[ROWS * CELL][COLS * CELL];

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

static void scroll(void)
{
    memmove(grid[0], grid[1], sizeof grid - sizeof grid[0]);
    memset(grid[ROWS - 1], ' ', COLS);
    cur_y = ROWS - 1;
}
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
    const uint32_t bg = 0xFF141A29u, fg = 0xFFEBEBEBu, cursor = 0xFF5FA8FFu;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            unsigned char ch = (unsigned char)grid[r][c];
            const uint8_t *g = ch >= QRX_FONT_FIRST && ch <= QRX_FONT_LAST ? qrx_glyph8x8[ch - QRX_FONT_FIRST] : qrx_glyph8x8[0];
            for (int y = 0; y < CELL; y++) {
                uint32_t *o = &console_px[r * CELL + y][c * CELL];
                uint8_t bits = g[y / 2];
                for (int x = 0; x < CELL; x++) o[x] = (bits >> (x / 2) & 1) ? fg : bg;
            }
        }
    for (int y = CELL - 3; y < CELL; y++)
        for (int x = 0; x < CELL; x++) console_px[cur_y * CELL + y][cur_x * CELL + x] = cursor;
}

/* ---- textures */
static int texture_for(int w, int h, uint32_t format)
{
    if (texture && tex_w == w && tex_h == h && tex_format == format) return 1;
    if (texture) SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, format, SDL_TEXTUREACCESS_STREAMING, w, h);
    tex_w = w; tex_h = h; tex_format = format;
    SDL_RenderSetLogicalSize(renderer, w, h);      /* letterbox; mouse events in these pixels */
    return texture != NULL;
}

/* The board's XRGB32 is big endian: bytes x R G B, which SDL calls BGRX8888 on a little endian
 * host, so those frames go to the texture as they are. RGB565 is big endian too and is swapped. */
/* 0: the console is shown; 1: the framebuffer, unchanged; 2: the framebuffer, a new frame */
static int show_framebuffer(void)
{
    const uint8_t *px;
    uint32_t w, h, stride, fmt;
    if (!board || !qec64_board_framebuffer(board, &px, &w, &h, &stride, &fmt)) {
        if (showing_fb) { showing_fb = 0; console_dirty = 1; SDL_ShowCursor(SDL_ENABLE); }
        return 0;
    }
    uint32_t frames = board->fb_frames;
    if (showing_fb && frames == shown_frames) return 1;
    redraw = 1;
    if (!showing_fb && !getenv("QRX_HOST_CURSOR")) SDL_ShowCursor(SDL_DISABLE);   /* the guest draws its own */
    showing_fb = 1;
    shown_frames = frames;
    if (fmt == QEC64_FB_FMT_XRGB32) {
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
        if (!texture_for((int)w, (int)h, SDL_PIXELFORMAT_BGRX8888)) return 1;
#else
        if (!texture_for((int)w, (int)h, SDL_PIXELFORMAT_RGB888)) return 1;
#endif
        SDL_UpdateTexture(texture, NULL, px, (int)stride);
        return 2;
    } else {
        if (!texture_for((int)w, (int)h, SDL_PIXELFORMAT_RGB565)) return 1;
        size_t need = (size_t)w * h;
        if (need > swap_cap) { free(swap565); swap565 = malloc(need * 2); swap_cap = swap565 ? need : 0; }
        if (!swap565) return 1;
        for (uint32_t y = 0; y < h; y++) {
            const uint16_t *in = (const uint16_t *)(const void *)(px + (size_t)y * stride);
            uint16_t *out = swap565 + (size_t)y * w;
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
            for (uint32_t x = 0; x < w; x++) out[x] = (uint16_t)__builtin_bswap16(in[x]);
#else
            memcpy(out, in, (size_t)w * 2);
#endif
        }
        SDL_UpdateTexture(texture, NULL, swap565, (int)w * 2);
    }
    return 2;
}

/* ---- keys: SDL scancodes -> macOS virtual key codes (Carbon kVK_*) */
static uint8_t mac_of[SDL_NUM_SCANCODES];
static void keymap_init(void)
{
    static const struct { uint16_t sc; uint8_t mac; } m[] = {
        {SDL_SCANCODE_A,0x00},{SDL_SCANCODE_S,0x01},{SDL_SCANCODE_D,0x02},{SDL_SCANCODE_F,0x03},{SDL_SCANCODE_H,0x04},
        {SDL_SCANCODE_G,0x05},{SDL_SCANCODE_Z,0x06},{SDL_SCANCODE_X,0x07},{SDL_SCANCODE_C,0x08},{SDL_SCANCODE_V,0x09},
        {SDL_SCANCODE_NONUSBACKSLASH,0x0A},{SDL_SCANCODE_B,0x0B},{SDL_SCANCODE_Q,0x0C},{SDL_SCANCODE_W,0x0D},
        {SDL_SCANCODE_E,0x0E},{SDL_SCANCODE_R,0x0F},{SDL_SCANCODE_Y,0x10},{SDL_SCANCODE_T,0x11},{SDL_SCANCODE_1,0x12},
        {SDL_SCANCODE_2,0x13},{SDL_SCANCODE_3,0x14},{SDL_SCANCODE_4,0x15},{SDL_SCANCODE_6,0x16},{SDL_SCANCODE_5,0x17},
        {SDL_SCANCODE_EQUALS,0x18},{SDL_SCANCODE_9,0x19},{SDL_SCANCODE_7,0x1A},{SDL_SCANCODE_MINUS,0x1B},
        {SDL_SCANCODE_8,0x1C},{SDL_SCANCODE_0,0x1D},{SDL_SCANCODE_RIGHTBRACKET,0x1E},{SDL_SCANCODE_O,0x1F},
        {SDL_SCANCODE_U,0x20},{SDL_SCANCODE_LEFTBRACKET,0x21},{SDL_SCANCODE_I,0x22},{SDL_SCANCODE_P,0x23},
        {SDL_SCANCODE_RETURN,0x24},{SDL_SCANCODE_L,0x25},{SDL_SCANCODE_J,0x26},{SDL_SCANCODE_APOSTROPHE,0x27},
        {SDL_SCANCODE_K,0x28},{SDL_SCANCODE_SEMICOLON,0x29},{SDL_SCANCODE_BACKSLASH,0x2A},{SDL_SCANCODE_NONUSHASH,0x2A},
        {SDL_SCANCODE_COMMA,0x2B},{SDL_SCANCODE_SLASH,0x2C},{SDL_SCANCODE_N,0x2D},{SDL_SCANCODE_M,0x2E},
        {SDL_SCANCODE_PERIOD,0x2F},{SDL_SCANCODE_TAB,0x30},{SDL_SCANCODE_SPACE,0x31},{SDL_SCANCODE_GRAVE,0x32},
        {SDL_SCANCODE_BACKSPACE,0x33},{SDL_SCANCODE_ESCAPE,0x35},
        /* the Windows keys are the Amiga keys, as in the 32-bit window */
        {SDL_SCANCODE_RGUI,0x36},{SDL_SCANCODE_LGUI,0x37},{SDL_SCANCODE_LSHIFT,0x38},{SDL_SCANCODE_CAPSLOCK,0x39},
        {SDL_SCANCODE_LALT,0x3A},{SDL_SCANCODE_LCTRL,0x3B},{SDL_SCANCODE_RSHIFT,0x3C},{SDL_SCANCODE_RALT,0x3D},
        {SDL_SCANCODE_RCTRL,0x3E},{SDL_SCANCODE_KP_PERIOD,0x41},{SDL_SCANCODE_KP_MULTIPLY,0x43},
        {SDL_SCANCODE_KP_PLUS,0x45},{SDL_SCANCODE_NUMLOCKCLEAR,0x47},{SDL_SCANCODE_KP_DIVIDE,0x4B},
        {SDL_SCANCODE_KP_ENTER,0x4C},{SDL_SCANCODE_KP_MINUS,0x4E},{SDL_SCANCODE_KP_EQUALS,0x51},
        {SDL_SCANCODE_KP_0,0x52},{SDL_SCANCODE_KP_1,0x53},{SDL_SCANCODE_KP_2,0x54},{SDL_SCANCODE_KP_3,0x55},
        {SDL_SCANCODE_KP_4,0x56},{SDL_SCANCODE_KP_5,0x57},{SDL_SCANCODE_KP_6,0x58},{SDL_SCANCODE_KP_7,0x59},
        {SDL_SCANCODE_KP_8,0x5B},{SDL_SCANCODE_KP_9,0x5C},{SDL_SCANCODE_F5,0x60},{SDL_SCANCODE_F6,0x61},
        {SDL_SCANCODE_F7,0x62},{SDL_SCANCODE_F3,0x63},{SDL_SCANCODE_F8,0x64},{SDL_SCANCODE_F9,0x65},
        {SDL_SCANCODE_F11,0x67},{SDL_SCANCODE_F13,0x69},{SDL_SCANCODE_F14,0x6B},{SDL_SCANCODE_F10,0x6D},
        {SDL_SCANCODE_F12,0x6F},{SDL_SCANCODE_F15,0x71},{SDL_SCANCODE_INSERT,0x72},{SDL_SCANCODE_HOME,0x73},
        {SDL_SCANCODE_PAGEUP,0x74},{SDL_SCANCODE_DELETE,0x75},{SDL_SCANCODE_F4,0x76},{SDL_SCANCODE_END,0x77},
        {SDL_SCANCODE_F2,0x78},{SDL_SCANCODE_PAGEDOWN,0x79},{SDL_SCANCODE_F1,0x7A},{SDL_SCANCODE_LEFT,0x7B},
        {SDL_SCANCODE_RIGHT,0x7C},{SDL_SCANCODE_DOWN,0x7D},{SDL_SCANCODE_UP,0x7E},
    };
    memset(mac_of, 0xFF, sizeof mac_of);
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) mac_of[m[i].sc] = m[i].mac;
}
static uint8_t keys_down[128];
static void key_event(unsigned kind, unsigned code)
{
    if (code >= 128) return;
    if (kind == QEC64_INPUT_KEY_DOWN) keys_down[code] = 1;
    else if (!keys_down[code]) return;             /* an up without a down */
    else keys_down[code] = 0;
    if (board) qec64_board_push_event(board, (kind << 24) | (code << 16));
}
static void release_all_keys(void)
{
    for (unsigned k = 0; k < 128; k++) if (keys_down[k]) key_event(QEC64_INPUT_KEY_UP, k);
}
static void key(SDL_Scancode sc, int down, int repeat)
{
    if (sc < SDL_NUM_SCANCODES && mac_of[sc] != 0xFF && !repeat)
        key_event(down ? QEC64_INPUT_KEY_DOWN : QEC64_INPUT_KEY_UP, mac_of[sc]);
    if (!down || !input.typed) return;
    if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) input.typed(input.ctx, "\n", 1);
    else if (sc == SDL_SCANCODE_BACKSPACE) input.typed(input.ctx, "\b", 1);
}

/* ---- mouse: with a logical size set, SDL reports framebuffer pixels */
static void mouse_move(int x, int y)
{
    if (!board || !showing_fb) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= tex_w) x = tex_w - 1;
    if (y >= tex_h) y = tex_h - 1;
    qec64_board_push_event(board, (QEC64_INPUT_MOVE << 24) | ((uint32_t)x & 0xFFF) << 12 | ((uint32_t)y & 0xFFF));
}
static void mouse_button(int sdl_button, int down, int x, int y)
{
    unsigned b = sdl_button == SDL_BUTTON_LEFT ? 0 : sdl_button == SDL_BUTTON_RIGHT ? 1 : sdl_button == SDL_BUTTON_MIDDLE ? 2 : 9;
    if (b > 2 || !board || !showing_fb) return;
    mouse_move(x, y);
    qec64_board_push_event(board, (QEC64_INPUT_BUTTON << 24) | (b << 16) | (down ? 1u : 0u));
}

/* ---- the size the guest's display follows: the window's inside and the screen, in points */
static void report_size(void)
{
    if (!board || !window) return;
    int w, h;
    SDL_DisplayMode dm;
    SDL_GetWindowSize(window, &w, &h);
    board->host_view = (uint32_t)w << 16 | ((uint32_t)h & 0xFFFF);
    if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(window), &dm) == 0)
        board->host_screen = (uint32_t)dm.w << 16 | ((uint32_t)dm.h & 0xFFFF);
    else
        board->host_screen = board->host_view;
}

/* ---- sound: the board's ring at 48 kHz */
static void audio_callback(void *opaque, Uint8 *stream, int len)
{
    (void)opaque;
    size_t frames = (size_t)len / 4u;
    qec64_board *b = board;
    size_t got = b ? qec64_board_pull_audio(b, (int16_t *)(void *)stream, frames) : 0;
    if (got < frames) memset(stream + got * 4u, 0, (frames - got) * 4u);
}

static double now_s(void) { return SDL_GetTicks64() / 1000.0; }

/* QRX_TEST_KEYS=seconds:scancode[:hold],...  QRX_EC64_CAPTURE=path at QRX_EC64_CAPTURE_AT */
static void test_hooks(void)
{
    double t = now_s() - t_open;
    static const char *next;
    static int init;
    static struct { int sc; double at; } held[8];
    if (!init) { init = 1; next = getenv("QRX_TEST_KEYS"); }
    for (int i = 0; i < 8; i++) if (held[i].sc && t >= held[i].at) { key((SDL_Scancode)held[i].sc, 0, 0); held[i].sc = 0; }
    while (next && *next) {
        char *e;
        double at = strtod(next, &e);
        if (*e != ':' || t < at) break;
        int sc = (int)strtol(e + 1, &e, 0);
        double hold = 0.15;
        if (*e == ':') hold = strtod(e + 1, &e);
        key((SDL_Scancode)sc, 1, 0);
        for (int i = 0; i < 8; i++) if (!held[i].sc) { held[i].sc = sc; held[i].at = t + hold; break; }
        next = *e == ',' ? e + 1 : "";
    }
}

/* QRX_EC64_CAPTURE: the frame just drawn, once (QRX_EC64_CAPTURE_REPEAT: whenever the file is gone) */
static void capture_hook(void)
{
    static int captured;
    const char *path = getenv("QRX_EC64_CAPTURE");
    if (!path) return;
    if (getenv("QRX_EC64_CAPTURE_REPEAT")) { FILE *f = fopen(path, "rb"); if (f) { fclose(f); return; } }
    else if (captured) return;
    const char *a = getenv("QRX_EC64_CAPTURE_AT");
    if (now_s() - t_open < (a ? atof(a) : 20.0)) return;
    captured = 1;
    int w, h;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    uint8_t *rgb = malloc((size_t)w * h * 3);
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    /* the whole output, not the letterboxed viewport the logical size sets */
    int lw, lh;
    SDL_RenderGetLogicalSize(renderer, &lw, &lh);
    SDL_RenderSetLogicalSize(renderer, 0, 0);
    SDL_RenderSetViewport(renderer, NULL);
    SDL_RenderSetScale(renderer, 1.0f, 1.0f);
    int ok = rgb && SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGB24, rgb, w * 3) == 0;
    if (lw && lh) SDL_RenderSetLogicalSize(renderer, lw, lh);
    if (ok) {
        FILE *f = fopen(tmp, "wb");
        if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(rgb, 1, (size_t)w * h * 3, f); fclose(f); rename(tmp, path); }
        if (!getenv("QRX_EC64_CAPTURE_REPEAT")) fprintf(stderr, "[EC64] window %d x %d captured to %s\n", w, h, path);
    }
    free(rgb);
}

int qec64_window_open(const char *title, qec64_board *b, const qec64_window_input *in)
{
    board = b;
    if (in) input = *in;
    keymap_init();
    memset(grid, ' ', sizeof grid);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[EC64] SDL: %s\n", SDL_GetError());
        return 0;
    }
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (getenv("QRX_FULLSCREEN")) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    window = SDL_CreateWindow(title ? title : "68100EC64", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              COLS * CELL * 5 / 8, ROWS * CELL * 5 / 8, flags);
    if (!window) { fprintf(stderr, "[EC64] SDL window: %s\n", SDL_GetError()); return 0; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) { fprintf(stderr, "[EC64] SDL renderer: %s\n", SDL_GetError()); return 0; }
    SDL_StartTextInput();
    SDL_AudioSpec want = {0}, have;
    want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    want.callback = audio_callback;
    audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);
    else fprintf(stderr, "[EC64] no audio output: %s\n", SDL_GetError());
    report_size();
    t_open = now_s();
    return 1;
}

static void handle_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT: closed = 1; break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) { report_size(); redraw = 1; }
        else if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) release_all_keys();
        else if (e->window.event == SDL_WINDOWEVENT_EXPOSED) { console_dirty = 1; redraw = 1; }
        break;
    case SDL_KEYDOWN: case SDL_KEYUP:
        key(e->key.keysym.scancode, e->type == SDL_KEYDOWN, e->key.repeat);
        break;
    case SDL_TEXTINPUT:
        if (input.typed) input.typed(input.ctx, e->text.text, strlen(e->text.text));
        break;
    case SDL_MOUSEMOTION: mouse_move(e->motion.x, e->motion.y); break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        mouse_button(e->button.button, e->type == SDL_MOUSEBUTTONDOWN, e->button.x, e->button.y);
        break;
    default: break;
    }
}

/* Waits up to about a frame for input (SDL's poll sentinel, 0x7F00 and up, does not end the
 * wait), takes all that came, and draws only when the console or the guest's frame changed:
 * an idle screen costs neither a copy nor a present. */
int qec64_window_pump(void)
{
    SDL_Event e;
    Uint32 end = SDL_GetTicks() + 16;
    for (;;) {
        int left = (int)(end - SDL_GetTicks());
        if (left <= 0 || !SDL_WaitEventTimeout(&e, left)) break;
        if (e.type >= 0x7F00u) continue;
        handle_event(&e);
        break;
    }
    while (SDL_PollEvent(&e)) if (e.type < 0x7F00u) handle_event(&e);
    test_hooks();
    take_console_text();
    int fb = show_framebuffer();
    if (!fb && console_dirty) {
        draw_console();
        if (texture_for(COLS * CELL, ROWS * CELL, SDL_PIXELFORMAT_ARGB8888))
            SDL_UpdateTexture(texture, NULL, console_px, COLS * CELL * 4);
        console_dirty = 0;
        redraw = 1;
    }
    if (redraw || getenv("QRX_EC64_CAPTURE")) {
        redraw = 0;
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (texture) SDL_RenderCopy(renderer, texture, NULL, NULL);
        capture_hook();
        SDL_RenderPresent(renderer);
    }
    return !closed;
}

void qec64_window_close(void)
{
    if (audio) { SDL_CloseAudioDevice(audio); audio = 0; }
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    texture = NULL; renderer = NULL; window = NULL;
    free(swap565); swap565 = NULL; swap_cap = 0;
    board = NULL;
    SDL_Quit();
}
