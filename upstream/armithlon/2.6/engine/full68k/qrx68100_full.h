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

/* FSAGA (final SAGA) v1: Apollo-compatible "Vampire V4 Standalone"-style RTG.
 * Same MMIO handshake as the real SAGA chipset so the stock AROS
 * `sagagfx.hidd` driver (DEVS:Drivers/sagagfx.hidd) attaches unchanged, but
 * chunky-framebuffer rendering is accelerated on the host and 3D passes
 * through the QRXV3D command door (see guest/qrxvirt_3d.h). No Denise/Agnus
 * planar chipset is emulated, exactly like Amithlon's RTG-only setup. */
#define QRX_FSAGA_BOARD_READ UINT32_C(0x00DFF3FC) /* VREG_BOARD, high byte = 5 */
#define QRX_FSAGA_VIDEO_MODE UINT32_C(0x00DFF1F4) /* 16-bit, low 7 bits format */
#define QRX_FSAGA_VIDEO_BPLPTR UINT32_C(0x00DFF1EC) /* 32-bit write: fb base */
#define QRX_FSAGA_VIDEO_BPLHMOD UINT32_C(0x00DFF1E6)
#define QRX_FSAGA_VIDEO_SPRITEX UINT32_C(0x00DFF1D0)
#define QRX_FSAGA_VIDEO_SPRITEY UINT32_C(0x00DFF1D2)
#define QRX_FSAGA_VIDEO_SYNC_BASE UINT32_C(0x00DFF300) /* HPIXEL..HVSYNC */
#define QRX_FSAGA_VIDEO_CLUT_BASE UINT32_C(0x00DFF400) /* 256 x WRITE32 */
#define QRX_FSAGA_VIDEO_SPRITEBPL UINT32_C(0x00DFF800) /* 16 x WRITE32 rows */
#define QRX_FSAGA_VIDEO_SPRITEPAL UINT32_C(0x00DFF3A0) /* cursor palette 1..3 */
#define QRX_FSAGA_VIDEO_PLLV4 UINT32_C(0x00DFF3F8)
/* SAGA_VIDEO_MODE format codes (low 7 bits; bit 7 LE, bit8..9 dblscan). */
#define QRX_FSAGA_FMT_AMIGA 0u
#define QRX_FSAGA_FMT_CLUT8 1u
#define QRX_FSAGA_FMT_RGB16 2u
#define QRX_FSAGA_FMT_RGB15 3u
#define QRX_FSAGA_FMT_RGB24 4u
#define QRX_FSAGA_FMT_RGB32 5u
#define QRX_FSAGA_FMT_OFF 0x80u
/* Host display hook: fires on each visible vblank with the current FSAGA
 * framebuffer. format is QRX_FSAGA_FMT_*. Call from the host main loop; the
 * guest RAM window is read through qrx_full_bus_read on demand by the host. */
struct QRXFullCPU;
typedef void (*QRXDisplayFn)(void*, const struct QRXFullCPU*, uint32_t frame,
                             uint32_t addr, uint32_t width, uint32_t height,
                             uint32_t format);
typedef bool (*QRXAuxReadFn)(void*, uint32_t, unsigned, uint32_t*);
typedef bool (*QRXAuxWriteFn)(void*, uint32_t, unsigned, uint32_t);
typedef void (*QRXRamWriteFn)(void*, uint32_t, unsigned);

typedef struct {
    uint8_t reg[16], pending, mask;
    uint16_t latch[2];
    uint32_t remaining[2], tod, tod_latched;
    bool tod_hold;
} QRXBootCIA;

