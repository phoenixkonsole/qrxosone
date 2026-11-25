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
# the whole tree (Gecko, the browser, NSS) except the web-platform tests (about 1 GB of test data;
# with QRX_NO_TESTS=1 the build reads their missing manifests as empty)
xz -dc dl/$V.esr.tar.xz | tar -x --exclude="$V/testing/web-platform/tests" \
  --exclude="$V/testing/web-platform/mozilla/tests"
patch -p1 -d $V < "$HERE/spidermonkey-153.8.0-aros.patch"
# the patch changes vendored crates (libc, getrandom, rustix, socket2, mtu, errno, libloading: AROS);
# cargo checks their file checksums
python3 -I "$HERE/fix-checksums.py" $(grep -o '^+++ b/third_party/rust/[^/]*' "$HERE/spidermonkey-153.8.0-aros.patch" |
  sed 's|^+++ b/|'"$V"'/|' | sort -u)
echo "unpacked and patched: $W/$V"
