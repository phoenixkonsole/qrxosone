# QRX 68100 — chipset-less 68k virtual platform

Architecture proposal, 2026-10-31. Target: QRX OS One m68k/AROS distribution running as a fast virtual 68k machine on an Apple M1 first, on lean Linux later, with host-accelerated graphics/audio/TCP.

## Non-negotiable distinction

`68k → ARM64` **dynamic binary translation (DBT/JIT)** is not ARM hardware virtualization. Apple HVF and ARM virtualization extensions cannot directly execute 68k binaries. The fast path runs JIT-generated native AArch64 *host* instructions implementing 68k semantics, not the original 68k instructions. Transmeta's Code Morphing idea is a useful model for translation and caching, not a means of bypassing ISA translation.

## Guest virtual machine ABI

CPU model: `QRX-68100`, 2 *defined* virtual CPUs, vCPU0 online, vCPU1 offline until 68k AROS SMP is explicitly supported. Expose to legacy applications as a compatible 68040-class CPU first; advertise more only when tested. Classical 68k OS assumptions are uniprocessor; two live vCPUs without reworking Exec, synchronization, interrupts, caches and task migration will corrupt data. Use native host threads for GPU/IO/JIT compilation immediately; that is NOT two guest cores.

Guest memory: flat **32-bit big-endian address space**, fixed guest-to-host mapping, validated access helpers, page-level dirty/write tracking, safe fault conversion, no raw guest pointers exposed to host services. 68k integer exceptions, supervisor/user registers, SR/CCR, proper 68020/040 FPU semantics, privileged instructions, CIA/Amiga resources *only if chosen*. No fictitious 64-bit pointers unless a separate ABI is designed. Treat Apollo 68080's AMMX and other extensions as an OPTIONAL profile, not something automatically provided by a 'faster' virtual CPU. The Apollo 68080 documentation states that MMU-related instructions are not supported, so do not infer a full MC68040 MMU from '68080 compatible'.

A proposed feature enum, not a currently implemented ISA:
- `qrxcpu=68040-compat` => target baseline for booting and traditional application execution.
- `qrxcpu=qrx68100` => 68040-compatible plus QRX-specific hypercalls and host device transport.
- `qrxcpu=qrx68100-amx` => *only after* AMMX differential tests and source/licensing review.

## Boot: no classic custom chips, with an important condition

Do not implement Agnus/Denise/Paula/Copper/Blitter as the normal QRX machine. Instead make a NEW AROS 68k `qrxvirt-m68k` hardware target (virtual board/BSP), bootstrap, interrupt controller, timer, boot disk and display driver. The EXISTING `amiga-m68k` AROS image expects Amiga machine hardware and cannot be assumed to boot when all Amiga custom chip registers disappear. Its graphics hardware path uses AROS HIDD drivers; a virtual GPU HIDD must be registered early. The current QRX OS One/Deadwood image is therefore not a guaranteed drop-in boot image; the source build and initialization need to be adapted. Optional separate classic chipset-compat mode, later, for ADFs/demos and Amiga hardware-poking software.

Boot milestones:
1. Memory map, basic boot ROM/loader, stack, reset vectors, exception/interrupt vector table, monotonic timer and serial debug console.
2. Interpreter boot trace to first AROS Exec/early startup; provide minimal virtual devices, no copper.
3. Native m68k AROS `qrxvirt` BSP or split AROS core+OS platform port, with screen/desktop and storage.
4. Replace hotspot interpretation with a correct JIT; enable QZX device ABI after bootstrap (device register map below).

## Host/device boundary and API

Host interface `libqrxhost` C ABI with tagged version (`QRXH1`), feature bits, fixed-width little-endian transport record fields (distinct from big-endian guest memory), ring command buffers, ring ownership, memory validation, rate limits, fences and asynchronous completion. Never map host physical PCI/Metal/Apple IOKit directly into guest pointers. Native guest `.device/.library/.hidd` drivers issue QRX hypercalls or use a bounded MMIO ring; host fulfills them in native ARM64 system threads. This is the practical meaning of 'native host drivers'; the 68k-facing parts remain m68k stubs, while expensive GPU, network, storage, audio implementation runs in native host code.

