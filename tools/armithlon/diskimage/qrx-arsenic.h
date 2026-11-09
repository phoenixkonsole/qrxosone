#ifndef QRX_ARSENIC_H
#define QRX_ARSENIC_H
#include <stdint.h>
int qrx_arsenic_decode(const uint8_t *src, uint32_t srclen,
                       uint8_t *dst, uint32_t dstlen);
#endif
