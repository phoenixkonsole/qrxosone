# ARMITHLON 1.0 developer milestone — GPU benchmark matrix

## Actual additions

- Added reproducible 800×450 240+16 triangle grid scene shared by native CPU,
  Metal/Vulkan pipelines, QRX3D/Glide-style guest MMIO and OpenGL immediate-mode HLE.
- Added 68k-safe QRX3D MMIO full-frame benchmark and OpenGL command-translation
  benchmark with exact command-count checks and monotonic wall-clock timing.
- Added deterministic CPU rasterization, FNV64 image hash, PPM screenshot and
  image-difference validation for GPU outputs.
- Added **real** native Metal offscreen benchmark source, drawing 256 triangles,
  with pipeline reuse, alpha blending, Z-test/write, GPU completion and readback.
- Updated existing Vulkan3D backend to replay its recorded 256-triangle GPU
  command buffer across timed frames (including GPU waits), and to emit JSON.
- Corrected Vulkan viewport Y mapping for 800×450 top-left-origin guest coordinates.
- Added standalone MIT native Direct3D9 Windows sample (source, build instructions,
  PPM screenshot). It is NOT a QRX86 D3D9 HLE.
- Added `./armithlon benchmark`, `benchmark-quick`, machine-readable JSON,
  markdown report, API statuses and explicit non-supported paths.
- Existing QRX86 OpenGL source patch, legacy Metal window, compatibility ISO
  launch and emulated CPU remain unchanged.

## Verification in current Linux x86-64 build environment

- Core CMake configure/build: PASS.
- 16/16 CTests: PASS.
- QRX86 OpenGL and input source patch idempotency checks: PASS.
- ASan+UBSan: 16/16 CTests PASS.
- Multi-frame CPU, QRX3D/Glide-style MMIO and OpenGL-HLE benchmarks: PASS.
- Native Apple M1 Metal execution: NOT TESTED; no macOS host is available here.
- Vulkan compilation/runtime: NOT TESTED; SDK/ICD/glslangValidator not installed.
- Windows D3D9 sample build/runtime: NOT TESTED; no Windows/MinGW here.
- QRX86 actual OpenGL-PE32 guest integration: NOT TESTED with real EXE.
- QRX68100 direct AROS boot and full QRX OS One ISO: STILL UNAVAILABLE.

Note: ARMITHLON 1.0 is a developer version identifier, NOT a production release.
