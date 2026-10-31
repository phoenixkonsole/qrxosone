# ARMithlon / embedded X11 integration evidence

Linux x86_64 cloud session; base Claude revision fb1d03e. The archive's imported
reports are kept separately under upstream/armithlon and are not these results.

- engine-ctest.log: actual portable ARMithlon build, 17/17 tests passed.
- win32-clock.log: existing scripted PE32 clock through the integrated host
  HLE. All three expected PPM files existed and the process returned zero.
- x11-host.log: actual i386 client, full pixel comparison and ASan/UBSan wire tests.
- AROS client logs (named aros-client.*): finite native AROS test, require rc=0.
- aros-x11-window.png: separate capture run with QRX_X11_HOLD, native AROS
  public-screen window and green rectangle. QRXScreenGrab reports 640x480,
  depth 8. The capture harness terminates the held process after capture;
  it is not counted as the finite client's exit-status test.
- jit-safety.log / jit-native.log: unchanged Claude-tip QRX86 safety and native
  differential tests; all four QEMU CPU profiles passed. These benchmark ticks
  are synthetic correctness-run output, not a new application performance claim.
- validate-22.5.12.log: source gates pass; libc-dependent -m32 differentials
  explicitly SKIPPED (multilib libc unavailable). The freestanding new i386
  GUI test does execute; it does not depend on that libc.

AROS setup: tools/qrx86/bootstrap-aros-sdk.py; cross flags -O2 -m68060,
-fno-pic -fno-PIC -fno-optimize-sibling-calls; link qmath, -lm, -lcybergraphics.
Use x86-runtime-22.5.12/build-target.sh with that prepared SDK. Compile
x11-guest.c using cc -m32 -nostdlib -static -fno-pie -no-pie
-fno-stack-protector -O1. For the screenshot only add -DQRX_X11_HOLD.

Copy qrx-i386 HUNK and x11-guest ELF to scenario/overlay/C. For a finite test,
User-Startup contains:

```
FailAt 1000
C:qrx-i386 -b 10000000 SYS:C/x11-guest >QRXOUT:client.log
Echo "client rc=$RC" >QRXOUT:client.rc
Echo "done" >QRXOUT:DONE
```

Run the existing amiga-runtime-22.5.11/emulator/run-fsuae-scenario.sh with the
verified extracted AROS ISO, original ROMs, that scenario, a fresh output path,
CPU 68060, timeout 180. Stock FS-UAE 3.1.66 host JIT remains off; warp is off.
Require STATUS=COMPLETED, client rc=0 and the client's PASS line.
No M1, Metal, physical Amiga or full GUI toolkit test was performed.
