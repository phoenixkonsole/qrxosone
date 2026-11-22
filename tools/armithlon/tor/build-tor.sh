#!/usr/bin/env bash
# Builds tor for AROS m68k from the source archives in $QRX_TOR_DL (see README.md).
# Usage: build-tor.sh <work dir> [sources|shim|libevent|openssl|tor|tests|all]
# The archives (see README.md) go in <work dir>/dl.
set -Eeuo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:?work dir}"; STEP="${2:-all}"
mkdir -p "$WORK"; WORK="$(cd "$WORK" && pwd)"
export QRX_SDK_BIN="${QRX_SDK_BIN:-$HERE/../../../.build/qrx-sdk/bin}"
export QRX_TOR_OUT="$WORK/out"
SRC="$WORK/src"; PREFIX="$WORK/out/prefix"; JOBS="${QRX_BUILD_JOBS:-8}"
CC="$HERE/qrx-tor-cc"; AR=m68k-elf-ar; RANLIB=m68k-elf-ranlib
# Implicit declarations stay visible: on this ABI one that returns a pointer reads the wrong
# register. audit-implicit lists them.
OPT="-O2 -fno-strict-aliasing -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-volatile-register-var"
mkdir -p "$PREFIX/lib" "$PREFIX/include" "$QRX_TOR_OUT/shim"

