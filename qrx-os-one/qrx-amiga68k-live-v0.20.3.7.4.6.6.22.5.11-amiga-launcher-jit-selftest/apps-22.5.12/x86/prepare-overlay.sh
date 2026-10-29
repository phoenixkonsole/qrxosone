#!/usr/bin/env bash
# Build the FS-UAE overlay for the x86 scenario.
#   prepare-overlay.sh <m68k-hunk-dir> <qrx-hunk-dir> <x86-binaries-dir> <work-dir>
# x86-binaries-dir: busybox (i386 static), hello32, difft, fpu, calc.exe, winmine.exe, qrxclock.exe
set -Eeuo pipefail
XH="$1"; QH="$2"; XB="$3"; WK="$4"; HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rm -rf "$WK"; mkdir -p "$WK/overlay/QRX/C" "$WK/overlay/X86"
cp "$XH"/{qrx-i386,qrx-win32} "$QH/QRXScreenGrab" "$WK/overlay/QRX/C/"
cp "$XB"/{busybox,hello32,difft,fpu,calc.exe,winmine.exe,qrxclock.exe} "$WK/overlay/X86/"
cp "$HERE"/{calc-test.txt,winmine-test.txt,clock-test.txt,bbtest.sh} "$WK/overlay/X86/"
cp "$HERE/User-Startup" "$WK/"
