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
$CC "$HERE/nettest.c" -o "$OUT/nettest.exe" -lwsock32
$CC -DD3DV=7 "$HERE/d3d7test.c" -o "$OUT/d3d7test.exe" -lddraw -ldxguid -lwinmm
$CC -DD3DV=5 "$HERE/d3d7test.c" -o "$OUT/d3d5test.exe" -lddraw -ldxguid -lwinmm
$CC -std=gnu99 -I/opt/homebrew/include "$HERE/vktest.c" -o "$OUT/vktest.exe" -lwinmm
