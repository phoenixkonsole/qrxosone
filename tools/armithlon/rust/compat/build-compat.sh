#!/bin/bash
# Builds libqrxaroscompat.a (POSIX functions AROS's libcrt lacks, see qrxaroscompat.c) for
# m68k64-unknown-aros and installs it next to libunwind in the LLVM build, where clang's AROS
# driver looks for libraries. The Rust target spec links it.
# Usage: build-compat.sh <llvm build dir> <AROS Development dir>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$1"; SDK="$2"
[ -x "$B/bin/clang" ] && [ -d "$SDK/include" ] || { echo "usage: $0 <llvm build> <AROS Development>"; exit 2; }
T=$(mktemp -d)
"$B/bin/clang" --target=m68k64-unknown-aros --sysroot "$SDK" -O2 -c "$HERE/qrxaroscompat.c" -o "$T/qrxaroscompat.o"
rm -f "$B/lib/libqrxaroscompat.a"
"$B/bin/llvm-ar" rcs "$B/lib/libqrxaroscompat.a" "$T/qrxaroscompat.o"
rm -rf "$T"
echo "installed $B/lib/libqrxaroscompat.a"
