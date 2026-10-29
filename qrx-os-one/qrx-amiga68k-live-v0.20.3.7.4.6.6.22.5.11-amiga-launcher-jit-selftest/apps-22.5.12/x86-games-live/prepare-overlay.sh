#!/usr/bin/env bash
# Build the FS-UAE overlay for the live card-game scenario (ReactOS sol.exe/spider.exe + cards.dll).
#   prepare-overlay.sh <x86-hunk-dir (qrx-win32, QRXInput)> <qrx-hunk-dir (QRXScreenGrab)> <x86-binaries-dir> <work-dir>
set -Eeuo pipefail
XH="$1"; QH="$2"; XB="$3"; WK="$4"; HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rm -rf "$WK"; mkdir -p "$WK/overlay/QRX/C" "$WK/overlay/X86"
cp "$XH"/{qrx-win32,QRXInput} "$QH/QRXScreenGrab" "$WK/overlay/QRX/C/"
cp "$XB"/{sol.exe,spider.exe,cards.dll} "$WK/overlay/X86/"
cp "$HERE/User-Startup" "$WK/"
