#!/bin/bash
# Builds the door test as a 68k program into the Blender test tree. GLEW comes from Blender's source.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
W=${QRX_BLENDER_WORK:-/Users/pascalpapara/Documents/GitHub/qrxosone/.build/blender-2.79b}
CC=$HERE/../../blender/aros_cc.py
O=$W/qgl-test
mkdir -p "$O"
"$HERE/../glew_aros.sh" "$W/src/blender-2.79b/extern/glew/src/glew.c" "$O/glew.c"
INC="-I$W/src/blender-2.79b/extern/glew/include -I$HERE/../guest"
for f in "$HERE/../guest/qgl.c" "$HERE/../guest/qgl_gen.c" "$O/glew.c" "$HERE/scene.c" "$HERE/guest_main.c"; do
  "$CC" -O2 -DGLEW_STATIC -DGLEW_NO_GLU $INC -c "$f" -o "$O/$(basename "${f%.c}").o"
done
"$CC" -o "$O/scene_guest" "$O"/qgl.o "$O"/qgl_gen.o "$O"/glew.o "$O"/scene.o "$O"/guest_main.o
"$W/elf2hunk" "$O/scene_guest" "$W/tree/scene_guest" >/dev/null
chmod +x "$W/tree/scene_guest"
echo "built: $W/tree/scene_guest"
