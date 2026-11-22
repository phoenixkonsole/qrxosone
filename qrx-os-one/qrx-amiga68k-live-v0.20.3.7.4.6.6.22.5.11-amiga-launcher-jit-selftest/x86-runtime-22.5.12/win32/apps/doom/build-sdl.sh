#!/usr/bin/env bash
# Build SDL 1.2.15 (DirectDraw "windx5" video driver) as a static MinGW library for
# the qrx-win32 PrBoom-plus build.  Plain compiler calls with our own SDL_config.h;
# SDL's configure/make are not used.
#   build-sdl.sh <SDL-1.2.15 source dir> <out dir>
set -Eeuo pipefail
S="${1:?SDL source dir}"; O="${2:?out dir}"; HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# qrx-win32 implements msvcrt, while current MinGW defaults to UCRT.
CC="i686-w64-mingw32-gcc -U_UCRT -D__MSVCRT_VERSION__=0x0700 -mcrtdll=msvcrt-os"; AR=i686-w64-mingw32-ar
mkdir -p "$O/obj" "$O/include/SDL"
cp "$S"/include/*.h "$O/include/SDL/"; cp "$HERE/SDL_config.h" "$O/include/SDL/SDL_config.h"
CF=(-O2 -march=i686 -mno-sse -mno-mmx -fno-strict-aliasing -w -I"$O/include/SDL" -I"$S/src" -I"$S/src/video" -I"$S/src/events" -I"$S/src/video/wincommon" -D_GNU_SOURCE -DNO_STDIO_REDIRECT)
files=( "$S"/src/SDL.c "$S"/src/SDL_error.c "$S"/src/SDL_fatal.c "$S"/src/cpuinfo/SDL_cpuinfo.c
        "$S"/src/events/*.c "$S"/src/file/SDL_rwops.c "$S"/src/stdlib/*.c
        "$S"/src/timer/SDL_timer.c "$S"/src/timer/win32/SDL_systimer.c
        "$S"/src/thread/SDL_thread.c "$S"/src/thread/generic/*.c "$S"/src/loadso/win32/SDL_sysloadso.c
        "$S"/src/video/SDL_*.c "$S"/src/video/wincommon/*.c "$S"/src/video/windib/*.c "$S"/src/video/windx5/*.c "$S"/src/video/dummy/*.c )
objs=()
for f in "${files[@]}"; do
  case "$f" in *SDL_yuv_mmx.c) continue;; esac
  o="$O/obj/$(basename "$(dirname "$f")")_$(basename "${f%.c}").o"
  $CC "${CF[@]}" -c "$f" -o "$o"
  objs+=("$o")
done
rm -f "$O/libSDL.a"; "$AR" rcs "$O/libSDL.a" "${objs[@]}"
$CC "${CF[@]}" -c "$S/src/main/win32/SDL_win32_main.c" -o "$O/obj/SDL_win32_main.o"
rm -f "$O/libSDLmain.a"; "$AR" rcs "$O/libSDLmain.a" "$O/obj/SDL_win32_main.o"
echo "SDL $O/libSDL.a (${#objs[@]} objects) + libSDLmain.a"
