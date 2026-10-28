#!/usr/bin/env bash
# QRX OS One 22.5.11 - offline/proxy-safe prefetch of every third-party archive the
# Deadwood AROS amiga-m68k toolchain (option 11) and core (option 12) build fetches.
# Sources used: Ubuntu archive (upstream .orig tarballs), GitHub release assets and
# plain git clones of upstream repositories.  Files are placed into PORTS_DIR with the
# exact names/top-level directories AROS' fetch.sh/fetch_cached expects, so the build
# never needs ftp.gnu.org, unicode.org, mesa3d.org, sourceforge, ijg.org, ... .
set -Eeuo pipefail
PORTS="${1:-/opt/qrx-os-one/deadwood-aros-build/portssources}"
DL="${QRX_PREFETCH_CACHE:-/opt/qrx-os-one/dl}"
U=http://archive.ubuntu.com/ubuntu/pool
OU=http://old-releases.ubuntu.com/ubuntu/pool
GH=https://github.com
mkdir -p "$PORTS" "$DL"
W="$(mktemp -d "$DL/prefetch.XXXXXX")"; trap 'rm -rf "$W"' EXIT
get(){ [[ -s "$DL/$(basename "$1")" ]] || curl -fsSL -o "$DL/$(basename "$1")" "$1"; echo "$DL/$(basename "$1")"; }
have(){ [[ -s "$PORTS/$1" ]] && echo "present $1"; }
# repack <src-archive> <old-topdir> <new-topdir> <dest> (tar.gz|tar.xz|tar.bz2 decided by dest suffix)
repack(){ local src="$1" old="$2" new="$3" dst="$4" d; d="$(mktemp -d "$W/r.XXXX")"; tar xf "$src" -C "$d"; [[ "$old" == "$new" ]] || mv "$d/$old" "$d/$new"
  case "$dst" in *.tar.gz|*.tgz) tar czf "$PORTS/$dst" -C "$d" "$new";; *.tar.xz) tar cJf "$PORTS/$dst" -C "$d" "$new";; *.tar.bz2) tar cjf "$PORTS/$dst" -C "$d" "$new";; esac; echo "staged $dst"; }
gittar(){ local repo="$1" ref="$2" top="$3" dst="$4" d; d="$(mktemp -d "$W/g.XXXX")"; git -c advice.detachedHead=false clone -q --depth 1 -b "$ref" "$GH/$repo" "$d/$top"; rm -rf "$d/$top/.git"
  mkdir -p "$(dirname "$PORTS/$dst")"; case "$dst" in *.zip) (cd "$d" && zip -qr "$PORTS/$dst" "$top");; *) tar czf "$PORTS/$dst" -C "$d" "$top";; esac; echo "staged $dst ($repo@$ref)"; }

# --- toolchain (gcc 10.5.0 / binutils 2.32 are the m68k defaults in config/*_def) ---
have gcc-10.5.0.tar.xz || { tar xzf "$(get $U/main/g/gcc-10/gcc-10_10.5.0.orig.tar.gz)" -C "$W" gcc-10-10.5.0/gcc-10.5.0.tar.xz; mv "$W/gcc-10-10.5.0/gcc-10.5.0.tar.xz" "$PORTS/"; echo staged gcc; }
have binutils-2.32.tar.gz || repack "$(get $OU/main/b/binutils/binutils_2.32.orig.tar.xz)" binutils-2.32 binutils-2.32 binutils-2.32.tar.gz
if ! have gmp-6.3.0.tar.gz; then d="$(mktemp -d "$W/gmp.XXXX")"; tar xJf "$(get $U/main/g/gmp/gmp_6.3.0+dfsg.orig.tar.xz)" -C "$d"; mv "$d/gmp-6.3.0+dfsg" "$d/gmp-6.3.0"
  # Debian's DFSG tarball drops doc/; configure still wants doc/Makefile.in.
  mkdir -p "$d/gmp-6.3.0/doc"; printf 'all install install-data install-exec uninstall clean distclean mostlyclean maintainer-clean check installcheck info dvi pdf html ps install-info install-strip install-pdf install-html install-dvi install-ps tags ctags distdir:\n\t@:\n' > "$d/gmp-6.3.0/doc/Makefile.in"
  tar czf "$PORTS/gmp-6.3.0.tar.gz" -C "$d" gmp-6.3.0; echo staged gmp; fi
