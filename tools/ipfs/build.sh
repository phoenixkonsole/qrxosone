#!/usr/bin/env bash
# Build the native 68k QRX IPFS applications and the encrypted volume tool.
# Uses the AROS SDK installed by the repository bootstrap; no host libc.
set -Eeuo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="$REPO/.build/qrx-sdk/bin"
OUT="${1:-$REPO/.build/qrx-ipfs}"
CC="$SDK/aros-gcc"
ELF2HUNK="$SDK/elf2hunk"

if [ ! -x "$CC" ] || [ ! -x "$ELF2HUNK" ]; then
    echo "AROS SDK missing: $CC" >&2
    echo "Run tools/armithlon/build-desktop.py or the SDK bootstrap first." >&2
    exit 1
fi

mkdir -p "$OUT"
cd "$REPO"
CFLAGS="-O2 -m68040 -fno-pic -fno-PIC -Itools/ipfs -Itools/volimage"
# The P2P image is larger than one 64 KiB HUNK.  -m68000 avoids PC-relative
# code relocations that elf2hunk cannot encode as RELRELOC32; the 68000
# instruction set runs unchanged on the 68040 used by ARMithlon.
P2P_CFLAGS="-Oz -m68000 -fno-pic -fno-PIC -ffunction-sections -fdata-sections -Wl,--gc-sections -Itools/ipfs -Itools/volimage"

build() {
    name="$1"; shift
    echo "  CC  $name"
    "$CC" $CFLAGS "$@" -o "$OUT/$name.elf"
    hout=$("$ELF2HUNK" "$OUT/$name.elf" "$OUT/$name" 2>&1)
    printf '%s\n' "$hout"
    if printf '%s\n' "$hout" | grep -q 'relocation(s) failed'; then
        echo "elf2hunk failed for $name" >&2
        exit 1
    fi
}

build_p2p() {
    name="$1"; shift
    echo "  CC  $name"
    "$CC" $P2P_CFLAGS "$@" -o "$OUT/$name.elf"
    hout=$("$ELF2HUNK" "$OUT/$name.elf" "$OUT/$name" 2>&1)
    printf '%s\n' "$hout"
    if printf '%s\n' "$hout" | grep -q 'relocation(s) failed'; then
        echo "elf2hunk failed for $name" >&2
        exit 1
    fi
}

echo "Building QRX IPFS 68k programs in $OUT"
build QRXIPFS      tools/ipfs/QRXIPFS.c      tools/ipfs/qrx_ipfs.c tools/ipfs/qrx_net.c
build QRXIPFSPub   tools/ipfs/qrx_ipfs_pub.c tools/ipfs/qrx_ipfs.c tools/ipfs/qrx_net.c
build QRXIPFSSync  tools/ipfs/qrx_ipfs_sync.c tools/ipfs/qrx_ipfs.c tools/ipfs/qrx_net.c
build QRXIPFSD     tools/ipfs/QRXIPFSD.c     tools/volimage/qrx_crypto.c tools/ipfs/qrx_net.c
build_p2p QRXIPFSP2P tools/ipfs/QRXIPFSP2P.c tools/ipfs/qrx_p2p.c tools/ipfs/qrx_net.c \
                  tools/volimage/qrx_crypto.c tools/ipfs/vendor/monocypher.c \
                  tools/ipfs/vendor/monocypher-ed25519.c
build QRXVolImage  tools/volimage/QRXVolImage.c tools/volimage/qrx_volimage.c tools/volimage/qrx_crypto.c tools/ipfs/qrx_ipfs.c tools/ipfs/qrx_net.c
build QRXVol       tools/volimage/qrx_vol_cli.c tools/volimage/qrx_volimage.c tools/volimage/qrx_crypto.c tools/ipfs/qrx_net.c

# Install into the active ARMithlon SystemFull tree used by ARMithlon.app.
tree="$REPO/.build/bundle/SystemFull"
if [ -d "$tree" ]; then
    mkdir -p "$tree/C" "$tree/QRX/C"
    for dest in "$tree/C" "$tree/QRX/C"; do
        cp "$OUT/QRXIPFS" "$OUT/QRXIPFSPub" "$OUT/QRXIPFSSync" \
           "$OUT/QRXIPFSD" "$OUT/QRXIPFSP2P" "$OUT/QRXVolImage" \
           "$OUT/QRXVol" "$dest/"
        echo "  installed in ${dest#$REPO/}"
    done

    # Only the real AROS startup belongs here; Emergency-Boot is a bootstrap
    # configuration and must not be patched.
    startup="$tree/S/Startup-Sequence"
    if [ -f "$startup" ]; then
        python3 "$REPO/tools/ipfs/install-startup.py" "$startup"
    fi
fi

echo "Done: $OUT/QRXIPFS, QRXIPFSPub, QRXIPFSSync, QRXIPFSD, QRXIPFSP2P, QRXVolImage, QRXVol"
