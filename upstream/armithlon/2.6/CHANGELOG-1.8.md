# ARMITHLON 1.8 – Real guest 68k firmware boot

- NEW: `qrx68100_boot_shell` 68k guest machine-code boot firmware at 0x1000;
  reset vectors, real 68k UART RX and 68k MMIO firmware services.
- NEW: read-only boot RAM builder and CLI ROM image export (256 KiB).
- NEW: QRXFS-1 fixed-slot persistent file image with CRC32 validation,
  bounded file names, confined storage and atomic saves. Save survives restarts.
- NEW: terminal shell (help, ls/cat/write/append/rm, cpu, mem, date, stats,
  reboot, shutdown) and guest QEX1 executable loading.
- NEW: HELLO.QEX and BENCH.QEX, genuinely executed by the existing 68k VM.
- NEW: CTest verifying CPU-executed boot, QEX1, persistence, restart, corruption
  rejection, command boundaries and memory-mapped UART.
- NEW: optional native Cocoa QRX firmware GUI with booted guest terminal,
  command input, quick actions and a locally buildable Finder .app (M1 untested).
- NEW: Start-QRX.command with firmware/FS-UAE choices; `./armithlon boot`,
  `boot-test`, `boot-rom`, `boot-full` (Musashi optional), `desktop` (FS-UAE).
- Existing QRX86 Windows/Linux benchmark and GPU tools retained unchanged.

Limits: Standalone firmware shell ≠ AmigaOS/AROS desktop boot. Existing AROS
GUI starts through FS-UAE compatibility backend only; no newly proven native
68040 Musashi or Apple M1 graphics runs. Dual core is not a general SMP OS.
- VERIFIED: portable Linux 26/26 CTest, sanitizer 26/26, independent-process
  persistent disk and scripted guest execution. Cocoa implementation untested.
