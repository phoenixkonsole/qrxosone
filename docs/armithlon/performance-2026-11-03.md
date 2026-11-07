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
- **DirectPlay beyond GTA.** Only the calls GTA makes exist, see "GTA network games" below.
  Lobby, groups and the service providers for IPX, modem and serial are missing.
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

## Update 2026-11-04, third part: GTA network games

"Gather Network" and "Join Network" in GTA 1 work between two ARMithlon instances over TCP/IP.
`win32/dplay.c` implements DirectPlay on the Winsock door. Its protocol on the wire is QRX86's
own and **does not talk to real DirectPlay**, so a QRX86 game cannot join one on Windows.

Which calls GTA makes was read from its code around the `dplayx` imports (ordinals 1 and 2) and
checked in the log of a running game. Only these exist; any other method stops the program with
its name in the log:

| Call | Used for |
| --- | --- |
| `DirectPlayEnumerateA` | Lists one provider, "Internet TCP/IP Connection For DirectPlay" |
| `DirectPlayCreate`, `QueryInterface(IDirectPlay2A)`, `Release` | The object GTA holds |
| `EnumSessions` | Search, with the default timeout of 1.5 s |
| `Open` with `DPOPEN_CREATE` or `DPOPEN_JOIN`, `Close` | Gather, join, leave |
| `CreatePlayer`, `DestroyPlayer`, `EnumPlayers` | Players; ids rise, and GTA sorts by id, so all see the same order |
| `Send`, `Receive` with `DPRECEIVE_ALL` | Game data, one target at a time; `DPID_ALLPLAYERS` is not there |

How it works:

- **Finding a session.** UDP probes go to ports 47800 to 47807 on the loopback address and as a
  broadcast. For another network, `SetEnv QRX_DPLAY_HOST name-or-address` before the game adds that
  host. Each host takes the next free port, which is how two emulators share one Mac.
- **In a session.** Each member holds one TCP connection to the host, which hands out player ids
  and relays data. So plain and guaranteed messages both arrive complete and in order.
- **Busy programs.** A service thread keeps the session answering while the program does other things.
- **Security.** There is no password and no encryption: any computer that reaches the ports can
  join an open session. Use it on a network you trust.
- **A host leaving** ends the session: GTA shows "Connection To Travis Has Failed" and the member
  plays on alone. There is no host migration.

To try it, start two emulators with `Execute S:GTA`. They must not share a game folder, because
each writes its settings there. In the first choose Gather Network, your character, the city,
the multiplayer options, the connection and the session name; wait on the Gather screen. In
the second choose Join Network, your character, the connection and the session. When the first
shows "Found 1 Joiner", press Enter there.

How it was checked, on the Mac M1 (log: `benchmarks/armithlon-m1-2026-11-03/directplay-gta.log`):

| Check | Result |
| --- | --- |
| `dptest.exe` alone in one engine (`guest/win32tests/dptest.c`): two hosts, one member, player lists, small buffer, unknown player, leaving, host gone | Passed 5 of 5 runs, 2000 messages each way, none wrong |
| `dptest.exe host` and `dptest.exe join` in two engines | Passed, 2000 messages each way, none wrong, about 4.5 ms for a round trip |
| GTA in two `armithlon-engine --window` processes, menus driven by `QRX_TEST_KEYS`, images from `QRX_RTG_CAPTURE` | Gather shows "Found 1 Joiner", Join "Found Gatherer"; both load Liberty City Deathmatch, one as yellow, one as green, and run at about 60 frames per second |
| Data in that game, with a temporary dump of what was sent and received | GTA's start message, both player names, its own handshake and key events arrived unchanged; 1154 messages each way, none lost, about 23 per second |
| Host quits with Esc and Enter | Member logs the lost connection, shows the message above and keeps running |
| The same game once more with the final build, without the dump | 1138 messages each way, none lost; the log above is from this run |

A bug that has nothing to do with the network showed up: after GTA quits cleanly from a game, it
saves `GTADATA/PLAYER_A.DAT`, and the next start of any level stops with a page fault at
`eip=00414ce3`, in single player too. It reads video mode 6 from GTA's mode table and finds no
entry there. Putting the original file back gets past it. It is not fixed yet.

Not checked: two computers, which means `QRX_DPLAY_HOST` and broadcast over a real network, and whether
the macOS firewall asks for permission there; more than two players; a whole deathmatch to its
end. `dptest.exe` alone expects to find exactly its own two sessions, so run it with no other
session open. The two players started apart and were not driven into each other, so the images
do not show one player's car on the other's screen; the key events crossing is the evidence for that.

