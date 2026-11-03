#include "qrx68100_full.h"
#include "m68k.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <sys/mman.h>
#include <time.h>


/* Environment options are immutable during one VM run. Cache outside hot paths. */
static const char *env_QRX_FULL_BOOTKICK_TRACE;
static const char *env_QRX_FULL_FL_HDPATCH;
static const char *env_QRX_FULL_FL_TRACE;
static const char *env_QRX_FULL_FSAGA_TRACE;
static const char *env_QRX_FULL_GATE_SPY;
static const char *env_QRX_FULL_HOOKSPY;
static const char *env_QRX_FULL_HOST_E_CLOCK;
static const char *env_QRX_FULL_HOST_E_CLOCK_TRACE;
static const char *env_QRX_FULL_HOST_VB_COUNT;
static const char *env_QRX_FULL_HW_TRACE;
static const char *env_QRX_FULL_ICR_TRACE;
static const char *env_QRX_FULL_IRQ_DEBUG_AFTER;
static const char *env_QRX_FULL_IRQ_DEBUG_MAX;
static const char *env_QRX_FULL_IRQ_TRACE;
static const char *env_QRX_FULL_KICK_TRACE;
static const char *env_QRX_FULL_PC_WATCH;
static const char *env_QRX_FULL_PHIST;
static const char *env_QRX_FULL_ROM_WRONLY;
static const char *env_QRX_FULL_STUCKKICK;
static const char *env_QRX_FULL_TRACE_HIGH;
static const char *env_QRX_FULL_TRACE_RAM;
static const char *env_QRX_FULL_TRACE_RAMW;
static const char *env_QRX_FULL_TRACE_RAMW_RANGE;
static const char *env_QRX_FULL_TRACE_RAMW_STACK;
static const char *env_QRX_FULL_TRACE_VEC;
static const char *env_QRX_FULL_TRACE_WINDOW;
static const char *env_QRX_FULL_TRACE_WINDOW_AFTER;
static const char *env_QRX_FULL_TRACE_WINDOW_MAX;
static const char *env_QRX_FULL_VBLANK_TRACE;
static const char *env_QRX_FULL_WATCH_PC;
static void read_options(void){
    env_QRX_FULL_BOOTKICK_TRACE=getenv("QRX_FULL_BOOTKICK_TRACE");
    env_QRX_FULL_FL_HDPATCH=getenv("QRX_FULL_FL_HDPATCH");
    env_QRX_FULL_FL_TRACE=getenv("QRX_FULL_FL_TRACE");
    env_QRX_FULL_FSAGA_TRACE=getenv("QRX_FULL_FSAGA_TRACE");
    env_QRX_FULL_GATE_SPY=getenv("QRX_FULL_GATE_SPY");
    env_QRX_FULL_HOOKSPY=getenv("QRX_FULL_HOOKSPY");
    env_QRX_FULL_HOST_E_CLOCK=getenv("QRX_FULL_HOST_E_CLOCK");
    env_QRX_FULL_HOST_E_CLOCK_TRACE=getenv("QRX_FULL_HOST_E_CLOCK_TRACE");
    env_QRX_FULL_HOST_VB_COUNT=getenv("QRX_FULL_HOST_VB_COUNT");
    env_QRX_FULL_HW_TRACE=getenv("QRX_FULL_HW_TRACE");
    env_QRX_FULL_ICR_TRACE=getenv("QRX_FULL_ICR_TRACE");
    env_QRX_FULL_IRQ_DEBUG_AFTER=getenv("QRX_FULL_IRQ_DEBUG_AFTER");
    env_QRX_FULL_IRQ_DEBUG_MAX=getenv("QRX_FULL_IRQ_DEBUG_MAX");
    env_QRX_FULL_IRQ_TRACE=getenv("QRX_FULL_IRQ_TRACE");
    env_QRX_FULL_KICK_TRACE=getenv("QRX_FULL_KICK_TRACE");
    env_QRX_FULL_PC_WATCH=getenv("QRX_FULL_PC_WATCH");
    env_QRX_FULL_PHIST=getenv("QRX_FULL_PHIST");
    env_QRX_FULL_ROM_WRONLY=getenv("QRX_FULL_ROM_WRONLY");
    env_QRX_FULL_STUCKKICK=getenv("QRX_FULL_STUCKKICK");
    env_QRX_FULL_TRACE_HIGH=getenv("QRX_FULL_TRACE_HIGH");
    env_QRX_FULL_TRACE_RAM=getenv("QRX_FULL_TRACE_RAM");
    env_QRX_FULL_TRACE_RAMW=getenv("QRX_FULL_TRACE_RAMW");
    env_QRX_FULL_TRACE_RAMW_RANGE=getenv("QRX_FULL_TRACE_RAMW_RANGE");
    env_QRX_FULL_TRACE_RAMW_STACK=getenv("QRX_FULL_TRACE_RAMW_STACK");
    env_QRX_FULL_TRACE_VEC=getenv("QRX_FULL_TRACE_VEC");
    env_QRX_FULL_TRACE_WINDOW=getenv("QRX_FULL_TRACE_WINDOW");
    env_QRX_FULL_TRACE_WINDOW_AFTER=getenv("QRX_FULL_TRACE_WINDOW_AFTER");
    env_QRX_FULL_TRACE_WINDOW_MAX=getenv("QRX_FULL_TRACE_WINDOW_MAX");
    env_QRX_FULL_VBLANK_TRACE=getenv("QRX_FULL_VBLANK_TRACE");
    env_QRX_FULL_WATCH_PC=getenv("QRX_FULL_WATCH_PC");
}

/* Musashi is intentionally single-context: no silent CPU-state crossover. */
static QRXFullCPU *active;
static void map_low_page_fwd(QRXFullCPU *c);
static unsigned full_model(unsigned m) {
    switch (m) {
    case 68000: return M68K_CPU_TYPE_68000;
    case 68010: return M68K_CPU_TYPE_68010;
    case 68020: return M68K_CPU_TYPE_68020;
    case 68030: return M68K_CPU_TYPE_68030;
    case 68040: return M68K_CPU_TYPE_68040;
    default: return M68K_CPU_TYPE_INVALID;
    }
}
static void host_ro_probe(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t value);
static bool upper_io(uint32_t addr,unsigned width) {
    return (uint64_t)addr>=QRX_FULL_IO_BASE && (uint64_t)addr+width<=(uint64_t)QRX_FULL_IO_BASE+QRX_IO_LENGTH;
}
static uint32_t norm_addr(uint32_t addr,unsigned width) {
    return upper_io(addr,width) ? QRX_IO_BASE+(addr-QRX_FULL_IO_BASE) : addr;
}
/* Amithlon-style: no custom-chipset emulation; trap Amiga board ranges
 * (custom 0xDFF000, CIA 0xBFE/BFD, reserved 0xF00000, Gayle 0xDA8000,
 * autoconfig 0xE80000) with benign idle values so Kickstart/AROS polling
 * loops proceed instead of bus-faulting. Two traps are functional:
 * SERDAT writes (0xDFF030) are forwarded to the host console as the AROS
 * serial-debug channel, SERDATR reads report TX-ready, and CIAA PRA
 * (0xBFE001) bit 0 drives the ROM overlay exactly like the AROS rom_init
 * `move.b #0,0xbfe001` expects. This is a trap scaffold, not
 * Paula/Agnus/Denise emulation. */
/* CIA timer A model: AROS calibrates PAL/NTSC by latching $FFFF, waiting two
 * TOD ticks and reading the residual count. TOD bytes free-run on the retired
 * instruction counter; timer A decrements while CRA bit 0 (start) is set. */
#define QRX_CIA_ICR 0x00BFED01u  /* legacy AROS CIA-A interrupt control alias */
/* Real Amiga hardware CIA-B window (floppy side): the trackdisk code drives
 * PRB $BFDD01 and the timer-B IRQ comes through ICR $BFDD1D. Registers sit
 * on every other byte (odd addresses). */
#define QRX_CIAB_BASE 0x00BFDD00u
#define QRX_CIAB_PRB  (QRX_CIAB_BASE + 0x01u)   /* 0x00BFDD01 */
#define QRX_CIAB_TBLO (QRX_CIAB_BASE + 0x09u)   /* 0x00BFDD09 */
#define QRX_CIAB_TBHI (QRX_CIAB_BASE + 0x0Bu)   /* 0x00BFDD0B */
#define QRX_CIAB_ICR  (QRX_CIAB_BASE + 0x1Du)   /* 0x00BFDD1D */
#define QRX_CIAB_CRA  (QRX_CIAB_BASE + 0x1Fu)   /* 0x00BFDD1F */
#define QRX_CIAB_CRB  (QRX_CIAB_BASE + 0x21u)   /* 0x00BFDD21 */
#define QRX_DFF_PTH 0x00DFF020u  /* DSKPT low word */
#define QRX_DFF_PTL 0x00DFF022u  /* DSKPT high word */
#define QRX_DFF_LEN 0x00DFF024u  /* DSKLEN */
#define QRX_DFF_MACHINE 0x00DFF096u /* DMACON */
#define QRX_FL_WORD_MASK 0x3FFFu
#define QRX_FL_WRITE_BIT 0x4000u
#define QRX_FL_INIT_BIT 0x8000u
#define QRX_FL_HDDISK_WORDS 12000u /* 13630 words = HD track DMA */
#define QRX_FL_HDDISK_SECTORS 22u
#define QRX_FL_DDDISK_SECTORS 11u
#define QRX_CIA_TAOFF 0x00BFE401u /* TALO legacy alias */
#define QRX_CIA_TAHI 0x00BFE501u /* TAHI legacy alias */
#define QRX_CIA_TBLO 0x00BFE601u /* TBLO legacy alias (CIA-A timer B) */
#define QRX_CIA_TBHI 0x00BFE701u /* TBHI legacy alias */
#define QRX_CIA_CRB  0x00BFEF01u /* Timer B control legacy alias */
#define QRX_CIA_TODLO 0x00BFE801u
#define QRX_CIA_TODMI 0x00BFE901u
#define QRX_CIA_TODHI 0x00BFEA01u
#define QRX_CIA_CRA 0x00BFEE01u
/* Correct Amiga-hw CIA-A addresses (register list steps odd bytes): the
 * ROM pulls some registers through these, the legacy aliases above cover
 * the rest. Both families get identical semantics below. */
