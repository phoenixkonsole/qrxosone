#!/bin/sh
# usage: build.sh output.hunk   (needs the m68k-amigaos toolchain in /opt/amiga)
set -e
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PATH=/opt/amiga/bin:$PATH
m68k-amigaos-gcc -m68040 -O2 -fomit-frame-pointer -fno-toplevel-reorder -fno-inline-functions -noixemul \
    -nostartfiles -nostdlib "$HERE/bench.c" -o "$1" -lgcc
