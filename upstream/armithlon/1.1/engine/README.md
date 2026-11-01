## ARMITHLON 0.5 GPU2D / guest audio DMA addendum

The v0.5 CPU/board now implements a bounded QRXGPU 2D guest-MMIO mailbox
(CLEAR, FILL, BLIT, PRESENT) plus bulk signed PCM16BE audio DMA from guest
memory to the host audio queue. A 68k machine-code test program issues GPU
commands; the macOS diagnostic renders its animated bars by actually running
68k memory-mapped writes to these commands (the host still rasterizes 2D in C,
then uploads the framebuffer for Metal presentation). See `docs/GPU2D_0_5.md`.
A guest `qrxvirt_gpu.h` header and AROS CLI prototype are available in `guest/aros/`.
They are NOT graphics.hidd / AHI / input.device drivers. Full AROS boot and
Direct3D/OpenGL interception are not implemented. 6 CTest suites.

---

## ARMITHLON 0.4 MEDIA ADDENDUM

This cumulative v0.2 engine now contains `qrxmedia.h` / `src/media.c` with testable
MMIO video/audio/input devices, portable `qrx_demo` firmware, a native Cocoa/Metal/
AudioQueue developer-preview frontend (`src/macos/`), and 5 CTest suites.
See `docs/MEDIA_0_4.md` and top-level `README-DE.md`. This is *not* yet an AROS
bootable virtual machine or a SAGA/AHI driver. The native macOS code must be built
and tested on Apple Silicon; Linux only validates the portable parts.

---

# QRX 68100 v0.2 — chipset-less virtual 68k boot foundation

**Cumulative source (v0.1 + v0.2).** Invented QRX-68100 CPU identity, QRX virtual-board ABI and a narrow 68k interpreter, with a directly emitted ARM64 JIT **only for `MOVEQ` and `NOP` straight-line blocks**. This is a working boot-monitor VM, **not yet a booting AROS/QRX OS One distribution**.

This is a **separate project**; the pre-existing QRX86 x86→m68k JIT and QRX OS One sources are not changed here. The two virtual CPUs are *described* by the board, but vCPU0 alone is online. 68080/AMMX/SAGA, 68040 MMU/FPU, Metal, Vulkan, AHI and TCP drivers are **not implemented**. The initial guest boot monitor uses no Amiga custom chips.

## Fast start on Apple Silicon or Linux

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/qrx68100_vm
# To opt in to the currently experimental ARM64 JIT (interpreter fallback):
./build/qrx68100_vm --jit
```

Expected monitor output: `QRX68100 qrxvirt board READY`, with the VM reporting `result=halt`, 32 instructions and `exception=0`. The boot monitor itself does not contain currently JIT-supported instructions, so it is expected to show `JIT_blocks=0`. The `vm_unit_tests` executable contains a separate JIT differential and code-cache invalidation test; **on actual ARM64, this test asserts that JIT blocks execute**. It has not yet been run on an M1 here.

The command-line runner also accepts a **raw ROM image** (`--raw-image <file>`), a byte-zero big-endian initial stack pointer and reset PC, RAM size in KiB (`--ram-kib <1024..65536>`), and an instruction budget (`--steps <N>`). `--raw-image` is a low-level test input, **not** an Amiga HDF/ADF, AROS boot ISO or AmigaOS kickstart loader.

Run `sh tools/test-m1.sh` on an M1 Mac to compile and exercise the true native JIT differential test. A hardened macOS app needs its own Apple JIT signing/entitlement setup; a failure must not be hidden by using interpreter results.

## Modules

| Path | Purpose | Actual status |
| --- | --- | --- |
| `src/interpreter.c` | Preserved v0.1 oracle for `MOVEQ`, `NOP`, test-only TRAP#15 halt | Pass |
| `src/jit_a64.c` | Preserved v0.1 test microblock plus optional VM ARM64 JIT; one-block cache, invalidates on any RAM write | Assembled encodings checked; M1 execution pending |
| `src/cpu_vm.c` | QRX VM 68k instruction interpreter, control flow, limited effective addressing, exceptions and stack | Tested subset, not 68040-complete |
| `src/virt.c` | Big-endian guest RAM, reset vectors, QRX virtual MMIO, UART, timer, shutdown | Tested |
| `src/boot_monitor.c` | Generated 68k program printing a banner, then shutting down | Boots |
| `src/virt_main.c` | CLI runner and raw ROM loader | Boots |
| `tests/vm_tests.c` | Deterministic CPU, board, cache coherence and exception tests | Pass on Linux x86-64 |
| `guest/qrxvirt_board.h` | Guest device register documentation/header, no guest driver compiled | ABI proposal |
| `docs/QRXVIRT_BOARD_ABI.md` | Existing board memory map and boot contract | Implemented subset |
| `docs/ARCHITECTURE.md` | Original long-term architecture | Design only for unimplemented parts |

## Currently decoded guest instructions (partial)

`MOVEQ`, `NOP`, `RTS`, `RTE` (temporary six-byte frame), `STOP`, `TRAP #n`, `ILLEGAL`, `MOVE USP`, `MOVE #imm,SR/CCR`, `BRA/BSR/Bcc` byte/word/020-long, `ADDQ/SUBQ` to Dn and An, selected `MOVE.L`/`MOVE.B` register/immediate/absolute/address-indirect forms, `LEA abs.l`, `JMP/JSR` absolute and `(An)`, `TST.L Dn`, `CLR.L Dn`, `CMPI.L #imm,Dn`. Illegal/unimplemented operations raise an exception instead of being executed approximately. Full 68020/040, FPU, MMU, cycle counts, exception frames, interrupts, task switching, and actual SMP remain open work.

## Important boundaries

- **JIT is opt-in and not performance validated.** It only compiles safe straight-line `MOVEQ` / `NOP` blocks, keeps a single block and falls back on all other instructions. No benchmark/GHz numbers should be inferred.
- Guest data is big-endian with validated access; MMIO lives at `0x00FF0000..0x00FF00FF`. ROM vectors live at addresses 0 and 4. Guest access to native API pointers is not provided.
- The AROS `amiga-m68k` machine still expects Amiga chipset support. A real, *new* `qrxvirt-m68k` BSP plus complete CPU core and boot protocol are needed before the existing QRX OS One userspace can run.
- The sample reset/exception behavior is an **initial QRX virtual-board profile**, not accurate 68040 bus-error and FPU state frames.
- macOS `MAP_JIT` and thread-local write protection are used. Linux native ARM64 uses RW→RX transitions. No RWX Linux JIT pages. Memory cache invalidation is intentionally conservative.

See [docs/QRXVIRT_BOARD_ABI.md](docs/QRXVIRT_BOARD_ABI.md) for the active ABI and [docs/NEXT_IMPLEMENTATION.md](docs/NEXT_IMPLEMENTATION.md) for the next real compatibility milestone.
