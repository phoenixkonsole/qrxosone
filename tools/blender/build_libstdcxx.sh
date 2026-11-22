#!/bin/bash
# Builds libstdc++ of GCC 16.2 (the compiler's own version) for the AROS SDK, without exceptions, RTTI or
# threads. configure may not link in a cross build, so what it wants to link is answered here.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
W=${QRX_BLENDER_WORK:-/Users/pascalpapara/Documents/GitHub/qrxosone/.build/blender-2.79b}
B=$W/gcc/build
BIN=/opt/homebrew/bin
export QRX_CXX_BARE=1 PATH=$BIN:$PATH
ANSWERS=${ANSWERS:-"ac_cv_func_fcntl=yes ac_cv_func_getexecname=no glibcxx_cv_LFS=no"}
cd "$B" || exit 1
for round in $(seq 1 40); do
  rm -f config.cache
  env $ANSWERS CC="$HERE/aros_cc.py" CXX="$HERE/aros-c++" CPP="$HERE/aros_cc.py -E" CXXCPP="$HERE/aros-c++ -E" \
    AR=$BIN/m68k-elf-ar RANLIB=$BIN/m68k-elf-ranlib NM=$BIN/m68k-elf-nm AS=$BIN/m68k-elf-as LD=$BIN/m68k-elf-ld \
    CFLAGS="-O2" CXXFLAGS="-O2 -fno-exceptions -fno-rtti" \
    "$W/gcc/gcc-16.2.0/libstdc++-v3/configure" --host=m68k-elf --build=aarch64-apple-darwin --target=m68k-elf --with-newlib \
    --disable-shared --disable-multilib --disable-libstdcxx-pch --disable-libstdcxx-verbose --disable-nls \
    --disable-libstdcxx-threads --disable-tls --disable-libstdcxx-filesystem-ts --disable-libstdcxx-backtrace --disable-werror \
    --with-gxx-include-dir="$W/gcc/install/include/c++" --prefix="$W/gcc/install" > ../configure.log 2>&1
  rc=$?
  if [ $rc = 0 ]; then
    # The newlib files assume newlib's ctype table; AROS has its own, so use the generic ones.
    G=$W/gcc/gcc-16.2.0/libstdc++-v3/config/os/generic
    ln -sf "$G/ctype_base.h" include/m68k-elf/bits/ctype_base.h
    ln -sf "$G/ctype_inline.h" include/m68k-elf/bits/ctype_inline.h
    ln -sf "$G/ctype_configure_char.cc" src/c++11/ctype_configure_char.cc
    echo "configure ok after $round rounds: $ANSWERS"
    exit 0
  fi
  last=$(grep -E '^checking ' ../configure.log | tail -1)
  fn=$(echo "$last" | sed -nE 's/^checking for ([A-Za-z_0-9]+)\.\.\..*/\1/p')
  echo "round $round: $(tail -1 ../configure.log | cut -c1-110)"
  [ -n "$fn" ] || { echo "stuck at: $last"; exit 1; }
  case " $ANSWERS " in *"ac_cv_func_$fn="*) echo "again $fn"; exit 1;; esac
  ANSWERS="$ANSWERS ac_cv_func_$fn=no"
done
