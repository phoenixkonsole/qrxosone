# Changelog

## v0.2 — 2026-10-31

- Cumulative preservation of v0.1 tiny interpreter and ARM64 smoke JIT.
- Added QRX `qrxvirt` guest machine with verified big-endian RAM and reserved MMIO region, reset vectors, debug UART, monotonic tick, shutdown, 1-online/2-configured vCPU.
- Added limited practical 68k interpreter (control flow, stack/exception handling, selected arithmetic, immediates and memory operations).
- Added host CLI and synthetic ROM monitor, with step budget and raw-image support.
- Integrated opt-in AArch64 JIT into virtual VM execution for straight-line MOVEQ/NOP blocks, one-entry code cache, conservative invalidation on guest RAM writes. Native Apple Silicon execution untested in authoring environment.
- Added unit, differential, cache invalidation and virtual boot monitor tests; all passed on Linux x86-64 including ASan+UBSan.
- Added actual board ABI document and precise AROS bring-up handoff; no fabricated guest drivers.

## v0.1

- Proof-of-concept MOVEQ/NOP/TRAP15 interpreter and AArch64 microblock JIT with simple differential smoke test, original architecture/implementation documents.
