#!/bin/bash
# Builds tests/hostfs-test.c for m68k64-unknown-aros and runs it in QRX OS One 64 (qrxboot) on a
# fresh host directory mounted as HOST: (qrxhostfs-handler, host service 12), then checks on the
# host what the guest wrote and that a symbolic link in the share stayed invisible and closed.
# Usage: run-hostfs-test.sh   (QRX_AROS64, QRX_LLVM: the AROS 64 build and the ec64 LLVM build)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
B=${QRX_AROS64:-/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64}; L=${QRX_LLVM:-/opt/qrx-os-one/ec64llvm/build}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
export PATH="$B/../linux-x86_64/tools:$PATH"
"$L/bin/clang" --target=m68k64-unknown-aros --sysroot "$B/AROS/Development" -O1 -nix "$HERE/hostfs-test.c" -o "$W/t" \
  "$L/lib/libqrxaroscompat.a" "$(find "$L" -name libunwind.a | head -1)" -lpthread
mkdir -p "$W/share"
printf 'hello world\n' > "$W/share/from-host.txt"
ln -s /etc/passwd "$W/share/escape"
out=$(QRX_HOSTFS="$W/share" QRX_PRE='Mount HOST:\nList HOST:\n' bash "$HERE/../../../rust/tests/run-guest.sh" "$W/t" 120)
echo "$out"
echo "$out" | grep -q "^PASSED: 0 failures" || { echo "hostfs test: FAILED in the guest"; exit 1; }
[ "$(cat "$W/share/written-by-guest.txt")" = "from QRX OS One 64" ] || { echo "hostfs test: the guest's file is not on the host"; exit 1; }
[ ! -e "$W/share/newdir" ] && [ -L "$W/share/escape" ] || { echo "hostfs test: the share is not as expected"; exit 1; }
echo "hostfs test: PASSED"
