#!/bin/bash
# Builds the GL door's guest library for 68k AROS programs: libqrxgl.a (GL 2.1 and the GLU calls Blender uses).
# usage: build_guest.sh <output directory>
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1
CC=$HERE/../blender/aros_cc.py
mkdir -p "$OUT/qgl"
for f in qgl qgl_gen qgl_glu; do
  "$CC" -O2 -c "$HERE/guest/$f.c" -o "$OUT/qgl/$f.o"
done
rm -f "$OUT/libqrxgl.a"
/opt/homebrew/bin/m68k-elf-ar rcs "$OUT/libqrxgl.a" "$OUT"/qgl/qgl.o "$OUT"/qgl/qgl_gen.o "$OUT"/qgl/qgl_glu.o
echo "built: $OUT/libqrxgl.a"
