#!/usr/bin/env bash
# Put QRX OS One 64 (64-bit AROS on the 68100EC64) into the ARMithlon bundle, next to the 32-bit
# system. Nothing of the 32-bit system changes.
#   build-qrxos64.sh <AROS build bin dir> <qrx_ec64_run> <qrx-win32 for m68k64> [bundle] [image MB]
# The AROS build bin dir holds native-m68k64 (see tools/armithlon/aros-qrx/README.md).
# QRX_CC64, if set, is the m68k64 clang (cc64): the App Store is built with it.
# QRX64_EXTRA_C, if set, is a directory of more m68k64 programs for C: (QRXDesktop, QRXHostSocket,
# EC64Input, EC64GLDemo, and the Mac tools from tools/qrxppc/build64.sh).
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
mkdir -p "$K" "$APP/MacOS"
cp "$BIN/native-m68k64/gen/rom/boot/core.elf" "$K/"
: > "$K/modules.txt"
for m in Libs/utility.library Libs/aros.library boot/native/Devs/battclock.resource \
         boot/native/Devs/timer.device boot/native/Libs/expansion.library Devs/bootloader.resource \
         Libs/dos.library Devs/lddemon.resource L/econsole-handler Devs/FileSystem.resource \
         L/afs-handler L/qrxsys-handler Devs/qrxdisk.device Devs/shell.resource Devs/shellcommands.resource \
         Devs/dosboot.resource; do
    # qrxsys: the file system on a host directory (aros-qrx/qrxhostfs, host service 12); with it the
    # system boots from System64 itself (--hostfs) instead of QRXOS64.img
    [ "$m" = L/qrxsys-handler ] && [ ! -f "$A/$m" ] && { echo "not built, left out: $m"; continue; }
    cp "$A/$m" "$K/"
    basename "$m" >> "$K/modules.txt"
done
# Graphics, when built: the board's framebuffer driver and what Intuition needs, from the kickstart
for m in Libs/oop.library Devs/Drivers/hiddclass.hidd Devs/Drivers/gfx.hidd Libs/graphics.library \
         Libs/layers.library Libs/keymap.library Devs/Drivers/inputclass.hidd Devs/Drivers/keyboard.hidd \
         Devs/Drivers/mouse.hidd Devs/input.device Devs/keyboard.device Devs/gameport.device \
         Libs/intuition.library Devs/console.device L/con-handler Libs/cybergraphics.library \
         Devs/Drivers/ec64gfx.hidd; do
    [ -f "$A/$m" ] || { echo "not built, left out: $m"; continue; }
    cp "$A/$m" "$K/"
    basename "$m" >> "$K/modules.txt"
done
cp "$ENGINE" "$APP/MacOS/qrx64-engine"
# Tor and WireGuard for the guests (tools/armithlon/qrxnetd, cargo build --release); the engine
# starts it from its own directory. Without it the Tor and VPN modes have no network at all.
NETD="${QRX_NETD_BIN:-$REPO/tools/armithlon/qrxnetd/target/release/qrxnetd}"
if [ -x "$NETD" ]; then cp "$NETD" "$APP/MacOS/qrxnetd"; else echo "qrxnetd not built: Tor and VPN modes stay offline"; fi