#define QRX_CIAA_TALO  0x00BFE005u
#define QRX_CIAA_TAHI  0x00BFE007u
#define QRX_CIAA_TBLO  0x00BFE009u
#define QRX_CIAA_TBHI  0x00BFE00Bu
#define QRX_CIAA_TODLO 0x00BFE00Du
#define QRX_CIAA_TODMI 0x00BFE00Fu
#define QRX_CIAA_TODHI 0x00BFE011u
#define QRX_CIAA_ICR   0x00BFE01Du
#define QRX_CIAA_CRA   0x00BFE01Fu
#define QRX_CIAA_CRB   0x00BFE021u
#define QRX_CIA_DIV 4u /* guest instructions per emulated E-clock tick */
static uint32_t cia_tick(const QRXFullCPU *c) { return (uint32_t)(c->instructions >> 8); }
static uint16_t cia_ta_cur(QRXFullCPU *c) {
    if (c->cia_cra & 1u) {
        uint64_t el = (c->instructions - c->cia_ta_start) / QRX_CIA_DIV;
        return (uint16_t)(c->cia_ta_latch - (el & 0xFFFFu));
    }
    return c->cia_ta_latch;
}
static uint32_t ones(unsigned width) { return width == 1 ? 0xFFu : width == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
/* Denise beam model: free-running PAL raster so VBLANK/beam waits terminate
 * (e.g. the exec WaitTOF spin on VHPOSR/VPOSR). Driven by bus cycles, not
 * retired instructions: Musashi STOP fast-forwards slices with zero retired
 * instructions, so an instruction-driven beam would freeze exactly while the
 * idle task waits for the next frame. One line = 454 E-clocks, 313 lines
 * per frame (PAL). Horizontal maps to H8:H1 (0..0xE0). */
#define QRX_BEAM_LINE_CYCLES 454u
#define QRX_BEAM_LINES 313u
static uint16_t beam_vposr(const QRXFullCPU *c) {
    uint64_t line = (c->cycles / QRX_BEAM_LINE_CYCLES) % QRX_BEAM_LINES;
    return (uint16_t)((line >> 8) & 7u); /* V10:V8 in bits 2:0, LOF = 0 */
}
static uint16_t beam_vhposr(const QRXFullCPU *c) {
    uint64_t t = c->cycles;
    uint32_t line = (uint32_t)((t / QRX_BEAM_LINE_CYCLES) % QRX_BEAM_LINES);
    uint32_t h = (uint32_t)((t % QRX_BEAM_LINE_CYCLES) * 0xE0u / QRX_BEAM_LINE_CYCLES);
    return (uint16_t)(((line & 0xFFu) << 8) | (h & 0xFFu));
}
static uint32_t beam_byte(const QRXFullCPU *c, uint32_t addr, uint16_t word) {
    return (addr & 1u) ? (word & 0xFFu) : ((word >> 8) & 0xFFu);
}
/* Amiga-bus float is active only while a Kickstart is mapped: firmware and
 * mock harnesses without a ROM keep the strict QRX board behavior, so the
 * dual/vampire/hostfs register contracts stay exact. */
static bool amiga_bus(const QRXFullCPU *c) { return c && c->rom_main; }

/* ---------------------------------------------------------------- */
/* Paula/CIA hardware model: CIA-A Timer A ICR and ADFS floppy DMA.  */
/* ---------------------------------------------------------------- */
static void paula_recompute(QRXFullCPU *c);
#include "boot_cia.inc"

/* Amithlon host-clock (see docs/boot-stall-analysis-2026-11.md §13): the
 * guest's ciab_eclock ISR is never dispatched on this boot path, so the
 * engine advances the guest TimerBase's tb_CurrentTime/tb_Elapsed itself
 * at EClock rate. TimerBase is detected from the ROM's EClock-CIA marker
 * writes ($BFE001 into tb+0x5E, rate at tb+0x8E). QRX_FULL_HOST_E_CLOCK=0
 * opts out. */
static void host_eclock_tick(QRXFullCPU *c, unsigned wraps) {
    if (c->aux_cpu_index != 0 || !amiga_bus(c)) return;
    if (!c->host_tb || !c->host_tb_rate || !c->host_tb_enable) return;
    if (!(c->cia_crb & 1u)) return; /* guest stopped the EClock timer */
    c->host_tb_total += (uint64_t)wraps * 65536u;
    uint32_t rate = c->host_tb_rate;
    uint64_t secs = c->host_tb_total / rate, rem = c->host_tb_total % rate;
    uint32_t micro = (uint32_t)((rem * 1000000u) / rate);
    qrx_vm_write(&c->vm, c->host_tb + 0x22, 4, (uint32_t)secs);  /* tb_CurrentTime.tv_secs */
    qrx_vm_write(&c->vm, c->host_tb + 0x26, 4, micro);           /* tb_CurrentTime.tv_micro */
    qrx_vm_write(&c->vm, c->host_tb + 0x2A, 4, (uint32_t)secs);  /* tb_Elapsed.tv_secs */
    qrx_vm_write(&c->vm, c->host_tb + 0x2E, 4, micro);           /* tb_Elapsed.tv_micro */
    c->host_tb_sec = secs;
    if (!c->host_tb_dumped) { /* one-shot TimerBase map at first host tick */
        c->host_tb_dumped = 1;
        uint32_t v[8]; const char *nm[8] = {
            "vbcount.secs@EA", "vbcount.frames@EE", "vbl_rate@F2", "vbl_micros@F4",
            "vblank_on@F6", "micro_on@D2", "lists[1]head@42", "lists[1]tail@46" };
        const uint32_t off[8] = { 0xEA, 0xEE, 0xF2, 0xF4, 0xF6, 0xD2, 0x42, 0x46 };
        for (unsigned i = 0; i < 8; i++) {
            unsigned w = (i == 6 || i == 7) ? 4u : ((i == 2 || i == 3 || i == 4) ? 2u : 4u);
            unsigned byte = (unsigned)(off[i]);
            uint32_t val = 0;
            if (qrx_full_bus_read(c, c->host_tb + byte, w, &val))
                fprintf(stderr, "[TBMAP] %s=%0*x\n", nm[i], w*2, val);
        }
    }
    { /* EXPERIMENTAL advance of tb_vb_count (v55 measured tb+0xF2 = the
       * PAL vblank rate 50, tb+0xF4 = frame micros, tb+0xEE = frame
       * counter) — layout: tv_secs@+0xEA, tv_micro@+0xEE (frames).
       * Default OFF; QRX_FULL_HOST_VB_COUNT=1 re-enables. */
      uint32_t vrate = 0, vmicros = 0, bsecs = 0, bmicro = 0, acc;
      static unsigned vbskip;
      int ok = c->host_vb_enable &&
          qrx_full_bus_read(c, c->host_tb + 0xF2, 2, &vrate) && vrate &&
          qrx_full_bus_read(c, c->host_tb + 0xF4, 2, &vmicros) && vmicros &&
          qrx_full_bus_read(c, c->host_tb + 0xEA, 4, &bsecs) &&
          qrx_full_bus_read(c, c->host_tb + 0xEE, 4, &bmicro);
      if (vbskip < 8 && env_QRX_FULL_HOST_E_CLOCK_TRACE) {
          vbskip++;
          fprintf(stderr, "[VBADV] reads=ok=%d vrate=%u vmicros=%u bmicro=%u instr=%llu\n",
              ok, vrate, vmicros, bmicro, (unsigned long long)c->instructions);
      }
      if (ok) {
          acc = c->host_vb_acc + (uint32_t)(((uint64_t)65536u * 1000000u / c->host_tb_rate) * wraps);
          { static unsigned ntick;
            if (ntick < 12 && env_QRX_FULL_HOST_E_CLOCK_TRACE) {
                ntick++;
                fprintf(stderr, "[VBADV] tick n=%u prevacc=%u sum=%u rv=%u/%u wraps=%u bmicro=%u\n",
                    ntick, c->host_vb_acc, acc, vrate, vmicros, wraps, bmicro);
            } }
          { unsigned nframes = 0;
            while (acc >= vmicros) { acc -= vmicros; bmicro++; nframes++; if (!bmicro) bsecs++; }
            if (nframes > 0 && env_QRX_FULL_HOST_E_CLOCK_TRACE)
                fprintf(stderr, "[VBADV] +%u frames now=%u instr=%llu\n", nframes,
                    bmicro, (unsigned long long)c->instructions); }
          c->host_vb_acc = acc;
          qrx_vm_write(&c->vm, c->host_tb + 0xEA, 4, bsecs);
          qrx_vm_write(&c->vm, c->host_tb + 0xEE, 4, bmicro);
          if (env_QRX_FULL_HOST_E_CLOCK_TRACE) {
              uint32_t rb = 0;
              qrx_full_bus_read(c, c->host_tb + 0xEE, 4, &rb);
              fprintf(stderr, "[VBADV] wb bsecs+bmicro=%u/%u readback=%u instr=%llu\n",
                  bsecs, bmicro, rb, (unsigned long long)c->instructions);
          }
      }
    }
    if (env_QRX_FULL_HOST_E_CLOCK_TRACE)
        fprintf(stderr, "[HCLOCK] sec=%llu micro=%u instr=%llu\n",
            (unsigned long long)secs, micro,
            (unsigned long long)c->instructions);
}

/* CIA-A Timer A: underflow every (latch==0 ? 65536 : latch) E-clocks while
 * the guest has the start bit set; fires flag 1 and permits level 2. */
static void cia_a_tick(QRXFullCPU *c) {
    if (c->aux_cpu_index != 0 || !amiga_bus(c)) return;
    if (!(c->cia_cra & 1u)) { c->cia_ref = c->instructions; return; }
    uint64_t latch = c->cia_ta_latch ? (uint64_t)c->cia_ta_latch : 65536u;
    uint64_t units = (c->instructions - c->cia_ref) / QRX_CIA_DIV;
    uint64_t wraps = units / latch;
    if (!wraps) return;
    c->cia_ref += wraps * latch * QRX_CIA_DIV;
    if (c->cia_icr_trace)
        fprintf(stderr, "[ICR] TimerA underflow wraps=%llu instr=%llu flags=%02x mask=%02x\n",
            (unsigned long long)wraps, (unsigned long long)c->instructions,
            (unsigned)((c->cia_icr | 1u) & 0x1fu), (unsigned)c->cia_icr_mask);
    c->cia_icr |= 1u;
    /* AROS cia.resource hooks the CIA INTs on the PORTS line via
     * AddIntServer(INTB_PORTS, ...) (cia_init.c:163): assert the Paula
     * PORTS request so the exec level-2 chain runs the CIA servers. */
    c->ports_asserted = 1;
    if (c->cia_cra & 8u) c->cia_cra &= (uint8_t)~1u; /* one-shot: hold until reloaded */
    paula_recompute(c);
}

/* CIA-A Timer B: mirrors the Timer A model (latch, CRB start bit, TBHI
 * reload auto-start) and raises the CIA-A ICR timer-B flag (bit 1). The
 * AROS boot tasks arm ICR 0x01/0x02/0x08 and sleep; without the Timer B
 * underflow they never see a second tick and the ready-queue stays empty. */
static void cia_b_tick(QRXFullCPU *c) {
    if (c->aux_cpu_index != 0 || !amiga_bus(c)) return;
    if (!(c->cia_crb & 1u)) { c->cia_tb_ref = c->instructions; return; }
    uint64_t latch = c->cia_tb_latch ? (uint64_t)c->cia_tb_latch : 65536u;
    uint64_t units = (c->instructions - c->cia_tb_ref) / QRX_CIA_DIV;
    uint64_t wraps = units / latch;
    if (!wraps) return;
    c->cia_tb_ref += wraps * latch * QRX_CIA_DIV;
    if (c->cia_icr_trace)
        fprintf(stderr, "[ICR] TimerB underflow wraps=%llu instr=%llu flags=%02x mask=%02x\n",
            (unsigned long long)wraps, (unsigned long long)c->instructions,
            (unsigned)((c->cia_icr | 2u) & 0x1fu), (unsigned)c->cia_icr_mask);
    c->cia_icr |= 2u;
    c->ports_asserted = 1; /* PORTS line: AddIntServer(INTB_PORTS) path */
    if (c->cia_crb & 8u) c->cia_crb &= (uint8_t)~1u; /* one-shot */
    host_eclock_tick(c, (unsigned)wraps);
    paula_recompute(c);
}

/* CIA-B Timer B: same model as CIA-A Timer A but with the CIA-B ICR flag
 * (bit 1). AROS's soft trackdisk timing and the ROM CIA interrupt server
 * both re-arm through the real TBLO/TBHI ($BFDD09/0B) window; without it
 * the stuck-kick still has to fake interrupts in guest structures. */
static void ciab_tb_tick(QRXFullCPU *c) {
    if (c->aux_cpu_index != 0 || !amiga_bus(c)) return;
    if (!(c->ciab_crb & 1u)) { c->ciab_tb_ref = c->instructions; return; }
    uint64_t latch = c->ciab_tb_latch ? (uint64_t)c->ciab_tb_latch : 65536u;
    uint64_t units = (c->instructions - c->ciab_tb_ref) / QRX_CIA_DIV;
    uint64_t wraps = units / latch;
    if (!wraps) return;
    c->ciab_tb_ref += wraps * latch * QRX_CIA_DIV;
    c->ciab_icr |= 2u; /* CIA-B ICR timer-B flag */
    c->ports_asserted = 1; /* PORTS line: AddIntServer(INTB_PORTS) path */
    if (c->ciab_crb & 8u) c->ciab_crb &= (uint8_t)~1u; /* one-shot */
    if (c->ciab_icr_trace)
        fprintf(stderr, "[CIAB] TimerB underflow wraps=%llu instr=%llu flags=%02x mask=%02x\n",
            (unsigned long long)wraps, (unsigned long long)c->instructions,
            (unsigned)c->ciab_icr, (unsigned)c->ciab_icr_mask);
    paula_recompute(c);
}

/* Amiga INTREQ semantics: a write with a bit set clears it. */
static void paula_recompute(QRXFullCPU *c) {
    uint16_t req=c->boot_intreq;
    if(c->vblank_asserted)req|=0x20;
    if(c->dskblk_asserted)req|=2;
    unsigned level=0;
    uint16_t pending=req&c->intena;
    if(c->intena&0x4000){
        if(pending&0x0007)level=1;
        if(pending&0x0008)level=2;
        if(pending&0x0070)level=3;
        if(pending&0x0780)level=4;
        if(pending&0x1800)level=5;
        if(pending&0x2000)level=6;
    }
    if(c->irq_pending && level<2)level=2;
    c->irq_level=level;
    if(active==c)m68k_set_irq(level);
}

/* ADFS MFM encoder: port of AROS arch/m68k-amiga/devs/trackdisk
 * td_encodebuffer_ados so the guest decode traffic stays byte-exact.
 * Sectors: 11 (DD ADF) or 22 (HD ADF). Checksums match the decoder. */
static void mfmcode(uint32_t *mfm, uint16_t longs) {
    uint32_t prev = mfm[-1];
    while (longs--) {
        uint32_t v = *mfm & 0x55555555u;
        uint32_t mask1 = (prev << 31) | (v >> 1);
        uint32_t mask2 = v << 1;
        uint32_t mfmbits = ((~mask1) & (~mask2)) & 0xaaaaaaaau;
        prev = v;
        v |= mfmbits;
        *mfm++ = v;
    }
}
static void fl_encode_track(QRXFullCPU *c);
static uint16_t raw_dmac(const QRXFullCPU *c) {
    uint16_t v = c->dmacon_shadow;
    if (c->dsk_active) v |= 0x0010u;
    return v;
}
static void fl_encode_track(QRXFullCPU *c) {
    const uint8_t *databuf;
    uint32_t track, sectors, stride, bufsize, gapsize, i, sec;
    uint32_t *mfmbuf = (uint32_t *)(void *)c->mfm_cache;
    /* Derive the layout from the guests DMA word count: 6815 = DD, 13630 = HD. */
    uint16_t words = c->dsk_len & QRX_FL_WORD_MASK;
    bool hddisk = words >= QRX_FL_HDDISK_WORDS;
    if (!c->adf_data || !c->adf_size) { c->mfm_track = -1; return; }
    sectors = hddisk ? QRX_FL_HDDISK_SECTORS : QRX_FL_DDDISK_SECTORS;
    stride  = hddisk ? QRX_FL_HDDISK_SECTORS : QRX_FL_DDDISK_SECTORS;
    bufsize = 2u * (hddisk ? 13630u : 6815u);
    track = (uint32_t)c->fl_cyl[0] * 2u +
        ((c->ciab_prb >> 2) & 1u ? 0u : 1u); /* SIDE: 0 = lower, 1 = upper */
    if (track * stride * 512u >= c->adf_size) { c->mfm_track = -1; return; }
    if (c->mfm_track == (int32_t)track) return; /* cache hit */
    databuf = c->adf_data + track * stride * 512u;
    gapsize = bufsize - sectors * 2u * 544u;
    for (i = 0; i < gapsize / 4u - 1u; i++) *mfmbuf++ = 0xaaaaaaaau;
    for (sec = 0; sec < sectors; sec++) {
        uint32_t deven, dodd, hck = 0, dck = 0, d;
        if (mfmbuf[-1] & 1u) mfmbuf[0] = 0x2aaaaaaau; else mfmbuf[0] = 0xaaaaaaaau;
        mfmbuf[1] = 0x44894489u;
        deven = (0xffu << 24) | (track << 16) | (sec << 8) | ((sectors - sec) & 0xffu);
        dodd = (deven >> 1) & 0x55555555u;
        deven &= 0x55555555u;
        mfmbuf[2] = dodd; mfmbuf[3] = deven;
        hck ^= dodd; hck ^= deven;
        for (i = 4; i < 12; i++) mfmbuf[i] = 0;
        databuf = c->adf_data + ((size_t)track * stride + sec) * 512u;
        for (i = 0; i < 128; i++) {
            d = ((uint32_t)databuf[i*4] << 24) | ((uint32_t)databuf[i*4+1] << 16) |
                ((uint32_t)databuf[i*4 + 2] << 8) | databuf[i*4 + 3];
            deven = d;
            dodd = (deven >> 1) & 0x55555555u;
            deven &= 0x55555555u;
            mfmbuf[0u + 16 + i] = dodd;
            mfmbuf[128u + 16u + i] = deven;
            dck ^= dodd; dck ^= deven;
        }
        deven = hck;
        dodd = deven >> 1;
        mfmbuf[12] = dodd; mfmbuf[13] = deven;
        deven = dck;
        dodd = deven >> 1;
        mfmbuf[14] = dodd; mfmbuf[15] = deven;
        mfmcode(mfmbuf + 2, (uint16_t)(2u * 128u + 16u - 2u));
        mfmbuf += 2u * 128u + 16u;
    }
    mfmbuf[0] = 0;
    mfmcode(mfmbuf, 1);
    c->mfm_track = (int32_t)track;
    for(unsigned j=0;j<bufsize;j+=4){
        uint32_t v;memcpy(&v,c->mfm_cache+j,4);
        c->mfm_cache[j]=v>>24;c->mfm_cache[j+1]=v>>16;
        c->mfm_cache[j+2]=v>>8;c->mfm_cache[j+3]=v;
    }

}

static void fl_dma_progress(QRXFullCPU *c) {
    if (!c->dsk_active || !c->adf_data) return;
    uint32_t words = c->dsk_len & QRX_FL_WORD_MASK;
    uint64_t need = (uint64_t)words * 8u; /* 8 cycles per MFM word */
    if (c->cycles - c->dsk_start < need) return;
    uint32_t addr = c->dsk_pt & ~1u;
    uint32_t transferred = 0, start=0;
    unsigned trackbytes=(words>=QRX_FL_HDDISK_WORDS)?27260:13630;
    if(c->adkcon_shadow&0x400){
        /* WORDSYNC: first matching sync is consumed, subsequent words DMA. */
        for(unsigned i=0;i+1<trackbytes;i+=2)
            if(c->mfm_cache[i]==0x44 && c->mfm_cache[i+1]==0x89){start=i+2;break;}
    }
    while (transferred < words && transferred * 2u < sizeof(c->mfm_cache) &&
           addr + 1u < 0x01000000u) {
        unsigned off=(start+transferred*2)%trackbytes;
        uint16_t w = (uint16_t)((c->mfm_cache[off] << 8) | c->mfm_cache[off+1]);
        qrx_full_bus_write(c, addr, 2, w);
        addr += 2; transferred++;
    }
    c->dsk_active = 0;
    c->dskblk_asserted = 1u; /* DSKBLK: block complete */
    paula_recompute(c);
}

/* disk.resource on this AROS ROM has no ID read: make DF0 look like a 150-RPM
 * HD drive so the guest trackdisk driver enumerates the correct HD layout.
 * Opt-in: the guest ROM owns the resource list; no silent patch by default. */
static void fl_hdpoke(QRXFullCPU *c) {
    static int done;
    if (!done) {
        const char *e = env_QRX_FULL_FL_HDPATCH;
        done = (!e || !strcmp(e, "0")) ? -1 : 1;
    }
    if (done < 0 || done == 1 || !c->adf_data || c->adf_size != 1802240u) return;
    /* Locate disk.resource's ROM name string, then the RAM node whose name
     * pointer points there (m68k Node: ln_Name at +10; ln_Type at +8). */
    static const char *cands[] = { "disk.resource\0", NULL };
    const uint8_t *s = c->rom_main;
    uint32_t strp = 0xDEADBEEFu;
    if (s) {
        const char *nm = cands[0];
        size_t len = strlen(nm);
        for (size_t i = 0; i + len + 1 < c->rom_main_size; i++) {        if (memcmp(s + i, nm, len + 1) == 0) { strp = 0x00F80000u + (uint32_t)i; break; }
        }
    }
    if (strp == 0xDEADBEEFu) return;
    for (uint32_t a = 0x00C00000u; a + 24u < 0x00DC0000u; a += 2u) {
        uint32_t v = 0;
        if (!qrx_full_bus_read(c, a, 4, &v) || v != strp) continue;
        uint32_t node = a - 10u;
        uint32_t t = 0;
        if (!qrx_full_bus_read(c, node + 8u, 1, &t) || (t & 0xFFu) != 0x02u) continue; /* NT_LIBRARY */
        /* dr_UnitID[0] lives near the end of the Library header in RAM: write
         * both plausible offsets to reach the real one on either layout. */
        (void)qrx_full_bus_write(c, node + 40u, 4, 0xAAAAAAAAu);
        (void)qrx_full_bus_write(c, node + 52u, 4, 0xAAAAAAAAu);
        fprintf(stderr, "[FL] HD identification written at DiskBase %08x (name %08x)\n", node, strp);
        return;
    }
}
static bool board_hole(const QRXFullCPU *c, uint32_t addr, unsigned width) {
    uint64_t e = (uint64_t)addr + width;
    /* No Gayle is advertised on this board. AROS owns the entire detected
     * C00000..DBFFFF slow RAM bank, including DA8000..DAFFFF. Do not
     * turn live allocator chunks in that bank into floating device reads. */
    if ((uint64_t)addr >= 0x00E80000u && e <= 0x00F00000u) return true; /* autoconfig, no boards */
    /* Zorro-III config header is nibble-read as bytes (ReadExpansionByte): only
     * those two byte lanes float. All long QRX registers above stay exact. */
    if (amiga_bus(c) && width == 1 &&
        (((uint64_t)addr >= 0xFF000000u && e <= 0xFF000080u) ||
         ((uint64_t)addr >= 0xFF000100u && e <= 0xFF000180u))) return true;
    return false;
}
#define QRX_FSAGA_BOARD_ID_V4 UINT32_C(0x0500) /* VREG_BOARD read: high byte 5 */
/* FSAGA register windows: the Apollo "SuperAGA" set, verbatim SAGA semantics.
 * No Denise/Agnus chipset is emulated; only the RTG door the AROS
 * `sagagfx.hidd` driver exercises. */
/* Present the FSAGA framebuffer to the host on every visible vblank. The RTG
 * driver composes the guest buffer in guest RAM, so the host needs a pointer,
 * size and format; the host does the 2-to-surface conversion itself. */
static bool fsaga_mode_displayable(uint16_t mode);
static void fsaga_present(QRXFullCPU *c) {
    uint32_t w = c->fsaga_width, h = c->fsaga_height;
    uint32_t fmt = c->fsaga_mode & 0x7Fu;
    if (!c->fsaga_bplptr || !fsaga_mode_displayable(c->fsaga_mode)) return;
    if (!w || !h) return;
    unsigned bpp = (fmt == QRX_FSAGA_FMT_CLUT8) ? 1u :
                   (fmt == QRX_FSAGA_FMT_RGB16 || fmt == QRX_FSAGA_FMT_RGB15) ? 2u :
                   (fmt == QRX_FSAGA_FMT_RGB24) ? 3u : 4u;
    /* Skip the BUS
ightarrow for secret data that is not actually inside the
     * guest RAM linearly; nothing else can be displayed anyway. */
    if ((uint64_t)c->fsaga_bplptr + (uint64_t)w * h * bpp > c->vm.ram.size) {
        static uint8_t warned;
        if (!warned) {
            warned = 1u;
            fprintf(stderr, "[FSAGA] fb=%08x +%u outside guest RAM; skipped\n",
                c->fsaga_bplptr, (unsigned)(w * h * bpp));
        }
        return;
    }
    c->fsaga_frame++;
    if (c->fsaga_trace)
        fprintf(stderr, "[FSAGA] frame=%llu mode=%04x fb=%08x %ux%u\n",
            (unsigned long long)c->fsaga_frame, c->fsaga_mode,
            c->fsaga_bplptr, w, h);
    if (c->display)
        c->display(c->display_arg, c, c->fsaga_frame, c->fsaga_bplptr, w, h, fmt);
}
static bool fsaga_mode_displayable(uint16_t mode) {
    uint32_t fmt = mode & 0x7Fu;
    return fmt == QRX_FSAGA_FMT_CLUT8 || fmt == QRX_FSAGA_FMT_RGB16 ||
           fmt == QRX_FSAGA_FMT_RGB15 || fmt == QRX_FSAGA_FMT_RGB24 ||
           fmt == QRX_FSAGA_FMT_RGB32;
}
static bool fsaga_sync_reg(uint32_t addr) {
    return addr >= 0x00DFF300u && addr <= 0x00DFF311u;
}
static bool fsaga_clut_addr(uint32_t addr) {
    return addr >= 0x00DFF400u && addr < 0x00DFF800u && (addr & 3u) == 0;
}
static bool fsaga_sprite_addr(uint32_t addr) {
    return addr >= 0x00DFF800u && addr < 0x00DFF840u && (addr & 3u) == 0;
}
/* Any access inside the RTG-mailbox window that is not hardware-trapped would
 * otherwise fall through to guest RAM reads of emulated chip RAM, which fake
 * success forever. The FSAGA window is host-owned, not RAM. */
static bool fsaga_window(uint32_t addr);
static bool fsaga_read(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t *out) {
    uint64_t e = (uint64_t)addr + width;
    if (addr >= 0x00DFF1D0u && e <= 0x00DFF1F6u && (width == 2 || width == 4)) {
        switch (addr) {
            case 0x00DFF1D0u: if (width==2){*out=c->fsaga_spritex;return true;} break;
            case 0x00DFF1D2u: if (width==2){*out=c->fsaga_spritey;return true;} break;
            case 0x00DFF1E6u: /* BPLHMOD */
                if (width==2){*out=c->fsaga_bplhmod;return true;} break;
            case 0x00DFF1ECu: /* BPLPTR hi half */
                if (width==2){*out=(c->fsaga_bplptr>>16)&0xFFFFu;return true;} break;
            case 0x00DFF1EEu: /* BPLPTR lo half */
                if (width==2){*out=c->fsaga_bplptr&0xFFFFu;return true;} break;
            case 0x00DFF1F4u: /* VIDEO_MODE */
                if (width==2){*out=c->fsaga_mode;return true;} break;
            default: break;
        }
    }
    if (fsaga_sync_reg(addr) && width == 2) {
        uint16_t *v = NULL;
        switch (addr - 0x00DFF300u) {
            case 0: v=&c->fsaga_hpixel; break; case 2: v=&c->fsaga_hsstrt; break;
            case 4: v=&c->fsaga_hsstop; break; case 6: v=&c->fsaga_htotal; break;
            case 8: v=&c->fsaga_vpixel; break; case 10: v=&c->fsaga_vsstrt; break;
            case 12: v=&c->fsaga_vsstop; break; case 14: v=&c->fsaga_vtotal; break;
            case 16: v=&c->fsaga_hvsync; break; default: break;
        }
        if (v) { *out=*v; return true; }
    }
    if (addr == 0x00DFF3F8u && width >= 2) { *out = c->fsaga_pllv4; return true; }
    if (addr == 0x00DFF3FCu) {
        /* VREG_BOARD: BFE0-style board ID word, big-endian. */
        if (width == 1) { *out = (addr == 0x00DFF3FCu ? 0x05u : 0x00u); return true; }
        *out = QRX_FSAGA_BOARD_ID_V4; return true;
    }
    if (addr >= 0x00DFF3A0u && addr <= 0x00DFF3A9u && width == 2) {
        unsigned i = (unsigned)((addr - 0x00DFF3A0u));
        if (i & 1u) { /* odd offset is not a register */ }
        else { *out = c->fsaga_spriteclut[i>>1]; return true; }
    }
    if (fsaga_clut_addr(addr) && width == 4) {
        uint32_t idx = (addr - 0x00DFF400u) >> 2;
        if (idx < 256) { *out = c->fsaga_clut[idx]; return true; }
    }
    if (fsaga_sprite_addr(addr) && width == 4) {
        uint32_t idx = (addr - 0x00DFF800u) >> 2;
        const uint8_t *row = c->fsaga_cursor[idx & 15u];
        *out = ((uint32_t)row[0] << 24) | ((uint32_t)row[1] << 16) |
               ((uint32_t)row[2] << 8) | row[3];
        return true;
    }
    return false;
}
static void fsaga_recompute_dims(QRXFullCPU *c) {
    /* Apollo semantics: HPIXEL/VPIXEL carry the DISPLAY size; DBLSCAN_X/Y
     * halves the *framebuffer* in that axis (fb stride is one of the planes
     * being doubled by the video engine). Our host view mirrors the display. */
    uint32_t w = c->fsaga_hpixel, h = c->fsaga_vpixel;
    if (!w || w > 4096u || !h || h > 4096u) { c->fsaga_width=0; c->fsaga_height=0; return; }
    c->fsaga_width = w;
    c->fsaga_height = h;
}
static void fsaga_write(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t value) {
    uint64_t e = (uint64_t)addr + width;
    (void)e;
    uint8_t trace = c->fsaga_trace;
    if (!fsaga_window(addr) && !(addr >= 0x00DFF1D0u && addr <= 0x00DFF1F6u)) return;
    /* BPLPTR: WRITE32 at $DFF1EC, or two WRITE16 halves. */
    if (addr == 0x00DFF1ECu && width == 4) {
        c->fsaga_bplptr = value;
        if (trace) fprintf(stderr, "[FSAGA] bplptr=%08x instr=%llu\n",
            value, (unsigned long long)c->instructions);
    } else if ((addr == 0x00DFF1ECu || addr == 0x00DFF1EEu) && width == 2) {
        uint32_t old = c->fsaga_bplptr;
        uint32_t nw = (addr == 0x00DFF1ECu) ?
            ((value & 0xFFFFu) << 16) | (old & 0xFFFFu) :
            (old & 0xFFFF0000u) | (value & 0xFFFFu);
        c->fsaga_bplptr = nw;
        if (trace) fprintf(stderr, "[FSAGA] bplptr16=%08x -> %08x instr=%llu\n",
            (unsigned)value, nw, (unsigned long long)c->instructions);
    }
    if (addr == 0x00DFF1E6u && width == 2) { c->fsaga_bplhmod = (uint16_t)value; }
    if (addr == 0x00DFF1F4u && width == 2) {
        uint16_t old = c->fsaga_mode;
        c->fsaga_mode = (uint16_t)value;
        /* AROS SAGA V4 selects a fixed mode; its SetModeline is a no-op. */
        static const uint16_t modes[][2]={{0,0},{320,200},{320,240},{320,256},
            {640,400},{640,480},{640,512},{960,540},{480,270},{304,224},
            {1280,720},{640,360},{800,600},{1024,768},{720,576},{848,480},
            {640,200},{1920,1080},{1280,1024},{1280,800},{1440,900}};
        unsigned id=value>>8;
        if(id && id<sizeof modes/sizeof modes[0]){
            c->fsaga_width=modes[id][0];c->fsaga_height=modes[id][1];
        }else fsaga_recompute_dims(c);
        if (trace)
            fprintf(stderr, "[FSAGA] mode %04x -> %04x fmt=%u modeid=%u instr=%llu\n",
                old, c->fsaga_mode, c->fsaga_mode & 0x7Fu,
                (c->fsaga_mode >> 8) & 0x3Fu, (unsigned long long)c->instructions);
    }
    /* SPRITEX / SPRITEY (16-bit each). */
    if (addr == 0x00DFF1D0u && width == 2) c->fsaga_spritex = (uint16_t)value;
    if (addr == 0x00DFF1D2u && width == 2) c->fsaga_spritey = (uint16_t)value;
    if (addr >= 0x00DFF3A0u && addr <= 0x00DFF3A9u && width == 2) {
        unsigned i = (unsigned)(addr - 0x00DFF3A0u);
        if (!(i & 1u)) c->fsaga_spriteclut[i>>1] = (uint16_t)value;
    }
    if (fsaga_sync_reg(addr) && width == 2) {
        uint16_t v = (uint16_t)value;
        switch (addr - 0x00DFF300u) {
            case 0: c->fsaga_hpixel=v; break; case 2: c->fsaga_hsstrt=v; break;
            case 4: c->fsaga_hsstop=v; break; case 6: c->fsaga_htotal=v; break;
            case 8: c->fsaga_vpixel=v; break; case 10: c->fsaga_vsstrt=v; break;
            case 12: c->fsaga_vsstop=v; break; case 14: c->fsaga_vtotal=v; break;
            case 16: c->fsaga_hvsync=v; break; default: break;
        }
        fsaga_recompute_dims(c);
        if (trace && (addr == 0x00DFF300u /* HPIXEL */ || addr == 0x00DFF308u /* VPIXEL */))
            fprintf(stderr, "[FSAGA] dim %ux%u instr=%llu\n",
                c->fsaga_hpixel, c->fsaga_vpixel, (unsigned long long)c->instructions);
    }
    /* Apollo SetPLL writes SAGA_VIDEO_PLLV4 as one 32-bit word. */
    if (addr == 0x00DFF3F8u && width >= 2) {
        c->fsaga_pllv4 = value;
        if (trace)
            fprintf(stderr, "[FSAGA] pllv4=%08x instr=%llu\n",
                value, (unsigned long long)c->instructions);
    }
    /* 256-entry RGB CLUT at $DFF400..$DFF7FF, 4 bytes per entry.
     * Apollo driver palette semantics: one 32-bit big-endian value per entry,
     * no "valid" bit (that bit is not part of the SAGA palette format). */
    if (fsaga_clut_addr(addr) && width == 4) {
        uint32_t idx = (addr - 0x00DFF400u) >> 2;
        if (idx < 256) c->fsaga_clut[idx] = value | UINT32_C(0xFF000000);
    }
    /* 16 rows of cursor bitmask (2 planes/row) at $DFF800..$DFF83F. */
    if (fsaga_sprite_addr(addr) && width == 4) {
        uint32_t idx = (addr - 0x00DFF800u) >> 2;
        if (idx < 16) {
            c->fsaga_cursor[idx][0]=(uint8_t)(value>>24);
            c->fsaga_cursor[idx][1]=(uint8_t)(value>>16);
            c->fsaga_cursor[idx][2]=(uint8_t)(value>>8);
            c->fsaga_cursor[idx][3]=(uint8_t)value;
        }
    }
}
/* Any FSAGA register access inside the RTG mailbox windows that is not
 * hardware-trapped here would otherwise fall through to guest RAM reads of
 * emulated chip RAM, which fake success forever. The FSAGA window is
 * host-owned, not RAM. */
static bool fsaga_window(uint32_t addr) {
    if (addr >= 0x00DFF300u && addr < 0x00DFF340u) return true;   /* sync */
    if (addr >= 0x00DFF3A0u && addr < 0x00DFF3B0u) return true;   /* sprite pal */
    if (addr >= 0x00DFF3F0u && addr < 0x00DFF410u) return true;   /* pll + board */
    if (addr >= 0x00DFF400u && addr < 0x00DFF840u) return true;   /* clut+sprite */
    return false;
}
static bool amiga_trap_read(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t *out) {
    uint64_t e = (uint64_t)addr + width;
    if (amiga_cia_read(c,addr,width,out)) return true;
    if (addr==0xdff016 && width==2){*out=0xffff;return true;}
    if (fsaga_read(c, addr, width, out)) return true;
    if (fsaga_window(addr)) { *out = 0; return true; }
    if ((uint64_t)addr >= 0x00DFF000u && e <= 0x00DFF200u) {
        if (addr == 0x00DFF018u && width == 2) { *out = 0x3000u; return true; } /* SERDATR: TBE|TSRE */
        if (addr == 0x00DFF01Cu && width == 2) { *out = c->intena; return true; } /* INTENAR */
        if (addr >= 0x00DFF01Cu && e <= 0x00DFF01Eu) {
            if (width == 1) { *out = beam_byte(c, addr, c->intena); return true; }
            if (width == 2 && addr == 0x00DFF01Cu) {
                if (env_QRX_FULL_VBLANK_TRACE)
                    fprintf(stderr, "[INTENAR] r=%04x instr=%llu\n", c->intena, (unsigned long long)c->instructions);
                *out = c->intena; return true; /* INTENAR */
            }
        }
        if ((addr >= 0x00DFF01Eu && e <= 0x00DFF020u) && width <= 4) {
            /* INTREQR: VERTB (0x20), DSKBLK (0x02) and PORTS (0x08) live.
             * $DFF01C additionally reports bit14 while a level>=2 pin is
             * in flight: the ROM's exec level-2 handler ($FE6204) gates its
             * CIA branch on that bit. */
            uint32_t v = c->boot_intreq | (c->vblank_asserted ? 0x0020u : 0u) |
                         (c->dskblk_asserted ? 0x0002u : 0u) |
                         (c->ports_asserted ? 0x0008u : 0u);
            if (addr == 0x00DFF01Cu) v |= (c->irq_level >= 2 ? 0x4000u : 0u);
            if (width == 1) { *out = beam_byte(c, addr, (uint16_t)v); return true; }
            if (width == 2 || addr == 0x00DFF01Eu) {
                if (env_QRX_FULL_VBLANK_TRACE)
                    fprintf(stderr, "[INTREQR] r=%04x instr=%llu\n", v, (unsigned long long)c->instructions);
                *out = v; return true;
            }
        }
        if (addr == 0x00DFF002u && width == 2) { *out = raw_dmac(c); return true; }
        if (addr == 0x00DFF020u && width == 4) { *out = c->dsk_pt; return true; } /* DSKPT */
        if (addr == 0x00DFF024u && width == 2) { *out = c->dsk_len; return true; } /* DSKLEN */
        if ((addr == 0x00DFF004u || addr == 0x00DFF005u) && width <= 2) {
            uint16_t v = beam_vposr(c); /* VPOSR */
            *out = width == 1 ? beam_byte(c, addr, v) : v;
            return true;
        }
        if ((addr == 0x00DFF006u || addr == 0x00DFF007u) && width <= 2) {
            uint16_t v = beam_vhposr(c); /* VHPOSR */
            *out = width == 1 ? beam_byte(c, addr, v) : v;
            return true;
        }
        *out = 0; return true;
    }
    /* Virtual floppy + CIA-B hardware window: the CIA-B base $BFDD00 sits
     * BELOW the old CIA-A trap start, so the window begins there and keeps
     * every legacy $BFE0xx alias. */
    if ((uint64_t)addr >= 0x00BFDD00u && e <= 0x00C00000u) {
        if (addr == QRX_CIAB_ICR && width <= 2) {
            uint32_t v = ((uint32_t)c->ciab_icr_mask << 8) | c->ciab_icr;
            if (c->ciab_icr_trace)
                fprintf(stderr, "[CIAB] ICR-R a=%08x w=%u v=%08x instr=%llu\n",
                    addr, width, v, (unsigned long long)c->instructions);
            c->ciab_icr = 0; /* ICR read clears the sticky flags */
            *out = 0xFFFFFF00u | v;
            paula_recompute(c);
            return true;
        }
        if (addr == QRX_CIAB_PRB && width <= 2) { *out = 0xFFFFFF00u | c->ciab_prb; return true; }
        if ((addr == QRX_CIAB_TBLO || addr == QRX_CIAB_TBHI || addr == QRX_CIAB_CRB ||
             addr == QRX_CIAB_CRA) && width == 1) {
            uint64_t latch = c->ciab_tb_latch ? (uint64_t)c->ciab_tb_latch : 65536u;
            uint64_t el = (c->ciab_crb & 1u) ?
                ((c->instructions - c->ciab_tb_ref) / QRX_CIA_DIV) & 0xFFFFu : 0u;
            uint16_t cur = (uint16_t)(latch - (el & 0xFFFFu));
            if (addr == QRX_CIAB_TBLO) { *out = 0xFFFFFF00u | (cur & 0xFFu); return true; }
            if (addr == QRX_CIAB_TBHI) { *out = 0xFFFFFF00u | (cur >> 8); return true; }
            *out = 0xFFFFFF00u | (addr == QRX_CIAB_CRB ? c->ciab_crb : c->ciab_crb); return true;
        }
        if ((addr == QRX_CIA_ICR || addr == QRX_CIAA_ICR) && width <= 2) {
            uint32_t v = ((uint32_t)c->cia_icr_mask << 8) | c->cia_icr;
            if (c->cia_icr_trace)
                fprintf(stderr, "[ICR-R] a=%08x w=%u v=%08x instr=%llu\n",
                    addr, width, v, (unsigned long long)c->instructions);
            c->cia_icr = 0; /* ICR read clears the sticky flags */
            *out = 0xFFFFFF00u | v;
            paula_recompute(c);
            return true;
        }
        if (addr == 0x00BFE001u && width <= 2) {
            uint8_t pra = c->ciaa_pra;
            /* DF0PRA / DF1PRA sense bits are active-LOW: 0 = "drive present
             * with a disk in". A mounted ADF makes DF0 present; without this
             * the ROM coldstart polls a "no drive" PRA forever. */
            if (c->adf_data && c->adf_size) pra &= (uint8_t)~0x40u;
            int sel = -1;
            for (int u = 0; u < 4; u++) if (!(c->ciab_prb & (0x08u << u))) { sel = u; break; }
            if (sel >= 0) { pra = (uint8_t)((pra & ~0x10u) | (c->fl_cyl[sel] == 0 ? 0 : 0x10u)); }
            *out = 0xFFFFFF00u | pra; return true;
        }
        if (addr == 0x00BFD100u && width <= 2) { *out = 0xFFFFFF00u | c->ciab_prb; return true; }
        if (width == 1) {
            if (addr == QRX_CIA_TODLO || addr == QRX_CIAA_TODLO) { *out = cia_tick(c) & 0xFFu; return true; }
            if (addr == QRX_CIA_TODMI || addr == QRX_CIAA_TODMI) { *out = (cia_tick(c) >> 8) & 0xFFu; return true; }
            if (addr == QRX_CIA_TODHI || addr == QRX_CIAA_TODHI) { *out = (cia_tick(c) >> 16) & 0xFFu; return true; }
            if (addr == QRX_CIA_TAOFF || addr == QRX_CIAA_TALO) { *out = cia_ta_cur(c) & 0xFFu; return true; }
            if (addr == QRX_CIA_TAHI || addr == QRX_CIAA_TAHI) { *out = (cia_ta_cur(c) >> 8) & 0xFFu; return true; }
            if (addr == QRX_CIA_TBLO || addr == QRX_CIAA_TBLO) {
                uint64_t tl = c->cia_tb_latch ? (uint64_t)c->cia_tb_latch : 65536u;
                uint64_t te = (c->cia_crb & 1u) ?
                    ((c->instructions - c->cia_tb_ref) / QRX_CIA_DIV) & 0xFFFFu : 0u;
                *out = (((uint16_t)(tl - te)) & 0xFFu); return true;
            }
            if (addr == QRX_CIA_TBHI || addr == QRX_CIAA_TBHI) {
                uint64_t tl = c->cia_tb_latch ? (uint64_t)c->cia_tb_latch : 65536u;
                uint64_t te = (c->cia_crb & 1u) ?
                    ((c->instructions - c->cia_tb_ref) / QRX_CIA_DIV) & 0xFFFFu : 0u;
                *out = ((((uint16_t)(tl - te)) >> 8) & 0xFFu); return true;
            }
            if ((addr == QRX_CIA_CRB || addr == QRX_CIAA_CRB) && width == 1) { *out = c->cia_crb; return true; }
        }
        *out = 0xFFFFFFFFu; return true;
    }
    if ((uint64_t)addr >= 0x00F00000u && e <= 0x00F80000u) { *out = 0; return true; }
    if (board_hole(c, addr, width)) { *out = ones(width); return true; }
    return false;
}
static bool amiga_trap_write(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t value) {
    if(env_QRX_FULL_FL_TRACE && addr==0xdff024)fprintf(stderr,"[DSKLEN] %04x last=%04x width=%u\n",value,c->dsk_last,width);
    if(amiga_cia_write(c,addr,width,value))return true;
    if(addr==0xdff09c && width==2){
        if(value&0x8000)c->boot_intreq|=value&0x7fff;
        else c->boot_intreq&=~value;
        if(value&0x20)c->vblank_asserted=(value&0x8000)!=0;
        if(value&2)c->dskblk_asserted=(value&0x8000)!=0;
        paula_recompute(c);return true;
    }
    uint64_t e = (uint64_t)addr + width;
    if ((uint64_t)addr >= 0x00DFF1D0u && (uint64_t)addr < 0x00DFF840u) {
        fsaga_write(c, addr, width, value);
        return true; /* FSAGA RTG window: never falls through to guest RAM. */
    }
    if ((uint64_t)addr >= 0x00DFF000u && e <= 0x00DFF200u) {
        if (addr == 0x00DFF030u && width == 2 && c->vm.console)
            c->vm.console(c->vm.host_ctx, (uint8_t)(value & 0xFFu)); /* SERDAT debug channel */
        if (addr == 0x00DFF020u && width == 4) { c->dsk_pt = value; return true; } /* DSKPT */
        if (addr == 0x00DFF024u && width <= 2) {
            uint16_t v = (uint16_t)(width == 1 ? ((c->dsk_len & 0xFF00u) | (value & 0xFFu)) : value);
            if (v & QRX_FL_INIT_BIT) {
                if ((c->dsk_last & QRX_FL_INIT_BIT) &&
                    (c->dsk_last & QRX_FL_WORD_MASK) == (v & QRX_FL_WORD_MASK)) {
                    c->dsk_len = v;
                    fl_encode_track(c);
                    c->dsk_active = 1u;
                    c->dsk_start = c->cycles;
                    if (env_QRX_FULL_FL_TRACE)
                        fprintf(stderr, "[FL] DMA start pt=%08x words=%u track=%d cyl=%d\n",
                            c->dsk_pt, v & QRX_FL_WORD_MASK, c->mfm_track, c->fl_cyl[0]);
                }
            } else if (c->dsk_active) {
                c->dsk_active = 0; /* DSKDMAOFF / length change stops DMA */
            }
            c->dsk_len = v;
            c->dsk_last = v;
            return true;
        }
        if(addr==0xdff09e && width==2){
            if(value&0x8000)c->adkcon_shadow|=value&0x7fff;
            else c->adkcon_shadow&=~value;
            return true;
        }
        if (addr == 0x00DFF096u && width == 2) { /* DMACON */
            uint16_t v = (uint16_t)value;
            if (v & 0x8000u) c->dmacon_shadow |= (uint16_t)(v & 0x7FFFu);
            else c->dmacon_shadow &= (uint16_t)~v;
            if ((c->dmacon_shadow & 0x0010u) == 0 && c->dsk_active) c->dsk_active = 0;
            return true;
        }
        /* Early_Alert panic flash (aros boot/early.c writes COLOR00): TRAP_FAIL
         * = $000f, ALLOC_FAIL=$003c, EXEC_DISPATCH_FAIL=$002c. Without this
         * watch the boot fails with a black screen and no host-side log. */
        if (addr == 0x00DFF180u && width == 2) { /* COLOR00 */
            uint16_t col = (uint16_t)value;
            static uint16_t lastcol = 0xFFFFu; static int n;
            if (col != lastcol || (n % 200 == 0)) {
                fprintf(stderr, "[EARLY-ALERT] COLOR00=%04x PC=%08x instr=%llu (alert=%04x)\n",
                    col, m68k_get_reg(NULL, M68K_REG_PC),
                    (unsigned long long)c->instructions, col);
                lastcol = col;
            }
            n++;
        }
        if (addr == 0x00DFF09Au && width == 2) { /* INTENA: bit 15 = set/clear */
            if (env_QRX_FULL_VBLANK_TRACE)
                fprintf(stderr, "[INTENA] w=%04x -> %04x PC=%08x instr=%llu ctx=%c\n", value & 0xFFFFu,
                    (int)(uint16_t)((value & 0x8000u) ? (c->intena | (value & 0x7FFFu)) : (c->intena & ~(value & 0x7FFFu))),
                    (int)m68k_get_reg(NULL, M68K_REG_PC),
                    (unsigned long long)c->instructions, c==active ? 'G' : '?');
            if (value & 0x8000u) c->intena |= (uint16_t)(value & 0x7FFFu);
            else c->intena &= (uint16_t)~(value & 0x7FFFu);
        } else if (addr >= 0x00DFF09Au && e <= 0x00DFF09Eu && width <= 4) {
            /* INTENA (09A) + INTREQ (09C): accept byte lanes too, guests may
             * use bset/bclr style byte accesses on these registers. */
            uint16_t loa = 0, lob = 0;
            if (width == 1) {
                uint16_t lane = (uint16_t)(value & 0xFFu);
                if (!(addr & 1u)) lane <<= 8;
                if (addr < 0x00DFF09Cu) loa = lane;
                else lob = lane;
            } else if (width == 2) {
                if (addr == 0x00DFF09Au) loa = (uint16_t)value;
                else lob = (uint16_t)value;
            } else if (addr == 0x00DFF09Au) {
                loa = (uint16_t)(value >> 16); lob = (uint16_t)value;
            }
            if (loa) { /* INTENA: bit 15 = set/clear */
                if (loa & 0x8000u) c->intena |= (uint16_t)(loa & 0x7FFFu);
                else c->intena &= (uint16_t)~loa;
            }
            if (lob & 0x0020u) { /* INTREQ: VERTB ack */
                if (env_QRX_FULL_VBLANK_TRACE && c->vblank_asserted)
                    fprintf(stderr, "[VBLANK] ack instr=%llu\n", (unsigned long long)c->instructions);
                c->vblank_asserted = 0;
            }
            if (lob & 0x0002u) c->dskblk_asserted = 0;
            if (lob & 0x0008u) c->ports_asserted = 0;
            if (lob & 0x002Au) paula_recompute(c);
        }
        return true;
    }
    if ((uint64_t)addr >= 0x00BFDD00u && e <= 0x00C00000u) {
        if (addr == QRX_CIAB_ICR && width <= 2) {
            uint8_t v = (uint8_t)(value & 0x7Fu);
            if (value & 0x80u) { /* SET: mask + flag latch */
                c->ciab_icr_mask |= v;
                c->ciab_icr |= v;
            } else {
                c->ciab_icr_mask &= (uint8_t)~v;
                c->ciab_icr &= (uint8_t)~v;
            }
            if (c->ciab_icr_trace)
                fprintf(stderr, "[CIAB] ICR w=%02x mask=%02x flags=%02x instr=%llu\n",
                    (unsigned)v, (unsigned)c->ciab_icr_mask, (unsigned)c->ciab_icr,
                    (unsigned long long)c->instructions);
            paula_recompute(c);
            return true;
        }
        if (addr == QRX_CIAB_CRB && width == 1) {
            uint8_t was = c->ciab_crb;
            c->ciab_crb = (uint8_t)(value & 0xFFu);
            if ((c->ciab_crb & 1u) && !(was & 1u)) {
                c->ciab_tb_ref = c->instructions;
            }
            if (c->ciab_icr_trace)
                fprintf(stderr, "[CIAB] CRB w=%02x was=%02x instr=%llu\n",
                    (unsigned)c->ciab_crb, (unsigned)was, (unsigned long long)c->instructions);
            return true;
        }
        if (addr == QRX_CIAB_TBLO && width == 1) {
            c->ciab_tb_latch = (uint16_t)((c->ciab_tb_latch & 0xFF00u) | (value & 0xFFu));
            return true;
        }
        if (addr == QRX_CIAB_TBHI && width == 1) {
            /* AROS lowlevel.c: reloading the latch auto-starts the timer. */
            c->ciab_tb_latch = (uint16_t)((c->ciab_tb_latch & 0x00FFu) | ((value & 0xFFu) << 8));
            c->ciab_tb_ref = c->instructions;
            c->ciab_crb |= 1u;
            if (c->ciab_icr_trace)
                fprintf(stderr, "[CIAB] TBHI w=%02x latch=%04x auto-start instr=%llu\n",
                    (unsigned)(value & 0xFFu), (unsigned)c->ciab_tb_latch,
                    (unsigned long long)c->instructions);
            return true;
        }
        if ((addr == QRX_CIAB_PRB || addr == 0x00BFD100u) && width <= 2) {
            /* CIA-B PRB $BFDD01 (plus the legacy $BFD100 test alias): STEP
             * rising edge (bit 0 low->high) moves the selected head. DIR
             * bit 1 set = inward (toward the spindle), clear = outward. */
            uint8_t old = c->ciab_prb;
            uint8_t nw = (uint8_t)(value & 0xFFu);
            if (c->ciab_icr_trace)
                fprintf(stderr, "[CIAB] PRB w=%02x was=%02x instr=%llu\n",
                    (unsigned)nw, (unsigned)old, (unsigned long long)c->instructions);
            if (!(old & 1u) && (nw & 1u)) {
                for (int u = 0; u < 4; u++) if (!(nw & (0x08u << u))) {
                    if (nw & 2u) { if (c->fl_cyl[u] > 0) c->fl_cyl[u]--; }
                    else if (c->fl_cyl[u] < 85) c->fl_cyl[u]++;
                }
            }
            c->ciab_prb = nw;
            return true;
        }
        if (addr == 0x00BFE001u && width <= 2) {
            c->ciaa_pra = (uint8_t)(value & 0xFFu);
            c->rom_overlay = (c->ciaa_pra & 1u) != 0; map_low_page_fwd(c); /* AROS rom_init clears OVL here */
        } else if ((addr == QRX_CIA_TAOFF || addr == QRX_CIAA_TALO) && width == 1) {
            if (c->cia_icr_trace)
                fprintf(stderr, "[TALO] w=%02x PC=%08x instr=%llu\n",
                    (unsigned)(value & 0xFFu), m68k_get_reg(NULL, M68K_REG_PC),
                    (unsigned long long)c->instructions);
            c->cia_ta_latch = (uint16_t)((c->cia_ta_latch & 0xFF00u) | (value & 0xFFu));
        } else if ((addr == QRX_CIA_TAHI || addr == QRX_CIAA_TAHI) && width == 1) {
            /* m68k AROS convention: reloading the latch auto-starts the timer
             * (lowlevel.c ciaint_timer "reload new timer value (timer autostarts)"). */
            if (c->cia_icr_trace)
                fprintf(stderr, "[TAHI] w=%02x PC=%08x latch=%04x auto-start instr=%llu\n",
                    (unsigned)(value & 0xFFu), m68k_get_reg(NULL, M68K_REG_PC),
                    (unsigned)((c->cia_ta_latch & 0x00FFu) | ((value & 0xFFu) << 8)),
                    (unsigned long long)c->instructions);
            c->cia_ta_latch = (uint16_t)((c->cia_ta_latch & 0x00FFu) | ((value & 0xFFu) << 8));
            c->cia_ta_start = c->instructions;
            c->cia_ref = c->instructions;
            c->cia_cra |= 1u;
        } else if ((addr == QRX_CIA_CRA || addr == QRX_CIAA_CRA) && width == 1) {
            uint8_t was = c->cia_cra;
            c->cia_cra = (uint8_t)(value & 0xFFu);
            if ((c->cia_cra & 1u) && !(was & 1u)) {
                c->cia_ta_start = c->instructions;
                c->cia_ref = c->instructions;
            }
            if (c->cia_icr_trace)
                fprintf(stderr, "[CRA] w=%02x PC=%08x was=%02x instr=%llu\n",
                    (unsigned)c->cia_cra, m68k_get_reg(NULL, M68K_REG_PC), (unsigned)was, (unsigned long long)c->instructions);
        } else if ((addr == QRX_CIA_TBLO || addr == QRX_CIAA_TBLO) && width == 1) {
            if (c->cia_icr_trace)
                fprintf(stderr, "[TBL0] w=%02x instr=%llu\n",
                    (unsigned)(value & 0xFFu), (unsigned long long)c->instructions);
            c->cia_tb_latch = (uint16_t)((c->cia_tb_latch & 0xFF00u) | (value & 0xFFu));
        } else if ((addr == QRX_CIA_TBHI || addr == QRX_CIAA_TBHI) && width == 1) {
            /* Same auto-start contract as TAHI: reloading the latch restarts. */
            if (c->cia_icr_trace)
                fprintf(stderr, "[TBHI] w=%02x latch=%04x auto-start instr=%llu\n",
                    (unsigned)(value & 0xFFu),
                    (unsigned)((c->cia_tb_latch & 0x00FFu) | ((value & 0xFFu) << 8)),
                    (unsigned long long)c->instructions);
            c->cia_tb_latch = (uint16_t)((c->cia_tb_latch & 0x00FFu) | ((value & 0xFFu) << 8));
            c->cia_tb_ref = c->instructions;
            c->cia_crb |= 1u;
        } else if ((addr == QRX_CIA_CRB || addr == QRX_CIAA_CRB) && width == 1) {
            uint8_t was = c->cia_crb;
            c->cia_crb = (uint8_t)(value & 0xFFu);
            if ((c->cia_crb & 1u) && !(was & 1u)) c->cia_tb_ref = c->instructions;
            if (c->cia_icr_trace)
                fprintf(stderr, "[CRB] w=%02x PC=%08x was=%02x instr=%llu\n",
                    (unsigned)c->cia_crb, m68k_get_reg(NULL, M68K_REG_PC), (unsigned)was, (unsigned long long)c->instructions);
        } else if ((addr == QRX_CIA_ICR || addr == QRX_CIAA_ICR) && width == 1) {
            uint8_t v = (uint8_t)(value & 0x7Fu);
            if (value & 0x80u) { /* SET: mask + Flag latch */
                c->cia_icr_mask |= v;
                c->cia_icr |= v;
            } else {
                c->cia_icr_mask &= (uint8_t)~v;
                c->cia_icr &= (uint8_t)~v;
            }
            if (c->cia_icr_trace)
                fprintf(stderr, "[ICR] w=%02x mask=%02x flags=%02x instr=%llu\n",
                    (unsigned)v, (unsigned)c->cia_icr_mask, (unsigned)c->cia_icr,
                    (unsigned long long)c->instructions);
            paula_recompute(c);
        }
        return true;
    }
    if (board_hole(c, addr, width)) return true;
    return false;
}

static bool rom_read(const QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t *out) {
    const uint8_t *rom = NULL;
    uint64_t ofs = 0;
    size_t len = 0;
    if (c->rom_main && addr >= QRX_FULL_ROM_MAIN_BASE &&
        (uint64_t)addr + width <= (uint64_t)QRX_FULL_ROM_MAIN_BASE + c->rom_main_size) {
        rom=c->rom_main;len=c->rom_main_size;ofs=addr-QRX_FULL_ROM_MAIN_BASE;
    } else if (c->rom_ext && addr >= QRX_FULL_ROM_EXT_BASE &&
        (uint64_t)addr + width <= (uint64_t)QRX_FULL_ROM_EXT_BASE + c->rom_ext_size) {
        rom=c->rom_ext;len=c->rom_ext_size;ofs=addr-QRX_FULL_ROM_EXT_BASE;
    } else if (c->rom_overlay && c->rom_main && (uint64_t)addr + width <= 8u) {
        rom=c->rom_main;len=c->rom_main_size;ofs=addr;
    }
    if (!rom || ofs + width > len) return false;
    uint32_t v=0;
    for (unsigned i=0;i<width;i++) v=(v<<8)|rom[ofs+i];
    *out=v;
    return true;
}
static bool rom_overlaps(const QRXFullCPU *c, uint32_t a, unsigned width) {
    uint64_t e=(uint64_t)a+width;
    if (c->rom_main && a < (uint64_t)QRX_FULL_ROM_MAIN_BASE+c->rom_main_size && e>QRX_FULL_ROM_MAIN_BASE) return true;
    if (c->rom_ext && a < (uint64_t)QRX_FULL_ROM_EXT_BASE+c->rom_ext_size && e>QRX_FULL_ROM_EXT_BASE) return true;
    if (c->rom_overlay && c->rom_main && a<8 && e>0) return true;
    return false;
}
static bool full_internal_read(QRXFullCPU *c,uint32_t a,unsigned n,uint32_t *out) {
    /* Exact long reads serve the QRX board registers; anything else in this
     * window is the AROS Zorro-III config scan (byte nibbles), which must
     * float high so no phantom board is detected. */
    if(n==4) {
        if(a==QRX_FULL_TIMER_CTRL) {*out=c->timer_control;return true;}
        if(a==QRX_FULL_TIMER_PERIOD) {*out=c->timer_period;return true;}
        if(a==QRX_FULL_IRQ_PENDING) {*out=c->irq_pending ? 1u<<2 : 0;return true;}
        if(a==QRX_FULL_ROM_OVERLAY_CTRL) {*out=c->rom_overlay ? 1u : 0u;return true;}
    }
    *out=ones(n);return true;
}
static bool full_internal_write(QRXFullCPU *c,uint32_t a,unsigned n,uint32_t val) {
    if (n!=4) return true; /* sub-word writes in this window: Z3 ShutUp to nothing */
    if(a==QRX_FULL_TIMER_CTRL) {
        c->timer_control=val&3u;
        if(c->timer_control&QRX_FULL_TIMER_ENABLE) c->timer_remaining=c->timer_period;
        return true;
    }
    if(a==QRX_FULL_TIMER_PERIOD) {
        c->timer_period=val;
        c->timer_remaining=val;
        return true;
    }
    if(a==QRX_FULL_IRQ_ACK) {
        if(val&(1u<<2)) {c->irq_pending=0; if(c->aux_cpu_index==0) m68k_set_irq(c->irq_level);}
        return true;
    }
    if(a==QRX_FULL_ROM_OVERLAY_CTRL) {c->rom_overlay=(val&1u)!=0;map_low_page_fwd(c);return true;}
    return false;
}
static bool overlap_internal(uint32_t a,unsigned n) {
    return (uint64_t)a<QRX_FULL_ROM_OVERLAY_CTRL+4u && (uint64_t)a+n>QRX_FULL_TIMER_CTRL;
}
bool qrx_full_bus_read(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t *out) {
    if (!c || !out || (width!=1&&width!=2&&width!=4)) return false;
    if (upper_io(addr,width)) {
        if (c->door_read && c->door_read(c, addr, width, out)) return true;
        if (c->aux_read && c->aux_read(c->aux_opaque, addr, width, out)) return true;
        if (overlap_internal(addr,width)) return full_internal_read(c,addr,width,out);
        /* Z3 config header (no boards) floats high; internal regs above win. */
        if (board_hole(c, addr, width)) { *out = ones(width); return true; }
        return qrx_vm_read(&c->vm,norm_addr(addr,width),width,out);
    }
    /* Old FF0000 virtual-board alias retained for RAM boot-monitor tests; it
       is shadowed by the actual F80000 Kickstart ROM when that ROM is present. */
    if (rom_read(c,addr,width,out)) return true;
    if (rom_overlaps(c,addr,width)) return false;
    if (amiga_trap_read(c,addr,width,out)) return true;
    if (qrx_vm_read(&c->vm,addr,width,out)) return true;
    /* With a Kickstart mapped, unmapped guest bus floats high like Amiga
     * hardware (no MMU yet): motherboard-RAM probes (findmbram 0x08000000+)
     * and absent-device polls must fail their compare, never fault the CPU. */
    if (amiga_bus(c)) { *out = ones(width); return true; }
    return false;
}
bool qrx_full_bus_write(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t value) {
    if (!c || (width!=1&&width!=2&&width!=4)) return false;
    { /* Amithlon host-clock discovery: the ROM's set_eclock writes the
       * 4-long sequence $BFE001,$BFEF01,$BFE601,$BFE701 at consecutive
       * TB-fields +0x5E/+0x62/+0x66/+0x6A (v27 [RAMWR]-verified); a full
       * match pins the TimerBase unambiguously. Rate value lands in
       * tb+0x8E (709379 PAL / 715909 NTSC). */
      static uint32_t htb_stage, htb_dbg, htb_rate_addr, htb_rate_val;
      int pc_urom = (int)m68k_get_reg(NULL, M68K_REG_PC) >= (int)0xF80000u;
      if (width == 4 && pc_urom && htb_stage < 4u) {
          uint32_t expect_v = htb_stage == 0 ? 0x00BFE001u : htb_stage == 1 ? 0x00BFEF01u :
              htb_stage == 2 ? 0x00BFE601u : 0x00BFE701u;
          uint64_t expect_a = (uint64_t)htb_dbg + 4u*htb_stage;
          if (value == expect_v && (htb_stage == 0 || (uint64_t)addr == expect_a)) {
              if (htb_stage == 0) htb_dbg = addr;
              htb_stage++;
              if (htb_stage == 4u) {
                  c->host_tb = htb_dbg - 0x5E;
                  htb_stage = 0;
                  { /* QRX_FULL_HOST_E_CLOCK: default on, Amithlon-trace STO */
                      const char *e = env_QRX_FULL_HOST_E_CLOCK;
                      c->host_tb_enable = (!e || strcmp(e, "0") != 0) ? 1u : 0u;
                  }
                  fprintf(stderr, "[HCLOCK] TimerBase=%08x detected\n", c->host_tb);
                  /* the rate write normally precedes the marker sequence */
                  if (htb_rate_addr && (uint32_t)(htb_rate_addr - c->host_tb) == 0x8Eu) {
                      c->host_tb_rate = htb_rate_val;
                      fprintf(stderr, "[HCLOCK] rate=%u\n", htb_rate_val);
                  } else {
                      uint32_t r = 0;
                      if (qrx_full_bus_read(c, c->host_tb + 0x8E, 4, &r) &&
                          r >= 100000u && r <= 2000000u) {
                          c->host_tb_rate = r;
                          fprintf(stderr, "[HCLOCK] rate=%u (from field)\n", r);
                      }
                  }
              }
          } else if (value == expect_v) { htb_stage = 1; htb_dbg = addr; }
          else htb_stage = 0;
      }
      if (width == 4 && pc_urom && (value == 709379u || value == 715909u)) {
          htb_rate_val = value; htb_rate_addr = addr;
          if (c->host_tb && (uint32_t)(addr - c->host_tb) == 0x8Eu) {
              c->host_tb_rate = value;
              fprintf(stderr, "[HCLOCK] rate=%u\n", value);
          }
      } else if (width == 4 && c->host_tb && (uint32_t)(addr - c->host_tb) == 0x8Eu) {
          c->host_tb_rate = value;
      }
    }
    { /* Debug watch: trace writes to one guest RAM address (env QRX_FULL_TRACE_RAMW as hex).
         Optional: env QRX_FULL_TRACE_RAMW_STACK also dumps guest regs and the
         return-address words on the current guest SP (first 28 longwords). */
      const char *rw = env_QRX_FULL_TRACE_RAMW;
      if (rw && addr == (uint32_t)strtoul(rw, NULL, 16) && active == c)
      {
          fprintf(stderr, "[RAMW] W%d %08x=%0*x PC=%08x instr=%llu\n", width*8, addr,
              width*2, value, m68k_get_reg(NULL, M68K_REG_PC),
              (unsigned long long)c->instructions);
          if (env_QRX_FULL_TRACE_RAMW_STACK)
          {
              static unsigned hits;
              if (hits++ < 64u)
              {
                  unsigned k;
                  fprintf(stderr, "[RAMW] D0=%08x D1=%08x D2=%08x D3=%08x A0=%08x A1=%08x A2=%08x A6=%08x SP=%08x\n",
                      m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
                      m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_D3),
                      m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
                      m68k_get_reg(NULL, M68K_REG_A2), m68k_get_reg(NULL, M68K_REG_A6),
                      m68k_get_reg(NULL, M68K_REG_A7));
                  for (k = 0; k < 28u; k++)
                  {
                      uint32_t sa = (uint32_t)m68k_get_reg(NULL, M68K_REG_A7) + k * 4u;
                      uint32_t v = 0;
                      if (qrx_full_bus_read(c, sa, 4, &v))
                          fprintf(stderr, "[RAMW] SP+%02x = %08x\n", k * 4u, v);
                      else
                          fprintf(stderr, "[RAMW] SP+%02x = <unreadable>\n", k * 4u);
                  }
              }
          }
      }
      { /* Optional region watch: QRX_FULL_TRACE_RAMW_RANGE=X0,X1[,max] traces
           every write inside [X0,X1] with PC and instruction count, first `max`
           lines (default 64). Use to find which TimerBase fields the ROM's
           EClock handler updates. */
          static uint32_t rwlo, rwhi, rwmax = 64, rwcount;
          const char *rwr = env_QRX_FULL_TRACE_RAMW_RANGE;
          if (rwr && rwmax != 0 && active == c)
          {
              if (rwlo == 0 && rwhi == 0)
              {
                  char e0[16] = "", e1[16] = "", e2[16] = "";
                  if (sscanf(rwr, "%15[^,],%15[^,],%15s", e0, e1, e2) >= 2)
                  {
                      rwlo = (uint32_t)strtoul(e0, NULL, 16);
                      rwhi = (uint32_t)strtoul(e1, NULL, 16);
                      if (e2[0]) { long n = strtol(e2, NULL, 10); rwmax = (n > 0 && n < 1000000) ? (uint32_t)n : 64; }
                  }
                  else { rwmax = 0; fprintf(stderr, "[RAMWR] bad range '%s'\n", rwr); }
              }
              if (rwmax && (addr & ~1u) >= rwlo && addr <= rwhi)
              {
                  if (rwcount++ < rwmax)
                      fprintf(stderr, "[RAMWR] W%d %08x=%0*x PC=%08x instr=%llu\n",
                          width * 8, addr, width * 2, value,
                          m68k_get_reg(NULL, M68K_REG_PC),
                          (unsigned long long)c->instructions);
                  else { fprintf(stderr, "[RAMWR] cap reached\n"); rwmax = 0; }
              }
          }
      }
    }    if (upper_io(addr,width)) {
        if(c->door_write && c->door_write(c,addr,width,value)) return true;
        if(c->aux_write && c->aux_write(c->aux_opaque,addr,width,value)) return true;
        if(overlap_internal(addr,width)) return full_internal_write(c,addr,width,value);
        if(board_hole(c,addr,width)) return true; /* ShutUp to absent Z3 boards */
        addr=norm_addr(addr,width);
    } else if(rom_overlaps(c,addr,width)) { host_ro_probe(c, addr, width, value); return false; }
    else if(amiga_trap_write(c,addr,width,value)) return true;
    bool ok=qrx_vm_write(&c->vm,addr,width,value);
    if (!ok && amiga_bus(c)) ok = true; /* Kickstart mapped: writes to open bus vanish */
    if (ok && c->ram_write && (uint64_t)addr < c->vm.ram.size)
        c->ram_write(c->aux_opaque,addr,width);
    if (ok && c->jit_notify && (uint64_t)addr < c->vm.ram.size)
        c->jit_notify(c,addr,width);
    if (ok && c->vm.cpu.halted && c->reset_done) m68k_end_timeslice();
    return ok;
}
/* Amithlon ROM-write probe: the ROM region is normally read-only, but AROS
 * patches itself (aros.library init); log all attempts, allow ROM-writes
 * with QRX_FULL_ROM_WRONLY=1 to see what the guest expects. */
