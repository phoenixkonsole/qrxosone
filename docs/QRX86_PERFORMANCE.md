# QRX86 performance evidence

## Current FS-UAE baseline and 25 FPS target (2026-10-31)

Windows x86 PrBoom-Plus executes through QRX86 on Deadwood AROS68060 and
patched FS-UAE3.2.35. These are actual RTG frames at320x200, measured with
host-monotonic START/END markers, 100M x86-instruction budget, demo1, no sound,
warp off and `render_wipescreen=0`. They include application startup and are
bounded throughput samples, not completed timedemos or physical Amiga results.

| FS-UAE CPU JIT | QRX86 JIT | Runs | Host FPS | Host seconds |
| --- | --- | ---: | ---: | --- |
| Off | Off | 1 | 0.170927 | 193.064745 |
| On | Off | 3 | **2.118388 median** | 15.105829, 14.488703, 15.504259 |
| On | Experimental | 3 | 1.728819 median | 18.509747, 18.192427, 18.962123 |

Every qualified capture of frame10, including the interpreter control, is
byte-identical: `6cea8144e940b8f60ce5a2340f9d1459047c9f3230260b6890587c15e48520e1`.
The control produces33frames and each host-JIT run32frames. All stop at exactly
100M instructions with explicit budget fault, guest rc139 and COMPLETED harness.
The host-JIT rate is about12.4times the single control observation. QRX86 JIT is
about18.4% slower here; leave it off for the fastest tested PrBoom configuration.
Evidence, exact options, hashes and complete per-run logs are in
`benchmarks/jit-2026-10-31/fsuae-host-jit/summary.json` and its sibling directories.

FS-UAE's original crash has two verified causes: the filesystem diagnostic ROM
bank lacks bounded address translation, and its unreserved backing allocation
can exceed4GB although native-address JIT registers are32-bit. The separate
upstream-pinned patch fixes both. `comp_trustnaddr=indirect` respects the bank
translation; normal data accesses remain direct. No MOVEM opcode is disabled.
The selected profile keeps CPU JIT active and interprets FPU instructions:
upstream FPU JIT differs from the control in two of64,000pixels; disabling only
FPU JIT restores an exact match. See `tools/qrx86/fsuae/README.md` for setup.
The final CPU-only profile passes the SDK ABI and complete native68060 QRX86
register/flags, memory, SMC, remapping, fault, budget and stop-precision suite.

### Next implementation work, authorized scope

Target25FPS under FS-UAE only. At2.118388FPS this requires approximately11.8times
more throughput. The present JIT is not fully optimized: native instruction
coverage, hot-block policy, dispatch/chaining, fallback cost, API transitions
and RTG presentation must be profiled before selecting the next change.
RTG and the actual Win32/HLE implementation may be modified when profiling
shows relevant overhead; do not assume a complete Wine runtime is on this path.
Preserve correctness checks and matching rendered frames. Make larger measured
improvements before small tuning. Retain historical measurements below as an
archive; do not use QEMU for new comparisons or qualify new performance there.
No claim that25FPS or a global maximum has been achieved.

## Historical measurements before the FS-UAE host-JIT repair

## Final source checkpoint, 2026-10-31

Windows PrBoom PE/WADs, fresh game directories, 100,000,000 guest instructions,
QEMU m68020. Three runs per mode with rotated order and no concurrent build or
emulator benchmark. Host wall FPS includes loading. Exit139 is accepted only
with an exact budget, explicit budget fault, positive frames and a snapshot.
These are bounded frame-throughput measurements, not completed timedemos.

| Mode | Median host FPS | Seconds per run |
| --- | ---: | --- |
| Previous interpreter | 2.139234 | 16.360995, 16.617190, 16.209731 |
| Previous JIT | 1.423419 | 24.588985, 24.588679, 24.284407 |
| Final interpreter, POSIX | 2.094223 | 16.816171, 16.408798, 16.712645 |
| Final JIT, POSIX | 2.437612 | 14.358317, 14.101202, 14.405783 |

Final POSIX JIT throughput is **71.3%** above the previous JIT and
**16.4%** above the final POSIX interpreter.
All twelve runs produce 35 frames and snapshot SHA-256
`3ceb236c90b9f1ae51f739a762df89d882a4073d81341574173efb0019ffa081`.
Final on/off wrappers use the same ELF; hashes are retained separately from
wrapper hashes in `benchmarks/jit-2026-10-31/FINAL-ELF-SHA256SUMS`.
The small interpreter difference is not promoted as an optimization.