have mpfr-4.2.2.tar.gz || repack "$(get $U/main/m/mpfr4/mpfr4_4.2.2.orig.tar.xz)" mpfr-4.2.2 mpfr-4.2.2 mpfr-4.2.2.tar.gz
have mpc-1.3.1.tar.gz || cp "$(get $U/main/m/mpclib3/mpclib3_1.3.1.orig.tar.gz)" "$PORTS/mpc-1.3.1.tar.gz"
have isl-0.27.tar.gz || repack "$(get $U/main/i/isl/isl_0.27.orig.tar.xz)" isl-0.27 isl-0.27 isl-0.27.tar.gz
have UnicodeData.txt || { [[ -f /usr/share/unicode/UnicodeData.txt ]] || DEBIAN_FRONTEND=noninteractive apt-get install -y unicode-data >/dev/null; cp /usr/share/unicode/UnicodeData.txt /usr/share/unicode/SpecialCasing.txt "$PORTS/"; }
# --- libraries pulled by the toolchain/core build ---
have zstd-1.5.7.tar.gz || cp "$(get $GH/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz)" "$PORTS/"
have glu-9.0.2.tar.gz || cp "$(get $U/main/libg/libglu/libglu_9.0.2.orig.tar.gz)" "$PORTS/glu-9.0.2.tar.gz"
have codesets/6.22.tar.gz || gittar jens-maus/libcodesets 6.22 libcodesets-6.22 codesets/6.22.tar.gz
have utf8proc/v2.11.0.tar.gz || gittar JuliaStrings/utf8proc v2.11.0 utf8proc-2.11.0 utf8proc/v2.11.0.tar.gz
have libpng-1.6.48.tar.gz || gittar pnggroup/libpng v1.6.48 libpng-1.6.48 libpng-1.6.48.tar.gz
have expat-2.7.3.tar.bz2 || cp "$(get $GH/libexpat/libexpat/releases/download/R_2_7_3/expat-2.7.3.tar.bz2)" "$PORTS/"
have jpegsrc.v9f.tar.gz || repack "$(get $U/universe/libj/libjpeg9/libjpeg9_9f.orig.tar.gz)" jpeg-9f jpeg-9f jpegsrc.v9f.tar.gz
have freetype-2.14.1.tar.gz || gittar freetype/freetype VER-2-14-1 freetype-2.14.1 freetype-2.14.1.tar.gz
have xz-5.8.1.tar.gz || cp "$(get $GH/tukaani-project/xz/releases/download/v5.8.1/xz-5.8.1.tar.gz)" "$PORTS/"
have tiff-4.7.1.tar.xz || repack "$(get $U/main/t/tiff/tiff_4.7.1.orig.tar.bz2)" libtiff-v4.7.1 tiff-4.7.1 tiff-4.7.1.tar.xz
have libwebp-1.5.0.tar.gz || cp "$(get $U/main/libw/libwebp/libwebp_1.5.0.orig.tar.gz)" "$PORTS/libwebp-1.5.0.tar.gz"
have bzip2-1.0.8.tar.gz || cp "$(get $U/main/b/bzip2/bzip2_1.0.8.orig.tar.gz)" "$PORTS/bzip2-1.0.8.tar.gz"
have mesa-20.0.8.tar.gz || repack "$(get $U/main/m/mesa/mesa_20.0.8.orig.tar.gz)" mesa-20.0.8 mesa-20.0.8 mesa-20.0.8.tar.gz
have SoftFloat-3e.zip || gittar ucb-bar/berkeley-softfloat-3 master SoftFloat-3e SoftFloat-3e.zip
have acpica-unix-20251212.tar.gz || cp "$(get $GH/acpica/acpica/releases/download/20251212/acpica-unix-20251212.tar.gz)" "$PORTS/"
have icu4c-61_1-src.tgz || cp "$(get $GH/unicode-org/icu/releases/download/release-61-1/icu4c-61_1-src.tgz)" "$PORTS/"
if ! have zlib.tar.gz; then
  # AROS fetches Chromium's third_party/zlib from googlesource (+archive/lkgr), which is
  # unreachable here and a moving target.  Deadwood's zlib-aros.diff applies cleanly to
  # Chromium tag 140.0.7339.0 (verified 2026-10-28), so pin that tag from the GitHub mirror.
  ZTAG="${QRX_CHROMIUM_ZLIB_TAG:-140.0.7339.0}"
  d="${QRX_CHROMIUM_CHECKOUT:-$W/chromium}"
  if [[ ! -f "$d/third_party/zlib/zlib.h" ]]; then
    for t in 1 2 3 4; do git clone -q --depth 1 --filter=tree:0 --no-checkout -b "$ZTAG" $GH/chromium/chromium "$d" && break; rm -rf "$d"; sleep $((t*4)); done
    git -C "$d" sparse-checkout set --no-cone third_party/zlib
    for t in 1 2 3 4; do git -C "$d" checkout -q && break; sleep $((t*4)); done
  fi
  git -C "$d" log -1 --format="chromium third_party/zlib tag $ZTAG commit %H %ci" > "$PORTS/zlib.PROVENANCE"
  tar czf "$PORTS/zlib.tar.gz" -C "$d/third_party/zlib" .; echo "staged zlib.tar.gz ($ZTAG)"; fi
# integrity gate: every staged archive must be readable and non-empty
while IFS= read -r -d '' a; do
  case "$a" in *.zip) n=$(unzip -l "$a" 2>/dev/null | tail -1 | awk '{print $2}');; *.txt|*.PROVENANCE) continue;; *) n=$(tar tf "$a" 2>/dev/null | wc -l);; esac
  [[ "${n:-0}" -gt 0 ]] || { echo "ERROR: staged archive is empty/corrupt: $a" >&2; exit 1; }
done < <(find "$PORTS" -type f ! -name '.*' -print0)
(cd "$PORTS" && find . -type f ! -name '.*' ! -name '*.sha256' -print0 | sort -z | xargs -0 sha256sum) > "$PORTS/../portssources.SHA256SUMS"
echo "prefetch complete: $PORTS"