## Update 2026-11-04, fourth part: x86 JIT, window mode, icons

### Measured first

Daytona USA ran at 9 to 20 frames per second and almost all CPU time was in the x86 interpreter on the host. After the JIT below, x86 code took about 1% of the time. What remained was this:

- 40% in `jit_notify`: writes to AROS system variables right next to translated 68k code walked the page's whole block list, 1.7 million times a second. The 68k JIT now keeps one bit per 8 bytes per page and skips the walk. All of AROS gains from this.
- Daytona waiting for itself, with 225,000 `PeekMessage` and `timeGetTime` calls per 5 s in its menu and demo scenes. Those run at 20 frames per second by the game's own timing, not for lack of speed. In a race it ran at 300 frames per second without a limit, which is too fast.

### QRX86 host JIT

`full68k/qrx86_hostjit.c` translates x86 to AArch64 blocks in the door. The interpreter stays the reference: instructions without a translation, memory accesses that miss the tables and NaN results run there, one instruction at a time. Native code changes no guest state before its last memory access, so a re-run and any fault see the state at the start of the instruction.

- Registers are pinned, memory goes through flat page tables, and flags use the host's NZCV. The lazy flag state is written only where a liveness pass and a short look into the successor blocks say it may be read.
- x87 loads, stores and arithmetic are native.
- Code pages are write protected for x86 and for 68k writes and DMA, so self-modifying and reloaded code is noticed. A page rewritten 16 times stays with the interpreter.
- `QRX86_NOJIT` in the host environment turns it off. `QRX86_PROF=1` prints a profile that also names x86 addresses.
- `tests/qrx86_jit_tests.c` runs random instruction sequences through both and compares registers, flags, x87 state and memory: 200,000 cases differ in none, see `qrx86-jit-differential.log`. It also tests self-modifying code and that a linked loop ends at its budget.

### Frames

- A Flip or `WaitForVerticalBlank` now waits for a 60 Hz tick, because games take their pace from it. `DDFLIP_NOVSYNC` and `QRX_WIN_NOVSYNC` skip the wait. The D3D7 test does 600 flips in 9990 ms.
- Door service 5 converts a whole frame on the host. The 68k needed a load, a table look-up and a store per pixel.

### Window mode

Fullscreen DirectDraw programs now get a centred window of the mode they asked for, on the desktop. `qrx-win32 --fullscreen` or `SetEnv QRX_WIN_MODE fullscreen` gives the old behaviour. Direct3D, OpenGL, Glide and Vulkan output is drawn by the Mac into the whole ARMithlon window: it is not in a window of its own on the desktop.

### Icons

`tools/armithlon/desktop` adds to QRXDesktop, built by `tools/armithlon/build-desktop.py` without touching the canonical source:

- Windows icons from the resources of `.exe`, `.dll` and `.scr` files, and `.ico` files. 70 of 86 programs tried gave one; the 16 others were not looked at one by one.
- AmigaOS classic images, converted with the Workbench colours because a deep screen's low pens hold something else. GlowIcons and AROS PNG icons through icon.library, shrunk when too large. NewIcons take the same path but no sample was available.
- A double click on a Windows program runs it on QRX86, in its own folder. Before, it went to `QRXWinRun`, which looks for a Wine that is not shipped.
- The file manager lists 256 entries, not 64.

The shipped desktop had Pack and Unpack menus that are not in the canonical source. They come from a local snapshot and are now `archive-actions.patch`, so a rebuild keeps them.

### All programs in windows_games

`windows-games-survey.txt` has one line per program and `windows-games-contact-sheet.png` the screens. All 68 ran for 44 s without input, so this says how far each one starts, not that it can be played. Frame rates are from five engines running at once.

