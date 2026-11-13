#ifndef QRX_HOSTNET_H
#define QRX_HOSTNET_H
#include <stdbool.h>
#include <stdint.h>
/* One Winsock-style call. a[0..7] arguments, a[8] result, a[9] WSA error, a[10] ip, a[11] port.
 * ram is guest RAM, for the buffers named in the arguments. */
/* a[1] guest address, a[2] bytes, at most 256: the host's random generator. */
#define QRX_NET_RANDOM 19u
/* a[10] Unix seconds, a[11] microseconds: the host's clock. The guest has no battery clock.
 * a[12] the host's offset from UTC in seconds, east positive. a has 13 words. */
#define QRX_NET_TIME 20u
void qrx_hostnet_call(uint32_t op, uint32_t *a, uint8_t *ram);
#endif
