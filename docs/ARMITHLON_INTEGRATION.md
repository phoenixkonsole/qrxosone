# Current integration: ARMithlon 2.6

The build wrapper now selects `upstream/armithlon/2.6`. The complete supplied archive was checksum-verified and retained alongside 1.1. All 52 portable CTest cases passed; tests explicitly labelled mock or compile-only do not establish real Musashi execution. Existing QRX Linux X11 code is preserved. Evidence: `benchmarks/armithlon-2.6/ctest.log`.

The missing layers.library, GadTools, BOOPSI, fonts, Workbench startup, ASL, file locks and real DOS process management remain outstanding implementation work. This merge does not claim broad AmigaOS application compatibility.

# ARMithlon integration

The starting revision is `fb1d03e45a09e0fb896712590782011baa76de92` on
`claude/qrx-22.5.11`, the latest remote Claude branch at inspection. Existing
JIT work is preserved. The full ARMithlon 1.1 archive is retained under
`upstream/armithlon/1.1`; its SHA256SUMS describes the original import.

## Build on Apple Silicon

On the M1, install Xcode command-line tools and CMake if absent, then from this
repository (with these integration commits checked out):

```sh
bash tools/armithlon/build.sh
.build/armithlon/engine/qrx68100_vm --jit
.build/armithlon/engine/armithlon_native
```

The wrapper builds Cocoa/Metal/CoreAudio targets automatically on macOS,
keeps assertions enabled in the imported tests, and uses four build jobs
by default (`QRX_BUILD_JOBS` can override). CTest's VM tests require actual
AArch64 native execution on that architecture; Linux x86_64 results are not
substitutes for this check. No Metal or M1 execution was available in the
integration cloud session. Do not mark them passed until run locally.

`tools/armithlon/build.sh /absolute/output/path` selects another build location.
Both macOS and Linux need a C compiler, CMake, Python 3, Bash and binutils.
No GitHub token or nested clone is needed. Native Vulkan is optional and requires
its SDK, shader compiler and working ICD. No GPU speed claim follows from the
portable tests.

For QRX86/Metal interactive use consult the imported integration documentation
and frontend options. The bridge-enabled executable is
`.build/armithlon/qrx-win32`; the frontend is
`.build/armithlon/engine/armithlon_native`. The wrapper applies the original
signature-checked GPU/input/OpenGL adapters only to a disposable runtime copy.
The ordinary m68k builds and default QRX86 JIT selection are unchanged.

## Verified in this session

- Full original archive manifest: PASS.
- Linux x86_64 engine build and all 17 CTest tests: PASS (no skipped CTest cases).
- Host QRX86 build with GPU, input and OpenGL adapters: PASS.
- Existing QRXClock PE32 script: PASS; analog, digital and info PPMs generated.
- macOS/Metal, ARM64 JIT execution, Vulkan device rendering: NOT RUN.
- Native virtual-68100 AROS boot: UNSUPPORTED by this imported developer kit.

## Earlier JIT work and recovery

This conversation's previous setup logs and SDK survived in `/workspace/qrx-setup`
and `/workspace/qrx-sdk`. They initially tested main at `3287d27`, not the newer
Claude tip. The Claude branch includes the subsequent native-loop work and the
performance evidence in `docs/QRX86_NATIVE_JIT_ITERATION_32.md` and
`docs/QRX86_NATIVE_LOOP_JIT.md`. Reuse those qualified results with their exact
flags and limitations; this integration makes no new FPS claim.

The current checkout was clean before switching branches. No uncommitted tracing
patch was present here. Other chats and the failed session's filesystem are not
accessible from this session; the pasted conversation does not contain a source
diff from which those changes could safely be reconstructed. No old environment
was deleted or reset. Keep it available for recovery.

## QRX68 bitmap text extension

The repository extends the imported 2.6 sources with graphics.library SetFont (-66), TextLength (-54) and Text (-60), using guest-owned classic 52-byte TextFont structures. Monochrome glyphs support baseline, character locations, signed spacing/kerning, fallback glyph and clipping through the existing window renderer. Invalid font memory and color-font structures fail closed. Tests check actual guest ABI calls, width, cursor movement and pixels. This does not implement OpenFont, diskfont, styled/color fonts, JAM2/complement modes, TextExtent or the remaining AmigaOS compatibility subsystems. The original upstream SHA256SUMS remains the import provenance; it is not a checksum manifest for these subsequent source edits.

## QRX68 DOS locks and directory inspection

Lock (-84), UnLock (-90), DupLock (-96), Examine (-102) and ExNext (-108)
now operate on the configured HOST share. FileLock handles are allocated guest
BPTRs; FileInfoBlock is written using the classic 260-byte layout. Shared and
exclusive locks use nonblocking advisory host flock; Open also cooperates and
truncates a new-file target only after acquiring its lock. DupLock duplicates
the descriptor, preserving object identity across host renames. Runtime teardown
releases descriptors and enumeration state. Read-only shares remain read-only.

Scope: one HOST share, root and single-component paths, bounded lock table,
no symlink traversal. This is not complete DOS locking: CurrentDir/ParentDir,
DOS packets, nested paths, case-insensitive name resolution and arbitrary
filesystem handlers remain absent. Host programs ignoring advisory locks can
still modify files. DateStamp uses a UTC-based host timestamp conversion.

All 52 portable tests pass, including real host-lock contention, duplication,
release, stale handles, metadata and enumeration end. macOS and real Amiga
application binaries have not been tested. Full Layers, GadTools, BOOPSI,
font loading, Workbench startup, ASL and real DOS child processes remain open.
