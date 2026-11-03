#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?usage: build-guest.sh OUTPUT_DIRECTORY}"
: "${QRX_AROS_CC:?Set QRX_AROS_CC to the actual installed AROS m68k cross compiler}"
mkdir -p "$OUT"
"$QRX_AROS_CC" ${QRX_AROS_CFLAGS:-} -O2 -Wall -Wextra \
    "$ROOT/QRX68Run.c" -o "$OUT/QRX68Run"
file "$OUT/QRX68Run"
echo "AROS guest executable built: $OUT/QRX68Run (not executed)"
