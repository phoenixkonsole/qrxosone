#!/bin/bash
# Boots the 64-bit AROS for the 68100EC64 on the reference machine and checks the console.
# Usage: run-tests.sh <AROS build dir, e.g. build-qrx/bin/native-m68k64> [work dir]
#   - the kernel selftest on 1, 2, 4 and 8 cores: tasks, the timer, IPIs, faults to trap handlers
#   - ColdReboot(): the run ends with a reboot, the loader starts the system again
#   - a fault without a trap handler: exec's alert, then a reboot
#   - the DOS boot through dosboot and the emergency console to the AROS shell, with input
#   - the boot from an FFS disk (qrxdisk.device, afs.handler) and a C program built with the m68k64
#     clang against the AROS C library, loaded from C: with its libraries from LIBS:
# The disk test needs the clang wrapper for m68k64-unknown-aros as $CLANG (default: clang in PATH)
# and libclang_rt.builtins-m68k64.a in the SDK's lib directory.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$(cd "$1" && pwd)"; W="${2:-$(mktemp -d)}"; mkdir -p "$W"
E="$HERE/../../../../upstream/armithlon/2.6/engine"
cc -std=gnu11 -O2 -I"$E/include" "$HERE/qrxboot.c" "$E/full68k/qrx68100ec64.c" \
   "$E/full68k/qrx68100ec64_machine.c" "$E/full68k/qrx_ec64_board.c" -lpthread -o "$W/qrxboot" || exit 2
A="$B/AROS"; CORE="$B/gen/rom/boot/core.elf"
fail=0
check() {   # name, log, pattern...
    local name="$1" log="$2"; shift 2
    for p in "$@"; do
        if ! grep -qF -- "$p" "$log"; then echo "FAIL $name: no '$p' (see $log)"; fail=$((fail + 1)); return; fi
    done
    echo "ok   $name"
}
for c in 1 2 4 8; do
    "$W/qrxboot" "$CORE" 60 $c 512 "$A/Libs/utility.library" > "$W/selftest$c.txt" 2>&1
    check "selftest, $c cores" "$W/selftest$c.txt" "[selftest] PASS" "value 0"
done
QRXBOOT_CMDLINE="selftest reboottest" "$W/qrxboot" "$CORE" 60 2 512 "$A/Libs/utility.library" > "$W/reboot.txt" 2>&1
check "ColdReboot" "$W/reboot.txt" "=== warm reboot, starting again" "[selftest] PASS"
QRXBOOT_CMDLINE="selftest crashtest" "$W/qrxboot" "$CORE" 60 2 512 "$A/Libs/utility.library" > "$W/crash.txt" 2>&1
check "fault to exec's alert" "$W/crash.txt" "Software Failure!" "Hardware bus fault" "reboot, starting again" "[selftest] PASS"
QRXBOOT_CMDLINE="econsole" \
QRXBOOT_INPUT='echo "Hallo von QRX OS One"\nset x 42\necho "x ist $x"\nresident\ncd\n' \
    "$W/qrxboot" "$CORE" 25 2 512 "$A/Libs/utility.library" "$A/boot/native/Devs/timer.device" \
    "$A/boot/native/Libs/expansion.library" "$A/Devs/bootloader.resource" "$A/Libs/dos.library" \
    "$A/L/econsole-handler" "$A/Devs/shell.resource" "$A/Devs/shellcommands.resource" \
    "$A/Devs/dosboot.resource" > "$W/shell.txt" 2>&1
check "DOS boot to the shell" "$W/shell.txt" "The AROS Development Team" "1> " "Hallo von QRX OS One" \
    "x ist 42" "Echo                           INTERNAL" "SYS:"
rm -rf "$W/sys"; mkdir -p "$W/sys/C" "$W/sys/Libs" "$W/sys/S" "$W/sys/T"
echo "m68k64" > "$W/sys/AROS.boot"
cp "$A/Libs/crt.library" "$A/Libs/stdlib.library" "$A/Libs/m.library" "$A/Libs/locale.library" \
   "$A/Libs/iffparse.library" "$A/Libs/uuid.library" "$W/sys/Libs/"
cp "$A"/C/* "$W/sys/C/"
printf 'SetClock LOAD\nEcho "Startup-Sequence done"\n' > "$W/sys/S/Startup-Sequence"
CLANG="${CLANG:-clang}"
TOOLS=$(ls -d "$(dirname "$B")"/*/tools 2>/dev/null | grep -v /native- | head -1)     # collect-aros of the AROS build
if PATH="$TOOLS:$(dirname "$(command -v "$CLANG")"):$PATH" "$CLANG" --sysroot "$A/Development" -O2 -o "$W/sys/C/hello" "$HERE/tests/hello.c" > "$W/hello-build.txt" 2>&1 &&
   python3 -I "$HERE/mkffsimg.py" "$W/sys" "$W/disk.img" 32 QRXSYS; then
    QRXBOOT_DISK="$W/disk.img" QRXBOOT_CMDLINE="econsole-startup" \
    QRXBOOT_INPUT='cd\nhello eins "zwei drei"\ndir\ninfo\ndate\nshutdown\n' \
        "$W/qrxboot" "$CORE" 60 2 512 "$A/Libs/utility.library" "$A/Libs/aros.library" "$A/boot/native/Devs/battclock.resource" \
        "$A/boot/native/Devs/timer.device" "$A/boot/native/Libs/expansion.library" "$A/Devs/bootloader.resource" \
        "$A/Libs/dos.library" "$A/Devs/lddemon.resource" "$A/L/econsole-handler" "$A/Devs/FileSystem.resource" \
        "$A/L/afs-handler" "$A/Devs/qrxdisk.device" "$A/Devs/shell.resource" "$A/Devs/shellcommands.resource" \
        "$A/Devs/dosboot.resource" > "$W/disk.txt" 2>&1
    check "boot from disk, C program" "$W/disk.txt" "QRXSYS:" "sizeof(long) 8, sizeof(void *) 8" \
        "argc 3: [hello] [eins] [zwei drei]" "sorted: -7 0 3 42 1000000000000" "double: 13.000, 4.33333, int 130" \
        "last byte 5a" "read back: written by hello, 1000000000000"
    check "C: commands, Startup-Sequence, clock, shutdown" "$W/disk.txt" "Startup-Sequence done" "AROS.boot" \
        "read/write FFS     QRXSYS" "-$(date -u +%y) " "=== power off"
else
    echo "FAIL boot from disk: could not build hello or the image (see $W/hello-build.txt)"; fail=$((fail + 1))
fi
echo "$fail failures"
exit $((fail != 0))
