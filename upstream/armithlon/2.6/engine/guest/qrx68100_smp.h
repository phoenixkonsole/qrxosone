#ifndef QRX68100_GUEST_SMP_H
#define QRX68100_GUEST_SMP_H
/* QRX68100 guest-side second core (Motorola 68040, big endian).
 * Core 1 is a real second host thread with its own registers and JIT cache. It starts
 * supervisor with interrupts masked and reaches shared RAM and these registers only:
 * no chipset, ROM, libraries or interrupts, and AROS Exec does not schedule onto it.
 * A job ends with STOP or by writing 2 to CTL (0x18). An exception or a bus fault parks
 * it with status bit 2 set, PC in 0x38, and vector or address in 0x3C.
 *
 * Registers, 32 bit: 0x00 magic, 0x04 ABI, 0x08 features (bit 4: runs in parallel),
 * 0x0C status (bit 0 core 0, bit 1 core 1 busy, bit 2 fault), 0x10 PC, 0x14 SP,
 * 0x18 CTL, 0x28 core 1 instructions of the job, 0x30 flush code caches, 0x34 own core id.
 * CTL: 1 start with a fresh code cache, 3 start keeping translations, 2 park.
 *
 * Code changes are not snooped between the cores, as between two 68040s: use CTL=1, or
 * write 0x30 after one core has changed code the other one runs. TAS and CAS (not CAS2)
 * are atomic on RAM. Ordering follows the 68040's bus order.
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
/* Entry and stack must be in shared RAM and 2-byte aligned. */
static inline void qrx_smp_start_secondary(uint32_t pc,uint32_t supervisor_stack) {
  qrx_smp_wr(0x10,pc);qrx_smp_wr(0x14,supervisor_stack);
  qrx_smp_wr(0x18,1);
}
static inline void qrx_smp_flush_code_cache(void){qrx_smp_wr(0x30,1);}
#endif
