#!/usr/bin/env bash
# Guest side of the boot: the HostFS handlers (SYS: and APPS:), host memory and host input,
# built with the Deadwood m68k-aros toolchain (as tools/armithlon/build-hostfs-guest.sh does
# with the bootstrapped SDK on the Mac).
#   build-guest.sh OUTDIR    (AROS_TC = toolchain-core-m68k dir, AROS_TOOLS = dir with elf2hunk)
set -Eeuo pipefail
REPO="$(cd "$(dirname "$0")/../../../.." && pwd)"
OUT="$1"; mkdir -p "$OUT"
TC="${AROS_TC:-/opt/qrx-os-one/deadwood-aros-build/toolchain-core-m68k}"
E2H="${AROS_TOOLS:-/opt/qrx-os-one/deadwood-aros-build/core-amiga-m68k/bin/linux-x86_64/tools}/elf2hunk"
CC="$TC/m68k-aros-gcc"; LD="$TC/m68k-aros-ld"
SRC="$REPO/upstream/armithlon/2.6/engine/guest/aros/bsp"
LDS="$REPO/tools/armithlon/handler.ld"
handler(){ # name source extra-cflags...
  local name=$1 src=$2; shift 2
  "$CC" -c -O2 -m68040 -ffreestanding -fno-builtin -fno-pic -fno-PIC "$@" "$SRC/$src" -o "$OUT/$name.o"
  "$LD" -r -T "$LDS" "$OUT/$name.o" -o "$OUT/$name.elf"
  "$E2H" "$OUT/$name.elf" "$OUT/$name"
}
handler QRXHostFS QRXHostFS.c
handler QRXHostMemory QRXHostMemory.c
handler QRXAppsFS QRXHostFS.c -DQRXV_HFS_BASE=0xff000840UL '-DQRX_VOLUME_NAME="APPS"'
for p in QRXHostInput QRXMoreMemory; do
  "$CC" -O2 -m68040 -fno-pic -fno-PIC "$SRC/$p.c" -o "$OUT/$p.elf"
  "$E2H" "$OUT/$p.elf" "$OUT/$p"
done
ls -la "$OUT" | grep -v '\.o$\|\.elf$'
