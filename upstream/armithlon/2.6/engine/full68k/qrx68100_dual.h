#ifndef QRX68100_DUAL_H
#define QRX68100_DUAL_H
/* QRX68100, fictional dual-68040 virtual CPU.  Version 1.5.
 * The two Musashi contexts share one coherent RAM/ROM/MMIO bus.
 * Timesliced, NOT host parallel, and NOT two independent Amiga machines.
 * Legacy mode is the power-on default; AmigaOS 3.x sees only core 0.
 */
#include "qrx68100_full.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define QRX_DUAL_BASE (QRX_FULL_IO_BASE + UINT32_C(0x600))
#define QRX_DUAL_MAGIC (QRX_DUAL_BASE + 0x00u)  /* 'QRXD' */
#define QRX_DUAL_ABI   (QRX_DUAL_BASE + 0x04u)  /* 0x00010000 */
#define QRX_DUAL_FEATURES (QRX_DUAL_BASE + 0x08u)
#define QRX_DUAL_STATUS   (QRX_DUAL_BASE + 0x0cu) /* core online mask */
#define QRX_DUAL_CORE1_PC (QRX_DUAL_BASE + 0x10u)
#define QRX_DUAL_CORE1_SP (QRX_DUAL_BASE + 0x14u)
#define QRX_DUAL_CORE1_CTL (QRX_DUAL_BASE + 0x18u) /* 1 start, 2 park */
#define QRX_DUAL_IPI_SET   (QRX_DUAL_BASE + 0x1cu) /* 1 target 0, 2 target 1 */
#define QRX_DUAL_IPI_ACK   (QRX_DUAL_BASE + 0x20u) /* acknowledge only own core */
#define QRX_DUAL_CODE_GEN  (QRX_DUAL_BASE + 0x24u)
#define QRX_DUAL_JIT_HITS  (QRX_DUAL_BASE + 0x28u)
#define QRX_DUAL_JIT_MISS  (QRX_DUAL_BASE + 0x2cu)
#define QRX_DUAL_CACHE_FLUSH (QRX_DUAL_BASE + 0x30u) /* 1 flush */
#define QRX_DUAL_CORE_ID  (QRX_DUAL_BASE + 0x34u)
#define QRX_DUAL_FEATURE_DUAL (1u<<0)
#define QRX_DUAL_FEATURE_IPI (1u<<1)
#define QRX_DUAL_FEATURE_CACHE_COHERENCE (1u<<2)
#define QRX_DUAL_FEATURE_ISA_TAGGED_BLOCK_CACHE (1u<<3) /* metadata only, no native JIT yet */
#define QRX_DUAL_CACHE_SLOTS 2048u
#define QRX_DUAL_CACHE_WAYS 4u
#define QRX_DUAL_PAGE_SIZE 4096u
#define QRX_DUAL_ISA_68K 1u
#define QRX_DUAL_ISA_X86 2u
#define QRX_DUAL_ISA_PPC 3u
#define QRX_DUAL_ISA_X64 4u
#define QRX_DUAL_ISA_ARM 5u

typedef enum { QRX_BOOT_LEGACY_AMIGA = 0, QRX_BOOT_QRX_DUAL = 1 } QRXDualMode;
typedef struct {
    uint32_t pc, page_generation, global_generation, asid;
    uint16_t guest_bytes;
    uint8_t isa, valid;
    uint64_t uses;
} QRXJITBlock;
typedef struct {
    void *musashi_context;
    uint64_t cycles, instructions, jit_hits, jit_misses;
    uint64_t parallel_iterations, native_jit_runs;
    uint8_t online, ipi_pending, idle;
    QRXJITBlock blocks[QRX_DUAL_CACHE_SLOTS];
} QRXDualCore;
typedef struct QRXDual {
    QRXFullCPU *board;
    QRXDualCore core[2];
    uint32_t *page_epochs;
    size_t page_count;
    uint32_t code_generation;
    uint32_t global_generation;
    uint32_t core1_pc, core1_sp;
    QRXDualMode mode;
    unsigned loaded;
    bool boot_requested, park_requested, initialized;
    struct QRXFastCache *fast_cache; /* owner-thread only; compiled fast-path */
    struct QRXVampState *vamp; /* 68080-inspired L1 + vector unit, QRX-only */
} QRXDual;

/* Must be called AFTER qrx_full_reset on a 68040 board. */
bool qrx_dual_init(QRXDual *d, QRXFullCPU *board);
void qrx_dual_destroy(QRXDual *d);
/* Called after qrx_full_reset to re-snapshot BSP core 0 and park core 1. */
void qrx_dual_reset(QRXDual *d);
/* Legacy remains default; enabling QRX mode is an explicit host/BSP decision. */
bool qrx_dual_set_mode(QRXDual *d, QRXDualMode mode);
/* Drain pending secondary boot at a safe instruction boundary. */
bool qrx_dual_commit(QRXDual *d);
/* Per-online-core budget, deterministic round-robin; returns -1 on fault,
 * 0 on global board shutdown, 1 budget consumed, 2 all cores STOP/idle. */
int qrx_dual_run(QRXDual *d, uint64_t cycles_per_core);
uint32_t qrx_dual_reg(QRXDual *d, unsigned core, unsigned musashi_reg);
uint32_t qrx_dual_pc(QRXDual *d, unsigned core);
/* Code cache metadata only: no native code generation or ISA translation yet.
 * Block must fit entirely in one 4K guest page; never return executable pointers. */
bool qrx_dual_jit_insert(QRXDual *d,unsigned core,uint32_t asid,uint8_t isa,
                         uint32_t pc,uint16_t byte_length);
bool qrx_dual_jit_lookup(QRXDual *d,unsigned core,uint32_t asid,uint8_t isa,
                         uint32_t pc);
void qrx_dual_jit_flush(QRXDual *d);
/* Mandatory for writes that bypass qrx_full_bus_write, e.g. host DMA. */
void qrx_dual_notify_ram_write(QRXDual *d,uint32_t addr,unsigned width);
#endif