static void host_ro_probe(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t value) {
    uint8_t *dst; size_t ofs = 0, len = 0;
    if (c->rom_main && addr >= QRX_FULL_ROM_MAIN_BASE &&
        (uint64_t)addr + width <= (uint64_t)QRX_FULL_ROM_MAIN_BASE + c->rom_main_size) {
        dst=c->rom_main;len=c->rom_main_size;ofs=addr-QRX_FULL_ROM_MAIN_BASE;
    } else if (c->rom_ext && addr >= QRX_FULL_ROM_EXT_BASE &&
        (uint64_t)addr + width <= (uint64_t)QRX_FULL_ROM_EXT_BASE + c->rom_ext_size) {
        dst=c->rom_ext;len=c->rom_ext_size;ofs=addr-QRX_FULL_ROM_EXT_BASE;
    } else return;
    static unsigned nolog, rohits;
    rohits++;
    if (rohits <= 64)
        fprintf(stderr, "[RO] W%d %08x=%0*x PC=%08x\n", width*8, addr, width*2, value,
            m68k_get_reg(NULL, M68K_REG_PC));
    if (c->ro_wronly) {
        for (unsigned i=width;i>0;i--) { dst[ofs+i-1]=(uint8_t)(value&0xFFu); value>>=8; }
    }
    (void)nolog;
}
#define QRX_PAGE_SHIFT 12u
/* A true active->fast_bus also means the tables exist. Accesses straddling a
 * page take the full bus, which handles any window boundary correctly. */
