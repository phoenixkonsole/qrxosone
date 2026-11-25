#!/bin/bash
# Builds nsstest.c against the static NSS of ../build.sh. Usage: build-test.sh <nss out> <nspr out>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; O="$(cd "$1" && pwd)"; P="$(cd "$2" && pwd)"
SDK=/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64/AROS/Development
export PATH="/opt/qrx-os-one/aros64/build-qrx/bin/linux-x86_64/tools:$PATH"
L=$O/dist/Release/lib
/opt/qrx-os-one/aros64/tc/bin/clang --sysroot=$SDK -O2 -fno-pic -fstack-protector-strong -DBSDSOCKET_NOLIBDEFINES \
  -D__BSDSOCKET_NOLIBBASE__ -DXP_UNIX -DAROS -I"$P/include" -I"$O/dist/public/nss" -I"$O/dist/private/nss" \
  "$HERE/nsstest.c" -o "$O/nsstest" -L$L -L"$P" \
  -lssl -lnss_static -lpk11wrap_static -lcryptohi -lcerthi -lcertdb -lnsspki -lnssdev -lnssb -lsoftokn_static \
  -lfreebl_static -lgcm -lsqlite -lnssutil -lssl -lnss_static -lpk11wrap_static -lcertdb -lnsspki -lnssdev -lnssb \
  -lsoftokn_static -lfreebl_static -lnssutil -lnss_zlib -lplds4 -lplc4 -lnspr4 \
  -L/opt/qrx-os-one/ec64llvm/build/lib -lqrxaroscompat -lpthread
echo "$O/nsstest"
