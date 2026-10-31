/* In-process X11 display :0 for the Linux personality. No host network listener. */
#ifndef QRX_X11_H
#define QRX_X11_H
#include <stddef.h>
#include <stdint.h>
typedef struct qxx qxx;
qxx *qxx_new(void);
void qxx_free(qxx *x);
int qxx_write(qxx *x, const void *data, size_t n);
int qxx_read(qxx *x, void *data, size_t n);
int qxx_ready(qxx *x);
void qxx_pump(qxx *x);
#endif
