#!/bin/sh
# usage: build.sh <output-dir>. Needs MinGW; links msvcrt, which qrx-win32 implements.
set -e
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=$1
mkdir -p "$OUT"
CC="i686-w64-mingw32-gcc -O2 -U_UCRT -D__MSVCRT_VERSION__=0x0700 -mcrtdll=msvcrt-os -w"
$CC "$HERE/gltest.c" -o "$OUT/gltest.exe" -lopengl32 -lgdi32 -lwinmm
$CC "$HERE/glidetest.c" -o "$OUT/glidetest.exe" -lwinmm
$CC -DD3DV=9 "$HERE/d3dtest.c" -o "$OUT/d3d9test.exe" -lwinmm
$CC -DD3DV=8 "$HERE/d3dtest.c" -o "$OUT/d3d8test.exe" -lwinmm