Suggested virtual resources (illustrative design IDs, NOT existing hardware):
- `0x51525800` device discovery/ABI version/feature bitmap
- `QRX_VGPU` shared graphics command queue and fence
- `QRX_VAUDIO` shared PCM ring buffers + clock
- `QRX_VNET` BSD socket service (or separate SANA-II virtual NIC)
- `QRX_VDISK` host file-backed block storage / virt queue
- `QRX_VTIME` monotonic timer + VBlank/RTG frame signal
- `QRX_VINPUT` keyboard/mouse/gamepad event queue

## Graphics: distinguish APIs and hardware emulation

1. Desktop/2D: AROS `graphics.library` -> custom `qrxgpu.hidd`/RTG -> host-accelerated blit/scanout/texture upload -> Metal on macOS, Vulkan on Linux. Prefer chunky 32-bit buffers with dirty rectangles and native compositor. Optional Picasso96 bridge for OS3.x binaries; AmigaOS 3.x P96 licensing and driver ABI must be checked separately.
2. SAGA: the Apollo SAGA RTG driver is a **specific** RTG solution backed by a particular chipset, not an OpenGL API. Expose a `SAGA-compatible` subset via a guest-driver adaptation or explicitly emulate documented SAGA registers needed by selected apps. Do not claim complete SAGA/68080 hardware equivalence without verifying register/feature coverage.
3. 3D: implement `Warp3D.library` first as a guest-side command encoder plus host-side GPU renderer. Add MiniGL/StormMesa and a limited OpenGL compatibility frontend. Provide Glide/3dfx **API** compatibility as an optional distinct module. 'Voodoo 3dfx pass-through' means translating guest *API calls*, not plugging a real Voodoo PCI card into Metal or magically running legacy binary drivers unchanged. Rasterization, fog, texture combine, depth and blending need explicit mappings and conformance tests. For real Voodoo PCI register/direct-PCI software, hardware emulation is a separate, expensive project.
4. On Apple Silicon choose native Metal for speed; on Linux use Vulkan. A single renderer-neutral IR/command list reduces divergent guest code. Vulkan on macOS is an additional backend only, not the primary speed path. Khronos' 2026 recommendation on Apple Silicon favors KosmicKrisp over the now-deprecated MoltenVK path; do not lock architecture to MoltenVK.

## Audio / networking

Guest `ahi.device` (AHI device driver) -> `QRX_VAUDIO` PCM/circular buffer -> CoreAudio on macOS / PipeWire or ALSA on Linux. Handle sample formats, buffer lifetime, timestamps, notifications, underruns, host sample-rate conversion and latency; do not depend on Paula. Music/sfx applications using Paula registers rather than AHI will not work without compatibility mode.

Guest `bsdsocket.library` adapter -> `QRX_VNET` asynchronous BSD-socket command set -> host sockets, including host DNS, TCP/UDP, poll/select semantics, error/errno mapping and shutdown cancellation. Alternatively, SANA-II virtual NIC + guest TCP/IP stack for greater fidelity but higher overhead. No arbitrary guest-supplied host pointer dereferences. Clipboard/shared folder integration can use the same transport.

## Host parallelism / guest dual-core

JIT optimization and code-generation workers, GPU command submission, audio mixing, file IO and networking can use several Apple M1 CPU cores from day one. This is safe even with a single guest vCPU. The defined second 68100 vCPU stays offline. Bring it up only after implementing 68k AROS SMP: Exec task/scheduler consistency, memory ordering, atomic TAS/CAS, per-vCPU CPU state, interrupt routing, memory barriers, locks, timer/IPI and reproducible race tests. The 68080 being advertised as 'hyper-threaded' does not make AmigaOS 3.x or AROS m68k SMP-ready.

## JIT stages and correctness gates

