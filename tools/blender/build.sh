#!/bin/bash
# Builds Blender 2.79b for QRX OS One, first stage. See README.md here and docs/BLENDER_AROS68K.md.
# Everything but these scripts and the patch stays in the untracked .build/blender-2.79b.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
ROOT=${QRX_ROOT:-/Users/pascalpapara/Documents/GitHub/qrxosone}
W=$ROOT/.build/blender-2.79b
export PATH=/opt/homebrew/bin:$PATH
mkdir -p "$W/dl" "$W/src" "$W/tree"
TAR=$W/dl/blender-2.79b.tar.gz
[ -e "$TAR" ] || curl -sSfL -o "$TAR" https://download.blender.org/source/blender-2.79b.tar.gz
[ "$(md5 -q "$TAR")" = cef9a203857dc65076e05c41fc7a7d03 ] || { echo "wrong checksum: $TAR"; exit 1; }
if [ ! -d "$W/src/blender-2.79b" ]; then
  tar xzf "$TAR" -C "$W/src"
  (cd "$W/src/blender-2.79b" && git init -q . && git apply --whitespace=nowarn "$HERE/patches/blender-2.79b-qrx.patch")
fi
# The AROS copy of msun, and elf2hunk that can describe a large program.
if [ ! -d "$W/msun/math" ]; then
  mkdir -p "$W/msun"
  tar xJf "$REPO/upstream/aros-deadwood/source/deadwood-AROS-master-5b5fd4cfd4f0.tar.xz" -C "$W/msun" \
    --strip-components=4 deadwood-AROS-5b5fd4c/compiler/crt/stdc/math
fi
if [ ! -x "$W/elf2hunk" ]; then
  cp "$ROOT/.build/qrx-sdk/source/deadwood-AROS-5b5fd4c/tools/elf2hunk/elf2hunk.c" "$W/elf2hunk.c"
  patch -s "$W/elf2hunk.c" "$HERE/elf2hunk-pc32.diff"
  cc -O2 -w "$W/elf2hunk.c" -o "$W/elf2hunk"
fi
# The test tree: a copy of the system without what a batch job does not need.
[ -e "$W/tree/C" ] || rsync -a --exclude 'Demos/*' --exclude Emergency-Boot.adf --exclude 'Fonts/*' --exclude 'Development/*' \
  --exclude 'QRX/Wallpapers/*' --exclude 'QRX/x86/*' --exclude 'QRX/Windows/*' --exclude 'QRX/Linux/*' \
  "$ROOT/.build/bundle/SystemFull/" "$W/tree/"
# An engine with the FPU fixes. Musashi is not tracked: link it from the main checkout in a worktree.
ENG=$REPO/upstream/armithlon/2.6/engine
[ -e "$ENG/third_party/Musashi" ] || { mkdir -p "$ENG/third_party"; ln -s "$ROOT/upstream/armithlon/2.6/engine/third_party/Musashi" "$ENG/third_party/Musashi"; }
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake -S "$ENG" -B "$W/engine-build" -DQRX_ENABLE_MUSASHI=ON -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/opt/homebrew/bin/python3 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build "$W/engine-build" --target qrx68100_full_run -j 8 >/dev/null
cp "$W/engine-build/qrx68100_full_run" "$W/engine-snapshot"
# Two rounds: the bridges look at what the libraries define, and need them built once.
"$HERE/build_port.py" msun
# libstdc++ of the compiler's own version, without exceptions.
GCC=$W/dl/gcc-16.2.0.tar.xz
[ -e "$GCC" ] || curl -sSfL -o "$GCC" https://ftp.gnu.org/gnu/gcc/gcc-16.2.0/gcc-16.2.0.tar.xz
[ "$(shasum -a 256 "$GCC" | cut -d' ' -f1)" = e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e ] || { echo "wrong checksum: $GCC"; exit 1; }
if [ ! -e "$W/gcc/build/src/.libs/libstdc++.a" ]; then
  mkdir -p "$W/gcc/build"
  tar xJf "$GCC" -C "$W/gcc" gcc-16.2.0/libstdc++-v3 gcc-16.2.0/libgcc gcc-16.2.0/config gcc-16.2.0/config.sub gcc-16.2.0/config.guess \
    gcc-16.2.0/install-sh gcc-16.2.0/ltmain.sh gcc-16.2.0/missing gcc-16.2.0/mkinstalldirs gcc-16.2.0/include gcc-16.2.0/libtool.m4 \
    gcc-16.2.0/gcc/BASE-VER gcc-16.2.0/gcc/DATESTAMP gcc-16.2.0/gcc/DEV-PHASE gcc-16.2.0/move-if-change gcc-16.2.0/config-ml.in gcc-16.2.0/libiberty
  "$HERE/build_libstdcxx.sh"
  # Stops at the C++ module files of libstdc++, which this build does not use.
  QRX_CXX_BARE=1 make -C "$W/gcc/build" -k -j8 || true
  [ -e "$W/gcc/build/src/.libs/libstdc++.a" ] || { echo "libstdc++ did not build"; exit 1; }
fi
"$HERE/configure.sh" >/dev/null
ninja -C "$W/build" -j 8 blender || true
"$HERE/build_port.py"
ninja -C "$W/build" -j 8 blender
"$W/elf2hunk" "$W/build/bin/blender" "$W/tree/blender"
chmod +x "$W/tree/blender"
echo "built: $W/tree/blender"
