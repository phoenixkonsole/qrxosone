# ARMITHLON 0.4 (2026-10-31)

- Cumulative upgrade of 0.3; retains tools for private QRX branch (`fetch`,
  verified AROS ISO, HUNK preview ISO, FS-UAE mode), original v0.2 CPU/JIT.
- Add fixed `qrxvirt` media MMIO region (0x00ff0100 gfx, 0x00ff0200 audio,
  0x00ff0300 input), feature negotiation, bounds and width checks.
- Add big-endian guest XRGB framebuffer presentation through synchronous
  host callback, stereo PCM16 ring buffer, input-event SPSC queue and mouse
  status registers; preserve device binding across VM resets.
- Add portable diagnostic firmware, including real 68k MOVE.L operations
  issuing MMIO PRESENT and keyboard queue length reads, validated headlessly.
- Add native macOS MetalKit frontend (single texture), CoreAudio AudioQueue
  playback, Cocoa mouse/key/scroll event mapping. Mac executable is built
  only on macOS. Linux has no window backend yet.
- Add Finder app bundler using ad-hoc local codesigning, native command starter.
- Add SPSC ring tests, register/width/bounds tests, headless frame test.
- Important: force assert() tests on with -UNDEBUG in Release/RelWithDebInfo;
  prior CTest builds with -DNDEBUG silently elided assertions.
- NOT included: AROS boot/guest drivers, complete 68040/68080 CPU, full ARM64
  JIT, SAGA, 3dfx/OpenGL, AHI, TCP, native Linux GPU/audio, SMP, full QRX ISO.
