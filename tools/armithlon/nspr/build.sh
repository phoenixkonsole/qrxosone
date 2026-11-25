#!/bin/bash
# Builds NSPR (nsprpub of Firefox ESR 153.8.0, with the AROS port of spidermonkey-153.8.0-aros.patch)
# for m68k64-unknown-aros as static libraries: libnspr4.a, libplc4.a, libplds4.a.
# The sources and defines are those of Mozilla's config/external/nspr/*/moz.build for a pthreads
# Unix, plus md/unix/aros.c. Usage: build.sh <firefox-153.8.0 tree> <out dir>
set -e
T="$(cd "$1" && pwd)"; O="$2"; mkdir -p "$O/obj"
SDK=/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64/AROS/Development
CC="/opt/qrx-os-one/aros64/tc/bin/clang --sysroot=$SDK"
AR=/opt/qrx-os-one/ec64llvm/build/bin/llvm-ar
N="$T/nsprpub"
DEFS="-DBSDSOCKET_NOLIBDEFINES -D__BSDSOCKET_NOLIBBASE__ -D_NSPR_BUILD_ -DXP_UNIX -DAROS -D_PR_PTHREADS -DHAVE_POINTER_LOCALTIME_R -DNDEBUG"
INC="-I$T/config/external/nspr -I$N/pr/include -I$N/pr/include/private -I$N/lib/ds -I$N/lib/libc/include"
CFLAGS="-O2 -fno-pic -fstack-protector-strong -w"
PR="io/prpolevt io/prfdcach io/priometh io/pripv6 io/prlayer io/prlog io/prmapopt io/prmmap io/prmwait
 io/prprf io/prscanf io/prstdio linking/prlink malloc/prmalloc malloc/prmem md/prosdep memory/prseg
 memory/prshma misc/praton misc/pratom misc/prdtoa misc/prenv misc/prerr misc/prerror misc/prerrortable
 misc/prinit misc/prinrval misc/pripc misc/prlog2 misc/prnetdb misc/prrng misc/prsystem misc/prtime
 misc/prtpool threads/prcmon threads/prrwlock threads/prtpd md/unix/unix md/unix/unix_errors
 md/unix/uxproces md/unix/uxrng md/unix/uxshm pthreads/ptio pthreads/ptmisc pthreads/ptsynch
 pthreads/ptthread md/unix/aros"
build() {   # lib, src dir, files
  local lib=$1 dir=$2; shift 2; local objs=()
  for f in "$@"; do
    o="$O/obj/$lib-$(echo $f | tr / _).o"
    $CC $CFLAGS $DEFS $INC -c "$dir/$f.c" -o "$o"
    objs+=("$o")
  done
  rm -f "$O/$lib.a"; $AR rcs "$O/$lib.a" "${objs[@]}"
  echo "$O/$lib.a: ${#objs[@]} objects"
}
build libnspr4 "$N/pr/src" $PR
build libplc4 "$N/lib/libc/src" plvrsion strdup strcase strlen strcpy strcat strcmp strchr strpbrk strstr strtok base64 plerror plgetopt
build libplds4 "$N/lib/ds" plarena plhash plvrsion
