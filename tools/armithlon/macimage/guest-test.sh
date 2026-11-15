#!/usr/bin/env bash
# Guest test for QRXMacFS: runs the container layer and the MFS reader inside
# ARMithlon over the synthetic images written by build.sh.
set -Eeuo pipefail

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$REPO"
TREE=.build/bundle/SystemFull
STARTUP=$TREE/S/Startup-Sequence
BUNDLE=.build/bundle/ARMithlon.app/Contents/Resources
HARNESS=.build/m1-merged/qrx68100_full_run
LOG=/tmp/qrx-macfs-guest.log
BOOT_LOG=macfs.log
BACKUP="$(mktemp /tmp/qrx-macfs-startup.XXXXXX)"

cp -p "$STARTUP" "$BACKUP"
restore() {
    cp -p "$BACKUP" "$STARTUP"
    rm -f "$BACKUP" "$TREE/C/$BOOT_LOG"
}
trap restore EXIT HUP INT TERM

bash tools/armithlon/macimage/build.sh > /tmp/qrx-macfs-build.log 2>&1

mkdir -p /tmp/qrx-apps-empty
printf '%s\n' \
    'Echo "QRX MacFS suite" >C:macfs.log' \
    'C:QRXMacFS INFO SYS:Storage/TestImages/MacFS/mfs400.img >>C:macfs.log' \
    'C:QRXMacFS LIST SYS:Storage/TestImages/MacFS/mfs400.img >>C:macfs.log' \
    'C:QRXMacFS EXTRACT SYS:Storage/TestImages/MacFS/mfs400.img Hello RAM:hello.txt >>C:macfs.log' \
    'Type RAM:hello.txt >>C:macfs.log' \
    'Echo "" >>C:macfs.log' \
    'C:QRXMacFS CAT SYS:Storage/TestImages/MacFS/mfs400.img Missing >>C:macfs.log' \
    'C:QRXMacFS INFO SYS:Storage/TestImages/MacFS/mfs400.dc42.img >>C:macfs.log' \
    'C:QRXMacFS INFO SYS:Storage/TestImages/MacFS/mfs400.ndif.img >>C:macfs.log' \
    'C:QRXMacFS LIST SYS:Storage/TestImages/MacFS/apm.img >>C:macfs.log' \
    'C:QRXMacFS LIST SYS:Storage/TestImages/MacFS/apm.img 2 >>C:macfs.log' \
    'C:QRXMacFS INFO SYS:Storage/TestImages/MacFS/mfs400.badcsum.img >>C:macfs.log' \
    'C:QRXMacFS INFO SYS:Storage/TestImages/MacFS/mfs400.shortndif.img >>C:macfs.log' \
    'C:QRXMacFS LIST SYS:Storage/TestImages/MacFS/hfs800.img >>C:macfs.log' \
    'C:QRXMacFS INFO SYS:Storage/TestImages/MacFS/hfs800.img >>C:macfs.log' \
    'C:QRXMacFS EXTRACT SYS:Storage/TestImages/MacFS/hfs800.img Frag RAM:frag.bin >>C:macfs.log' \
    'C:QRXMacFS EXTRACT SYS:Storage/TestImages/MacFS/hfs800.img Docs:Note RAM:note.bin >>C:macfs.log' \
    'C:QRXMacFS CAT SYS:Storage/TestImages/MacFS/hfs800.img Docs >>C:macfs.log' \
    'C:QRXMacFS LIST SYS:Storage/TestImages/MacFS/hfsp.img >>C:macfs.log' \
    'C:QRXMacFS INFO SYS:Storage/TestImages/MacFS/hfsp.img >>C:macfs.log' \
    'C:QRXMacFS EXTRACT SYS:Storage/TestImages/MacFS/hfsp.img Frag RAM:hfsp-frag.bin >>C:macfs.log' \
    'C:QRXMacFS EXTRACT SYS:Storage/TestImages/MacFS/hfsp.img Docs:Note RAM:hfsp-note.bin >>C:macfs.log' \
    'C:QRXMacFS EXTRACT SYS:Storage/TestImages/MacFS/hfsp.img Payload RAM:hfsp-rsrc.bin --rsrc >>C:macfs.log' \
    'C:QRXMacFS LIST SYS:Storage/TestImages/MacFS/hfsp.wrapper.img >>C:macfs.log' \
    'C:QRXMacFS FORK SYS:Storage/TestImages/MacFS/hello.as >>C:macfs.log' \
    'C:QRXMacFS FORK SYS:Storage/TestImages/MacFS/hello.as RAM:as-data.bin >>C:macfs.log' \
    'C:QRXMacFS FORK SYS:Storage/TestImages/MacFS/hello.as RAM:as-rsrc.bin --rsrc >>C:macfs.log' \
    'C:QRXMacFS FORK SYS:Storage/TestImages/MacFS/payload.ad >>C:macfs.log' \
    'C:QRXMacFS FORK SYS:Storage/TestImages/MacFS/payload.mb >>C:macfs.log' \
    'C:QRXMacFS FORK SYS:Storage/TestImages/MacFS/payload.mb RAM:mb-data.bin >>C:macfs.log' \
    'C:QRXMacFS FORK SYS:Storage/TestImages/MacFS/runs.hqx >>C:macfs.log' \
    'C:QRXMacFS FORK SYS:Storage/TestImages/MacFS/runs.hqx RAM:hqx-data.bin >>C:macfs.log' \
    > "$STARTUP"