#define LIKELY_FAST(a,w) (__builtin_expect(active&&active->fast_bus,1)&&((a)&0xFFFu)<=0x1000u-(w))
#define QRX_PAGE_SIZE (1u<<QRX_PAGE_SHIFT)
#define QRX_PAGE_ENTRIES (1u<<20)
/* Every bus trap on this board sits inside these two windows. */
static bool page_is_plain_ram(const QRXFullCPU *c,uint32_t a) {
    if((uint64_t)a+QRX_PAGE_SIZE>c->vm.ram.size)return false;
    if(a>=0x00A00000u && a<0x00C00000u)return false;
    if(a>=0x00DC0000u && a<0x01000000u)return false;
    return true;
}
static void map_rom_pages(QRXFullCPU *c,uint32_t base,const uint8_t *rom,size_t size) {
    if(!rom)return;
    for(size_t o=0;o+QRX_PAGE_SIZE<=size;o+=QRX_PAGE_SIZE)
        c->rpage[(base+o)>>QRX_PAGE_SHIFT]=(uintptr_t)rom-(uintptr_t)base;
}
static void set_write_page(QRXFullCPU *c,uint32_t page,uintptr_t v) {
    uint32_t first=page<<(QRX_PAGE_SHIFT-QRX_WPAGE_SHIFT);
    for(uint32_t i=0;i<(1u<<(QRX_PAGE_SHIFT-QRX_WPAGE_SHIFT));i++)c->wpage[first+i]=v;
}
static void map_low_page(QRXFullCPU *c) {
    if(!c->fast_bus)return;
    uintptr_t v=c->rom_overlay?0:(uintptr_t)c->ram_owned;
    c->rpage[0]=v;set_write_page(c,0,v);
}
/* Tracing options need to observe every access, so they keep the full bus. */
static bool bus_tracing(void) {
    return env_QRX_FULL_WATCH_PC||env_QRX_FULL_TRACE_HIGH||env_QRX_FULL_TRACE_RAM||
        env_QRX_FULL_TRACE_VEC||env_QRX_FULL_TRACE_RAMW||env_QRX_FULL_TRACE_RAMW_RANGE;
}
static void build_page_maps(QRXFullCPU *c) {
    if(!c->rpage)return;
    memset(c->rpage,0,QRX_PAGE_ENTRIES*sizeof(uintptr_t));
    memset(c->wpage,0,QRX_WPAGE_ENTRIES*sizeof(uintptr_t));
    c->fast_bus=!bus_tracing()&&!c->aux_opaque;
    if(!c->fast_bus)return;
    for(uint32_t a=0;(uint64_t)a+QRX_PAGE_SIZE<=c->vm.ram.size;a+=QRX_PAGE_SIZE)
        if(page_is_plain_ram(c,a)){
            c->rpage[a>>QRX_PAGE_SHIFT]=(uintptr_t)c->ram_owned;
            set_write_page(c,a>>QRX_PAGE_SHIFT,(uintptr_t)c->ram_owned);
        }
    map_rom_pages(c,QRX_FULL_ROM_MAIN_BASE,c->rom_main,c->rom_main_size);
    map_rom_pages(c,QRX_FULL_ROM_EXT_BASE,c->rom_ext,c->rom_ext_size);
    map_low_page(c);
}
static void map_low_page_fwd(QRXFullCPU *c){map_low_page(c);}
static inline unsigned page_load(const uint8_t *p,unsigned width) {
    if(width==2){uint16_t v;memcpy(&v,p,2);return __builtin_bswap16(v);}
    if(width==4){uint32_t v;memcpy(&v,p,4);return __builtin_bswap32(v);}
    return *p;
}
static inline void page_store(uint8_t *p,unsigned width,unsigned value) {
    if(width==2){uint16_t v=__builtin_bswap16((uint16_t)value);memcpy(p,&v,2);}
    else if(width==4){uint32_t v=__builtin_bswap32(value);memcpy(p,&v,4);}
    else *p=(uint8_t)value;
}
/* The ROM's EClock setup is the only guest write the full bus inspects by
 * value; everything else on a plain RAM page bypasses it. */
