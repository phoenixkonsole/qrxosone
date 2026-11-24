#!/bin/bash
# Boots the test tree in the ARMithlon engine with its window and the normal desktop, starts S:job (usually Blender with its
# interface) and takes pictures of the guest screen. Stops after the given time.
# usage: run-gui.sh <job script> <seconds> <capture at seconds> [capture count] [engine options]
# Pictures: tree/gui.ppm (or gui.ppm.N with a count). Input for tests: QRX_TEST_CLICKS, QRX_TEST_KEYS,
# QRX_TEST_DRAG in the environment, see the engine's macos/qrx_aros_window.m.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
eval "$("$HERE/qrxenv.py" sh)"
ROOT=$QRX_ROOT
ENGINE=${QRX_ENGINE:-$W/engine-build/qrx68100_full_run}
T=${QRX_GUI_TREE:-$W/guitree}
# A tree with fonts, Python and Blender's scripts; the batch tree (tree) leaves the fonts out.
if [ ! -e "$T/C" ]; then
  mkdir -p "$T"
  rsync -a --exclude 'Demos/*' --exclude Emergency-Boot.adf --exclude 'QRX/x86/*' --exclude 'QRX/Windows/*' \
    --exclude 'QRX/Linux/*' --exclude 'Development/*' "$SYSTEM_TREE/" "$T/"
  rsync -a "$W/tree/2.79" "$W/tree/lib" "$T/"
fi
"$W/elf2hunk" "$W/build/bin/blender" "$T/blender" 2>&1 | grep -v '^$' || true
chmod +x "$T/blender"
cp "$1" "$T/S/job"
LIMIT=$2; AT=$3; COUNT=${4:-}; shift 4 2>/dev/null || shift $#
rm -f "$T/gui.ppm" "$T"/gui.ppm.*
# A short start without the desktop (QRXDesktop does not return): RTG monitor, host input, the job.
cat > "$T/S/Startup-Sequence" <<'S'
FailAt 30
MakeDir RAM:T RAM:ENV
Assign T: RAM:T
Assign ENV: RAM:ENV
Assign FONTS: SYS:Fonts
Assign LOCALE: SYS:Locale
Assign KEYMAPS: DEVS:Keymaps
Assign LIBS: DEVS:Drivers ADD
DEVS:Monitors/SAGA
Run <NIL: >NIL: C:QRXHostInput
Execute S:job
S
rm -f "$T"/glframe.*
export QRX_GL68K_CAPTURE=$T/glframe
export QRX_RTG_CAPTURE=$T/gui.ppm QRX_RTG_CAPTURE_AT=$AT
[ -n "$COUNT" ] && export QRX_RTG_CAPTURE_COUNT=$COUNT QRX_RTG_CAPTURE_STEP=${QRX_RTG_CAPTURE_STEP:-1}
QRX_FULL_ADF="$BOOT_DIR/boot.adf" "$ENGINE" --rom "$BOOT_DIR/aros-rom.bin" \
  --ext-rom "$BOOT_DIR/aros-ext.bin" --share "$T" --share-rw --apps "$W" --window \
  --seconds "$LIMIT" "$@" >"$T/engine.log" 2>&1
grep -E "\[RTG\]|QRX-GL68K|Software Failure" "$T/engine.log" | head -20
