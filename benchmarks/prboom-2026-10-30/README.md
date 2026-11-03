# Windows PrBoom through qrx86: measured baseline

Date: 2026-10-30. Branch: `claude/qrx-22.5.11`, base commit
`25a6057a00db7201a323db80fc40e66cd459cba1`.

Guest: PrBoom-plus 2.5.1.4 built with MinGW i686 GCC 14, SDL 1.2.15
DirectDraw driver, Freedoom 0.13.0 `freedoom1.wad`, `demo1`, 320x200,
fullscreen, no sound. Archived inputs passed their repository SHA-256 checks.
`tranmap.dat` was generated to completion before measurement. The old report
used a different build environment; do not infer a code speedup from it.

## AROS m68k target result

FS-UAE 3.1.66, 68060, A4000/040 model, uaegfx RTG, AROS master 5b5fd4c,
prebuilt qrx-win32 22.5.12. Warp, emulator JIT and MMU emulation disabled.
The existing Doom scenario was used without its preliminary dxtest and optional
QRXScreenGrab tool. The harness temporary paths were relocated to `/tmp`.

**43 DirectDraw frames / 370.450 wall seconds = 0.1161 FPS**,
about **8.62 seconds per frame**, at a fixed 150 million guest
instructions. The guest reported 358.112 seconds and
418,863 guest instructions/second. Scenario status:
**COMPLETED**. DirectDraw logs confirm two locked, directly mapped 320-byte-pitch
RTG buffers, capture at frame 30, the expected budget stop and clean buffer release.
`target-frame30.png` shows the actual rendered Freedoom scene.

This is one cloud-emulator observation, not real 68060 hardware performance.
The first part of the run overlapped host experiments. No speedup relative to
older machines or the old report is claimed. See `target-results.json` and
`target-live-doom.log`. The old report's 44 frames in 302 seconds is historical,
not a controlled comparison with this run or the newly built Windows guest.

## Host results (Linux x86_64, not Amiga FPS)

Same portable qrx86 interpreter and Win32 HLE compiled with GCC 14.2.0, `-O2`.
CPU reported by the machine: Intel Xeon Platinum 8370C, 2.80 GHz.

* Three fresh-defaults budget runs: 1,000,000,000 guest instructions and
  325 DirectDraw frames each. Median **29.94 wall FPS**. Individual runs:
  30.81, 29.94, 13.30 FPS. The large spread means small differences are not
  reliable evidence of an optimization. See `host-baseline.json`.
* Complete timedemo: **7,343 DirectDraw frames / 148.4575 wall seconds =
  49.46 wall FPS**, 13,964,520,047 guest instructions; 7,117 game tics completed.
  This is one observation, including loading and screen transitions. The
  program intentionally ends timedemo via its error/exit path (status 255)
  after printing `Timed 7117 gametics ...`; this was not a budget timeout.
* PrBoom's internal **5.1 FPS** uses the headless **virtual** clock and must
  not be presented as real host throughput. Likewise, 100,000 virtual ms in
  the budget tests are not 100 seconds of host time.
* Budget runs intentionally stop with `instruction budget exhausted`, exit
  139. The runner accepts this only with the exact requested instruction
  count, positive frames, and a snapshot. It does not count early exits as
  successful benchmarks.

The full demo and short budget window cover different portions of the workload;
49.46 versus 29.94 FPS is not an optimization comparison. Headless presentation
is also different from the actual RTG target backend.

## Experiments

A profiling run highlighted instruction dispatch and immediate fetching.
Forcing `fetchimm` inline gave 28.41 versus 29.63 median wall FPS across three
alternating runs each, with identical frame counts and snapshot hashes. It was
reverted. Those runs preceded the target-emulator job.

An exploratory `-O3 -flto` build also failed to establish a repeatable improvement:
23.99 versus 25.47 median FPS, identical output. Other benchmark/emulator jobs
were active during this exploratory set, so these values must not be treated as
an isolated compiler comparison. No compiler setting or production core change
was retained. All six snapshots in each comparison had SHA-256
`c5f993231d9145b59c321b2f4a9b74851e6c18d5f0fad0df0f269ee95a3ab4c5`.

## Reproduce the host window

Source root below means
`qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest`.
The source tree and application inputs are unchanged. From that directory:

```sh
bash x86-runtime-22.5.12/qmath/build-qmath.sh gcc ar /workspace/qrx-bench/baseline \
  -Wno-error=implicit-int -Wno-error=declaration-missing-parameter-type
gcc -std=gnu99 -O2 -w -o /workspace/qrx-bench/baseline/qrx-win32 \
  x86-runtime-22.5.12/core/qrx86core.c x86-runtime-22.5.12/win32/*.c \
  /workspace/qrx-bench/baseline/libqmath.a -lm
python3 x86-runtime-22.5.12/tests/bench-win32-doom.py \
  /workspace/qrx-bench/doom /workspace/qrx-bench/NEW-RESULT-DIRECTORY \
  /workspace/qrx-bench/baseline/qrx-win32
```

The two warning overrides accommodate the existing unused `weak_alias`
declaration in qmath's `remainder.c` under GCC 14; they change neither math code
nor signature/TLS verification. Build the Windows guest with the existing
`win32/apps/doom/build-sdl.sh` and `build-doom.sh`; sources and WAD packages are
archived under `upstream/doom`. Locally installed MinGW tools are on
`/workspace/qrx-tools/bin`. The benchmark requires a completed 65,793-byte
`tranmap.dat` and copies game files into a fresh directory for each run, excluding
old configuration files. It fixes virtual clock ratio and RTG mode, records wall
and child CPU time, hashes inputs/runtime/snapshots, and rejects mismatched frames.
Use a new output directory; run comparisons sequentially without builds or other
benchmark jobs. These tools do not enable the quarantined QRX86 JIT.

## Validation limits

No native i386 execution is available in this cloud kernel. The existing integer
suite against QEMU user-mode gave 4,278/4,278, 4,271/4,272, and 4,272/4,273 matches
for seeds 22512, 1 and 2. Both differing cases use AAM with a high-bit immediate;
QEMU and QRX disagree. The x87 comparison against QEMU gave 10,876/11,205 matches;
all 329 rejected cases are `inf-sin` (QEMU returns infinity, QRX returns NaN).
These are reference disagreements, not a passed native CPU differential suite.
No instruction semantics were changed in this work. Complete outputs remain in
`/workspace/qrx-bench/baseline`; native-hardware validation is still required for
an ISA correctness claim.

The Deadwood m68k compiler is not part of the archived SDK. Target measurements
use the checksum-verified prebuilt 22.5.12 HUNK binary, not a newly compiled core.
Do not infer target performance from host experiments. Real Amiga hardware was
not available.
