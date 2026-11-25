#!/bin/bash
# Runs tests/hallo.js with the js shell in the QRX OS One 64 guest (rust/tests/run-guest.sh: qrxboot, two
# EC64 cores) and compares the output with hallo.expected. Usage: run-js.sh <js binary> [seconds]
HERE="$(cd "$(dirname "$0")" && pwd)"
out=$(QRX_FILES="$HERE/hallo.js" QRX_IMG_MB=96 QRX_PRE='Stack 4000000\n' QRX_ARGS="T:hallo.js" \
      bash "$HERE/../../rust/tests/run-guest.sh" "$1" "${2:-1200}")
if [ "$out" = "$(cat "$HERE/hallo.expected")" ]; then echo "JS PASS"; else echo "JS FAIL"; echo "$out"; exit 1; fi
