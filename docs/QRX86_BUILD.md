# Experimental cross-build and native tests

Run at repository root. This adapter uses the pinned AROS SDK/source (SHA-256
verified), generic GCC, official collect-aros and elf2hunk. It is not the original
Deadwood compiler. Outputs live outside tracked source. Never enable JIT globally.

## Linux / macOS Apple Silicon Docker recipe

On Apple Silicon with Docker Desktop, from the checkout:

```sh
docker build --platform linux/amd64 -t qrx86-dev -f tools/qrx86/Dockerfile .
docker run --rm -it --platform linux/amd64 -v "$PWD:/src" qrx86-dev
```

Inside the container (or Linux with the listed Debian dependencies installed):

```sh
python3 tools/qrx86/bootstrap-aros-sdk.py /tmp/qrx-sdk
ROOT="$PWD/qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest"
bash "$ROOT/x86-runtime-22.5.12/tests/run-jit-tests.sh" /tmp/qrx-native
bash "$ROOT/x86-runtime-22.5.12/tests/test-jit-safety-host.sh" /tmp/qrx-safety
bash tools/qrx86/run-aros-jit-tests.sh /tmp/qrx-sdk/bin /tmp/qrx-aros
```

The last command requires a new output directory; it verifies and extracts the
archived AROS ISO, boots headless FS-UAE profiles 20/30/40/60, runs the SDK ABI
probe and the actual generated-code suite, then requires logs and return codes.
A startup process alone is not success. First 68020 run can take several minutes.
Linux constituent commands were tested; Docker and macOS itself were not run.
Cross-build under Docker on Apple Silicon remains an unverified recipe.

Build the Win32 HLE runtime against the prepared SDK:

```sh
QRX_AROS_CC=/tmp/qrx-sdk/bin/aros-gcc \
QRX_AROS_AR=m68k-linux-gnu-ar QRX_ELF2HUNK=/tmp/qrx-sdk/bin/elf2hunk \
QRX_AROS_CFLAGS='-m68060 -Wno-volatile-register-var' \
bash "$ROOT/x86-runtime-22.5.12/build-target.sh" /tmp/qrx-target
```

Select CPU flags appropriate to the target; never deploy a 68060 binary to a
68020. The adapter conditionally supplies portable int64 arithmetic for 68060.
On AROS, leave QRX86_JIT unset/off for normal use. Explicit disposable tests may
use `SetEnv QRX86_JIT experimental`. Host builds reject native execution.
`QRX86_JIT_TRACE` is diagnostic pre-entry tracing, not exception recovery.

## Prepared cloud instance

Relocated verified tools are retained in `/workspace/qrx-tools/root`.
Use command-scoped `PATH=/workspace/qrx-tools/root/usr/bin:$PATH` and
`LD_LIBRARY_PATH=/workspace/qrx-tools/root/usr/lib/x86_64-linux-gnu`.
Prepared adapter: `/workspace/qrx-jit-work/repro-toolchain/bin`.
AROS system: `/workspace/qrx-bench/target/aros` (use QRX_AROS_SYSTEM to reuse it).
The relocated 7zip binary is `/workspace/qrx-tools/root/usr/lib/7zip/7z`.
Do not assume emulator processes survive snapshots. Use the existing checkout;
do not create a worktree. No API tokens or additional services are required.

Historical release ZIPs and vendor checksum manifests are unchanged. This is an
experimental source addition; compare its separate manifest and review changes
before building a release. Detailed evidence is in `benchmarks/jit-2026-10-30`.

Package the complete component plus experimental docs/tools/evidence with:

```sh
python3 tools/qrx86/package-source.py /tmp/qrx-artifacts
```

The output has a separate source SHA-256 manifest and change journal. Vendor
archives remain in the original checkout and are not duplicated in this ZIP.

## Expanded oracle and target checks (2026-10-31)

```sh
bash "$ROOT/x86-runtime-22.5.12/tests/run-jit-oracle-tests.sh" /tmp/qrx-oracle
QRX_JIT_FSUAE_FAST=1 \
bash tools/qrx86/run-aros-jit-tests.sh /tmp/qrx-sdk/bin /tmp/qrx-aros-expanded
```

The oracle script extracts only the audited core from Git, renames its exported
symbols and links it next to the new engine. The reference remains unchanged.
It also compiles and tests the optional leading-load experiment. Default builds
exclude native loads. The AROS suite uses the standard backend. FAST selects
unlimited emulation and warp for correctness checks only; its timing must never
be described as hardware speed. No assertions or cases are removed.

`tools/qrx86/bench-aros-doom.py --help` describes real host-monotonic frame
measurement around guest START/END markers in fresh FS-UAE runs. It uses
`-G ... 10` for capture; it deliberately supplies no `-s`, since that selects
the headless backend. Keep FAST
settings unset for those measurements. Compare the same ROM, CPU profile,
WADs, budget, game configuration and snapshot; deliberate rc139 requires a
confirmed budget fault and completed harness, not an unexplained crash.

Keep the standard -O2 build. POSIX applies -Os only to its JIT loop through a guarded GNU function
attribute; AROS uses -O2, shared opcode fetch and a local register writeback cache. A global -Os target was slower
and is not recommended. The real RTG benchmark defaults to capture frame 10
so a 100M-budget run with fewer than 30 frames still produces a usable image.
AROS backend clocks are real; frame counts can vary slightly at a fixed budget.
The tool compares the same numbered snapshot and requires positive frames.

For the final AROS defaults, omit experimental QRX_JIT_* compiler overrides.
The standard build selects the best tested AROS JIT internals but leaves JIT
OFF. In the measured FS-UAE configurations the QRX86 interpreter is still faster; QRX86_JIT=experimental is a
controlled developer experiment. Optional native loads remain compiled out.

## FS-UAE host m68k JIT (2026-10-31)

This is independent of QRX86's experimental x86 JIT. The stock FS-UAE3.1.66
crashes on AROS filesystem-ROM copies with host JIT on. The separate patched
FS-UAE3.2.35 fixes the ROM bank translation and the truncated 64-bit backing
pointer. Build instructions and the patch are in
[tools/qrx86/fsuae](../tools/qrx86/fsuae/README.md).

In this prepared workspace:

```sh
export PATH=/workspace/qrx-tools/root/usr/bin:$PATH
export LD_LIBRARY_PATH=/workspace/qrx-tools/root/usr/lib/x86_64-linux-gnu
source tools/qrx86/fsuae/enable.sh /workspace/qrx-fsuae-jit/install/bin/fs-uae
```

The profile enables FS-UAE JIT, selects unlimited CPU speed and keeps warp off.
QRX86 stays off unless explicitly requested. The native68060 differential and
safety suite passes with both JITs active. Correctness FAST/warp timing remains
unsuitable for FPS comparisons.

For PrBoom use `--fsuae-jit on --no-wipe --jit off` and then
`--fsuae-jit on --no-wipe --jit experimental`, with the same runtime and fresh
output directories. The explicit `--fsuae-jit` flag overrides the profile;
`--fsuae-jit off --no-wipe --jit off` measures the control. The benchmark records
configuration and emulator/runtime hashes and checks effective host-JIT state.
It rejects mismatched/missing frame snapshots. The harness retains its FS-UAE
configuration and Xvfb log, and fails when the display server cannot start.
Local Xvfb sockets need an execution context that permits Unix socket binding.
