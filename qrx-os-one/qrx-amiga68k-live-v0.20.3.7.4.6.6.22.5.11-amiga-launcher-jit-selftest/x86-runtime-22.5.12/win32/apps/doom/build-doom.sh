#!/usr/bin/env bash
# Build PrBoom-plus 2.5.1.4 (GPL, SDL 1.2) with MinGW into prboom-plus.exe for qrx-win32.
# Software renderer, DirectDraw through SDL's windx5 driver; no OpenGL, sound libraries
# or network.  Plain compiler calls with our own config.h (no configure).
#   build-doom.sh <prboom-plus-2.5.1.4 source dir> <SDL build dir from build-sdl.sh> <out dir>
set -Eeuo pipefail
S="${1:?prboom source}"; SD="${2:?SDL build}"; O="${3:?out dir}"; HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CC=i686-w64-mingw32-gcc
mkdir -p "$O/obj"
CF=(-O2 -march=i686 -mno-sse -mno-mmx -fno-strict-aliasing -fcommon -w -DHAVE_CONFIG_H -include "$HERE/config.h" -I"$HERE" -I"$HERE/shim" -I"$S/src" -I"$SD/include/SDL" -I"$SD/include")
objs=()
for f in "$S"/src/*.c "$S"/src/SDL/{i_main,i_system,i_video,i_sound,i_joy,i_network,i_sshot}.c "$HERE"/qrx_doom_stubs.c; do
  b="$(basename "${f%.c}")"
  case "$b" in gl_*|e6y_launcher|d_server|d_ipxgate|i_pcsound|w_mmap) continue;; esac
  "$CC" "${CF[@]}" -c "$f" -o "$O/obj/$b.o"
  objs+=("$O/obj/$b.o")
done
"$CC" -mwindows -o "$O/prboom-plus.exe" "${objs[@]}" -L"$SD" -lmingw32 -lSDLmain -lSDL -lddraw -ldinput -ldxguid -lwinmm -lgdi32 -luser32 -lkernel32 -lm
echo "PRBOOM $O/prboom-plus.exe ($(stat -c %s "$O/prboom-plus.exe") bytes, ${#objs[@]} objects)"
