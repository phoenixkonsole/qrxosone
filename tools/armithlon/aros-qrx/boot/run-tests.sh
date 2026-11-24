#!/bin/bash
# Boots the 64-bit AROS for the 68100EC64 on the reference machine and checks the console.
# Usage: run-tests.sh [--gecko] <AROS build dir, e.g. build-qrx/bin/native-m68k64> [work dir]
# --gecko runs only the native browser runtime probes. CLANGXX can select the C++ wrapper.
# Use QRXBOOT to point to the qrxboot executable from the engine build.
#   - the kernel selftest on 1, 2, 4 and 8 cores: tasks, the timer, IPIs, faults to trap handlers
#   - ColdReboot(): the run ends with a reboot, the loader starts the system again
#   - a fault without a trap handler: exec's alert, then a reboot
#   - the DOS boot through dosboot and the emergency console to the AROS shell, with input
#   - the boot from an FFS disk (qrxdisk.device, afs.handler) and a C program built with the m68k64
#     clang against the AROS C library, loaded from C: with its libraries from LIBS:
# The disk test needs the clang wrapper for m68k64-unknown-aros as $CLANG (default: clang in PATH)
# and libclang_rt.builtins-m68k64.a in the SDK's lib directory.
set -u
gecko=0
if [[ ${1:-} == --gecko ]]; then
    gecko=1
    shift
