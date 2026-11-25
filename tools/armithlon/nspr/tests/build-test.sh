#!/bin/bash
# Builds nsprtest.c against the libraries of ../build.sh. Usage: build-test.sh <firefox tree> <out dir>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; T="$1"; O="$2"
SDK=/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64/AROS/Development
export PATH="/opt/qrx-os-one/aros64/build-qrx/bin/linux-x86_64/tools:$PATH"
/opt/qrx-os-one/aros64/tc/bin/clang --sysroot=$SDK -O2 -fno-pic -fstack-protector-strong -DBSDSOCKET_NOLIBDEFINES \
  -D__BSDSOCKET_NOLIBBASE__ -DXP_UNIX -DAROS -I"$T/config/external/nspr" -I"$T/nsprpub/pr/include" \
  -I"$T/nsprpub/lib/ds" -I"$T/nsprpub/lib/libc/include" "$HERE/nsprtest.c" -o "$O/nsprtest" \
  -L"$O" -lplds4 -lplc4 -lnspr4 -L/opt/qrx-os-one/ec64llvm/build/lib -lqrxaroscompat -lpthread
echo "$O/nsprtest"
