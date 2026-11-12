#!/bin/bash
# Stubs for the native comparison build: FreeType and JPEG are not on this Mac, and `-b` to PNG calls
# only FT_Init_FreeType and a font load, which may fail. Anything else stops with a message.
set -eu
W=${QRX_BLENDER_WORK:-/Users/pascalpapara/Documents/GitHub/qrxosone/.build/blender-2.79b}
{
  echo '#include <stdio.h>'; echo '#include <stdlib.h>'
  echo 'static void gone(const char *n) { fprintf(stderr, "host stub called: %s\n", n); exit(20); }'
  echo 'int FT_Init_FreeType(void **lib) { *lib = (void *)1; return 0; }'
  echo 'int FT_Done_FreeType(void *lib) { (void)lib; return 0; }'
  echo 'int FT_New_Memory_Face(void) { return 1; }'
  echo 'int FT_New_Face(void) { return 1; }'
  nm -u "$W/hostref/bin/blender" | grep -E '^_(FT_|jpeg_|jmemsrc|qrx_)' | sed 's/^_//' | grep -vE '^FT_((Init|Done)_FreeType|New_Memory_Face|New_Face)$' | \
    while read -r n; do echo "void $n(void) { gone(\"$n\"); }"; done
} > "$W/hoststub.c"
cc -w -dynamiclib "$W/hoststub.c" -o "$W/hoststub.dylib"
