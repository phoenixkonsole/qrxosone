#ifndef QRX_HUNK_H
#define QRX_HUNK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Strict, bounded AmigaDOS executable HUNK loader: CPU-side code/data/BSS and
 * HUNK_RELOC32, NOT the AmigaDOS/Exec resident library or device ABI. */
#define QRX_HUNK_STAGE_ADDR UINT32_C(0x00010000)
#define QRX_HUNK_STAGE_MAX (32u*1024u*1024u) /* bounded, dynamic scratch; NOT 32 MiB minimum RAM */
#define QRX_HUNK_MAX_SEGMENTS 16u
/* Transactional: on invalid HUNK no guest bytes are modified. */
bool qrx_hunk_load(const uint8_t *file,size_t len,uint8_t *guest,size_t guest_len,
                   uint32_t stage_base,uint32_t *entry,size_t *loaded);
#endif
