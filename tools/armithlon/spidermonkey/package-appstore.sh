#!/bin/bash
# Packs the Gecko browser as ThunderCat for AROS (Firefox 153.8.0's source with the QRX patches,
# for QRX OS One 64) as a QRX App Store archive:
#   package-appstore.sh <obj-browser dir> <output dir> <revision>
# writes <output dir>/thundercat-153.8.0-qrx64-<revision>.tar.xz holding thundercat/:
#   ThunderCat (Gecko's firefox program, without debug sections: llvm-strip --strip-debug),
#   omni.ja, browser/omni.ja, application.ini, platform.ini            package-browser.py
#   Start        the launcher (Execute script): PIPE:, the fonts, the environment, the profile
#   xulstore.json  the first window's size and place (Start copies it into a new profile)
#   logo.png, ThunderCat.info, Start.info   the logo (branding/make-logo.py), the icons as AROS PNG icons
#   Fonts/TrueType   Bitstream Vera and Open Sans from the AROS source (Gecko reads FONTS:TrueType)
#   start.html   the start page, NOTICE  licences and where the source is
# The archive is reproducible: the same input gives the same bytes and the same SHA-256.
# QRX_LLVM_STRIP: llvm-strip of the m68k64 LLVM; QRX_AROS_SRC: the AROS source (for the fonts).
set -euo pipefail
OBJ="${1:?usage: package-appstore.sh OBJ-BROWSER OUT REVISION}"; OUT="${2:?}"; REV="${3:?}"
HERE="$(cd "$(dirname "$0")" && pwd)"
STRIP=${QRX_LLVM_STRIP:-/opt/qrx-os-one/ec64llvm/build/bin/llvm-strip}
AROSSRC=${QRX_AROS_SRC:-/opt/qrx-os-one/aros64/src}
VER=153.8.0
NAME=thundercat-$VER-qrx64-$REV
D=thundercat
W="$OUT/$NAME.work"
rm -rf "$W"; mkdir -p "$W"
python3 -I "$HERE/package-browser.py" "$OBJ/dist/bin" "$W/$D"
"$STRIP" --strip-debug -o "$W/$D/ThunderCat" "$W/$D/firefox"
rm "$W/$D/firefox"
mkdir -p "$W/$D/Fonts/TrueType"
cp "$AROSSRC"/workbench/fonts/truetype/bitstream/*.ttf "$AROSSRC"/workbench/fonts/truetype/opensans/*.ttf \
   "$W/$D/Fonts/TrueType/"
cp "$HERE/appstore/Start" "$HERE/appstore/start.html" "$HERE/appstore/NOTICE" "$HERE/appstore/xulstore.json" "$W/$D/"
python3 "$HERE/branding/make-logo.py" "$W/logo" >/dev/null
cp "$W/logo/thundercat-128.png" "$W/$D/logo.png"
cp "$W/logo/thundercat-64.png" "$W/$D/ThunderCat.info"
cp "$W/logo/thundercat-64.png" "$W/$D/Start.info"
rm -rf "$W/logo"
# fixed dates, owners and order: the same bytes on every build
find "$W/$D" -exec touch -h -d @1767225600 {} +
tar -C "$W" --sort=name --owner=0 --group=0 --numeric-owner --mtime=@1767225600 --format=gnu \
    -cf - "$D" | xz -9 -T1 -c > "$OUT/$NAME.tar.xz"
echo "installed bytes: $(du -sb "$W/$D" | cut -f1)"
rm -rf "$W"
ls -l "$OUT/$NAME.tar.xz"; sha256sum "$OUT/$NAME.tar.xz"
