#!/usr/bin/env bash
# Compile AROS-side QRX board tools with your configured Deadwood m68k SDK.
# This DOES NOT build or patch an AROS Kickstart, Exec or DOS handler.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${AROS_CC:-m68k-aros-gcc}"
OUT="${1:-$HERE/build}"
command -v "$CC" >/dev/null || { echo "AROS cross compiler '$CC' unavailable; supply AROS_CC" >&2; exit 2; }
mkdir -p "$OUT"
for name in QRXShare QRXSmpInfo QRXBoardInfo QRXSmpBench; do
  "$CC" -O2 -m68040 -DQRX_NATIVE_040=1 -I"$HERE" -I"$HERE/../.." "$HERE/$name.c" -o "$OUT/$name"
  echo "built $OUT/$name"
done
