# QRX86 native JIT iteration 32 — 2026-10-31

Scope: QRX86's x86-to-m68k compiler. FS-UAE is a fixed execution environment;
no new QEMU runs or comparisons. Branch: `claude/qrx-22.5.11`.
Previous checkpoint `2ac369d` was committed and pushed before this iteration.

## Findings

The release interpreter's previously qualified median is 2.1184 FPS and the
original native JIT's median is 1.7288 FPS. These are startup-inclusive,
100-million-instruction PrBoom/AROS RTG runs, not a complete timedemo or a
measurement of physical 486 hardware. 25 FPS has not been achieved.

Compile-only opcode/operand and API diagnostics identify memory MOV/MOVZX,
register arithmetic, immediate shifts and short native blocks as major work.
Variable shifts occur only 18,814 times in the bounded run; they are not the
first priority. RTG flip took 73 diagnostic milliseconds across 27 calls.
API profiling includes instrumentation and nested work; it is not wall-time
performance evidence. Diagnostic profiles must not be compared as release FPS.

## Native implementation experiments

All new features default to zero; none is promoted on coverage alone.

- `QRX_JIT_NATIVE_MEMORY`: guarded interior byte/dword MOV, extensions, SIB and
  segment addressing, exact cold-TLB/cross-page/permission deoptimization.
- `QRX_JIT_KEEP_POSITIVE`: preserve published code across cold or failed
  colliding probes; raw byte freshness is still checked at every entry.
- `QRX_JIT_WRITE_CONTINUE`: continue after a store only with a host-address
  overlap guard protecting decoded successors, including virtual aliases.
- `QRX_JIT_FLAG_LIVENESS`: eliminate lazy flag metadata only when it cannot be
  observed at an exit or fault guard.
- `QRX_JIT_COMPACT_DEOPT`: immutable instruction boundaries reconstruct exact
  EIP/progress from compact native exit stubs.
- `QRX_JIT_MEMORY_CMP`: native CMP memory operands, both orders and immediates,
  precise prior flags if the read faults.
- `QRX_JIT_CARRY_CHAIN`: carry shadow connects arithmetic, INC/DEC and ADC/SBB
  without prematurely ending blocks. Full flags remain independently precise.
- `QRX_JIT_PERSIST_CC`: preserve proven native conditions across later guarded
  memory accesses; parity retains the interpreter condition calculation.
- `QRX_JIT_LOOP_BATCH`: bounded native conditional self loops, at most 256
  iterations and no more complete iterations than the remaining guest budget.
  Every memory access and code-alias store guard still executes per iteration.
  Entry-CF-dependent loops are deliberately excluded.
- `QRX_JIT_PAGE_CACHE`: experimental per-access host-page caches; mapping and
  permission changes invalidate retained pointers before native entry.

Feature manifests identify the actual compiled paths. The benchmark can require
an exact subset with `--require-jit-features`; guest options are saved verbatim.
The FS-UAE reference runner links the unchanged interpreter from `25a6057`,
using the current header and the same structure feature defines as the candidate.

## Measurements before the page-cache experiment

Same FS-UAE binary SHA256
`468b2bc4c871120615f6c19bba13ea0b5c83672e64839d9daa5c7030af52acc3`,
CPU JIT on, FPU JIT off, 68060, warp off, same Windows PE and WAD inputs,
`render_wipescreen 0`, 100M instructions. Each row below is **one observation**.

| Candidate | Frames | Wall FPS |
| --- | ---: | ---: |
| Old optional leading loads enabled | 32 | 1.3307 |
| Native memory, initial | 31 | 0.9231 |
| Native memory, minimum 8 ops / hotness 64 | 32 | 1.8976 |
| Positive cache retention only | 32 | 1.8678 |
| Memory + positive retention | 31 | 1.6041 |
| + store continuation | 31 | 1.3341 |
| + flag liveness | 31 | 1.4387 |
| + compact exits + memory CMP | 31 | 1.3435 |
| + carry chains + persistent condition | 31 | 1.3213 |
| + native loop batching | 31 | 1.3006 |
| + per-access page cache | 31 | 1.1835 |
| Page cache, minimum 8 ops / hotness 64 | 32 | 1.9797 |

All captures match SHA256
`6cea8144e940b8f60ce5a2340f9d1459047c9f3230260b6890587c15e48520e1`.
The new broad-coverage candidates are slower than the old interpreter. Increasing
native coverage to 73.6M instructions does not establish a throughput improvement.
The six-instruction synthetic loop improves from about 1,290 to 220 warm ticks
when batched, but this improvement does not transfer to the full PrBoom run.
Native memory guard cost and remaining short blocks still dominate.

## Validation and development failures

The 68060 reference suite passes native memory guards, positive retention,
store continuation, flag liveness, compact exits, memory comparisons,
persistent conditions, carry chains and native loop batching. Loop tests compare
all budgets 1..1560, the 256-iteration boundary and a fault after complete native
iterations. Tests also cover raw self-modification, host aliases, callee D2,
high-byte registers and all x86 conditions. The final combined-feature reference suite passes on all four profiles:
68020, 68030, 68040 and 68060. Each completes with ABI rc=0 and unit rc=0,
including warmed external-page remapping, raw data edits, and reallocation.

The first compact-exit boot timed out before BOOTED; the same executable passed
on retry. Early native-memory development exposed a negative-success return bug
and a test filling 8KB through a single 4KB host pointer; both were corrected
before qualification. The first page-cache reference trial completed with unit
rc=1 and an empty unit log. The oracle compiler omitted the candidate feature
defines, producing incompatible CPU structure offsets after adding the mapping
serial. The runner now applies identical defines to both builds; the subsequent
68060 rerun and the final four-profile matrix both pass. The failed original
trial is retained alongside those successful runs.

Host safety checks pass: unsupported host fails closed, watchdog/terminal helper
behavior, stack allocation failure, and 100,000 independent division cases.

Evidence: `benchmarks/jit-2026-10-31/qrx-native-memory/`. Saved configurations,
input/runtime/emulator hashes, host timing, feature masks, reference logs and
failures distinguish diagnostics from uninstrumented measurements. No claim of
maximum attainable performance or physical 486 equivalence is made.