static inline bool write_needs_full_bus(const QRXFullCPU *c,uint32_t a,unsigned width,uint32_t v) {
    if(width!=4)return false;
    return (v&0xFFFFF000u)==0x00BFE000u||v==709379u||v==715909u||
           (c->host_tb && a-c->host_tb==0x8Eu);
}
static unsigned int core_read(unsigned address,unsigned width) {
    if(LIKELY_FAST(address,width)) {
        uintptr_t base=active->rpage[address>>QRX_PAGE_SHIFT];
        if(base)return page_load((const uint8_t *)(base+address),width);
    }
    /* Debug watch: count opcode fetches in a guest range (env QRX_FULL_WATCH_PC
     * as "start-end", hex). Used to prove an interrupt server ever executes. */
    if (width == 2) {
        const char *w = env_QRX_FULL_WATCH_PC;
        if (w) {
            unsigned long s = 0, e = 0;
            if (sscanf(w, "%lx-%lx", &s, &e) == 2 && (unsigned)address >= s && (unsigned)address < e) {
                static unsigned long hits;
                ++hits;
                if (w[0] == '+' || (hits & 0xFFFu) == 1u)
                    fprintf(stderr, "[WATCH] PC=%08x D%08x A%08x SR=%04x hits=%lu instr=%llu cyc=%llu\n",
                        address, m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_A0),
                        m68k_get_reg(NULL, M68K_REG_SR) & 0xFFFFu, hits,
                        (unsigned long long)active->instructions, (unsigned long long)active->cycles);
            }
        }
    }
    uint32_t out=0;
    const char *watch;
    if (env_QRX_FULL_TRACE_HIGH && address >= 0x00E80000u)
        fprintf(stderr, "[BUS] R%d %08x PC=%08x\n", width * 8, address, m68k_get_reg(NULL, M68K_REG_PC));
    if ((watch = env_QRX_FULL_TRACE_RAM) && address == (unsigned)strtoul(watch, NULL, 16)) {
        uint32_t rv = 0; qrx_full_bus_read(active, address, width, &rv);
        fprintf(stderr, "[RAM] R%d %08x=%08x PC=%08x\n", width * 8, address, rv, m68k_get_reg(NULL, M68K_REG_PC));
    }
    if (env_QRX_FULL_TRACE_VEC && address < 0x100u) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        fprintf(stderr, "[VEC] R%d %08x PC=%08x\n", width * 8, address, pc);
        if (address == 0x0Cu && active) {
            /* last retired PCs before the fault (oldest first) */
            unsigned k, n = active->pc_ring_i < 16u ? active->pc_ring_i : 16u;
            fprintf(stderr, "[VEC] ADDERR SP=%08x last PCs:", m68k_get_reg(NULL, M68K_REG_A7));
            for (k = 0; k < n; k++) fprintf(stderr, " %08x", active->pc_ring[(active->pc_ring_i - n + k) & 15u]);
            fprintf(stderr, "\n");
        }
    }
    if(!active || !qrx_full_bus_read(active,address,width,&out)) {
        if(active) {active->mem_fault=true;active->fault_address=address;m68k_end_timeslice();}
        return UINT32_MAX;
    }
    return out;
}
static void core_write(unsigned address,unsigned width,unsigned value) {
    if(__builtin_expect(active&&active->fast_bus,1)&&(address&(QRX_WPAGE_SIZE-1u))<=QRX_WPAGE_SIZE-width) {
        uintptr_t base=active->wpage[address>>QRX_WPAGE_SHIFT];
        if(base && !write_needs_full_bus(active,address,width,value)) {
            page_store((uint8_t *)(base+address),width,value);
            active->vm.ram_write_epoch++;
            return;
        }
    }
    const char *watch;
    if ((watch = env_QRX_FULL_TRACE_RAM) && address == (unsigned)strtoul(watch, NULL, 16))
        fprintf(stderr, "[RAM] W%d %08x=%08x PC=%08x\n", width * 8, address, value, m68k_get_reg(NULL, M68K_REG_PC));
    if(!active || !qrx_full_bus_write(active,address,width,value)) {
        if(active) {active->mem_fault=true;active->fault_address=address;m68k_end_timeslice();}
    }
}
unsigned int m68k_read_memory_8(unsigned int a){return core_read(a,1);}
unsigned int m68k_read_memory_16(unsigned int a){return core_read(a,2);}
unsigned int m68k_read_memory_32(unsigned int a){return core_read(a,4);}
void m68k_write_memory_8(unsigned int a,unsigned int v){core_write(a,1,v);}
void m68k_write_memory_16(unsigned int a,unsigned int v){core_write(a,2,v);}
void m68k_write_memory_32(unsigned int a,unsigned int v){core_write(a,4,v);}
void m68k_write_memory_32_pd(unsigned int a,unsigned int v) {
    core_write(a+2u,2,v&0xffffu);core_write(a,2,(v>>16)&0xffffu);
}
/* Resident/ROM entry watchpoints: QRX_FULL_PC_WATCH=addr,addr,...
 * Each hit prints PC, the guest exec context (ThisTask name/state and signal
 * state) and a few data registers, so we can see exactly which ROM resident
 * the boot reached and where it parked. */
static uint32_t qrx_pcw[32]; static unsigned qrx_pcw_n;
/* Irq-visibility probe enlisted by Musashi's m68ki_check_interrupts:
 * prints the cpu host pin level vs the guest's IPL mask and a stale NMI
 * flag. Bounded samples only (QRX_FULL_IRQ_DEBUG_MAX, default 60) so a
 * hot probe loop doesn't flood. */
void qrx_irq_probe(int cpu_level, int mask, int nmi) {
    static unsigned dbg_hits, dbg_after;
    static int dbg_max = -1;
    if (dbg_max < 0) {
        const char *m = env_QRX_FULL_IRQ_DEBUG_MAX;
        dbg_max = m ? (int)strtoul(m, NULL, 10) : 60;
        dbg_after = (unsigned)strtoul(env_QRX_FULL_IRQ_DEBUG_AFTER ? env_QRX_FULL_IRQ_DEBUG_AFTER : "0", NULL, 10);
    }
    if (active && active->instructions < dbg_after) return;
    if (dbg_hits++ >= (unsigned)(dbg_max > 0 ? dbg_max : 0)) return;
    fprintf(stderr, "[IRQPROBE] lvl=%d mask=%04x nmi=%d instr=%llu\n", cpu_level, mask & 0xFFFFu, nmi,
        active ? (unsigned long long)active->instructions : 0u);
}
static void qrx_pcw_init(const char *pv) {
    const char *p = pv;
    while (*p && qrx_pcw_n < 32) {
        qrx_pcw[qrx_pcw_n] = (uint32_t)strtoul(p, (char **)&p, 16);
        if (qrx_pcw[qrx_pcw_n]) qrx_pcw_n++;
        if (*p == ',') p++; else break;
    }
}
static void qrx_pcw_hit(unsigned pc) {
    static unsigned whits[32];
    for (unsigned i = 0; i < qrx_pcw_n; i++) {
        if (pc == qrx_pcw[i] && whits[i] < 256u) {
            whits[i]++;
            uint32_t sysb = 0, td = 0, sigw = 0, sigr = 0;
            uint32_t st = 0xff;
            char tname[40] = "?";
            qrx_full_bus_read(active, 4, 4, &sysb);
            if (qrx_full_bus_read(active, sysb + 276u, 4, &td) && td) {
                qrx_full_bus_read(active, td + 15u, 1, &st);
                for (int k = 0; k < (int)sizeof(tname) - 1; k++) {
                    uint32_t np = 0, ch = 0;
                    if (!qrx_full_bus_read(active, td + 10u, 4, &np) ||
                        !qrx_full_bus_read(active, np + (uint32_t)k, 1, &ch) ||
                        !(ch & 0xFFu) || (ch & 0xFFu) < 32 || (ch & 0xFFu) > 126) break;
                    tname[k] = (char)(ch & 0xFFu);
                }
                tname[sizeof(tname) - 1] = 0;
                qrx_full_bus_read(active, td + 0x16u, 4, &sigw);
                qrx_full_bus_read(active, td + 0x1Au, 4, &sigr);
            }
            uint32_t usp = m68k_get_reg(NULL, M68K_REG_A7), ra = 0;
            qrx_full_bus_read(active, usp, 4, &ra);
            fprintf(stderr, "[PCWATCH] hit=%u PC=%08x instr=%llu task=%08x '%s' st=%u sw=%08x sr=%08x D0=%08x A0=%08x A1=%08x A2=%08x A6=%08x SR=%04x RA@SP=%08x\n",
                whits[i], pc, (unsigned long long)active->instructions, td, tname, st, sigw, sigr,
                m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_A0),
                m68k_get_reg(NULL, M68K_REG_A1), m68k_get_reg(NULL, M68K_REG_A2),
                m68k_get_reg(NULL, M68K_REG_A6),
                m68k_get_reg(NULL, M68K_REG_SR) & 0xFFFFu, ra);
        }
    }
}
/* Normal execution keeps counters and subcall termination without walking
 * diagnostic options on every guest instruction. */
