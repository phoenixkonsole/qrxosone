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
gmake linklibs-romhack kernel-dos kernel-timer kernel-bootloader kernel-shell
gmake kernel-expansion-quick kernel-econsole-quick kernel-dosboot-quick kernel-shellcommands-quick
gmake kernel-aros-quick kernel-lddemon-quick kernel-filesystem-quick kernel-fs-afs-quick kernel-qrxdisk-quick
gmake compiler-m-m68k64 compiler-m-quick compiler-stdlib-m68k64 compiler-stdlib-quick \
      compiler-crt-m68k64 compiler-crt-quick                # m.library, stdlib.library, crt.library
cp ../tc/lib/generic/libclang_rt.builtins-m68k64.a bin/native-m68k64/AROS/Development/lib/
```

Programs build with the SDK: `clang --sysroot bin/native-m68k64/AROS/Development -O2 -o hello hello.c`,
with the build's `bin/darwin-aarch64/tools` (collect-aros) in `PATH`. The clang driver links
`libclang_rt.builtins-m68k64.a` for soft float. If a quick target stops with "Error 20" while
generating includes, create its `include/{proto,clib,defines,inline}` directories under `gen/` once.

The `-quick` targets build only the module: their full targets also pull in libpng, zlib and other
workbench libraries, which do not build for this target yet.

## Boot on the reference machine

`boot/qrxboot.c` is the boot loader for `qrx68100ec64_machine`: it relocates `core.elf` (kernel.resource,
exec.library, task.resource) and further modules below 4 GB, lays out the boot tags and starts the
kernel with A0 at them. The kernel's console is MOVEC `$8F0`: writing it puts a character out, reading
it takes the next input character.

```sh
E=upstream/armithlon/2.6/engine
cc -std=gnu11 -O2 -I$E/include tools/armithlon/aros-qrx/boot/qrxboot.c $E/full68k/qrx68100ec64.c \
   $E/full68k/qrx68100ec64_machine.c -lpthread -o qrxboot
./qrxboot core.elf <seconds> <cores> <RAM MB> <module>...
```

- `$QRXBOOT_CMDLINE` is the kernel command line, `selftest` by default.
- `$QRXBOOT_INPUT` is the console input, `\n` for a new line.
- `$QRXBOOT_MAP=<file>` writes the address of every function, for addresses in the console output.
- `$QRXBOOT_DISK=<image>` puts a disk image at the top of RAM, outside exec's memory, and adds
  `qrxdisk=<address>,<size>` to the command line. `qrxdisk.device` serves it and adds the boot node
  `DH0:` with the Fast File System. The contents stay over a reboot; `$QRXBOOT_DISK_WRITE` writes them
  back to the file at the end.
- When the system ends the run with a reboot (`ColdReboot()`, `ShutdownA()`), the loader loads it again
  and starts it with ` rebooted` added to the command line, as a reset would.

Command line words:

- `selftest`: after exec's start the kernel tests tasks (Signal/Wait, preemption by the timer, busy
  tasks on two cores, workers on every core that signal the boot task across cores), faults in user
  state (divide by zero, privilege, illegal, bus fault) that reach the task's trap handler, and the jump
  code of in-memory segments. It ends the machine with 0 when it all passed. With `reboottest` it calls
  `ColdReboot()` first, with `crashtest` a task faults without a trap handler first.
- `econsole`: dosboot boots from the emergency console `ECON:`, dos.library starts the AROS shell on
  the machine's console.
- `idledump`: when core 0 has run only its idle task for 5 seconds, the kernel prints every task with
  its state, signals and PC.

`boot/mkffsimg.py <directory> <image> [MB] [name]` makes an FFS (`DOS\1`) image from a host directory.
For booting from it, the directory needs a file `AROS.boot` that contains `m68k64`.

To the shell from disk: the modules utility.library, aros.library, timer.device, expansion.library,
bootloader.resource, dos.library, lddemon.resource, econsole-handler, FileSystem.resource,
afs-handler, qrxdisk.device, shell.resource, shellcommands.resource and dosboot.resource, the image
in `$QRXBOOT_DISK`, and `QRXBOOT_CMDLINE=econsole` (the console is the emergency console, there is
no `CON:` without intuition). `SYS:` is then the disk, `C:` and `LIBS:` load from it.

To the shell without a disk: the modules utility.library, timer.device, expansion.library, bootloader.resource,
dos.library, econsole-handler, shell.resource, shellcommands.resource and dosboot.resource, and
`QRXBOOT_CMDLINE=econsole`.

`CLANG=<clang wrapper> boot/run-tests.sh <build-qrx/bin/native-m68k64>` runs all of it: the selftest on
1, 2, 4 and 8 cores, the reboot, exec's alert for a fault, the DOS boot with shell commands from the
console input, and the boot from an FFS disk with `boot/tests/hello.c`, built with the m68k64 clang
against the AROS C library: printf, arguments, qsort, soft float, malloc, and a file written and read
back on the disk.

The host needs gawk, netpbm and Python's mako. The AROS build downloads third-party source archives
(zlib, libpng, freetype and others) by itself, without checksums.

## In the emulator

The engine has the machine as its own program, `qrx_ec64_run` (built with the engine, see
`upstream/armithlon/2.6/engine`). The 32-bit QRX OS One (`qrx68100_full_run`) does not link any of it;
its binaries are byte for byte the same as before. The loader is the engine's `full68k/qrx_ec64_boot.c`,
the devices are `full68k/qrx_ec64_board.c` (see "ARMithlon board" in
[docs/armithlon/68100ec64.md](../../../docs/armithlon/68100ec64.md)).

```sh
qrx_ec64_run [--cores N] [--ram MB] [--disk disk.img [--disk-write]] [--cmdline "econsole"]
             [--window | --stdin] [--input TEXT] [--seconds S] [--stats] [--capture fb.ppm] \
             core.elf <the modules of the disk boot above>
