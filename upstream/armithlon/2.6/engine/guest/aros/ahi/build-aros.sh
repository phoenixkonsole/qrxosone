#!/bin/sh
# usage: build-aros.sh <repo-root> <output-dir>
# The same three files as build.sh, built with the Deadwood AROS m68k toolchain instead of
# m68k-amigaos-gcc (AROS_TC = toolchain-core-m68k dir, AROS_TOOLS = dir with elf2hunk).
set -eu
ROOT=$1; OUT=$2
HERE=$ROOT/upstream/armithlon/2.6/engine/guest/aros/ahi
SDK=$ROOT/upstream/aros-deadwood/m68k-build-5b5fd4c/aros-m68k-sdk-5b5fd4c.tar.xz
TC=${AROS_TC:-/opt/qrx-os-one/deadwood-aros-build/toolchain-core-m68k}
E2H=${AROS_TOOLS:-/opt/qrx-os-one/deadwood-aros-build/core-amiga-m68k/bin/linux-x86_64/tools}/elf2hunk
mkdir -p "$OUT/inc"
tar -xf "$SDK" -C "$OUT/inc" --strip-components=2 Development/include/devices/ahi.h Development/include/libraries/ahi_sub.h
FLAGS="-m68040 -O2 -fomit-frame-pointer -fno-pic -fno-PIC -I$OUT/inc -Wno-pointer-sign"
# a library: no startup code, the romtag is found by scanning the hunks
"$TC/m68k-aros-gcc" $FLAGS -ffreestanding -fno-builtin -fno-toplevel-reorder -c "$HERE/qrxhost_audio.c" -o "$OUT/qrxhost_audio.o"
"$TC/m68k-aros-ld" -r -T "$ROOT/tools/armithlon/handler.ld" "$OUT/qrxhost_audio.o" -o "$OUT/qrxhost_audio.elf"
"$E2H" "$OUT/qrxhost_audio.elf" "$OUT/qrxhost.audio"
"$TC/m68k-aros-gcc" $FLAGS "$HERE/QRXAHITest.c" -o "$OUT/QRXAHITest.elf"
"$E2H" "$OUT/QRXAHITest.elf" "$OUT/QRXAHITest"
python3 "$HERE/make-modefile.py" "$OUT/QRXHOST" "$OUT/ahi.prefs"
