# QRX86 native loop JIT — iteration 33

This work optimizes QRX86's x86-to-m68k compiler. FS-UAE is the fixed, qualified
execution environment. No new QEMU measurements or FS-UAE tuning are involved.
The source defaults retain the existing interpreter/opt-in behavior. The new
compiler preset is explicit and experimental.

## Implementation

The native backend now supports longer traces, guarded interior memory accesses,
low-32-bit signed IMUL, byte register aliases and register allocation into
callee-saved m68k D3–D7 inside loops. Every normal or guard exit commits guest
registers and restores the host ABI. Code freshness is checked at every native
entry, and stores retain host-address checks against decoded code, including
virtual aliases.

A per-access mapping cache shortens repeated reads/writes. Its retained host
pointers are revoked by a mapping serial incremented at every map, unmap,
protection change or translation-cache flush. With register allocation enabled,
the slow fill proves that the entire host page cannot alias CPU register storage.
The proof remains valid until the serial changes; unsafe pages deoptimize.

Backward flag and carry dataflow preserves every observable guard/exit state,
while removing shadow-carry work for dead INC/DEC flags. The final carry producer
can feed the next native iteration. Branch and carry conditions are both captured
before metadata stores alter the host CCR. Parity and unproven conditions retain
the exact C condition calculation. IMUL uses the supported 020/030/040/060
32-bit-result instruction, not the unsupported 060 64-bit-result variant.

Native conditional self loops execute at most 256 iterations per C entry and
never exceed the remaining guest instruction budget. Precise compact exits
include complete preceding iterations and the current instruction prefix.
The optional backedge policy discovers loop heads at taken backward conditional
jumps, so ordinary interpreted PCs bypass JIT lookup and compilation attempts.
It changes metadata only; x86 branch targets, flags and instruction charges are
unchanged. Small immediate values use MOVEQ; dead-flag immediate arithmetic uses
exact quick/immediate forms without changing a live carry.

## Reproduction

Activate the prepared cross-toolchain and fixed FS-UAE profile documented in
`tools/qrx86/fsuae/README.md`. Build the explicit preset:

```sh
bash tools/qrx86/build-native-loop-jit.sh TOOLCHAIN-BIN NEW-OUTDIR 60
```

The feature list is `tools/qrx86/native-loop-flags.txt`; it contains no CPU flag
and can also be used by the reference test runner. The wrapper writes the actual
compiler flags and AROS usage into its output directory. The selected native
block limit is 64 operations, code capacity 16,384 bytes. The ordinary limits
remain 24 and 4,096 unless explicitly overridden.

Before invoking the resulting `hunk/qrx-win32` on AROS:

```text
SetEnv QRX86_JIT experimental
SetEnv QRX86_JIT_LOOPS_ONLY 1
SetEnv QRX86_JIT_BACKEDGE_ONLY 1
SetEnv QRX86_JIT_THRESHOLD 64
```

The runtime manifest must report mask 4194111, max_ops=64 and code_cap=16384.
The benchmark supports requiring both the mask and block limit, rather than
assuming a binary contains the requested compiler paths.

## Correctness

The final native suite passes inside FS-UAE on all four CPU profiles: 68020,
68030, 68040 and 68060. Each completes with ABI rc=0 and unit rc=0 against the
unchanged interpreter from `25a6057`. The candidate and oracle use the same CPU
structure feature defines. Coverage includes:

- all x86 conditions, signed overflow edges, ADC/SBB carry chains and IMUL aliases;
- high/low byte registers, sign extensions and actual cached register execution;
- raw code changes, host aliases, remapping, protection and cross-page faults;
- all budgets 1..1560 across a 256-iteration batch boundary;
- a 52-instruction trace, budgets 1..156 and a late guarded fault;
- loop-carried CF at a later-iteration fault, with both initial CF values;
- CPU-storage aliases and all m68k callee-saved registers on success/deopt exits;
- actual backedge-discovered native execution and initial-loop partial budgets.

