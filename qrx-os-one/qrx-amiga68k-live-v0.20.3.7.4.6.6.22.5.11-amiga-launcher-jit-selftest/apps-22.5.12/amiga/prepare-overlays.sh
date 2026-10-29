#!/usr/bin/env bash
# Build FS-UAE scenario overlays for the three Amiga 68k test programs.
#   prepare-overlays.sh <downloads-dir> <qrx-hunk-dir> <work-dir>
# downloads-dir: DOpus5_104_os3.lha, AmigaGPT-3.4.0.lha, HippoPlayer git checkout (v2.67)
set -Eeuo pipefail
DL="$1"; QH="$2"; WK="$3"; HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qrx(){ mkdir -p "$1/QRX/C"; cp "$QH"/{QRXAmigaRun,amigarun,amigacheck,QRXScreenGrab} "$1/QRX/C/"; }
for s in dopus5 hippoplayer amigagpt; do rm -rf "$WK/$s"; mkdir -p "$WK/$s/overlay/Libs"; cp "$HERE/scenarios/$s/User-Startup" "$WK/$s/"; qrx "$WK/$s/overlay"; done
# Directory Opus 5.104 (BlitterStudio/dopus5 release, AROS Public License)
( cd "$WK/dopus5/overlay" && lha xq "$DL/DOpus5_104_os3.lha" DOpus5/* >/dev/null && cp DOpus5/Libs/dopus5.library Libs/ )
# HippoPlayer 2.67 (koobo/HippoPlayer, MIT): program + playsid.library + a generated test module
mkdir -p "$WK/hippoplayer/overlay/HippoPlayer"
cp "$DL/HippoPlayer/dist/HiP" "$DL/HippoPlayer/dist/HiP.info" "$WK/hippoplayer/overlay/HippoPlayer/"
cp "$DL/HippoPlayer/dist/playsid.library" "$WK/hippoplayer/overlay/Libs/"
python3 -I "$HERE/make-test-mod.py" "$WK/hippoplayer/overlay/HippoPlayer/qrx-test.mod"
# AmigaGPT 3.4.0 (sacredbanana/AmigaGPT release): program + MUI custom classes
( cd "$WK/amigagpt/overlay" && lha xq "$DL/AmigaGPT-3.4.0.lha" >/dev/null )
mkdir -p "$WK/amigagpt/overlay/Libs/MUI" "$WK/amigagpt/overlay/Classes/Zune"
for m in "$WK/amigagpt/overlay/AmigaGPT/MCC/"*_OS3.mcc; do b=$(basename "$m" _OS3.mcc); cp "$m" "$WK/amigagpt/overlay/Libs/MUI/$b.mcc"; cp "$m" "$WK/amigagpt/overlay/Classes/Zune/$b.mcc"; done
echo "overlays ready in $WK"
