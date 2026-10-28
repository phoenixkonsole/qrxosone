#!/usr/bin/env bash
set -Eeuo pipefail
IFS=$'\n\t'

REL="0.20.3.7.4.6.5.8"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="${QRX_DEADWOOD_WORKSPACE:-/opt/qrx-os-one/deadwood-aros-build}"
AROS_REPO="${QRX_DEADWOOD_REPO:-https://github.com/deadwood2/AROS.git}"
# Exact Deadwood source revision observed on 2026-08-16 in the QRX build workspace.
AROS_REF="${QRX_DEADWOOD_REF:-29dfac42dd55cddf813c695ab603f19ce2108656}"
REFERENCE_TAR="${QRX_DEADWOOD_REFERENCE_TAR:-/tmp/amiga-m68k-classic-20260815-012049.tar.gz}"
REFERENCE_SHA_EXPECTED="${QRX_DEADWOOD_REFERENCE_SHA256:-c2d7362aea32232952365cba6ab01848d62eb1ea70bb47d3726d4e7d211f8ab9}"
REACTOS_FILE="${QRX_REACTOS_FILE:-/tmp/ReactOS-0.4.15-x86-live.zip}"
QRX_V020_ROOT="${QRX_V020_ROOT:-}"
AROS_CC_OVERRIDE="${QRX_AROS_CC:-}"
OUT="${QRX_NATIVE_LIVECD_OUT:-/tmp/qrx-os-one-amiga68k-live-v0.20.3.7.4.6.5.9-deadwood-native-aros-abi-reactos.iso}"
JOBS="${MAKE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
ADF_SECTORS="${QRX_EMERGENCY_ADF_SECTORS:-3520}"
EBFHUNKROM="${QRX_EBFHUNKROM:-no}"
FORCE_CORE=0
INSTALL_DEPS=1

say(){ printf '[QRX-v0.20.3.7.4.6.5.9-CleanHost] %s
' "$*" >&2; }
die(){ printf '[QRX-v0.20.3.7.4.6.5.9-CleanHost] ERROR: %s
' "$*" >&2; exit 1; }
warn(){ printf '[QRX-v0.20.3.7.4.6.5.9-CleanHost] WARNING: %s
' "$*" >&2; }
have(){ command -v "$1" >/dev/null 2>&1; }

usage(){
cat <<EOF
QRX OS One Amiga 68K v0.20.3.7.4.6.5.9 – Requester-Free Volume Discovery / Clean QRX Desktop Host

Usage:
  sudo ./deploy.sh [options]

Options:
  --workspace PATH
  --aros-ref COMMIT
  --reference-tar PATH
  --reactos-file PATH
  --qrx-v020-root PATH
  --aros-cc PATH
  --output PATH
  --jobs N
  --force-core-rebuild
  --no-install-deps
  -h, --help

This release deliberately preserves the working v0.20.1 native AROS boot path:
  * core-amiga-m68k / Deadwood option 12
  * native distfiles packaging
  * EBFHUNKROM=no
  * 3520-sector Emergency-Boot ADF
It preserves the proven Deadwood native LiveCD boot path and keeps QRXDesktop autostart disabled
for one final manual smoke test. All QRX 68K runtime executables are built through the
Deadwood-native m68k-aros-gcc toolchain using startup objects and libraries from the same
core-amiga-m68k runtime tree. The legacy m68k-amigaos-gcc runtime path is not used.
EOF
}

while (($#)); do
 case "$1" in
  --workspace) WORKSPACE="$2"; shift 2;;
  --aros-ref) AROS_REF="$2"; shift 2;;
  --reference-tar) REFERENCE_TAR="$2"; shift 2;;
  --reactos-file) REACTOS_FILE="$2"; shift 2;;
  --qrx-v020-root) QRX_V020_ROOT="$2"; shift 2;;
  --aros-cc) AROS_CC_OVERRIDE="$2"; shift 2;;
  --output) OUT="$2"; shift 2;;
  --jobs) JOBS="$2"; shift 2;;
  --force-core-rebuild) FORCE_CORE=1; shift;;
  --no-install-deps) INSTALL_DEPS=0; shift;;
  -h|--help) usage; exit 0;;
  *) die "unknown option: $1";;
 esac
done

[[ "$EBFHUNKROM" == no ]] || die "v0.20.3 preserves EBFHUNKROM=no because that is the proven boot path"
[[ "$ADF_SECTORS" == 3520 ]] || die "v0.20.3 preserves Emergency ADF to 3520 sectors because that is the proven boot path"

if [[ $EUID -ne 0 ]]; then die "run with sudo/root"; fi

if [[ $INSTALL_DEPS -eq 1 ]]; then
  missing=()
  for t in git make gcc g++ python3 xorriso unzip file sha256sum mtools; do have "$t" || missing+=("$t"); done
  if ((${#missing[@]})); then
    say "Installing missing build dependencies"
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
      git gcc g++ make gawk bison flex bzip2 netpbm autoconf automake \
      libx11-dev libxext-dev libc6-dev liblzo2-dev libxxf86vm-dev libpng-dev \
      gcc-multilib libsdl1.2-dev byacc python3-mako libxcursor-dev cmake \
      genisoimage dh-make yasm curl nasm xorriso mtools unzip file python3 python3-pil
  fi
fi

mkdir -p "$WORKSPACE" "$(dirname "$OUT")"
SRC="$WORKSPACE/AROS"

if [[ ! -d "$SRC/.git" ]]; then
  say "Cloning Deadwood AROS"
  git clone "$AROS_REPO" "$SRC"
fi
say "Synchronizing Deadwood source"
git -C "$SRC" fetch --tags --prune origin
git -C "$SRC" checkout --detach "$AROS_REF"
git -C "$SRC" reset --hard "$AROS_REF"
COMMIT="$(git -C "$SRC" rev-parse HEAD)"
[[ "$COMMIT" == "$AROS_REF" ]] || die "source alignment failed: expected $AROS_REF, got $COMMIT"
say "Aligned Deadwood commit: $COMMIT"

[[ -f "$REFERENCE_TAR" ]] || die "Deadwood 2026 reference tar missing: $REFERENCE_TAR"
REFSHA="$(sha256sum "$REFERENCE_TAR" | awk '{print $1}')"
[[ "$REFSHA" == "$REFERENCE_SHA_EXPECTED" ]] || die "reference tar SHA mismatch: $REFSHA"
tar -tzf "$REFERENCE_TAR" | grep '/AROS/S/Startup-Sequence$' >/dev/null || die "reference tar lacks Startup-Sequence"
say "Reference tar verified: $REFSHA"

# Locate the proven QRX v0.20 payload builder already used by v0.20.1.
if [[ -z "$QRX_V020_ROOT" ]]; then
  for c in \
    /tmp/qrx-amiga68k-live-v0.20.1 \
    /tmp/qrx-amiga68k-live-v0.20 \
    /opt/qrx-os-one/qrx-amiga68k-live-v0.20 \
    /opt/qrx-os-one/releases/qrx-amiga68k-live-v0.20; do
    if [[ -f "$c/build-qrx-amiga68k-live-v0.20.sh" ]]; then QRX_V020_ROOT="$c"; break; fi
  done
fi
[[ -n "$QRX_V020_ROOT" && -f "$QRX_V020_ROOT/build-qrx-amiga68k-live-v0.20.sh" ]] \
  || die "v0.20 payload builder not found; use --qrx-v020-root PATH"
chmod +x "$QRX_V020_ROOT/build-qrx-amiga68k-live-v0.20.sh"
find "$QRX_V020_ROOT/tools" -maxdepth 1 -type f -name '*.sh' -exec chmod +x {} + 2>/dev/null || true
say "QRX payload builder: $QRX_V020_ROOT"

# Keep rebuild.sh aligned with the exact source revision.
cp -f "$SRC/scripts/rebuild.sh" "$WORKSPACE/rebuild.sh"
chmod +x "$WORKSPACE/rebuild.sh"

# Toolchain may be reused; it is not the runtime identity.
if [[ ! -d "$WORKSPACE/toolchain-core-m68k" ]]; then
  say "Building Deadwood m68k toolchain (option 11)"
  (cd "$WORKSPACE"; printf '11\n' | MAKE_JOBS="$JOBS" ./rebuild.sh)
else
  say "Reusing existing m68k toolchain"
fi

CORE="$WORKSPACE/core-amiga-m68k"
STAMP="$CORE/.qrx-deadwood-source-commit"
OLD=""
[[ -f "$STAMP" ]] && OLD="$(cat "$STAMP" 2>/dev/null || true)"

# v0.20.1 could reuse an old option-12 tree. v0.20.2 never does that silently.
if [[ $FORCE_CORE -eq 1 || "$OLD" != "$COMMIT" ]]; then
  say "Runtime core is stale/unproven (${OLD:-no stamp}); rebuilding option 12 from $COMMIT"
  rm -rf "$CORE"
  (cd "$WORKSPACE"; printf '12\n' | MAKE_JOBS="$JOBS" ./rebuild.sh)
  [[ -d "$CORE/bin/amiga-m68k/AROS" ]] || die "option 12 did not create the AROS runtime tree"
  printf '%s\n' "$COMMIT" > "$STAMP"
else
  say "Runtime core stamp matches $COMMIT; reuse allowed"
fi

SYSROOT="$CORE/bin/amiga-m68k/AROS"
[[ -d "$SYSROOT" ]] || die "runtime sysroot missing"

# Patch only the already-proven Emergency ADF geometry; do not alter boot architecture.
say "Locking proven Emergency-Boot geometry: 3520 sectors"
while IFS= read -r f; do
  sed -i 's/--size 1760/--size 3520/g' "$f"
done < <(find "$SRC/arch/m68k-amiga/boot/floppy" "$CORE/arch/m68k-amiga/boot/floppy" \
          -type f \( -name 'mmakefile' -o -name 'mmakefile.src' \) 2>/dev/null)

# Prepare ReactOS System32.
[[ -f "$REACTOS_FILE" ]] || die "ReactOS artifact missing: $REACTOS_FILE"
TMP="$(mktemp -d /var/tmp/qrx-v0203.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/ros" "$TMP/unpack"
case "$(file -b "$REACTOS_FILE")" in
 *"Zip archive"*)
   unzip -q "$REACTOS_FILE" -d "$TMP/unpack"
   ROSISO="$(find "$TMP/unpack" -type f -iname '*.iso' -printf '%s %p\n' | sort -nr | head -1 | cut -d' ' -f2-)"
   ;;
 *"ISO 9660"*|*"CD-ROM filesystem"*) ROSISO="$REACTOS_FILE";;
 *) die "unsupported ReactOS artifact";;
