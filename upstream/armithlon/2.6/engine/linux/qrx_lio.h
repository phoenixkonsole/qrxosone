#ifndef QRX_LIO_H
#define QRX_LIO_H
/* QRX Linux I/O: the console's hardware without SDL or a display server, for the Raspberry Pi
 * appliance (linux/qrx_lio.c).
 *   Display  DRM/KMS through the kernel's ioctls (no libdrm): the guest frame is a dumb buffer on
 *            a plane that the display hardware scales to the screen (aspect kept, letterboxed),
 *            flipped at vertical blank; without a scaling plane the frame is scaled on the CPU.
 *            A hardware cursor shows the pointer.
 *   Input    evdev: keyboards, mice, touch screens and tablets, read directly and grabbed;
 *            devices plugged in later are found through inotify on /dev/input.
 *   Console  the virtual terminal goes to graphics mode; Ctrl+Alt+F1..F6 switch terminals (the
 *            display and input are handed back and taken again when ours is active).
 *   Sound    ALSA (alsa-lib, which also does the IEC958 framing HDMI needs on the Pi), on its own
 *            thread pulling from a callback.
 * QRX_LIO_TEST=dir runs without hardware for tests: the display is a memory buffer of
 * QRX_LIO_TEST_SIZE (default 1280x720), input comes from the files event* in dir (FIFOs or
 * plain files of struct input_event), and qrx_lio_capture() writes what the screen shows. */
#include <stddef.h>
#include <stdint.h>

/* Input events, after the devices' own coordinates are turned into the frame's pixels */
enum { QRX_LIO_KEY = 1, QRX_LIO_MOVE, QRX_LIO_BUTTON, QRX_LIO_QUIT };
typedef struct {
    int type;
    unsigned code;      /* KEY: Linux KEY_* code; BUTTON: 0 left, 1 right, 2 middle */
    int value;          /* KEY/BUTTON: 1 down, 0 up (key repeats are not reported) */
    int x, y;           /* MOVE: the pointer in frame pixels */
    int shift;          /* KEY: Shift held (for typed text) */
} qrx_lio_event;

int qrx_lio_open(void);
void qrx_lio_close(void);
/* The screen's mode in pixels */
void qrx_lio_screen(unsigned *w, unsigned *h);
/* The frame: w x h XRGB8888 pixels (bits 23-0 R G B), shown scaled to the screen. begin returns
 * the buffer to write and its pitch in bytes; it holds the last frame, so only changed rows need
 * writing. end shows it at the next vertical blank (rows y0..y1-1 changed); it may wait for it. */
uint32_t *qrx_lio_frame_begin(unsigned w, unsigned h, unsigned *pitch);
void qrx_lio_frame_end(unsigned y0, unsigned y1);
/* Next input event, waiting up to timeout_ms; 0 when none came */
int qrx_lio_poll(qrx_lio_event *e, int timeout_ms);
/* The pointer: -1 hidden; 0 arrow, 1 QRX neon, 2 Amiga style, 3 crosshair, 4 large white (the
 * desktop's choices); at frame pixel x, y */
void qrx_lio_pointer(int shape, int x, int y);
/* The pointer position the input moved to, in frame pixels */
void qrx_lio_pointer_pos(int *x, int *y);
/* PPM of what the screen shows (tests) */
int qrx_lio_capture(const char *path);
/* Sound: 48 kHz stereo 16-bit; pull fills up to n frames and returns how many */
typedef size_t (*qrx_lio_pull)(void *ctx, int16_t *out, size_t n);
int qrx_lio_audio_open(qrx_lio_pull pull, void *ctx);
void qrx_lio_audio_close(void);
/* US layout: the character a key types, 0 for none */
int qrx_lio_key_char(unsigned code, int shift);
#endif
