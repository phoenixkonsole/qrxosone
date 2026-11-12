#!/bin/bash
# Boots the 64-bit AROS for the 68100EC64 on the reference machine and checks the console.
# Usage: run-tests.sh <AROS build dir, e.g. build-qrx/bin/native-m68k64> [work dir]
#   - the kernel selftest on 1, 2, 4 and 8 cores: tasks, the timer, IPIs, faults to trap handlers
#   - ColdReboot(): the run ends with a reboot, the loader starts the system again
#   - a fault without a trap handler: exec's alert, then a reboot
#   - the DOS boot through dosboot and the emergency console to the AROS shell, with input
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$(cd "$1" && pwd)"; W="${2:-$(mktemp -d)}"; mkdir -p "$W"
E="$HERE/../../../../upstream/armithlon/2.6/engine"
cc -std=gnu11 -O2 -I"$E/include" "$HERE/qrxboot.c" "$E/full68k/qrx68100ec64.c" \
   "$E/full68k/qrx68100ec64_machine.c" -lpthread -o "$W/qrxboot" || exit 2
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
echo "$fail failures"
exit $((fail != 0))
