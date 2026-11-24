#!/bin/bash
# Boots a private AROS test tree in the ARMithlon engine without a window, runs one AmigaDOS
# script from the tree (S:job), and stops when the script has written SYS:job.done.
# usage: run-guest.sh <tree> <timeout seconds> [engine options...]
# Output of the guest programs belongs in files on the tree, see the job script.
set -u
TREE=$1; LIMIT=$2; shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
eval "$("$HERE/qrxenv.py" sh)"
ROOT=$QRX_ROOT
ENGINE=${QRX_ENGINE:-$W/engine-snapshot}
rm -f "$TREE/job.done"
cat > "$TREE/S/Startup-Sequence" <<'S'
FailAt 30
MakeDir RAM:T RAM:ENV
Assign T: RAM:T
Assign ENV: RAM:ENV
Assign FONTS: SYS:Fonts
Assign LOCALE: SYS:Locale
Execute S:job
Echo done >SYS:job.done
S
QRX_FULL_ADF="$BOOT_DIR/boot.adf" "$ENGINE" --rom "$BOOT_DIR/aros-rom.bin" \
  --ext-rom "$BOOT_DIR/aros-ext.bin" --share "$TREE" --share-rw --apps "${QRX_APPS:-$ROOT/.build/bundle/Apps}" --realtime \
  --seconds "$LIMIT" "$@" >"$TREE/engine.log" 2>&1 &
PID=$!
T0=$(date +%s)
while kill -0 $PID 2>/dev/null; do
  if [ -e "$TREE/job.done" ]; then
    sleep 1; kill $PID 2>/dev/null; wait $PID 2>/dev/null
    echo "job finished after $(( $(date +%s) - T0 )) s of host time"; exit 0
  fi
  sleep 1
done
echo "job did NOT finish within $LIMIT s"; exit 1
