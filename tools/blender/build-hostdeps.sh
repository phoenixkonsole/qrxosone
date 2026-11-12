#!/bin/bash
# Python 3.5.10 and FreeType for the native Mac reference with interface (configure-host-gui.sh), into
# .build/blender-2.79b/hostdeps. Python comes from the archive build.sh downloads; FreeType from a tarball
# given as FREETYPE_TAR (2.12.1 was used; the guest's SDK has 2.14).
set -eu
W=${QRX_BLENDER_WORK:-/Users/pascalpapara/Documents/GitHub/qrxosone/.build/blender-2.79b}
D=$W/hostdeps
T=$(mktemp -d)
tar xJf "$W/dl/Python-3.5.10.tar.xz" -C "$T"
cd "$T/Python-3.5.10"
# Python 3.5 knows the Mac only as i386 or ppc.
sed -i '' 's/as_fn_error $? "Unexpected output of .arch. on OSX" "$LINENO" 5/MACOSX_DEFAULT_ARCH="arm64"/' configure
./configure --prefix="$D/python" --disable-shared --without-ensurepip --build=aarch64-apple-darwin CC=clang \
  CFLAGS="-w -Wno-implicit-function-declaration" ac_cv_header_libintl_h=no ac_cv_lib_intl_textdomain=no >/dev/null
make -j8 >/dev/null && make install >/dev/null
mkdir -p "$T/ft" && tar xJf "${FREETYPE_TAR:?set FREETYPE_TAR to a freetype-2.x.tar.xz}" -C "$T/ft" --strip-components=1
cd "$T/ft"
./configure --prefix="$D/freetype" --disable-shared --without-zlib --without-bzip2 --without-png --without-harfbuzz \
  --without-brotli CFLAGS="-O2 -w" >/dev/null
make -j8 >/dev/null && make install >/dev/null
rm -rf "$T"
echo "built: $D"
