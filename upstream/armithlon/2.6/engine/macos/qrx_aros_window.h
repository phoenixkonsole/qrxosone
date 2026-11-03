#ifndef QRX_AROS_WINDOW_H
#define QRX_AROS_WINDOW_H
#include "qrxvirt.h"
int qrx_aros_window_open(QRXVM *vm);
int qrx_aros_window_pump(void);
/* Called by the guest CPU thread; the main thread reads it at display rate. */
void qrx_aros_window_publish(uint32_t fb,uint32_t stride,uint32_t w,uint32_t h,uint32_t fmt,const uint32_t *clut);
void qrx_aros_window_close(void);
#endif
