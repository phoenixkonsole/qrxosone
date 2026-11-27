#ifndef QRX_GL3_H
#define QRX_GL3_H
/* HOST_CALL service 13: OpenGL 3.3 core for programs of QRX OS One 64 (tools/qrxgl68k/guest/qgl3.c).
 * q is the request block in guest RAM: [3] 1, [4..5] the stream's address, [6] its size in bytes,
 * [7] the context the stream draws with; the last call's result comes back in [8..9]. */
#include <stdbool.h>
#include <stdint.h>
bool qrx_gl3_available(void);
void qrx_gl3_service(uint8_t *ram, uint64_t ram_size, uint8_t *q);
void qrx_gl3_report(void);
#endif
