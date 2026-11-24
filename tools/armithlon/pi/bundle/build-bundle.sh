#!/usr/bin/env bash
# QRX OS One for the Pi image, built on Linux from the checked Deadwood AROS 5b5fd4c release:
#   OUT/aros-rom.bin, aros-ext.bin   ROM (lowlevel patch applied when its bytes are there)
#   OUT/boot.adf                     AROS boot disk + HostFS handlers and the HostFS bootstrap
#   OUT/SystemFull/                  the AROS ISO tree + QRX desktop, apps and the host drivers
# build-bundle.sh OUT   (AROS_TC = toolchain-core-m68k, AROS_TOOLS = dir with elf2hunk,
#                        .build/qrx-sdk/bin/aros-gcc for build-desktop.py / build-system-apps.py)
set -Eeuo pipefail
REPO="$(cd "$(dirname "$0")/../../../.." && pwd)"
OUT="$1"; mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
REL="$REPO/upstream/aros-deadwood/m68k-build-5b5fd4c"
TC="${AROS_TC:-/opt/qrx-os-one/deadwood-aros-build/toolchain-core-m68k}"
E2H="${AROS_TOOLS:-/opt/qrx-os-one/deadwood-aros-build/core-amiga-m68k/bin/linux-x86_64/tools}/elf2hunk"
CC="$TC/m68k-aros-gcc"
G="$OUT/guest"

# 1. the release files, only with their recorded checksums
(cd "$REL" && sha256sum -c --quiet SHA256SUMS)
xz -dc "$REL/aros-amiga-m68k-5b5fd4c.iso.xz" > "$OUT/aros.iso"
(cd "$OUT" && sed 's/  .*/  aros.iso/' "$REL/SHA256SUMS.iso" | sha256sum -c --quiet)

# 2. ROM: the lowlevel fix of patch-boot-rom.py is for the ROM the Mac bundle uses; this
#    release ROM does not contain those bytes, so it is used unchanged (and that is said).
if python3 "$REPO/tools/armithlon/patch-boot-rom.py" "$REL/aros-amiga-m68k-rom.bin" "$OUT/aros-rom.bin" 2>/dev/null; then
  echo "ROM: lowlevel patch applied"
else
  cp "$REL/aros-amiga-m68k-rom.bin" "$OUT/aros-rom.bin"; echo "ROM: release ROM unchanged (patch bytes not present)"
fi
cp "$REL/aros-amiga-m68k-ext.bin" "$OUT/aros-ext.bin"

# 3. guest programs
bash "$REPO/tools/armithlon/pi/bundle/build-guest.sh" "$G" >/dev/null
app() { # name source...
  local n=$1; shift
  "$CC" -O2 -m68040 -fno-pic -fno-PIC -w "$@" -o "$G/$n.elf"
  "$E2H" "$G/$n.elf" "$G/$n" >/dev/null
}
app QRXWireless "$REPO/tools/armithlon/qrxwireless/QRXWireless.c"
app QRXSystem "$REPO/tools/armithlon/qrxsystem/QRXSystem.c"
app QRXNetwork "$REPO/tools/armithlon/qrxnetwork/QRXNetwork.c"
app QRXInfo "$REPO/tools/armithlon/qrxinfo/QRXInfo.c"
app QRXLoadDevice "$REPO/tools/armithlon/diskimage/qrx-load-device.c"
app QRXAppStore "$REPO/tools/armithlon/appstore/QRXAppStore.c"
AROS_TC="$TC" sh "$REPO/upstream/armithlon/2.6/engine/guest/aros/ahi/build-aros.sh" "$REPO" "$OUT/ahi" >/dev/null 2>&1
python3 "$REPO/tools/armithlon/build-desktop.py" >/dev/null
python3 "$REPO/tools/armithlon/build-system-apps.py" >/dev/null

