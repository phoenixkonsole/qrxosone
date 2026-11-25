#!/bin/bash
# Runs the Gecko browser in the QRX OS One 64 guest (rust/tests/run-guest.sh: qrxboot, two EC64 cores),
# headless, and has it render a page into a PNG: firefox --headless --screenshot. The PNG comes back
# through the console (Type HEX) and is written to <out.png>.
# Usage: run-firefox.sh <obj-browser dir> <qrx.rocks mirror dir> <out.png> [seconds]
# The page is the local copy (qrxrocks/mirror.py) at SYS:Web/qrxrocks; Gecko runs in AROS's Unix path
# mode (-nix), so it is file:///SYS/Web/qrxrocks/index.html. Fonts: AROS's TrueType fonts (Bitstream
# Vera, Open Sans) from the AROS source in SYS:Fonts/TrueType.
set -e
OBJ="$1"; WEB="$2"; OUT="$3"; SECS="${4:-7200}"
HERE="$(cd "$(dirname "$0")" && pwd)"
AROSSRC=${QRX_AROS_SRC:-/opt/qrx-os-one/aros64/src}
T=$(mktemp -d)
mkdir -p $T/fonts
cp $AROSSRC/workbench/fonts/truetype/bitstream/*.ttf $AROSSRC/workbench/fonts/truetype/opensans/*.ttf $T/fonts/
# dist/bin without the program itself (that becomes C:T)
mkdir -p $T/ff; cp -rL "$OBJ/dist/bin/." $T/ff/; rm -f $T/ff/firefox $T/ff/firefox-bin
URL="${QRX_URL:-file:///SYS/Web/qrxrocks/index.html}"
export QRX_TREES="$T/ff:Firefox $T/fonts:Fonts/TrueType $WEB:Web/qrxrocks"
export QRX_IMG_MB=${QRX_IMG_MB:-768} QRX_MEM_MB=${QRX_MEM_MB:-3072}
export QRX_PRE='Assign FONTS: SYS:Fonts\nStack 2000000\nSet TMPDIR /T\nSet HOME /T\nMakeDir T:profile\n'
export QRX_ARGS="--headless --profile /T/profile --screenshot /T/shot.png --window-size 1280,900 $URL\nType T:shot.png HEX"
log=$(bash "$HERE/../../rust/tests/run-guest.sh" "$OBJ/dist/bin/firefox" "$SECS")
echo "$log" | grep -v "^[0-9a-f]\{4,\}: " | tail -40
# Type HEX lines: "0000: 89504e47 0d0a1a0a ...   text"
echo "$log" | python3 -I -c '
import sys, re
data = bytearray()
for line in sys.stdin:
    m = re.match(r"^([0-9a-f]{4,}): ((?:[0-9a-f]{2,8} ?){1,4})", line)
    if m:
        data += bytes.fromhex(m.group(2).replace(" ", ""))
open(sys.argv[1], "wb").write(data)
print("png bytes:", len(data), "signature ok:", data[:8] == b"\x89PNG\r\n\x1a\n")
' "$OUT"
rm -rf $T