Host safety tests also pass: unsupported execution fails closed, exact shared
watchdog charging, terminal jumps, stack allocation failure and 100,000 native
reference division cases. Correctness runs use warp and may run concurrently;
their clock ticks are not FPS evidence. FPS runs execute separately with warp off.

Development failures are retained. One carry-liveness trial timed out because an
acyclic test fixture lacked an explicit terminator and included bytes from the
preceding loop. Adding INT3 restored the intended fixture. Another trial caught
one remaining admission guard that still excluded carry-dependent batches; the
guard was corrected and the full four-profile suite passed. AROS libc assertions
open modal requesters, so test assertions now retain every condition but write
the expression to the captured log and exit nonzero instead of waiting for UI.

## Benchmark method and limitations

Earlier 100M-instruction observations include loading and PrBoom's timed pitch
autotuner. The old interpreter typically chose pitch 320 after 447 iterations
per test; the faster backedge candidate executed 2452/5491 iterations and chose
pitch 352. A matching frame-10 image does not make these timed startup workloads
identical. Their single-run FPS values are retained as historical observations,
not a qualified improvement over the old interpreter median.

New matched measurements use fixed pitch 320 (`try_to_reduce_cpu_cache_misses 0`),
320×200 actual AROS RTG, no wipe, the same Windows PE/WAD/tranmap inputs and the
same FS-UAE binary/profile. The benchmark reports total-run throughput separately
from a host-monotonic interval between displayed-frame markers. Two opt-in marker
files change no guest state and do not stop execution. Marker frame numbers,
exact instruction budget, explicit budget termination and captures are validated.
Capture I/O at the end frame occurs after the interval's end marker.

The 25 FPS target is a game-throughput target. Startup-inclusive FPS, a short
frame interval, a complete timedemo and physical 486 measurements are distinct.
No physical 486 equivalence or globally maximal implementation is claimed.

## Matched measurement results

Three separate fresh runs per mode, fixed pitch 320, 300M instructions, actual
RTG 320×200, warp off. The measured interval is frame 20 to frame 70; the capture
is taken at frame 70 after the end marker. Order: control, JIT, control, JIT, JIT,
control. No other emulator, build or test runs alongside these timed runs.

| Mode | Interval FPS, three runs | Median interval FPS | Median total-run FPS |
| --- | --- | ---: | ---: |
| Default interpreter | 2.1307, 2.2659, 2.2436 | 2.2436 | 2.1628 |
| QRX86 loop JIT | 2.7738, 2.5998, 2.5907 | 2.5998 | 2.4661 |

The observed median interval gain is **15.87%**. These are three observations per
mode, not a statistical confidence interval or a complete timedemo. Every run
ends at exactly 300M instructions with 94 displayed frames and the explicit
budget fault. All six frame-70 captures match SHA256
`f58c3aa77a2a7b8fbb22bc5f3ded40260e4f66eeb16b0a834071c3c988abddbf`.

Selected runtime SHA256:
`37b8674f3541f11bf4f8457fa68b3c57b4de68f33091cb529004c9406c52735f`.
Default runtime SHA256:
`bd1e87aac64c86a4b78347f554d5914d3ea8f8f60a0e3d3f0ceda145bcc35a2f`.
Fixed FS-UAE SHA256:
`468b2bc4c871120615f6c19bba13ea0b5c83672e64839d9daa5c7030af52acc3`.

The selected run executes 78,814,159 instructions natively and 221,185,841 through
the interpreter: 26.27% native coverage. It compiles 42 blocks into 42,714 bytes,
with 340,836 native entries. The remaining interpreter paths are the next major
opportunity; the 25 FPS goal is **not reached**. No maximum-performance claim is
made. The experimental preset is useful and reproducible, but is not promoted
as the ordinary default.

Raw results, exact configurations, input/runtime/emulator hashes, frame markers,
late captures, reference logs and development failures are retained in
`benchmarks/jit-2026-10-31/qrx-native-loops/`. `summary.json` distinguishes interval
FPS from startup-inclusive throughput. Earlier single-run candidates stay in
separate directories and are not mixed into this matched comparison.

