#!/bin/bash
# Downloads Microsoft's DirectX End-User Runtime (June 2010) and installs its 32-bit helper
# DLLs (D3DX, D3DCompiler, XInput, XAudio, X3DAudio, XACT) for QRX86 programs.
#
#   tools/armithlon/install-directx.sh [--yes] [--from FILE] [apps-folder]
#
# Microsoft licenses these files to you, so this runs on your request and nothing of it
# ships with ARMithlon. The core of DirectX (DirectDraw, Direct3D, DirectSound, DirectInput)
# comes from QRX86 itself and cannot be replaced by Microsoft's drivers: they need Windows.
# Programs find the DLLs in APPS:windows_system.
set -euo pipefail
URL="https://download.microsoft.com/download/8/4/A/84A35BF1-DAFE-4AE8-82AF-AD2AE20B6B14/directx_Jun2010_redist.exe"
YES=0 FROM="" APPS=""
while [ $# -gt 0 ]; do
    case "$1" in
    --yes) YES=1 ;;
    --from) FROM="$2"; shift ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) APPS="$1" ;;
    esac
    shift
done
[ -n "$APPS" ] || APPS="$(defaults read org.qrxosone.armithlon.aros AppsDirectory 2>/dev/null || echo "$HOME/Downloads/test-apps")"
command -v 7zz >/dev/null || { echo "7zz fehlt: brew install sevenzip" >&2; exit 2; }
DEST="$APPS/windows_system"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SETUP="$FROM"
if [ -z "$SETUP" ]; then
    echo "Quelle: $URL"
    echo "Groesse: rund 100 MB. Ziel: $DEST"
    echo "Es gelten die Lizenzbedingungen von Microsoft fuer die DirectX-Laufzeit."
    if [ "$YES" != 1 ]; then
        printf "Herunterladen und installieren? [j/N] "
        read -r answer
        case "$answer" in j|J|y|Y) ;; *) echo "Abgebrochen."; exit 1 ;; esac
    fi
    SETUP="$WORK/directx_redist.exe"
    curl -fL --progress-bar -o "$SETUP" "$URL"
fi
7zz x -y -bso0 -bse0 "-o$WORK/cabs" "$SETUP"
mkdir -p "$DEST"
count=0
for cab in "$WORK"/cabs/*_x86.cab "$WORK"/cabs/*_x86_*.cab; do
    [ -f "$cab" ] || continue
    7zz x -y -bso0 -bse0 "-o$WORK/dll" "$cab" '*.dll' >/dev/null 2>&1 || continue
done
if [ -d "$WORK/dll" ]; then
    for dll in "$WORK"/dll/*.dll "$WORK"/dll/*.DLL; do
        [ -f "$dll" ] || continue
        cp "$dll" "$DEST/"
        count=$((count + 1))
    done
fi
[ "$count" -gt 0 ] || { echo "Keine DLLs gefunden: ist das wirklich die DirectX-Laufzeit?" >&2; exit 3; }
echo "$count DLLs installiert in $DEST"
