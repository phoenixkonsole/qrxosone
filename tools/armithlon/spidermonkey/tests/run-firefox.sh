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
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; trap 'exit 143' TERM INT
mkdir -p $T/fonts
cp $AROSSRC/workbench/fonts/truetype/bitstream/*.ttf $AROSSRC/workbench/fonts/truetype/opensans/*.ttf $T/fonts/
# the browser packed as shipped (package-browser.py: omni.ja, browser/omni.ja, the firefox program)
python3 -I "$HERE/../package-browser.py" "$OBJ/dist/bin" $T/ff
URL="${QRX_URL:-file:///SYS/Web/qrxrocks/index.html}"
# the profile starts with prefs for a single-process browser: extensions in this process too (their
# background pages otherwise wait for an extension process that never comes, and shutdown hangs on
# them until nsTerminator crashes Firefox, before the shell can Type the PNG)
mkdir -p $T/profile
cat > $T/profile/user.js <<'EOP'
user_pref("extensions.webextensions.remote", false);
EOP
export QRX_TREES="$T/ff:Firefox $T/fonts:Fonts/TrueType $WEB:Web/qrxrocks $T/profile:T/profile"
# the disk image lives in guest memory; the guest boots with at most 2048 MB (qrxboot)
export QRX_IMG_MB=${QRX_IMG_MB:-600} QRX_MEM_MB=${QRX_MEM_MB:-2048}
# MOZ_FORCE_DISABLE_E10S: one process; AROS cannot start Gecko's content processes (fork, shared
# memory handed to a child), and a build that is not MOZILLA_OFFICIAL takes this variable
export QRX_PRE='Assign FONTS: SYS:Fonts\nStack 2000000\nSet TMPDIR /T\nSet HOME /T\nSet MOZ_FORCE_DISABLE_E10S 1\nMakeDir T:profile\n'"${QRX_EXTRA_PRE:-}"   # QRX_EXTRA_PRE: e.g. Set QRX_TEXTBASE 1
export QRX_CMD="SYS:Firefox/firefox"
export QRX_ARGS="--headless --profile /T/profile --screenshot /T/shot.png --window-size 1280,900 $URL\nType T:shot.png HEX"
# the system disk comes back too: when Firefox does not end cleanly (a crash on shutdown after it
# saved the screenshot) the shell never gets to Type, and the PNG is read from the disk instead
export QRX_KEEP_DISK=$T/disk.img
log=$(bash "$HERE/../../rust/tests/run-guest.sh" /dev/null "$SECS")
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
if ! head -c 8 "$OUT" | grep -q PNG && [ -f $T/disk.img ]; then
  python3 -I "$HERE/../../aros-qrx/boot/ffsget.py" $T/disk.img T/shot.png "$OUT" &&
    python3 -I -c 'import sys; d=open(sys.argv[1],"rb").read(); print("from the disk: png bytes:", len(d), "signature ok:", d[:8] == b"\x89PNG\r\n\x1a\n")' "$OUT"
fi
rm -rf $T
