#!/bin/bash
# Installs Rockstar's GTA-1_installer.ZIP into the Apps folder for ARMithlon, because its
# InstallShield setup and "GTA Settings" (MFC) do not run under QRX86. It writes the settings
# that program would write: English, and the default keys.
#
#   tools/armithlon/install-gta1.sh [installer.zip] [apps-folder]
#
# Needs unzip, 7zz and unshield (brew install sevenzip unshield). Start in ARMithlon with
# Execute S:GTA
set -euo pipefail
APPS="${2:-$(defaults read org.qrxosone.armithlon.aros AppsDirectory 2>/dev/null || echo "$HOME/Downloads/test-apps")}"
ZIP="${1:-$APPS/windows_games/GTA-1_installer.ZIP}"
DEST="$APPS/windows_games/gta1"
for tool in unzip 7zz unshield; do
    command -v "$tool" >/dev/null || { echo "$tool fehlt: brew install sevenzip unshield" >&2; exit 2; }
done
[ -f "$ZIP" ] || { echo "Installer nicht gefunden: $ZIP" >&2; exit 2; }
[ -e "$DEST" ] && { echo "$DEST existiert bereits" >&2; exit 2; }
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
unzip -q "$ZIP" -d "$WORK/zip"
7zz x -y -bso0 -bse0 "-o$WORK/inst" "$WORK/zip/GTAINSTALLER.exe"
# unshield cannot read one file that spans the two cabinets (Track8.wav): the game runs without it.
unshield -d "$WORK/out" x "$WORK/inst/GTAINSTALLER/data1.cab" >/dev/null || echo "Hinweis: nicht jede Datei liess sich entpacken" >&2
SRC="$WORK/out/Program_Executable_Files"
mkdir -p "$DEST"
cp -R "$SRC/WINO" "$SRC/GTADATA" "$SRC/Music" "$SRC/README.TXT" "$DEST/"
# Key values are what GTA Settings stores as defaults: arrows, space, return, ctrl, x, z, tab.
{
    K='hklm\software\dma design\grand theft auto'
    echo "$K|Language|4|00000000"
    i=0
    for code in cb cd c8 d0 39 1c 1d 2d 2c 0f; do
        echo "$K\\controls|Control $i|4|${code}000000"
        i=$((i + 1))
    done
} > "$DEST/WINO/QRXREG.TXT"
echo "GTA installiert in $DEST"