# 4. the system tree
S="$OUT/SystemFull"; rm -rf "$S"; mkdir -p "$S"
xorriso -osirrox on -indev "$OUT/aros.iso" -extract / "$S" >/dev/null 2>&1
chmod -R u+w "$S"
cp "$REPO/tools/armithlon/Startup-Sequence-FullISO" "$S/S/Startup-Sequence"
mkdir -p "$S/QRX/C"
for f in QRXHostInput QRXMoreMemory QRXWireless QRXSystem QRXNetwork QRXInfo QRXLoadDevice QRXAppStore; do cp "$G/$f" "$S/C/$f"; done
cp "$REPO/.build/qrx-desktop/QRXDesktop.hunk" "$S/QRX/C/QRXDesktop"
cp "$REPO/.build/qrx-desktop/QRXDesktopPrefs.hunk" "$S/QRX/C/QRXDesktopPrefs"
for f in QRXWelcome QRXWriter QRXSpreadsheet QRXPresenter QRXHardware QRXSettings QRXIPFSPrefs; do
  cp "$REPO/.build/qrx-apps/$f" "$S/QRX/C/$f"
done
# Windows and Linux i386 programs (x86 runtime 22.5.12): C:qrx-win32, C:qrx-i386, C:QRXInput;
# the x86 code runs on the host door (the engine's x86 JIT). Two test programs in SYS:X86.
XR=$(ls -d "$REPO"/qrx-os-one/*/x86-runtime-22.5.12 | head -1)
DEV="$(dirname "$E2H")/../../amiga-m68k/AROS/Development"
QRX_AROS_CC="$CC" QRX_ELF2HUNK="$E2H" QRX_AROS_CFLAGS="-B$DEV/lib/ -I$DEV/include -L$DEV/lib" \
  bash "$XR/build-target.sh" "$OUT/x86rt" >/dev/null
for f in qrx-win32 qrx-i386 QRXInput; do cp "$OUT/x86rt/hunk/$f" "$S/C/$f"; done
mkdir -p "$S/X86"
cp "$XR/win32/apps/prebuilt/dxtest.exe" "$XR/win32/apps/prebuilt/qrxclock.exe" "$S/X86/"
# wallpapers and design icons (gen-assets.py: the Linux port of gen-assets.swift; fonts-inter)
python3 "$REPO/tools/armithlon/desktop/gen-assets.py" "$S/QRX" >/dev/null
# sound: the host AHI driver is the default mode (install-ahi.sh does the same on the Mac)
A="$OUT/ahi"
cp "$A/qrxhost.audio" "$S/Devs/AHI/qrxhost.audio"
cp "$A/QRXHOST" "$S/Devs/AudioModes/QRXHOST"
cp "$A/QRXAHITest" "$S/C/QRXAHITest"
mkdir -p "$S/Prefs/Env-Archive/Sys" "$S/Storage/AudioModes"
cp "$A/ahi.prefs" "$S/Prefs/Env-Archive/Sys/ahi.prefs"
[ -f "$S/Devs/AudioModes/PAULA" ] && mv "$S/Devs/AudioModes/PAULA" "$S/Storage/AudioModes/PAULA"

# 5. boot disk: the release boot disk + HostFS; its Startup-Sequence mounts the system tree
cp "$REL/bootdisk-amiga-m68k-3520.adf" "$OUT/boot.adf"
X() { xdftool "$OUT/boot.adf" "$@" >/dev/null; }
X makedir L
X write "$G/QRXHostFS" L/QRXHostFS
X write "$G/QRXAppsFS" L/QRXAppsFS
X write "$G/QRXHostMemory" C/QRXHostMemory
xdftool "$OUT/boot.adf" delete DEVS/Mountlist >/dev/null 2>&1 || true
X write "$REPO/tools/armithlon/Mountlist" DEVS/Mountlist
X delete S/Startup-Sequence
X write "$REPO/tools/armithlon/Startup-HostFS" S/Startup-Sequence
rm -f "$OUT/aros.iso"
echo "bundle: $OUT ($(du -sh "$S" | cut -f1) system tree)"
