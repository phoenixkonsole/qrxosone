#ifndef QRX_DEMO_H
#define QRX_DEMO_H
#include "qrxmedia.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct QRXDemo QRXDemo;
/* Native host diagnostic firmware (not an AROS desktop). A real subset of
 * 68k machine instructions issues PRESENT and reads keyboard queue length. */
QRXDemo *qrx_demo_create(void (*present)(void *,const uint8_t *,uint32_t,uint32_t,uint32_t), void *ctx);
void qrx_demo_destroy(QRXDemo *d);
bool qrx_demo_frame(QRXDemo *d);
QRXMediaDevice *qrx_demo_media(QRXDemo *d);
uint32_t qrx_demo_frames(QRXDemo *d);
uint32_t qrx_demo_last_input(QRXDemo *d);
#endif
