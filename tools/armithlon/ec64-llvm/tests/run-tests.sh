#!/bin/bash
# Compiles basic.ll with llc for m68k64 at each optimization level, with the machine code verifier,
# makes a flat image and runs it in the reference core.
# Usage: run-tests.sh <llvm build dir> <work dir>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; B="$1"; W="$2"; E="$HERE/../../../../upstream/armithlon/2.6/engine"
mkdir -p "$W"
cc -std=gnu11 -O2 -I"$E/include" "$HERE/run.c" "$E/full68k/qrx68100ec64.c" -o "$W/run"
for O in 0 1 2 3; do
  "$B/bin/llc" -mtriple=m68k64-unknown-unknown -relocation-model=pic -O$O -verify-machineinstrs -filetype=obj "$HERE/basic.ll" -o "$W/basic.o"
  "$B/bin/llvm-objcopy" -O binary -j .text "$W/basic.o" "$W/basic.bin"
  "$B/bin/llvm-nm" --defined-only "$W/basic.o" | awk '{print $1, $3}' >"$W/basic.syms"
  echo "-O$O: $("$W/run" "$W/basic.bin" "$W/basic.syms" | tail -1)"
  "$W/run" "$W/basic.bin" "$W/basic.syms" >/dev/null || { "$W/run" "$W/basic.bin" "$W/basic.syms" | grep -v '^ok'; exit 1; }
done
