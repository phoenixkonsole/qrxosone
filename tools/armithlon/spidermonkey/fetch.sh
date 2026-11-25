#!/bin/sh
# Fetches Firefox ESR 153.8.0's source, checks it and unpacks the part SpiderMonkey's own source
# package takes (js/src/make-source-package.py, rsync_filter_list), then applies the AROS patch.
# Usage: fetch.sh WORKDIR
set -e
HERE=$(cd "$(dirname "$0")" && pwd); W=${1:?usage: fetch.sh WORKDIR}; V=firefox-153.8.0
mkdir -p "$W/dl"; cd "$W"
[ -f dl/$V.esr.tar.xz ] || curl -fL -o dl/$V.esr.tar.xz \
  https://archive.mozilla.org/pub/firefox/releases/153.8.0esr/source/firefox-153.8.0esr.source.tar.xz
# from https://archive.mozilla.org/pub/firefox/releases/153.8.0esr/SHA256SUMS
echo "824d5e0bbcae30825ee4ec3267d21018b01d33ac2fda50065de8c41153d4ec85  dl/$V.esr.tar.xz" | sha256sum -c
xz -dc dl/$V.esr.tar.xz | tar -x --wildcards \
  --exclude="$V/testing/web-platform/tests/*" --exclude="$V/testing/web-platform/meta/*" \
  --exclude="$V/testing/web-platform/mozilla/*" \
  "$V/client.mk" "$V/configure.py" "$V/LICENSE" "$V/mach" "$V/Makefile.in" "$V/moz.build" "$V/moz.configure" \
  "$V/test.mozbuild" "$V/.ycm_extra_conf.py" "$V/Cargo.toml" "$V/Cargo.lock" "$V/.cargo" \
  "$V/mfbt" "$V/nsprpub" "$V/intl" "$V/memory" "$V/modules/fdlibm" "$V/modules/zlib" "$V/mozglue" \
  "$V/tools/rb/fix_stacks.py" "$V/tools/fuzzing" "$V/build" "$V/config" "$V/python" \
  "$V/third_party/fmt" "$V/third_party/function2" "$V/third_party/python" "$V/third_party/rust" \
  "$V/third_party/gemmology" "$V/third_party/xsimd" "$V/layout/tools/reftest/reftest" \
  "$V/testing/*.py" "$V/testing/manifest" "$V/testing/moz.build" "$V/testing/mozbase" "$V/testing/mozharness" \
  "$V/testing/performance" "$V/testing/test" "$V/testing/web-platform/*.ini" \
  "$V/toolkit/crashreporter/tools/symbolstore.py" "$V/toolkit/mozapps/installer/package-name.mk" \
  "$V/toolkit/components/glean" "$V/toolkit/moz.configure" "$V/xpcom/ds/tools/perfecthash.py" \
  "$V/xpcom/geckoprocesstypes_generator" "$V/modules/libpref/init/StaticPrefList.yaml" "$V/js"
patch -p1 -d $V < "$HERE/spidermonkey-153.8.0-aros.patch"
# the patch changes vendored crates (libc, getrandom: AROS); cargo checks their file checksums
python3 -I "$HERE/fix-checksums.py" $V/third_party/rust/libc $V/third_party/rust/getrandom
echo "unpacked and patched: $W/$V"
