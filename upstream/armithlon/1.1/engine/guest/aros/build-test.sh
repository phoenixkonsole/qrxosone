#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${QRX_AROS_CC:-m68k-aros-gcc}"
command -v "$CC" >/dev/null || { echo "Missing AROS native m68k cross compiler: QRX_AROS_CC=/path/to/m68k-aros-gcc" >&2; exit 1; }
DEST="${1:-$HERE/out}"
mkdir -p "$DEST"
# Must use the matching AROS startup objects/includes/linker options for your
# QRX OS One 22.5.11 SDK. Standard compiler flags can be set via QRX_AROS_CFLAGS.
# If the vendor toolchain emits ELF, convert with its matching elf2hunk tool.
# This script deliberately fails if toolchain or target ABI is unavailable.
"$CC" ${QRX_AROS_CFLAGS:-} -O2 -Wall -I "$HERE/.." -o "$DEST/QRXBoardTest" "$HERE/QRXBoardTest.c"
echo "Built AROS target: $DEST/QRXBoardTest (verify HUNK format before deployment)"
