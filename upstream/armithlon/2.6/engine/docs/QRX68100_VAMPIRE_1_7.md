# QRX68100 1.7 CPU Extension – ABI contract (non-Apollo)

## Profiles

* `QRX_BOOT_LEGACY_AMIGA` (power-on): Musashi 68040, core 0 only. The entire dual CPU and vector MMIO windows are invisible. Extended vector registers are reset on profile change.
* `QRX_BOOT_QRX_DUAL`: core 1 may be booted explicitly at a separate PC/SP, both cores have 24 private vector registers, private 16 KiB I and 32 KiB D caches, and private dynamic branch state. RAM/JIT page epochs are shared. Standard Musashi `m68k_execute` remains serialized.

## Register map (big-endian 32-bit aligned read/writes)

| Offset at `0xFF000700` | Register | Permission |
|---|---|---|
| 0x00 | MAGIC `0x51563137` (QV17) | read |
| 0x04 | ABI `0x00010000` | read |
| 0x08 | FEATURES mask: VEC64/MUL64/DIV64/L1I/L1D/BRANCH | read |
| 0x0C | QRX ID `0x00068100` | read |
| 0x10 | CTRL: bit 0 cache, bit 1 safe prefetch, bit 2 predictor | supervisor write / read |
| 0x14 | Logical I-cache size `16384` bytes | read |
| 0x18 | Logical D-cache size `32768` bytes | read |
| 0x1C | L2-cache size `0` – not implemented | read |
| 0x20 | VSEL (0–23) | read/write |
| 0x24 | VSRC (0–23) | read/write |
| 0x28 | VDST (0–23) | read/write |
| 0x2C | selected reg low 32 bits | read/write |
| 0x30 | selected reg high 32 bits | read/write |
| 0x34 | execute selected 64-bit operation | write |
| 0x38 | result status `0` success, `1` bad command, `2` divide-by-zero | read |
| 0x40–0x4C | L1 I/D hit/miss counters | read |
| 0x50–0x54 | branch prediction good/bad samples | read |
| 0x58 | explicit cache invalidate, write `1` | supervisor write |

Only 4-byte aligned accesses are accepted. State lives in `QRXDual::vamp`, is not shared with the other core, and is reset on mode transition / core1 restart. The kernel must save/restore the 24 vector registers for context switching; a full save-image ABI has not been defined yet.

## Opcodes / commands

These **are MMIO command IDs, not 68080 AMMX instructions**. Sources `E[VSEL]`,`E[VSRC]`, destination `E[VDST]`. No 68k CCR, FPU, or CPU register side-effects.

| ID | Command | Semantics |
|---:|---|---|
| 1 | PADDB | 8 unsigned 8-bit lanes, modular addition |
| 2 | PADDW | 4 unsigned 16-bit lanes, modular addition |
| 3 | PSUBB | 8 unsigned 8-bit lanes, modular subtraction |
| 4 | PXOR | bitwise XOR |
| 5 | PAND | bitwise AND |
| 6 | POR | bitwise OR |
| 7 | MUL64 | unsigned 64-bit multiply, low 64 bits |
| 8 | DIV64 | unsigned quotient, div0 rejects and preserves destination |
| 9 | PAVGB | rounded unsigned 8-bit per-lane average |
| 10 | PADDSB | unsigned saturating 8-bit addition |

Future real 68k F-line opcode support must be versioned and tested against the 040 FPU coprocessor encodings. `QRX-VEC64` is purposefully not silently advertised as Apollo AMMX, which uses its own 68080 encodings.

## Cache correctness / ownership

Cache has 64-byte lines, 4 ways, per-core sets, least-recently-used replacement via host counter. Physical capacity of **data bytes** is 16 KiB instruction + 32 KiB data per core; host metadata adds overhead. Cache helper returns big-endian 8/16/32-bit reads; reads crossing a line or unknown RAM/ROM overlay are delegated to the normal board bus. On guest RAM writes the shared bus invokes the extension invalidator, invalidating the relevant cache lines on **both** cores. Host-DMA writers must invoke `qrx_dual_notify_ram_write` after they change RAM directly. Caches are invalidated on mode changes and explicit cache flush. The cache is *actually wired* into the tested ARM64 ADDQ/SUBQ fast-loop decoder. Musashi interpreter memory fetches are still uncached; there is no fake cycle/FPS claim.

Branch predictor: 256 indexed 2-bit counters initialized weakly not-taken. One real branch-history update occurs after each completed fast-loop block invocation, not once per unobserved virtual iteration. Stats therefore describe collected host samples, not historical CPU IPC.

## Why not simply rename the 68040 as 68080?

The proprietary Apollo68080 FPGA features a superscalar pipeline, 64-bit extensions and a hardware video subsystem with vendor-specific opcodes. Musashi implements a 68000–68040-compatible software CPU, and does not become a true 68080 by setting a CPU-identification bit. The QRX68100 instead offers **separate explicitly discoverable extensions** and retains 68040 legacy behavior. SAGA and Maggie belong in the separate QRXGPU virtual hardware API, not in the CPU instruction decoder.

Official reference features: https://wiki.apollo-computer.com/doku.php?id=apollo_core:start and https://www.apollo-computer.com/features.php .
