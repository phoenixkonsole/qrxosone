#ifndef QRX68100_GUEST_SMP_H
#define QRX68100_GUEST_SMP_H
/* QRX68100 1.5 guest-side SMP interface (Motorola 68040, big endian).
 * 3.x AmigaOS MUST NOT probe QRX-specific MMIO. Its host profile keeps core1
 * hidden and inactive. Only a QRX-aware OS/BSP enables the enhanced profile.
 */
#include <stdint.h>
#define QRX_SMP_BASE ((uintptr_t)0xff000600u)
static inline uint32_t qrx_smp_rd(unsigned byteoffset) {
  return *(volatile const uint32_t *)(QRX_SMP_BASE+byteoffset);
}
static inline void qrx_smp_wr(unsigned byteoffset,uint32_t value) {
  *(volatile uint32_t *)(QRX_SMP_BASE+byteoffset)=value;
}
#define QRX_SMP_MAGIC 0x51525844u
#define QRX_SMP_CORE0 0u
#define QRX_SMP_CORE1 1u
/* For OS kernels with supervisor access only; core1 entry and stack
 * must refer to valid shared RAM and be 2-byte aligned. */
static inline void qrx_smp_start_secondary(uint32_t pc,uint32_t supervisor_stack) {
  qrx_smp_wr(0x10,pc);qrx_smp_wr(0x14,supervisor_stack);
  qrx_smp_wr(0x18,1);
}
static inline void qrx_smp_ipi(unsigned target) {
  qrx_smp_wr(0x1c,(uint32_t)1u<<target);
}
static inline void qrx_smp_ipi_ack(void){qrx_smp_wr(0x20,1);}
static inline void qrx_smp_flush_code_cache(void){qrx_smp_wr(0x30,1);}
#endif
