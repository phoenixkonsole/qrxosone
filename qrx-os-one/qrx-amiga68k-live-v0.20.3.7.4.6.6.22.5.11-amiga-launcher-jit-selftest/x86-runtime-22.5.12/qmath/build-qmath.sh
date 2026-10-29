#!/usr/bin/env bash
# Compile the qmath library (musl-derived math + qfloat) into OUTDIR/libqmath.a
#   build-qmath.sh <cc> <ar> <outdir> [cflags...]
set -Eeuo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CC="$1"; AR="$2"; OUT="$3"; shift 3
mkdir -p "$OUT/qmath-obj"
objs=()
for f in "$HERE"/*.c; do
  o="$OUT/qmath-obj/$(basename "${f%.c}").o"
  "$CC" "$@" -std=gnu99 -O2 -fno-builtin -I"$HERE/include" -I"$HERE" -w -c "$f" -o "$o"
  objs+=("$o")
done
rm -f "$OUT/libqmath.a"
"$AR" rcs "$OUT/libqmath.a" "${objs[@]}"
echo "QMATH $OUT/libqmath.a (${#objs[@]} objects)"
