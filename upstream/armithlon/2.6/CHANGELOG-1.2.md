# ARMITHLON 1.2 – current QRX GitHub / Linux i386 GUI → Metal

**Audited GitHub tip:** `phoenixkonsole/qrxosone`, branch `claude/qrx-22.5.11`, commit `cc2ba06a0cc76a87bdd49edcd4b475df4227e716` (2026-10-31 21:37:49 UTC). `main` is at `3287d273e3093098b539b3c1551c39c2490876dd`; the two branches diverge. This project deliberately uses the newer Claude tip and verifies the exact commit when fetching.

The GitHub source already contains the ARMITHLON 1.1 import and a **new embedded X11 server** in `x86-runtime-22.5.12/linux/qrx_x11.c`. Its AROS host has a native Intuition/CyberGraphX backend and has passed a real 68060 FS-UAE screenshot test. Host-native macOS/Linux output previously offered only the optional PPM dump.

## Actual new implementation

- A bounded `qrx_x11_bridge_present` implementation sends local Linux/i386 X11 window pixel output into the **existing Q86F frame socket**, consumed by ARMITHLON's Metal rendering frontend on macOS.
- A format-conversion fast path preserves 0x00RRGGBB X11 pixels without resampling for dimensions up to **800×450**. Larger X11 windows (max **1024×768**) are downscaled with deterministic aspect-preserving nearest-neighbour sampling to fit within 800×450; nothing is silently truncated.
- Original native AROS X11 screen path and `QRX_X11_DUMP_DIR` PPM output remain unmodified.
- `tools/patch-qrx86-x11-host-gpu.py` uses exact source signatures and fails closed on drift. Host-only `QRX86_HOST_X11_GPU` flag. Never enable this flag in Amiga 68k builds.
- `tools/build-qrx86-linux-host.sh` copies the **current GitHub runtime** into disposable staging before patching and compiling. Win32 native build was also revised to operate on a disposable copy, protecting the checkout.
- New `./armithlon build-qrx86-linux-host`, `run-linux-x11`, `x11-bridge-test` commands. Existing `fetch` now pins the actual current GitHub tip, replacing the stale `fb1d03e...` pin that previously rejected the current branch.
- A source and prebuilt **static i386 ELF32 X11 color-bar demo** are included under `benchmarks/linux-i386/`; no i386 compiler is required on the Mac to run the bundled demo.
- A native socket end-to-end test confirms `X11 XRGB → Q86F → QRXGPU RGBA image`, tests invalid dimensions, buffer bounds and downscaling. Patch tests check idempotence, source drift and compilation with/without the feature flag.

## Evidence (Linux x86-64)

- CMake build in RelWithDebInfo: **18/18 CTests passed** (including the new X11 bridge).
- New patch fixture test: **PASS**.
- Prebuilt i386 demo: ELF32, Intel 80386, statically linked (checked with `file`/`readelf`).
- The full native QRX86 X11 host executable was **not** rebuilt from the private GitHub checkout in this isolated environment; compilation and the real i386 window on macOS are **not yet verified**.
- No Apple M1, Metal framebuffer output, Vulkan GPU run, full AROS boot via QRX68100, or GUI input from the Metal window to the X11 client was performed. This patch does **not** implement GLX, XRender, Vulkan for i386 ELF, or full Xlib/GTK/Qt support.
- ARMITHLON JIT safety constraints and existing 0.3–1.1 functionality retained.
