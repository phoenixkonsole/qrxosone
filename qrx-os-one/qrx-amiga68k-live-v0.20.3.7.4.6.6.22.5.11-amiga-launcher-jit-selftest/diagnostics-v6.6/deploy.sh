#!/usr/bin/env bash
set -Eeuo pipefail
IFS=$'\n\t'
REL="0.20.3.7.4.6.6.22.5.11"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="${QRX_DEADWOOD_WORKSPACE:-/opt/qrx-os-one/deadwood-aros-build}"
AROS_REF="${QRX_DEADWOOD_REF:-29dfac42dd55cddf813c695ab603f19ce2108656}"
OUT="${QRX_NATIVE_LIVECD_OUT:-/tmp/qrx-os-one-amiga68k-live-v${REL}-native-aros-abi-packaging-proof.iso}"
JOBS="${MAKE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
AROS_CC_OVERRIDE="${QRX_AROS_CC:-}"
say(){ printf '[QRX-v%s-ABIProof] %s\n' "$REL" "$*" >&2; }
die(){ printf '[QRX-v%s-ABIProof] ERROR: %s\n' "$REL" "$*" >&2; exit 1; }
[[ $EUID -eq 0 ]] || die "run with sudo/root"
SRC="$WORKSPACE/AROS"; CORE="$WORKSPACE/core-amiga-m68k"; SYSROOT="$CORE/bin/amiga-m68k/AROS"; HUNKROOT="$CORE/bin/amiga-m68k/AROS.HUNK"
[[ -d "$SRC/.git" && -d "$SYSROOT/QRX/C" ]] || die "Deadwood/.5.9 baseline missing"
COMMIT="$(git -C "$SRC" rev-parse HEAD)"; [[ "$COMMIT" == "$AROS_REF" ]] || die "Deadwood commit mismatch: $COMMIT"
mkdir -p "$SYSROOT/QRX/Diagnostics" "$SYSROOT/QRX/Documents" "$HUNKROOT/QRX/C" "$HUNKROOT/QRX/S" "$HUNKROOT/QRX/Diagnostics" "$HUNKROOT/QRX/Documents"
REPORT="$SYSROOT/QRX/Diagnostics/NATIVE-ABI-PACKAGING-PROOF.txt"; : > "$REPORT"
exec > >(tee -a "$REPORT") 2>&1
say "Starting strict compiler/link/packaging proof"
AROS_CC="$AROS_CC_OVERRIDE"
if [[ -z "$AROS_CC" ]]; then
 for c in "$CORE/bin/linux-x86_64/tools/m68k-amiga-aros-gcc" "$CORE/bin/linux-x86_64/tools/m68k-aros-gcc" "$WORKSPACE/toolchain-core-m68k/bin/linux-x86_64/tools/m68k-amiga-aros-gcc" "$WORKSPACE/toolchain-core-m68k/m68k-aros-gcc"; do [[ -x "$c" ]] && AROS_CC="$c" && break; done