fi
if (( $# < 1 || $# > 2 )); then
    echo "usage: $0 [--gecko] AROS-BUILD-DIR [WORK-DIR]" >&2
    exit 2
fi
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$(cd "$1" && pwd)" || exit 2
W="${2:-$(mktemp -d)}"
mkdir -p "$W" || exit 2
E="$HERE/../../../../upstream/armithlon/2.6/engine"
QRXBOOT="${QRXBOOT:-$E/build/qrxboot}"
if [[ ! -x $QRXBOOT && $QRXBOOT == "$E/build/qrxboot" ]]; then
    QRXBOOT=qrxboot
fi
if ! command -v "$QRXBOOT" >/dev/null; then
    echo "qrxboot not found. Build the engine's qrxboot target and set QRXBOOT to its path." >&2
    exit 2
fi
A="$B/AROS"; CORE="$B/gen/rom/boot/core.elf"
if [[ ! -f $CORE || ! -d $A/Development ]]; then
    echo "expected core.elf and an AROS SDK under $B (see $HERE/../README.md)" >&2
    exit 2
fi
fail=0
failed() {
    echo "FAIL $*" >&2
    fail=$((fail + 1))
}
check() {   # name, log, pattern...
    local name="$1" log="$2"; shift 2
    for p in "$@"; do
        if ! grep -qF -- "$p" "$log"; then failed "$name: no '$p' (see $log)"; return; fi
    done
    echo "ok   $name"
}
if (( ! gecko )); then
    for c in 1 2 4 8; do
        "$QRXBOOT" "$CORE" 60 $c 512 "$A/Libs/utility.library" > "$W/selftest$c.txt" 2>&1
        check "selftest, $c cores" "$W/selftest$c.txt" "[selftest] PASS" "value 0"
    done
    QRXBOOT_CMDLINE="selftest reboottest" "$QRXBOOT" "$CORE" 60 2 512 "$A/Libs/utility.library" > "$W/reboot.txt" 2>&1
    check "ColdReboot" "$W/reboot.txt" "=== warm reboot, starting again" "[selftest] PASS"
    QRXBOOT_CMDLINE="selftest crashtest" "$QRXBOOT" "$CORE" 60 2 512 "$A/Libs/utility.library" > "$W/crash.txt" 2>&1
    check "fault to exec's alert" "$W/crash.txt" "Software Failure!" "Hardware bus fault" "reboot, starting again" "[selftest] PASS"
    QRXBOOT_CMDLINE="econsole" \
    QRXBOOT_INPUT='echo "Hallo von QRX OS One"\nset x 42\necho "x ist $x"\nresident\ncd\n' \
        "$QRXBOOT" "$CORE" 25 2 512 "$A/Libs/utility.library" "$A/boot/native/Devs/timer.device" \
        "$A/boot/native/Libs/expansion.library" "$A/Devs/bootloader.resource" "$A/Libs/dos.library" \
        "$A/L/econsole-handler" "$A/Devs/shell.resource" "$A/Devs/shellcommands.resource" \
        "$A/Devs/dosboot.resource" > "$W/shell.txt" 2>&1
    check "DOS boot to the shell" "$W/shell.txt" "The AROS Development Team" "1> " "Hallo von QRX OS One" \
        "x ist 42" "Echo                           INTERNAL" "SYS:"
fi
SYS="$(mktemp -d "$W/sys.XXXXXX")" || exit 2
mkdir -p "$SYS/C" "$SYS/Libs" "$SYS/S" "$SYS/T" || exit 2
echo "m68k64" > "$SYS/AROS.boot" || exit 2
cp "$A/Libs/crt.library" "$A/Libs/stdlib.library" "$A/Libs/m.library" "$A/Libs/locale.library" \
   "$A/Libs/iffparse.library" "$A/Libs/uuid.library" "$SYS/Libs/" || exit 2
cp "$A"/C/* "$SYS/C/" || exit 2
printf 'SetClock LOAD\nEcho "Startup-Sequence done"\n' > "$SYS/S/Startup-Sequence" || exit 2
CLANG="${CLANG:-clang}"
# Prefer collect-aros from the same AROS build, with PATH as the fallback.
for tools_dir in "$(dirname "$B")"/*/tools; do
    if [[ -x $tools_dir/collect-aros ]]; then
        PATH="$tools_dir:$PATH"
        break
    fi
done
export PATH
boot_disk() {
    QRXBOOT_DISK="$1" QRXBOOT_CMDLINE="econsole-startup" QRXBOOT_INPUT="$2" \
        "$QRXBOOT" "$CORE" 60 2 512 "$A/Libs/utility.library" "$A/Libs/aros.library" "$A/boot/native/Devs/battclock.resource" \
        "$A/boot/native/Devs/timer.device" "$A/boot/native/Libs/expansion.library" "$A/Devs/bootloader.resource" \
        "$A/Libs/dos.library" "$A/Devs/lddemon.resource" "$A/L/econsole-handler" "$A/Devs/FileSystem.resource" \
        "$A/L/afs-handler" "$A/Devs/qrxdisk.device" "$A/Devs/shell.resource" "$A/Devs/shellcommands.resource" \
        "$A/Devs/dosboot.resource"
}

run_gecko() {
    local name="$1" compiler="$2" source="$3" standard="$4" marker="$5"
    local build_log="$W/$name-build.txt" guest_log="$W/$name-guest.txt"
    local object="$W/$name.o" image="$W/$name.img"
    local flags=(--target=m68k64-unknown-aros --sysroot "$A/Development" "-std=$standard" -O2 -pthread)

    if ! "$compiler" "${flags[@]}" -c "$HERE/tests/$source" -o "$object" > "$build_log" 2>&1; then
        failed "$name: compile (see $build_log)"
        tail -n 12 "$build_log" >&2
        return
    fi
    if ! "$compiler" "${flags[@]}" "$object" -o "$SYS/C/$name" >> "$build_log" 2>&1; then
        failed "$name: link (see $build_log)"
        tail -n 12 "$build_log" >&2
        return
    fi
    if ! python3 -I "$HERE/mkffsimg.py" "$SYS" "$image" 32 QRXSYS >> "$build_log" 2>&1; then
        failed "$name: build guest image (see $build_log)"
        return
    fi

    if boot_disk "$image" "C:$name\nshutdown\n" > "$guest_log" 2>&1; then
        check "$name, two EC64 cores" "$guest_log" "$marker" "=== power off"
    else
        failed "$name: guest failed to power off cleanly (see $guest_log)"
        tail -n 12 "$guest_log" >&2
    fi
    rm -f "$SYS/C/$name"
}

if (( gecko )); then
    CLANGXX="${CLANGXX:-${CLANG}++}"
    export QRX_EC64_SMP_SPREAD=0
    run_gecko GeckoPThread "$CLANG" gecko-pthread.c c11 "PASS: pthread create/join, key isolation and destructors"
    run_gecko GeckoRuntime "$CLANGXX" gecko-runtime.cpp c++17 "PASS: C++ exceptions, guest threads and thread-local storage"
elif "$CLANG" --sysroot "$A/Development" -O2 -o "$SYS/C/hello" "$HERE/tests/hello.c" > "$W/hello-build.txt" 2>&1 &&
   python3 -I "$HERE/mkffsimg.py" "$SYS" "$W/disk.img" 32 QRXSYS; then
    boot_disk "$W/disk.img" 'cd\nhello eins "zwei drei"\ndir\ninfo\ndate\nshutdown\n' > "$W/disk.txt" 2>&1
    check "boot from disk, C program" "$W/disk.txt" "QRXSYS:" "sizeof(long) 8, sizeof(void *) 8" \
        "argc 3: [hello] [eins] [zwei drei]" "sorted: -7 0 3 42 1000000000000" "double: 13.000, 4.33333, int 130" \
        "last byte 5a" "read back: written by hello, 1000000000000"
    check "C: commands, Startup-Sequence, clock, shutdown" "$W/disk.txt" "Startup-Sequence done" "AROS.boot" \
        "read/write FFS     QRXSYS" "-$(date -u +%y) " "=== power off"
else
    failed "boot from disk: could not build hello or the image (see $W/hello-build.txt)"
fi
echo "$fail failures"
exit $((fail != 0))
