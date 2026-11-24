#!/bin/bash
# Packs the built Blender for the QRX App Store: OUT/blender-2.79b-qrx-<rev>.tar.xz with the folder blender/
#   blender/Start              launcher (an AmigaDOS script: memory, stack, current directory)
#   blender/blender            the program (hunk file, from build.sh)
#   blender/lib/python3.5      Python's library (PROGDIR:lib/python3.5)
#   blender/2.79/scripts       Blender's Python scripts and the add-ons it enables by default
#   blender/COPYING.txt ...    the licences (GPL-2.0 Blender, PSF Python)
# Then prints the size and SHA-256 for the catalog (tools/armithlon/appstore/add-to-catalog.sh).
# usage: package-appstore.sh OUT [rev]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
eval "$("$HERE/qrxenv.py" sh)"
OUT=$(mkdir -p "$1" && cd "$1" && pwd); REV=${2:-1}
SRC=$W/src/blender-2.79b
PY=$W/py/Python-3.5.10
[ -s "$W/build/bin/blender" ] || { echo "no $W/build/bin/blender: run build.sh first"; exit 1; }
S=$(mktemp -d "$W/pkg.XXXXXX"); trap 'rm -rf "$S"' EXIT
B=$S/blender
mkdir -p "$B/lib" "$B/2.79/scripts/addons"
"$W/elf2hunk" "$W/build/bin/blender" "$B/blender" >/dev/null
# the same file set as install_tree.sh
rsync -a --exclude test --exclude tests --exclude idle_test --exclude idlelib --exclude tkinter --exclude turtledemo \
  --exclude lib2to3 --exclude ensurepip --exclude __pycache__ "$PY/Lib/" "$B/lib/python3.5/"
rsync -a --exclude __pycache__ --exclude 'addons/*' --exclude addons_contrib "$SRC/release/scripts/" "$B/2.79/scripts/"
for a in io_scene_3ds io_scene_fbx io_anim_bvh io_mesh_ply io_scene_obj io_scene_x3d io_mesh_stl io_mesh_uv_layout io_curve_svg; do
  rsync -a --exclude __pycache__ "$SRC/release/scripts/addons/$a" "$B/2.79/scripts/addons/"
done
cp "$SRC/COPYING" "$B/COPYING.txt"
cp "$SRC/doc/license/GPL-license.txt" "$B/GPL-license.txt" 2>/dev/null || true
cp "$PY/LICENSE" "$B/Python-LICENSE.txt"
# The launcher is not called "Blender": AROS names ignore case, it would be the program. Blender finds its scripts from the current directory; the guest's memory beyond the first 2 GB
# (QRXMoreMemory) and a large stack, as the tests in docs/BLENDER_AROS68K.md run it.
cat > "$B/Start" <<'EOF'
; Blender 2.79b for QRX OS One, installed by the App Store
C:QRXMoreMemory >NIL:
Stack 8000000
CD APPS:blender
APPS:blender/blender
EOF
chmod 755 "$B/blender" "$B/Start"
A=$OUT/blender-2.79b-qrx-$REV.tar.xz
# reproducible: fixed order, owner and times
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='2026-11-24 00:00Z' -C "$S" -cf - blender | xz -9e -T1 > "$A"
echo "archive:   $A"
echo "bytes:     $(wc -c < "$A")"
echo "installed: $(du -sb "$B" | cut -f1)"
echo "sha256:    $(sha256sum "$A" | cut -d' ' -f1)"
