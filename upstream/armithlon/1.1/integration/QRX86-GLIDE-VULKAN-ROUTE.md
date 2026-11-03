# Integration with the QRX OS One 22.5.11 branch / embedded 22.5.12 runtime

Repository: phoenixkonsole/qrxosone, commit
`fb1d03e45a09e0fb896712590782011baa76de92` on branch `claude/qrx-22.5.11`.
The branch actually contains embedded `x86-runtime-22.5.12` implementation.

## Real existing implementation entry points

`qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest/x86-runtime-22.5.12/`

- `win32/dx.c` – DirectDraw/DirectSound/DirectInput v3 HLE. Uses 32-bit
  guest-memory COM objects + HLE thunk slots. There is **no Direct3D yet**.
- `win32/hle.c` – guest memory read/write (`x86_rd32`, `x86_wr32`), import
  thunks, guest heap, dispatch. HLE interfaces should be added here, not by
  loading a host DLL into the 68k address space.
- `win32/backend_aros.c` – CyberGraphX/Intuition presentation (with RTG),
  existing framebuffer window and input. The DirectDraw 2D path should be
  preserved as fallback, including its 8/16/32-bit modes.
- `linux/qrx86linux.c` – Linux i386 execution and host syscalls, a distinct
  import model; no automatic Vulkan ELF loader or GLX/EGL glue.

## Recommended order (all unimplemented)

1. AROS board (`qrxvirt-m68k`) must boot on QRX68100. Convert existing
   `graphics.hidd`/RTG, AHI and input devices to the versioned MMIO interface.
   FS-UAE with classic AROS is not qrxvirt and must never access our MMIO.
2. Add a **Win32 import/HLE adapter** for `glide2x.dll` and `glide3x.dll`
   (`grSstWinOpen`, `grBufferClear`, `grDrawTriangle`, `grBufferSwap`, texture
   allocation/upload and state functions), with x86 cdecl/stdcall semantics
   validated against known exports. Each call reads x86 guest memory, writes
   a BE staging buffer in QRX68k guest RAM and submits QRX3D commands.
3. Add 3D3–D3D7/9 surface/COM semantics separately, or integrate a native
   DXVK/Wine-compatible host service via **serialized RPC**, translating
   D3D8/9/10/11 → Vulkan. DXVK x86 DLLs cannot directly run in a 68k guest.
4. For native Vulkan guest calls: marshal `Vk*` structs, arrays, handles,
   pNext chains and callbacks via allowlisted RPC; disallow raw host pointers,
   set explicit resource quotas, maintain queue/synchronization ordering.
   68k guest Vulkan requires a new loader/ICD frontend; it does not become
   automatically native just because a host Vulkan driver exists.
5. OpenGL calls need context/GLX/EGL/WGL state, glGet-style readback and
   shader source/compiler validation; map them to host Metal or Vulkan/Zink.
6. macOS M1: Metal renderer now exists as framebuffer upload; KosmicKrisp
   Vulkan-over-Metal is an optional future GPU backend (macOS 26+). Linux
   ARM64: Vulkan via Mesa/proprietary ICD, display via Wayland/SDL3.
7. Voodoo compatibility: prefer genuine 3dfx Glide API-level translation,
   not full timing-correct Voodoo PCI. Evaluate GlideVK as a candidate but
   review licensing, ABI assumptions, Wine dependencies and known caveats.

## Performance path

- Use bulk vertex and texture staging, command batching and shared pages.
- Avoid GPU->CPU readbacks except explicit lock/readback operations.
- Keep guest/host addresses separate and validate I/O sizes and command budgets.
- Use Vulkan synchronization and Metal fences when real GPU backend exists.
- Keep QRX86's x86→68k JIT independent from QRX68100's 68k→ARM64 JIT.

## Status of this release

This v0.6 adds a *real bounded API-neutral 3D guest command target* for
further integration, **not** actual QRX86 import hooks. The QRX OS One source
archive and original ISO are not modified by this release.