typedef struct QRXFullCPU {
    QRXBootCIA boot_cia[2];
    uint64_t boot_cia_clock;
    uint16_t boot_intreq;
    unsigned boot_drive_id_bit;
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
    /* full-musashi ROM-write support (see host_ro_probe) */
    bool ro_wronly;         /* QRX_FULL_ROM_WRONLY: persist guest ROM writes */
    bool rom_overlay;
    uint8_t ciaa_pra; /* PRA shadow; bit 0 = ROM overlay, Amiga polarity */
    uint8_t ciab_prb; /* CIA-B PRB latch (SEL/DIR/SIDE/STEP/MOTOR), guest RMW */
    int8_t fl_cyl[4]; /* virtual head cylinder per unit; PRA bit4 = track0 */
    uint8_t cia_cra; /* timer A control shadow */
    uint16_t cia_ta_latch; /* timer A latch, host-ticked while CRA start bit set */
    uint64_t cia_ta_start; /* instruction counter at timer A (re)start */
    uint16_t cia_tb_latch; /* timer B latch (CIA-A), host-ticked while CRB start set */
    uint64_t cia_tb_ref; /* instruction counter at the last Timer B underflow check */
    uint8_t cia_crb; /* timer B control shadow (auto-start on TBHI reload) */
    uint32_t timer_control, timer_period, timer_remaining;
    uint8_t irq_level, irq_pending;
    uint64_t beam_frame; /* last delivered Denise frame index */
    uint8_t vblank_asserted; /* VERTB (level 3) held until INTREQ ack */
    uint16_t intena; /* INTENA shadow: Paula interrupt enable state */
    uint8_t dskblk_asserted; /* DSKBLK (level 1) held until INTREQ ack */
    uint8_t ports_asserted; /* Paula PORTS (level 2), software + CIA driven */
    uint8_t kick_pending; /* host boot-wake pulse: level-3 until int_ack */
    uint8_t cia_icr, cia_icr_mask; /* CIA-A flag latch / enable mask */
    uint64_t cia_ref; /* instruction counter at the last Timer A underflow check */
    uint8_t cia_icr_trace; /* env meter, set by the host loader */
    uint8_t ciab_icr, ciab_icr_mask; /* CIA-B flag latch / enable mask */
    uint8_t ciab_crb; /* CIA-B timer-B control register CRB */
    uint32_t ciab_tblo; /* last TBLO byte written, hex debug */
    uint16_t ciab_tb_latch; /* CIA-B timer-B 16-bit latch */
    uint64_t ciab_tb_ref; /* instruction counter at the last Timer B check */
    uint8_t ciab_icr_trace; /* env meter, set by the host loader */
        /* FSAGA (final SAGA) RTG emulation (see header block above). */
    uint16_t fsaga_mode; /* QRX_FSAGA_VIDEO_MODE shadow */
    uint16_t fsaga_hpixel, fsaga_hsstrt, fsaga_hsstop, fsaga_htotal;
    uint16_t fsaga_vpixel, fsaga_vsstrt, fsaga_vsstop, fsaga_vtotal, fsaga_hvsync;
    uint16_t fsaga_bplhmod;
    uint32_t fsaga_bplptr;
    uint32_t fsaga_pllv4;
    uint32_t fsaga_clut[256]; /* one RGB entry per palette index */
    uint16_t fsaga_spriteclut[4]; /* cursor palette, palette[0] unused by driver */
    uint8_t fsaga_cursor[16u][4]; /* 16x16 cursor bitmask rows, 2 pw per row */
    uint16_t fsaga_spritex, fsaga_spritey;
    uint32_t fsaga_frame; /* increment on each presented vblank with fb */
    uint8_t fsaga_trace; /* env QRX_FULL_FSAGA_TRACE */
    QRXDisplayFn display;
    void *display_arg;
    uint32_t fsaga_width, fsaga_height; /* cached fb dimensions */
    /* Amithlon host-clock: the engine advances the guest TimerBase's
       SystemTime itself (the ROM's ciab_eclock ISR is never dispatched on
       this boot path): detected via the guest's EClock-CIA-marker writes. */
    uint32_t host_tb;       /* guest TimerBase address, 0 = unknown */
    uint32_t host_tb_rate;  /* guest Eclock rate Hz, cached from tb+0x8E */
    uint32_t host_tb_enable;/* host-clock on (QRX_FULL_HOST_E_CLOCK, def 1) */
    uint64_t host_tb_total; /* total Eclock ticks since TimerBase armed */
    uint64_t host_tb_sec;   /* whole seconds already written to the guest */
    uint32_t host_vb_acc;   /* vblank micro accumulator for tb_vb_count */
    bool host_vb_enable;    /* QRX_FULL_HOST_VB_COUNT=1, default off */
    bool host_tb_dumped;    /* one-shot TimerBase field snapshot */
    /* a zero-initialized struct covers it; struct init is calloc-family. */
    /* Amiga floppy: host-injected ADF image + live MFM DMA. */
    uint8_t *adf_data;
    size_t adf_size;
    uint8_t mfm_cache[27260u]; /* one encoded ADFS track (HD fits, DD reuses) */
    int32_t mfm_track; /* encoded track number, -1 = invalid */
    uint8_t dsk_active; /* DMA in progress */
    uint16_t dsk_len; /* guest DSKLEN word write */
    uint32_t dsk_pt; /* guest DSKPT pointer */
    uint16_t dsk_last; /* previous DSKLEN write (twice-write start rule) */
    uint16_t adkcon_shadow;
    uint16_t dmacon_shadow; /* DMACON data shadow (DSKEN bit checked on read) */
    uint64_t dsk_start; /* cycles at DMA start */
    uint64_t cycles, instructions;
    uint32_t pc_ring[16];
    unsigned pc_ring_i;
    bool vec_trace;
    bool mem_fault, reset_done;
    uint32_t fault_address;
    uint32_t subroutine_sentinel;
    bool subroutine_armed, subroutine_returned;
    int cpu_type;
} QRXFullCPU;

/* Host-injected ADF floppy image: real MFM DMA, DataBlocks et al follow. */
void qrx_full_load_adf(QRXFullCPU *c, const void *data, size_t size);

/* FSAGA display hookup; must be set before qrx_full_run_cycles(). */
void qrx_full_set_display(QRXFullCPU *c, QRXDisplayFn fn, void *opaque);
/* Push the current FSAGA framebuffer to the host without waiting for the next
 * guest vblank (uses the same sink set by qrx_full_set_display). No-ops when
 * the RTG driver has not yet armed SAGA_VIDEO_MODE, the base pointer or size. */
void qrx_full_fsaga_present(QRXFullCPU *c);

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
