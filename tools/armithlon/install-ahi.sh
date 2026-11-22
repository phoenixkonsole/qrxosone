#!/bin/sh
# usage: install-ahi.sh <repo-root> <system-tree>
# Builds the AHI host driver and makes it the default: no Paula exists on this
# board, and AHI's fallback to that mode hangs waiting for its interrupts.
set -eu
ROOT=$1; SYS=$2
OUT=$ROOT/.build/ahi
"$ROOT/upstream/armithlon/2.6/engine/guest/aros/ahi/build.sh" "$ROOT" "$OUT"
cp "$OUT/qrxhost.audio" "$SYS/Devs/AHI/qrxhost.audio"
cp "$OUT/QRXHOST" "$SYS/Devs/AudioModes/QRXHOST"
cp "$OUT/QRXAHITest" "$SYS/C/QRXAHITest"
mkdir -p "$SYS/Prefs/Env-Archive/Sys"
cp "$OUT/ahi.prefs" "$SYS/Prefs/Env-Archive/Sys/ahi.prefs"
mkdir -p "$SYS/Storage/AudioModes"
if [ -f "$SYS/Devs/AudioModes/PAULA" ]; then mv "$SYS/Devs/AudioModes/PAULA" "$SYS/Storage/AudioModes/PAULA"; fi
