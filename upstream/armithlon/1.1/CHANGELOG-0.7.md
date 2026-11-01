# ARMITHLON 0.7 – GPU pipeline & QRX86 Win32 2D HLE integration

- Added `engine/include/qrx_accel.h` + `src/accel.c`: bounded, ownership-
  transferring GPU frame command stream for 2D and 3D guest commands.
- Modified QRXGPU / QRX3D MMIO so native GPU mode bypasses CPU rasterization;
  CPU software fallback still works and is regression tested.
- Added Objective-C Metal 3D renderer (`qrx_metal_renderer.m/.h`) with actual
  drawPrimitives for 2D/3D, Gouraud color, textured nearest, alpha and depth.
- Extended Cocoa frontend to render the stream and expose a Unix socket mode.
- Added `qrx86_gpu.c` 8/16/32bpp HLE surface adapter and optional
  QRX86 22.5.12 `backend_headless.c` source patch (headless present hook).
- Added endian-validated QRX86 host-frame AF_UNIX transport and a synthetic
  frame producer for a Mac GUI end-to-end smoke test.
- Added `build-qrx86-host.sh` compiling the existing private-branch QRX86
  runtime locally with the new GPU hook; actual M1 build unverified.
- All 9 Linux CTests pass; ASAN + UBSAN pass; Unix socket sender/receiver tested.

Known limitations: No native macOS test available here, no Vulkan draw backend,
no actual QRX86 Win32 PE execution verified with this bridge, no interactive
Win32 input, no AROS qrxvirt boot, no original Glide/OpenGL/D3D ABI wrapping.