static void instr_hook_fast(unsigned pc){
    if(!active)return;
    if(active->vec_trace)active->pc_ring[active->pc_ring_i++&15u]=pc;
    if(active->subroutine_armed && pc==active->subroutine_sentinel){
        active->subroutine_returned=true;m68k_end_timeslice();
    }
    active->instructions++;
}
static void instr_hook(unsigned pc){
    if(!active)return;
    /* Bounded instruction-window tracer: env QRX_FULL_TRACE_WINDOW as
     * "start-end" (hex) prints PC + D0/D1/D2/A0/A1/A2/A3/A6/A7/SR and the
     * first 5 stack longwords for every instruction in the range, up to
     * QRX_FULL_TRACE_WINDOW_MAX lines (default 400). Used to decode hot
     * policy loops inside the ROM instead of per-4096 sampling. */
    { static uint32_t tw_s, tw_e; static unsigned tw_left, tw_after; static bool twi;
      if (!twi) { twi = true; tw_left = 400; tw_after = 0;
        const char *tw = env_QRX_FULL_TRACE_WINDOW;
        if (tw) {
          unsigned long s = 0, e = 0;
          if (sscanf(tw, "%lx-%lx", &s, &e) == 2) { tw_s = (uint32_t)s; tw_e = (uint32_t)e; }
          else tw_e = tw_s = 0;
        } else tw_e = tw_s = 0;
        const char *m = env_QRX_FULL_TRACE_WINDOW_MAX;
        if (m) tw_left = (unsigned)strtoul(m, NULL, 10);
        tw_after = (unsigned)strtoul(env_QRX_FULL_TRACE_WINDOW_AFTER ? env_QRX_FULL_TRACE_WINDOW_AFTER : "0", NULL, 10);
      }
      if (active->instructions > tw_after && tw_e > tw_s && tw_left && pc >= tw_s && pc < tw_e) {
        uint32_t sw0=0,sw1=0,sw2=0,sw3=0,sw4=0;
        uint32_t a7 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A7);
        (void)qrx_full_bus_read(active, a7, 4, &sw0);
        (void)qrx_full_bus_read(active, a7+4, 4, &sw1);
        (void)qrx_full_bus_read(active, a7+8, 4, &sw2);
        (void)qrx_full_bus_read(active, a7+12, 4, &sw3);
        (void)qrx_full_bus_read(active, a7+16, 4, &sw4);
        fprintf(stderr, "[TRACEW] PC=%08x D0=%08x D1=%08x D2=%08x A0=%08x A1=%08x A2=%08x A3=%08x A6=%08x A7=%08x SR=%04x st=%08x %08x %08x %08x %08x\n",
            pc, m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_D2),
            m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1), m68k_get_reg(NULL, M68K_REG_A2),
            m68k_get_reg(NULL, M68K_REG_A3), m68k_get_reg(NULL, M68K_REG_A6), a7,
            m68k_get_reg(NULL, M68K_REG_SR) & 0xFFFFu, sw0, sw1, sw2, sw3, sw4);
        --tw_left;
      }
    }
    { const char *pv = env_QRX_FULL_PC_WATCH;
      if (pv) { { static bool winit; if (!winit) { winit = true; qrx_pcw_init(pv); } } qrx_pcw_hit(pc); } }
    { static unsigned hc; if (env_QRX_FULL_HOOKSPY && hc < 3u)
        fprintf(stderr, "[HOOK] fired pc=%08x n=%u\n", (unsigned)pc, hc++); }
    { const char *g = env_QRX_FULL_GATE_SPY;
      if (g && (pc == (unsigned)strtoul(g, NULL, 16) || (strchr(g, ',') && pc == (unsigned)strtoul(strchr(g, ',') + 1, NULL, 16)))) {
        unsigned a7 = m68k_get_reg(NULL, M68K_REG_A7);
        unsigned sr = m68k_get_reg(NULL, M68K_REG_SR);
        uint32_t v = 0; qrx_full_bus_read(active, a7 + 0x18u, 1, &v);
        uint32_t w0=0,w1=0,w2=0,w3=0;
        qrx_full_bus_read(active, a7, 4, &w0); qrx_full_bus_read(active, a7+4, 4, &w1);
        qrx_full_bus_read(active, a7+8, 4, &w2); qrx_full_bus_read(active, a7+12, 4, &w3);
        unsigned d0 = m68k_get_reg(NULL, M68K_REG_D0);
        fprintf(stderr, "[GATE] PC=%08x D0=%08x A7=%08x SR=%04x B18=%02x stack=%08x %08x %08x %08x\n", pc, d0, a7, sr&0xffffu, v&0xffu, w0, w1, w2, w3);
      } }
    if(active->vec_trace){active->pc_ring[active->pc_ring_i++&15u]=(uint32_t)pc;}
    /* The next fetched instruction is a harmless NOP, which consumes the
       remaining slice; never execute the firmware PC during the subcall. */
    if(active->subroutine_armed && pc==active->subroutine_sentinel){
        active->subroutine_returned=true;
        m68k_end_timeslice();
    }
    active->instructions++;
}
/* Denise VBLANK is deasserted when the CPU accepts it (edge-like) as well as
 * on the guest's INTREQ ack write. Without this, a guest that never acks
 * (early boot stubs) would re-enter the VertB chain thousands of times per
 * frame on the stuck level-triggered line, starving the bootstrap task. */