esac
[[ -f "$ROSISO" ]] || die "ReactOS ISO not found in artifact"
xorriso -osirrox on -indev "$ROSISO" -extract / "$TMP/ros" >/dev/null 2>&1
SYSTEM32="$(python3 - "$TMP/ros" <<'PY'
from pathlib import Path
import sys
root=Path(sys.argv[1]); best=None
for d in root.rglob("*"):
    if d.is_dir() and d.name.lower()=="system32":
        n=sum(1 for f in d.iterdir() if f.is_file() and f.suffix.lower() in {".dll",".exe"})
        x=(n,-len(d.parts),str(d))
        if best is None or x>best: best=x
if not best or best[0]<20: raise SystemExit(2)
print(best[2])
PY
)" || die "ReactOS System32 not found"
say "ReactOS System32: $SYSTEM32"

# QRX v0.20.3.7.4.6.5.9: migrate the complete runtime payload to the SAME native AROS ABI
# that has now passed QRXArosAbi00/01/02 on the target LiveCD.
export QRX_OVERLAY_SYSROOT="$SYSROOT"
export QRX_REACTOS_SYSTEM32="$SYSTEM32"
mkdir -p "$SYSROOT/QRX/C" "$SYSROOT/QRX/Diagnostics"

say "Installing QRXDesktop canonical 6.6.20 source (legacy deploy-time repatching disabled)"
QRX_DESKTOP_SRC="$(find "$QRX_V020_ROOT" -type f -name 'qrx_desktop.c' -print -quit)"
[[ -n "$QRX_DESKTOP_SRC" && -f "$QRX_DESKTOP_SRC" ]] \
  || die "qrx_desktop.c not found below $QRX_V020_ROOT"
CANONICAL_DESKTOP="$ROOT/tools/qrx_desktop.canonical-6.6.20.c"
CANONICAL_DESKTOP_SHA="$ROOT/tools/qrx_desktop.canonical-6.6.20.c.sha256"
[[ -s "$CANONICAL_DESKTOP" && -s "$CANONICAL_DESKTOP_SHA" ]] || die "canonical 6.6.20 desktop source/provenance missing"
(cd "$ROOT/tools" && sha256sum -c "$(basename "$CANONICAL_DESKTOP_SHA")") || die "canonical desktop SHA256 gate failed"
cp -f "$CANONICAL_DESKTOP" "$QRX_DESKTOP_SRC"
# Source-level feature gates replace fragile ordered text transformations.
for gate in \
  'QRX 6.6.20 canonical desktop source' \
  'SA_LikeWorkbench, TRUE' \
  'LockDosList(flags)' \
  'g_term_cwd' \
  'IDCMP_RAWKEY' \
  'qrx_draw_datetime' \
  'desktop_click_dock' \
  'qrx_windows_start_click' \
  'g_context_open' \
  'QRX:C/QRXWeb' \
  'QRX:C/ARESAntivirus'; do
  grep -Fq "$gate" "$QRX_DESKTOP_SRC" || die "canonical desktop feature gate missing: $gate"
done
if grep -Eq '"USB[0-9]+:"|USB0:-USB3' "$QRX_DESKTOP_SRC"; then die "canonical desktop contains fixed USB probes"; fi
say "Canonical QRXDesktop source verified; historical desktop patch scripts are NOT executed"

QRX_HARDWARE_SRC="$(find "$QRX_V020_ROOT" -type f -name 'qrx_hardware.c' -print -quit)"
[[ -n "$QRX_HARDWARE_SRC" && -f "$QRX_HARDWARE_SRC" ]] \
  || die "qrx_hardware.c not found below $QRX_V020_ROOT"
say "Applying QRXHardware requester-free mounted-volume discovery / default-screen fallback"
HARDWARE_VOLUME_PATCHER="$ROOT/tools/patch-qrxhardware-requester-free-volumes.py"
[[ -f "$HARDWARE_VOLUME_PATCHER" ]] || die "bundled QRXHardware .5.8 volume patcher missing"
python3 "$HARDWARE_VOLUME_PATCHER" "$QRX_HARDWARE_SRC" \
  || die "QRXHardware requester-free volume patch failed"
grep -Fq 'QRX v0.20.3.7.4.6.5.8 requester-free mounted-volume discovery' "$QRX_HARDWARE_SRC" \
  || die "QRXHardware .5.8 source marker gate failed"
if grep -Eq '"USB[0-9]+:"|USB0:-USB3' "$QRX_HARDWARE_SRC"; then
  die "QRXHardware still contains fixed USB volume probes"
fi

for sym in g_terminal g_desktop g_screen g_screen_width g_screen_height g_display_label PoseidonBase g_session_action; do
  grep -q "$sym" "$QRX_DESKTOP_SRC" || die "canonical QRXDesktop baseline integrity failure: missing $sym"
done

AROS_CC="$AROS_CC_OVERRIDE"
if [[ -z "$AROS_CC" ]]; then
  for c in \
    "$CORE/bin/linux-x86_64/tools/m68k-amiga-aros-gcc" \
    "$CORE/bin/linux-x86_64/tools/m68k-aros-gcc" \
    "$WORKSPACE/toolchain-core-m68k/bin/linux-x86_64/tools/m68k-amiga-aros-gcc" \
    "$WORKSPACE/toolchain-core-m68k/m68k-aros-gcc"; do
    if [[ -x "$c" ]]; then AROS_CC="$c"; break; fi
  done
fi
if [[ -z "$AROS_CC" ]]; then
  AROS_CC="$(find "$CORE/bin" "$WORKSPACE/toolchain-core-m68k" -type f -perm -111 \
      -name '*m68k*aros-gcc' -print 2>/dev/null | head -1 || true)"
fi
[[ -n "$AROS_CC" && -x "$AROS_CC" ]] \
  || die "Deadwood-native m68k AROS compiler not found; use --aros-cc PATH"
case "$AROS_CC" in
  *m68k-amigaos-gcc) die "refusing AmigaOS compiler for v0.20.3.7.4.6.5.9 runtime migration";;
esac
say "Deadwood-native AROS compiler: $AROS_CC"

AROS_STARTUP="$(find "$CORE" -type f -name 'startup.o' -print 2>/dev/null | \
  grep -E '/(Development|lib|Libs|gen)/' | head -1 || true)"
if [[ -z "$AROS_STARTUP" ]]; then
  AROS_STARTUP="$(find "$WORKSPACE" -type f -name 'startup.o' -print 2>/dev/null | \
    grep -E 'core-amiga-m68k' | head -1 || true)"
fi
[[ -n "$AROS_STARTUP" && -f "$AROS_STARTUP" ]] \
  || die "Deadwood AROS startup.o not found below $CORE"
AROS_STARTUP_DIR="$(dirname "$AROS_STARTUP")"

