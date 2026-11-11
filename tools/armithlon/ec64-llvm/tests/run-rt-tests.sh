#!/bin/bash
# Builds compiler-rt's builtins with build-builtins.sh, then rttests.c for the Mac and for m68k64
# at each optimization level, links the builtins it needs in, and runs it on the reference core.
# Usage: run-rt-tests.sh <llvm build dir> <llvm-project source dir> <work dir>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; B="$1"; SRC="$2"; W="$3"; E="$HERE/../../../../upstream/armithlon/2.6/engine"
mkdir -p "$W"
bash "$HERE/../build-builtins.sh" "$B" "$SRC" "$W" | tail -1
cc -std=gnu11 -O2 -I"$E/include" "$HERE/runc.c" "$E/full68k/qrx68100ec64.c" -o "$W/runc"
cc -std=gnu11 -O1 -DHOST_TEST -fno-builtin -w "$HERE/rttests.c" -o "$W/rttests_host"
"$W/rttests_host" >"$W/rttests.want"
for O in 0 1 2 3 s; do
  "$B/bin/clang" --target=m68k64-unknown-unknown -ffreestanding -fno-builtin -fno-pic -O$O -mllvm -verify-machineinstrs -c "$HERE/rttests.c" -o "$W/rttests.o"
  # One relocatable object: the test and the archive members it uses.
  "$B/bin/ld.lld" -r "$W/rttests.o" "$W/libclang_rt.builtins-m68k64.a" -o "$W/rttests-linked.o"
  echo "-O$O: $("$W/runc" "$W/rttests-linked.o" <"$W/rttests.want" | tail -1)"
  "$W/runc" "$W/rttests-linked.o" <"$W/rttests.want" >"$W/rttests.out" || { grep -v '^ok' "$W/rttests.out"; }
done
