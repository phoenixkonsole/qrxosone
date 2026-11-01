#ifndef QRX68100_VAMPIRE_H
#define QRX68100_VAMPIRE_H
/* QRX68100 1.7: independent Vampire/APOLLO-inspired extensions.
 * NOT an Apollo 68080 clone. No Apollo AMMX binary-opcode support.
 * Legacy AmigaOS always sees a stock 68040 with one core; QRX MMIO is hidden.
 * Functional L1 caches accelerate *QRX JIT instruction reads*, not Musashi.
 */
#include "qrx68100_dual.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QRX_VAMP_BASE (QRX_DUAL_BASE+0x100u) /* FF000700 .. FF0007FF */
#define QRX_VAMP_MAGIC      (QRX_VAMP_BASE+0x00u) /* 'QV17' */
#define QRX_VAMP_ABI        (QRX_VAMP_BASE+0x04u)
#define QRX_VAMP_FEATURES   (QRX_VAMP_BASE+0x08u)
#define QRX_VAMP_CPU_ID     (QRX_VAMP_BASE+0x0cu) /* 0x00068100, QRX-only */
#define QRX_VAMP_CTRL       (QRX_VAMP_BASE+0x10u)
#define QRX_VAMP_IC_BYTES   (QRX_VAMP_BASE+0x14u)
#define QRX_VAMP_DC_BYTES   (QRX_VAMP_BASE+0x18u)
#define QRX_VAMP_L2_BYTES   (QRX_VAMP_BASE+0x1cu) /* 0: not emulated */
#define QRX_VAMP_VSEL       (QRX_VAMP_BASE+0x20u)
#define QRX_VAMP_VSRC       (QRX_VAMP_BASE+0x24u)
#define QRX_VAMP_VDST       (QRX_VAMP_BASE+0x28u)
#define QRX_VAMP_VLO        (QRX_VAMP_BASE+0x2cu)
#define QRX_VAMP_VHI        (QRX_VAMP_BASE+0x30u)
#define QRX_VAMP_VCMD       (QRX_VAMP_BASE+0x34u)
#define QRX_VAMP_VSTATUS    (QRX_VAMP_BASE+0x38u)
#define QRX_VAMP_IC_HITS    (QRX_VAMP_BASE+0x40u)
#define QRX_VAMP_IC_MISSES  (QRX_VAMP_BASE+0x44u)
#define QRX_VAMP_DC_HITS    (QRX_VAMP_BASE+0x48u)
#define QRX_VAMP_DC_MISSES  (QRX_VAMP_BASE+0x4cu)
#define QRX_VAMP_BP_GOOD    (QRX_VAMP_BASE+0x50u)
#define QRX_VAMP_BP_BAD     (QRX_VAMP_BASE+0x54u)
#define QRX_VAMP_CACHE_INV  (QRX_VAMP_BASE+0x58u) /* supervisor-only; 1 */
#define QRX_VAMP_F_VEC64    (1u<<0)
#define QRX_VAMP_F_MUL64    (1u<<1)
#define QRX_VAMP_F_DIV64    (1u<<2)
#define QRX_VAMP_F_L1I      (1u<<3)
#define QRX_VAMP_F_L1D      (1u<<4)
#define QRX_VAMP_F_BRANCH   (1u<<5)
#define QRX_VAMP_CAPABILITIES (QRX_VAMP_F_VEC64|QRX_VAMP_F_MUL64|QRX_VAMP_F_DIV64|QRX_VAMP_F_L1I|QRX_VAMP_F_L1D|QRX_VAMP_F_BRANCH)
#define QRX_VAMP_CTRL_CACHE (1u<<0)
#define QRX_VAMP_CTRL_PREFETCH (1u<<1)
#define QRX_VAMP_CTRL_BRPRED (1u<<2)
#define QRX_VAMP_CTRL_MASK 7u
#define QRX_VAMP_VEC_REGS 24u
#define QRX_VAMP_LINE_BYTES 64u
#define QRX_VAMP_ICACHE_BYTES (16u*1024u)
#define QRX_VAMP_DCACHE_BYTES (32u*1024u)
/* All commands operate on two 64-bit guest-visible regs and store in VDST.
 * No 68k CCR/FPU registers are modified, like common SIMD operations. */
#define QRX_VAMP_CMD_PADDB  1u /* packed 8x8 unsigned wrap */
#define QRX_VAMP_CMD_PADDW  2u /* packed 4x16 unsigned wrap */
#define QRX_VAMP_CMD_PSUBB  3u
#define QRX_VAMP_CMD_PXOR   4u
#define QRX_VAMP_CMD_PAND   5u
#define QRX_VAMP_CMD_POR    6u
#define QRX_VAMP_CMD_MUL64  7u /* result low 64 bits */
#define QRX_VAMP_CMD_DIV64  8u /* unsigned, div0 rejected */
#define QRX_VAMP_CMD_PAVGB  9u /* rounded byte average */
#define QRX_VAMP_CMD_PADDSB 10u /* saturating unsigned byte add */
#define QRX_VAMP_STATUS_OK 0u
#define QRX_VAMP_STATUS_BAD_CMD 1u
#define QRX_VAMP_STATUS_DIV_ZERO 2u

struct QRXVampState;
/* Allocated per virtual motherboard; only owner thread may call these. */
bool qrx_vamp_init(QRXDual *d);
void qrx_vamp_destroy(QRXDual *d);
void qrx_vamp_reset(QRXDual *d);
void qrx_vamp_reset_core(QRXDual *d,unsigned cpu);
void qrx_vamp_invalidate(QRXDual *d,uint32_t addr,unsigned n);
bool qrx_vamp_mmio_read(QRXDual *d,uint32_t addr,unsigned width,uint32_t *out);
bool qrx_vamp_mmio_write(QRXDual *d,uint32_t addr,unsigned width,uint32_t value);
/* Functional, coherent L1 access for JIT decode. Never caches MMIO, ROM,
 * reset overlay, or out-of-range RAM. Musashi 68040 fetches remain unchanged. */
bool qrx_vamp_cached_read(QRXDual *d,unsigned core,uint32_t addr,unsigned width,
                          bool instruction,uint32_t *out);
/* Optional safe speculation, only for validated RAM and enabled prefetch. */
bool qrx_vamp_prefetch(QRXDual *d,unsigned core,uint32_t addr);
/* Software branch history for future JIT scheduling; no claimed IPC acceleration. */
bool qrx_vamp_predict(QRXDual *d,unsigned core,uint32_t pc);
void qrx_vamp_branch(QRXDual *d,unsigned core,uint32_t pc,bool taken);
uint64_t qrx_vamp_vector(QRXDual *d,unsigned core,unsigned reg);
/* Native JIT/QRX kernel interface: guest-architecture-neutral execution. */
bool qrx_vamp_vector_write(QRXDual *d,unsigned core,unsigned reg,uint64_t val);
unsigned qrx_vamp_vector_execute(QRXDual *d,unsigned core,unsigned src,
                                 unsigned src2,unsigned dst,unsigned cmd);
#endif