mapfile -t AROS_LIB_DIRS < <(
  find "$CORE" -type f -name 'lib*.a' -printf '%h\n' 2>/dev/null | sort -u
)
((${#AROS_LIB_DIRS[@]} > 0)) || die "no Deadwood AROS static library directories found"

for required in libdos.a libexec.a libautoinit.a liblibinit.a; do
  find "$CORE" -type f -name "$required" -print -quit 2>/dev/null | grep -q . \
    || die "required aligned Deadwood AROS library missing: $required"
done

say "AROS startup.o: $AROS_STARTUP"
say "AROS library directories discovered: ${#AROS_LIB_DIRS[@]}"

# v0.20.3.7.4.6.5.9: the standalone compiler also needs the generated/installed AROS
# SDK headers from this same core tree.  v0.20.3.7 supplied only link paths,
# which is why compilation stopped at <exec/types.h>.
say "Discovering aligned Deadwood AROS SDK include paths"

# v0.20.3.7.4.6.5.9 resolves the complete header closure needed by the QRX sources.
# AROS separates OS API headers and C runtime headers across multiple SDK roots;
# finding exec/types.h alone is not enough (v0.20.3.7.2 then failed on string.h).
#
# Parse every angle-bracket include from the QRX source tree, resolve it against
# the aligned core and same-commit Deadwood source, and add the exact include
# root needed for that include spelling.
HEADER_REQ="$TMP/qrx-required-headers.txt"
{
  find "$QRX_V020_ROOT/src" -type f \( -name '*.c' -o -name '*.h' \) -print0 2>/dev/null |
    xargs -0 grep -hE '^[[:space:]]*#[[:space:]]*include[[:space:]]*<[^>]+>' 2>/dev/null |
    sed -E 's/^[^<]*<([^>]+)>.*/\1/'
  printf '%s\n' exec/types.h proto/exec.h proto/dos.h string.h stdio.h stdlib.h
} | awk 'NF && !seen[$0]++' | sort > "$HEADER_REQ"

HEADER_MAP="$SYSROOT/QRX/Diagnostics/AROS-HEADER-RESOLUTION.txt"
: > "$HEADER_MAP"
AROS_INCLUDE_ROOTS_FILE="$TMP/qrx-aros-include-roots.txt"
: > "$AROS_INCLUDE_ROOTS_FILE"

resolve_header_root(){
  local req="$1" hit="" suffix="/$req" root=""

  # Runtime/core headers have priority.
  hit="$(find "$CORE" -type f -path "*$suffix" -print 2>/dev/null | head -1 || true)"

  # Same Deadwood commit is the only permitted fallback.
  if [[ -z "$hit" ]]; then
    hit="$(find "$SRC" -type f -path "*$suffix" -print 2>/dev/null | head -1 || true)"
  fi

  if [[ -n "$hit" ]]; then
    root="${hit%$suffix}"
    printf '%s\n' "$root" >> "$AROS_INCLUDE_ROOTS_FILE"
    printf '%-40s -> %s\n' "$req" "$hit" >> "$HEADER_MAP"
    return 0
  fi

  printf '%-40s -> MISSING\n' "$req" >> "$HEADER_MAP"
  return 1
}

MISSING_HEADERS=0
while IFS= read -r req; do
  [[ -n "$req" ]] || continue
  if ! resolve_header_root "$req"; then
    # Some project/local optional headers may be generated or supplied by the
    # source directory itself. Only fail below for fundamental platform/CRT
    # headers and when GCC preflight proves a required include is unresolved.
    case "$req" in
      exec/*|dos/*|intuition/*|graphics/*|proto/*|libraries/*|workbench/*|utility/*|devices/*|hardware/*|string.h|stdio.h|stdlib.h|stddef.h|stdint.h|ctype.h|errno.h|math.h|time.h|limits.h|setjmp.h|signal.h)
        MISSING_HEADERS=1
        ;;
    esac
  fi
done < "$HEADER_REQ"

sort -u "$AROS_INCLUDE_ROOTS_FILE" -o "$AROS_INCLUDE_ROOTS_FILE"
mapfile -t AROS_INCLUDE_DIRS < "$AROS_INCLUDE_ROOTS_FILE"

# Add compiler-owned include directories too. These are part of the same
# Deadwood cross toolchain and provide GCC implementation headers where needed.
for query in \
  "$("$AROS_CC" -print-file-name=include 2>/dev/null || true)" \
  "$("$AROS_CC" -print-file-name=include-fixed 2>/dev/null || true)"; do
  [[ -n "$query" && -d "$query" ]] && AROS_INCLUDE_DIRS+=("$query")
done

# Deduplicate after compiler include discovery.
printf '%s\n' "${AROS_INCLUDE_DIRS[@]}" | awk 'NF && !seen[$0]++' > "$AROS_INCLUDE_ROOTS_FILE"
mapfile -t AROS_INCLUDE_DIRS < "$AROS_INCLUDE_ROOTS_FILE"

AROS_INCLUDE_FLAGS=()
for d in "${AROS_INCLUDE_DIRS[@]}"; do
  [[ -d "$d" ]] || continue
  AROS_INCLUDE_FLAGS+=("-I$d")
done

((${#AROS_INCLUDE_FLAGS[@]} > 0)) \
  || die "no aligned Deadwood AROS include directories found"

# Hard gate the headers that already proved essential.
for req in exec/types.h proto/exec.h proto/dos.h string.h stdio.h stdlib.h; do
  if ! grep -Fq "$req" "$HEADER_MAP" || grep -E "^${req//\//\\/}[[:space:]]+-> MISSING" "$HEADER_MAP" >/dev/null 2>&1; then
    die "required native AROS/CRT header unresolved: $req (see AROS-HEADER-RESOLUTION.txt)"
  fi
done

AROS_EXEC_TYPES="$(awk '$1=="exec/types.h" && $2=="->" {print $3; exit}' "$HEADER_MAP")"
AROS_STRING_H="$(awk '$1=="string.h" && $2=="->" {print $3; exit}' "$HEADER_MAP")"
[[ -f "$AROS_EXEC_TYPES" ]] || die "resolved exec/types.h path is invalid"
[[ -f "$AROS_STRING_H" ]] || die "resolved string.h path is invalid"

say "AROS exec/types.h: $AROS_EXEC_TYPES"
say "AROS string.h:     $AROS_STRING_H"
say "AROS include roots resolved: ${#AROS_INCLUDE_DIRS[@]}"
say "QRX source headers mapped: $(wc -l < "$HEADER_REQ")"

AROS_WRAPPER="$TMP/qrx-deadwood-aros-gcc"

AROS_WRAPPER="$TMP/qrx-deadwood-aros-gcc"
AROS_AUDIT="$SYSROOT/QRX/Diagnostics/AROS-NATIVE-COMPILER-AUDIT.log"
: > "$AROS_AUDIT"

{
  echo '#!/usr/bin/env bash'
  echo 'set -Eeuo pipefail'
  printf 'REAL_CC=%q\n' "$AROS_CC"
  printf 'STARTUP_DIR=%q\n' "$AROS_STARTUP_DIR"
  printf 'AUDIT_LOG=%q\n' "$AROS_AUDIT"
  echo 'args=()'
  echo 'out=""'
  echo 'prev=""'
  echo 'for a in "$@"; do'
  echo '  if [[ "$prev" == "-o" ]]; then out="$a"; fi'
  echo '  prev="$a"'
  echo '  case "$a" in'
  echo '    -noixemul) ;;'
  echo '    *) args+=("$a");;'
  echo '  esac'
  echo 'done'
  echo 'link=(-B"$STARTUP_DIR/")'
  for d in "${AROS_LIB_DIRS[@]}"; do
    printf 'link+=(%q)\n' "-L$d"
  done
  echo 'incs=()'
  for f in "${AROS_INCLUDE_FLAGS[@]}"; do
    printf 'incs+=(%q)\n' "$f"
  done
  echo 'for a in "${args[@]}"; do'
  echo '  case "$a" in'
  echo '    -I/opt/amiga/*|-isystem/opt/amiga/*) echo "ERROR: legacy /opt/amiga header path rejected: $a" >&2; exit 97;;'
  echo '  esac'
  echo 'done'
  echo 'printf "output=%s\n" "${out:-<none>}" >> "$AUDIT_LOG"'
  echo 'printf "argv=" >> "$AUDIT_LOG"'
  echo 'printf "%q " "${args[@]}" >> "$AUDIT_LOG"'
  echo 'printf "\n" >> "$AUDIT_LOG"'
  echo 'compat=(-fno-pic -fno-PIC)'
  echo 'printf "elf2hunk_compat=%q %q\\n" "${compat[@]}" >> "$AUDIT_LOG"'
  echo 'exec "$REAL_CC" "${incs[@]}" "${link[@]}" "${args[@]}" "${compat[@]}"' 
} > "$AROS_WRAPPER"
chmod +x "$AROS_WRAPPER"

{
  echo "QRX OS One Amiga 68K v0.20.3.7.4.6.5.9 - Clean QRX Desktop Host"
  echo "Deadwood commit: $COMMIT"
  echo "Compiler: $AROS_CC"
  echo "startup.o: $AROS_STARTUP"
  echo "exec/types.h: $AROS_EXEC_TYPES"
  echo "string.h: $AROS_STRING_H"
  echo "Header map: QRX:Diagnostics/AROS-HEADER-RESOLUTION.txt"
  echo "Legacy AmigaOS runtime compiler allowed: NO"
  echo "Legacy /opt/amiga headers allowed: NO"
  echo "ELF2HUNK codegen policy: -fno-pic -fno-PIC; intra-section PC8/PC16 converter support"
  echo
  echo "AROS include flags:"
  printf '%s\n' "${AROS_INCLUDE_FLAGS[@]}"
  echo "QRXDesktop autostart during smoke-test release: NO"
  echo
  echo "AROS library directories:"
  printf '%s\n' "${AROS_LIB_DIRS[@]}"
} > "$SYSROOT/QRX/Diagnostics/NATIVE-ABI-MIGRATION.txt"

say "Running native AROS header/compiler preflight before full payload build"
cat > "$TMP/qrx-native-header-preflight.c" <<'EOF_PREFLIGHT'
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    const char *msg = "QRX native AROS SDK preflight OK\n";
    char copy[64];
    struct Library *DOSBase;
    strcpy(copy, msg);
    DOSBase = OpenLibrary((STRPTR)"dos.library", 0);
    if (DOSBase) {
        PutStr((STRPTR)copy);
        CloseLibrary(DOSBase);
    }
    return 0;
}
EOF_PREFLIGHT
if ! "$AROS_WRAPPER" -Os -m68020 -Wall -Wextra \
     -o "$TMP/qrx-native-header-preflight" "$TMP/qrx-native-header-preflight.c" \
     >"$SYSROOT/QRX/Diagnostics/NATIVE-AROS-SDK-PREFLIGHT.log" 2>&1; then
  cat "$SYSROOT/QRX/Diagnostics/NATIVE-AROS-SDK-PREFLIGHT.log" >&2 || true
  die "native AROS SDK preflight failed before QRX payload build"
fi
python3 - "$TMP/qrx-native-header-preflight" <<'PY'
from pathlib import Path
import sys, struct
p=Path(sys.argv[1])
b=p.read_bytes()
if len(b) < 4:
    raise SystemExit("native SDK preflight output is empty/truncated")

kind = "unknown"
if b[:4] == b"\x00\x00\x03\xf3":
    kind = "Amiga HUNK"
elif b[:4] == b"\x7fELF":
    if len(b) < 20:
        raise SystemExit("native SDK preflight ELF is truncated")
    ei_class = b[4]
    ei_data = b[5]
    endian = ">" if ei_data == 2 else "<"
    e_machine = struct.unpack(endian + "H", b[18:20])[0]
    # EM_68K = 4. AROS m68k may intentionally emit ELF and load/convert it
    # through the native AROS executable path. This is a valid target format.
    if e_machine != 4:
        raise SystemExit(f"native SDK preflight ELF has wrong machine: {e_machine}")
    kind = "ELF/m68k"
else:
    raise SystemExit(f"native SDK preflight has unsupported magic: {b[:4].hex()}")

print(f"Native AROS SDK preflight executable OK: {kind}, {len(b)} bytes")
PY

# Record host-side format diagnostics too; do not assume HUNK-only output.
{
  echo "Preflight executable:"
  file "$TMP/qrx-native-header-preflight" 2>/dev/null || true
  od -An -tx1 -N16 "$TMP/qrx-native-header-preflight" 2>/dev/null || true
} >> "$SYSROOT/QRX/Diagnostics/NATIVE-AROS-SDK-PREFLIGHT.log"

say "Native AROS SDK preflight compile/link: OK"

say "Preparing shadow QRX v0.20 payload root with native ELF-aware executable validation"
QRX_NATIVE_ROOT="$TMP/qrx-v020-native-root"
rm -rf "$QRX_NATIVE_ROOT"
mkdir -p "$QRX_NATIVE_ROOT"
cp -a "$QRX_V020_ROOT"/. "$QRX_NATIVE_ROOT"/
QRX_NATIVE_BUILDER="$QRX_NATIVE_ROOT/build-qrx-amiga68k-live-v0.20.sh"
[[ -f "$QRX_NATIVE_BUILDER" ]] || die "shadow payload builder missing"

# The shadow tree is now a full copy so no source file can be modified through a hardlink.
QRX_NATIVE_BUILDER_MODE="$(stat -c '%a' "$QRX_NATIVE_BUILDER" 2>/dev/null || echo 755)"
rm -f "$QRX_NATIVE_BUILDER"
cp "$QRX_V020_ROOT/build-qrx-amiga68k-live-v0.20.sh" "$QRX_NATIVE_BUILDER"
chmod "$QRX_NATIVE_BUILDER_MODE" "$QRX_NATIVE_BUILDER"

SRC_BUILDER_INODE="$(stat -c '%d:%i' "$QRX_V020_ROOT/build-qrx-amiga68k-live-v0.20.sh")"
SHADOW_BUILDER_INODE="$(stat -c '%d:%i' "$QRX_NATIVE_BUILDER")"
[[ "$SRC_BUILDER_INODE" != "$SHADOW_BUILDER_INODE" ]]   || die "shadow payload builder is still hard-linked to canonical source"

say "Shadow payload builder isolated from canonical source"

python3 - "$QRX_NATIVE_BUILDER" <<'PY_PATCH_BUILDER'
from pathlib import Path
import re
import sys

p = Path(sys.argv[1])
text = p.read_text()

needle = 'does not start with Amiga HUNK_HEADER (0x3F3)'
pos = text.find(needle)
if pos < 0:
    raise SystemExit('legacy HUNK-only validation marker not found in payload builder')

# Find the Python heredoc containing the legacy validation.  Keep BOTH the
# original invocation and the original heredoc tag.  Only replace the body.
start = text.rfind('python3 ', 0, pos)
if start < 0:
    raise SystemExit('could not locate legacy validator invocation')

line_end = text.find('\n', start)
if line_end < 0:
    raise SystemExit('could not locate validator invocation line end')

invocation = text[start:line_end]
m = re.search(r"<<'([^']+)'", invocation)
if not m:
    raise SystemExit('could not locate legacy validator heredoc tag')
tag = m.group(1)

body_start = line_end + 1
closing = '\n' + tag
body_end = text.find(closing, body_start)
if body_end < 0:
    raise SystemExit('could not locate legacy validator heredoc terminator')

validator_body = """from pathlib import Path
import struct
import sys

if len(sys.argv) < 2:
    raise SystemExit("native executable validator received no target path")

arg = Path(sys.argv[1])

# The legacy v0.20 builder passes its BUILD DIRECTORY to this validator.
# Some later builders may pass the QRXDesktop executable directly. Support
# both shapes without changing the original shell invocation.
if arg.is_dir():
    p = arg / "QRXDesktop"
else:
    p = arg

if not p.is_file():
    raise SystemExit(
        f"QRXDesktop validator target not found: argument={arg} resolved={p}"
    )

b = p.read_bytes()

if len(b) < 4:
    raise SystemExit(f"{p.name} executable is empty/truncated")

if b[:4] == b"\\x00\\x00\\x03\\xf3":
    print(f"{p.name} executable format OK: Amiga HUNK")
elif b[:4] == b"\\x7fELF":
    if len(b) < 20:
        raise SystemExit(f"{p.name} ELF is truncated")
    endian = ">" if b[5] == 2 else "<"
    machine = struct.unpack(endian + "H", b[18:20])[0]
    if machine != 4:
        raise SystemExit(
            f"{p.name} ELF machine is {machine}, expected EM_68K (4)"
        )
    print(f"{p.name} executable format OK: ELF/m68k")
else:
    raise SystemExit(
        f"{p.name} has unsupported executable magic: {b[:4].hex()}"
    )
"""

# Preserve invocation and terminator exactly; body contains REAL newlines.
patched = text[:body_start] + validator_body + text[body_end:]
p.write_text(patched)

# Structural self-check of the generated shell script.
check = p.read_text()
if needle in check:
    raise SystemExit('legacy HUNK-only diagnostic remains after patch')
if 'executable format OK: ELF/m68k' not in check:
    raise SystemExit('ELF-aware validator body missing after patch')
if 'if arg.is_dir()' not in check or 'arg / "QRXDesktop"' not in check:
    raise SystemExit('QRXDesktop directory/direct-path resolver missing after patch')
if check.count("\n" + tag + "\n") < 1:
    raise SystemExit('original heredoc terminator was lost')

print(
    'Patched HUNK-only validator body in place; '
    f'original invocation/tag preserved ({tag}): {p}'
)
PY_PATCH_BUILDER

bash -n "$QRX_NATIVE_BUILDER" || die "patched shadow payload builder has invalid shell syntax"
grep -q 'executable format OK: ELF/m68k' "$QRX_NATIVE_BUILDER" || die "ELF-aware executable validation patch did not land"
if grep -Fq '$BUILD/QRXDesktop' "$QRX_NATIVE_BUILDER"; then
  die "unsafe out-of-scope BUILD reference remains in shadow builder"
fi
if grep -q 'does not start with Amiga HUNK_HEADER (0x3F3)' "$QRX_NATIVE_BUILDER"; then
  die "legacy HUNK-only QRXDesktop gate remains in shadow builder"
fi
say "Shadow payload builder validation: HUNK or ELF/m68k"
say "Canonical QRX v0.20.1 payload root remains unmodified"

say "Building complete QRX v0.20 runtime through Deadwood-native AROS ABI"
export QRX_AMIGA_CC="$AROS_WRAPPER"
MIGRATION_BUILD_LOG="$SYSROOT/QRX/Diagnostics/NATIVE-ABI-MIGRATION-BUILD.log"
if ! "$QRX_NATIVE_BUILDER" 2>&1 | tee "$MIGRATION_BUILD_LOG"; then
  die "native AROS ABI payload build failed; see QRX:Diagnostics/NATIVE-ABI-MIGRATION-BUILD.log"
fi
unset QRX_AMIGA_CC
unset QRX_OVERLAY_SYSROOT

MIGRATED_BUILD_OUTPUTS=(
  QRXDesktop QRXWriter QRXSpreadsheet QRXPresenter QRXHardware
  QRXUnix QRXELF QRXPosixTest QRXHelloPosix QRXLinuxRun
  QRX86Run QRXWinRun QRXCompatSuite QRXCompatDB QRXWinRuntimeTest
  QRXWinScan QRXWinExtTest QRXMSI QRXDynarecTest QRXWineServicesTest
  QRXWineDisplayTest QRXWineUnixTest QRXWineHostTest QRXWineBoot
)
for target in "${MIGRATED_BUILD_OUTPUTS[@]}"; do
  grep -Fxq "output=$target" "$AROS_AUDIT" \
    || die "native ABI migration audit failed: $target was not built through m68k-aros-gcc"
done
say "Compiler audit OK: ${#MIGRATED_BUILD_OUTPUTS[@]} QRX build outputs used Deadwood-native AROS GCC"

MIGRATED_RUNTIME_FILES=(
  "$SYSROOT/QRX/C/QRXDesktop"
  "$SYSROOT/QRX/C/QRXWriter"
  "$SYSROOT/QRX/C/QRXSpreadsheet"
  "$SYSROOT/QRX/C/QRXPresenter"
  "$SYSROOT/QRX/C/QRXHardware"
  "$SYSROOT/QRX/C/QRXUnix"
  "$SYSROOT/QRX/C/QRXELF"
  "$SYSROOT/QRX/C/QRXPosixTest"
  "$SYSROOT/QRX/POSIX/bin/hello-posix"
  "$SYSROOT/QRX/C/QRXLinuxRun"
  "$SYSROOT/QRX/C/QRX86Run"
  "$SYSROOT/QRX/C/QRXWinRun"
  "$SYSROOT/QRX/C/QRXCompatSuite"
  "$SYSROOT/QRX/C/QRXCompatDB"
  "$SYSROOT/QRX/C/QRXWinRuntimeTest"
  "$SYSROOT/QRX/C/QRXWinScan"
  "$SYSROOT/QRX/C/QRXWinExtTest"
  "$SYSROOT/QRX/C/QRXMSI"
  "$SYSROOT/QRX/C/QRXDynarecTest"
  "$SYSROOT/QRX/C/QRXWineServicesTest"
  "$SYSROOT/QRX/C/QRXWineDisplayTest"
  "$SYSROOT/QRX/C/QRXWineUnixTest"
  "$SYSROOT/QRX/C/QRXWineHostTest"
  "$SYSROOT/QRX/C/QRXWineBoot"
)

python3 - "${MIGRATED_RUNTIME_FILES[@]}" <<'PY'
from pathlib import Path
import sys, struct

def fmt(p):
    b=p.read_bytes()
    if len(b) < 4:
        raise SystemExit(f"migrated runtime binary is empty/truncated: {p}")
    if b[:4] == b"\x00\x00\x03\xf3":
        return "Amiga HUNK"
    if b[:4] == b"\x7fELF":
        if len(b) < 20:
            raise SystemExit(f"truncated ELF: {p}")
        endian = ">" if b[5] == 2 else "<"
        machine = struct.unpack(endian + "H", b[18:20])[0]
        if machine != 4:
            raise SystemExit(f"wrong ELF machine {machine} for {p}")
        return "ELF/m68k"
    raise SystemExit(f"unsupported executable magic {b[:4].hex()} for {p}")

for raw in sys.argv[1:]:
    p=Path(raw)
    if not p.is_file():
        raise SystemExit(f"missing migrated runtime binary: {p}")
    kind=fmt(p)
    print("Native AROS executable OK:", p.name, kind, p.stat().st_size)
PY

# Remove ambiguous old probes first, then install only native AROS sanity probes.
find "$SYSROOT/QRX/C" -maxdepth 1 -type f \
  \( -name 'QRXAbi0*' -o -name 'QRXAbiRaw' -o -name 'QRXAmigaAbi0*' -o -name 'QRXArosAbi0*' \) \
  -delete 2>/dev/null || true
find "$SYSROOT/QRX/C" -maxdepth 1 -type f -name 'QRXArosAbi*.map' -delete 2>/dev/null || true

"$AROS_WRAPPER" -Os -m68020 -Wall -Wextra \
  -o "$SYSROOT/QRX/C/QRXArosAbi02" "$ROOT/tools/qrx_abi02_dos.c"
"$AROS_WRAPPER" -Os -m68020 -Wall -Wextra \
  -o "$SYSROOT/QRX/C/QRXArosAbi05" "$ROOT/tools/qrx_abi05_window.c"

{
  echo
  echo "MIGRATED RUNTIME BINARIES"
  for f in "${MIGRATED_RUNTIME_FILES[@]}"; do
    sha256sum "$f"
  done
} >> "$SYSROOT/QRX/Diagnostics/NATIVE-ABI-MIGRATION.txt"

cat > "$SYSROOT/QRX/Diagnostics/MIGRATION-SMOKE-TEST.txt" <<'EOF_SMOKE'
QRX OS One Amiga 68K v0.20.3.7.4.6.5.9 - Requester-Free Volume Discovery / Clean QRX Desktop Host

The legacy m68k-amigaos-gcc runtime path has been removed for QRX runtime binaries.
QRXDesktop autostart stays OFF for this release so a failing UI cannot damage boot.

After AROS/Wanderer is stable:

1. QRX:C/QRXArosAbi02
   Expected: QRXABI02 DOS PutStr OK

2. QRX:C/QRXDesktop

3. If QRXDesktop opens:
     QRX:C/QRXWriter
     QRX:C/QRXHardware

4. Optional:
     QRX:C/QRXArosAbi05

If QRXDesktop fails, press More and record the Error + PC.
Do not re-enable QRXDesktop autostart until the manual desktop smoke test passes.
EOF_SMOKE

say "Auditing native QRX ELF relocations for Deadwood elf2hunk compatibility"
RELOC_AUDIT="$SYSROOT/QRX/Diagnostics/ELF2HUNK-RELOCATION-AUDIT.txt"
: > "$RELOC_AUDIT"

python3 - "$RELOC_AUDIT" "${MIGRATED_RUNTIME_FILES[@]}" <<'PY_RELOC_AUDIT'
from pathlib import Path
import re
import shutil
import subprocess
import sys

report = Path(sys.argv[1])
files = [Path(x) for x in sys.argv[2:]]
readelf = shutil.which("readelf")
if not readelf:
    raise SystemExit("readelf is required for ELF2HUNK relocation audit")

# QRX-patched Deadwood elf2hunk additionally folds PC8/PC16 when intra-section.
allowed = {"R_68K_NONE", "R_68K_32", "R_68K_PC32", "R_68K_PC16", "R_68K_PC8"}
bad = []

with report.open("w") as out:
    out.write("QRX native ELF -> HUNK relocation compatibility audit\n")
    out.write("Allowed relocation classes: " + ", ".join(sorted(allowed)) + "\n\n")

    for p in files:
        b = p.read_bytes()[:4]
        if b == b"\x00\x00\x03\xf3":
            out.write(f"{p}: HUNK already; no ELF relocation audit required\n")
            continue
        if b != b"\x7fELF":
            bad.append((p, "unsupported executable format"))
            out.write(f"{p}: FAIL unsupported executable format\n")
            continue

        proc = subprocess.run(
            [readelf, "-rW", str(p)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        out.write(f"\n===== {p} =====\n")
        out.write(proc.stdout)
        if proc.returncode != 0:
            bad.append((p, f"readelf failed rc={proc.returncode}"))
            continue

        types = set(re.findall(r"\bR_68K_[A-Z0-9_]+\b", proc.stdout))
        unsupported = sorted(types - allowed)
        if unsupported:
            bad.append((p, ", ".join(unsupported)))
            out.write("QRX-ELF2HUNK-AUDIT: FAIL unsupported=" + ",".join(unsupported) + "\n")
        else:
            out.write("QRX-ELF2HUNK-AUDIT: OK\n")

if bad:
    print("ELF2HUNK relocation audit failed:", file=sys.stderr)
    for p, why in bad:
        print(f"  {p.name}: {why}", file=sys.stderr)
    raise SystemExit(1)

print(f"ELF2HUNK relocation audit OK for {len(files)} QRX runtime files")
PY_RELOC_AUDIT

say "ELF2HUNK relocation audit: OK"

say "Native AROS ABI migration payload complete; QRXDesktop remains manual after Wanderer bootstrap"


mkdir -p "$SYSROOT/QRX"
cat > "$SYSROOT/QRX/NATIVE-LIVECD-PROVENANCE.txt" <<EOF
QRX OS One Amiga 68K v0.20.3.7.4.6.5.9
Release: Requester-Free Volume Discovery / Clean QRX Desktop Host
Packaging mode: Deadwood native AROS build-system LiveCD
QRX payload ABI: v0.20 / Deadwood-native AROS
Deadwood repository: $AROS_REPO
Deadwood commit: $COMMIT
Deadwood 2026 reference archive: $(basename "$REFERENCE_TAR")
Deadwood 2026 reference SHA256: $REFSHA
Runtime tree: core-amiga-m68k / rebuild option 12
Emergency-Boot: EBFHUNKROM=no
Emergency ADF sectors: 3520
Alignment policy: runtime source commit must equal provenance commit
Generated UTC: $(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

cat > "$SYSROOT/QRX/RUNTIME-ALIGNMENT.txt" <<EOF
QRX v0.20.3.7.4.6.5.9 requester-free volume discovery + clean public-screen host

EXPECTED DEADWOOD COMMIT
$COMMIT

REFERENCE ARCHIVE SHA256
$REFSHA

The LiveCD runtime and the native packaging tree are generated from the same
Deadwood source revision. A previously cached unstamped/old core-amiga-m68k
tree is rejected and rebuilt.

The v0.20.1 proven boot architecture is preserved:
- native AROS distfiles ISO
- EBFHUNKROM=no
- 3520-sector Emergency-Boot ADF
- QRX payload ABI v0.20
- QRX native startup: manual migration smoke-test mode
- QRXDesktop autostart: DISABLED pending manual clean-host smoke test
- QRX runtime compiler: matched Deadwood m68k-aros-gcc
- ReactOS/QRX86/Win32 runtime payload: integrated
EOF


install_qrx_native_runtime(){
  local rt="$SYSROOT/QRX/Runtime"
  mkdir -p "$rt" "$SYSROOT/QRX/Logs" "$SYSROOT/QRX/S"

  say "Keeping QRXDesktop autostart disabled for the v0.20.3.7.4.6.5.9 manual clean-host smoke test"

  while IFS= read -r p; do
    [[ -f "$p" ]] || continue
    cp -a "$p" "$p.qrx-v02037-preclean" 2>/dev/null || true
    python3 - "$p" <<'PY'
from pathlib import Path
import re, sys
p=Path(sys.argv[1])
s=p.read_text(errors="replace")
orig=s
s=re.sub(r'(?ims)^; --- QRX OS One Amiga 68K v0\.20[^\n]*---\s*.*?^; --- /QRX[^\n]*---\s*', '', s)
lines=[]
for line in s.splitlines(True):
    low=line.lower()
    if "qrx-startup" in low:
        continue
    if "qrx:c/qrxdesktop" in low or "sys:qrx/c/qrxdesktop" in low:
        continue
    lines.append(line)
s=''.join(lines)
if s != orig:
    p.write_text(s)
    print("cleaned active QRX autostart:", p)
PY
  done < <(
    find "$SYSROOT/S" -maxdepth 1 -type f \
      \( -iname 'Startup-Sequence' -o -iname 'User-Startup' \
         -o -iname 'Network-Startup' -o -iname 'Wanderer-Startup' \) \
      ! -name '*.qrx-*' ! -name '*.bak' ! -name '*.backup' \
      2>/dev/null | sort -u
  )

  cat > "$SYSROOT/S/QRX-Startup" <<'EOF_QRX_START'
; QRX OS One Amiga 68K v0.20.3.7.4.6.5.9
; Manual smoke-test launcher. Not called from Startup-Sequence in this release.
Assign QRX: SYS:QRX >NIL:
Echo "QRX v0.20.3.7.4.6.5.9 Clean QRX Desktop Host"
Echo "First: QRX:C/QRXArosAbi02"
Echo "Then : QRX:C/QRXDesktop"
EOF_QRX_START

  cat > "$SYSROOT/QRX/S/Runtime-SelfTest" <<'EOF_SELFTEST'
Echo "QRX OS One Amiga 68K v0.20.3.7.4.6.5.9 Clean Host / Requester-Free Volumes"
IF EXISTS QRX:C/QRXDesktop
  Echo "QRXDesktop: present / Deadwood-native build / manual smoke test"
ELSE
  Echo "QRXDesktop: MISSING"
ENDIF
IF EXISTS QRX:C/QRXArosAbi02
  Echo "DOS ABI probe: present"
ENDIF
IF EXISTS QRX:Diagnostics/NATIVE-ABI-MIGRATION.txt
  Echo "Migration audit: present"
ENDIF
EOF_SELFTEST

  cat > "$rt/native-runtime.conf" <<EOF_RUNTIME
QRX_RELEASE=0.20.3.7
QRX_RUNTIME_MODE=native-aros-m68k
QRX_DESKTOP_AUTOSTART=0
QRX_ABI_ISOLATION=0
QRX_NATIVE_AROS_ABI=1
QRX_PAYLOAD_ABI=0.20
QRX_PAYLOAD_COMPILER=deadwood-native-m68k-aros-gcc
QRX_DEADWOOD_COMMIT=$COMMIT
QRX_REACTOS_USERLAND=integrated
QRX_EBFHUNKROM=no
QRX_EMERGENCY_ADF_SECTORS=3520
EOF_RUNTIME

  cat > "$rt/README.txt" <<EOF_RUNTIME_README
QRX OS One Amiga 68K v0.20.3.7.4.6.5.9 - Requester-Free Volume Discovery / Clean QRX Desktop Host

All QRX runtime HUNK executables are rebuilt through the Deadwood-native
m68k-aros-gcc toolchain using startup.o and libraries from the same
core-amiga-m68k runtime tree.

The previously proven-bad m68k-amigaos-gcc/NDK runtime ABI path is not used
for the migrated QRX binaries.

Autostart is intentionally disabled for this release.

After boot:
  QRX:C/QRXArosAbi02
  QRX:C/QRXDesktop

Then, if the desktop opens:
  QRX:C/QRXWriter
  QRX:C/QRXHardware

Diagnostics:
  QRX:Diagnostics/NATIVE-ABI-MIGRATION.txt
  QRX:Diagnostics/NATIVE-ABI-MIGRATION-BUILD.log
  QRX:Diagnostics/AROS-NATIVE-COMPILER-AUDIT.log
  QRX:Diagnostics/MIGRATION-SMOKE-TEST.txt

Deadwood commit:
  $COMMIT
EOF_RUNTIME_README

  ACTIVE_GATE_LIST="$TMP/qrx-active-startup-files.txt"
  find "$SYSROOT/S" -maxdepth 1 -type f \
    \( -iname 'Startup-Sequence' -o -iname 'User-Startup' \
       -o -iname 'Network-Startup' -o -iname 'Wanderer-Startup' \) \
    ! -name '*.qrx-*' ! -name '*.bak' ! -name '*.backup' \
    -print > "$ACTIVE_GATE_LIST"

  GATE_HIT=0
  while IFS= read -r active_startup; do
    [[ -f "$active_startup" ]] || continue
    if grep -niE 'Run[[:space:]]+.*QRX:C/QRXDesktop|Execute[[:space:]]+.*QRX-Startup|Run[[:space:]]+.*QRX-Startup|SYS:QRX/C/QRXDesktop' \
         "$active_startup" 2>/dev/null; then
      GATE_HIT=1
    fi
  done < "$ACTIVE_GATE_LIST"

  [[ "$GATE_HIT" == "0" ]] \
    || die "manual smoke-test gate failed: active QRXDesktop autostart remains"

  say "Startup gate OK: QRXDesktop remains manual"
  say "Native migration contract installed: Deadwood AROS ABI=ON, AmigaOS runtime ABI=OFF"
}
install_qrx_native_runtime

say "Restoring canonical Deadwood Startup-Sequence before QRX assign + IPrefs no-reset desktop bootstrap"
STARTUP_SEQ="$SYSROOT/S/Startup-Sequence"
[[ -f "$STARTUP_SEQ" ]] || die "active AROS Startup-Sequence missing: $STARTUP_SEQ"
REFERENCE_STARTUP_MEMBER="$(tar -tzf "$REFERENCE_TAR" | grep '/AROS/S/Startup-Sequence$' | head -1)"
[[ -n "$REFERENCE_STARTUP_MEMBER" ]] || die "reference archive Startup-Sequence member not found"
tar -xOzf "$REFERENCE_TAR" "$REFERENCE_STARTUP_MEMBER" > "$STARTUP_SEQ"

# A reused option-12 tree may still contain v0.20.3.7.4.6.5.3/.5.4 helper files.
# They must not participate in boot anymore.
rm -f "$SYSROOT/S/QRX-PostIPrefs-Startup"       "$SYSROOT/S/QRX-ScreenReset"       "$SYSROOT/S/QRX-ScreenReset-Startup" 2>/dev/null || true

say "Installing IPrefs no-reset / preserved Workbench screen bootstrap"
STARTUP_PATCHER="$ROOT/tools/patch-aros-startup-arosvision-bootstrap.py"
[[ -f "$STARTUP_PATCHER" ]] || die "bundled IPrefs no-reset bootstrap patcher missing"
python3 "$STARTUP_PATCHER" "$STARTUP_SEQ"   || die "IPrefs no-reset Startup-Sequence patch failed"

say "Installing persistent QRX: -> SYS:QRX startup assign"
QRX_ASSIGN_PATCHER="$ROOT/tools/patch-qrx-volume-assign.py"
[[ -f "$QRX_ASSIGN_PATCHER" ]] || die "bundled QRX volume assign patcher missing"
python3 "$QRX_ASSIGN_PATCHER" "$STARTUP_SEQ" || die "QRX volume assign Startup-Sequence patch failed"

grep -Fq 'QRX v0.20.3.7.4.6.5.7 IPREFS NO-RESET DESKTOP BOOTSTRAP' "$STARTUP_SEQ" \
  || die "IPrefs no-reset bootstrap marker gate failed"
grep -Fq 'Echo "QRX: IPrefs deferred - preserving existing Workbench screen" >>RAM:QRX-Startup.log' "$STARTUP_SEQ" \
  || die "IPrefs deferred diagnostic gate failed"
grep -Fq 'Echo "QRX: launching Wanderer on preserved Workbench screen" >>RAM:QRX-Startup.log' "$STARTUP_SEQ" \
  || die "preserved-screen Wanderer diagnostic gate failed"
grep -Fxq 'Assign "QRX:" "SYS:QRX" >NIL:' "$STARTUP_SEQ" \
  || die "persistent QRX: -> SYS:QRX startup assign gate failed"
grep -Fq 'Echo "QRX: persistent assign QRX: -> SYS:QRX installed" >>RAM:QRX-Startup.log' "$STARTUP_SEQ" \
  || die "persistent QRX assign runtime log gate failed"
if grep -Eq '^[[:space:]]*IPrefs[[:space:]]*$' "$STARTUP_SEQ"; then
  die "active IPrefs command remains in initial desktop bootstrap"
fi
grep -Fxq '    WANDERER:Wanderer' "$STARTUP_SEQ" || die "direct Wanderer command missing"
grep -Fq '        EndCLI' "$STARTUP_SEQ" || die "successful Wanderer EndCLI gate missing"
if grep -Eq 'QRX-PostIPrefs-Startup|QRX-StartupChild\.ready|FAIL-SAFE INTUITION SCREEN RESET HANDOFF' "$STARTUP_SEQ"; then
  die "retired detached Intuition handoff remains in active Startup-Sequence"
fi

cat > "$SYSROOT/S/QRX-ApplyIPrefs" <<'EOF_QRX_APPLY_IPREFS'
; Manual diagnostic helper only. Not called during boot.
Echo "QRX: manually running IPrefs; this may request a Workbench screen reset."
IPrefs
EOF_QRX_APPLY_IPREFS

cat > "$SYSROOT/QRX/Diagnostics/IPREFS-DEFERRED.txt" <<'EOF_IPREFS_DIAG'
QRX OS One Amiga 68K v0.20.3.7.4.6.5.9
Requester-Free Volume Discovery / Clean QRX Desktop Host

Target evidence from v0.20.3.7.4.6.5.5:
- IPrefs displayed the Intuition/Workbench screen reset requester.
- Cancelling that requester allowed Wanderer to start normally.

Boot policy in this release:
- Do not execute IPrefs during initial Startup-Sequence.
- Preserve the already active Workbench/default public screen.
- Continue normal startup services and User-Startup.
- Launch WANDERER:Wanderer directly and synchronously.
- QRXDesktop remains a manual post-Wanderer smoke test.

Manual diagnostic only:
  Execute S:QRX-ApplyIPrefs

Running that helper may reproduce the screen reset requester; it is intentionally
not part of the normal boot path.
EOF_IPREFS_DIAG

cat > "$SYSROOT/QRX/Diagnostics/QRX-VOLUME-ASSIGN.txt" <<'EOF_QRX_ASSIGN_DIAG'
QRX OS One Amiga 68K v0.20.3.7.4.6.5.9
Requester-Free Volume Discovery / Clean QRX Desktop Host

Target evidence from v0.20.3.7.4.6.5.6:
- Boot reached the preserved Workbench/Wanderer path without the old IPrefs requester.
- Accessing QRX: produced: Please insert volume "QRX" in any drive.
- The payload exists physically at SYS:QRX, but normal boot had not installed the QRX: assign.

Boot policy in this release:
- Install global Assign QRX: SYS:QRX before User-Startup and Wanderer.
- Keep IPrefs deferred and preserve the current Workbench screen.
- Keep QRXDesktop manual until the native-ABI smoke test is complete.

Expected shell checks after boot:
  Assign QRX:
  Dir QRX:C
  QRX:C/QRXArosAbi02
  QRX:C/QRXDesktop
EOF_QRX_ASSIGN_DIAG

cat > "$SYSROOT/QRX/Diagnostics/REQUESTER-FREE-VOLUME-DISCOVERY.txt" <<'EOF_QRX_VOL_DISCOVERY'
QRX OS One Amiga 68K v0.20.3.7.4.6.5.9
Requester-Free Volume Discovery / Clean QRX Desktop Host

Target evidence from v0.20.3.7.4.6.5.7:
- QRXDesktop starts on the preserved Workbench/public screen.
- File Manager probing USB0:/USB1: causes repeated "Please insert volume" requesters.
- The borrowed host is visually mixed with Wanderer/Shell because the QRX full-screen
  window is opened as a BACKDROP window behind existing owner windows.

Policy in this release:
- QRXDesktop and QRXHardware suppress DOS volume requesters for their process lifetime.
- Removable/additional volumes are discovered only from the locked DOS volume list.
- No synthetic USB0:/USB1:/USB2:/USB3: paths are probed.
- The existing default public screen is preferred as the QRX host.
- On a borrowed host, QRX temporarily saves/restores the palette and title state.
- The QRX full-screen host window is a normal front window, not WFLG_BACKDROP.
- Compact <=700x550 screens use full-content terminal/file-manager geometry.
- QRXHardware falls back to the current default public screen when no named QRX screen exists.
EOF_QRX_VOL_DISCOVERY

say "Bootstrap gate OK: QRX: assign -> preserved Workbench screen -> Wanderer -> EndCLI"
say "Installing native QRX primary-shell boot"
python3 "$ROOT/tools/patch-qrx-native-boot-6.6.13.py" "$STARTUP_SEQ" || die "native QRX boot patch failed"

say "Installing narrow Deadwood elf2hunk PC8/PC16 intra-section compatibility patch"
ELF2HUNK_SRC="$SRC/tools/elf2hunk/elf2hunk.c"
[[ -f "$ELF2HUNK_SRC" ]] || die "Deadwood elf2hunk source not found: $ELF2HUNK_SRC"

python3 - "$ELF2HUNK_SRC" <<'PY_ELF2HUNK_PATCH'
from pathlib import Path
import re
import sys

p = Path(sys.argv[1])
src = p.read_text()

marker = "QRX_ELF2HUNK_PC8_PC16_INTRASECTION"
define_marker = "QRX_ELF2HUNK_PC8_DEFINE_FALLBACK"

# v0.20.3.7.4.6.5.9:
# Do NOT place the fallback after the "last #include" in the file. elf2hunk.c
# contains conditional include regions, so such a definition can be present in
# the text yet be outside the active preprocessing branch used by the Linux
# host compiler.
#
# Instead, bind the fallback to Deadwood's own m68k relocation definition
# block, directly after R_68K_PC16. This is the exact scope in which
# R_68K_PC32/R_68K_PC16 are visible to relocate().
#
# ELF/m68k ABI:
#   R_68K_PC32 = 4
#   R_68K_PC16 = 5
#   R_68K_PC8  = 6

# Remove any QRX fallback previously inserted in a potentially inactive
# conditional include branch.
src = re.sub(
    r'(?ms)\n?/\*\s*QRX_ELF2HUNK_PC8_DEFINE_FALLBACK\s*\*/\s*'
    r'#ifndef\s+R_68K_PC8\s*'
    r'#define\s+R_68K_PC8\s+6\s*'
    r'#endif\s*',
    '\n',
    src
)

# If a plain fallback from an interrupted QRX run exists, remove it only when
# it is exactly our guarded R_68K_PC8=6 block. Do not touch upstream defines.
src = re.sub(
    r'(?ms)\n?#ifndef\s+R_68K_PC8\s*\n'
    r'#define\s+R_68K_PC8\s+6\s*\n'
    r'#endif\s*\n?',
    '\n',
    src
)

pc16_def = re.search(
    r'(?m)^(?P<indent>\s*)#\s*define\s+R_68K_PC16\s+5(?:\s*/\*.*\*/)?\s*$',
    src
)
if not pc16_def:
    raise SystemExit("Deadwood R_68K_PC16=5 relocation definition anchor not found")

insert_at = pc16_def.end()
fallback = (
    "\n"
    "/* QRX_ELF2HUNK_PC8_DEFINE_FALLBACK */\n"
    "#ifndef R_68K_PC8\n"
    "#define R_68K_PC8 6\n"
    "#endif"
)
src = src[:insert_at] + fallback + src[insert_at:]
print("Installed R_68K_PC8=6 beside Deadwood R_68K_PC16=5 definition")

# Keep previously installed compatibility cases if complete. Otherwise patch
# Deadwood's unsupported PC16 case and add PC8 handling.
already_cases = (
    marker in src
    and re.search(r"(?m)^\s*case\s+R_68K_PC8\s*:", src)
    and "Unsupported cross-hunk relocation R_68K_PC8" in src
    and "Unsupported cross-hunk relocation R_68K_PC16" in src
)

if not already_cases:
    case_match = re.search(r"(?m)^(?P<indent>\s*)case\s+R_68K_PC16\s*:\s*$", src)
    if not case_match:
        raise SystemExit("elf2hunk R_68K_PC16 switch case not found")

    case_pos = case_match.start()
    indent = case_match.group("indent")
    tail = src[case_match.end():]
    next_case = re.search(r"(?m)^" + re.escape(indent) + r"case\s+", tail)
    if not next_case:
        raise SystemExit("elf2hunk switch boundary after R_68K_PC16 not found")
    next_case_pos = case_match.end() + next_case.start()

    old_block = src[case_pos:next_case_pos]
    if "R_68K_PC16" not in old_block:
        raise SystemExit("unexpected R_68K_PC16 block")

    i = indent
    lines = [
        f"{i}case R_68K_PC16:",
        f"{i}    /* QRX_ELF2HUNK_PC8_PC16_INTRASECTION */",
        f"{i}    if (shid == shrel->info)",
        f"{i}    {{",
        f"{i}        SIPTR disp = (SIPTR)sym.value + rel->addend - (SIPTR)offset;",
        f"{i}        if (disp < -32768 || disp > 32767)",
        f"{i}        {{",
        f"{i}            bug(\"[ELF2HUNK] R_68K_PC16 displacement out of range for symbol '%s'\\n\", symname);",
        f"{i}            set_error(EINVAL);",
        f"{i}            return 0;",
        f"{i}        }}",
        f"{i}        ((UBYTE *)h->data)[offset] = (UBYTE)(((UWORD)disp >> 8) & 0xff);",
        f"{i}        ((UBYTE *)h->data)[offset + 1] = (UBYTE)((UWORD)disp & 0xff);",
        f"{i}        shid = ~0;",
        f"{i}        continue;",
        f"{i}    }}",
        f"{i}    bug(\"[ELF2HUNK] Unsupported cross-hunk relocation R_68K_PC16\\n\");",
        f"{i}    bug(\"[ELF2HUNK] for symbol '%s'\\n\", symname);",
        f"{i}    set_error(EINVAL);",
        f"{i}    return 0;",
        "",
        f"{i}case R_68K_PC8:",
        f"{i}    if (shid == shrel->info)",
        f"{i}    {{",
        f"{i}        SIPTR disp = (SIPTR)sym.value + rel->addend - (SIPTR)offset;",
        f"{i}        if (disp < -128 || disp > 127)",
        f"{i}        {{",
        f"{i}            bug(\"[ELF2HUNK] R_68K_PC8 displacement out of range for symbol '%s'\\n\", symname);",
        f"{i}            set_error(EINVAL);",
        f"{i}            return 0;",
        f"{i}        }}",
        f"{i}        ((UBYTE *)h->data)[offset] = (UBYTE)(disp & 0xff);",
        f"{i}        shid = ~0;",
        f"{i}        continue;",
        f"{i}    }}",
        f"{i}    bug(\"[ELF2HUNK] Unsupported cross-hunk relocation R_68K_PC8\\n\");",
        f"{i}    bug(\"[ELF2HUNK] for symbol '%s'\\n\", symname);",
        f"{i}    set_error(EINVAL);",
        f"{i}    return 0;",
        "",
    ]
    src = src[:case_pos] + "\n".join(lines) + src[next_case_pos:]
    print("Installed PC16/PC8 intra-section compatibility cases")
else:
    print("PC16/PC8 compatibility cases already present")

p.write_text(src)

out = p.read_text()

# Structural checks:
# The prior v0.20.3.7.4.6.5 gate was too strict because it assumed a specific
# ordering/formatting of the surrounding PC32/PC16 definitions. What matters
# is that the fallback was inserted immediately after the exact PC16=5 anchor
# we discovered above. Verify that local relationship directly.
m = re.search(
    r'(?ms)(#\s*define\s+R_68K_PC16\s+5[^\n]*\n)'
    r'(?P<body>\s*/\*\s*QRX_ELF2HUNK_PC8_DEFINE_FALLBACK\s*\*/\s*\n'
    r'\s*#ifndef\s+R_68K_PC8\s*\n'
    r'\s*#define\s+R_68K_PC8\s+6\s*\n'
    r'\s*#endif)',
    out
)
assert m, "R_68K_PC8 fallback is not immediately after Deadwood R_68K_PC16=5"
assert marker in out
assert "case R_68K_PC8:" in out
assert "case R_68K_PC16:" in out

print("Deadwood elf2hunk relocation-definition local-scope verified")
PY_ELF2HUNK_PATCH

grep -q 'QRX_ELF2HUNK_PC8_PC16_INTRASECTION' "$ELF2HUNK_SRC" \
  || die "elf2hunk compatibility patch verification failed"

python3 - "$ELF2HUNK_SRC" <<'PY_ELF2HUNK_SCOPE_GATE'
from pathlib import Path
import re, sys
s = Path(sys.argv[1]).read_text()
m = re.search(
    r'(?ms)#\s*define\s+R_68K_PC16\s+5[^\n]*\n'
    r'\s*/\*\s*QRX_ELF2HUNK_PC8_DEFINE_FALLBACK\s*\*/\s*\n'
    r'\s*#ifndef\s+R_68K_PC8\s*\n'
    r'\s*#define\s+R_68K_PC8\s+6\s*\n'
    r'\s*#endif',
    s
)
if not m:
    raise SystemExit("R_68K_PC8 fallback is not immediately after Deadwood R_68K_PC16=5")
print("R_68K_PC8 local relocation-definition scope gate: OK")
PY_ELF2HUNK_SCOPE_GATE

say "Deadwood elf2hunk source gate: R_68K_PC8=6 active beside R_68K_PC16=5"

touch "$ELF2HUNK_SRC"
say "Deadwood elf2hunk source compatibility patch: OK"

# v0.20.3.7.4.6.5.9:
# The previous release patched elf2hunk.c correctly, but distfiles could still
# use the already-built host elf2hunk executable.  Force the host build tool to
# be rebuilt from the patched source *before* invoking the AROS.HUNK conversion.
say "Resolving Deadwood host elf2hunk executable"
ELF2HUNK_BIN="$(make -s -C "$CORE" -pn 2>/dev/null | awk -F' = ' '/^ELF2HUNK = /{print $2; exit}')"

if [[ -z "$ELF2HUNK_BIN" || "$ELF2HUNK_BIN" == *'$('* ]]; then
  ELF2HUNK_BIN="$(find "$CORE" -type f -name elf2hunk -perm -111 2>/dev/null | head -n 1 || true)"
fi

if [[ -n "$ELF2HUNK_BIN" && "$ELF2HUNK_BIN" != /* ]]; then
  ELF2HUNK_BIN="$CORE/$ELF2HUNK_BIN"
fi

if [[ -n "$ELF2HUNK_BIN" && -e "$ELF2HUNK_BIN" ]]; then
  say "Removing stale host elf2hunk: $ELF2HUNK_BIN"
  rm -f "$ELF2HUNK_BIN"
else
  warn "Existing host elf2hunk executable was not found; forcing tools target anyway"
fi

say "Rebuilding Deadwood host tools from patched elf2hunk source"
make -C "$CORE" tools -j "$JOBS"

# Resolve again after rebuilding.  AROS normally installs the host converter in
# the build tool directory; use make's ELF2HUNK variable first and a filesystem
# fallback second.
ELF2HUNK_BIN="$(make -s -C "$CORE" -pn 2>/dev/null | awk -F' = ' '/^ELF2HUNK = /{print $2; exit}')"
if [[ -n "$ELF2HUNK_BIN" && "$ELF2HUNK_BIN" != /* ]]; then
  ELF2HUNK_BIN="$CORE/$ELF2HUNK_BIN"
fi
if [[ -z "$ELF2HUNK_BIN" || ! -x "$ELF2HUNK_BIN" ]]; then
  ELF2HUNK_BIN="$(find "$CORE" -type f -name elf2hunk -perm -111 2>/dev/null | head -n 1 || true)"
fi
[[ -n "$ELF2HUNK_BIN" && -x "$ELF2HUNK_BIN" ]] \
  || die "rebuilt Deadwood host elf2hunk executable not found"

say "Rebuilt host elf2hunk: $ELF2HUNK_BIN"

# Strong provenance gate: the patched binary must contain the new diagnostic
# strings.  If it does not, distfiles would silently run an old converter and
# fail later with 'Unrecognized relocation ... 6'.
if command -v strings >/dev/null 2>&1; then
  strings "$ELF2HUNK_BIN" | grep -q 'Unsupported cross-hunk relocation R_68K_PC8' \
    || die "rebuilt elf2hunk does not contain the QRX PC8 compatibility code"
  strings "$ELF2HUNK_BIN" | grep -q 'Unsupported cross-hunk relocation R_68K_PC16' \
    || die "rebuilt elf2hunk does not contain the QRX PC16 compatibility code"
  say "Host elf2hunk provenance gate: patched PC8/PC16 code present"
else
  warn "strings command unavailable; skipping binary provenance string gate"
fi

# Run the converter once on the exact binary that previously killed distfiles.
# elf2hunk accepts input and output paths; use a temporary destination so this
# preflight cannot mutate the runtime tree.
QRX_ELF2HUNK_PROBE="$SYSROOT/QRX/C/QRXLinuxRun"
if [[ -f "$QRX_ELF2HUNK_PROBE" ]]; then
  PROBE_OUT="$TMP/QRXLinuxRun.elf2hunk-preflight"
  rm -f "$PROBE_OUT"
  say "Preflighting patched elf2hunk with QRXLinuxRun"
  if "$ELF2HUNK_BIN" "$QRX_ELF2HUNK_PROBE" "$PROBE_OUT"; then
    [[ -s "$PROBE_OUT" ]] || die "elf2hunk preflight returned success but produced no output"
    say "Patched elf2hunk QRXLinuxRun preflight: OK"
  else
    die "patched host elf2hunk still rejects QRXLinuxRun"
  fi
else
  warn "QRXLinuxRun unavailable for elf2hunk preflight"
fi

# Force native packaging products to be regenerated after payload injection.
rm -f "$CORE/distfiles/aros-amiga-m68k.iso"
rm -f "$CORE/bin/amiga-m68k/gen/boot/bootdisk-amiga-m68k.adf"
rm -rf "$CORE/bin/amiga-m68k/AROS/Emergency-Boot"

say "Building aligned native LiveCD via Deadwood distfiles"
make -C "$CORE" distfiles -j "$JOBS" EBFHUNKROM=no

ISO="$CORE/distfiles/aros-amiga-m68k.iso"
ADF="$CORE/bin/amiga-m68k/gen/boot/bootdisk-amiga-m68k.adf"
[[ -s "$ISO" ]] || die "native ISO was not produced"
[[ -s "$ADF" ]] || die "Emergency ADF was not produced"
ADF_BYTES="$(stat -c '%s' "$ADF")"
[[ "$ADF_BYTES" -ge 1802240 ]] || die "Emergency ADF too small: $ADF_BYTES"

cp -a "$ISO" "$OUT"
sha256sum "$OUT" > "$OUT.sha256"
cp -a "$ADF" "${OUT%.iso}-emergency.adf"
sha256sum "${OUT%.iso}-emergency.adf" > "${OUT%.iso}-emergency.adf.sha256"

cat > "${OUT%.iso}-build-provenance.txt" <<EOF
QRX OS One Amiga 68K v0.20.3.7.4.6.5.9 – Requester-Free Volume Discovery / Clean QRX Desktop Host
Deadwood commit: $COMMIT
Reference SHA256: $REFSHA
Runtime + packaging: core-amiga-m68k / option 12
QRX payload ABI: v0.20 / Deadwood-native AROS
QRX startup: manual native-ABI migration smoke test
QRX Native Runtime: integrated
QRXDesktop autostart: DISABLED pending manual clean-host smoke test
Emergency-Boot: EBFHUNKROM=no
Emergency ADF: $ADF_BYTES bytes
Final ISO: $OUT
Final SHA256: $(awk '{print $1}' "$OUT.sha256")
EOF

say "Packaging final ISO as ZIP"
ZIP_OUT="${QRX_NATIVE_LIVECD_ZIP_OUT:-${OUT%.iso}.zip}"
rm -f "$ZIP_OUT" "$ZIP_OUT.sha256"
if ! command -v zip >/dev/null 2>&1; then
  apt-get update
  DEBIAN_FRONTEND=noninteractive apt-get install -y zip
fi
zip -9 -j "$ZIP_OUT" "$OUT"
sha256sum "$ZIP_OUT" > "$ZIP_OUT.sha256"
say "ZIP: $ZIP_OUT"
say "ZIP SHA256: $(cat "$ZIP_OUT.sha256")"

say "DONE"
say "ISO: $OUT"
say "SHA256: $(cat "$OUT.sha256")"
say "Runtime commit: $COMMIT"
say "After boot: run QRX:C/QRXArosAbi02, then QRX:C/QRXDesktop."
say "Display policy: existing public screen first -> clean borrowed host; RTG/AGA creation remains fallback."
say "Desktop host: requester-free DosList volumes, no fixed USB probes, front full-screen borrowed host with restored owner state."
say "If QRXDesktop opens, test QRX:C/QRXWriter and QRX:C/QRXHardware."
say "Migration audit: QRX:Diagnostics/NATIVE-ABI-MIGRATION.txt"
