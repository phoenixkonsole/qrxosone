#!/bin/bash
# GLEW 1.13's glew.c for AROS: functions come from the GL door's qgl_GetProcAddress, and there is no GLX.
# usage: glew_aros.sh glew.c out.c
set -eu
python3 - "$1" "$2" <<'PY'
import sys
s = open(sys.argv[1]).read()
glx = "!defined(__ANDROID__) && !defined(__native_client__) && !defined(__HAIKU__)"
assert glx in s
s = s.replace(glx, glx + " && !defined(__AROS__)")
anchor = "#elif defined(__native_client__)\n#  define glewGetProcAddress(name) NULL /* TODO */"
assert anchor in s
s = s.replace(anchor, "#elif defined(__AROS__)\nextern void *qgl_GetProcAddress(const char *name);\n"
              "#  define glewGetProcAddress(name) qgl_GetProcAddress((const char *)(name))\n" + anchor, 1)
open(sys.argv[2], 'w').write(s)
PY
