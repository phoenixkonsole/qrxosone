#ifndef QRX68100_FULL_H
#define QRX68100_FULL_H
/* ARMITHLON 1.4: optional Musashi-backed 68000..68040 QRX virtual CPU.
 * Not 68060, and not an Amiga chipset or AROS BSP implementation.
 * Musashi's public API is global: ONLY ONE live QRXFullCPU per process.
 */
#include "qrxvirt.h"
#include "qrx_memory.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define QRX_FULL_ROM_MAIN_BASE UINT32_C(0x00F80000)
#define QRX_FULL_ROM_EXT_BASE  UINT32_C(0x00E00000)
#define QRX_FULL_ROM_MAX (1024u * 1024u)
#define QRX_FULL_IO_BASE UINT32_C(0xFF000000) /* disjoint from Kickstart ROM */
#define QRX_FULL_TIMER_CTRL  (QRX_FULL_IO_BASE+0x20u)
#define QRX_FULL_TIMER_PERIOD (QRX_FULL_IO_BASE+0x24u)
#define QRX_FULL_IRQ_PENDING (QRX_FULL_IO_BASE+0x28u)
#define QRX_FULL_IRQ_ACK (QRX_FULL_IO_BASE+0x2cu)
#define QRX_FULL_ROM_OVERLAY_CTRL (QRX_FULL_IO_BASE+0x30u)
/* control bits: 1 enable timer, 2 autoreload; level fixed to 2 for timer */
#define QRX_FULL_TIMER_ENABLE 1u
#define QRX_FULL_TIMER_AUTO   2u

struct QRXFullCPU;
typedef bool (*QRXAuxReadFn)(void*, uint32_t, unsigned, uint32_t*);
typedef bool (*QRXAuxWriteFn)(void*, uint32_t, unsigned, uint32_t);
typedef void (*QRXRamWriteFn)(void*, uint32_t, unsigned);

typedef struct QRXFullCPU {
    QRXAuxReadFn aux_read;
    QRXAuxWriteFn aux_write;
    QRXRamWriteFn ram_write;
    void *aux_opaque;
    unsigned aux_cpu_index; /* 0 = bootstrap core: board timer IRQ owner */
    QRXVM vm;
    uint8_t *ram_owned;
    QRXHostMemory host_allocation;
    uint8_t *rom_main, *rom_ext;
    size_t rom_main_size, rom_ext_size;
    bool rom_overlay;
    uint32_t timer_control, timer_period, timer_remaining;
    uint8_t irq_level, irq_pending;
    uint64_t cycles, instructions;
    bool mem_fault, reset_done;
    uint32_t fault_address;
    uint32_t subroutine_sentinel;
    bool subroutine_armed, subroutine_returned;
    int cpu_type;
} QRXFullCPU;

/* Hooks are owned by the board extension; only one extension per board. */
void qrx_full_set_aux(QRXFullCPU *c, QRXAuxReadFn readfn,
                      QRXAuxWriteFn writefn, QRXRamWriteFn ramfn, void *opaque);

/* Fatal if a second live context is already bound: returns false. */
bool qrx_full_init(QRXFullCPU *c, size_t ram_size, unsigned model);
void qrx_full_destroy(QRXFullCPU *c);
/* Copies ROM to owned storage. base is F80000 or E00000, 512K main, 512K ext. */
bool qrx_full_load_rom(QRXFullCPU *c, uint32_t base, const void *data, size_t size);
/* Optional Amiga ROM vector overlay at reset; no compatibility promise. */
void qrx_full_set_rom_overlay(QRXFullCPU *c, bool enabled);
bool qrx_full_reset(QRXFullCPU *c);
void qrx_full_set_host(QRXFullCPU *c, QRXConsoleFn out, QRXClockFn ms, void *opaque);
/* Deterministic guest-bus entry points. 0xff/ffff/ffffffff returned for unmapped. */
bool qrx_full_bus_read(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t *out);
bool qrx_full_bus_write(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t value);
/* IRQ levels 1..7 are level-sensitive. 0 deasserts an external IRQ.
 * Full 32-bit QRX MMIO alias only accessible as guest address for 68020+. */
void qrx_full_irq(QRXFullCPU *c, unsigned level);
/* 0: guest shutdown; 1: cycle budget; 2: idle STOP; -1: invalid bus address/fault. */
int qrx_full_run_cycles(QRXFullCPU *c, uint64_t budget);
/* Execute trusted guest 68k subroutine with bounded cycle budget and a NOP
 * return sentinel. Restores the firmware PC and SP on success; other registers
 * retain their normal 68k callee semantics. NOT an OS task scheduler. */
bool qrx_full_run_subroutine(QRXFullCPU *c,uint32_t entry,
                             uint64_t cycle_budget,uint64_t *retired);
uint32_t qrx_full_pc(const QRXFullCPU *c);
uint32_t qrx_full_reg(const QRXFullCPU *c, unsigned reg); /* use Musashi enum */
#endif
