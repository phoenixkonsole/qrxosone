# 0.9 QRX86 PE HLE → QRXGPU

Verified upstream branch: `claude/qrx-22.5.11` at
`fb1d03e45a09e0fb896712590782011baa76de92`.
Relevant sources live in `x86-runtime-22.5.12/win32`:

- `pe.c`: allow `opengl32` virtual DLL imports.
- `hle.c`: append the `qw_gl` API registration table.
- `qrxwin.h`: declare `qw_gl`.
- `kernel.c`: one additional HLE module handle for LoadLibrary/GetProcAddress.
- `qrx_gl_hle.c` copied from `tools/`: minimalist API thunk implementations
  compiled only for native host with QRX86_HOST_GL.

`qrx_gl_hle.c` marshals 32-bit PE float arguments from the guest little-endian
stack (separate from 68k big-endian guest memory) into `qrx_gl_compat`, then
serializes its CLEAR and TRIANGLE command stream into the versioned `Q86C`
Unix stream. Host receiving socket already uses QRXAccelStream; macOS native
frontend already has a Metal GPU renderer for those commands.

`Q86C` records are fixed 124 bytes: four big-endian uint32 fields (kind,
flags,color,depth) and three groups of nine big-endian IEEE754 floats for each
triangle. Bounds and NaN validation apply BEFORE output stream mutation.
Existing `Q86F` image packets remain unchanged; legacy 2D HLE still works.

This is a limited and experimental OpenGL 1.x subset, not proper WGL, Windows
pixel formats, Wine, Direct3D, SAGA or 3dfx binary Glide. GL initialization
requires a suitable GDI+WGL test executable. The first *real* QRX86 runtime
build and tested PE32 execution must be carried out on a machine with the
private source and MinGW-built sample. Normal qrx-win32 AROS releases are not
modified. The QRX86 x86→m68k JIT is **still experimental/off**.

Call flow:

```text
PE32 app -> IAT opengl32/glBegin+glVertex+glEnd -> qrx_gl_hle.c
 -> qrx_gl_compat.c -> host QRXAccelFrame -> Q86C Unix socket
 -> qrx86_ipc_read_one -> QRXAccelStream -> qrx_metal_renderer.m
```

For Vulkan offscreen alternative, the separate `vulkan_3d.c` is a device-side
shader and graphics-pipeline diagnostic. It is NOT yet the QRX86 socket's
Linux window renderer. Do not confuse those two paths.