| Result | Programs, by entry in the survey file |
| --- | --- |
| Picture and frame rate | daytona (track missing in this run, see the correction below), doom95 35, nrse95d 31, sega_rally 30, vsquad 30, nevdemo_full 25, pbdemo title. GTA 1 60 to 70, measured alone |
| Window and a first screen or dialog, waiting for input | tr2demo1, tr2demo2, raptorshr, gextrial, nfighter, socdemo, shadowsdemo, turok3dfx, turok2demo |
| Window stays black within 44 s | abeodd, ff7demo(battle), ft2dem95, mdkpcw95, nfs2demo, nfs2sea, s3d_demo, MSIFDEMO |
| No window within 44 s | beavislg, bunghole, ch2setup, dnmp_demo_101, mj97, mscasinotrial, MSpuzzle |
| Page fault, not examined yet | BB2001, FinalFantasy, HaloTrial, MtM2trial, naboo, pitdroidsdemo_pc, cursedemo, sk_demo, vf2_demo, w95turokdemov101, islepro |
| Missing function | MaxPayneDemo1-05 (`_EH_prolog`), sc3000_demo (`operator new`), diablosw (`SHGetDesktopFolder`), bossw13 (`OpenMutexA`, added after this run and not run again) |
| Asks for data, a CD or its launcher, or refuses | MSAoE, fury3x, heltrial, mtmtrial, descent3demo2, rashdemo, b2demoa, aoe2demo(Japanese), roguedemo (assertion), sonic_cd (256 colours, still refused) |
| 16-bit, which QRX86 does not run | Setups of Worms2Demo23Feb98i, ff7demo, ft2dem31, jamxdemo, and freddi, galac15 |
| Other installers | clawdins and gmdemins: extraction fails. J2swc123 and SCDemo stop at their first message, DiabloIIDemo and MaxPayne2DemoSetup show nothing |

Input and sound were not judged by a person in any of them. Installers ran only as far as listed, so programs unpacked with host tools can lack settings their setup would write.

## Update 2026-11-05: corrections, input, desktop look

### Corrections to the fourth part

- Daytona's "about 60 FPS" and "correct graphics" were wrong. The track was missing. The interpreter runs REP string instructions in portions and stays on the instruction until the count is done, and a translated block went on to the next instruction regardless, losing the rest of the copy. Blocks now leave when the interpreter is somewhere else. The differential test had no REP and now has it, with large counts. With the track, Daytona runs at 40 to 50 frames per second in a race, measured with the game paused by a test key, so a race proper is still to be measured.
- Fullscreen programs no longer get a window by default, which was a misreading of the request. They get the display as on Windows. Alt+Enter toggles any exclusive DirectDraw program at any time, and `--window` or `SetEnv QRX_WIN_MODE window` starts in a window.

### GTA 1 fullscreen on the desktop

GTA 1 stopped while loading after the menu when it ran fullscreen with QRXDesktop running. It was neither the JIT nor the runtime: `qrx-win32` waited for a signal inside `malloc`, called from `HeapAlloc`. The 16 MiB of boot memory, which was used first, had 122 KB left, with 3.9 GB free in the board RAM behind it. `QRXMoreMemory` now puts the board RAM first in exec's memory list. That fixed the stall: the game runs fullscreen, and 11.8 MiB of boot memory stay free. That AROS's low memory handling is where it waited is inferred from the stack, not seen in its source.

A survey of all 68 programs afterwards shows no regression: the same six keep their frame rates (`windows-games-survey.txt`).

`QRX86_PROF=1` now also prints which AROS task runs, the free memory per region, and the stack of a waiting `qrx-win32`. A task that seems to run 99% of the time is usually an idle system: AROS keeps the last task as current while it idles.

### Input

- `QRXHostInput` polled with `Delay(1)`: at best 30 updates a second, with keys in bursts. It polls every 2 ms now through timer.device and passes only the newest pointer position of a burst.
- The virtual key was taken from the character the key gives with the current qualifiers. Ctrl, Alt or Shift could turn that into nothing, and the key press was lost. It now comes from the key alone, and a release uses the key its press gave.
- macOS auto repeat is marked as a repeat and dropped for programs that present DirectDraw frames. Keys are released when the window loses focus, on the Mac and in the guest.
- Tested with injected chords: Ctrl+X, Ctrl+Z, Shift+1, Shift+A all arrive with their releases. Nobody has yet judged this by hand.

### Dragging windows

The terminal followed the pointer 30 times a second, 58 pixels behind. It is now 53 times a second, 34 pixels behind, with the pointer moving 22 pixels per display frame. A drag waits for its last `MoveWindow`, because the call is asynchronous and relative, and refreshes no longer reload the palette. `QRX_TEST_DRAG` injects a drag, and building with `QRX_DRAG_STATS=1` makes the desktop log the numbers.

### Desktop look

- `tools/armithlon/desktop/gen-assets.swift` draws three wallpapers (Convergence, Horizon, Aurora) in four sizes, and seven icons for each of the four designs. `build-desktop.py` puts them into the system tree, about 44 MB that is not in the repository.
- All designs use their own icons for drives, drawers and files without a picture of their own. Programs keep theirs.
- Drives: the desktop now lists every mounted volume and APPS:, which is mounted as a device and has no volume entry.
- `QRXDesktopPrefs`, opened from Settings, changes wallpaper and pointer. The desktop applies and saves them at once. The pointer is the Mac's own, chosen through door service 6. The five pointers were not looked at in the window, because captures do not show the host pointer.

