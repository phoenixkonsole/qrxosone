#!/usr/bin/env bash
# Build the QRX ARMithlon Pi 4 kernel: Raspberry Pi Linux (rpi-6.6.y) with bcm2711_defconfig
# plus qrx-pi4.config, drivers built in, Wi-Fi/Bluetooth firmware and regdb embedded.
#   build-kernel.sh LINUX-SRC FIRMWARE-ROOT OUTDIR
# FIRMWARE-ROOT holds brcm/..., regulatory.db and regulatory.db.p7s (see ../image/mkrootfs.py --firmware).
set -Eeuo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$1" && pwd)"; FW="$(cd "$2" && pwd)"; OUT="$3"; mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
export ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
rm -rf "$SRC/firmware-qrx"; cp -r "$FW" "$SRC/firmware-qrx"
make -C "$SRC" O="$OUT/obj" bcm2711_defconfig >/dev/null
"$SRC/scripts/kconfig/merge_config.sh" -m -O "$OUT/obj" "$OUT/obj/.config" "$HERE/qrx-pi4.config" >/dev/null
make -C "$SRC" O="$OUT/obj" olddefconfig >/dev/null
# every requested option must have survived the dependency resolution
missing=0
while IFS= read -r l; do
  [[ "$l" =~ ^CONFIG_([A-Z0-9_]+)=(.*)$ ]] || continue
  k="CONFIG_${BASH_REMATCH[1]}"; v="${BASH_REMATCH[2]}"
  if [[ "$v" == n ]]; then grep -q "^$k=" "$OUT/obj/.config" && { echo "still set: $k"; missing=1; }
  else grep -qxF "$k=$v" "$OUT/obj/.config" || { echo "not applied: $k=$v ($(grep "^$k=" "$OUT/obj/.config" || echo unset))"; missing=1; }; fi
done < "$HERE/qrx-pi4.config"
make -C "$SRC" O="$OUT/obj" -j"${QRX_BUILD_JOBS:-$(nproc)}" Image dtbs
cp "$OUT/obj/arch/arm64/boot/Image" "$OUT/kernel8.img"
cp "$OUT/obj/arch/arm64/boot/dts/broadcom/bcm2711-rpi-4-b.dtb" "$OUT/"
mkdir -p "$OUT/overlays"
cp "$OUT"/obj/arch/arm64/boot/dts/overlays/{vc4-kms-v3d-pi4,disable-bt,miniuart-bt}.dtbo "$OUT/overlays/" 2>/dev/null || true
cp "$OUT/obj/arch/arm64/boot/dts/overlays/overlay_map.dtb" "$OUT/overlays/" 2>/dev/null || true
cp "$OUT/obj/.config" "$OUT/config-qrx-pi4"
echo "kernel: $OUT/kernel8.img ($(stat -c %s "$OUT/kernel8.img") bytes); config check: $([[ $missing == 0 ]] && echo ok || echo 'see above')"
