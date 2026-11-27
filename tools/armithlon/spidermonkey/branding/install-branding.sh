#!/bin/sh
# ThunderCat for AROS's logo into Gecko's unofficial branding (the one the AROS build uses), in place
# of Mozilla's: about dialog, about: pages, window and tab icons. The names (brand.ftl,
# brand.properties) come with spidermonkey-153.8.0-aros.patch; the images are made here from
# make-logo.py, which is why they are not in the text patch.
#   install-branding.sh <firefox-153.8.0 tree>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
T="$1/browser/branding/unofficial"
[ -d "$T/content" ] || { echo "usage: $0 <firefox-153.8.0 tree>"; exit 2; }
W=$(mktemp -d)
python3 "$HERE/make-logo.py" "$W" >/dev/null
for s in 16 22 24 32 48 64 128 256; do cp "$W/thundercat-$s.png" "$T/default$s.png"; done
cp "$W/thundercat-192.png" "$T/content/about-logo.png"
cp "$W/thundercat-384.png" "$T/content/about-logo@2x.png"
cp "$W/thundercat-192.png" "$T/content/about-logo-private.png"
cp "$W/thundercat-384.png" "$T/content/about-logo-private@2x.png"
cp "$W/thundercat-256.png" "$T/content/about.png"
cp "$W/thundercat.svg" "$T/content/about-logo.svg"
cp "$W/wordmark.svg" "$T/content/about-wordmark.svg"
cp "$W/wordmark.svg" "$T/content/firefox-wordmark.svg"
rm -rf "$W"
echo "ThunderCat branding in $T"