### Games so far

Going through them one by one has only started. Sonic CD now passes its 256 colour test, loads its DLLs from subfolders and has lz32 with SZDD support, and then faults reading a sequence in DINO2D.DLL. The fixes on the way help others too: `LoadLibrary` with a path, `RealizePalette`, off-screen window positions.

16-bit programs are not started on. They need an NE loader, segmented addressing in the core and the Win16 API.

## Update 2026-11-05, second part: rhythmic slowdowns, Sonic CD

### The display ran at 30 Hz

Windows games felt fast but slowed down in a rhythm. `QRX_PRESENT_STATS=1` showed why: the window showed exactly 30 frames a second on a 60 Hz display, whatever the game did. `nextDrawable` waited 33 ms per frame. A game's own rate beats against that: GTA 1's 24 frames against 30 gives a 6 Hz rhythm, and no game was ever seen above 30.

Frames now follow the display through `CVDisplayLink`, the vsync wait in the Metal layer is off, and the engine registers as latency critical so that macOS does not throttle it. Measured on the desktop and in GTA 1: 300 frames in 5 s, nearly all 14 to 18 ms apart. Single gaps of 30 to 80 ms remain, seconds apart and without a pattern. The measurements come from one Mac with the window open; whether the slowdowns are gone for the eye has to be judged by a person.

Measured and found not to be the cause, during play in GTA 1 and Daytona's demo:

- x86 and the guest: `QRX86_STALLS=1` logs every pause over 8 ms in x86 execution. They come in clusters while loading and are rare and short otherwise.
- Sound: the queue never ran empty, lowest 1376 of 2400 frames.
- Whole frames: Flips are even, GTA 1 at 42 ms and Daytona's demo at 50 ms. These intervals are in the `-v` log as `Flips:`.

### Frame rate numbers

`DirectDraw: N frames per second` counts every update of the primary surface. For GTA 1 that said 70 while the game shows 23 whole frames a second, so the 70 in earlier parts of this document means updates. The `Flips:` line gives whole frames.

### Sonic CD

It installs with its own `SETUP.EXE` and runs its level demo in colour, in its window, at an even 30 frames a second. Run the game from where the setup put it; a copy in another folder faults, because `sonic.ini` names the install path.

What it took, and what other programs gain from it:

