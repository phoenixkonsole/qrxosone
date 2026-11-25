#!/bin/bash
# The "all ones if carry" pattern (SETCS_C) after a compare must not use subx: subx reads the X flag,
# which CMP/TST leave unchanged on the 68k (x86's CMP+SBB idiom). Checks the assembly of
# setcs-carry.cpp at -O1/-O2/-Os, and with a guest (QRX OS One 64, rust/tests/run-guest.sh) the
# results. Usage: setcs-carry.sh <llvm build dir> [guest]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK=${QRX_SDK:-/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64/AROS/Development}
for O in 1 2 s; do
  asm=$("$1/bin/clang++" --target=m68k64-unknown-aros --sysroot=$SDK -O$O -S "$HERE/setcs-carry.cpp" -o -)
  echo "$asm" | grep -q "scs" || { echo "-O$O: no scs (the test lost its pattern)"; exit 1; }
  if echo "$asm" | grep -q "subx"; then echo "-O$O: subx after a compare"; exit 1; fi
  echo "-O$O: ok"
done
if [ "$2" = guest ]; then
  T=$(mktemp -d)
  PATH=$SDK/../../../linux-x86_64/tools:$PATH "$1/bin/clang++" --target=m68k64-unknown-aros --sysroot=$SDK -O2 \
    "$HERE/setcs-carry.cpp" -o $T/t -L"$1/lib" -lqrxaroscompat
  out=$(bash "$HERE/../../rust/tests/run-guest.sh" $T/t 120)
  rm -rf $T
  echo "$out"
  echo "$out" | grep -q "sel(0,27) = 27, sel(5,27) = 4294967295" && echo "SETCS PASS" || { echo "SETCS FAIL"; exit 1; }
fi