**This QEMU speedup does not transfer to AROS/FS-UAE.** See the separate RTG
measurements below. Global -O2 remains. POSIX uses -Os only for the JIT loop;
AROS retains -O2 and shares opcode fetch with the interpreter, using a local
D0 writeback cache. These platform choices are compile-time defaults, not
permission to enable JIT globally. Normal/default execution remains interpreted.

Expanded tests compare against the unchanged audited core on all four QEMU
CPU profiles. They check LEA/SIB, all constant shift counts, carry edge cases,
all Jcc, register aliases, single-step, stop/budget precision, raw SMC, mapping
changes and page faults. The optional leading-load build is included. AROS
correctness tests use unlimited emulation/warp; their times are not hardware
speed evidence. Native/fallback counts remain exact; high-frequency cache hit/
lookup counters require QRX86_JIT_CACHE_PROFILE and otherwise are unmeasured.

### Actual AROS/FS-UAE RTG measurements

Pinned AROS ROM/system, FS-UAE 68060, real AROS graphics backend, warp and emulator
JIT disabled, 320x200, demo1, no sound, fresh boot/game per run. Host monotonic
START/END markers include process loading but exclude emulator boot. 100M guest
instructions each; deliberate rc139 requires budget fault and completed harness.
No `-s` option is used: it would select the headless backend. Capture is via
`-G ... 10`. These are single cloud-emulator observations, not physical hardware
or a completed timedemo.

| Runtime configuration | Frames | Host seconds | Host FPS |
| --- | ---: | ---: | ---: |
| Previous interpreter | 30 | 199.105235 | 0.150674 |
| Selected interpreter | 30 | 199.875577 | 0.150093 |
| Initial mixed-optimization JIT | 28 | 263.425905 | 0.106292 |
| JIT loop -O2 | 28 | 257.141268 | 0.108890 |
| Immediate classifier | 28 | 269.510080 | 0.103892 |
| Immediate classifier + dirty cache | 28 | 263.863007 | 0.106116 |
| Shared opcode fetch + dirty cache | 29 | 234.917406 | 0.123448 |
| Decoder-case probe (rejected) | 28 | 248.919424 | 0.112486 |
| Final source, interpreter | 29 | 206.237110 | 0.140615 |

All runs capture frame 10 with SHA-256
`24c5613f1d537287d5a9572a80d7ce7ebf9ad0ec266dfe207f2f88fd8af17caa`.
AROS uses real clocks; total frame counts can differ slightly at the fixed
instruction budget. The first selected-interpreter run contains a brief client
environment-status event; the same process survived, and its value agrees with
the clean old reference. This event is recorded rather than silently ignored.

The controlled QEMU speedup does **not** transfer to FS-UAE. The best observed RTG option is the previous interpreter at about 0.150 FPS;
the final interpreter observation is 0.141 FPS. No target speedup is demonstrated. The best experimental
JIT configuration tested so far shares opcode fetch and delays register writes;
it reaches 0.123 FPS and therefore stays off for normal use. The final HUNK
hash exactly matches that measured JIT binary, so the result applies to the
packaged executable rather than merely similar source. This is about 6.6 to 7.1 seconds per frame for the interpreter over this
loading/rendering window,
not 50 FPS. No native hardware speed is inferred.

Global -Os, different dispatch classifiers and register caching alone did not
beat the interpreter on this target. A deeper decoder-case probe regressed and
was removed. The register-only backend's low coverage and per-block overhead
remain the practical limit of these trials; general native memory translation
would require a new correctness/performance effort, not a safe flag flip.

### Profiling correctness fix

One SDK clock sample previously produced compile_ticks=4294967377. Profiling
now uses unsigned wrap-aware deltas, rejects backward/error samples and reports
clock_invalid. Tests cover unsigned wrap, signed-boundary crossing and backward
samples. Host-monotonic FPS measurements do not use that SDK clock.

### Rejected tuning

A global -Os candidate reached about 2.01 JIT FPS in QEMU but slowed the AROS
interpreter (100M instructions, 29 frames, 224582 backend milliseconds versus
190730 previously). Frame 30 was not reached, so that candidate has no qualified
wall-FPS comparison. It is not the selected build.

Fully inlined ModRM decoding, -O3, larger minimum blocks and the leading-load
experiment also failed to improve PrBoom. Leading loads reached 22.6% native
coverage but ran slower; standard builds exclude them. TLB sizes 256/1024 gave
only small single-run differences, so the established 64-entry memory TLB stays.
Earlier single-run interpreter gains were not robust and are not final claims.

