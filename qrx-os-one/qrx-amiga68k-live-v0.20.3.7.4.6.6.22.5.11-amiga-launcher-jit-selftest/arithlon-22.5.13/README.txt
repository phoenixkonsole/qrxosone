Arithlon 22.5.13 (work in progress) - 68040 CPU for x86-64 hosts in the spirit of Amithlon
==========================================================================================
core/m68k.c     reference interpreter (integer unit + FPU, user mode)
core/jit_x64.c  68k -> x86-64 dynamic recompiler (blocks, flag liveness, MOVBE,
                chaining, indirect-target cache; other instructions call m68k_step)
linux/          static m68k Linux ELF loader + system calls on the x86-64 host

Build:  gcc -O2 -std=gnu11 -o arithlon core/m68k.c core/jit_x64.c linux/arithlon_linux.c -lm
Run:    arithlon [-i interpreter] [-s stats] M68K-PROGRAM [ARGS]

Host results so far (x86-64 host, not Amiga hardware):
* m68k build of qrx-i386 running BusyBox: output identical to qemu-m68k;
  interpreter 7.99 s, JIT 0.47 s, qemu-m68k 0.69 s.
* x87 differential test through qrx-i386 (m68k): 11205/11205 under interpreter
  and JIT; JIT 1.6 s, qemu-m68k 2.25 s, interpreter 25.8 s.
* Doom (qrx-win32 m68k build, 150 M x86 instructions): JIT 19.9 s, qemu-m68k 21.5 s.
Open: SMC detection via page protection, more native instructions (BTST, BFTST,
SUBX, FPU), AROS system mode with paravirtual drivers (phase 2).
