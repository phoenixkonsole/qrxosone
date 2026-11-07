#!/bin/bash
# Native build of the same configuration on the Mac, as a little endian 64-bit comparison for the 68k build
# when the official Intel build cannot run (no Rosetta). FreeType and JPEG stay unresolved: `-b` to PNG calls
# neither, apart from FT_Init_FreeType, see port/host_stubs.c.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
W=${QRX_BLENDER_WORK:-/Users/pascalpapara/Documents/GitHub/qrxosone/.build/blender-2.79b}
OFF=$(sed -n '/^OFF="/,/"$/p' "$HERE/configure.sh" | sed 's/OFF="//; s/"$//')
A=(); for o in $OFF; do A+=("-D$o=OFF"); done
export PATH=/opt/homebrew/bin:$PATH
exec cmake -S "$W/src/blender-2.79b" -B "$W/hostref" -G Ninja -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-w -ffp-contract=off -Wno-error -Wno-implicit-function-declaration" -DCMAKE_CXX_FLAGS="-w -ffp-contract=off -std=c++11" \
  -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,-undefined,dynamic_lookup -framework CoreServices -framework CoreFoundation" \
  -DFREETYPE_INCLUDE_DIRS=$W/hostinc/freetype -DFREETYPE_INCLUDE_DIR_ft2build=$W/hostinc \
  -DFREETYPE_INCLUDE_DIR_freetype2=$W/hostinc/freetype -DFREETYPE_LIBRARY=$(xcrun --show-sdk-path)/usr/lib/libz.tbd \
  -DJPEG_LIBRARY=$(xcrun --show-sdk-path)/usr/lib/libz.tbd -DJPEG_INCLUDE_DIR=$W/hostinc \
  -DPNG_LIBRARY=/opt/homebrew/lib/libpng.dylib -DPNG_PNG_INCLUDE_DIR=/opt/homebrew/include \
  -DZLIB_LIBRARY=$(xcrun --show-sdk-path)/usr/lib/libz.tbd -DZLIB_INCLUDE_DIR=$(xcrun --show-sdk-path)/usr/include \
  -DOPENGL_gl_LIBRARY=$(xcrun --show-sdk-path)/usr/lib/libz.tbd -DOPENGL_glu_LIBRARY=$(xcrun --show-sdk-path)/usr/lib/libz.tbd -DOPENGL_opengl_LIBRARY=$(xcrun --show-sdk-path)/usr/lib/libz.tbd \
  -DOPENGL_glx_LIBRARY=$(xcrun --show-sdk-path)/usr/lib/libz.tbd -DOPENGL_INCLUDE_DIR=$W/hostinc \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DWITH_HEADLESS=ON -DWITH_LEGACY_DEPSGRAPH=ON "${A[@]}" "$@"
