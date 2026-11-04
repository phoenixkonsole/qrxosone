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
- **DirectPlay.** `DirectPlayCreate` reports the service as unavailable, so GTA's network
  games do not work. Winsock is there; DirectPlay's session layer on top of it is missing.
- **Direct3D.** No execute buffers (Direct3D 1 to 3 style), no lighting, no points or lines in
  versions 5 to 7. A 2D lock of a surface does not mix with what the host GPU has drawn on it.
- **Vulkan.** A subset of the API: no descriptor sets, no depth images, no textures yet.
- **HostFS packets** larger than now need the guest handler rebuilt.
- **Larger programs:** "GTA Settings" and other MFC programs need `mfc42.dll`, which is not there.

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

## Update 2026-11-04, second part: GTA 1, threads, more of DirectX

Grand Theft Auto from Rockstar's `GTA-1_installer.ZIP` runs and can be played: menus, character
and city selection, the city with its 3D buildings, sound and music at about 70 frames per
second in 640x480 at 16 bits. Install with `tools/armithlon/install-gta1.sh`, start with
`Execute S:GTA`. Tried with injected keys in the real window. `Track8.wav` is missing because
unshield cannot read a file that spans two cabinets; that was not seen to matter.

| New | What it does | How it was checked |
| --- | --- | --- |
| Windows threads (`win32/threads.c`) | Coroutines with their own x86 state, stack, TEB and C stack, switched at API calls and door quanta, real events, mutexes, semaphores, critical sections | GTA's Miles sound thread, `guest/win32tests/nettest.c` |
| 16 and 32-bit fullscreen | Own screen of that depth, frames copied with byte swap | GTA in 16 bits |
| Flip chains | Up to 7 back buffers | GTA, which asks for 7 |
| `waveOut`, `timeSetEvent` | Host mixer voice over a ring; timers on a service thread | GTA music through Miles |
| MIDI | `midiOut*` and `midiStream*` on the macOS DLS synthesizer (`macos/qrx_midi.m`) | Doom95: 500 messages, output peak 31908 of 32767 |
| Winsock | `wsock32` and `ws2_32` on host sockets (`full68k/qrx_hostnet.c`) | `hostnet_tests`, and `nettest.exe` under AROS: 512 KB TCP echo, UDP, refused connection |
| Direct3D 5, 6, 7 | `IDirect3D2`, `3`, `7` and their devices on the host renderer, with textures from DirectDraw surfaces, colour key, vertex buffers | `d3d5test.exe`, `d3d7test.exe`: image checked, 300 frames in 54 and 85 ms headless |
| Vulkan | `vulkan-1` on MoltenVK through 32-bit structure conversion (`full68k/qrx_hostapi_vk.c`) | `vktest.exe`: shaders, vertex buffer, swapchain, image checked, 300 frames in about 100 ms |
| Registry, INI files, file mappings, `mmio`, old file calls | `win32/compat.c`. The registry persists in `QRXREG.TXT` beside the game | GTA reads its settings this way |

The bundle carries MoltenVK in `Contents/Frameworks`. To build, `vulkan/vulkan.h` must exist
(`brew install vulkan-headers`); for the test programs also `glslang`.

### Microsoft's DirectX files

`tools/armithlon/install-directx.sh` downloads Microsoft's June 2010 redistributable after asking,
and puts its 32-bit D3DX, XInput, XAudio and XACT DLLs in `APPS:windows_system`, where programs
now find them. A program missing one says so in its log and names this script. It was tested
with a stand-in archive and a stand-in `d3dx9_43.dll`, not with the real download. The core of
DirectX stays QRX86's own: Microsoft's drivers need Windows.

### The other demos

See `benchmarks/armithlon-m1-2026-11-03/demo-survey.txt` for what each demo in `test-apps` did
within about 26 seconds, without input and without its installer. Of 28, these showed a game
image or a running frame rate: `NORSE95.EXE` (31 FPS), `NEVDEMO.EXE` (25 FPS), `DAYTONAD.EXE`
(20 FPS), `PPJDD.EXE` (13 FPS), `RALLY.EXE` (8 FPS), and `PCSONIC.EXE`, `VF2DEMO.EXE` and
`charlie2.exe` up to the point they stop or wait. Showing an image is not being playable: only
Doom95 and GTA got any input. The others stop at missing data because they were not installed
by their setup, want a launcher, or hit an API gap. `TOMB2.EXE` and `MDKDEM95.EXE` open a window
and go no further within that time. `msacm32` answers but has no codecs.
