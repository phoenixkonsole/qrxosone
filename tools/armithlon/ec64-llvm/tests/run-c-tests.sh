#!/bin/bash
# Builds ctests.c for the Mac and for m68k64 at each optimization level. The reference core must
# give the numbers the Mac gave. Usage: run-c-tests.sh <llvm build dir> <work dir>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; B="$1"; W="$2"; E="$HERE/../../../../upstream/armithlon/2.6/engine"
mkdir -p "$W"
cc -std=gnu11 -O2 -I"$E/include" "$HERE/runc.c" "$E/full68k/qrx68100ec64.c" -o "$W/runc"
cc -std=gnu11 -O1 -DHOST_TEST -fno-builtin -w "$HERE/ctests.c" -o "$W/ctests_host"
"$W/ctests_host" >"$W/ctests.want"
for O in 0 1 2 3 s; do
  "$B/bin/clang" --target=m68k64-unknown-unknown -ffreestanding -fno-builtin -fno-pic -O$O -mllvm -verify-machineinstrs -c "$HERE/ctests.c" -o "$W/ctests.o"
  echo "-O$O: $("$W/runc" "$W/ctests.o" <"$W/ctests.want" | tail -1)"
  "$W/runc" "$W/ctests.o" <"$W/ctests.want" >"$W/ctests.out" || { grep -v '^ok' "$W/ctests.out"; }
done