fi
[[ -x "$AROS_CC" ]] || die "Deadwood native AROS compiler missing"
case "$AROS_CC" in *m68k-amigaos-gcc) die "legacy AmigaOS compiler rejected";; esac
echo "compiler=$AROS_CC"; "$AROS_CC" --version | head -3
STARTUP="$(find "$CORE" -type f -name startup.o -print 2>/dev/null | grep -E '/(Development|lib|Libs|gen)/' | head -1 || true)"
[[ -f "$STARTUP" ]] || die "aligned AROS startup.o missing"
echo "startup=$STARTUP"
STARTUP_DIR="$(dirname "$STARTUP")"
mapfile -t INCDIRS < <(find "$CORE/bin/amiga-m68k/AROS.HUNK/Development/include" "$CORE/bin/amiga-m68k/AROS/Development/include" -type d -print 2>/dev/null | awk '!seen[$0]++')
mapfile -t LIBDIRS < <(find "$CORE" -type f -name 'lib*.a' -printf '%h\n' 2>/dev/null | sort -u)
((${#INCDIRS[@]})) || die "AROS includes missing"; ((${#LIBDIRS[@]})) || die "AROS libs missing"
printf 'include=%s\n' "${INCDIRS[@]}"; printf 'libdir=%s\n' "${LIBDIRS[@]}"
CCARGS=(-Os -m68020 -fno-pic -fno-PIC -Wall -Wextra -B"$STARTUP_DIR/")
for d in "${INCDIRS[@]}"; do CCARGS+=("-I$d"); done
for d in "${LIBDIRS[@]}"; do CCARGS+=("-L$d"); done
STAGE="$(mktemp -d /var/tmp/qrx-abi-proof.XXXXXX)"; trap 'rm -rf "$STAGE"' EXIT
# Build two tiny ABI probes plus Diagnostics with the exact same compiler/startup/libs.
say "Building minimal DOS ABI probe"
"$AROS_CC" "${CCARGS[@]}" -o "$STAGE/QRXArosAbi02.elf" "$ROOT/../base-v5.9/tools/qrx_abi02_dos.c"
say "Building minimal Intuition ABI probe"
"$AROS_CC" "${CCARGS[@]}" -o "$STAGE/QRXArosAbi05.elf" "$ROOT/../base-v5.9/tools/qrx_abi05_window.c"
say "Building QRXDiagnostics"
"$AROS_CC" "${CCARGS[@]}" -o "$STAGE/QRXDiagnostics.elf" "$ROOT/tools/qrx_diagnostics.c"
say "Installing QRXAI bilingual knowledge pack"
mkdir -p "$SYSROOT/QRX/AI" "$HUNKROOT/QRX/AI"
cp -f "$ROOT/tools/qrxai.kb" "$SYSROOT/QRX/AI/qrxai.kb"
cp -f "$ROOT/tools/qrxai.kb" "$HUNKROOT/QRX/AI/qrxai.kb"

say "Building QRXAI local 68K chatbot"
"$AROS_CC" "${CCARGS[@]}" -o "$STAGE/QRXAI.elf" "$ROOT/tools/qrx_ai.c"
say "Building QRX htop and Compatibility Center"
"$AROS_CC" "${CCARGS[@]}" -o "$STAGE/QRXHtop.elf" "$ROOT/tools/qrx_htop.c"
"$AROS_CC" "${CCARGS[@]}" -o "$STAGE/QRXCompatCenter.elf" "$ROOT/tools/qrx_compat_center.c"
say "Building unified QRX application UX"
"$AROS_CC" "${CCARGS[@]}" -DAPP_WRITER -o "$STAGE/QRXWriter.elf" "$ROOT/tools/qrx_unified_apps.c"
"$AROS_CC" "${CCARGS[@]}" -DAPP_SHEET -o "$STAGE/QRXSpreadsheet.elf" "$ROOT/tools/qrx_unified_apps.c"
"$AROS_CC" "${CCARGS[@]}" -DAPP_PRESENTER -o "$STAGE/QRXPresenter.elf" "$ROOT/tools/qrx_unified_apps.c"
"$AROS_CC" "${CCARGS[@]}" -DAPP_HARDWARE -o "$STAGE/QRXHardware.elf" "$ROOT/tools/qrx_unified_apps.c"
"$AROS_CC" "${CCARGS[@]}" -DAPP_SETTINGS -o "$STAGE/QRXSettings.elf" "$ROOT/tools/qrx_unified_apps.c"
"$AROS_CC" "${CCARGS[@]}" -DAPP_WELCOME -o "$STAGE/QRXWelcome.elf" "$ROOT/tools/qrx_unified_apps.c"
say "Installing QRX Web local test suite"
cp -f "$ROOT/tools/QRX-test-suite-page.html" "$SYSROOT/QRX/Documents/QRX-test-suite-page.html"
cp -f "$ROOT/tools/QRX-test-suite-page.html" "$HUNKROOT/QRX/Documents/QRX-test-suite-page.html"
cp -f "$ROOT/tools/qrx-test-image.ppm" "$SYSROOT/QRX/Documents/qrx-test-image.ppm"
cp -f "$ROOT/tools/qrx-test-image.ppm" "$HUNKROOT/QRX/Documents/qrx-test-image.ppm"
say "Preflight: verifying C runtime declarations required by network/QRX86 sources"
python3 - "$ROOT/tools/qrx_net.c" "$ROOT/tools/qrx_win_guest_loader.c" "$ROOT/tools/qrx_win_guest_runtime.c" "$ROOT/tools/qrx86_grt_exec.c" <<'PYHDR'
from pathlib import Path
import sys
checks={sys.argv[1]:['#include <stdio.h>','#include <stdlib.h>','extern int snprintf'],sys.argv[2]:['#include <stdio.h>','#include <stdlib.h>','typedef void QRX_FILE','extern QRX_FILE *fopen','extern void *malloc'],sys.argv[3]:['typedef void QRX_FILE','extern QRX_FILE *fopen','extern size_t fwrite','extern void *malloc'],sys.argv[4]:['typedef void QRX_FILE','extern QRX_FILE *fopen','extern void *malloc']}
for fn,need in checks.items():
    s=Path(fn).read_text()
    miss=[x for x in need if x not in s]
    if miss: raise SystemExit(f"{fn}: missing required headers: {', '.join(miss)}")
    print(f"HEADER-PROOF {Path(fn).name}: AROS libc declarations present")
PYHDR
say "Building ARES Antivirus and QRX Web"
"$AROS_CC" "${CCARGS[@]}" -o "$STAGE/ARESAntivirus.elf" "$ROOT/tools/ares_antivirus.c"
"$AROS_CC" "${CCARGS[@]}" -o "$STAGE/QRXWeb.elf" "$ROOT/tools/qrx_web.c" "$ROOT/tools/qrx_net.c"
say "Building QRX86 guest PE32 recursive module loader"
"$AROS_CC" "${CCARGS[@]}" -o "$STAGE/QRXWinGuestLoader.elf" "$ROOT/tools/qrx_win_guest_loader.c"
say "Building QRX86 ReactOS init runtime (DllMain/TLS + ntdll host bridge)"
"$AROS_CC" "${CCARGS[@]}" -o "$STAGE/QRXWinGuestRuntime.elf" "$ROOT/tools/qrx_win_guest_runtime.c"
say "Building QRX86 QRXGRT executor + m68k tiered JIT foundation"
"$AROS_CC" "${CCARGS[@]}" -o "$STAGE/QRX86GRTExec.elf" "$ROOT/tools/qrx86_grt_exec.c"

python3 - "$STAGE" <<'PY'
from pathlib import Path
import struct,sys,hashlib
for p in sorted(Path(sys.argv[1]).glob('*.elf')):
 b=p.read_bytes();
 if b[:4]!=b'\x7fELF': raise SystemExit(f'{p.name}: compiler did not emit ELF')
 endian='>' if b[5]==2 else '<'; machine=struct.unpack(endian+'H',b[18:20])[0]
 if machine!=4: raise SystemExit(f'{p.name}: ELF machine={machine}, expected m68k=4')
 print(f'ELF-PROOF {p.name} size={len(b)} sha256={hashlib.sha256(b).hexdigest()} machine=m68k')
PY
# Deadwood LiveCD executes HUNK files. Find the actual converter produced by this same core.
ELF2HUNK="$(make -s -C "$CORE" -pn 2>/dev/null | awk -F' = ' '/^ELF2HUNK = /{print $2; exit}')"
if [[ -n "$ELF2HUNK" && "$ELF2HUNK" != /* ]]; then ELF2HUNK="$CORE/$ELF2HUNK"; fi
[[ -x "$ELF2HUNK" ]] || ELF2HUNK="$(find "$CORE" -type f -name elf2hunk -perm -111 -print 2>/dev/null | head -1 || true)"
[[ -x "$ELF2HUNK" ]] || die "Deadwood elf2hunk missing"
echo "elf2hunk=$ELF2HUNK"
convert_one(){ local src="$1" dst="$2"; mkdir -p "$(dirname "$dst")"; case "$(od -An -tx1 -N4 "$src" | tr -d ' \n')" in 000003f3) cp -f "$src" "$dst";; 7f454c46) "$ELF2HUNK" "$src" "$dst" || die "elf2hunk failed for $(basename "$src")";; *) die "unsupported source format: $src";; esac; [[ "$(od -An -tx1 -N4 "$dst" | tr -d ' \n')" == 000003f3 ]] || die "HUNK proof failed: $dst"; echo "HUNK-PROOF $(basename "$dst") size=$(stat -c %s "$dst") sha256=$(sha256sum "$dst"|awk '{print $1}')"; }
# Install proof tools in BOTH source payload and HUNK payload; no ELF fallback is accepted for these.
for n in QRXArosAbi02 QRXArosAbi05 QRXDiagnostics QRXAI QRXHtop QRXCompatCenter QRXWriter QRXSpreadsheet QRXPresenter QRXHardware QRXSettings QRXWelcome ARESAntivirus QRXWeb QRXWinGuestLoader QRXWinGuestRuntime QRX86GRTExec; do convert_one "$STAGE/$n.elf" "$HUNKROOT/QRX/C/$n"; cp -f "$HUNKROOT/QRX/C/$n" "$SYSROOT/QRX/C/$n"; done
# Re-prove critical existing QRX applications. Convert their native AROS ELF to HUNK explicitly.
CRITICAL=(QRXDesktop QRXWriter QRXSpreadsheet QRXPresenter QRXHardware QRXSettings QRXWelcome ARESAntivirus QRXWeb QRXWinGuestLoader QRXWinGuestRuntime QRX86GRTExec QRXUnix QRXELF QRXPosixTest)
for n in "${CRITICAL[@]}"; do [[ -s "$SYSROOT/QRX/C/$n" ]] || die "critical runtime missing: $n"; convert_one "$SYSROOT/QRX/C/$n" "$HUNKROOT/QRX/C/$n"; done
cp -f "$ROOT/tools/qrx-system-test" "$SYSROOT/QRX/S/SystemTest"; cp -f "$ROOT/tools/qrx-system-test" "$HUNKROOT/QRX/S/SystemTest"
# 22.5.11: classic AmigaOS HUNK launcher (amigarun/amigacheck) + opt-in JIT self-test.
say "Building 22.5.11 QRXAmigaRun / amigacheck / qrx-jit-selftest / HUNK test programs"
RT2211="$ROOT/../amiga-runtime-22.5.11"
[[ -x "$RT2211/build-target.sh" ]] || die "22.5.11 amiga-runtime sources missing"
QRX_AROS_CC="$AROS_CC" QRX_ELF2HUNK="$ELF2HUNK" QRX_AROS_CFLAGS="$(printf '%s ' "${CCARGS[@]:6}")" \
  bash "$RT2211/build-target.sh" "$STAGE/rt2211" || die "22.5.11 target build failed"
mkdir -p "$SYSROOT/QRX/Tests/Amiga" "$HUNKROOT/QRX/Tests/Amiga" "$SYSROOT/QRX/Docs" "$HUNKROOT/QRX/Docs"
for n in QRXAmigaRun amigarun amigacheck qrx-jit-selftest; do
  convert_one "$STAGE/rt2211/hunk/$n" "$HUNKROOT/QRX/C/$n"; cp -f "$HUNKROOT/QRX/C/$n" "$SYSROOT/QRX/C/$n"
done
for n in QRXHunkTest QRXHunkTestNeedLib; do
  convert_one "$STAGE/rt2211/hunk/$n" "$HUNKROOT/QRX/Tests/Amiga/$n"; cp -f "$HUNKROOT/QRX/Tests/Amiga/$n" "$SYSROOT/QRX/Tests/Amiga/$n"
done
for f in "$STAGE/rt2211/fixtures/"*; do cp -f "$f" "$SYSROOT/QRX/Tests/Amiga/"; cp -f "$f" "$HUNKROOT/QRX/Tests/Amiga/"; done
cp -f "$RT2211/target/TestAmigaRun" "$SYSROOT/QRX/S/TestAmigaRun"; cp -f "$RT2211/target/TestAmigaRun" "$HUNKROOT/QRX/S/TestAmigaRun"
for d in JIT-SELFTEST-22.5.11.txt AMIGA-LAUNCHER-22.5.11.txt; do
  cp -f "$RT2211/$d" "$SYSROOT/QRX/Docs/$d"; cp -f "$RT2211/$d" "$HUNKROOT/QRX/Docs/$d"
done
# qrx-jit-selftest must stay opt-in: never referenced by boot or ReactOS/QRX86 scripts.
if grep -rlsI 'qrx-jit-selftest' "$SYSROOT/S" "$SYSROOT/QRX/S" "$HUNKROOT/QRX/S" 2>/dev/null | grep -v '/TestAmigaRun$' | grep -q .; then
  die "qrx-jit-selftest referenced from a startup/runtime script; it must stay manual"
fi
cat > "$SYSROOT/QRX/Diagnostics/ABI-PROOF-README.txt" <<EOF
QRX $REL strict Deadwood native AROS ABI packaging proof
Compiler: $AROS_CC
Startup: $STARTUP
Converter: $ELF2HUNK
Policy: critical QRX executables in final LiveCD MUST be Amiga HUNK (000003f3); ELF fallback forbidden.
EOF
cp -f "$SYSROOT/QRX/Diagnostics/ABI-PROOF-README.txt" "$HUNKROOT/QRX/Diagnostics/ABI-PROOF-README.txt"
say "Installing complete ReactOS System32 PE runtime payload + three-app tests"
ROS32="$SYSROOT/QRX/Windows/ReactOS/System32"
[[ -d "$ROS32" ]] || die "ReactOS System32 missing"
mkdir -p "$HUNKROOT/QRX/Windows/ReactOS/System32" "$SYSROOT/QRX/Windows/Tests" "$HUNKROOT/QRX/Windows/Tests" "$SYSROOT/QRX/Diagnostics" "$HUNKROOT/QRX/Diagnostics"
# PE files are guest data, not Amiga executables: copy verbatim. Runtime loads dependencies on demand.
find "$ROS32" -maxdepth 1 -type f \( -iname '*.dll' -o -iname '*.exe' -o -iname '*.cpl' \) -print0 | while IFS= read -r -d '' f; do cp -f "$f" "$HUNKROOT/QRX/Windows/ReactOS/System32/$(basename "$f")"; done
# ReactOS releases do not all ship the same accessory set. calc/notepad are required;
# the third smoke app is selected deterministically from known PE32 userland tools.
for n in calc.exe notepad.exe; do
  [[ -s "$ROS32/$n" ]] || die "ReactOS required smoke test missing: $n"
  cp -f "$ROS32/$n" "$SYSROOT/QRX/Windows/Tests/$n"
  cp -f "$ROS32/$n" "$HUNKROOT/QRX/Windows/Tests/$n"
done
ROS_THIRD=""
for n in clock.exe mspaint.exe wordpad.exe regedit.exe taskmgr.exe charmap.exe cmd.exe; do
  if [[ -s "$ROS32/$n" ]]; then ROS_THIRD="$n"; break; fi
done
[[ -n "$ROS_THIRD" ]] || die "ReactOS third smoke app missing (tried clock/mspaint/wordpad/regedit/taskmgr/charmap/cmd)"
printf '%s\n' "$ROS_THIRD" > "$SYSROOT/QRX/Diagnostics/REACTOS-THIRD-APP.txt"
cp -f "$SYSROOT/QRX/Diagnostics/REACTOS-THIRD-APP.txt" "$HUNKROOT/QRX/Diagnostics/REACTOS-THIRD-APP.txt"
cp -f "$ROS32/$ROS_THIRD" "$SYSROOT/QRX/Windows/Tests/$ROS_THIRD"
cp -f "$ROS32/$ROS_THIRD" "$HUNKROOT/QRX/Windows/Tests/$ROS_THIRD"
# ReactOS Solitaire is normally sol.exe. Keep this optional because accessory sets can differ.
ROS_SOL=""
for n in sol.exe solitaire.exe; do
  if [[ -s "$ROS32/$n" ]]; then ROS_SOL="$n"; break; fi
done
if [[ -n "$ROS_SOL" ]]; then
  printf 'present=%s\npath=QRX:Windows/ReactOS/System32/%s\n' "$ROS_SOL" "$ROS_SOL" > "$SYSROOT/QRX/Diagnostics/REACTOS-SOLITAIRE.txt"
  python3 "$ROOT/tools/reactos-pe-audit.py" "$ROS32" "$ROS_SOL" > "$SYSROOT/QRX/Diagnostics/REACTOS-SOLITAIRE-PE-DEPENDENCIES.json" || die "ReactOS Solitaire PE dependency closure incomplete"
  cp -f "$SYSROOT/QRX/Diagnostics/REACTOS-SOLITAIRE-PE-DEPENDENCIES.json" "$HUNKROOT/QRX/Diagnostics/REACTOS-SOLITAIRE-PE-DEPENDENCIES.json"
  cat > "$SYSROOT/QRX/S/RunReactOSSolitaire" <<EOF_SOL
Echo "=== ReactOS Solitaire through QRX86 ==="
QRX:C/QRXWinGuestRuntime --init-plan --bridge-dump --emit-handoff T:QRX-solitaire.qrxgrt QRX:Windows/ReactOS/System32/$ROS_SOL
QRX:C/QRX86GRTExec --jit=on T:QRX-solitaire.qrxgrt
EOF_SOL
else
  printf 'present=no\nsearched=sol.exe,solitaire.exe\n' > "$SYSROOT/QRX/Diagnostics/REACTOS-SOLITAIRE.txt"
  cat > "$SYSROOT/QRX/S/RunReactOSSolitaire" <<'EOF_SOL'
Echo "ReactOS Solitaire is not present in this ReactOS System32 image."
Type QRX:Diagnostics/REACTOS-SOLITAIRE.txt
EOF_SOL
fi
cp -f "$SYSROOT/QRX/Diagnostics/REACTOS-SOLITAIRE.txt" "$HUNKROOT/QRX/Diagnostics/REACTOS-SOLITAIRE.txt"
cp -f "$SYSROOT/QRX/S/RunReactOSSolitaire" "$HUNKROOT/QRX/S/RunReactOSSolitaire"
python3 "$ROOT/tools/reactos-pe-audit.py" "$ROS32" calc.exe notepad.exe "$ROS_THIRD" > "$SYSROOT/QRX/Diagnostics/REACTOS-PE-DEPENDENCIES.json" || die "ReactOS PE dependency closure incomplete; see REACTOS-PE-DEPENDENCIES.json"
cp -f "$SYSROOT/QRX/Diagnostics/REACTOS-PE-DEPENDENCIES.json" "$HUNKROOT/QRX/Diagnostics/REACTOS-PE-DEPENDENCIES.json"
(cd "$ROS32" && find . -maxdepth 1 -type f \( -iname '*.dll' -o -iname '*.exe' -o -iname '*.cpl' \) -print0 | sort -z | xargs -0 sha256sum) > "$SYSROOT/QRX/Diagnostics/REACTOS-SYSTEM32.sha256"
cp -f "$SYSROOT/QRX/Diagnostics/REACTOS-SYSTEM32.sha256" "$HUNKROOT/QRX/Diagnostics/REACTOS-SYSTEM32.sha256"
cat > "$SYSROOT/QRX/S/TestReactOS3" <<'EOF_ROS3'
Echo "=== QRX86 ReactOS 3-app smoke test ==="
Echo "[1/3] Calculator"
QRX:C/QRXWinGuestRuntime --init-plan --bridge-dump --emit-handoff T:QRX-calc.qrxgrt QRX:Windows/ReactOS/System32/calc.exe
QRX:C/QRX86GRTExec --jit=on T:QRX-calc.qrxgrt
Echo "[2/3] Notepad"
QRX:C/QRXWinGuestRuntime --init-plan --bridge-dump --emit-handoff T:QRX-notepad.qrxgrt QRX:Windows/ReactOS/System32/notepad.exe
QRX:C/QRX86GRTExec --jit=on T:QRX-notepad.qrxgrt
Echo "[3/3] Selected ReactOS accessory"
Type QRX:Diagnostics/REACTOS-THIRD-APP.txt
QRX:C/QRXRunReactOSThird
EOF_ROS3
cat > "$SYSROOT/QRX/C/QRXRunReactOSThird" <<EOF_THIRD
QRX:C/QRXWinGuestRuntime --init-plan --bridge-dump --emit-handoff T:QRX-third.qrxgrt QRX:Windows/ReactOS/System32/$ROS_THIRD
QRX:C/QRX86GRTExec --jit=on T:QRX-third.qrxgrt
EOF_THIRD
cp -f "$SYSROOT/QRX/C/QRXRunReactOSThird" "$HUNKROOT/QRX/C/QRXRunReactOSThird"
cp -f "$SYSROOT/QRX/S/TestReactOS3" "$HUNKROOT/QRX/S/TestReactOS3"
cp -f "$SYSROOT/QRX/S/TestReactOS3" "$SYSROOT/QRX/S/TestWin32"
cp -f "$SYSROOT/QRX/S/TestReactOS3" "$HUNKROOT/QRX/S/TestWin32"
cat > "$SYSROOT/QRX/S/TestWinGuestInit" <<'EOF_GINIT'
Echo "=== QRX86 ReactOS guest initialization / ntdll bridge ==="
QRX:C/QRXWinGuestRuntime --init-plan --bridge-dump --emit-handoff T:QRX-calc.qrxgrt QRX:Windows/ReactOS/System32/calc.exe
QRX:C/QRX86GRTExec --jit=on --init-only --trace T:QRX-calc.qrxgrt
Echo "The trace above is real guest TLS/DllMain execution; unsupported opcodes fail closed."
EOF_GINIT
cp -f "$SYSROOT/QRX/S/TestWinGuestInit" "$HUNKROOT/QRX/S/TestWinGuestInit"
cat > "$SYSROOT/QRX/S/TestWine" <<'EOF_WINETEST'
Echo "QRX Wine service/display/host smoke test"
QRX:C/QRXWineHostTest
QRX:C/QRXWineServicesTest
QRX:C/QRXWineDisplayTest
QRX:C/QRXWineBoot
EOF_WINETEST
cp -f "$SYSROOT/QRX/S/TestWine" "$HUNKROOT/QRX/S/TestWine"
echo "REACTOS-RUNTIME System32 guest payload files=$(find "$ROS32" -maxdepth 1 -type f \( -iname '*.dll' -o -iname '*.exe' -o -iname '*.cpl' \) | wc -l)"
say "Rebuilding LiveCD after explicit HUNK synchronization"
rm -f "$CORE/distfiles/aros-amiga-m68k.iso"
make -C "$CORE" -j"$JOBS" distfiles EBFHUNKROM=no
ISO="$CORE/distfiles/aros-amiga-m68k.iso"; [[ -s "$ISO" ]] || die "ISO not generated"
# End-to-end gate: extract the FINAL ISO, not the staging tree.
command -v xorriso >/dev/null 2>&1 || die "xorriso required for final ISO proof"
ISOEX="$STAGE/iso-root"; mkdir -p "$ISOEX"
xorriso -osirrox on -indev "$ISO" -extract / "$ISOEX" >/dev/null 2>&1 || die "final ISO extraction failed"
find_iso_file(){ local name="$1"; find "$ISOEX" -type f -path '*/QRX/C/*' -name "$name" -print -quit; }
FINAL=(QRXArosAbi02 QRXArosAbi05 QRXDiagnostics QRXAI QRXHtop QRXCompatCenter QRXAmigaRun amigarun amigacheck qrx-jit-selftest "${CRITICAL[@]}")
for n in "${FINAL[@]}"; do p="$(find_iso_file "$n")"; [[ -n "$p" && -s "$p" ]] || die "FINAL ISO GATE: QRX:C/$n missing"; magic="$(od -An -tx1 -N4 "$p"|tr -d ' \n')"; [[ "$magic" == 000003f3 ]] || die "FINAL ISO GATE: $n is not HUNK (magic=$magic)"; echo "ISO-PROOF QRX:C/$n format=HUNK size=$(stat -c %s "$p") sha256=$(sha256sum "$p"|awk '{print $1}')"; done
# Ensure diagnostics support file also made it into the image.
find "$ISOEX" -type f -path '*/QRX/S/SystemTest' -print -quit | grep -q . || die "FINAL ISO GATE: QRX:S/SystemTest missing"
find "$ISOEX" -type f -path '*/QRX/S/TestWine' -print -quit | grep -q . || die "FINAL ISO GATE: QRX:S/TestWine missing"
find "$ISOEX" -type f -path '*/QRX/S/TestReactOS3' -print -quit | grep -q . || die "FINAL ISO GATE: QRX:S/TestReactOS3 missing"
find "$ISOEX" -type f -path '*/QRX/S/TestWinGuestInit' -print -quit | grep -q . || die "FINAL ISO GATE: QRX:S/TestWinGuestInit missing"
find "$ISOEX" -type f -path '*/QRX/C/QRX86GRTExec' -print -quit | grep -q . || die "FINAL ISO GATE: QRX:C/QRX86GRTExec missing"
find "$ISOEX" -type f -path '*/QRX/S/RunReactOSSolitaire' -print -quit | grep -q . || die "FINAL ISO GATE: QRX:S/RunReactOSSolitaire missing"
for n in QRXHunkTest QRXHunkTestNeedLib; do p="$(find "$ISOEX" -type f -path "*/QRX/Tests/Amiga/$n" -print -quit)"; [[ -n "$p" && "$(od -An -tx1 -N4 "$p"|tr -d ' \n')" == 000003f3 ]] || die "FINAL ISO GATE: QRX:Tests/Amiga/$n missing or not HUNK"; echo "ISO-PROOF QRX:Tests/Amiga/$n format=HUNK"; done
find "$ISOEX" -type f -path '*/QRX/S/TestAmigaRun' -print -quit | grep -q . || die "FINAL ISO GATE: QRX:S/TestAmigaRun missing"
for n in calc.exe notepad.exe kernel32.dll ntdll.dll user32.dll gdi32.dll advapi32.dll; do find "$ISOEX" -type f -ipath "*/QRX/Windows/ReactOS/System32/$n" -print -quit | grep -q . || die "FINAL ISO GATE: ReactOS System32/$n missing"; done
find "$ISOEX" -type f -ipath "*/QRX/Windows/ReactOS/System32/$ROS_THIRD" -print -quit | grep -q . || die "FINAL ISO GATE: selected ReactOS third app $ROS_THIRD missing"
if [[ -n "$ROS_SOL" ]]; then
  find "$ISOEX" -type f -ipath "*/QRX/Windows/ReactOS/System32/$ROS_SOL" -print -quit | grep -q . || die "FINAL ISO GATE: ReactOS Solitaire $ROS_SOL missing"
  echo "ISO-PROOF ReactOS Solitaire=$ROS_SOL"
else
  echo "ISO-PROOF ReactOS Solitaire=not-present-in-source-image"
fi
find "$ISOEX" -type f -path '*/QRX/S/TestWin32' -print -quit | grep -q . || die "FINAL ISO GATE: QRX:S/TestWin32 missing"
KBISO="$(find "$ISOEX" -type f -path '*/QRX/AI/qrxai.kb' -print -quit)"
[[ -n "$KBISO" && -s "$KBISO" ]] || die "FINAL ISO GATE: QRX:AI/qrxai.kb missing"
echo "ISO-PROOF QRX:AI/qrxai.kb size=$(stat -c %s "$KBISO") sha256=$(sha256sum "$KBISO"|awk '{print $1}')"
say "FINAL ISO ABI/PACKAGING PROOF PASSED"
cp -f "$ISO" "$OUT"; sha256sum "$OUT" > "$OUT.sha256"
ZIP="${OUT%.iso}.zip"; rm -f "$ZIP" "$ZIP.sha256"; (cd "$(dirname "$OUT")" && zip -9 "$(basename "$ZIP")" "$(basename "$OUT")" "$(basename "$OUT").sha256" >/dev/null); sha256sum "$ZIP" > "$ZIP.sha256"
# Preserve proof next to build output and inside sysroot for next build.
cp -f "$REPORT" "${OUT%.iso}-abi-packaging-proof.txt"
say "DONE: $OUT"
say "ZIP: $ZIP"
say "Proof: ${OUT%.iso}-abi-packaging-proof.txt"
say "After boot test ONLY QRX:C/QRXArosAbi02 first, then QRX:C/QRXArosAbi05, then QRX:C/QRXDiagnostics. QRXDesktop comes after those three pass."
