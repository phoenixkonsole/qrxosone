#!/usr/bin/env bash
# Put QRX OS One 64 (64-bit AROS on the 68100EC64) into the ARMithlon bundle, next to the 32-bit
# system. Nothing of the 32-bit system changes.
#   build-qrxos64.sh <AROS build bin dir> <qrx_ec64_run> <qrx-win32 for m68k64> [bundle] [image MB]
# The AROS build bin dir holds native-m68k64 (see tools/armithlon/aros-qrx/README.md).
# Result in the bundle:
#   ARMithlon.app/Contents/MacOS/qrx64-engine   the EC64 machine (qrx_ec64_run)
#   ARMithlon.app/Contents/MacOS/launch64       starts it in a Mac window
#   ARMithlon.app/Contents/Resources/qrx64/     kernel and kickstart modules, modules.txt
#   System64/                                   the system tree, QRXOS64.img made from it
set -Eeuo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:?usage: build-qrxos64.sh AROS-BIN EC64-ENGINE QRX-WIN32 [BUNDLE] [MB]}"
ENGINE="${2:?qrx_ec64_run required}"
QW="${3:?qrx-win32 for m68k64 required (build-ec64.sh)}"
BU="${4:-$REPO/.build/bundle}"
MB="${5:-128}"
A="$BIN/native-m68k64/AROS"
APP="$BU/ARMithlon.app/Contents"
K="$APP/Resources/qrx64"
mkdir -p "$K"
cp "$BIN/native-m68k64/gen/rom/boot/core.elf" "$K/"
: > "$K/modules.txt"
for m in Libs/utility.library Libs/aros.library boot/native/Devs/battclock.resource \
         boot/native/Devs/timer.device boot/native/Libs/expansion.library Devs/bootloader.resource \
         Libs/dos.library Devs/lddemon.resource L/econsole-handler Devs/FileSystem.resource \
         L/afs-handler Devs/qrxdisk.device Devs/shell.resource Devs/shellcommands.resource \
         Devs/dosboot.resource; do
    cp "$A/$m" "$K/"
    basename "$m" >> "$K/modules.txt"
done
cp "$ENGINE" "$APP/MacOS/qrx64-engine"

T="$BU/System64"
rm -rf "$T"
mkdir -p "$T/C" "$T/Libs" "$T/S" "$T/X86"
echo m68k64 > "$T/AROS.boot"
cp "$A"/C/* "$T/C/"
for l in crt stdlib m locale iffparse uuid; do cp "$A/Libs/$l.library" "$T/Libs/"; done
cp "$QW" "$T/C/qrx-win32"
cat > "$T/S/Startup-Sequence" <<'EOF'
SetClock LOAD
Echo "QRX OS One 64 - 64-bit AROS on the 68100EC64"
Echo "Windows programs (headless for now): qrx-win32 -s SCRIPT PROGRAM.EXE"
Echo "Power off: Shutdown"
EOF
python3 -I "$REPO/tools/armithlon/aros-qrx/boot/mkffsimg.py" "$T" "$BU/QRXOS64.img" "$MB" QRXOS64

cat > "$APP/MacOS/launch64" <<'EOF'
#!/bin/sh
# QRX OS One 64: 64-bit AROS on the dual 68100EC64, in a Mac window. The system disk is
# QRXOS64.img next to the app (made from System64 with mkffsimg.py); changes are written back.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
K="$HERE/../Resources/qrx64"
ROOT="$HERE/../../.."
set --
while read -r m; do set -- "$@" "$K/$m"; done < "$K/modules.txt"
exec "$HERE/qrx64-engine" --window --cores "${QRX64_CORES:-2}" --ram "${QRX64_RAM:-2048}" \
    --disk "$ROOT/QRXOS64.img" --disk-write --cmdline econsole-startup \
    "$K/core.elf" "$@" >>"$ROOT/qrxos64.log" 2>&1
EOF
chmod +x "$APP/MacOS/launch64"
echo "QRX OS One 64 in $BU: start with ARMithlon.app/Contents/MacOS/launch64"
