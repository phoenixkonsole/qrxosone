# ARMITHLON 0.8 – Vulkan 2D transfer backend + QRX86 interactive input bridge

**Build**: 31 October 2026. Cumulative continuation of 0.7; original 0.3–0.7 code retained.

## Implemented

1. `engine/src/vulkan_offscreen.c`: optional native Vulkan SDK build. Uses a Vulkan
   physical/logical device, GPU image in device memory, GPU image clear,
   VkBuffer→VkImage region transfers for 2D RGB images and filled rectangles,
   per-operation synchronization, GPU readback and PPM screenshot. Clear/rect/image
   obey QRXAccel command ordering. Rectangle pixels are still generated on the
   host and uploaded — this is a transfer-based 2D renderer, NOT a fully
   GPU-rasterized 2D geometry engine or a 3D Vulkan pipeline. Triangles return an
   explicit unsupported error. 800×450 and 24 MiB staging cap for this diagnostic.
   It does not create a swapchain or interactive Vulkan window.
2. `engine/include/qrx86_input.h` and `src/qrx86_input.c`: **reverse-direction**
   versioned binary IPC protocol (`Q86I`) for input events, with bounds checks,
   socket permissions, explicit listener lifecycle, limited reads and timeouts.
3. `macos/armithlon_mac.m`: macOS Cocoa key and mouse events now route to a
   separately opted-in QRX86 Win32 host process, in addition to unchanged
   68k demo events. Common Mac keycodes are translated to Win32 virtual keys.
   No gamepad, wheel or IME support for QRX86 yet.
4. `tools/patch-qrx86-host-input.py`: guarded, idempotent source patch for the
   actual QRX86 22.5.12 headless backend. Changes clock to monotonic real time
   and returns live QE_* events **only** with native host input socket enabled
   and no deterministic -s test script. Preserves AROS m68k backend/other code.
5. `tools/build-qrx86-host.sh`: includes both host graphics and input adapters;
   patches only an authenticated local working copy of private GitHub sources.
6. `./armithlon vulkan-test`: builds/runs optional native Vulkan offscreen
   renderer and checks exact pixel values from GPU readback (when SDK & ICD present).

## Verification – this environment

- Linux x86-64: 10/10 CTest passed in RelWithDebInfo.
- Linux x86-64: 10/10 CTest passed with AddressSanitizer+UndefinedBehaviorSanitizer.
- Extra host-only QRX86 patch/idempotence/source-marker guard test passed.
- Vulkan SDK headers and an active Vulkan ICD were not available here. The new
  Vulkan backend was therefore **not compiled or executed** and its GPU
  correctness remains to be confirmed on macOS/Linux with an installed SDK.
- The Cocoa/Metal macOS frontend and full QRX86 22.5.12 Win32 HLE integration
  could not be built or exercised on a real Apple M1 here.
- Neither original DXVK/vkd3d/glide2x.dll/GL APIs nor real AROS HIDD/AHI driver
  integration are claimed in 0.8. QRX68100 cannot boot QRX OS One yet.

## Commands

```bash
./armithlon build
./armithlon test
./armithlon run-native
./armithlon vulkan-test
./armithlon fetch
./armithlon build-qrx86-host
./armithlon run-native-qrx86
# second terminal: ./armithlon run-qrx86 /path/to/compatible-program.exe
```

**Security**: Both IPC transports are local-only and do not give the guest a
native GPU pointer. A process running under the same user can still inject
input to the interactive development bridge: do not expose it as a production
privilege boundary. Future distro release needs robust permission/peer identity
checks, protocol hardening, resource budgets and fuzz testing.
