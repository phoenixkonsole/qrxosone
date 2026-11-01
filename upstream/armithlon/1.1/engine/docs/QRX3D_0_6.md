# QRX3D 0.6 – memory mapped command protocol

**Not Voodoo PCI emulation. Not a binary `glide2x.dll`/`glide3x.dll`.**

## Host-visible registers

- `QRX_IO_BASE = 0x00ff0000`, `QRX_3D_BASE = 0x00ff0500` (within the existing
  0x800-byte board-MMIO aperture).
- All registers are 32-bit aligned, **big-endian**.
- `+0x00`: 0x51523344 ("QR3D") read only; `+0x04`: 0x00060000 ABI.
- `+0x08`: capability bitmask (clear/tri/swap/tex/depth);
  `+0x0c`: last command error; `+0x10`: command trigger/last command;
  `+0x14`: successful command counter; `+0x20..+0x3c`: eight command arguments.
- Command=1 CLEAR: arg0 0xAARRGGBB (RGB rendered), arg1 depth in [0,65535].
- Command=2 TRIANGLE: arg0 guest RAM address of three contiguous vertices,
  arg1 flags: 1 depth test, 2 depth write, 4 alpha blend, 8 texture enabled,
  16 back-face cull. Caller owns vertex data until synchronous call returns.
- Command=3 SWAP: presents current 800x450 XRGB8888 framebuffer through media
  callback. The front/back buffer is not double buffered yet.
- Command=4 TEXTURE: arg0 guest RAM address, arg1 byte row stride, arg2 width,
  arg3 height (<=2048). Format ARGB8 big-endian. Zero width+height unbinds.
  The host validates that the entire image is in guest RAM and not overlapping
  the framebuffer; pixels are fetched as needed for draw. Guest may alter the
  texture data after binding; the next draw sees new data.

Vertex (24 bytes, three per triangle, 72 bytes in guest memory):

```c
struct QRXV3DVertex {
  int32_t x16, y16, z16; // signed 16.16; z 0..65536
  uint32_t argb;
  int32_t s16, t16;     // normalized signed 16.16 texcoords, clamped to [0,1]
};
```

Gouraud RGB interpolation uses barycentric coordinates; depth is linear 16-bit,
not Glide's W-buffer. Triangle order controls optional back-face culling. Alpha
blending is source-over against destination framebuffer; nearest textures only.
No triangle fan, polygons, mipmapping, color combine, texture perspective,
MSAA, fog, multiple TMUs, Gamma or TMU memory layout, PCI I/O, shader support,
Vulkan command buffers or Glide registers. Rasterization is software CPU.

## Security/correctness constraints

- MMIO is synchronous and must be accessed by only one guest writer until a
  command ring/locking contract exists. Invalid commands return stable error
  codes rather than using native pointers.
- Guest memory addresses are treated as offsets; every region is range-checked
  against RAM size, reserved I/O, and current framebuffer.
- Texture dimensions and stride are capped; vertex coordinates are limited;
  clipping occurs before pixel traversal; no guest shader execution.
- `qrx_glide_shutdown(media)` frees lazy host depth allocation at VM shutdown.
- Legacy graphic API wrappers must deserialize API structs from their own
  guest-architecture layouts; do not cast x86 pointers to host pointers.
