# QRX86 JIT audit

Baseline: `25a6057a00db7201a323db80fc40e66cd459cba1`, branch
`claude/qrx-22.5.11`. Prior benchmark files are retained unchanged.

## Confirmed architecture before modifications

There are two independent engines, not the single conceptual pipeline in the task:

* `diagnostics-v6.6/tools/qrx86_grt_exec.c`: QRXGRT1 handoff loader, region-based
  memory, small IA32 interpreter, linear 4096-block lookup, conservative block
  recording, and quarantined m68k threaded emitter. Emitted code is only repeated
  `JSR helper; ...; RTS`; the helper decodes and interprets every instruction.
  There is no general native arithmetic backend, IR optimizer, register allocator,
  lazy flags, page dependency cache, chaining, or AOT in this engine.
* `x86-runtime-22.5.12/core/qrx86core.c`: a separate portable IA32/x87 interpreter,
  lazy flags, fetch window and page TLB. `win32/pe.c` loads PE32, resolves imports
  to HLE thunks or supplied guest DLLs. `qrx_win32_main.c` enters `qw_run`, then
  `x86_run`; graphics use `win32/dx.c` and backend-specific RTG functions. Linux
  i386 ELF runs through the same core via `linux/qrx86linux.c`. This is the Doom
  path; it had no JIT at baseline.

PrBoom is an i386 PE built with MinGW, not native m68k. Its game instruction code
runs in qrx86core; Win32 APIs run as native host C. The old QRXGRT JIT cannot
improve this workload without integration. Historical real-timed target baseline:
43 DirectDraw frames in 370.4497 host seconds = 0.116075 FPS in FS-UAE/68060,
320x200x8 RTG direct, 150 million guest instructions. It is not physical hardware
performance. See `benchmarks/prboom-2026-10-30`.

## Confirmed safety issues / constraints before critical edits

1. The historical 1.7 MB automatic legacy VM was already moved to BSS before this
   task. Stack corruption is a plausible historical Illegal Instruction cause,
   not a reproduced/proven diagnosis for every recorded crash.
2. The dormant legacy native helper bypasses watchdog charging. It can continue
   through cached instructions after a terminal instruction because it ignores
   `term`; its emitter has no per-block freshness validation. Do not unlock it.
3. Legacy native allocation is `malloc` plus CacheClearU, whereas the standalone
   self-test uses OS-owned public memory and targeted I/D cache synchronization.
   There is no local exception recovery around arbitrary generated instructions.
4. The standalone emitter has no whole-instruction reservation: a failed 32-bit
   write can leave a partial word in the buffer. Publication must reject overflow.
5. Modern `x86cpu` is copied by value during cooperative Linux fork; any new owned
   JIT pointer must not be shared or double-freed. Mapping/TLB flushes and raw
   host writes must not make a compiled block stale.
6. Native execution must share the existing exact instruction budget, preserve
   host callee-saved registers, and stop at requested guest EIPs. Unsupported or
   fault-capable operations must retain the existing interpreter semantics.

Baseline executed now: existing legacy interpreter host suite **15 PASS, 0 FAIL**.
Host source and prebuilt target evidence are distinct. No physical 68k hardware
or Apple Silicon machine is attached. New code needs independent emission,
execution, differential, budget, SMC and lifecycle tests before any enablement.

## Implemented experimental backend

The modern core now has an opt-in m68k backend (`core/qrx86jit.h`). It emits
32-bit MOV, ADD/SUB, AND/OR/XOR, CMP/TEST, LEA, constant shifts, NOP and terminal
short/near Jcc/JMP. INC/DEC/ADC/SBB are admitted while entry carry is live; a
full carry producer ends the block before the next carry consumer. x87, stack
operations and unsupported encodings stay interpreted. A separately compiled,
explicitly enabled leading-load experiment exists; it is disabled by default
because its higher native coverage slowed PrBoom.
Guest registers are written through; D0 reuse is local, not a global allocator.
Only the final flag producer is published because normal native blocks cannot
fault or call guest APIs. Entry carry is materialized before a carry-dependent
block. Selected ADD/SUB/logical conditions are captured into caller-saved A1
immediately after the arithmetic instruction, before stores change CCR. The
14 supported conditions have explicit tested mappings; parity and partial/shift
producers use existing C lazy-flag evaluation. CCR is never treated as a full
x86 EFLAGS value. Differential tests exercise all conditions, signed overflow,
carry, parity fallback and later D0/D1 clobbers.

The cache has 8192 compact direct-mapped slots (16 bytes each on m68k), a 16-hit hot threshold, bounded blocks
(maximum 24 guest operations, one code page), negative-entry periodic re-probes,
and byte-for-byte validation at every native entry. Only compiled slots allocate
a full instruction snapshot; negative slots carry no snapshot or executable code. This covers code written
through raw host pointers as well as normal mapping changes. There is no chaining,
AOT, writable-page protection, persistent register allocation or native memory
translation. These remain future work, not completed features.

