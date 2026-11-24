#!/bin/bash
# Puts Blender, Python and their scripts into the test tree: SYS:blender, SYS:python, SYS:lib/python3.5,
# SYS:2.79/scripts with the add-ons Blender enables by default (Cycles is not built).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
eval "$("$HERE/qrxenv.py" sh)"
T=$W/tree
S=$W/src/blender-2.79b/release/scripts
"$W/elf2hunk" "$W/build/bin/blender" "$T/blender"
"$W/elf2hunk" "$W/py/build/python.elf" "$T/python"
chmod +x "$T/blender" "$T/python"
mkdir -p "$T/lib" "$T/2.79/scripts/addons"
rsync -a --delete --exclude test --exclude tests --exclude idle_test --exclude idlelib --exclude tkinter --exclude turtledemo \
  --exclude lib2to3 --exclude ensurepip --exclude __pycache__ "$W/py/Python-3.5.10/Lib/" "$T/lib/python3.5/"
rsync -a --delete --exclude __pycache__ --exclude 'addons/*' --exclude addons_contrib "$S/" "$T/2.79/scripts/"
for a in io_scene_3ds io_scene_fbx io_anim_bvh io_mesh_ply io_scene_obj io_scene_x3d io_mesh_stl io_mesh_uv_layout io_curve_svg; do
  rsync -a --exclude __pycache__ "$S/addons/$a" "$T/2.79/scripts/addons/"
done
