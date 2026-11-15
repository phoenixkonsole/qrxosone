#!/usr/bin/env bash
# Build the Mac image container, the MFS reader, the host test and the guest
# tool, then install the tool and its synthetic images into the guest tree.
set -Eeuo pipefail

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$REPO/tools/armithlon/macimage"
SDK="$REPO/.build/qrx-sdk/bin"
OUT="${1:-$REPO/.build/macimage}"
TREE="$REPO/.build/bundle/SystemFull"
CC="${CC:-cc}"
CFLAGS="-std=c99 -O2 -Wall -Wextra -Werror"

mkdir -p "$OUT"

echo "  CC  test_macimage"
"$CC" $CFLAGS -I"$SRC" \
    "$SRC/qrx_macimage.c" "$SRC/qrx_mfs.c" "$SRC/qrx_hfs.c" \
    "$SRC/qrx_hfsplus.c" "$SRC/qrx_forkfile.c" "$SRC/qrx_testimg.c" \
    "$SRC/test_macimage.c" -o "$OUT/test_macimage"

echo "  RUN test_macimage"
"$OUT/test_macimage"

echo "  CC  make-test-images"
"$CC" $CFLAGS -I"$SRC" \
    "$SRC/qrx_macimage.c" "$SRC/qrx_mfs.c" "$SRC/qrx_hfs.c" \
    "$SRC/qrx_hfsplus.c" "$SRC/qrx_forkfile.c" "$SRC/qrx_testimg.c" \
    "$SRC/make-test-images.c" -o "$OUT/make-test-images"

if [ ! -x "$SDK/aros-gcc" ] || [ ! -x "$SDK/elf2hunk" ]; then
    echo "AROS SDK missing: $SDK/aros-gcc" >&2
    exit 1
fi

echo "  CC  QRXMacFS.elf"
"$SDK/aros-gcc" -O2 -m68040 -fno-pic -fno-PIC -I"$SRC" \
    "$SRC/qrx-macfs.c" "$SRC/qrx_macimage.c" "$SRC/qrx_mfs.c" \
    "$SRC/qrx_hfs.c" "$SRC/qrx_hfsplus.c" "$SRC/qrx_forkfile.c" \
    -o "$OUT/QRXMacFS.elf"

echo "  HUNK QRXMacFS"
hunk_out="$("$SDK/elf2hunk" "$OUT/QRXMacFS.elf" "$OUT/QRXMacFS" 2>&1)"
printf '%s\n' "$hunk_out"
if printf '%s\n' "$hunk_out" | grep -q 'relocation(s) failed'; then
    echo "elf2hunk failed for QRXMacFS" >&2
    exit 1
fi
if ! file "$OUT/QRXMacFS" | grep -q 'AmigaOS loadseg()ble executable/binary'; then
    echo "QRXMacFS is not a loadseg()ble Hunk" >&2
    exit 1
fi

if [ -d "$TREE" ]; then
    mkdir -p "$TREE/C" "$TREE/Storage/TestImages/MacFS"
    cp "$OUT/QRXMacFS" "$TREE/C/QRXMacFS"
    echo "  IMG Storage/TestImages/MacFS"
    "$OUT/make-test-images" "$TREE/Storage/TestImages/MacFS"
fi

echo "Done: $OUT/QRXMacFS"
