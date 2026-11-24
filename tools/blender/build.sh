#!/bin/bash
# Builds Blender 2.79b for QRX OS One. See README.md here and docs/BLENDER_AROS68K.md.
# Everything but these scripts and the patch stays in the untracked .build/blender-2.79b.
# On the Mac with Homebrew's m68k-elf-gcc 16; on Linux with m68k-linux-gnu-gcc 13 around the SDK of
# tools/qrx86/bootstrap-aros-sdk.py (.build/qrx-sdk-elf). Paths and tools: qrxenv.py.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
eval "$("$HERE/qrxenv.py" sh)"
ROOT=$QRX_ROOT
mkdir -p "$W/dl" "$W/src" "$W/tree"
digest() { python3 -c 'import hashlib,sys; print(hashlib.new(sys.argv[1], open(sys.argv[2],"rb").read()).hexdigest())' "$@"; }
TAR=$W/dl/blender-2.79b.tar.gz
[ -e "$TAR" ] || curl -sSfL -o "$TAR" https://download.blender.org/source/blender-2.79b.tar.gz
[ "$(digest md5 "$TAR")" = cef9a203857dc65076e05c41fc7a7d03 ] || { echo "wrong checksum: $TAR"; exit 1; }
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
  cp "$SDK_DIR/source/deadwood-AROS-5b5fd4c/tools/elf2hunk/elf2hunk.c" "$W/elf2hunk.c"
  patch -s "$W/elf2hunk.c" "$HERE/elf2hunk-pc32.diff"
  cc -O2 -w "$W/elf2hunk.c" -o "$W/elf2hunk"
fi
# The test tree: a copy of the system without what a batch job does not need.
[ -e "$W/tree/C" ] || rsync -a --exclude 'Demos/*' --exclude Emergency-Boot.adf --exclude 'Fonts/*' --exclude 'Development/*' \
  --exclude 'QRX/Wallpapers/*' --exclude 'QRX/x86/*' --exclude 'QRX/Windows/*' --exclude 'QRX/Linux/*' \
  "$SYSTEM_TREE/" "$W/tree/"
# An engine with the FPU fixes. Musashi is not tracked: link it from the main checkout in a worktree.
ENG=$REPO/upstream/armithlon/2.6/engine
[ -e "$ENG/third_party/Musashi" ] || { mkdir -p "$ENG/third_party"; ln -s "$ROOT/upstream/armithlon/2.6/engine/third_party/Musashi" "$ENG/third_party/Musashi"; }
if [ "$(uname)" = Darwin ]; then export DEVELOPER_DIR=/Library/Developer/CommandLineTools; fi
cmake -S "$ENG" -B "$W/engine-build" -DQRX_ENABLE_MUSASHI=ON -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE="$(command -v python3)" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
cmake --build "$W/engine-build" --target qrx68100_full_run -j 8 >/dev/null
cp "$W/engine-build/qrx68100_full_run" "$W/engine-snapshot"
# Two rounds: the bridges look at what the libraries define, and need them built once.
"$HERE/build_port.py" msun
# libstdc++ of the compiler's own version, without exceptions.
[ -n "$GCC_URL" ] || { echo "no pinned libstdc++ source for GCC $GCC_VERSION (qrxenv.py LIBSTDCXX)"; exit 1; }
GCC=$W/dl/gcc-$GCC_VERSION.tar.xz
G=gcc-$GCC_VERSION
[ -e "$GCC" ] || curl -sSfL -o "$GCC" "$GCC_URL"
[ "$(digest sha256 "$GCC")" = "$GCC_SHA256" ] || { echo "wrong checksum: $GCC"; exit 1; }
if [ ! -e "$W/gcc/build/src/.libs/libstdc++.a" ]; then
  mkdir -p "$W/gcc/build"
  tar xJf "$GCC" -C "$W/gcc" $G/libstdc++-v3 $G/libgcc $G/config $G/config.sub $G/config.guess \
    $G/install-sh $G/ltmain.sh $G/missing $G/mkinstalldirs $G/include $G/libtool.m4 \
    $G/gcc/BASE-VER $G/gcc/DATESTAMP $G/gcc/DEV-PHASE $G/move-if-change $G/config-ml.in $G/libiberty
  # fixes for a build without threads, where the release needs them (patches/libstdc++-<version>-qrx.patch)
  P=$HERE/patches/libstdc++-$GCC_VERSION-qrx.patch
  [ ! -e "$P" ] || patch -s -d "$W/gcc/$G" -p1 < "$P"
  "$HERE/build_libstdcxx.sh"
  # GCC 16 stops at the C++ module files of libstdc++, which this build does not use.
  QRX_CXX_BARE=1 make -C "$W/gcc/build" -k -j8 || true
  [ -e "$W/gcc/build/src/.libs/libstdc++.a" ] || { echo "libstdc++ did not build"; exit 1; }
fi
# GL for 68k programs through the ARMithlon door: Blender links it in place of libGL.
"$REPO/tools/qrxgl68k/build_guest.sh" "$W/port" >/dev/null
"$HERE/configure.sh" >/dev/null
ninja -C "$W/build" -j 8 blender || true
"$HERE/build_port.py"
ninja -C "$W/build" -j 8 blender
"$W/elf2hunk" "$W/build/bin/blender" "$W/tree/blender"
chmod +x "$W/tree/blender"
echo "built: $W/tree/blender"
