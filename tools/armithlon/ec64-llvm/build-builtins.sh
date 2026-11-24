#!/bin/bash
# Builds compiler-rt's generic builtins for m68k64 (soft float, 64- and 128-bit integer helpers)
# without CMake, which would need a working libc for the target first.
# Usage: build-builtins.sh <llvm build dir> <llvm-project source dir> <out dir> [AROS Development dir]
# Writes <out dir>/libclang_rt.builtins-m68k64.a, the name the AROS build links. With the AROS SDK
# (4th argument) it adds emutls.c: thread_local on AROS is emulated TLS on pthread keys.
set -e
B="$1"; SRC="$2/compiler-rt/lib/builtins"; OUT="$3"
[ -x "$B/bin/clang" ] && [ -f "$SRC/CMakeLists.txt" ] || { echo "usage: $0 <llvm build> <llvm-project src> <out>"; exit 2; }
W="$OUT/builtins-obj"
rm -rf "$W"; mkdir -p "$W"
# The source list is GENERIC_SOURCES from compiler-rt's own CMakeLists.txt. Left out: Apple's
# versioning and OS checks, and trampolines, which need an executable stack.
FILES=$(sed -n '/^set(GENERIC_SOURCES/,/^)/p' "$SRC/CMakeLists.txt" | tr -s ' \t' '\n' | grep '\.c$' |
        grep -v -E '^(apple_versioning|os_version_check|trampoline_setup)\.c$')
FLAGS="--target=m68k64-unknown-unknown -O2 -ffreestanding -fno-builtin -fno-pic -fvisibility=hidden -fomit-frame-pointer -w"
fail=0; n=0
for f in $FILES; do
    n=$((n + 1))
    if ! "$B/bin/clang" $FLAGS -c "$SRC/$f" -o "$W/${f%.c}.o" 2>"$W/${f%.c}.log"; then
        echo "FAIL $f: $(grep -m1 -E 'error|LLVM ERROR' "$W/${f%.c}.log")"; fail=$((fail + 1))
    fi
done
if [ -n "${4:-}" ]; then
    n=$((n + 1))
    if ! "$B/bin/clang" --target=m68k64-unknown-aros --sysroot "$4" -O2 -fno-pic -fvisibility=hidden -w \
            -c "$SRC/emutls.c" -o "$W/emutls.o" 2>"$W/emutls.log"; then
        echo "FAIL emutls.c: $(grep -m1 -E 'error|LLVM ERROR' "$W/emutls.log")"; fail=$((fail + 1))
    fi
fi
rm -f "$OUT/libclang_rt.builtins-m68k64.a"
"$B/bin/llvm-ar" rcs "$OUT/libclang_rt.builtins-m68k64.a" "$W"/*.o
echo "$n files, $fail failures, $(ls "$W"/*.o | wc -l | tr -d ' ') objects in $OUT/libclang_rt.builtins-m68k64.a"
[ "$fail" = 0 ]
