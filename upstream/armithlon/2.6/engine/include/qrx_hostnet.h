#ifndef QRX_HOSTNET_H
#define QRX_HOSTNET_H
#include <stdbool.h>
#include <stdint.h>
/* One Winsock-style call. a[0..7] arguments, a[8] result, a[9] WSA error, a[10] ip, a[11] port.
 * ram is guest RAM, for the buffers named in the arguments. */
void qrx_hostnet_call(uint32_t op, uint32_t *a, uint8_t *ram);
#endif
