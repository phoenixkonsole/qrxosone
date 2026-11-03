# ARMithlon on Apple M1: performance work, 2026-11-03

Results come from `benchmarks/armithlon-m1-2026-11-03/` on the M1 in this
repository's checkout. They are emulator measurements, not real Amiga hardware.

| Measurement | Before | After |
| --- | --- | --- |
| AROS boot, 3e9 cycles, interpreter | 10.07 s | 3.49 s, same stdout hash and instruction count |
| AROS boot, 8e9 cycles | not measured | 1.7 s with the JIT, stdout identical to the interpreter |
| Bare-metal CPU workload (`tools/armithlon/bench68k`) | not measured | interpreter 128 MIPS, JIT about 2900 MIPS, same result checksum |
| Desktop idle CPU load | one core at 100 % | 3 to 5 % |
| PrBoom-Plus demo1, 100M x86 instructions | 2.12 FPS (FS-UAE with host JIT, older 22.5.12 build) | 24 FPS including load time |
| PrBoom-Plus demo1, 1e9 x86 instructions | not measured | 32 FPS, about 91M x86 instructions per second |

The PrBoom numbers use a game rebuilt with a current MinGW, so the binary is not
byte-identical to the one behind the FS-UAE figure. Within this build, frame 10
is byte-identical with the host door and with the 68k interpreter
(`629d88d766bb0d64...`). Sound is off in these runs, as in the baseline.

## What changed

- **68k to AArch64 JIT** (`engine/full68k/qrx68100_jit.c`). Guest registers stay in
  host registers, flags stay in NZCV and are only computed where they are live.
  Blocks chain directly. Instructions without a translation go to Musashi one at
  a time, so coverage can grow by profile (`QRX_JIT_STATS=1` lists them). Under
  AROS and PrBoom over 99.9 % of guest instructions run natively. `QRX_JIT=0`
  turns it off. Self-modifying code is caught through 256-byte write gating.
- **Differential test** (`engine/tests/jit_diff_tests.c`): random instruction
  sequences through JIT and Musashi, comparing registers, flags and memory. It
  caught two real bugs during development, and a deliberately broken SUB made it
  fail. It covers the integer instructions the generator emits, not every
  addressing combination, and neither the FPU nor privileged instructions, which
  Musashi always executes.
- **Interpreter**: host-pointer page maps in place of the full bus for plain RAM;
  `_setjmp` in place of `setjmp`, which made two syscalls per time slice.
- **Guest time**: with a window or `--realtime`, beam and CIA timers follow the host
  clock at the PAL rate. A faster CPU now does more per guest second instead of
  making guest time run fast. An idle guest sleeps. Headless runs without
  `--realtime` stay deterministic for comparisons.
- **RTG**: the guest CPU has its own thread. A Metal fragment shader reads the
  framebuffer directly from guest RAM through a no-copy buffer, so presentation
  takes no CPU work and no conversion on the CPU. All FSAGA formats are covered.
- **HostFS**: file descriptor cache, directory snapshots instead of a rescan per
  entry, no `fsync` per packet (a write now syncs when its cache slot closes or
  the share unmounts, so an OS crash can lose the last writes), no full directory
  scan for absent names on case-insensitive volumes. The handler in the guest is
  unchanged and still reads in 4 KiB packets.
- **AHI**: new sub-driver `DEVS:AHI/qrxhost.audio` with mode file and default
  prefs (`tools/armithlon/install-ahi.sh`). It hands mixed buffers to the host ring,
  and CoreAudio plays them at 48 kHz. `C:QRXAHITest` plays a tone through
  `ahi.device`; headless, `QRX_AUDIO_DUMP` confirmed 1 s of 440 Hz at the expected
  length. In the window the tone also plays without errors through the CoreAudio
  queue, but nobody has listened to the speakers yet. The PAULA
  mode file moved to `SYS:Storage/AudioModes`: this board has no Paula, and AHI's
  fallback to it hung.
- **QRX86 host door** (`engine/full68k/qrx86_door.c`, guest side in `qrx86core.c`):
  with `SetEnv QRX86_HOST 1`, x86 code runs on the host from guest RAM. API thunks,
  `INT n` and faults go back to the guest, which interprets that one instruction,
  so that behaviour exists in one place only. This replaces both the 68k
  interpreter and QRX86's own x86-to-68k JIT on this board. The JIT path measured
  12M x86 instructions per second here, the door 91M.
- **QRX86 compatibility**: `__p__iob`, `__p___initenv`, `__p___mb_cur_max`,
  `__lc_codepage`, `_stati64`, `_fstati64`, `ldexp`, `GetCPInfo` and others, plus
  `atexit` returning 0, which a current MinGW runtime requires.
- **3D**: the QRX3D door now reaches Metal in the AROS window, verified with
  `QRX3DSpin` by reading back a rendered frame.

## Not done

- **Second 68100 core.** The JIT drives one core. `qrx68100_dual.c` still switches
  cooperatively on one thread and is not used by the AROS path. AROS m68k has no
  SMP scheduler, so a real second thread would speed up nothing existing until
  something in the guest explicitly schedules work on it.
- **Glide, Direct3D, OpenGL and Vulkan for applications.** What exists is the host
  half: QRX3D commands to Metal. No application-facing `glide2x.dll`, `d3d*.dll`,
  `opengl32.dll` or Vulkan loader uses it in the QRX86 HLE yet, and nothing has
  been tested with a real 3D game. macOS has no DirectX or native Vulkan, so those
  would be translated to Metal, and Vulkan would need MoltenVK as well.
- **Sound in QRX86 games.** DirectSound in the Win32 HLE is not connected to AHI;
  the PrBoom runs use `-nosound`.
- **Larger HostFS packets** need the guest handler rebuilt.
- Interactive play of PrBoom in the window has not been tried, only the timedemo.

## Rebuild

```sh
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
cmake -S upstream/armithlon/2.6/engine -B .build/m1-merged -DQRX_ENABLE_MUSASHI=ON -DCMAKE_BUILD_TYPE=Release
cmake --build .build/m1-merged -j 8
ctest --test-dir .build/m1-merged
python3 tools/qrx86/bootstrap-aros-sdk.py --cross-prefix m68k-elf- "$PWD/.build/qrx-sdk"
QRX_AROS_CC=$PWD/.build/qrx-sdk/bin/aros-gcc QRX_AROS_AR=m68k-elf-ar \
QRX_ELF2HUNK=$PWD/.build/qrx-sdk/bin/elf2hunk QRX_AROS_CFLAGS='-m68040 -Wno-volatile-register-var' \
bash qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest/x86-runtime-22.5.12/build-target.sh "$PWD/.build/qrx-target"
tools/armithlon/install-ahi.sh "$PWD" "$PWD/.build/bundle/SystemFull"
```

Then copy `.build/m1-merged/qrx68100_full_run` to
`.build/bundle/ARMithlon.app/Contents/MacOS/armithlon-engine` and
`.build/qrx-target/hunk/qrx-win32` and `qrx-i386` to `.build/bundle/SystemFull/C/`.
The local bundle in this checkout is already up to date.
