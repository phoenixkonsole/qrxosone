# QRX68100 v1.5 — CPU design (fictional 2×68040)

## Hardware identity and compatibility

The **QRX68100 is a fictitious virtual 68k CPU**, NOT a Motorola product. It has two *independent* 68040 architectural contexts (D0–D7, A0–A7, PC, SR, VBR, FPU/MMU state as modeled by the selected Musashi backend), a single shared 32-bit address space and one virtual QRX board. A 68060 or Apollo 68080 is NOT implied.

### Legacy boot (default)

**`QRX_BOOT_LEGACY_AMIGA` (power-on default): only core 0 runs.** No second CPU is started, no guest secondary-core interrupt is injected, the new SMP registers are absent. An ordinary AmigaOS 3.x installation must never be asked to schedule across cores; its Exec scheduler and drivers are single-processor designs. This is CPU *exposure* compatibility, not a promise that stock AmigaOS 3.x boots on the chipset-less QRX board. For real AmigaOS 3.x the virtual chipset, autoconfig, CIA, interrupt and disk hardware also must be supplied, or the CPU must run under an existing Amiga emulator.

### QRX-enhanced boot

Only **host-specified** `QRX_BOOT_QRX_DUAL` enables new MMIO at `0xFF000600..0xFF00063F` (separate from QRXGPU +0x400 and QRX3D +0x500). The QRX kernel writes secondary 68040 stack and PC to its registers, then writes START=1. The start happens only at a CPU instruction boundary. Core 0 owns the existing timer IRQ; each core has its own pending IPI. A QRX-specific SMP kernel must add per-CPU run queues, spinlocks/atomics, exception stacks and cross-CPU TLB/cache coherency operations. These kernel facilities are **not implemented** here.

| Offset | Name | Direction | Meaning |
|---:|---|---|---|
| 0x00 | MAGIC | R | `0x51525844` QRXD |
| 0x04 | ABI | R | `0x00010000` |
| 0x08 | FEATURES | R | 2 CPU contexts, IPI, guest page coherence, ISA-tagged JIT block metadata |
| 0x0c | STATUS | R | 1=core0, 2=core1 online |
| 0x10 | CORE1_PC | RW | Secondary PC, even RAM address |
| 0x14 | CORE1_SP | RW | Secondary supervisor stack, even RAM address |
| 0x18 | CORE1_CTL | RW | 1=deferred start, 2=deferred park |
| 0x1c | IPI_SET | RW | target mask 1=core0, 2=core1 |
| 0x20 | IPI_ACK | RW | write 1 to acknowledge current CPU's IPI |
| 0x24 | CODE_GEN | R | Shared guest-code invalidation counter |
| 0x28 | JIT_HITS | R | Metadata cache hits, truncated to 32 bits |
| 0x2c | JIT_MISS | R | Metadata cache misses |
| 0x30 | CACHE_FLUSH | RW | Write 1 to invalidate all translation records |
| 0x34 | CORE_ID | R | Caller core ID |

Write operations to SMP controls are supervisor-only (SR.S=1), not user-mode callable. All registers are 32-bit aligned, big-endian. `CORE1_PC` must be valid RAM, `CORE1_SP` must be even and <= mapped RAM. The bootstrap CPU is mandatory; only core1 can be parked. Legacy mode hides this extension. Guest C bindings are in `engine/guest/qrx68100_smp.h`.

## JIT-oriented memory/cache architecture

The current implementation contains a **real host-side metadata cache**: 2 × 2,048 translation record slots, 4-way set associative, indexed by guest PC + address-space ID (ASID) + guest ISA tag; ISA tags 68k, x86, PPC, x86-64 and ARM32. Entries are limited to 128 guest bytes in one 4 KiB page. Shared per-page code epochs are incremented on every successful RAM write through the QRX bus; *both* cores' translations become stale after a code overwrite. Direct host DMA must call `qrx_dual_notify_ram_write` explicitly. Global flush invalidates all records. The cache distinguishes guest address spaces and architectures so x86 and PPC code at identical numerical addresses cannot be confused.

**It is not yet a native JIT or executable block cache**. Actual translated host instruction buffers, W^X allocation, page protection, translation flags, compiler workers, branch linking, code patching and native execution are future steps. Similarly, virtual L1I/L1D 32 KiB per core and shared L2 512 KiB are an **architectural target only**, not physically simulated or a measured speed-up. Actual Apple M1 cache sizes are properties of the host and cannot be changed by software. A fake L1 cache timer would not accelerate native code.

### Recommended path for future QRX86/PPC/x64/ARM execution

1. Keep guest CPU cores separate from the **cross-ISA translation runtime**, with ISA-tagged blocks and a common intermediate representation. Use the QRX86 guest to run x86 workloads now; PPC/x64/ARM are future translators, not current capabilities.
2. Promote validated IR blocks to native ARM64 machine code only after interpreter-vs-JIT differential tests. Use host W^X executable-memory APIs, correct instruction-cache invalidation and serializable exit/deoptimization records.
3. Keep hot code on a per-core L1 **translation** lookup (metadata), share immutable code pages where safe, and flush *all* aliases on writes/unmap. Add thread-safe compilation queues later; the current Musashi frontend has process-global state, so both CPU contexts are **deterministically time-sliced, not simultaneously executed on two host threads**.
4. For 68040 guest `TAS`/`CAS` and shared-memory synchronization, qualify atomicity against concurrent backends. Current time-slicing prevents simultaneous memory updates but does NOT provide correct multicore memory-ordering proofs.
5. Do not overstate benchmarking: use completed guest workloads, verified output hashes and wall-clock time on the actual M1 for each OS/ISA.

## Local tests

`./armithlon build && ./armithlon test` tests the virtual board using a fake Musashi implementation; it **does not validate execution of a real 68040**. To test real 68040 instructions and independent contexts, run `./armithlon full68k-setup && ./armithlon full68k-build && ./armithlon dualcore-test`; this downloads pinned Musashi. `./armithlon dualcore-demo` shows legacy single-core; `./armithlon dualcore-demo dual` enables QRX mode. The real Musashi code was not available in the current Linux container, so these *real CPU* tests remain pending until run on the user’s machine.

**Boot status:** current chipset-less QRX board still cannot boot stock AmigaOS 3.x or AROS Workbench. FS-UAE and the documented AROS compatibility path are separate. QRX OS One's SMP-aware BSP is yet to be implemented.
