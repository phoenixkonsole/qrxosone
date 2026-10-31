#!/usr/bin/env bash
# Build the FS-UAE overlay for the live RTG-direct scenario (dxtest + PrBoom-plus/Freedoom).
#   prepare-overlay.sh <x86-hunk-dir (qrx-win32)> <qrx-hunk-dir (QRXScreenGrab)> <doom-dir> <work-dir>
# <doom-dir>: prboom-plus.exe (build-doom.sh), prboom-plus.wad, freedoom1.wad,
#             tranmap.dat (written by PrBoom-plus on its first run)
set -Eeuo pipefail
XH="$1"; QH="$2"; DD="$3"; WK="$4"; HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rm -rf "${WK:?}"; mkdir -p "$WK/overlay/QRX/C" "$WK/overlay/X86" "$WK/overlay/DOOM"
cp "$XH/qrx-win32" "$QH/QRXScreenGrab" "$WK/overlay/QRX/C/"
cp "$HERE/../../x86-runtime-22.5.12/win32/apps/prebuilt/dxtest.exe" "$WK/overlay/X86/"
cp "$DD"/{prboom-plus.exe,prboom-plus.wad,freedoom1.wad,tranmap.dat} "$WK/overlay/DOOM/"
cp "$HERE/User-Startup" "$WK/"
