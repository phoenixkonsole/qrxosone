#!/bin/bash
# Builds NSS (security/nss of Firefox ESR 153.8.0) for m68k64-unknown-aros as static libraries,
# with NSS's own gyp files through the gyp in Firefox's third_party/python, then ninja.
# Static: softoken and freebl linked into the program (NSS_STATIC_SOFTOKEN), AROS has no dlopen.
# Usage: build.sh <firefox tree with nsprpub and third_party/python/gyp> <nss dir> <nspr out> <out>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
FT="$(cd "$1" && pwd)"; NSS="$(cd "$2" && pwd)"; NSPR="$(cd "$3" && pwd)"; OUT="$4"
mkdir -p "$OUT/dist"; OUT="$(cd "$OUT" && pwd)"
SDK=/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64/AROS/Development
export PATH="/opt/qrx-os-one/aros64/build-qrx/bin/linux-x86_64/tools:$PATH"
export CC="/opt/qrx-os-one/aros64/tc/bin/clang --sysroot=$SDK" CXX="/opt/qrx-os-one/aros64/tc/bin/clang++ --sysroot=$SDK"
export AR=/opt/qrx-os-one/ec64llvm/build/bin/llvm-ar CC_host=gcc CXX_host=g++
# sockets through libqrxaroscompat, not bsdsocket inlines (see ../nspr/README.md)
export CFLAGS="-DBSDSOCKET_NOLIBDEFINES -D__BSDSOCKET_NOLIBBASE__ -DXP_UNIX -DAROS -fno-pic -fstack-protector-strong -Dalloca=__builtin_alloca -D_GNU_SOURCE -DNS_PTR_GT_32 -DSQLITE_OMIT_LOAD_EXTENSION -DSEED_ONLY_DEV_URANDOM -U__STRICT_ANSI__ -include $HERE/aros-nss.h"
export CXXFLAGS="$CFLAGS"
G="$FT/third_party/python/gyp"
cd "$NSS"
PYTHONPATH="$G/pylib" python3 "$G/gyp_main.py" -f ninja --depth=. --generator-output="$OUT" nss.gyp \
  -DOS=aros -Dtarget_arch=m68k64 -Dstatic_libs=1 -Ddisable_tests=1 -Ddisable_dbm=1 -Ddisable_libpkix=1 \
  -Dsign_libs=0 -Dcc_is_clang=1 -Dhave_int128_support=0 -Ddisable_werror=1 -Duse_system_sqlite=0 \
  -Duse_system_zlib=0 -Ddisable_fips=1 -Denable_sslkeylogfile=0 -Dpython=python3 \
  -Dnspr_include_dir="$NSPR/include" -Dnspr_lib_dir="$NSPR" \
  -Dnss_dist_dir="$OUT/dist" -Dnss_dist_obj_dir="$OUT/dist/Release"
ninja -C "$OUT/out/Release" "${@:5}"
