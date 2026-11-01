#!/usr/bin/env bash
# Adds independently built QRX m68k tools to AROS LiveCD; does NOT turn AROS into QRX OS One.
set -Eeuo pipefail
[[ $# -eq 3 ]] || { echo "Usage: make-preview-iso.sh BASE.iso QRX-HUNK.zip OUT.iso" >&2; exit 2; }
base="$1"; archive="$2"; output="$3"
for p in xorriso python3; do command -v "$p" >/dev/null || { echo "$p missing" >&2; exit 2; }; done
[[ -f "$base" && -f "$archive" && "$base" != "$output" ]] || { echo 'Invalid input' >&2; exit 2; }
mkdir -p "$(dirname "$output")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python3 - "$archive" "$work" <<'PY'
from pathlib import Path
from zipfile import ZipFile
import sys
archive, work = sys.argv[1:]
work=Path(work)
root='QRX-22.5.11-m68k-HUNK/'
chosen={}
with ZipFile(archive) as z:
    for info in z.infolist():
        if info.is_dir():continue
        name=info.filename
        if not name.startswith(root):continue
        relative=Path(name[len(root):])
        if relative.parts[0] == 'C' and len(relative.parts)==2:
            target=Path('C')/relative.name
        elif relative.parts[0] == 'Tests' and len(relative.parts)==2:
            target=Path('Tests')/relative.name
        elif str(relative)=='TestAmigaRun':
            target=Path('S')/'TestAmigaRun'
        elif str(relative)=='README.txt':
            target=Path('README-22.5.11.txt')
        else:continue
        if str(target) in chosen:raise SystemExit('Duplicate tool: '+str(target))
        if info.file_size > 16*1024*1024:raise SystemExit('Suspiciously large tool: '+name)
        chosen[str(target)]=z.read(info)
if not {'C/amigarun','C/amigacheck','C/qrx-jit-selftest','C/QRXAmigaRun'}.issubset(chosen):
    raise SystemExit('QRX-HUNK archive has unexpected layout; refusing incomplete ISO preview')
dest=work/'QRXTools'
for name,data in chosen.items():
    f=dest/name; f.parent.mkdir(parents=True,exist_ok=True); f.write_bytes(data)
(dest/'ABOUT.txt').write_text(
  'QRX OS One 22.5.11 separately built HUNK tools. AROS 5b5fd4c base ISO. '
  'This preview lacks the complete QRX v0.20 payload and QRXDesktop. '
  'Not a QRX OS One release.\n')
print('Attached QRX HUNK programs:', ', '.join(sorted(chosen)))
PY
# Clone the original ISO tree and append a separate, explicitly labeled QRX tools directory.
# It is NOT an Amiga standalone boot test; boot with the separate AROS emergency floppy.
xorriso -indev "$base" -outdev "$output.tmp" \
  -map "$work/QRXTools" /QRXTools -commit -end
mv "$output.tmp" "$output"
printf 'Created PREVIEW ISO (not QRX OS One release): %s\n' "$output"
