#!/bin/bash
# Builds the test kernel with clang, llvm-mc and lld, and runs it on two cores.
# Usage: run.sh <llvm build dir> <work dir> [optimization level]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; B="$1"; W="$2"; O="${3:-2}"; E="$HERE/../../../../../upstream/armithlon/2.6/engine"
mkdir -p "$W"
"$B/bin/llvm-mc" -triple=m68k64-unknown-unknown -filetype=obj "$HERE/boot.S" -o "$W/boot.o"
"$B/bin/clang" --target=m68k64-unknown-unknown -ffreestanding -fno-builtin -fno-pic -fno-asynchronous-unwind-tables -O$O -mllvm -verify-machineinstrs -c "$HERE/kernel.c" -o "$W/kernel.o"
"$B/bin/ld.lld" -T "$HERE/link.ld" "$W/boot.o" "$W/kernel.o" -o "$W/kernel.elf"
cc -std=gnu11 -O2 -I"$E/include" "$HERE/run.c" "$E/full68k/qrx68100ec64.c" "$E/full68k/qrx68100ec64_machine.c" "$E/full68k/qrx68100ec64_jit.c" -lpthread -o "$W/run"
"$W/run" "$W/kernel.elf"
