# ARMITHLON 0.6 – QRX3D / Glide-style HLE + Vulkan device probe

Changes are cumulative over v0.5. Nothing from the QRX68100 guest, 2D/GPU,
media, native Metal/Cocoa/CoreAudio demo, FS-UAE launcher or QRX asset fetcher
was deliberately removed.

## Newly executable code

- `engine/src/glide3d.c`: versioned QRX3D HLE MMIO (0x00ff0500), with
  vertex fetch from guest RAM in big-endian fixed-point format and independent
  error/success registers. Commands: clear+depth, Gouraud RGB triangle,
  nearest-neighbor ARGB texture, alpha blending, 16-bit depth test/write,
  back-face cull, swap/present. The rasterizer runs on the host CPU; subsequent
  Metal upload/display is inherited from v0.5. No native Voodoo device.
- `engine/guest/qrxvirt_3d.h`: static inlines for 68k QRXvirt BSP developers.
  `engine/guest/aros/QRX3DTest.c`: guest demonstration source. Neither is an
  installed AROS RTG driver or drop-in Glide2 binary at this stage.
- `engine/src/vulkan_probe.c`: optional runtime probe using system Vulkan
  loader, enumerates API, VkInstance and suitable VkDevice/queue. Does not
  submit graphics commands and does not translate guest Vulkan API calls.
- `engine/tests/glide3d_tests.c`: real 68k MMIO writes + triangle/depth/alpha/
  texture/bounds validation + display swap. Existing tests preserved.
- `engine/src/media_demo.c`: animated colored 3D triangle submitted by an
  emulated 68k instruction stream, visible in existing Metal window.

## Verification 2026-10-31 (host Linux x86-64)

- Debug: 7/7 CTest PASS.
- ASan/UBSan debug: 7/7 PASS.
- Media demo: 5 frames, produces frame with RGB triangle.
- Vulkan loader: API 1.4.309 discovered; `vkCreateInstance` returns -9
  (VK_ERROR_INCOMPATIBLE_DRIVER) because no functioning graphics ICD was
  available. Graphics-device creation was **not verified** on this host.
- Apple Silicon Metal frontend: source updated; Mac build/run remains to be
  tested on actual M1. This environment is Linux x86-64.

## Crucial limitations

There is no native Glide2/3 ABI binary, no D3D/OpenGL translation implementation,
no QRX86 import-hook integration, no real Vulkan renderer/swapchain, no
SAGA-equivalent hardware or native chipsetless AROS boot. The existing FS-UAE
mode remains the temporary actual-AROS solution.
