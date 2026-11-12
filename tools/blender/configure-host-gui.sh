#!/bin/bash
# The Mac reference for the interface: the same Blender source, configuration and GHOST for AROS, natively
# on the Mac, with tools/blender/hostref in place of Intuition and the GL door. Needs build-hostdeps.sh first.
#   ninja -C .build/blender-2.79b/hostref-gui blender
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
W=${QRX_BLENDER_WORK:-/Users/pascalpapara/Documents/GitHub/qrxosone/.build/blender-2.79b}
D=$W/hostdeps
SDK=$(xcrun --show-sdk-path)
# GHOST's AROS sources, with the host stand-in as their Intuition part.
G=$W/hostref-ghost
mkdir -p "$G"
for f in "$HERE"/ghost/GHOST_*AROS* "$HERE/ghost/ghost_aros_native.h"; do ln -sf "$f" "$G/"; done
ln -sf "$HERE/hostref/ghost_host_native.c" "$G/ghost_aros_native.c"
mkdir -p "$W/hostref-gui"
clang -c -O2 -w -I"$HERE/../qrxgl68k/guest" "$HERE/hostref/qgl_host.c" -o "$W/hostref-gui/qgl_host.o"
OFF=$(sed -n '/^OFF="/,/"$/p' "$HERE/configure.sh" | sed 's/OFF="//; s/"$//')
A=(); for o in $OFF; do A+=("-D$o=OFF"); done
export PATH=/opt/homebrew/bin:$PATH
exec cmake -S "$W/src/blender-2.79b" -B "$W/hostref-gui" -G Ninja -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-w -ffp-contract=off -Wno-error -Wno-implicit-function-declaration -DQRX_GL_DOOR -DGL_SILENCE_DEPRECATION" \
  -DCMAKE_CXX_FLAGS="-w -ffp-contract=off -std=c++11 -DQRX_GL_DOOR" \
  -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,-undefined,dynamic_lookup $W/hostref-gui/qgl_host.o -framework OpenGL -framework CoreServices -framework CoreFoundation -framework SystemConfiguration" \
  -DFREETYPE_INCLUDE_DIRS=$D/freetype/include/freetype2 -DFREETYPE_INCLUDE_DIR_ft2build=$D/freetype/include/freetype2 \
  -DFREETYPE_INCLUDE_DIR_freetype2=$D/freetype/include/freetype2 -DFREETYPE_LIBRARY=$D/freetype/lib/libfreetype.a \
  -DJPEG_LIBRARY=/opt/homebrew/lib/libjpeg.a -DJPEG_INCLUDE_DIR=/opt/homebrew/include \
  -DPNG_LIBRARY=/opt/homebrew/lib/libpng.dylib -DPNG_PNG_INCLUDE_DIR=/opt/homebrew/include \
  -DZLIB_LIBRARY=$SDK/usr/lib/libz.tbd -DZLIB_INCLUDE_DIR=$SDK/usr/include \
  -DOPENGL_gl_LIBRARY=$SDK/usr/lib/libz.tbd -DOPENGL_glu_LIBRARY=$SDK/usr/lib/libz.tbd -DOPENGL_opengl_LIBRARY=$SDK/usr/lib/libz.tbd \
  -DOPENGL_glx_LIBRARY=$SDK/usr/lib/libz.tbd -DOPENGL_INCLUDE_DIR=$W/hostinc \
  -DWITH_PYTHON=ON -DPYTHON_VERSION=3.5 -DPYTHON_INCLUDE_DIR=$D/python/include/python3.5m -DPYTHON_INCLUDE_CONFIG_DIR=$D/python/include/python3.5m \
  -DPYTHON_LIBRARY=$D/python/lib/libpython3.5m.a -DPYTHON_LIBPATH=$D/python/lib -DPYTHON_LINKFLAGS= -DWITH_PYTHON_SAFETY=OFF \
  -DWITH_HEADLESS=OFF -DWITH_GHOST_AROS=ON -DQRX_GHOST_DIR="$G" -DQRX_GL_DIR="$HERE/../qrxgl68k/guest" \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DWITH_LEGACY_DEPSGRAPH=ON "${A[@]}" "$@"