- Interpreter oracle: complete 68020, supervisor and 68040-compatible user instructions, 68881/040 floating point (precise rounding and exception tests), regression harness, executable metadata.
- Tier 0 interpreter on unknown blocks and before JIT warms.
- Tier 1 fast decoder -> AArch64 native basic blocks: register promotion D0-D7/A0-A7, lazy condition-code evaluation, block chaining, direct branch fast path, byte swaps only where required, safe page-checked memory accesses, fast A64 FPU where behavior is equivalent.
- Tier 2 hot traces, constant propagation, cross-block propagation, inline memory ops with transparent slow paths, signal/interrupt safepoints and deoptimization. Use NEON for tested AMMX ops. Never silently drop flags, FP status, faults or privilege checks for performance.
- Code cache: executable page provenance, self-modifying-code invalidation, memory-map changes, dispatch versioning, memory cap, deterministic translation correctness. On macOS follow MAP_JIT + JIT write protection + `sys_icache_invalidate`; on Linux apply W^X + `__builtin___clear_cache`.
- Benchmark separate CPU loops, GUI/2D, mixed 3D, Quake software renderer, compile workloads, AHI, TCP and boot latency. Measure *on actual M1* against reference interpreter, FS-UAE (where comparable) and current QRX OS One. Never announce '680xx GHz' or native performance from theory.

## Cross-platform binaries and Amithlon-like target

One `libqrxcore` 68k decoder/IR/validation and `libqrxhost` interface with platform backends:

```text
Mac M1 (macOS): QRX/AROS 68k -> AArch64 JIT -> native Metal, CoreAudio, BSD sockets
Linux ARM64:     QRX/AROS 68k -> AArch64 JIT -> native Vulkan, PipeWire, Linux sockets
Linux x86_64:    QRX/AROS 68k -> x86_64 JIT -> native Vulkan, PipeWire, Linux sockets
```

An *Amithlon-like* Linux distribution would boot a tiny kernel + initramfs and run this user-space runtime in kiosk mode, with Wayland/DRM/seat/input/audio. Zero classic chipset emulation in default QRX mode. A 68k vCPU still executes via dynamic binary translation on ARM64, never CPU hardware virtualization. ARM64 Linux guests optionally run under Apple's virtualization frameworks, but then QRX CPU still needs its own 68k translator inside Linux.

## Code integration with user's existing project

Existing QRX OS One 22.5.x has `QRX86` which translates **x86 -> 68k**. This new emulator translates **68k -> host ARM64/x86_64**. Keep these separate; a double-translation x86->68k->ARM64 is a possible compatibility fallback but not likely to be high-performance. The prior QRX86 native JIT remained intentionally disabled following illegal-instruction failures. Never re-enable or bypass that safety gate as a side effect of this project. The new 68k translator has its own independent test harness, toggles and deployment gates. Boot with the existing cumulative QRX OS One source snapshot in CI after the virtual board exists.

## Licensing/compatibility references

- Apollo 68080/SAGA technical features: https://wiki.apollo-computer.com/doku.php?id=apollo_core:start and https://www.apollo-computer.com/features.php
- Apollo SAGA RTG source/notes: https://wiki.apollo-computer.com/doku.php?id=system_tools:rtg
- AROS amiga-m68k port: https://www.aros.org/introduction/ports.html
- Emu68 bare-metal ARM64 JIT (reference implementation, not Mac drop-in): https://github.com/michalsc/Emu68
- Warp3D API: https://wiki.amigaos.net/wiki/UserDoc:Warp3D
- QuarkTex Warp3D to host OpenGL precedent: https://github.com/RobDangerous/QuarkTex
- macOS JIT: https://developer.apple.com/documentation/Apple-Silicon/porting-just-in-time-compilers-to-apple-silicon
- QEMU TCG MT concepts: https://www.qemu.org/docs/master/devel/multi-thread-tcg.html
- Khronos GPU portability: https://github.com/KhronosGroup/MoltenVK
