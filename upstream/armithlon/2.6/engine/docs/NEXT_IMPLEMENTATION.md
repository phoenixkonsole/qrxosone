# QRX68100 v0.2 handoff → first AROS bring-up

## What is done

The cumulative v0.2 source builds a chipset-less proof VM, a virtual MMIO board, reset vectors and tests; real 68000/020/040-style instruction decoding is intentionally partial. The initial ARM64 JIT compiles straight-line `MOVEQ/NOP` blocks and invalidates on guest RAM writes. Tests pass on Linux x86-64; ARM64 encoded instruction words have been independently checked using an AArch64 assembler. The code has not yet been tested on Apple M1 hardware. **Do not treat JIT speed as characterized.**

## Priorities for actual QRX OS One boot

1. Obtain the **cumulative current QRX OS One** source (separate from this repo), its AROS fork/version, build scripts, kickstart components and boot logs. Do not assume a stock Amiga-m68k ISO works on `qrxvirt` without hardware drivers.
2. Build reproducible standalone AROS m68k ROM/Exec unit test in a loader using the **qrxvirt** virtual-board ABI (or add a new AROS-specific BSP). A real `qrxvirt-m68k` target needs CPU reset/supervisor exceptions, timer/IRQ, resource/device discovery, early UART, memory allocation and a virtual disk. Keep classic Amiga chipset register assumptions out of the new machine target.
3. Complete the 68k reference interpreter with Motorola addressing mode cross-product, 68020/040 exceptions, stack switching, `MOVEM`, `MOVEC`, FPU, MMU usage policy and differential conformance. Only then use JIT beyond explicitly validated instruction sequences.
4. Move from single-block demo JIT cache to page-validated hash indexed code cache and instruction-level safepoints. Add native ARM64 hot-block backend, compare full guest state after every JIT block, dynamic invalidation under guest code writes, and M1 soak tests. Do not enable second guest vCPU yet.
5. Implement actual AROS guest driver stubs and host transport in order: UART/timer/disk, 32-bit chunky framebuffer/HIDD with Metal compositor, input, AHI→CoreAudio, TCP sockets (or SANA-II NIC). Then Vulkan on Linux. Warp3D, SAGA-compatible selected RTG semantics and 3dfx Glide adapter **later** and independently.
6. At every stage, publish new cumulative source, compiler versions, reference-image identifiers, test logs, JIT differential results and the exact feature matrix. Never change existing QRX86 native-JIT safety flags.

## Apple M1 reproducibility gate

`sh tools/test-m1.sh` invokes the tests on **real Apple Silicon**. This is required to graduate from v0.2 proof to a tested ARM64 JIT. If the JIT test fails because macOS refuses `MAP_JIT`, fix code signing/entitlements rather than silently claiming a JIT pass via interpreter fallback.