- `LZInit` returned 0 instead of the handle.
- GDI palettes are real: `CreatePalette`, `RealizePalette`, `AnimatePalette` and the system palette, which an 8-bit DirectDraw primary without a palette shows.
- A DirectDraw program without exclusive mode gets a primary as large as the desktop, and its window shows the part under it. If a program draws at a fixed place that the window does not cover, as Norse by Norse West does, the window follows the picture.
- There is one registry for all programs, `SYS:QRXREG.TXT`. Setup and game now share it, and an old `QRXREG.TXT` in a program folder is merged once. Subkeys can be listed and deleted: `DMIX.DLL` finds its effects by listing `HKLM\SOFTWARE\Intel\RDX\Effects`. Without that the effect handle stayed 0, which was the fault in `DMIX.DLL`.
- `C:\WINDOWS` is one folder for all programs, `SYS:WINDOWS`, and DLLs are searched in `SYS:WINDOWS/SYSTEM`. `C:\` is still the program's folder. An absolute path that repeats the end of that folder is shortened, so `C:\SEGA\SONICCD\R1` works from `SEGA/SONICCD`.
- Programs linked before 1998 are told they run on Windows 95. The setup refuses NT.
- Dialogs load from the module the program names, not always from the EXE. Dialog controls keep `WS_VISIBLE` in their style; a program that read and wrote back the style hid its buttons.
- INI files are written for real. `WritePrivateProfileStringA` did nothing before, so settings were lost.
- `DSETUP.DLL` is built in and reports DirectX 9, and the registry holds the DirectX version. Without this Need for Speed II SE stops with "requires DirectX 5" once it is told it runs on Windows 95.
- `qw_alloc` zeroes in blocks instead of word by word. Sonic CD allocates 16 MB per frame in its levels, which took about 100 ms a call: 4 frames a second before, 30 now.

Still open with this game:

- Static texts in the setup are cut off at their last lines, because our font is wider than Windows' system font.
- Only the demos have been seen, through levels and a special stage. Playing by keyboard has not been tested, and sound has not been listened to.

## Update 2026-11-05, third part: child processes

`CreateProcessA` and `CreateProcessW`, `WinExec` and `ShellExecuteA` on a program now start a second qrx-win32 task with its own x86 memory and window. `Play` in the Sonic CD setup starts the game, and the setup then ends.

- The child gets its command line, drive C:, current directory and Windows path from a file in `T:`, because quotes do not survive the shell. Its exit code comes back in a second file, which `WaitForSingleObject`, `WaitForMultipleObjects`, `MsgWaitForMultipleObjects` and `GetExitCodeProcess` read.
- A child sees the same C: as the program that started it, and `GetModuleFileName` gives it its real path. A program started directly still sees `C:\NAME.EXE`.
- The door keeps thunk bindings and sound voices per page directory, and forgets all it knows about an address when a new program starts. Direct3D 8 and 9 vtables are written again into each new process. Before this, a second program in one session could inherit the first one's bindings.
- Only the process that ran x86 last is heard.

Checked with a small test pair: exit code, quotes in the command line, current directory, module path, a missing program, and two processes computing at once without mixing their results. The 68 programs show no regression. `mscasino.exe` now starts its `EBU0001.exe`, and Final Fantasy VII's `demo1.exe` starts `ff7config.exe`, which asks for a reinstall because a registry key is missing, where the program used to fault.

Limits:

- `TerminateProcess` on a child only marks it as ended; the task goes on.
- 16-bit and DOS programs are refused with error 193.
- Named mutexes, `FindWindow` and DDE do not reach across processes.
- Documents in `ShellExecuteA` report that no application is associated.
- A status file stays in `T:` until reboot if a child outlives the program that started it.

## Update 2026-11-05, fourth part: a heap overrun in waveOut, FreeLibrary

Sonic CD stopped after its first demo, about 100 seconds in, with a black window and an empty blue box. The box was AROS's "Software Failure" window, its dark text unreadable on our theme: "Sanity check on memory list failed" in `C:qrx-win32`.

- `waveOutWrite` copied each buffer in one piece through a host pointer into its ring. Guest pages do not lie next to each other in our memory, so the copy ran past the page and over the heap of qrx-win32. Any `waveOut` program could be hit, and the ring held wrong audio. It now copies through the page-safe accessors.
- `MapViewOfFile` had the same mistake in a `memset`, which is gone: `qw_alloc` has already zeroed the memory.
- `FreeLibrary` did nothing, so a DLL loaded again kept its used globals. A DLL from `LoadLibrary` now gets `DLL_PROCESS_DETACH` at its last reference and, when loaded again, its writable sections as they were and a new `DLL_PROCESS_ATTACH`. Its pages stay mapped, and DLLs that other modules import stay loaded. Sonic CD loads a DLL per stage.

How it was found, for next time: enhancing the blue box in a capture showed the text. Guard bytes around every `malloc` of `win32/*.c` then showed the overwritten blocks, and their contents were found in the game's `PCM8.CMP`.

Sonic CD now cycles through level, title, special stage and the next level, from the setup's `Play` too; eight captures over 160 seconds all show a picture. The 68 programs report the same as before.

Still open: AROS's Software Failure window has unreadable text on the QRX theme, so a crashed program looks like an empty box.

## Update 2026-11-05, fifth part: keyboard in dialogs, real message boxes

Dialogs now answer the keyboard, modal ones and through `IsDialogMessage` the others: Tab and Shift+Tab, arrows inside a group, Return for the focused or default button, Escape, the space bar and `&` letters. Modal dialogs now also get `WM_CHAR`, so their edit fields take typing. `GetNextDlgTabItem` and `GetNextDlgGroupItem` are real.

`MessageBox` showed only OK and always returned 1, whatever the program asked. It now has all six button sets, the default button and the right return value. A program that asked "Yes or No?" used to read the answer as IDOK.

Checked with a scripted test program: every button set, default button, Escape, typing, Tab, arrows, space and a modeless dialog. The 68 programs report the same as before, which says little here, since the survey presses no keys.

## Update 2026-11-05, sixth part: property sheets and wizards

`PropertySheetA` and `W`, `CreatePropertySheetPage` and the `PSM_` messages are real, with wizards and with plain sheets, modal and modeless. Pages are created when first shown and get the `PSN_` notifications. Tabs are a row of buttons, since there is no tab control.

`J2swc123.exe` (Jazz Jackrabbit 2) installs from start to end, about 40 MB.

This work uncovered general faults that were hiding behind the stub:

- Combo boxes ran on the list box code but did not know a single `CB_` message. They now do, and they show the selected entry; a click or an arrow takes the next one, as there is no open list.
- ANSI programs put list and combo entries in through a wide call, which garbled them. They are now converted, in both directions.
- `FindResourceA` passed ANSI names to the wide version, so named resources were never found. Tomb Raider's Lara and the picture on GEX's start screen depend on it.
- A text message could pass through a program's ANSI window procedure and be converted twice.
- Group boxes filled their whole area and painted over the controls inside them.
- `StretchDIBits` did nothing, and `SetDIBitsToDevice`, `SetDIBits`, `GetTextExtentExPointA`, `GetNumberFormatA` and `SHChangeNotify` were missing.
- A modal loop went on for ever after a fault in guest code, because nothing could end it. It now stops, and the fault is reported.

Visible in the survey: Rat Poker shows its window with its picture, and GEX shows its start screen. Both Tomb Raider II demos start with their configuration sheet on a first run, as the real program does, and the sheet works with mouse and keyboard.

Still open:

- There is no progress bar or animation control: `msctls_progress32` and `SysAnimate32` appear as empty static controls.
- `ff7demo.exe` waits for `ff7config.exe` on a first run; see the log of the child.
- Window captions on popup windows appear without text.

## Update 2026-11-06: Tomb Raider II in hardware 3D

Tomb Raider II now gets into its level with textures, in hardware 3D and in its software renderer, at 30 flips a second, and Lara answers the keyboard.

- `com_new` allocated 32 bytes, but a viewport keeps its rectangle at +32, so `SetViewport` overwrote the start of the next object. Here that was a surface, and the game faulted on its missing vtable. This could hit any Direct3D 3 to 7 program.
- `DX_MAX` went from 128 to 1024 objects: the game uses more texture surfaces than that.
- `IDirect3DTexture2::Load` did nothing, so textures stayed empty. It now copies the source surface.
- A surface created with a pixel format of its own kept the default 565 masks, which gave wrong colours for formats with alpha. It now uses the format it asked for.

Rogue Squadron no longer stops at its assertion and draws frames; how well it runs has not been looked at.

To check a hardware 3D picture, use `QRX_HG_CAPTURE=file QRX_HG_CAPTURE_FRAME=n`. `QRX_RTG_CAPTURE` shows guest video memory only, where such a program is simply black.

Still open: the level looks dark in both renderers, with or without the JIT (mean brightness 11 of 255 in the software renderer). Whether that is correct has not been settled.

## Update 2026-11-06, second part: Jazz Jackrabbit 2 plays

The shareware version goes from its own installer through intro, menu and episode choice into the first level at 60 frames a second, and the character runs both ways under the arrow keys.

- `FILD` and `FISTP` on 64 bits lost data, because the x87 here keeps doubles with 53 bits of mantissa. Pentium-era memory copies use exactly that pair, and every eighth pixel came out black, with dots elsewhere. A register still holding what `FILD` put there now writes those exact bits back. This is in the CPU core, so it covers every program with such a copy.
- A `WH_KEYBOARD` hook did not get the extended key bit in `lParam`. The game files arrows under `vk | 0x100`, so its menu worked through messages while the level saw no arrows. Doom95, which also reads through a hook, still answers its arrow keys.
- `SPI_GETNONCLIENTMETRICS` returned nothing and the game computed a window of negative size from the garbage. It now fills the structure, in both the ANSI and the wide form.
- `InsertMenuItem`, `WSAAsyncSelect`, `WSAAsyncGetHostByName`, `WSACancelAsyncRequest`, `GetClipRgn` and `OffsetClipRgn` are new.

Limits:

- `WSAAsyncSelect` accepts the call and delivers no events, so network play in such programs does not work yet.
- Sound in the game has not been listened to.

## Update 2026-11-06, third part: Microsoft's C runtime, exceptions, MFC

Programs that bring their own `MSVCRT.DLL` now run on it, and MFC programs show their dialogs. Max Payne reaches its start dialog, finds the host GPU and starts its Direct3D 8 driver. SimCity 3000 loads and runs into a null pointer in one of its DLLs. Neither is playable yet.

- A program with `MSVCRT.DLL` beside it uses that file. The built-in table covers only part of the runtime, and heap and exceptions cannot be split between the two.
- `RaiseException` and `RtlUnwind` are real for exceptions raised by software. Handlers run as guest code and return through a thunk, because a C++ catch never returns. As on Windows, a frame of the system's is on the chain while a handler runs: the C++ runtime takes the head of the chain, unwinds and links that head in again, and without the frame a frame ended up pointing at itself. Page faults still stop the program.
- `VirtualAlloc` ignored the address it was given, so each commit in reserved memory took new memory. SimCity 3000 spent 7.7 of every 10 seconds there.
- HostFS refused every access above 64 MB. Max Payne's archive is 98 MB. The limit is now 2 GB, the range of the guest's signed positions.
- Direct3D 8: `GetAdapterIdentifier` and `GetDeviceCaps` wrote the larger Direct3D 9 structures, which zeroed a return address in Max Payne's driver. The caps were all zero and now hold plausible values. `GetDepthStencilSurface`, `GetRenderTarget` and `GetBackBuffer` return objects without contents. `QRX_D3D_LOG=1` names the methods a game wants and the host lacks.
- The import table cut names at 95 characters, so long C++ names did not resolve. `LoadLibrary` also looks in the current directory, 64 modules can be loaded instead of 16, `WH_CBT` delivers `HCBT_CREATEWND`, and `CreateDialogIndirectParamA` and `DialogBoxIndirectParamA` are real instead of returning 0.
- Also new: a silent `IDirectSound3DBuffer` (the sound plays unpositioned), `LoadCursorFromFile`, `WriteProfileStringA`, `winmm` ordinal 2, `SHGetDesktopFolder` and `SHGetSpecialFolderLocation` as failing stubs, and resource names of the form `#1024`.

Survey of 66 programs, 40 seconds each: 14 stop, against 18 before this work, and no program that ran before now stops. The survey sees crashes and window sizes, not whether a game plays. `MSIFDEMO` now shows its settings dialog first, which the old stub skipped, and goes on to Direct3D after OK.

Limits:

- Max Payne ends with a message from its graphics engine after Play. It now asks for `GetSurfaceLevel`, the start of the texture path.
- SimCity 3000 faults at `78711d9b`, and `SetFilePointer` takes about 0.5 ms a call.
- `clawdins` and `gmdemins` unpack and then start a 16-bit InstallShield `SETUP.EXE`, which is not supported. The same message, `no PE header`, stops the setups of Worms 2, FF7, Fighter Tactics, Jam X and two more programs; whether each of those is 16-bit or DOS has not been checked.
- Static text in some MFC dialogs stays empty and a check box label is cut off in Monster Truck Madness 2.
- Under the JIT a jump to address 0 hangs where the interpreter reports the fault. Use `QRX86_NOJIT` when a program stops without a message.
- Diablo's shareware program stops at the message `exp1:C:\Diablo\Spawn`, not examined.

## Update 2026-11-07: Max Payne reaches its loading screen

Max Payne's demo now draws its loading screen through Direct3D 8 on the host, with correct textures and a moving bar. After 12 frames it stops presenting, so it is still not playable.

- New in the Direct3D 8 and 9 wrappers: surfaces for texture levels and free memory (`GetSurfaceLevel`, `GetDesc`, `LockRect`, `UnlockRect`, `GetLevelDesc`, `GetLevelCount`), and `CreateImageSurface`, `CopyRects` and `UpdateTexture`.
- Also new: `ValidateDevice`, `GetAvailableTextureMem`, `ResourceManagerDiscardBytes`, and `SetPixelShader` on Direct3D 8, which accepts the call and does nothing.
- `CheckDeviceFormat` no longer accepts everything: it answers yes only for formats `CreateTexture` can handle and for depth formats. DXT is therefore refused, and a game has to choose another format. `CheckDeviceMultiSampleType` accepts "no multisampling", which it refused before.
- `QRX_D3D_LOG=1` now also names Direct3D calls that return a failure.

What the stop looks like: after the last frame the game loads a few more textures, then runs a loop at about 780 passes a second that calls `PeekMessage`, the DirectInput devices, `QueryPerformanceCounter` and `GetFileAttributes("C:\\data")`, and calls Direct3D no more. The interpreter behaves the same, so the JIT is not the cause.

Ruled out: key presses, and making `SetForegroundWindow` and `SetActiveWindow` real, which changed nothing and was taken out again. All calls in the loop return success. The `-developer` switch creates `verbose_<computer>.txt`, but it stayed empty in these runs. The cause is not known.

## Update 2026-11-07: the JIT translates FPU instructions

The 68k JIT now runs the common 68040 FPU instructions natively, so floating point work no longer drops to about 50 MIPS. Measured with Blender 2.79b as a guest (`-t 1`, no core 1), engines alternating, with other work on the Mac at a load of 7 to 9:

| Scene | Before | After | Factor |
|---|---|---|---|
| Raytracing 200x200, render step | 47.3 / 48.1 / 50.5 s | 3.3 / 3.5 / 3.9 s | about 14 |
| Procedural textures 640x640 | 39.7 / 40.2 / 41.6 s | 4.0 / 4.0 / 6.0 s | about 10 |
| Default cube 960x540 | 28.3 / 28.7 / 29.8 s | 1.8 / 1.8 / 1.9 s | about 16 |
| All three, boot included, host clock | 124 / 125 / 131 s | 18 / 18 / 20 s | about 7 |

During a raytrace at 640x640 the engine went from 55 to 577 MIPS over the same 40 seconds. That job now finishes its render step in 34 s, matches the Mac reference to the pixel, and was at tile 20 of 324 with the old engine after the same time. No guest had finished this scene before. Interpreted instructions fell from 27% to 0.16% of the total. The native Mac build takes about 0.3 s on the 200x200 raytrace, so a guest is now roughly 11 times slower than native on this work, not 100 to 170 times.

How it works:

- Between blocks the guest registers stay 80-bit values in `REG_FP`. Inside a block, the ones in use live in `d8`-`d15` as IEEE single or double, and the translator knows which. C helpers preserve those registers, so a slow memory access does not disturb them.
- Translated: `FMOVE`, `FABS`, `FNEG`, `FTST`, `FCMP`, the `FS` and `FD` forms of those plus `FADD`, `FSUB`, `FMUL`, `FDIV` and `FSQRT`, `FBcc`, `FScc`, `FMOVEM` of data registers, `FMOVE.X`, and `FMOVE` between a data register and `FPCR` or `FPSR`. Sources may be registers, memory or immediates in `.S`, `.D`, `.L`, `.W` and `.B`.
- Native code only handles normal numbers and zero, with round-to-nearest (`FMOVE` to an integer also handles round-to-zero, because C casts ask for it). An operand or result that is a denormal, infinity or NaN, an operand with bits the format cannot hold, or another rounding mode leaves through a bail exit. The exit writes changed registers back, and Musashi runs that one instruction with nothing changed.
- `FADD`, `FSUB`, `FMUL`, `FDIV` and `FSQRT` without `FS` or `FD` need a 64-bit mantissa, which the host FPU does not have. They stay with Musashi and softfloat, so plain extended-precision programs do not get this speedup.
- `FPSR` condition codes share the flag liveness pass: they are only computed when something can read them.

**Musashi's results changed on purpose.** The pinned code rounded `FSxxx` and `FDxxx` twice, first to 64 bits and then to the target, and took the condition codes from before the second rounding. A 68040 and AArch64 round once, so JIT and interpreter would have disagreed. `tools/patch-musashi-fsave-an.py` now rounds once and takes the codes from the rounded result. Blender's images are identical to those of the old engine: a 640x640 scene still has the same 1 of 409,600 pixels off by 1/255 against the Mac reference, the others have none.

`jit_diff_tests` now compares FPU registers, `FPSR`, `FPCR` and the just-reset state. It covers the new instructions with special values, full 53-bit mantissas, all rounding modes and hand-overs to Musashi, and it fails if the common forms stop running natively. All 15 faults built in on purpose were caught, as was bringing back the old double rounding. Core 1 gets the same code through `gen-core1-rename.py`; it rendered tiles with identical pictures at a factor of about 1.7 on the raytrace.

Musashi faults this work found and did not fix, because they need their own decision:

- `FMOVE.W` and `FMOVE.B` to a data register replace all 32 bits; a 68040 keeps the upper ones. The JIT leaves those two forms to Musashi, so behaviour is unchanged.
- `floatx80_sqrt` never returns on an unnormal number (exponent set, integer bit clear). Only a program storing such a value by hand can reach it.
- `FINT` and `FINTRZ` go through a 32-bit integer, so large values saturate and -0.5 gives +0.

## Update 2026-11-07: two FPU faults in the Musashi core

Found while porting Blender ([docs/BLENDER_AROS68K.md](../BLENDER_AROS68K.md)), fixed in `tools/patch-musashi-fsave-an.py`:
`FSSQRT` and `FDSQRT` ran as `FINT`, and `FMOVEM` of several FPU control registers with `(An)` wrote them all to one
address, so AROS lost FPCR and the condition codes on every task switch. Repeated renders are now identical to the
pixel. `QRX_FULL_DUMP_RAM` now takes ranges of up to 1 GiB.
