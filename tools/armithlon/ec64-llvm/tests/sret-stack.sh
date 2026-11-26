#!/bin/bash
# Struct return must not overwrite the caller's result buffer (sret-stack.cpp). The m68k64 backend
# lowers the fast calling convention as the C one, so viahelper() passes the hidden pointer on the
# stack too ("move.q %a0, (%sp)" before its call); with a guest (QRX OS One 64,
# rust/tests/run-guest.sh) the results must be right. Usage: sret-stack.sh <llvm build dir> [guest]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK=${QRX_SDK:-/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64/AROS/Development}
for O in 1 2 s; do
  asm=$("$1/bin/clang++" --target=m68k64-unknown-aros --sysroot=$SDK -O$O -S "$HERE/sret-stack.cpp" -o - |
        sed -n '/^_Z9viahelperv:/,/rts/p')
  [ -n "$asm" ] || { echo "-O$O: no viahelper"; exit 1; }
  echo "$asm" | grep -q "move.q	%a0, (%sp)" || { echo "-O$O: the hidden pointer does not go on the stack"; exit 1; }
  echo "-O$O: ok"
done
if [ "$2" = guest ]; then
  T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
  export PATH="$1/bin:$SDK/../../../linux-x86_64/tools:$PATH"
  "$1/bin/clang++" --target=m68k64-unknown-aros --sysroot=$SDK -O2 "$HERE/sret-stack.cpp" -o "$T/t"
  out=$(bash "$HERE/../../rust/tests/run-guest.sh" "$T/t" 60)
  echo "$out"
  echo "$out" | grep -q "SRET PASS"
fi
