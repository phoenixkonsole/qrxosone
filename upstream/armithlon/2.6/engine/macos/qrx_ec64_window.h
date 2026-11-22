#ifndef QRX_EC64_WINDOW_H
#define QRX_EC64_WINDOW_H
/* The Mac window of the 68100EC64 machine: the console (text out, typed text in) and the board's
 * framebuffer once the guest turns it on. Open, pump and close on the main thread; the console
 * text may come from any thread. */
#include "qrx_ec64_board.h"
#include <stddef.h>

typedef struct {
    void (*typed)(void *ctx, const char *text, size_t len);   /* text for the console input */
    void *ctx;
} qec64_window_input;

int qec64_window_open(const char *title, qec64_board *board, const qec64_window_input *input);
void qec64_window_console(const char *text, size_t len);
/* Runs the event loop for a moment; 0 once the window was closed */
int qec64_window_pump(void);
void qec64_window_close(void);
#endif
