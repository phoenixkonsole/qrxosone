# m68k64: LLVM and clang for the 68100EC64

`clang`, `llvm-mc`, `ld.lld` and `llc` for the instruction set in [docs/armithlon/68100ec64.md](../../../docs/armithlon/68100ec64.md),
on LLVM 22.1.2. It is a copy of LLVM's M68k backend (`make-fork.py`) with
`llvm-22.1.2-m68k64.patch` on top, and `llvm-22.1.2-core.patch` for the triple and clang. The core patch
also carries AROS's own clang driver (from AROS's `tools/crosstools/llvm` patches for clang 20), so
`--target=m68k64-unknown-aros` links through AROS's `collect-aros`.

```sh
tools/armithlon/ec64-llvm/build.sh <work dir>      # with the LLVM archive in <work dir>/dl
tools/armithlon/ec64-llvm/make-patches.py <work dir>   # after changing the tree
tools/armithlon/ec64-llvm/build-builtins.sh <llvm build> <llvm-project src> <out dir>
```

`build-builtins.sh` builds compiler-rt's generic builtins as `libclang_rt.builtins-m68k64.a` without
CMake: soft float for `float` and `double` (the core has no FPU) and the 64- and 128-bit integer helpers.
`long double` is the same as `double` on m68k64.

## ABI, version 0

| | |
| --- | --- |
| Triple | `m68k64-unknown-unknown`, for AROS `m68k64-unknown-aros` |
| Predefined | `__m68k64__`, `__mc68100ec64__`, `__LP64__`. Not `__m68k__` or `__mc68000`: code that tests those expects the 32-bit m68k ABI |
| Data model | LP64, big endian, natural alignment, 8-byte stack alignment |
| Arguments | On the stack, an 8-byte slot each, right to left. Small values at the slot's high address |
| Results | Integers in D0, D1. Pointers in A0 |
| Kept by the callee | D2 to D7, A2 to A6 |
| Code reach | PC relative, 32 bits: everything within 2 GB |
| Pointers in data | Relocation `R_68K64_64`, number 240, ours |
| Inline asm | `d`, `a` and `r` take 64-bit values. A8 is not allocated, AROS keeps its per-core data pointer there |

This ABI is a starting point and will change with AROS 64-bit.

## Frames larger than 32 KB

Frame objects are reached through `(d16,An)`. In a frame larger than 32 KB the displacement of the far
objects (the arguments, the saved registers, big local arrays) wrapped around to a negative one:
Firefox's IndexedDB (`ObjectStoreAddOrPutRequestOp::DoDatabaseWork`, 35 KB of frame) read its `this`
from the wrong place, and 64 other functions of Firefox (media codecs, ANGLE, Hunspell, Glean) had the
same problem. `eliminateFrameIndex` now forms such an address with the pseudo `LEA64far` in a virtual
address register (one definition, so frame index scavenging can give it a register), which
`M68k64ExpandPseudo` turns into `lea (0,An)` and `adda.q #offset` (neither touches the condition codes),
and uses `(0,An)`. Frames that large get two emergency spill slots near the register their locals are
reached from (SP, unless they go through the frame pointer). The offsets are sign-extended from 32 bits:
`getFrameIndexReference` hands negative ones back as 32-bit patterns. `tests/ctests.c` (`bigframe`)
covers it.

## Not there yet

Registers D8 to D15 and A8 to A15 in the allocator, the disassembler, the privileged instructions in the
assembler, hardware floating point (the reference core has no FPU, floats go through the builtins), TLS,
shared libraries in lld (no GOT and no PLT), DWARF relocations, C++.

## Tests

- `tests/run-tests.sh` runs `tests/basic.ll`.
- `tests/run-c-tests.sh` runs `tests/ctests.c`: the same C source on the Mac and in the reference core,
  with a small loader in place of a linker.
- `tests/run-rt-tests.sh` builds the builtins and runs `tests/rttests.c` the same way: `float` and
  `double` arithmetic, comparisons and conversions, and 128-bit multiply, divide and shifts. The results
  are bit patterns and must equal the Mac's IEEE results exactly.
- `tests/kernel/run.sh` builds a small kernel with `llvm-mc`, `clang` and `ld.lld` and runs it on two
  cores. It covers user state tasks with their own stacks, preemption by the timer, a shared run queue
  under a spin lock, a signal to start the second core, system calls, and three faults that end only
  their task. The result is compared with the same task code run on the Mac.
