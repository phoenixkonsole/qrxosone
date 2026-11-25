#!/bin/bash
# Compiles ssp-return.c with the stack protector and checks that the block ending in rts copies the
# result into the second return register. Usage: ssp-return.sh <llvm build dir>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
for O in 1 2 s; do
  "$1/bin/clang" --target=m68k64-unknown-unknown -ffreestanding -fno-pic -O$O -fstack-protector-strong -S \
    "$HERE/ssp-return.c" -o - | awk -v O=$O '
    /^[.A-Za-z_%;]/ { blk = "" }          # a label or block comment starts a new block
    /^\t/ { blk = blk $0 "\n" }
    /^\trts/ { n++; if (blk !~ /movn\.q\t%d0, %a0|movn\.q\t%a0, %d0/) { bad++; printf "-O%s: rts without the dual copy:\n%s", O, blk } }
    END { if (!n) { print "-O" O ": no rts"; exit 1 } if (bad) exit 1; print "-O" O ": ok" }'
done