sources() {
  cd "$WORK/dl"
  shasum -a 256 -c "$HERE/patches/SHA256SUMS"
  rm -rf "$SRC"; mkdir -p "$SRC"
  for a in tor-0.4.9.9 libevent-2.1.12-stable openssl-3.5.4; do tar xzf "$a.tar.gz" -C "$SRC"; done
  patch -p1 -d "$SRC/tor-0.4.9.9" <"$HERE/patches/tor-0.4.9.9-aros.patch"
}
shim() {
  python3 "$HERE/shim/gen-bridges.py" "$QRX_SDK_BIN/toolchain.json" "$QRX_TOR_OUT/shim/bridges" >/dev/null
  for f in qrx_posix qrx_alloc; do
    "$CC" -O2 -Wall -Wno-unused-parameter -Wno-volatile-register-var -c "$HERE/shim/$f.c" -o "$QRX_TOR_OUT/shim/$f.o"
  done
  rm -f "$QRX_TOR_OUT/shim/libqrxposix.a"
  "$AR" rcs "$QRX_TOR_OUT/shim/libqrxposix.a" "$QRX_TOR_OUT/shim/qrx_posix.o" "$QRX_TOR_OUT/shim/qrx_alloc.o"
}
# Needs tor's configured headers.
core1() {
  "$CC" -O2 -fno-strict-aliasing -fno-builtin -Wno-volatile-register-var -I"$WORK/b-tor" -I"$SRC/tor-0.4.9.9" -I"$SRC/tor-0.4.9.9/src" \
    -c "$HERE/shim/qrx_core1.c" -o "$QRX_TOR_OUT/shim/qrx_core1.o"
  "$AR" rs "$QRX_TOR_OUT/shim/libqrxposix.a" "$QRX_TOR_OUT/shim/qrx_core1.o" 2>/dev/null
}
libevent() {
  rm -rf "$WORK/b-libevent"; mkdir -p "$WORK/b-libevent"; cd "$WORK/b-libevent"
  CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS="$OPT" \
  ac_cv_func_epoll_ctl=no ac_cv_func_kqueue=no ac_cv_func_poll=no ac_cv_header_poll_h=no ac_cv_func_eventfd=no \
  ac_cv_func_pipe=no ac_cv_func_pipe2=no ac_cv_func_getaddrinfo=no ac_cv_func_getnameinfo=no ac_cv_func_sendfile=no \
  ac_cv_func_mmap=no ac_cv_header_sys_mman_h=no ac_cv_func_readv=no ac_cv_func_writev=no ac_cv_header_sys_uio_h=no \
  ac_cv_func_getifaddrs=no ac_cv_func_inet_ntop=no ac_cv_func_inet_pton=no ac_cv_func_issetugid=no \
  ac_cv_func_arc4random=yes ac_cv_func_arc4random_buf=yes ac_cv_func_arc4random_addrandom=no \
  "$SRC/libevent-2.1.12-stable/configure" --host=m68k-aros --prefix="$PREFIX" --disable-shared --enable-static \
    --disable-openssl --disable-samples --disable-libevent-regress --disable-debug-mode \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
  make -j"$JOBS" >make.log 2>&1 || { tail -40 make.log; exit 1; }
  make install >>make.log 2>&1
}
openssl() {
  rm -rf "$WORK/b-openssl"; cp -R "$SRC/openssl-3.5.4" "$WORK/b-openssl"; cd "$WORK/b-openssl"
  CC="$CC" AR="$AR" RANLIB="$RANLIB" perl ./Configure BSD-generic32 --prefix="$PREFIX" --libdir=lib \
    --with-rand-seed=getrandom \
    no-asm no-pic no-secure-memory no-shared no-dso no-module no-apps no-tests no-docs no-engine no-legacy no-quic no-comp no-afalgeng \
    no-async no-ssl3 no-tls1 no-tls1_1 no-dtls no-dgram no-srp no-gost no-idea no-seed no-mdc2 no-rc2 no-rc4 no-rc5 \
    no-cmp no-cms no-ts no-ocsp no-ct no-ui-console no-autoload-config \
    -DOPENSSL_RAND_SEED_GETRANDOM -D__USE_MISC $OPT >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
  make -k -j"$JOBS" build_libs >make.log 2>&1 || { tail -40 make.log; exit 1; }
  make install_dev >>make.log 2>&1
}
tor() {
  rm -rf "$WORK/b-tor"; mkdir -p "$WORK/b-tor"; cd "$WORK/b-tor"
  CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS="$OPT" CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" \
  ac_cv_func_getaddrinfo=no ac_cv_func_getifaddrs=no ac_cv_func_mlockall=no ac_cv_func_pipe=no ac_cv_func_pipe2=no \
  ac_cv_func_eventfd=no ac_cv_func_fork=no ac_cv_func_getrandom=no ac_cv_func_getentropy=yes \
  ac_cv_func_mmap=yes ac_cv_header_sys_mman_h=yes ac_cv_func_accept4=no ac_cv_func_prctl=no \
  ac_cv_func_vasprintf=no ac_cv_func_sigaltstack=no ac_cv_func_flock=yes ac_cv_func_lockf=no \
  "$SRC/tor-0.4.9.9/configure" --host=m68k-aros --prefix="$PREFIX" \
    --enable-static-libevent --with-libevent-dir="$PREFIX" --enable-static-openssl --with-openssl-dir="$PREFIX" \
    --disable-asciidoc --disable-manpage --disable-html-manual --disable-unittests --disable-module-relay \
    --disable-module-dirauth --disable-tool-name-check --disable-lzma --disable-zstd --disable-systemd \
    --disable-gcc-hardening --disable-linker-hardening --disable-seccomp --disable-libscrypt \
    >configure.log 2>&1 || { tail -40 configure.log; exit 1; }
  core1
  make -k -j"$JOBS" src/app/tor >make.log 2>&1 || { tail -60 make.log; exit 1; }
}
# tor's own test program, to run in the guest with --no-fork.
tests() {
  rm -rf "$WORK/b-tests"; mkdir -p "$WORK/b-tests"; cd "$WORK/b-tests"
  CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS="$OPT" CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" \
  ac_cv_func_getaddrinfo=no ac_cv_func_getifaddrs=no ac_cv_func_mlockall=no ac_cv_func_pipe=no ac_cv_func_pipe2=no \
  ac_cv_func_eventfd=no ac_cv_func_fork=no ac_cv_func_getrandom=no ac_cv_func_getentropy=yes \
  ac_cv_func_mmap=yes ac_cv_header_sys_mman_h=yes ac_cv_func_accept4=no ac_cv_func_prctl=no \
  ac_cv_func_vasprintf=no ac_cv_func_sigaltstack=no ac_cv_func_flock=yes ac_cv_func_lockf=no \
  "$SRC/tor-0.4.9.9/configure" --host=m68k-aros --prefix="$PREFIX" \
    --enable-static-libevent --with-libevent-dir="$PREFIX" --enable-static-openssl --with-openssl-dir="$PREFIX" \
    --disable-asciidoc --disable-manpage --disable-html-manual --enable-unittests --disable-module-relay --disable-module-dirauth --disable-module-pow \
    --disable-tool-name-check --disable-lzma --disable-zstd --disable-systemd \
    --disable-gcc-hardening --disable-linker-hardening --disable-seccomp --disable-libscrypt \
    >configure.log 2>&1 || { tail -20 configure.log; exit 1; }
  : >micro-revision.i
  make -k -j"$JOBS" src/test/test >make.log 2>&1 || { grep -E ' error:|^ +U ' make.log | sort -u | head -40; exit 1; }
}
case "$STEP" in
  all) sources; shim; libevent; openssl; tor ;;
  *) "$STEP" ;;
esac
