#!/bin/bash
# Builds the GL guest libraries and their programs for QRX OS One 64 (m68k64):
#   GL 2.1 (service 8): EC64GLDemo; GL 3.3 core (service 13): libqgl3.a and EC64GL3Test.
#   build-ec64.sh CC64 OUTDIR      CC64: the m68k64 clang with the AROS 64 sysroot (cc64)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CC=${1:?cc64}; OUT=${2:?out dir}
mkdir -p "$OUT"
G="$HERE/../guest"
for f in qgl qgl_gen qgl_glu qgl3 qgl3_gen; do "$CC" -O2 -w -c "$G/$f.c" -o "$OUT/$f.o"; done
"$CC" -O2 -I"$G" -o "$OUT/EC64GLDemo" "$HERE/EC64GLDemo.c" "$OUT/qgl.o" "$OUT/qgl_gen.o" "$OUT/qgl_glu.o" -lcybergraphics
AR=$(dirname "$(command -v "$CC" || echo "$CC")")/llvm-ar
[ -x "$AR" ] || AR=llvm-ar
rm -f "$OUT/libqgl3.a"; "$AR" rcs "$OUT/libqgl3.a" "$OUT/qgl3.o" "$OUT/qgl3_gen.o"
"$CC" -O2 -I"$G" -o "$OUT/EC64GL3Test" "$HERE/EC64GL3Test.c" "$OUT/libqgl3.a"
echo "built: $OUT/EC64GLDemo $OUT/libqgl3.a $OUT/EC64GL3Test"
