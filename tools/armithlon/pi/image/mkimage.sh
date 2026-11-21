#!/usr/bin/env bash
# The SD card image of the QRX ARMithlon Pi 4 appliance: one file to write to a card.
#   p1 FAT32 "QRXBOOT" 512 MiB: Pi firmware (start4.elf, fixup4.dat), kernel8.img, device tree,
#                              overlays, config.txt, cmdline.txt, qrx.txt; room for QRX OS One 64
#                              (qrx64/ and QRXOS64.img, copied from the Mac bundle)
#   p2 ext4  "qrxroot":        the root file system of mkrootfs.py (grows to the card on first start)
# mkimage.sh ROOTFS KERNELDIR FIRMWAREDIR OUT.img
#   KERNELDIR   output of kernel/build-kernel.sh;  FIRMWAREDIR  .../usr/lib/linux-firmware-raspi
set -Eeuo pipefail
ROOT="$1"; K="$2"; FW="$3"; IMG="$4"
HERE="$(cd "$(dirname "$0")" && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
MiB=$((1024 * 1024)); BOOT_MB=512

# boot partition
mkdir -p "$W/boot/overlays"
cp "$FW/start4.elf" "$FW/fixup4.dat" "$W/boot/"
cp "$K/kernel8.img" "$K/bcm2711-rpi-4-b.dtb" "$W/boot/"
cp "$K"/overlays/* "$W/boot/overlays/"
cp "$HERE/boot/config.txt" "$HERE/boot/cmdline.txt" "$HERE/boot/qrx.txt" "$W/boot/"
truncate -s $((BOOT_MB * MiB)) "$W/boot.img"
mkfs.vfat -F 32 -n QRXBOOT "$W/boot.img" >/dev/null
mcopy -s -i "$W/boot.img" "$W"/boot/* ::/

# root partition: the files plus a quarter, at least 256 MiB free, whole MiB
used=$(du -s --block-size=1M "$ROOT" | cut -f1)
size=$(( used + used / 4 + 256 ))
truncate -s $((size * MiB)) "$W/root.img"
mkfs.ext4 -q -F -L qrxroot -m 1 -d "$ROOT" "$W/root.img"

# the card: MBR, p1 at 4 MiB, p2 right after it
start1=$((4 * MiB / 512)); n1=$((BOOT_MB * MiB / 512)); start2=$((start1 + n1)); n2=$((size * MiB / 512))
rm -f "$IMG"
truncate -s $(( (start2 + n2) * 512 )) "$IMG"
printf 'label: dos\n%s,%s,c,*\n%s,%s,83\n' "$start1" "$n1" "$start2" "$n2" | sfdisk -q "$IMG"
dd if="$W/boot.img" of="$IMG" bs=512 seek=$start1 conv=notrunc,sparse status=none
dd if="$W/root.img" of="$IMG" bs=512 seek=$start2 conv=notrunc,sparse status=none
sha256sum "$IMG" > "$IMG.sha256"
echo "image: $IMG ($(( (start2 + n2) * 512 / MiB )) MiB, root $size MiB)"
