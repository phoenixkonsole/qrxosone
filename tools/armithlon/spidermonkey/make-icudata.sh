#!/bin/sh
# ICU's data in big endian (icudt78b.dat) for the browser objdir. Gecko would convert it with a host
# icupkg of its own, but the browser build for AROS makes neither that tool nor the rule; this builds
# icupkg from the tree's ICU for the host and converts icudt78l.dat.
#   make-icudata.sh <firefox-153.8.0 tree> <objdir> <work dir>
set -e
T="$(cd "$1" && pwd)"; O="$(cd "$2" && pwd)"; mkdir -p "$3"; W="$(cd "$3" && pwd)"
cd "$W"
[ -x bin/icupkg ] || { "$T/intl/icu/source/configure" --disable-tests --disable-samples --disable-extras \
    --disable-icuio --disable-layoutex >cfg.log && make -j4 >mk.log; }
mkdir -p "$O/config/external/icu/data"
LD_LIBRARY_PATH="$W/lib" bin/icupkg -tb "$T/config/external/icu/data/icudt78l.dat" \
    "$O/config/external/icu/data/icudt78b.dat"
echo "made $O/config/external/icu/data/icudt78b.dat"
