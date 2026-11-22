#!/bin/sh
# Run from the repository root; uses the previously bootstrapped AROS SDK.
set -eu
export PATH="/opt/homebrew/bin:$PATH"
SDK="${AROS_SDK:-.build/aros-sdk}/bin"
OUT=.build/boot-rom
SRC=upstream/armithlon/2.6/engine/guest/aros/bsp
mkdir -p "$OUT"
for NAME in QRXHostFS QRXHostMemory; do
 "$SDK/aros-gcc" -c -O2 -m68040 -ffreestanding -fno-builtin -fno-pic -fno-PIC "$SRC/$NAME.c" -o "$OUT/$NAME.o"
 m68k-elf-ld -r -T tools/armithlon/handler.ld "$OUT/$NAME.o" -o "$OUT/$NAME.elf"
 "$SDK/elf2hunk" "$OUT/$NAME.elf" "$OUT/$NAME"
done
"$SDK/aros-gcc" -O2 -m68040 -fno-pic -fno-PIC "$SRC/QRXHostInput.c" -o "$OUT/QRXHostInput.elf"
"$SDK/elf2hunk" "$OUT/QRXHostInput.elf" "$OUT/QRXHostInput"

"$SDK/aros-gcc" -c -O2 -m68040 -ffreestanding -fno-builtin -fno-pic -fno-PIC -DQRXV_HFS_BASE=0xff000840UL '-DQRX_VOLUME_NAME="APPS"' "$SRC/QRXHostFS.c" -o "$OUT/QRXAppsFS.o"
m68k-elf-ld -r -T tools/armithlon/handler.ld "$OUT/QRXAppsFS.o" -o "$OUT/QRXAppsFS.elf"
"$SDK/elf2hunk" "$OUT/QRXAppsFS.elf" "$OUT/QRXAppsFS"
