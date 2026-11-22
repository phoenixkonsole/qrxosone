#!/bin/bash
# Builds the GL guest library and EC64GLDemo for QRX OS One 64 (m68k64).
#   build-ec64.sh CC64 OUTDIR      CC64: the m68k64 clang with the AROS 64 sysroot (cc64)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CC=${1:?cc64}; OUT=${2:?out dir}
mkdir -p "$OUT"
G="$HERE/../guest"
for f in qgl qgl_gen qgl_glu; do "$CC" -O2 -w -c "$G/$f.c" -o "$OUT/$f.o"; done
"$CC" -O2 -I"$G" -o "$OUT/EC64GLDemo" "$HERE/EC64GLDemo.c" "$OUT/qgl.o" "$OUT/qgl_gen.o" "$OUT/qgl_glu.o" -lcybergraphics
echo "built: $OUT/EC64GLDemo"
