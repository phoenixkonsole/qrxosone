#!/bin/bash
# Builds libstdc++ of the compiler's own GCC version (16.2 on the Mac, 13.3 with m68k-linux-gnu) for the AROS SDK, without exceptions, RTTI or
# threads. configure may not link in a cross build, so what it wants to link is answered here.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
eval "$("$HERE/qrxenv.py" sh)"
B=$W/gcc/build
export QRX_CXX_BARE=1
ANSWERS=${ANSWERS:-"ac_cv_func_fcntl=yes ac_cv_func_getexecname=no glibcxx_cv_LFS=no"}
cd "$B" || exit 1
for round in $(seq 1 40); do
  rm -f config.cache
  env $ANSWERS CC="$HERE/aros_cc.py" CXX="$HERE/aros-c++" CPP="$HERE/aros_cc.py -E" CXXCPP="$HERE/aros-c++ -E" \
    AR=$XAR RANLIB=$XRANLIB NM=$XNM AS=$XAS LD=$XLD \
    CFLAGS="-O2" CXXFLAGS="-O2 -fno-exceptions -fno-rtti" \
    "$W/gcc/gcc-$GCC_VERSION/libstdc++-v3/configure" --host=m68k-elf --build=$BUILD_TRIPLE --target=m68k-elf --with-newlib \
    --disable-shared --disable-multilib --disable-libstdcxx-pch --disable-libstdcxx-verbose --disable-nls \
    --disable-libstdcxx-threads --disable-tls --disable-libstdcxx-filesystem-ts --disable-libstdcxx-backtrace --disable-werror \
    --with-gxx-include-dir="$W/gcc/install/include/c++" --prefix="$W/gcc/install" > ../configure.log 2>&1
  rc=$?
  if [ $rc = 0 ]; then
    # The newlib files assume newlib's ctype table; AROS has its own, so use the generic ones.
    G=$W/gcc/gcc-$GCC_VERSION/libstdc++-v3/config/os/generic
    ln -sf "$G/ctype_base.h" include/m68k-elf/bits/ctype_base.h
    ln -sf "$G/ctype_inline.h" include/m68k-elf/bits/ctype_inline.h
    ln -sf "$G/ctype_configure_char.cc" src/c++11/ctype_configure_char.cc
    # configure tests strerror_r as C with -Werror, which the driver's -w defeats; the SDK keeps it out of
    # view (aros/stdc/string.h), so libstdc++ uses strerror
    sed -i.bak 's|^#define HAVE_STRERROR_R 1|/* #undef HAVE_STRERROR_R (hidden in the SDK) */|' config.h
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