Executable allocation uses AROS public memory with explicit I/D cache flushing;
the POSIX fixture publishes RX memory only after complete emission. Overflow
rejects publication. ABI tests check D2-D7/A2-A6 and stack preservation. Exact
watchdog and stop-EIP checks include partial blocks and 32-bit address wrap.
Linux cooperative fork drops the child's owned cache pointer. Teardown frees it.
The interpreter loop remains separate so disabled mode avoids per-op
JIT bookkeeping. The original normal loop and single-step implementation are retained.
POSIX optimizes only the JIT loop for size; AROS keeps that loop at -O2.
Global -Os and the single-step/bulk refactor were rejected on the target.
AROS reuses the opcode already fetched by the original decoder; the native
helper charges only the remaining block instructions and hashes/decodes from
the original block PC, preserving partial budgets, stop addresses and faults. API transition counters count actual HLE calls.

Legacy helper budget/terminal handling and allocation were repaired, but its
existing global quarantine remains. The standalone emitter rejects partial
32-bit writes. Failed private-stack allocation returns an error rather than
running the large runtime on the small DOS stack.

## Toolchain findings and remaining limits

A generic Linux m68k compiler cannot directly use the AROS SDK ABI. The adapter
in `tools/qrx86` explicitly adjusts A6 frame access and bridges a bounded list of
pointer-return CRT functions from D0 to A0. Its ABI probe is mandatory. It is not
a general replacement for Deadwood GCC. 68060 needs portable 64-bit arithmetic
instead of the packaged 68020 libgcc wide MUL/DIV. A recursive compiler lowering
in the first multiply helper was found and fixed with 32-bit limbs, then checked
against 100,000 host reference cases and strict QEMU 68060 execution.

Independent GNU-as byte comparison and native differential/safety execution pass
on QEMU CPU profiles 20/30/40/60. AROS/FS-UAE also passed all four profiles with the expanded MUL/DIV fallback
cases and SDK ABI probes; the final rerun logs are retained in evidence.
No physical hardware or host exception recovery has been validated. Debug traces
provide pre-entry context, not recovery from arbitrary host CPU exceptions.

The selected backend now improves the controlled QEMU PrBoom comparison.
It remains off by default pending wider application/hardware validation; see
QRX86_PERFORMANCE.md for results separated by execution layer. Full graphics tuning, wider native
coverage, compatibility certification and the requested maximum performance
remain unfinished. No unsupported CPU or application is certified by these tests.

## Optimization continuation, 2026-10-31

Tests now link a symbol-renamed copy of qrx86core.c from the audited base commit.
This independent oracle detects shared regressions in the modified interpreter
and JIT. The expanded suite checks all LEA ModRM/SIB forms, all constant shift
counts, carry edge cases, memory addressing/width/segment variants, single-step
and precise faults. A sentinel terminates each generated test program so stale
bytes from a longer predecessor cannot silently force interpreter fallback.
Native coverage is asserted; register equality alone is insufficient.

Leading load mode requires `-DQRX_JIT_NATIVE_LOADS=1` and QRX86_JIT_LOADS. It
permits only one load at block entry, validates a currently mapped same-page
address (with DS/SS selection) before any guest effect, and converts little-endian
data explicitly. Cross-page or absent mappings fall back before charging/execution
and retain original faults. It is not a general memory JIT and is not enabled
in the standard build.

QRX86_JIT_CACHE_PROFILE enables high-frequency lookup/hit counters. With
cache_profile=0 those two counters are unmeasured, not zero cache activity.
Native/fallback instruction counts, compilation, invalidation, eviction and
allocation counters remain available. QRX86_JIT_MIN_OPS supports controlled
experiments; default is 3, bounded at 3..24. Larger values did not improve Doom.
Rejected experiments include fully inlined ModRM decoding, -O3, aggressive
minimum block lengths, larger memory TLBs and global -Os. Preserve global -O2
and apply the size optimization only to the JIT loop. See the final measurements.

## Final platform defaults and bounded register cache

The engine remains opt-in on both platforms. AROS defaults to SIZE_LOOP=0,
TABLE_CANDIDATE=1, DIRTY_REGS=1 and FUSED_FETCH=1; POSIX defaults to 1,1,0,0.
Macros use the QRX_JIT_ prefix. They change the experimental implementation,
not enablement or supported-host checks. Global -O2 and the original memory TLB
size remain unchanged.

The AROS D0 cache writes dirty guest state before eviction and every native
exit. CMP/TEST preserve the guest operand before using D0 for a result; a source
that aliases cached D0 is copied into D1 rather than reloaded from stale RAM.
LEA flushes before address inputs are read. Dead register CMP/TEST can be omitted
when their flags are overwritten; guest instruction charging is preserved.
This relies on the existing no-fault/no-callback block contract. It is not
persistent register allocation across blocks. The optional leading load is
guarded before any effect.

A deeper decoder-case probe was semantically correct against the oracle but
slower in actual RTG measurements, so it is removed from the active core and
retained only under benchmark rejected-source evidence. SDK clock deltas are
unsigned/wrap-aware and reject backward/error samples with clock_invalid; they
never replace the host monotonic FPS clock.
