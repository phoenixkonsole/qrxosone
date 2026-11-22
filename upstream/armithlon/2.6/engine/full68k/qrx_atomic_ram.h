#ifndef QRX_ATOMIC_RAM_H
#define QRX_ATOMIC_RAM_H
/* TAS and CAS on guest RAM, atomic against the other 68100 core. Guest memory is big
 * endian. Returns 0 for a misaligned operand, which the caller leaves to the plain path. */
#include <stdint.h>
static inline int qrx_ram_cas(uint8_t *p, unsigned width, uint32_t cmp, uint32_t val, uint32_t *old, int *swapped)
{
    if (width == 4) {
        if ((uintptr_t)p & 3u) return 0;
        uint32_t e = __builtin_bswap32(cmp);
        *swapped = __atomic_compare_exchange_n((uint32_t *)p, &e, __builtin_bswap32(val), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *old = __builtin_bswap32(e);
    } else if (width == 2) {
        if ((uintptr_t)p & 1u) return 0;
        uint16_t e = __builtin_bswap16((uint16_t)cmp);
        *swapped = __atomic_compare_exchange_n((uint16_t *)p, &e, __builtin_bswap16((uint16_t)val), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *old = __builtin_bswap16(e);
    } else {
        uint8_t e = (uint8_t)cmp;
        *swapped = __atomic_compare_exchange_n(p, &e, (uint8_t)val, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *old = e;
    }
    return 1;
}
#endif
