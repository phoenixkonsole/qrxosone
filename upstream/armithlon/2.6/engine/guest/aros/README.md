# AROS 68k QRXGPU bootstrap adapter (not yet an AROS RTG/AHI system driver)

This directory supplies an **AROS 68k developer CLI** (`QRXBoardTest.c`) and a
portable guest register header (`../qrxvirt_gpu.h`) with `qrxv_gpu_fill`,
`qrxv_gpu_blit`, `qrxv_gpu_present`, and `qrxv_audio_pcm16_dma` for your next
AROS platform-port step. These functions genuinely reach the QRXvirt host
through 68k memory-mapped I/O *once running inside qrxvirt*.

Use your **AROS cross compiler and matching SDK**, not the host Clang. Example:

```sh
QRX_AROS_CC=/opt/qrx-os-one/deadwood-aros-build/toolchain-core-m68k/m68k-aros-gcc \
QRX_AROS_CFLAGS='-B/path/to/startup.o/ -I/path/to/AROS/include -L/path/to/AROS/lib' \
  bash engine/guest/aros/build-test.sh /tmp/aros-qrx
```

The resulting file must be a verified Amiga HUNK built for the correct AROS ABI;
check the object format before installing. This build path was not validated
with the user's AROS toolchain here. The **current AROS Amiga-platform boot
cannot access QRX GPU MMIO through FS-UAE**: FS-UAE does not present this new
board. Do not run QRXBoardTest on an ordinary AROS/Amiga system; it will read
unmapped device addresses and can fault.

To boot AROS natively under QRX68100 still requires:
1. A 68040-compatible CPU/MMU/exception implementation or a compatible
   AROS port, well beyond the experimental interpreter/JIT opcode set.
2. A new QRXvirt machine/board BSP and startup with ROM, interrupts, timers,
   scheduler-compatible atomic operations, Exec/DOS memory layout and bootloader.
3. `graphics.hidd`/RTG display driver that maps gfx primitives to these calls,
   plus AHI audio driver and `input.device` mapping of event and scancode formats.
4. More capable GPU command queues, synchronization and memory pinning.

The provided adapter is source groundwork, **not a booting AROS graphics.hidd,
AHI or input.device implementation**. Do not install it as a system driver.
