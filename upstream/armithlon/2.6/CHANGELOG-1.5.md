# ARMITHLON 1.5

- QRX68100 formally defined as a **fictional dual-68040** CPU for QRX OS One, with CPU 0 in legacy 68040 mode and core 1 hidden until explicit QRX-enhanced initialization.
- Added two independent Musashi CPU contexts on a coherent common QRX board, using sequential deterministic context switches at instruction boundaries. No false claim of host parallel execution.
- Added SMP MMIO controls at `0xFF000600`, shared bus, IPI mailbox, deferred core1 startup/parking, core0 timer IRQ ownership and legacy AmigaOS 3.x mode. Old GPU and audio devices unchanged.
- Added a 2×2,048-entry, 4-way per-core metadata cache for JIT block identities: ISA tagging (68k/x86/PPC/x64/ARM), ASID segregation, per-page epochs, cross-core invalidation on writes, explicit DMA invalidation and full flush. **No executable native JIT added in this release.**
- Added guest SMP header, real-Musashi independent-core unit test and CLI demo; mock MMIO/cache tests in the ordinary build.
- The externally fetched upstream Musashi core remains pinned, unchanged and BSD/MIT-style. No false 68060 or AROS boot claims.