static int int_ack(int level) {
    if (env_QRX_FULL_IRQ_TRACE)
        fprintf(stderr, "[INTACK] level=%d PC=%08x instr=%llu SR=%04x ports=%d vbl=%d cia=%02x/%02x "
            "D0=%08x A0=%08x A2=%08x A6=%08x\n",
            level, m68k_get_reg(NULL, M68K_REG_PC),
            active ? (unsigned long long)active->instructions : 0,
            m68k_get_reg(NULL, M68K_REG_SR) & 0xFFFFu,
            active ? active->ports_asserted : 0, active ? active->vblank_asserted : 0,
            active ? active->cia_icr : 0, active ? active->cia_icr_mask : 0,
            m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_A0),
            m68k_get_reg(NULL, M68K_REG_A2), m68k_get_reg(NULL, M68K_REG_A6));
    /* Host boot-wake pulse: aKick task was made READY and the exec idle STOP
     * must be exited. Musashi took the exception; drop the line now so the
     * ret does not re-enter the same autovector forever. */
    if (level == 3 && active && active->kick_pending) {
        active->kick_pending = 0u;
        if (!active->vblank_asserted)
            m68k_set_irq(active->irq_pending ? active->irq_level : 0u);
    }
    /* Device requests remain asserted until the guest acknowledges INTREQ. */
    /* Level 2: the exec chain ($FE609E/$FE6204) itself reads $DFF01C bit14
     * to confirm the level-2 is in flight, then dispatches the CIA INT
     * servers which read ICR ($BFED01/$BFDD1D — the read clears) and clear
     * PORTS via their $DFF09C write. Do NOT pre-clear ports_asserted or the
     * CIA flags on the host ack: dropping everything before the handler's
     * reads killed the CIA INT chain — the system clock never advanced and
     * the input.device task sat forever in a CmpTime($FAB1E0) busy loop. */
    if (level == 2 && active) {
        int had = active->ports_asserted ||
                  (active->cia_icr & active->cia_icr_mask) != 0 ||
                  (active->ciab_icr & active->ciab_icr_mask) != 0;
        /* (no flag or port clears: the guest's level-2 handler does them) */
        /* Do NOT clear the CIA flags here: on real hardware the CPU ack does
         * not clear the CIA flag latch — the guest reads ICR ($BFED01/
         * $BFDD1D) to see WHICH source fired and that read clears. The old
         * pre-emptive clear made the ROM's level-2 chain ($FE609E, ciab_
         * eclock INT) see flags==0 forever, so tb_CurrentTime never advanced
         * and the input.device task spent its whole boot in a CmpTime
         * ($(FAB1E0)) busy loop. The level-2 line re-raises from paula_
         * recompute once the flags are read by the guest, matching
         * level-triggered CIA interrupts. */
        if (had) paula_recompute(active);
        if (!active->irq_pending) m68k_set_irq(0);
    }
    return (int)M68K_INT_ACK_AUTOVECTOR;
}
bool qrx_full_init(QRXFullCPU *c,size_t ram_size,unsigned model) {
    if(!c || active || !full_model(model) || ram_size<1024 || ram_size>QRX_RAM_MAX_BYTES) return false;
    memset(c,0,sizeof(*c));
    if(!qrx_ram_allocate(&c->host_allocation,ram_size))return false;
    c->ram_owned=(uint8_t*)c->host_allocation.addr;
    qrx_vm_init(&c->vm,c->ram_owned,ram_size);
    c->cpu_type=(int)model;
    active=c;
    read_options();
    /* Lazily-zeroed virtual tables; only pages the guest touches get backed. */
    size_t maps=QRX_PAGE_ENTRIES*sizeof(uintptr_t);
    void *r=mmap(NULL,maps,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    size_t wmaps=QRX_WPAGE_ENTRIES*sizeof(uintptr_t);
    void *w=mmap(NULL,wmaps,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(r!=MAP_FAILED&&w!=MAP_FAILED){c->rpage=r;c->wpage=w;}
    else{if(r!=MAP_FAILED)munmap(r,maps);if(w!=MAP_FAILED)munmap(w,wmaps);}
    /* Musashi init/reset retains general registers across VM lifetimes. */
    void *fresh=calloc(1,m68k_context_size());
    if(!fresh){qrx_full_destroy(c);return false;}
    m68k_set_context(fresh);free(fresh);
    m68k_init();
    m68k_set_cpu_type(full_model(model));
    m68k_set_int_ack_callback(int_ack);
    m68k_set_instr_hook_callback(
        (env_QRX_FULL_TRACE_WINDOW || env_QRX_FULL_PC_WATCH ||
         env_QRX_FULL_HOOKSPY || env_QRX_FULL_GATE_SPY) ? instr_hook : instr_hook_fast);
    return true;
}
void qrx_full_destroy(QRXFullCPU *c){
    if(!c)return;
    if(active==c){m68k_set_irq(0);active=NULL;}
    if(c->rpage)munmap(c->rpage,QRX_PAGE_ENTRIES*sizeof(uintptr_t));
    if(c->wpage)munmap(c->wpage,QRX_WPAGE_ENTRIES*sizeof(uintptr_t));
    qrx_vm_destroy(&c->vm);
    free(c->rom_main);free(c->rom_ext);qrx_ram_release(&c->host_allocation);
    memset(c,0,sizeof(*c));
}
bool qrx_full_load_rom(QRXFullCPU *c,uint32_t base,const void *data,size_t size){
    if(!c||!data||!size||size>QRX_FULL_ROM_MAX) return false;
    if(base!=QRX_FULL_ROM_MAIN_BASE && base!=QRX_FULL_ROM_EXT_BASE) return false;
    if(base==QRX_FULL_ROM_MAIN_BASE && size!=512u*1024u) return false;
    if(base==QRX_FULL_ROM_EXT_BASE && size!=512u*1024u) return false;
    uint8_t *newrom=(uint8_t*)malloc(size);
    if(!newrom)return false;
    memcpy(newrom,data,size);
    if(base==QRX_FULL_ROM_MAIN_BASE){free(c->rom_main);c->rom_main=newrom;c->rom_main_size=size;}
    else {free(c->rom_ext);c->rom_ext=newrom;c->rom_ext_size=size;}
    return true;
}
void qrx_full_set_rom_overlay(QRXFullCPU *c,bool enabled){if(c){c->rom_overlay=enabled;map_low_page(c);if(enabled)c->ciaa_pra|=1u;else c->ciaa_pra&=~1u;}}
void qrx_full_set_host(QRXFullCPU *c,QRXConsoleFn out,QRXClockFn ms,void *arg){if(c)qrx_vm_set_host(&c->vm,out,ms,arg);}
bool qrx_full_reset(QRXFullCPU *c){
    if (!c || active!=c) return false;
    /* The AROS amiga ROM starts with a 0x1114 header + JMP, not 68000
     * vectors: the reset SSP is junk (odd) and never used before rom_init
     * installs the real stack, exactly like on silicon. Only the reset PC
     * is validated; Musashi loads both through the guest bus. */
    uint32_t pc=0;
    uint32_t sp=0;
    if(!qrx_full_bus_read(c,0,4,&sp)||!qrx_full_bus_read(c,4,4,&pc)||
       (pc&1u)||!pc) return false;
    (void)sp;
    c->mem_fault=false;c->fault_address=0;c->instructions=0;c->cycles=0;
    c->ciaa_pra=0xFFu;c->ciab_prb=0xFFu; /* pull-ups: disk in, unprot, motor off */
    c->fl_cyl[0]=c->fl_cyl[1]=c->fl_cyl[2]=c->fl_cyl[3]=0;
    c->beam_frame=0;c->vblank_asserted=0;c->intena=0;
    c->cia_icr=0;c->cia_icr_mask=0;c->cia_ref=0;
    c->cia_tb_latch=0;c->cia_tb_ref=0;c->cia_crb=0;c->cia_ta_start=0;
    c->ports_asserted=0;
    c->ciab_icr=0;c->ciab_icr_mask=0;c->ciab_crb=0;c->ciab_tb_latch=0;c->ciab_tb_ref=0;
    c->dskblk_asserted=0;c->dsk_active=0;c->dsk_len=0;c->dsk_last=0;
    c->adkcon_shadow=0;c->dsk_pt=0;c->dsk_start=0;c->dmacon_shadow=0;c->mfm_track=-1;
    memset(c->boot_cia,0,sizeof c->boot_cia);c->boot_cia_clock=0;c->boot_intreq=0;
    c->pc_ring_i=0;c->vec_trace=env_QRX_FULL_TRACE_VEC!=NULL;
    c->fsaga_trace=env_QRX_FULL_FSAGA_TRACE!=NULL;
    { memset(c->fsaga_clut,0xff,sizeof(c->fsaga_clut));
      c->fsaga_mode=0; c->fsaga_bplptr=0; c->fsaga_bplhmod=0;
      c->fsaga_hpixel=c->fsaga_vpixel=0;
      c->fsaga_hpixel=c->fsaga_hsstrt=c->fsaga_hsstop=c->fsaga_htotal=0;
      c->fsaga_vpixel=c->fsaga_vsstrt=c->fsaga_vsstop=c->fsaga_vtotal=0;
      c->fsaga_hvsync=0; c->fsaga_spritex=c->fsaga_spritey=0;
      c->fsaga_frame=0; c->fsaga_width=c->fsaga_height=0; }
    c->vm.cpu.halted=0;c->vm.cpu.fault=0;
    build_page_maps(c);
    c->irq_level=c->irq_pending=0;
    c->timer_control=c->timer_period=c->timer_remaining=0;
    m68k_set_irq(0);
    m68k_pulse_reset();
    c->reset_done=true;
    return !c->mem_fault;
}
void qrx_full_irq(QRXFullCPU *c,unsigned level){
    if(!c || c!=active || level>7)return;
    c->irq_level=(uint8_t)level;
    m68k_set_irq(c->irq_pending && level<2 ? 2u : level);
}
static uint64_t beam_frame_idx(const QRXFullCPU *c) {
    return (c->cycles / QRX_BEAM_LINE_CYCLES) / QRX_BEAM_LINES;
}
/* Stuck-kick: the trackdisk/bootstrap task arms a MICROHZ timer request and
 * idles in exec's Wait loop, but no CIA-A timer-A underflow IRQ exists yet,
 * so CheckTimer never completes it and the boot stalls forever with an
 * empty Ready list. When the classic stall signature is seen (idle-loop PC
 * for several consecutive frames, ThisTask waiting, Ready empty, armed
 * timer request owned by the idle task), complete that one request by hand:
 * unlink it from the timer unit list, mark NT_REPLYMSG, queue it on its
 * reply port, wake the task (TS_READY + signal + Ready-enqueue) and set the
 * QuantumOver SysFlag so the supervisor-mode interrupt chains can run.
 * Everything is verified before any write; a failed check only logs.
 * Env QRX_FULL_STUCKKICK=0 disables. */
static uint32_t kick_rd32(QRXFullCPU *c, uint32_t a, bool *ok) {
    uint32_t v = 0;
    if (!qrx_full_bus_read(c, a, 4, &v)) { if (ok) *ok = false; return 0; }
    return v;
}
static uint32_t kick_rd8(QRXFullCPU *c, uint32_t a, bool *ok) {
    uint32_t v = 0;
    if (!qrx_full_bus_read(c, a, 1, &v)) { if (ok) *ok = false; return 0; }
    return v & 0xFFu;
}
static bool kick_wr32(QRXFullCPU *c, uint32_t a, uint32_t v) {
    return qrx_full_bus_write(c, a, 4, v);
}
static bool kick_wr8(QRXFullCPU *c, uint32_t a, uint32_t v) {
    return qrx_full_bus_write(c, a, 1, v & 0xFFu);
}
static bool kick_name_is(QRXFullCPU *c, uint32_t nameptr, const char *want) {
    /* Names may live in slow RAM or in the Kickstart ROM. */
    bool inram = nameptr >= 0x00C00000u && nameptr < 0x00DC0000u;
    bool inrom = nameptr >= 0x00F80000u && nameptr < 0x01000000u;
    if (!nameptr || (!inram && !inrom)) return false;
    for (int i = 0; want[i]; i++) {
        bool ok = true;
        uint32_t ch = kick_rd8(c, nameptr + (uint32_t)i, &ok);
        if (!ok || ch != (uint32_t)(uint8_t)want[i]) return false;
    }
    return true;
}
/* Exec Bootstrap Task wait fix: the coldstart bootstrap task parks in
 * WaitPort() on its reply signal. Observed at the stall: tc_SigWait was
 * written once as $00008000 (PC=$F8A37C, instr 12 710 276) and tc_SigRecvd
 * was never written again; the TCB has SPLower=NULL / SPUpper=$FFFFFFFF,
 * exactly how exec_init.c creates the boot task. No guest device ever
 * delivers the reply signal, so the ROM never resumes and idles forever.
 * When the classic idle stall is seen, deliver the simulator's missing
 * kick for the boot task only:
 *  a) a completed message already queued on a reply port owned by the
 *     task whose Signal was lost, or
 *  b) an armed timer.device TR_ADDREQUEST owned by the task, completed
 *     like the device-task path in stuck_kick below (no data semantics:
 *     ports/units wait, io_Error=0).
 * Either way: port reply bookkeeping, Signal(sigwait), TS_READY and
 * Ready-enqueue so the dispatcher picks the task up again.
 * Env QRX_FULL_STUCKKICK=0 disables this too. */
static void kick_irq_pulse(QRXFullCPU *c);
static bool kick_ready_enq(QRXFullCPU *c, uint32_t td, uint32_t sb);
static bool kick_ready_orphan_resweep(QRXFullCPU *c, uint32_t sb);
static void boot_reply_wake(QRXFullCPU *c, uint32_t sb) {
    static bool bannounced;
    static bool btraced, bart;
    bool ok = true;
    uint32_t ready_head = kick_rd32(c, sb + 406u, &ok);
    uint32_t ready_tail = kick_rd32(c, sb + 410u, &ok);
    if (!ok || ready_head != sb + 410u || ready_tail != 0) return;
    /* Bootstrap task: NT_TASK, TS_WAIT, SPLower==0, SPUpper==$FFFFFFFF.
     * Only exec's boot task is created exactly that way. */
    uint32_t boot = 0, sigwait = 0, sigrec = 0;
    for (uint32_t a = 0x00C00000u; a < 0x00DC0000u; a += 4) {
        ok = true;
        if (kick_rd8(c, a + 8u, &ok) != 1) continue;
        ok = true;
        if (kick_rd8(c, a + 15u, &ok) != 4) continue;
        ok = true;
        if (kick_rd32(c, a + 0x3Au, &ok) != 0) continue;
        ok = true;
        if (kick_rd32(c, a + 0x3Eu, &ok) != 0xFFFFFFFFu) continue;
        ok = true;
        uint32_t sw = kick_rd32(c, a + 0x16u, &ok);
        if (!ok || !sw) continue;
        ok = true;
        uint32_t sr = kick_rd32(c, a + 0x1Au, &ok);
        if (!ok || (sr & sw)) continue; /* already signalled */
        boot = a; sigwait = sw; sigrec = sr; break;
    }
    if (!boot) {
        if (env_QRX_FULL_BOOTKICK_TRACE && !btraced) {
            btraced = true;
            fprintf(stderr, "[BOOTKICK] no bootstrap task found (ready=%08x/%08x sb=%08x)\n",
                ready_head, ready_tail, sb);
        }
        return;
    }
    if (env_QRX_FULL_BOOTKICK_TRACE && !btraced) {
        btraced = true;
        fprintf(stderr, "[BOOTKICK] boot=%08x st=%02x sigwait=%08x sigrec=%08x ready empty\n",
            boot, kick_rd8(c, boot + 15u, &ok), sigwait, sigrec);
        /* Enumerate every port owned by the boot task + queued msgs.
         * NOTE: TCBs contain stray boot-task pointers, so candidate ports
         * include junk; scan the full slow-RAM range (cap 64) before
         * drawing conclusions. */
        unsigned np = 0;
        for (uint32_t p = 0x00C00000u; p < 0x00DC0000u && np < 64; p += 4) {
            ok = true;
            if (kick_rd32(c, p + 16u, &ok) != boot || !ok) continue;
            ok = true;
            uint32_t head = kick_rd32(c, p + 20u, &ok);
            ok = true;
            uint32_t ltail = kick_rd32(c, p + 24u, &ok);
            ok = true;
            uint8_t sigb = kick_rd8(c, p + 15u, &ok);
            ok = true;
            uint8_t mtype = (ok && head != p + 24u && ltail == 0) ?
                (uint8_t)kick_rd8(c, head + 8u, &ok) : 0;
            ok = true;
            fprintf(stderr, "[BOOTKICK] port=%08x flags=%02x sigbit=%02u head=%08x tail=%08x msgt=%02x\n",
                p, kick_rd8(c, p + 14u, &ok), sigb, head, ltail, mtype);
            ok = true;
            np++;
        }
        /* Messages anywhere in RAM with mn_ReplyPort owned by the task. */
        unsigned nm = 0;
        for (uint32_t m = 0x00C00000u; m < 0x00DC0000u && nm < 64; m += 4) {
            ok = true;
            uint8_t t = kick_rd8(c, m + 8u, &ok);
            if (!ok || (t != 5 && t != 7)) continue;
            ok = true;
            uint32_t rp = kick_rd32(c, m + 14u, &ok);
            if (!ok || !rp || rp < 0x00C00000u || rp >= 0x00DC0000u) continue;
            ok = true;
            if (kick_rd32(c, rp + 16u, &ok) != boot) continue;
            ok = true;
            fprintf(stderr, "[BOOTKICK] msg=%08x type=%02u replyport=%08x node=%08x,%08x io_dev=%08x cmd=%04x\n",
                m, t, rp, kick_rd32(c, m, &ok), kick_rd32(c, m + 4u, &ok),
                kick_rd32(c, m + 20u, &ok),
                (uint16_t)(ok ? kick_rd32(c, m + 28u, &ok) & 0xFFFFu : 0));
            nm++;
        }
    }
    /* a) completed message queued on a reply port owned by the task. */
    uint32_t port = 0, req = 0, cport = 0, cmsg = 0;
    uint8_t cmsgt = 0;
    for (uint32_t p = 0x00C00000u; p < 0x00DC0000u && !req; p += 4) {
        ok = true;
        if (kick_rd32(c, p + 16u, &ok) != boot) continue;
        ok = true;
        uint32_t head = kick_rd32(c, p + 20u, &ok);
        ok = true;
        uint32_t ltail = kick_rd32(c, p + 24u, &ok);
        if (!ok) continue;
        if (!cport) {
            cport = p;
            cmsg = head;
            if (ltail != 0) cmsgt = 0xFF;
            else if (head == p + 24u) cmsgt = 0xFE;
            else if (kick_rd8(c, head + 8u, &ok) && ok) cmsgt = (uint8_t)(ok ? kick_rd8(c, head + 8u, &ok) : 0);
        }
        if (ltail != 0 || head == p + 24u) continue; /* empty or junk */
        ok = true;
        if (kick_rd8(c, head + 8u, &ok) != 7) continue; /* NT_REPLYMSG only */
        port = p; req = head;
    }
    /* b) armed timer.device TR_ADDREQUEST owned by the task. */
    if (!req) {
        uint32_t timerbase = 0;
        for (uint32_t a = 0x00C00000u; a < 0x00DC0000u; a += 4) {
            ok = true;
            uint32_t t = kick_rd8(c, a + 8u, &ok);
            ok = true;
            uint32_t nm = kick_rd32(c, a + 10u, &ok);
            if (!ok || t != 3 || !kick_name_is(c, nm, "timer.device")) continue;
            timerbase = a; break;
        }
        if (timerbase) for (uint32_t a = 0x00C00000u; a < 0x00DC0000u; a += 4) {
            ok = true;
            if (kick_rd8(c, a + 8u, &ok) != 5) continue;
            ok = true;
            if (kick_rd32(c, a + 20u, &ok) != timerbase) continue;
            ok = true;
            uint32_t cmd = 0;
            (void)qrx_full_bus_read(c, a + 28u, 2, &cmd);
            if ((cmd & 0xFFFFu) != 9) continue; /* TR_ADDREQUEST */
            ok = true;
            uint32_t p = kick_rd32(c, a + 14u, &ok);
            if (!ok || !p) continue;
            ok = true;
            if (kick_rd32(c, p + 16u, &ok) != boot) continue;
            ok = true;
            uint32_t rpred = kick_rd32(c, a + 4u, &ok);
            uint32_t rsucc = kick_rd32(c, a, &ok);
            if (!ok || rpred == rsucc) continue; /* must be linked */
            port = p; req = a; break;
        }
    }
    if (!req) {
        if (env_QRX_FULL_BOOTKICK_TRACE && !bart) {
            bart = true;
            /* No completed reply queued yet and no armed timer request:
             * waking now would fake a DoIO reply (WaitPort returns before
             * GetMsg finds anything). Wait until the device actually
             * completes its work. */
            fprintf(stderr, "[BOOTKICK] boot=%08x sigwait=%08x no queued reply yet — waiting for device\n",
                boot, sigwait);
        }
        return;
    }
    if (!bannounced) { bannounced = true;
        fprintf(stderr, "[KICK] boot task=%08x sigwait=%08x req=%08x port=%08x sigrec=%08x\n",
            boot, sigwait, req, port, sigrec); }
    /* Re-validate the reply port owner before writing (reply case only). */
    if (req) {
        ok = true;
        if (kick_rd32(c, port + 16u, &ok) != boot || !ok) return;
    }
    /* Generic timer completion path: unlink from the timer unit list,
     * mark NT_REPLYMSG + io_Error=0 and queue on the reply port. For the
     * already-replied case (req found via a) nothing to do. */
    {
        ok = true;
        uint8_t ltype = kick_rd8(c, req + 8u, &ok);
        if (!ok && req) return;  /* unreadable req is fatal; req==0 is the plain-signal path */
        if (req && ltype != 7) {
            ok = true;
            uint32_t rpred = kick_rd32(c, req + 4u, &ok);
            uint32_t rsucc = kick_rd32(c, req, &ok);
            if (!ok) return;
            if (!kick_wr32(c, rpred, rsucc) || !kick_wr32(c, rsucc + 4u, rpred)) return;
            if (!kick_wr8(c, req + 8u, 7) || !kick_wr8(c, req + 31u, 0)) return;
            if (!kick_wr32(c, port + 20u, req) || !kick_wr32(c, req, port + 24u) ||
                !kick_wr32(c, req + 4u, port + 20u) || !kick_wr32(c, port + 28u, req)) return;
        }
    }
    /* Wake: TS_READY, Signal(sigwait), Ready-enqueue (proper tail splice;
     * the old blank lh_Head write orphaned tasks enqueued by earlier
     * sweeps). */
    if (!kick_wr8(c, boot + 15u, 3)) return;
    if (!kick_wr32(c, boot + 0x1Au, sigrec | sigwait)) return;
    if (!kick_ready_enq(c, boot, sb)) return;
    /* QuantumOver so user-IRQ server chains can run (same as stuck_kick). */
    uint32_t sf = kick_rd8(c, sb + 293u, &ok);
    if (!ok || !kick_wr8(c, sb + 293u, sf | 0x20u)) return;
    fprintf(stderr, "[KICK] boot wake done src=%s req=%08x port=%08x task=%08x sigrecvd=%08x\n",
        req ? "reply" : "signal", req, port, boot, sigrec | sigwait);
    /* Deliver a level-3 autovector so the ROM idle kernel exits STOP and
     * dispatches the now-READY task; WITHOUT this the 68040 sits in
     * STOP #$2000 forever even though TaskReady is non-empty. */
    kick_irq_pulse(c);
}
/* Raise one level-3 autovector pulse for a persisted BOOT-wake: the ROM
 * dispatcher runs, finds a READY task and dispatches it; int_ack drops the
 * line before the exec handler returns. */
static void kick_irq_pulse(QRXFullCPU *c) {
    if (!c || c != active) return;
    if (c->kick_pending) return; /* already pending */
    c->kick_pending = 1u;
    if (env_QRX_FULL_KICK_TRACE)
        fprintf(stderr, "[KICK] irq pulse lvl=3 PC=%08x instr=%llu\n",
            m68k_get_reg(NULL, M68K_REG_PC), (unsigned long long)c->instructions);
    qrx_full_irq(c, 3);
}
/* Complete one armed timer request for whichever task owns its reply
 * port: unlink from the timer unit list, mark NT_REPLYMSG + io_Error=0,
 * queue on the reply port, wake the task (TS_READY + Signal), Ready-enqueue,
 * QuantumOver, and pulse level-3 so the exec idle STOP dispatches it. */
static bool kick_timer_task(QRXFullCPU *c, uint32_t req, uint32_t sb) {
    static bool announced;
    bool ok = true;
    uint32_t port = kick_rd32(c, req + 14u, &ok);
    if (!ok || !port) return false;
    uint32_t td = kick_rd32(c, port + 16u, &ok);      /* owner task */
    if (!ok || td < 0x00C00000u || td >= 0x00DC0000u) return false;
    uint8_t st = kick_rd8(c, td + 15u, &ok);
    if (!ok || st != 4) return false;                  /* must be WAITing */
    uint32_t rpred = kick_rd32(c, req + 4u, &ok);
    uint32_t rsucc = kick_rd32(c, req, &ok);
    if (!ok || rpred == rsucc) return false;           /* not armed/linked */
    uint32_t sigrec = kick_rd32(c, td + 26u, &ok);
    uint32_t sigwait = kick_rd32(c, td + 22u, &ok);
    if (!ok) return false;
    if (!announced) { announced = true;
        fprintf(stderr, "[KICK] timer-completion path armed req=%08x port=%08x task=%08x sigwait=%08x\n",
            req, port, td, sigwait); }
    /* 1. unlink request from timer unit list (generic Remove) */
    if (!kick_wr32(c, rpred, rsucc) || !kick_wr32(c, rsucc + 4u, rpred)) return false;
    /* 2. reply it: NT_REPLYMSG + no error */
    if (!kick_wr8(c, req + 8u, 7) || !kick_wr8(c, req + 31u, 0)) return false;
    /* 3. queue on reply port (list known empty: Head=&Tail) */
    if (!kick_wr32(c, port + 20u, req) || !kick_wr32(c, req, port + 24u) ||
        !kick_wr32(c, req + 4u, port + 20u) || !kick_wr32(c, port + 28u, req)) return false;
    /* 4. wake task: TS_READY, signal, Ready-enqueue (proper tail splice) */
    if (!kick_wr8(c, td + 15u, 3)) return false;
    if (!kick_wr32(c, td + 26u, sigrec | (sigwait ? sigwait : 0x40000u))) return false;
    if (!kick_ready_enq(c, td, sb)) return false;
    /* 5. QuantumOver so user-IRQ server chains can run */
    uint32_t sf = kick_rd8(c, sb + 293u, &ok);
    if (!ok || !kick_wr8(c, sb + 293u, sf | 0x20u)) return false;
    fprintf(stderr, "[KICK] done req=%08x task=%08x ready sigrecvd=%08x\n",
        req, td, sigrec | (sigwait ? sigwait : 0x40000u));
    kick_irq_pulse(c); /* same as boot_reply_wake: exit the exec idle STOP */
    return true;
}
static void kick_timer_sweep(QRXFullCPU *c, uint32_t sb);
static void kick_reply_sweep(QRXFullCPU *c, uint32_t sb);
/* Wake a task that is blocked in a Wait() although the event it waits for
 * has already happened. Verified DoIO/WaitIO contract: once rk dispatched,
 * WaitIO's loop just re-checks mn_Type == NT_REPLYMSG and does the port
 * unlink itself — the Wait() mask therefore does not need to have the
 * correct bit for the I/O call to complete. */
/* Walk the TaskReady list (max 16 entries) to see whether td is already
 * queued. A plain lh_Head overwrite by a later sweep previously orphaned
 * an earlier READY task: the node kept its one-node links (ln_Pred=&lh_Head,
 * ln_Succ=&lh_Tail) while lh_Head/lh_TailPred no longer referenced it, so
 * exec's idle dispatcher ($FE7F94) never found it again — the final
 * boot-TCB state in the v7 dump. */
static bool kick_ready_contains(QRXFullCPU *c, uint32_t sb, uint32_t td) {
    bool ok = true;
    uint32_t n = kick_rd32(c, sb + 406u, &ok);
    if (!ok) return false;
    for (unsigned i = 0; i < 16u; i++) {
        if (n == td) return true;
        if (n == sb + 410u) return false;                 /* &lh_Tail: end */
        if (n < 0x00C00000u || n >= 0x00DC0000u) return false;
        n = kick_rd32(c, n, &ok);
        if (!ok) return false;
    }
    return false;
}
/* Unlink td from wherever its links claim membership and properly
 * tail-append it onto the TaskReady list. Neighbors are only rewired when
 * they still reference td back (lh_Head/A-node points at us), so a stale
 * first/last reference of an orphaned READY node can never clobber a live
 * queue that has since gained another head. */
static bool kick_ready_enq(QRXFullCPU *c, uint32_t td, uint32_t sb) {
    bool ok = true;
    uint32_t tpred = kick_rd32(c, td + 4u, &ok);
    uint32_t tsucc = kick_rd32(c, td, &ok);
    if (!ok) return false;
    if (tpred != tsucc && tpred >= 0x00C00000u && tpred < 0x00DC0000u &&
        tsucc >= 0x00C00000u && tsucc < 0x00DC0000u) {
        { /* pred->succ = td? (sentinel fields read like nodes) */
            bool q = true;
            uint32_t ps = kick_rd32(c, tpred, &q);
            if (q && ps == td && !kick_wr32(c, tpred, tsucc)) return false;
        }
        { /* succ->pred = td? */
            bool q = true;
            uint32_t sp = kick_rd32(c, tsucc + 4u, &q);
            if (q && sp == td && !kick_wr32(c, tsucc + 4u, tpred)) return false;
        }
    }
    ok = true;
    uint32_t last = kick_rd32(c, sb + 414u, &ok);        /* TailPred: last node or &lh_Head */
    if (!ok) return false;
    if (!kick_wr32(c, td, sb + 410u)) return false;       /* succ = &lh_Tail sentinel */
    if (!kick_wr32(c, td + 4u, last)) return false;       /* pred = old last */
    if (!kick_wr32(c, last, td)) return false;            /* old last.succ = td (empty: lh_Head = td) */
    if (!kick_wr32(c, sb + 414u, td)) return false;       /* lh_TailPred = td */
    return true;
}
/* Rescue READY tasks that were orphaned out of TaskReady by the pre-fix
 * blank lh_Head writes: node carries one-node membership links
 * (ln_Pred=&lh_Head, ln_Succ=&lh_Tail) but lh_Head no longer reaches it.
 * Signature-checked (NT_TASK, TS_READY, sentinel links, valid name ptr);
 * anything else leaves its links alone. */
static bool kick_ready_orphan_resweep(QRXFullCPU *c, uint32_t sb) {
    bool ok = true;
    bool fixed = false;
    for (uint32_t a = 0x00C00000u; a < 0x00DC0000u; a += 4) {
        ok = true;
        if (kick_rd32(c, a, &ok) != sb + 410u) continue;   /* ln_Succ == &lh_Tail */
        ok = true;
        if (kick_rd32(c, a + 4u, &ok) != sb + 406u) continue; /* ln_Pred == &lh_Head */
        ok = true;
        if (kick_rd8(c, a + 8u, &ok) != 1) continue;       /* NT_TASK */
        ok = true;
        if (kick_rd8(c, a + 15u, &ok) != 3) continue;      /* TS_READY */
        ok = true;
        uint32_t nameptr = kick_rd32(c, a + 10u, &ok);
        bool ninrange = ok && nameptr &&
            ((nameptr >= 0x00C00000u && nameptr < 0x00DFFFFFu) ||
             (nameptr >= 0x00F80000u && nameptr < 0x01000000u));
        if (!ninrange) continue;
        if (kick_ready_contains(c, sb, a)) continue;       /* genuinely queued */
        /* Stale neighbor references may exist; kick_ready_enq only repairs
         * back-referencing neighbors, so re-append is safe. */
        if (!kick_ready_enq(c, a, sb)) continue;
        ok = true;
        uint32_t sf = kick_rd8(c, sb + 293u, &ok);
        if (ok) (void)kick_wr8(c, sb + 293u, sf | 0x20u);
        fprintf(stderr, "[KICK] orphan rescue enq task=%08x st=3\n", a);
        fixed = true;
    }
    if (fixed) kick_irq_pulse(c);
    return fixed;
}
static void kick_ready_task(QRXFullCPU *c, uint32_t td, uint32_t sb, uint32_t sigbits) {
    bool ok = true;
    if (kick_rd8(c, td + 15u, &ok) != 4) return;          /* only WAITing tasks */
    if (kick_ready_contains(c, sb, td)) return;           /* already queued */
    uint32_t sigrec = kick_rd32(c, td + 26u, &ok);
    uint32_t sigwait = kick_rd32(c, td + 22u, &ok);
    if (!ok) return;
    if (!kick_wr8(c, td + 15u, 3)) return;
    if (!kick_wr32(c, td + 26u, sigrec | (sigwait ? sigwait : sigbits))) return;
    if (!kick_ready_enq(c, td, sb)) return;
    uint32_t sf = kick_rd8(c, sb + 293u, &ok);
    if (!ok || !kick_wr8(c, sb + 293u, sf | 0x20u)) return;
    fprintf(stderr, "[KICK] ready req=done task=%08x sigrecvd=%08x\n",
        td, sigrec | (sigwait ? sigwait : sigbits));
    kick_irq_pulse(c);
}
/* Sweep for Waiting tasks whose reply port has a completed NT_REPLYMSG
 * queued. This is the lowlevel.library DoIO(IND_ADDHANDLER) blockage: that
 * port was created with an invalid signal (mp_SigBit = -1), so the ROM's
 * Wait(1 << mp_SigBit) compiles to Wait(0) on the 68040 (LSL.L count>32
 * yields 0) and no host-side 'timer kick' can ever clear it. */
static void kick_reply_sweep(QRXFullCPU *c, uint32_t sb) {
    bool ok = true;
    unsigned woke = 0;
    for (uint32_t p = 0x00C00000u; p < 0x00DC0000u && woke < 4; p += 4) {
        ok = true;
        uint8_t t = kick_rd8(c, p + 8u, &ok);
        if (!ok || t != 4) continue;              /* NT_MSGPORT */
        uint32_t td = kick_rd32(c, p + 16u, &ok);
        if (!ok || td < 0x00C00000u || td >= 0x00DC0000u) continue;
        uint8_t st = kick_rd8(c, td + 15u, &ok);
        if (!ok || st != 4) continue;             /* TS_WAIT only */
        uint32_t head = kick_rd32(c, p + 20u, &ok);
        if (!ok || head == p + 24u || head == p + 28u) continue;
        ok = true;
        if (kick_rd8(c, head + 8u, &ok) != 7) continue; /* NT_REPLYMSG */
        /* Only wake if the request was sent by THIS task and already
         * merely pines for the missed Wait() signal, i.e. io_Error is 0. */
        ok = true;
        (void)kick_rd8(c, head + 31u, &ok);
        if (!ok) continue;
        kick_ready_task(c, td, sb, 0x00200000u);
        woke++;
    }
    if (env_QRX_FULL_KICK_TRACE && woke)
        fprintf(stderr, "[KICK] reply sweep woke=%u instr=%llu\n",
            woke, (unsigned long long)c->instructions);
}
/* Complete up to 4 armed timer.device TR_ADDREQUESTs owned by tasks that
 * are WAITing. This covers every parker whose exec idle STOP froze the
 * whole ROM boot when only ThisTask's own poll timer was being served. */
static void kick_timer_sweep(QRXFullCPU *c, uint32_t sb) {
    bool ok = true;
    uint32_t timerbase = 0;
    for (uint32_t a = 0x00C00000u; a < 0x00DC0000u; a += 4) {
        bool q = true;
        uint32_t t = kick_rd8(c, a + 8u, &q);
        uint32_t nm = kick_rd32(c, a + 10u, &q);
        if (!q || t != 3 || !kick_name_is(c, nm, "timer.device")) continue;
        timerbase = a; break;
    }
    /* reply-восточка percorso: fixing the "Wait(0)" parker even before the
     * timer device path. */
    kick_reply_sweep(c, sb);
    unsigned served = 0;
    for (uint32_t a = 0x00C00000u; a < 0x00DC0000u && served < 4; a += 4) {
        bool q = true;
        if (kick_rd8(c, a + 8u, &q) != 5) continue;              /* NT_MESSAGE */
        uint32_t dev = kick_rd32(c, a + 20u, &q);
        if (!q || dev != timerbase) continue;
        uint32_t cmd = 0; (void)qrx_full_bus_read(c, a + 28u, 2, &cmd);
        if ((cmd & 0xFFFFu) != 9) continue;                       /* TR_ADDREQUEST */
        uint32_t port = kick_rd32(c, a + 14u, &q);
        if (!q || !port) continue;
        if (kick_timer_task(c, a, sb)) served++;
    }
    if (env_QRX_FULL_KICK_TRACE && served)
        fprintf(stderr, "[KICK] sweep served=%u instr=%llu\n",
            served, (unsigned long long)c->instructions);
}
static void stuck_kick(QRXFullCPU *c, uint32_t pc) {
    static uint32_t prev_kick_pc;
    static uint32_t last_pc;
    static unsigned same;
    static bool disabled;
    static bool announced;
    if (!disabled) {
        const char *e = env_QRX_FULL_STUCKKICK;
        if (!e || strcmp(e, "1")) disabled = true;
    }
    if (disabled || !amiga_bus(c) || c->instructions < 5000000u) return;
    /* Supervisor trapper loop (FE6830..FE6870: IPL7, JSR idle-sub, STOP, BRA)
     * plus the idle subroutine it calls. */
    if ((pc < 0x00FE6830u || pc >= 0x00FE6870u) &&
        (pc < 0x00FE7F60u || pc >= 0x00FE7F80u)) { same = 0; last_pc = pc; return; }
    if (pc != last_pc) { same = 0; last_pc = pc; return; }
    if (++same < 4) return; /* 4 consecutive idle frames before touching RAM */
    same = 0;
    bool ok = true;
    uint32_t sb = kick_rd32(c, 4, &ok);
    if (!ok || sb < 0x00C00000u || sb >= 0x00DC0000u) return;
    /* The stalled CPU's ThisTask is whatever device task was dispatched
     * last, never the parked bootstrap task — so run the boot-task reply
     * wake before the ThisTask-name gate. */
    boot_reply_wake(c, sb);
    kick_ready_orphan_resweep(c, sb);
    uint32_t td = kick_rd32(c, sb + 276u, &ok); /* ThisTask */
    if (!ok || td < 0x00C00000u || td >= 0x00DC0000u) return;
    /* Name gate dropped: after the first idle STOP phase the pooled name
     * strings no longer match (TCBs get re-pooled); the decisive gate is
     * "READY queue empty + ThisTask waiting + armed timer request owned". */
    uint32_t ready_head = kick_rd32(c, sb + 406u, &ok);
    uint32_t ready_tail = kick_rd32(c, sb + 410u, &ok);
    if (!ok || ready_head != sb + 410u || ready_tail != 0) return; /* Ready not empty: progress */
    if (kick_rd8(c, td + 15u, &ok) != 4) {
        if (env_QRX_FULL_KICK_TRACE)
            fprintf(stderr, "[KICK] exit state byte=%u instr=%llu\n",
                ok ? kick_rd8(c, td + 15u, &ok) : 0xFFu,
                (unsigned long long)c->instructions);
        return;
    } /* task not waiting */
    if (!ok) return;
    /* Find this task's armed timer request: NT_MESSAGE + timer.device + TR_ADDREQUEST. */
    uint32_t timerbase = 0;
    for (uint32_t a = 0x00C00000u; a < 0x00DC0000u; a += 4) {
        bool q = true;
        uint32_t t = kick_rd8(c, a + 8u, &q);
        uint32_t nm = kick_rd32(c, a + 10u, &q);
        if (!q || t != 3 || !kick_name_is(c, nm, "timer.device")) continue;
        timerbase = a; break;
    }
    if (!timerbase) { fprintf(stderr, "[KICK] no timer.device\n"); return; }
    uint32_t req = 0;
    for (uint32_t a = 0x00C00000u; a < 0x00DC0000u; a += 4) {
        bool q = true;
        if (kick_rd8(c, a + 8u, &q) != 5) continue;                    /* NT_MESSAGE */
        if (kick_rd32(c, a + 20u, &q) != timerbase) continue;         /* io_Device */
        uint32_t cmd = 0; (void)qrx_full_bus_read(c, a + 28u, 2, &cmd);
        if ((cmd & 0xFFFFu) != 9) continue;                           /* TR_ADDREQUEST */
        uint32_t port = kick_rd32(c, a + 14u, &q);                    /* reply port */
        if (!q || !port) continue;
        bool p = true;
        if (kick_rd32(c, port + 16u, &p) != td || !p) continue;       /* owned by idle task */
        req = a; break;
    }
    if (!req) { fprintf(stderr, "[KICK] no armed timer req for task %08x\n", td); kick_timer_sweep(c, sb); return; }
    kick_timer_task(c, req, sb);
    /* Even after ThisTask's own timer is served, other device/boot tasks
     * (dosboot retry delay, gameport polling, ...) can be parked on their
     * own armed timer requests. Complete those too or the system-wide
     * idle STOP livelocks on a single task's poll loop. */
    kick_timer_sweep(c, sb);
}
/* Denise VBLANK: on each new PAL frame, raise level-3 autovector while the
 * guest has master+VERTB enabled in the INTENA shadow. The guest's VertB
 * server deasserts via its INTREQ ack write. */
static void vblank_tick(QRXFullCPU *c) {
    if (!amiga_bus(c) || c->aux_cpu_index != 0) return;
    uint64_t f = beam_frame_idx(c);
    if (f == c->beam_frame) return;
    c->beam_frame = f;
    (void)0; /* marker for a later real Paula model */
    fl_hdpoke(c);
    fl_dma_progress(c);
    { uint32_t pcnow = m68k_get_reg(NULL, M68K_REG_PC);
      stuck_kick(c, pcnow);
      if (env_QRX_FULL_PHIST && c->instructions > 5000000u) {
        static uint32_t hist[65536]; static uint64_t nframes;
        /* 400 frames (~10s @ ~40fps kick rate) so idle-loop histograms are
         * visible in short runs; the 2000 frame interval used to swallow
         * whole 120M-cycle runs silently. */
        hist[(pcnow >> 8) & 0xFFFFu]++;
        if (++nframes >= 400) {
            nframes = 0;
            fprintf(stderr, "[PHIST] instr=%llu top:", (unsigned long long)c->instructions);
            for (int k = 0; k < 12; k++) {
                unsigned bi = 0; uint32_t bv = 0;
                for (unsigned i = 0; i < 65536; i++) if (hist[i] > bv) { bv = hist[i]; bi = i; }
                if (!bv) break; hist[bi] = 0;
                fprintf(stderr, " %05xxx:%u", bi, bv);
            }
            fprintf(stderr, "\n");
        }
      } }
    fsaga_present(c);
    if (c->vblank_asserted) return;
    if ((c->intena & 0x4020u) != 0x4020u) return;
    c->vblank_asserted = 1;
    if (env_QRX_FULL_VBLANK_TRACE)
        fprintf(stderr, "[VBLANK] assert frame=%llu intena=%04x instr=%llu\n",
            (unsigned long long)f, c->intena, (unsigned long long)c->instructions);
    paula_recompute(c);
}
// FSAGA display hookup, standard for the 2D RTG surface
void qrx_full_set_display(QRXFullCPU *c, QRXDisplayFn fn, void *opaque) {
    if (!c) return;
    c->display = fn;
    c->display_arg = opaque;
}
void qrx_full_fsaga_present(QRXFullCPU *c) { if (c) fsaga_present(c); }
static void timer_tick(QRXFullCPU *c,unsigned cycles){
    if(!(c->timer_control&QRX_FULL_TIMER_ENABLE)||!c->timer_period||c->irq_pending)return;
    if(cycles < c->timer_remaining) {c->timer_remaining-=cycles;return;}
    c->irq_pending=1;c->timer_remaining=c->timer_period;
    if(!(c->timer_control&QRX_FULL_TIMER_AUTO))c->timer_control&=~QRX_FULL_TIMER_ENABLE;
    m68k_set_irq(c->irq_level>2 ? c->irq_level : 2);
}
static uint64_t host_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
}
void qrx_full_set_realtime(QRXFullCPU *c, bool (*cpu_stopped)(void)) {
    if(!c)return;
    c->realtime=true;
    c->cpu_stopped=cpu_stopped;
    c->realtime_base_us=host_us();
}
/* PAL master rate used by the beam (454 x 313 x 50) and the CIAs (/10). */
#define QRX_REALTIME_HZ UINT64_C(7105000)
static inline void realtime_clock(QRXFullCPU *c) {
    uint64_t now=(host_us()-c->realtime_base_us)*QRX_REALTIME_HZ/1000000u;
    if(now>c->cycles)c->cycles=now;
}
int qrx_full_run_cycles(QRXFullCPU *c,uint64_t budget){
    if(!c||active!=c||!c->reset_done||!budget)return -1;    uint64_t elapsed=0;
    /* Longer slices without the JIT would change Musashi's deterministic trace. */
    const uint64_t max_slice=c->jit_exec?4096u:256u;
    while(elapsed<budget && !c->vm.cpu.halted && !c->mem_fault) {
        int slice=(int)((budget-elapsed)>max_slice?max_slice:(budget-elapsed));
        int taken=c->jit_exec?c->jit_exec(c,slice):m68k_execute(slice);
        if(taken<0) {c->mem_fault=true;break;}
        if(taken==0) {
            /* STOP with no active IRQ: the beam must NOT freeze — advance idle
             * cycles and keep vblank_tick running so the host-side boot kick
             * can pulse the exec idle kernel out of STOP (a task is READY and
             * dispatchable). Without this the 68040 stays in STOP #$2000
             * forever right at PC=$FE6866, exactly the historical boot stall → */
            elapsed += (uint64_t)slice; c->cycles += (uint64_t)slice;
            if(c->aux_cpu_index==0)timer_tick(c,(unsigned)slice);
            if(amiga_bus(c))boot_cia_tick(c);
            fl_dma_progress(c);
            vblank_tick(c);
            continue;
        }
        elapsed+=(unsigned)taken;
        if(c->realtime)realtime_clock(c);else c->cycles+=(unsigned)taken;
        if(c->aux_cpu_index==0)timer_tick(c,(unsigned)taken);
        if(amiga_bus(c))boot_cia_tick(c);
        fl_dma_progress(c);
        vblank_tick(c);
        /* An idle guest waits for the next timer on the host's own clock and
         * hands the caller its turn, instead of burning a core in STOP. */
        if(c->realtime&&c->cpu_stopped&&c->cpu_stopped()){
            struct timespec nap={0,150000};
            nanosleep(&nap,NULL);
            break;
        }
        if(elapsed>budget+4096u)break; /* instruction granularity overshoot guard */
    }
    if(c->mem_fault)return -1;
    return c->vm.cpu.halted ? 0 : 1;
}
bool qrx_full_run_subroutine(QRXFullCPU *c,uint32_t entry,
                             uint64_t cycle_budget,uint64_t *retired){
    /* The firmware's guest stack and return trampoline are not host pointers.
       The sentinel is inside the reserved boot region above the loaded app. */
    const uint32_t sentinel=0x1e000u;
    if(!c||active!=c||!c->reset_done||c->subroutine_armed||
       !cycle_budget||cycle_budget>100000000u||entry&1u||
       (uint64_t)entry+2>c->vm.ram.size||
       (uint64_t)sentinel+2>c->vm.ram.size)return false;
    uint32_t oldpc=m68k_get_reg(NULL,M68K_REG_PC);
    uint32_t oldsp=m68k_get_reg(NULL,M68K_REG_A7);
    if(oldsp<4u||(oldsp&1u)||oldsp>c->vm.ram.size)return false;
    uint32_t saved_sentinel=0;
    if(!qrx_full_bus_read(c,sentinel,2,&saved_sentinel)||
       !qrx_full_bus_write(c,sentinel,2,0x4e71u)||
       !qrx_full_bus_write(c,oldsp-4u,4,sentinel))return false;
    const uint64_t start=c->instructions;
    c->subroutine_sentinel=sentinel;
    c->subroutine_returned=false;
    c->subroutine_armed=true;
    m68k_set_reg(M68K_REG_A7,oldsp-4u);
    m68k_set_reg(M68K_REG_PC,entry);
    uint64_t consumed=0;
    while(consumed<cycle_budget && !c->mem_fault && !c->subroutine_returned){
        int sl=(int)((cycle_budget-consumed)>128u?128u:cycle_budget-consumed);
        int got=qrx_full_run_cycles(c,(unsigned)sl);
        if(got<0||got==2)break;
        consumed+=(unsigned)sl;
    }
    bool ok=c->subroutine_returned&&!c->mem_fault;
    if(retired)*retired=c->instructions-start;
    c->subroutine_armed=false;
    m68k_set_reg(M68K_REG_PC,oldpc);
    m68k_set_reg(M68K_REG_A7,oldsp);
    (void)qrx_full_bus_write(c,sentinel,2,saved_sentinel);
    return ok;
}
uint32_t qrx_full_pc(const QRXFullCPU *c){return c&&c==active?m68k_get_reg(NULL,M68K_REG_PC):0;}
uint32_t qrx_full_reg(const QRXFullCPU *c,unsigned r){return c&&c==active?m68k_get_reg(NULL,(m68k_register_t)r):0;}


void qrx_full_load_adf(QRXFullCPU *c, const void *data, size_t size) {    if (!c || !data || !size || size % 512u) { c->adf_data = NULL; c->adf_size = 0; return; }
    c->adf_data = (uint8_t *)data;
    c->adf_size = size;
    c->mfm_track = -1;
    if (env_QRX_FULL_FL_TRACE)
        fprintf(stderr, "[FL] ADF loaded: %zu bytes (%s disk)\n", (size_t)size,
            size == 1802240u ? "HD" : "DD");
    c->cia_icr_trace = env_QRX_FULL_ICR_TRACE != NULL;
    c->ciab_icr_trace = env_QRX_FULL_ICR_TRACE != NULL;
    c->ro_wronly = env_QRX_FULL_ROM_WRONLY != NULL;
    c->host_vb_enable = env_QRX_FULL_HOST_VB_COUNT != NULL;
}

void qrx_full_set_aux(QRXFullCPU *c, QRXAuxReadFn rd, QRXAuxWriteFn wr,
                      QRXRamWriteFn ram, void *opaque) {
    if (!c) return;
    c->aux_read=rd; c->aux_write=wr; c->ram_write=ram; c->aux_opaque=opaque;
    c->vm.dma_ram_write=ram; c->vm.dma_opaque=opaque;
    build_page_maps(c); /* board extensions observe every RAM write */
}
