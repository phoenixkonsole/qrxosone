# QRX86 → QRXGPU → Metal / Vulkan: proposed API interception architecture

Status v0.5: **Only QRXGPU 2D mailbox and PCM16 DMA are implemented and tested.**
No Direct3D/OpenGL/Vulkan import interception is installed in QRX86 in this
release. No DirectX or Linux graphics application is made runnable by this kit.

## Why HLE / direct API translation beats full fake Voodoo GPU emulation

```
Win32 PE x86 ─ QRX86 PE import dispatcher ─ ddraw/gdi/opengl32/d3d9/DXGI HLE ┐
Linux ELF x86 ─ QRX86 ELF PLT resolver ──── libGL/libEGL/libvulkan/X11 HLE ──┼─ QRXGFX command ABI
AROS 68k apps ─ graphics.hidd / Warp3D / optional native GL adapter ─────────┘           │
                                                                       ┌───────────────┴───────────────┐
                                                                       │ macOS Metal  │ Linux Vulkan   │
                                                                       └───────────────────────────────┘
```

When QRX86 resolves `opengl32.dll!glDrawElements`, `d3d9.dll!Direct3DCreate9`,
`d3d11.dll!D3D11CreateDevice`, `dxgi.dll` or Linux ELF `libGL.so.1`/`libEGL.so.1`
imports, it can issue an **HLE trap** instead of invoking the x86 implementation.
The host must implement each called API and object lifetime, synchronize guest
memory, validate handles/lengths/shaders, map formats/features and create
native pipelines. Windows graphics requires compatible user32/window/event,
COM objects, DXGI and shaders; Linux GL requires X11/Wayland/GLX/EGL context
semantics. An import name map alone is not an API implementation.

## Recommended backend selection (future)

| Guest API | Initial HLE route | Host target | Notes |
| --- | --- | --- | --- |
| Win32 GDI, DirectDraw | 2D QRXGPU | Metal (macOS), Vulkan (Linux) | Begin with framebuffer/bulk copies; subset only |
| OpenGL 1.x fixed-function | Build stateful 68k/x86 HLE wrapper | Metal/Vulkan | Must translate GL state/shaders |
| OpenGL modern | HLE + shader translation | Metal/Vulkan | Context, extensions, GLSL complicate compatibility |
| Direct3D 8/9/10/11 | DXVK for a compatible *native* Vulkan runtime, or API HLE | Vulkan; Metal translation on Mac | DXVK x86 Windows DLLs cannot simply be copied into a 68k process |
| Direct3D 12 | vkd3d-proton if adequate Vulkan/ABI support | Vulkan then Metal translation | Not a drop-in bridge: very large feature/driver requirements |
| Vulkan | Vulkan loader HLE with guest-handle virtualization | Vulkan; KosmicKrisp on Apple Silicon | Do not pass raw guest pointers or opaque handles into native Vulkan |
| Warp3D / SAGA RTG | AROS guest API adapter | Metal/Vulkan | SAGA chipset is not automatically emulated |

DXVK is D3D8–11 -> Vulkan under Wine/Proton; vkd3d-proton is D3D12 -> Vulkan.
2026 Khronos recommends **KosmicKrisp** over deprecated MoltenVK for Vulkan on
Apple Silicon, but on macOS native Metal is simplest for our custom protocol.

## Production milestone dependencies

- Add a proper QRX86 PE import/ELF PLT trap with return ABI, per-thread contexts.
- Implement host-side GDI/DirectDraw 2D and then GL1 test vectors.
- Define descriptor handles, dynamic VRAM/multiple surfaces, asynchronous bounded
  packet rings, fences, pinned guest pages, 64-bit offsets, pipeline/texture cache.
- Implement Metal backend first; Linux Vulkan backend using same API-independent
  command schema second. DMA is already bounded but still CPU-copied.
- Validate hostile guest data; never forward native host pointers or raw shaders.
- Add CPU/JIT correctness and performance tests with actual target QRX ISO.

## References

- https://github.com/doitsujin/dxvk
- https://github.com/HansKristian-Work/vkd3d-proton
- https://github.com/KhronosGroup/MoltenVK
- https://www.vulkan.org/porting
