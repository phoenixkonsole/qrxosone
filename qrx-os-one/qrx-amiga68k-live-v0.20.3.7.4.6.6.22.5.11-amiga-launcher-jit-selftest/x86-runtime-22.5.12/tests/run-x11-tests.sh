#!/usr/bin/env bash
set -Eeuo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?usage: run-x11-tests.sh OUTDIR}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
cc -std=c99 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
  "$HERE/tests/x11-protocol.c" "$HERE/linux/qrx_x11.c" -o "$OUT/protocol"
"$OUT/protocol"
bash "$HERE/qmath/build-qmath.sh" cc ar "$OUT" > "$OUT/qmath.log" 2>&1
cc -std=gnu99 -O2 -o "$OUT/qrx-i386" "$HERE/core/qrx86core.c" "$HERE"/linux/*.c "$OUT/libqmath.a" -lm
cc -m32 -nostdlib -static -fno-pie -no-pie -fno-stack-protector -O1 \
  "$HERE/tests/x11-guest.c" -o "$OUT/x11-guest"
QRX_X11_DUMP_DIR="$OUT" timeout 30 "$OUT/qrx-i386" -b 10000000 "$OUT/x11-guest"
python3 - "$OUT/window-00200001.ppm" <<'CHECK'
import pathlib,sys
p=pathlib.Path(sys.argv[1]).read_bytes()
header=b'P6\n64 48\n255\n'
assert p.startswith(header)
p=p[len(header):]
assert len(p)==64*48*3
for y in range(48):
    for x in range(64):
        want=bytes.fromhex('12ab34') if 4<=x<24 and 6<=y<16 else b'\xff\xff\xff'
        assert p[3*(y*64+x):3*(y*64+x)+3]==want,(x,y)
print('PASS all 3072 pixels of i386 X11 window match expected drawing')
CHECK
