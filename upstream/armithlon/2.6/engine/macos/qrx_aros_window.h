#ifndef QRX_AROS_WINDOW_H
#define QRX_AROS_WINDOW_H
#include "qrxvirt.h"
int qrx_aros_window_open(QRXVM *vm);
int qrx_aros_window_pump(void);
void qrx_aros_window_frame(const unsigned char *rgb,unsigned w,unsigned h);
void qrx_aros_window_close(void);
#endif
