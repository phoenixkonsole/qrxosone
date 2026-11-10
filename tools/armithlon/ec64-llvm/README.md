# m68k64: LLVM and clang for the 68100EC64

`clang`, `llvm-mc`, `ld.lld` and `llc` for the instruction set in [docs/armithlon/68100ec64.md](../../../docs/armithlon/68100ec64.md),
on LLVM 22.1.2. It is a copy of LLVM's M68k backend (`make-fork.py`) with
`llvm-22.1.2-m68k64.patch` on top, and `llvm-22.1.2-core.patch` for the triple and clang.

```sh
tools/armithlon/ec64-llvm/build.sh <work dir>      # with the LLVM archive in <work dir>/dl
tools/armithlon/ec64-llvm/make-patches.py <work dir>   # after changing the tree
```

## ABI, version 0

| | |
| --- | --- |
| Triple | `m68k64-unknown-unknown` |
| Data model | LP64, big endian, natural alignment, 8-byte stack alignment |
| Arguments | On the stack, an 8-byte slot each, right to left. Small values at the slot's high address |
| Results | Integers in D0, D1. Pointers in A0 |
| Kept by the callee | D2 to D7, A2 to A6 |
| Code reach | PC relative, 32 bits: everything within 2 GB |
| Pointers in data | Relocation `R_68K64_64`, number 240, ours |

This ABI is a starting point and will change with AROS 64-bit.

## Not there yet

Registers D8 to D15 and A8 to A15 in the allocator, the disassembler, the privileged instructions in the
assembler, floating point (the reference core has no FPU), TLS, shared libraries in lld (no GOT and no
PLT), DWARF relocations, a C library, C++.

## Tests

- `tests/run-tests.sh` runs `tests/basic.ll`.
- `tests/run-c-tests.sh` runs `tests/ctests.c`: the same C source on the Mac and in the reference core,
  with a small loader in place of a linker.
- `tests/kernel/run.sh` builds a small kernel with `llvm-mc`, `clang` and `ld.lld` and runs it on two
  cores. It covers user state tasks with their own stacks, preemption by the timer, a shared run queue
  under a spin lock, a signal to start the second core, system calls, and three faults that end only
  their task. The result is compared with the same task code run on the Mac.