T="$BU/System64"
rm -rf "$T"
mkdir -p "$T/C" "$T/Libs" "$T/S" "$T/X86" "$T/Apps" "$T/Home" "$T/T"
echo m68k64 > "$T/AROS.boot"
cp "$A"/C/* "$T/C/"
for l in crt stdlib m locale iffparse uuid gadtools workbench diskfont asl z1; do
    [ -f "$A/Libs/$l.library" ] && cp "$A/Libs/$l.library" "$T/Libs/"
done
cp "$QW" "$T/C/qrx-win32"
# PIPE: (pipe(), std::process of Rust, shell redirections): qrxpipe-handler from build-linux.sh
if [ -f "$A/L/qrxpipe-handler" ]; then
    mkdir -p "$T/L" "$T/Devs/DOSDrivers"
    cp "$A/L/qrxpipe-handler" "$T/L/"
    printf 'Handler = L:qrxpipe-handler\nStackSize = 16384\nPriority = 5\nGlobVec = -1\n' > "$T/Devs/DOSDrivers/PIPE"
fi
[ -n "${QRX64_EXTRA_C:-}" ] && cp "$QRX64_EXTRA_C"/* "$T/C/"
# The App Store (m68k64 build: shows only apps for QRX OS One 64), with QRX_CC64 = the m68k64 clang (cc64)
if [ -n "${QRX_CC64:-}" ]; then
    "$QRX_CC64" -O2 -w "$REPO/tools/armithlon/appstore/QRXAppStore.c" -o "$T/C/QRXAppStore"
fi
GUI=0; grep -q '^ec64gfx.hidd$' "$K/modules.txt" && GUI=1
# The desktop's wallpapers in all sizes (the screen follows the Mac window), from the 32-bit system
if [ -d "$BU/SystemFull/QRX/Wallpapers" ]; then
    mkdir -p "$T/QRX/Wallpapers"
    cp "$BU/SystemFull/QRX/Wallpapers/"*.qrxwp "$T/QRX/Wallpapers/" 2>/dev/null || true
fi
# and the design icons of the four looks (gen-assets.swift / gen-assets.py)
if [ -d "$BU/SystemFull/QRX/Icons" ]; then
    mkdir -p "$T/QRX/Icons"
    cp -R "$BU/SystemFull/QRX/Icons/." "$T/QRX/Icons/"
    rm -f "$T/QRX/Icons/sheet.png"
fi
cat > "$T/S/Startup-Sequence" <<'EOF'
SetClock LOAD
Assign >NIL: L: SYS:L
Assign >NIL: DEVS: SYS:Devs
Assign >NIL: T: SYS:T
If EXISTS DEVS:DOSDrivers/PIPE
  Mount >NIL: DEVS:DOSDrivers/PIPE
EndIf
; the App Store installs into SYS:Apps (on the Pi /data/qrx64/Apps, the system's host directory)
If EXISTS SYS:Apps
  Assign >NIL: APPS: SYS:Apps
EndIf
Echo "QRX OS One 64 - 64-bit SMP AROS on the 68100EC64"
If EXISTS C:QRXHostSocket
  Run >NIL: C:QRXHostSocket
EndIf
Echo "Windows programs: qrx-win32 PROGRAM.EXE (sound through the board)"
Echo "Mac Classic: QRX68KRun, PowerPC: QRXAppRun, Mach-O: QRXMach"
Echo "OpenGL: EC64GLDemo   Power off: Shutdown"
EOF
if [ $GUI = 1 ]; then cat >> "$T/S/Startup-Sequence" <<'EOF'
Assign >NIL: QRX: SYS:
If EXISTS C:EC64Input
  Run >NIL: C:EC64Input
EndIf
If EXISTS C:OpenWB
  C:OpenWB
EndIf
If EXISTS C:QRXDesktop
  Run >NIL: C:QRXDesktop
EndIf
EOF
fi
python3 -I "$REPO/tools/armithlon/aros-qrx/boot/mkffsimg.py" "$T" "$BU/QRXOS64.img" "$MB" QRXOS64

cat > "$APP/MacOS/launch64" <<'EOF'
#!/bin/sh
# QRX OS One 64: 64-bit AROS on the dual 68100EC64, in a Mac window. The system disk is
# the directory System64 next to the app, through hostfs (qrxsys in modules.txt), when it has
# AROS.boot and the kernel has qrxsys; else QRXOS64.img (made from System64 with mkffsimg.py),
# changes are written back.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
K="$HERE/../Resources/qrx64"
ROOT="$HERE/../../.."
if [ -e "$ROOT/System64/AROS.boot" ] && grep -q '^qrxsys-handler$' "$K/modules.txt"; then
    SYSDISK="--hostfs $ROOT/System64"
else
    SYSDISK="--disk $ROOT/QRXOS64.img --disk-write"
fi
set --
while read -r m; do set -- "$@" "$K/$m"; done < "$K/modules.txt"
exec "$HERE/qrx64-engine" --window --cores "${QRX64_CORES:-2}" --ram "${QRX64_RAM:-2048}" \
    $SYSDISK --cmdline econsole-startup \
    "$K/core.elf" "$@" >>"$ROOT/qrxos64.log" 2>&1
EOF
chmod +x "$APP/MacOS/launch64"
echo "QRX OS One 64 in $BU: start with ARMithlon.app/Contents/MacOS/launch64"
