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
gmake kernel-utility kernel-qrx-m68k64          # bin/native-m68k64/gen/rom/boot/core.elf, AROS/Libs/utility.library
```

## Boot on the reference machine

`boot/qrxboot.c` is the boot loader for `qrx68100ec64_machine`: it relocates `core.elf` (kernel.resource,
exec.library, task.resource) and further modules below 4 GB, lays out the boot tags and starts the
kernel with A0 at them. The kernel's console is MOVEC `$8F0`.

```sh
E=upstream/armithlon/2.6/engine
cc -std=gnu11 -O2 -I$E/include tools/armithlon/aros-qrx/boot/qrxboot.c $E/full68k/qrx68100ec64.c \
   $E/full68k/qrx68100ec64_machine.c -lpthread -o qrxboot
./qrxboot core.elf <seconds> <cores> <RAM MB> utility.library
```

The command line is `selftest`: after exec's start the kernel tests tasks (Signal/Wait between two
tasks, preemption by the timer, busy tasks on two cores, workers on every core that signal the boot task
on core 0 across cores) and ends the machine with 0 when it all passed.

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
- SMP for the 68100EC64 (`kernel_smp.c`): spinlocks with CAS, CPU masks, starting the other cores
  (a core that takes its first signal with A8 still 0 enters `cpu_SecondaryStart`), IPIs on level 6,
  `core_DoCallIPI`, the x86 scheduler with this port's affinity test.
- Generic fixes: `InitResident` reads the AutoInit size as IPTR, `Tag` is an IPTR on big-endian
  64-bit, `taskres_Exit` only restores what it patched, `THIS_PROGRAM_HANDLES_SYMBOLSET` is weak for lld.

## State (2026-11-11)

- Boots on the reference machine: kernel, exec with multitasking, task.resource, utility.library.
- SMP: with 2, 4 and 8 cores every core starts, runs tasks and takes the timer; `selftest` passed 30 of
  30 runs (10 each). Tasks are bound to the core that creates them (`EXECF_CPUAffinity`, as on x86
  AROS); `Signal()` to a task on another core runs there through `core_DoCallIPI`.
- Not SMP-safe in this AROS itself: exec's semaphores and everything else that relies on `Forbid()`
  only keep out tasks on the same core. A test with roaming tasks and a shared semaphore hung in 9 of
  10 runs, which is why tasks stay bound. Making `Forbid()` and the semaphores SMP-safe is AROS work
  for later.
- Missing: dos.library and the rest of the system, drivers, the FPU context, reboot; faults stop the
  core with a register dump instead of going to exec's trap handling; the idle task spins in user
  state; the spinlock fail hook (task spinning) is not used.
- Found on the way, fixed in the patch: big-endian 64-bit bugs in AROS that little-endian x86_64 hides
  (the AutoInit table size read as ULONG, a 32-bit `Tag` at the start of IPTR tag arrays), and
  task.resource putting back vectors it never patched.
