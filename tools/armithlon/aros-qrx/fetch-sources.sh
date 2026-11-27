#!/bin/bash
# Puts the third-party archives AROS' build fetches into WORK/build-qrx/bin/Sources from places a
# restricted network reaches: the Ubuntu archive (the upstream tarballs as Debian keeps them), git
# clones of release tags (GitHub's archive and release downloads, freetype's and libpng's), the Mesa
# archive and Chromium's googlesource. Each is the version deadwood's AROS 5b5fd4c names.
#   fetch-sources.sh WORK
# zlib is Chromium's at 85f05b0 (2025-09-18): zlib-aros.diff is from that day, a newer lkgr does
# not take it.
set -Eeuo pipefail
S="$(cd "$1" && pwd)/build-qrx/bin/Sources"; mkdir -p "$S/codesets" "$S/utf8proc"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
U=http://archive.ubuntu.com/ubuntu/pool
get() { [ -s "$S/$1" ] || curl -sfL -o "$S/$1" "$2"; }
get bzip2-1.0.8.tar.gz $U/main/b/bzip2/bzip2_1.0.8.orig.tar.gz
get glu-9.0.2.tar.gz $U/main/libg/libglu/libglu_9.0.2.orig.tar.gz
get jpegsrc.v9f.tar.gz $U/universe/libj/libjpeg9/libjpeg9_9f.orig.tar.gz
get libwebp-1.5.0.tar.gz $U/main/libw/libwebp/libwebp_1.5.0.orig.tar.gz
get acpica-unix-20251212.tar.gz https://downloadmirror.intel.com/871486/acpica-unix-20251212.tar.gz
get mesa-20.0.8.tar.xz https://archive.mesa3d.org/older-versions/20.x/mesa-20.0.8.tar.xz
[ -s "$S/xz-5.8.1.tar.gz" ] || { curl -sfL $U/main/x/xz-utils/xz-utils_5.8.1.orig.tar.xz | xz -dc | gzip > "$S/xz-5.8.1.tar.gz"; }
if [ ! -s "$S/tiff-4.7.1.tar.xz" ]; then   # Debian's has libtiff-v4.7.1/ at the top
  curl -sfL $U/main/t/tiff/tiff_4.7.1.orig.tar.bz2 | tar -xj -C "$T"
  mv "$T/libtiff-v4.7.1" "$T/tiff-4.7.1"; tar -cJf "$S/tiff-4.7.1.tar.xz" -C "$T" tiff-4.7.1
fi
tag() {  # tag(repo, tag, prefix, out)
  [ -s "$4" ] && return
  git clone -q --depth 1 --branch "$2" "$1" "$T/g" 2>/dev/null
  git -C "$T/g" archive --format=tar.gz --prefix="$3/" HEAD > "$4"; rm -rf "$T/g"
}
tag https://github.com/jens-maus/libcodesets.git 6.22 libcodesets-6.22 "$S/codesets/6.22.tar.gz"
tag https://github.com/JuliaStrings/utf8proc.git v2.11.0 utf8proc-2.11.0 "$S/utf8proc/v2.11.0.tar.gz"
tag https://github.com/freetype/freetype.git VER-2-14-1 freetype-2.14.1 "$S/freetype-2.14.1.tar.gz"
tag https://github.com/pnggroup/libpng.git v1.6.48 libpng-1.6.48 "$S/libpng-1.6.48.tar.gz"
if [ ! -s "$S/zlib.tar.gz" ]; then
  git clone -q https://chromium.googlesource.com/chromium/src/third_party/zlib "$T/z"
  git -C "$T/z" archive --format=tar.gz 85f05b0 > "$S/zlib.tar.gz"
fi
# a fetch that was stopped leaves its lock behind, and the next one waits on it for ever
find "$S" -name '*.fetch' -delete
echo "sources in $S"
