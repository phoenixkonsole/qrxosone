QRX OS One 0.20.3.7.4.6.6.22.5.12 - Linux i386 + Win32 programs and Amiga applications
=====================================================================================

Cumulative source release on top of 22.5.11 (nothing removed; the directory name
keeps "22.5.11" so every existing path, gate and validator stays valid).

  CHANGELOG-22.5.12.txt         what changed and why
  TESTREPORT-22.5.12.txt        host results vs. FS-UAE target results, open points
  INSTALL-22.5.12.txt           build, deploy, host tests, emulator tests
  SHA-GATE-REPORT-22.5.12.txt   documented SHA changes
  SHA256SUMS-22.5.12            checksums of every file in this tree

New components
  x86-runtime-22.5.12/          qrx86core (IA-32 interpreter + x87), qrx-i386
                                (Linux i386), qrx-win32 (Win32 HLE), qmath,
                                QRXClock, tests  -> X86-RUNTIME-22.5.12.txt
  apps-22.5.12/                 FS-UAE scenarios: x86 tests, live Win32 windows,
                                Directory Opus 5, HippoPlayer, AmigaGPT
  test-evidence-22.5.12/        target logs and screenshots
  validate-22.5.12.py           source/integration gates (run by deploy.sh)

Programs
  Windows (ReactOS 0.4.15, unmodified): calc.exe (Rechner), winmine.exe
  (Minesweeper), sol.exe (Solitaire) and spider.exe with cards.dll.  ReactOS has
  no stand-alone clock -> QRXClock (own MinGW PE).
  DirectX 3 (DirectDraw/DirectSound/DirectInput) HLE, verified with the own test
  program dxtest.exe; exclusive 8-bit modes get an own AROS screen.
  Linux i386: BusyBox 1.36.1 (Debian/Ubuntu busybox-static), static glibc programs.
  Amiga 68k: Directory Opus 5.104 (open source), HippoPlayer 2.67 (open source);
  AmigaGPT tested but does not start on AROS (needs MUI classes + AmiSSL).

Honest limits: everything x86 is interpreted (no JIT; the old QRX86 JIT stays
quarantined), about 236,000 x86 instructions/s in FS-UAE (a DirectDraw frame takes
seconds); x87 is IEEE double; no threads/sockets/signal delivery/audio output;
only emulator tests (FS-UAE), no real hardware.  No commercial game demo could
be tested: the build environment cannot reach the download sites.  Aminet was not reachable from the
build environment, the Amiga programs come from their GitHub releases.
