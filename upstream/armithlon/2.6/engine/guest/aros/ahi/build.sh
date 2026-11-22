#!/bin/sh
# usage: build.sh <repo-root> <output-dir>
# Produces qrxhost.audio (Devs/AHI), QRXHOST (Devs/AudioModes) and QRXAHITest.
set -eu
ROOT=$1; OUT=$2
HERE=$ROOT/upstream/armithlon/2.6/engine/guest/aros/ahi
SDK=$ROOT/upstream/aros-deadwood/m68k-build-5b5fd4c/aros-m68k-sdk-5b5fd4c.tar.xz
PATH=/opt/amiga/bin:$PATH
mkdir -p "$OUT/inc"
tar -xf "$SDK" -C "$OUT/inc" --strip-components=2 Development/include/devices/ahi.h Development/include/libraries/ahi_sub.h
FLAGS="-m68040 -O2 -fomit-frame-pointer -noixemul -I$OUT/inc -DIPTR=ULONG -Wno-pointer-sign"
# libnix startup would claim the entry point of a library, so none is linked.
m68k-amigaos-gcc $FLAGS -nostartfiles -nostdlib -fno-toplevel-reorder "$HERE/qrxhost_audio.c" -o "$OUT/qrxhost.audio" -lamiga -lgcc
m68k-amigaos-gcc $FLAGS "$HERE/QRXAHITest.c" -o "$OUT/QRXAHITest"
python3 "$HERE/make-modefile.py" "$OUT/QRXHOST" "$OUT/ahi.prefs"
