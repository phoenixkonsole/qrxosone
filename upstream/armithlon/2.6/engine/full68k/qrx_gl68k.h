#ifndef QRX_GL68K_H
#define QRX_GL68K_H
/* Door service 8: OpenGL 2.1 for native 68k programs (tools/qrxgl68k). q is the 128-byte request
 * block in guest RAM. */
#include <stdint.h>
void qrx_gl68k_service(uint8_t *ram, uint64_t ram_size, uint8_t *q);
void qrx_gl68k_report(void);
#endif