```

- Without `--window` the console is stdout, with `--stdin` the shell reads stdin.
- `--window` opens a Mac window: the console, and the board's framebuffer once a program turns it on.
  Typed text goes to the console.
- `--capture` writes the framebuffer at the end as a PPM.
- `boot/tests/fbtest.c` draws into the framebuffer, `boot/tests/bench.c` measures with the board's clock
  and instruction count.
- `include/qrx_instance.h` is the interface for a second instance, Classic (the 32-bit system), in
  the same machine. Classic does not use it yet.

Measured on the M1 (2026-11-12), `bench` (an integer hash, a sort, a prime sieve), the same results as
the Mac build:

| | Time | |
| --- | --- | --- |
| Native on the M1, `cc -O2` | 14 ms | |
| AROS 64 on the EC64 interpreter in the engine | 1103 ms | 82 million instructions a second, 79 times slower |
| For comparison: the 32-bit 68100, `bench68k` (docs/armithlon/performance-2026-11-03.md) | | interpreter 128, JIT about 2900 million a second |

Two `bench` at once take as long on 4 cores as on 1: a new task stays on the core that creates it
(`EXECF_CPUAffinity`), so the shell's programs share one core. Spreading them needs an SMP-safe
`Forbid()` and semaphores first (see State). A long-mode JIT is the next large step for speed.

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
- Faults: the kernel's exception handlers first, then the task's `tc_TrapCode` or exec's, which turns the
  fault into an alert for the task; in supervisor state a register dump and a stop.
- Reboot and power off: a reset callback at the end of exec's chain ends the run with `$100` plus the
  `SD_ACTION`, the boot loader starts the system again for a reboot.
- `RemTask(NULL)` on SMP leaves the TaskRunning list before the housekeeper frees the task
  (`EXEC_REMTASK_NEEDSSWITCH`, as on x86). Before, the freed task stayed linked there and tasks of the
  ready list got lost.
- `KrnMayGetChar` (TRAP #4), the 64-bit relocation in dos.library's ELF loader, setjmp/longjmp for
  dos.library (`linklibs-romhack`).
- The C library (deadwood's `m`, `stdlib` and `crt` libraries) with the m68k64 setjmp, longjmp,
  vfork and fenv; library entry points are jumps (`AROS_GM_STACKALIAS`), since an ELF alias to a
  symbol of another object stays undefined with the integrated assembler.
- `qrxdisk.device` (`arch/m68k64-native/qrx/qrxdisk`), a disk in RAM for the reference machine.
- Generic fixes: `InitResident` reads the AutoInit size as IPTR, `Tag` is an IPTR on big-endian
  64-bit, `taskres_Exit` only restores what it patched, `THIS_PROGRAM_HANDLES_SYMBOLSET` is weak for lld.
- Generic fixes found on the way to the shell, all in this AROS snapshot:
  - `ObtainSystemLock()` and `ObtainLock()` in `exec_locks.c` tested the flags with `&&` instead of `&`, so
    every lock with a flag took both `Disable()` and `Forbid()` and gave back only one: after the boot
    the boot task was left with `TDNestCnt` 15 and `IDNestCnt` 4, and higher priority tasks did not
    preempt it. This also affects x86 SMP.
  - `RunCommand()` only ran segments that `LoadSeg()` registered as ELF, so the shell's built-in
    commands (made in memory) always failed with -1. Unregistered segments now count as native code,
    68k hunk segments are still refused.
  - `ECON:` did not know the name `*`, the current console, so the shell could not open its error stream.
  - econsole and shellcommands link `stdc.static`, as the other ROM modules do.
  - `vfwprintf.c` passed a `wchar_t` callback where an `int` one is declared; clang 22 makes that an
    error.
  - The FAT handler does not convert all its little-endian fields (it writes cluster numbers and
    sizes unswapped), so it does not work on a big-endian CPU. Not fixed: the disk uses FFS.

## State (2026-11-11)

- Boots on the reference machine: kernel, exec with multitasking, task.resource, utility.library,
  timer.device (VBlank based, 50 Hz), expansion.library, bootloader.resource, dos.library, dosboot, the
  emergency console and the AROS shell with its 28 built-in commands, and from an FFS disk with
  `SYS:`, `C:` and `LIBS:` on it. C programs built with the m68k64 clang run with the C library from
  disk. `boot/run-tests.sh` passes all 8 tests: the selftest on 1, 2, 4 and 8 cores (also 40 of 40
  repeated runs), reboot, the fault alert, the shell (also on 1 and 8 cores), and the disk boot with
  the C program.
- SMP: tasks are bound to the core that creates them (`EXECF_CPUAffinity`, as on x86 AROS); `Signal()`
  to a task on another core runs there through `core_DoCallIPI`. Tasks without affinity, such as the
  shell, run on any core.
- Not SMP-safe in this AROS itself: exec's semaphores and everything else that relies on `Forbid()`
  only keep out tasks on the same core. A test with roaming tasks and a shared semaphore hung in 9 of
  10 runs, which is why the tests bind their tasks. Making `Forbid()` and the semaphores SMP-safe is
  AROS work for later.
- No FPU context: the 68100EC64 has no FPU, floating point is soft float and keeps no state (`fenv` has
  no flags and one rounding mode). There is nothing to save until the core gets an FPU.
- Missing: `CON:` and so the Startup-Sequence (it needs console.device and intuition), the C:
  commands of the workbench (only the shell's built-in ones exist), graphics, intuition and the
  drivers for them (the reference machine has no display); C++ is untested;
  the rest of the workbench libraries (libpng and zlib do not build yet); D8-D15 and A9-A15 are not saved
  in the task context, since the compiler does not use them yet; the idle task spins in user state; the
  spinlock fail hook (task spinning) is not used.
- Found on the way, fixed in the patch: big-endian 64-bit bugs in AROS that little-endian x86_64 hides
  (the AutoInit table size read as ULONG, a 32-bit `Tag` at the start of IPTR tag arrays), and
  task.resource putting back vectors it never patched.
