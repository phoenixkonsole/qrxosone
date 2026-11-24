#!/bin/bash
# Builds libqrxaroscompat.a (POSIX functions AROS's libcrt lacks, sockets over the board's host
# door, poll, host entropy, process spawning; see qrxaroscompat.c and qrxnet.c) for
# m68k64-unknown-aros and installs it next to libunwind in the LLVM build, where clang's AROS
# driver looks for libraries. The Rust target spec links it.
# Usage: build-compat.sh <llvm build dir> <AROS Development dir>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$1"; SDK="$2"
[ -x "$B/bin/clang" ] && [ -d "$SDK/include" ] || { echo "usage: $0 <llvm build> <AROS Development>"; exit 2; }
T=$(mktemp -d)
for c in qrxaroscompat qrxnet qrxwchar; do
  "$B/bin/clang" --target=m68k64-unknown-aros --sysroot "$SDK" -O2 -Wall -Werror -c "$HERE/$c.c" -o "$T/$c.o"
done
rm -f "$B/lib/libqrxaroscompat.a"
"$B/bin/llvm-ar" rcs "$B/lib/libqrxaroscompat.a" "$T/qrxaroscompat.o" "$T/qrxnet.o" "$T/qrxwchar.o"
rm -rf "$T"
echo "installed $B/lib/libqrxaroscompat.a"
