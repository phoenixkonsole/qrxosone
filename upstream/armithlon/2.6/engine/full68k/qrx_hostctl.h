#ifndef QRX_HOSTCTL_H
#define QRX_HOSTCTL_H
/* Door service 11: Wi-Fi, Bluetooth and power of the host appliance, see qrx_hostctl.c */
#include <stdint.h>
#define QRX_HOSTCTL_SERVICE 11u
void qrx_hostctl_call(uint8_t *ram, uint64_t ram_size, uint8_t *q);
#endif
