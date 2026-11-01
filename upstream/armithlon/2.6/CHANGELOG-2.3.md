# ARMITHLON 2.3 (2026-11-01)

- New QRX68/Musashi adapter `qrx68_musashi.c`: shared memory, exception-aware
  bus callback boundary, guest/host register synchronization and CPU instruction
  limits; real 040 ISA/FPU sourced from pinned upstream only when installed.
- Added `qrx68-musashi-run` and real-only CPU exception/RTE + HUNK HLE tests to
  conditional CMake configuration. Real Musashi NOT built in offline environment.
- Created explicit stub-only Musashi ABI compile/link check, never labeled CPU PASS.
- CPU-neutral QRX68 boundary dispatcher shared with portable backend.
- Opt-in limited Intuition and graphics HLE (OpenWindow/CloseWindow,
  SetAPen, RectFill, Move, Draw, WritePixel, ReadPixel), no IDCMP/Workbench.
- Original 68k GUI HUNK plus reproducible builder, 640×256 PPM screenshot
  renderer and native Metal-compatible Q86F Unix socket framebuffer delivery.
- QRX68 RAM now sparse, default one quarter host memory up to 3840MiB;
  CLI override `--ram auto|MiB`.
- Raised HUNK total code/data cap from 64KiB to 32MiB with proportional
  scratch allocation, no longer demanding max-cap guest RAM for small HUNKs;
  Exec heap starts AFTER loaded HUNK plus guard page. HUNK file cap 64MiB.
- Added guarded QRXDesktop 6.6.20 `QRXMODE=QRX68` opt-in patch that preserves
  normal native HUNK launches and fails closed if guest QRX68Run is not installed.
  QRX68Run AROS guest service is NOT included and no AROS boot claim is made.
- 47 portable regression tests including new GUI, socket, desktop patch, large HUNK.
- FULL 68040/FPU, true desktop and AmigaOS3 compatibility NOT asserted until
  pinned real-Musashi build and end-to-end actual guest validation.
