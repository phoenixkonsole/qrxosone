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
(`629d88d766bb0d64...`). Sound is off in these runs, as in the baseline. Newer results
are in the 2026-11-04 update below.

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

- **AROS on the second core.** Core 1 runs jobs a program gives it; AROS m68k has no SMP
  scheduler, so existing software gains nothing from it until it starts such jobs.
- **Vulkan.** `vulkan-1.dll` is a stub whose `vkCreateInstance` returns
  `VK_ERROR_INCOMPATIBLE_DRIVER`, so programs can fall back cleanly. A real
  implementation would need MoltenVK.
- **Direct3D 2 to 7.** Only D3D8 and D3D9 fixed-function exist. DirectDraw stays guest
  HLE writing straight to RTG, without a host blitter.
- **No real 3D game tested.** OpenGL, Glide and D3D were only run with the test programs
  in `guest/win32tests`, which use limited subsets of those APIs.
- **MIDI.** `midiOut` has no device, so Doom95 plays without music.
- **Larger HostFS packets** need the guest handler rebuilt.

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

## Update 2026-11-04: Doom95, host APIs, second core

| Measurement | Result |
| --- | --- |
| Doom95 `-timedemo demo3`, 640x400, sound on, host door | 2134 gametics in 1592 realtics, about 47 FPS, 45.4 s |
| Doom95 normal game | 35 game tics per second, the game's own limit |
| OpenGL test, 300 frames, 3200 triangles | 395 FPS |
| Glide test, same scene | 402 to 416 FPS |
| D3D8 and D3D9 test, same scene | 300 frames in 0.26 s and 0.16 s of wall time |
| Core 1 alongside core 0, bare metal | 1.75x to 2.01x the throughput of one core |
| Core 1 alongside core 0, under AROS (`C:QRXSmpBench`) | one job 0.96 s, two jobs 1.06 s, about 1.8x |

The 47 FPS may sit close to a limit of the HLE's flip and vblank timing; that was not
checked. The API tests ran headless.

### Doom95

Start with `Execute S:Doom95 [args]`. It reads the game from `APPS:windows_games/doom95`.
What was needed beyond the HLE stubs:

- The game reads the keyboard only through a `WH_KEYBOARD` hook, which is now real.
  `WM_ACTIVATE` and `WM_ACTIVATEAPP` are delivered as well.
- Windows tick counts start low. An epoch-sized value is wrong for 32-bit programs.
- The DirectSound primary buffer is no longer played as a voice. It used to give a
  constant full-scale output in the host mixer.
- A flip publishes its frame at once, and each of the two RTG bitmaps gets its palette
  reloaded. Together these removed the fullscreen flicker: 0 of 40 sampled frames black.

The launcher dialog is skipped and answers "play", so the game starts in E1M1 and the
player waits for input. This was checked in the real window with injected keys: ESC opens
the menu, Ctrl fires and uses ammunition, the shot produces 0.55 s of sound, then silence.
Real keyboard and speaker output have not been judged by a person.

### Host APIs

An import of `opengl32`, `glide2x`, `glide3x`, `d3d8`, `d3d9` or `vulkan-1` with no guest
implementation binds to a host function on first call. Later calls stay inside the host
door, and drawing goes to a Metal batch renderer. The code is `full68k/qrx_hostapi*.c` and
`macos/qrx_hostgpu.m`, and the tests are in `guest/win32tests`.

### Second 68100 core

`full68k/qrx68100_core1.c` runs on a second host thread with its own registers and JIT
cache. It is a second compiled copy of Musashi and the JIT with all globals renamed by
`tools/gen-core1-rename.py`, so core 0 pays nothing until core 1 is first started.
`guest/qrx68100_smp.h` documents the registers.

- Core 1 reaches shared RAM and its registers only: no chipset, ROM or interrupts. Faults
  and exceptions park it and report the place.
- TAS and CAS are atomic on RAM. CAS2 is not.
- Once core 1 has started, both cores keep the 68040's memory order through ordered AArch64
  accesses. This has a cost on core 0 that was not measured.
- Code changes are not snooped between cores, as between two 68040s: a program has to
  flush, which `CTL=1` does on start.

`tests/core1_tests.c` was checked by breaking the code on purpose: without atomic TAS and
CAS the test hangs and now fails on a time limit, and without ordered accesses its message
passing check counts 59 million violations, against 0 with them.

### RAM and HostFS

- RAM is a quarter of host RAM, up to the 3840 MiB board limit, opened after boot by
  `C:QRXMoreMemory`.
- HostFS file data reaches the disk within 200 ms through a background `F_FULLFSYNC`, and
  directory changes sync before the reply. `hostfs_tests` checks this with SIGKILL, which
  covers a crash of the emulator. A power failure within those 200 ms can still lose data.
