#!/bin/sh
# Builds xt.cpp (the calling convention xptcinvoke/xptcstubs_m68k64_aros.cpp rely on) and runs it in
# the QRX OS One 64 guest. Expected: XPT PASS.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK=${QRX_SDK:-/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64/AROS/Development}
export PATH=$SDK/../../../linux-x86_64/tools:$PATH
T=$(mktemp -d)
/opt/qrx-os-one/aros64/tc/bin/clang++ --sysroot=$SDK -O2 "$HERE/xt.cpp" -o $T/xt -L/opt/qrx-os-one/ec64llvm/build/lib -lqrxaroscompat
sh "$HERE/../../../rust/tests/run-guest.sh" $T/xt 120
rm -rf $T
