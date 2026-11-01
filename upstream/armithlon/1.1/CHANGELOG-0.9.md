# ARMITHLON 0.9 — QRX86 OpenGL HLE, QRXGPU command IPC, Vulkan 3D pipeline

Base: cumulative ARMITHLON 0.8 (all prior engine and macOS frontend retained).

## New code

- `engine/include/qrx_gl_compat.h`, `engine/src/gl_compat.c`:
  bounded CPU-independent OpenGL 1.x immediate-mode translator: glBegin(3/4),
  glVertex2f/3f, glColor3f/4f, glClearColor, glClear, glEnable/Disable(DEPTH_TEST,
  BLEND), glOrtho, glFlush. Generates QRXAccel CLEAR and TRIANGLE commands,
  **not** an OpenGL implementation or compatibility with all existing GL apps.
  No matrix stacks, shaders, lighting, display lists, textures or glGet*.
- `engine/src/qrx86_ipc.c`: Q86C big-endian wire protocol for host-owned CLEAR and
  TRIANGLE commands with NaN/size/range validation; existing Q86F framebuffer
  packets and backwards compatibility preserved. A macOS QRX86 renderer that
  already consumes QRXAccelStream can receive and draw these commands through
  the existing Metal renderer.
- `tools/qrx_gl_hle.c`, `tools/patch-qrx86-opengl.py`:
  optional native-host-only PE import adapter for the pinned private QRX86
  Win32 22.5.12 tree. Registers a small opengl32.dll GL/WGL subset and minimal
  gdi32 pixel-format and SwapBuffers stubs, route GL to Q86C socket. Patching is
  source-signature gated, fails closed and does not commit to GitHub.
  Does not implement real Windows pixel formats, multiple GL contexts, texture
  upload, general OpenGL 1.x compliance or the Direct3D 9–12 ABI.
- `engine/shaders/qrx3d.{vert,frag}`, `engine/src/vulkan_3d.c`: optional
  Vulkan 1.0 offscreen **graphics** pipeline (not 0.8's transfer-only backend),
  GPU vertex/color interpolation, 8 depth-test/write/blend pipeline variants,
  command-ordered VkClearAttachments, GPU image readback. No swapchain, texture
  sampling or binary-compatible Glide. Built only when Vulkan SDK and
  glslangValidator are discovered. GPU performance not measured.
- New tests: `qrx_gl_compat_tests`, `qrx86_command_ipc_tests`, patch integrity
  harness, GPU screenshot sanity checker.

## Verified in this execution environment

- Linux x86-64 CMake Debug/RelWithDebInfo build: **12 / 12 CTests PASS**.
- Linux x86-64 ASan+UBSan: **12 / 12 CTests PASS**.
- Native socket round trip: Q86C CLEAR + TRIANGLE, validates decoded coordinates.
- Optional QRX86 source-patch idempotence and fail-closed behavior: PASS using
  exact signature fixtures. This is NOT an actual build of the private QRX86
  runtime and NOT proof of Win32 PE guest compatibility.
- Vulkan development headers, `glslangValidator`, installed ICD and Apple
  M1/Metal host were absent here. **`vulkan_3d.c` has not been compiled or
  executed against a real GPU.** The user must run `vulkan3d-test` on a system
  with those dependencies; any shader or pipeline errors are still open.

## Still required for ARMITHLON release

1. Real Apple M1 build and command-stream image check; real ARM Linux Vulkan
   GPU test, validation layers, clean GPU/Metal timing and shader fixes.
2. QRX86 native host HLE rebuild from the private branch and a controlled PE32
   test executable using the supported OpenGL subset; verify GetProcAddress.
3. Full chipless AROS `qrxvirt-m68k` boot, graphics.hidd, AHI/input drivers,
   complete 68k ISA, correct traps, memory and JIT performance.
4. Texture sampling and depth/blend pixel-for-pixel regression against the
   software reference; Vulkan swapchain and Linux ARM64 launch.
5. Full Windows/Direct3D APIs require separate compatibility architecture.

The 0.9 package is a **developer-source milestone**, not a distributable
fully functional QRX OS One live system or a fast, validated 68100 machine.