set +e
QRX_FULL_ADF="$BUNDLE/boot.adf" "$HARNESS" --cpu 68040 \
    --rom "$BUNDLE/aros-rom.bin" --ext-rom "$BUNDLE/aros-ext.bin" \
    --share "$TREE" --share-rw --apps /tmp/qrx-apps-empty --seconds 25 \
    > /tmp/qrx-macfs-harness.log 2>&1
harness_rc=$?
set -e

if [ ! -f "$TREE/C/$BOOT_LOG" ]; then
    echo "guest produced no $BOOT_LOG (harness rc=$harness_rc)" >&2
    tail -20 /tmp/qrx-macfs-harness.log >&2 || true
    exit 1
fi
cp "$TREE/C/$BOOT_LOG" "$LOG"

fail=0
checks=0
expect() {
    checks=$((checks + 1))
    if grep -Fqx -- "$1" "$LOG"; then
        return 0
    fi
    printf 'MISSING: %s\n' "$1"
    fail=$((fail + 1))
    return 1
}

expect 'container=raw size=409600 volume=mfs offset=0'
expect 'volume name="Test Disk" blocks=391 alloc=1024 free=387 files=3'
expect 'file 1 "Hello" type=TEXT creator=ttxt data=5 rsrc=0 locked=0'
expect 'file 2 "Payload" type=BINA creator=QRX+ data=1300 rsrc=64 locked=0'
expect 'file 3 "Empty" type=TEXT creator=ttxt data=0 rsrc=0 locked=0'
expect 'extracted="Hello" bytes=5'
expect 'Hello'
expect 'error=not-found'
expect 'container=dc42 size=409600 name="Test Disk" checksum=ok volume=mfs offset=0'
expect 'container=ndif size=409600 name="Test Disk" blocks=800 volume=mfs offset=0'
expect 'container=apm size=1048576 partitions=2 volume=mfs offset=32768'
expect 'part 1 start=1 count=2 type="Apple_partition_map" name="Apple"'
expect 'part 2 start=64 count=800 type="Apple_HFS" name="MacTest"'
expect 'error=checksum'
expect 'error=ndif'
expect 'container=raw size=819200 volume=hfs offset=0'
expect 'volume fs=hfs name="Test Disk" blocks=1580 alloc=512 free=1557'
expect 'volume files=4 dirs=2'
expect 'entry 1 parent=1 dir "Docs" cnid=16 icon=0'
expect 'entry 2 parent=1 file "Frag" cnid=5 type=BINA creator=QRX+ flags=0 icon=0 data=2048 rsrc=0'
expect 'entry 3 parent=1 file "Hello" cnid=2 type=TEXT creator=ttxt flags=256 icon=0 data=5 rsrc=0'
expect 'entry 4 parent=1 file "Payload" cnid=3 type=BINA creator=QRX+ flags=0 icon=0 data=1300 rsrc=64'
expect 'entry 5 parent=16 file "Note" cnid=17 type=TEXT creator=ttxt flags=0 icon=0 data=700 rsrc=0'
expect 'extracted="Frag" bytes=2048'
expect 'extracted="Note" bytes=700'
expect 'error=is-dir'
expect 'container=raw size=2097152 volume=hfs+ offset=0'
expect 'volume fs=hfs+ name="Test Disk" blocks=2048 blocksize=1024 free=2022'
expect 'volume files=5 dirs=2 journal=0 wrapped=0'
expect 'entry 2 parent=2 dir "Docs" cnid=16 icon=0'
expect 'entry 3 parent=2 file "Frag" cnid=5 type=BINA creator=QRX+ flags=0 icon=0 data=9000 rsrc=0'
expect 'entry 4 parent=2 file "Hello" cnid=2 type=TEXT creator=ttxt flags=256 icon=0 data=5 rsrc=0'
expect 'entry 5 parent=2 file "Payload" cnid=3 type=BINA creator=QRX+ flags=0 icon=0 data=1300 rsrc=64'
expect 'entry 6 parent=16 file "Note" cnid=17 type=TEXT creator=ttxt flags=0 icon=0 data=700 rsrc=0'
expect 'extracted="Frag" bytes=9000'
expect 'extracted="Note" bytes=700'
expect 'extracted="Payload" bytes=64'
expect 'container=raw size=2101248 volume=hfs+ offset=0'
expect 'volume files=5 dirs=2 journal=0 wrapped=1'
expect 'fork format=applesingle name="Hello" type=TEXT creator=ttxt flags=256 data=5 rsrc=64 crc=none'
expect 'fork format=appledouble name="Payload" type=BINA creator=QRX+ flags=128 data=0 rsrc=64 crc=none'
expect 'fork format=macbinary name="Payload" type=BINA creator=QRX+ flags=256 data=260 rsrc=64 crc=ok'
expect 'fork format=binhex name="Runs" type=BINA creator=QRX+ flags=0 data=260 rsrc=64 crc=none'
expect 'extracted="Hello" bytes=5'
expect 'extracted="Hello" bytes=64'
expect 'extracted="Payload" bytes=260'
expect 'extracted="Runs" bytes=260'

volume_lines=$(grep -Fc 'volume name="Test Disk" blocks=391 alloc=1024 free=387 files=3' "$LOG" || true)
checks=$((checks + 1))
if [ "$volume_lines" -lt 3 ]; then
    printf 'MISSING: the volume was listed %s times, expected at least 3\n' "$volume_lines"
    fail=$((fail + 1))
fi

restore
trap - EXIT HUP INT TERM

if [ "$fail" -ne 0 ]; then
    echo "macfs guest: FAIL ($fail of $checks checks)"
    exit 1
fi
echo "macfs guest: PASS ($checks checks, harness rc=$harness_rc)"
shasum -a 256 "$STARTUP"
