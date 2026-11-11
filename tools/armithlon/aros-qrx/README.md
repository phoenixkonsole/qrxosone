# AROS for the 68100EC64 (qrx-m68k64)

A 64-bit AROS target for the 68100EC64 in long mode, see
[docs/armithlon/68100ec64.md](../../../docs/armithlon/68100ec64.md). `aros-5b5fd4c-qrx-m68k64.patch` applies
to deadwood's AROS at commit `5b5fd4c` (github.com/deadwood2/AROS). It builds with the m68k64 clang from
[../ec64-llvm](../ec64-llvm).

## Build

```sh
cd deadwood-AROS-5b5fd4c && git apply .../aros-5b5fd4c-qrx-m68k64.patch && autoconf && cd ..
mkdir build-qrx && cd build-qrx
# tc/bin holds clang and clang++ wrappers that add --target=m68k64-unknown-aros, and links to the LLVM tools;
# tc/lib/generic holds libclang_rt.builtins-m68k64.a from ../ec64-llvm/build-builtins.sh
CC=clang CXX=clang++ ../deadwood-AROS-5b5fd4c/configure --target=qrx-m68k64 --with-toolchain=llvm \
    --with-aros-toolchain=yes --with-aros-toolchain-install=$PWD/../tc --disable-crosstools
gmake kernel-kernel kernel-exec
```

The host needs gawk, netpbm and Python's mako. The AROS build downloads third-party source archives
(zlib, libpng, freetype and others) by itself, without checksums.

## What is in the patch

- `configure.in`: the `qrx*` target, the integrated assembler (there is no GNU assembler for this CPU), and
  an `-elf-clang` kernel compiler wrapper (`scripts/elf-clang.in`).
- `arch/m68k64-all`: CPU headers (LP64, big endian, library base in A1 for stubs, vectors are pointers),
  exception context, atomics, soft-float `fenv.h`, exec stubs, `StackSwap`, `NewStackSwap`, task
  context setup, setjmp/longjmp, vfork.
- `arch/m68k64-native`: the kernel for the reference machine. One exception entry for all 256 vectors
  builds a `struct ExceptionContext` on the supervisor stack. The timer (level 5) drives the VBlank
  quantum, `TRAP #0` is the kernel call (`KrnCli`, `KrnSti`, `KrnIsSuper` and the scheduler). Per-core
  data is reached through A8, which the compiler never allocates and user code may read.
- Generic headers: the m68k64 case in `aros/cpu.h`, `genmodule.h`, `asmcall.h`, `libcall.h`,
  `exec/interrupts.h`, `fenv.h`, `fpmath.h` (`long double` is `double`), `dos/elf.h`, `printertag.h`,
  `exec_util.h`.
- `collect-aros`: the `elf64m68k` format, and weak `__aros_libreq_*` symbols, since lld does not accept
  the same absolute symbol twice.
- `rom/kernel/mmakefile.src`: a rule for `kernel-print-includes`, which the snapshot announces but lacks.

## State (2026-11-11)

`kernel.resource` and `exec.library` compile and link as 64-bit big-endian AROS modules, with no
undefined symbols. Nothing has booted yet: the boot image (`kernel-qrx-m68k64`) and a loader for
`qrx68100ec64_machine` are next, then the second core (SMP), which the kernel does not start yet.
Known gaps in the kernel: no FPU context, no reboot, the idle task spins in user state, and faults
stop the core with a register dump instead of going to exec's trap handling.