Evidence and raw exploratory logs are in `benchmarks/jit-2026-10-31`. Some
exploratory minimum-block trials overlapped compiler work and are screening
results, not qualified speedup evidence. The selected repeated comparison had
no concurrent build or emulator benchmark.

No physical hardware, 50-FPS Amiga, full application certification or absolute
maximum-performance claim is made.

# Previous checkpoint: 2026-10-30

Measured 2026-10-30, branch `claude/qrx-22.5.11`. Experimental JIT stays OFF.

## Windows PrBoom through QRX86

Final comparison uses one identical statically linked m68k Linux runtime under
QEMU `-cpu m68020`, alternating off/experimental order, three runs per mode,
100,000,000 guest instructions, isolated game directories and identical WADs.
This is **QEMU host wall time**, not physical Amiga FPS or FS-UAE FPS.

| Mode | Seconds per run | Median wall FPS | Frames each |
| --- | --- | ---: | ---: |
| Interpreter | 16.334307, 16.524745, 15.757196 | 2.142729 | 35 |
| Experimental JIT | 24.799023, 25.244865, 24.696302 | 1.411346 | 35 |

JIT throughput is 34.1% lower; median runtime is 51.8% longer. All six frame
snapshots have SHA-256 `3ceb236c90b9f1ae51f739a762df89d882a4073d81341574173efb0019ffa081`.
Exit 139 is the runner-verified deliberate instruction-budget stop, not a clean
completed timedemo. Therefore these are bounded frame-throughput measurements.
The benchmark rejects unexpected faults and zero-frame runs.

The first experimental run executes only 6,410,570 of 100,000,000 instructions
natively (6.41%), across 2,100,579 short blocks. It records 1,278,775 cache evictions
and 3,720 compilations. This establishes low native coverage and cache churn as
concrete next optimization targets. It does not isolate the cost of each factor;
a larger cache or memory codegen still needs controlled measurement and safety tests.

Register-loop microbenchmarks pass and can accelerate substantially; they do not
predict PrBoom, whose unsupported operations and cache lookups add overhead.
Optional timing instrumentation changes runtime and is unsuitable for FPS claims.
No graphics or AOT speedup is claimed.

Earlier target baseline in `benchmarks/prboom-2026-10-30`: AROS/FS-UAE 68060,
43 frames / 370.4497 host seconds = 0.116075 FPS, 150M instructions. This uses
a different runtime and emulation layer and is not directly comparable above.
The earlier x86_64 cloud result near 49.46 FPS also does not measure Amiga speed.
There is currently no evidence that the emulated Amiga achieves 50 FPS.

Reproduce with `x86-runtime-22.5.12/tests/bench-win32-doom.py --help` and provide
separate wrappers selecting off/experimental around the same runtime. Preserve
runtime ELF hash as well as wrapper hashes, inputs, results.json and snapshots.
Final evidence is retained in `benchmarks/jit-2026-10-30`.

## AROS synthetic register-loop timing

Final expanded suite in FS-UAE, 1,200,002 guest instructions per measurement,
AROS clock at 50 ticks/second. Single samples, not repeated application FPS:

| CPU profile | Interpreter ticks | JIT cold ticks | JIT warm ticks |
| --- | ---: | ---: | ---: |
| 68020 | 3139 | 1198 | 1198 |
| 68030 | 81 | 32 | 32 |
| 68040 | 77 | 30 | 31 |
| 68060 | 84 | 31 | 34 |

One 96-byte native block, 3,599,910 native instructions and 96 interpreted
instructions over the measured sequence. This illustrates the narrow successful
case. It does not contradict the slower PrBoom result, whose coverage is much lower.

## PE32 integration check on AROS/FS-UAE 68060

The freshly rebuilt runtime executes `jit-pe-loop.exe` through its real PE loader
and Win32 HLE, off and experimental. Both return the expected ExitProcess(42)
and count one actual API transition (eight imported thunks are a different count).
JIT executes 9,999,925 native instructions plus 93 fallback instructions.
One diagnostic sample reports 12,305 virtual milliseconds off and 5,367 on.
These are runtime virtual-clock figures, not repeated host-wall measurements,
Doom FPS or physical hardware throughput. The 94-byte block covers almost the
entire synthetic loop; this is deliberately unlike PrBoom.

Under QEMU m68020, the same PE32 test in three alternating runs has median host
wall time 1.333419 seconds interpreted and 0.453027 seconds experimental (2.94x).
All six exit with 42; all JIT logs show one compilation and 9,999,925 native
instructions. This synthetic speedup must not be presented as a game speedup.
