#!/bin/bash
# CPython 3.5.10 for QRX OS One: download, patch, configure, build. See README.md.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
eval "$("$HERE/qrxenv.py" sh)"
mkdir -p "$W/dl" "$W/py/build" "$W/py/fake"
TAR=$W/dl/Python-3.5.10.tar.xz
[ -e "$TAR" ] || curl -sSfL -o "$TAR" https://www.python.org/ftp/python/3.5.10/Python-3.5.10.tar.xz
[ "$(python3 -c 'import hashlib,sys; print(hashlib.md5(open(sys.argv[1],"rb").read()).hexdigest())' "$TAR")" = 75c9c268703654aa6f6f2ae67303dde4 ] || { echo "unexpected file: $TAR"; exit 1; }
if [ ! -d "$W/py/Python-3.5.10" ]; then
  tar xJf "$TAR" -C "$W/py"
  (cd "$W/py/Python-3.5.10" && git init -q . && git apply --whitespace=nowarn "$HERE/patches/python-3.5.10-qrx.patch")
fi
# configure asks for a build Python of the same version; this build never runs it.
printf '#!/bin/sh\nexit 0\n' > "$W/py/fake/python3.5"; chmod +x "$W/py/fake/python3.5"
if [ ! -e "$W/py/build/pyconfig.h" ]; then
  (cd "$W/py/build" && PATH=$W/py/fake:$PATH CC="$HERE/aros_cc.py" CPP="$HERE/aros_cc.py -E" CFLAGS="-O2 -fwrapv" \
    AR=$XAR RANLIB=$XRANLIB READELF=$XREADELF \
    ac_cv_file__dev_ptmx=no ac_cv_file__dev_ptc=no ac_cv_buggy_getaddrinfo=no \
    "$W/py/Python-3.5.10/configure" --host=m68k-linux --build=$BUILD_TRIPLE --without-ensurepip --disable-shared \
    --with-threads --without-pymalloc --disable-ipv6 > configure.log)
fi
"$HERE/build_python.py"
