#!/usr/bin/env bash
# QRX OS One 64 (AROS on the 68100EC64) built on Linux, the steps of README.md in one script:
# the m68k64 clang from ../ec64-llvm, compiler-rt's builtins, AROS 5b5fd4c with the qrx-m68k64
# patch, then the kickstart modules, the shell commands, the libraries and the screen.
#   build-linux.sh LLVM-BUILD LLVM-SRC AROS-SRC WORK
#     LLVM-BUILD  the build dir of ../ec64-llvm/build.sh (bin/clang, ld.lld, llvm-ar, ...)
#     LLVM-SRC    llvm-project-22.1.2.src (for compiler-rt's builtins)
#     AROS-SRC    deadwood AROS at 5b5fd4c with aros-5b5fd4c-qrx-m68k64.patch applied, autoconf run
#     WORK        tc/ (the toolchain wrappers) and build-qrx/ (the AROS build) go here
# Result: WORK/build-qrx/bin/native-m68k64 (gen/rom/boot/core.elf, AROS/...), for
# ../build-qrxos64.sh. The AROS build fetches some third-party archives (zlib and others) itself.
set -Eeuo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LB="$(cd "$1" && pwd)"; LS="$(cd "$2" && pwd)"; SRC="$(cd "$3" && pwd)"; mkdir -p "$4"; W="$(cd "$4" && pwd)"
TC="$W/tc"
mkdir -p "$TC/bin" "$TC/lib/generic"

# 1. the toolchain: clang wrappers for the AROS target, the LLVM tools under their plain names
for t in clang clang++; do
  printf '#!/bin/sh\nexec "%s/bin/%s" --target=m68k64-unknown-aros "$@"\n' "$LB" "$t" > "$TC/bin/$t"
  chmod +x "$TC/bin/$t"
done
for t in ld.lld llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-objdump llvm-strip llvm-as llvm-readelf llvm-mc; do
  [ -e "$LB/bin/$t" ] || { echo "missing $LB/bin/$t (ninja -C LLVM-BUILD llvm-ar llvm-as)" >&2; exit 1; }
  ln -sf "$LB/bin/$t" "$TC/bin/$t"
done
[ -f "$TC/lib/generic/libclang_rt.builtins-m68k64.a" ] ||
  bash "$HERE/../ec64-llvm/build-builtins.sh" "$LB" "$LS" "$TC/lib/generic"

# 2. configure
B="$W/build-qrx"
mkdir -p "$B"
cd "$B"
[ -f config.status ] || CC=gcc CXX=g++ "$SRC/configure" --target=qrx-m68k64 --with-toolchain=llvm \
    --with-aros-toolchain=yes --with-aros-toolchain-install="$TC" --disable-crosstools
J="-j${QRX_BUILD_JOBS:-4}"
# AROS' parallel make sometimes races copying the same header twice ("File exists"), and a -quick
# target can stop with "Error 20" while generating includes, its include/{proto,clib,defines,inline}
# directories under gen/ missing (README). A failed step runs again with one job, up to eight
# times, after making the directories its error names.
mk() {
  echo "== gmake $*"
  gmake $J "$@" && return
  local try out
  for try in 1 2 3 4 5 6 7 8; do
    echo "== again with one job ($try): $*"
    out=$(mktemp)
    if gmake -j1 "$@" 2>&1 | tee "$out"; then
      [ "${PIPESTATUS[0]}" = 0 ] && { rm -f "$out"; return; }
    fi
    # .../gen/<module>/include/proto/<name>.h: No such file or directory
    grep -o '/[^ ]*/include/proto/[^ :]*\.h: No such file' "$out" | sed 's|/proto/[^/]*$||' | sort -u |
      while read -r d; do mkdir -p "$d/proto" "$d/clib" "$d/defines" "$d/inline"; done
    rm -f "$out"
  done
  return 1
}

# 3. the kernel and the kickstart modules
mk kernel-utility kernel-qrx-m68k64
mk linklibs-romhack kernel-dos kernel-timer kernel-bootloader kernel-shell
# dosboot links hiddstubs, which README's order only builds later with the graphics (on a tree
# built step by step it was already there)
mk linklibs-hiddstubs
mk kernel-expansion-quick kernel-econsole-quick kernel-dosboot-quick kernel-shellcommands-quick
mk kernel-aros-quick kernel-lddemon-quick kernel-filesystem-quick kernel-fs-afs-quick kernel-qrxdisk-quick
mk compiler-m-m68k64 compiler-m-quick compiler-stdlib-m68k64 compiler-stdlib-quick \
   compiler-crt-m68k64 compiler-crt-quick
mkdir -p bin/native-m68k64/AROS/Development/lib
cp "$TC/lib/generic/libclang_rt.builtins-m68k64.a" bin/native-m68k64/AROS/Development/lib/
mk kernel-battclock-native-m68k64-qrx kernel-battclock-quick
mk workbench-libs-uuid-quick workbench-libs-iffparse-quick workbench-libs-locale-quick
mk workbench-c-quick workbench-c-sh-quick workbench-c-private-quick
# 4. the screen: graphics, Intuition and the board's framebuffer driver
mk kernel-oop kernel-graphics kernel-intuition kernel-hidd kernel-hidd-gfx kernel-layers kernel-keymap
mk kernel-input-quick kernel-console-quick kernel-keyboard-quick kernel-gameport-quick kernel-fs-con-quick
mk kernel-hidd-input kernel-hidd-kbd kernel-hidd-mouse kernel-hidd-ec64gfx-quick workbench-libs-cgfx-quick
mk workbench-libs-gadtools-quick workbench-libs-workbench workbench-libs-diskfont-quick
# 5. zlib: the fetched port has an empty zconf.h; take zconf.h.in, with unistd.h and stdarg.h on
mk zlib-fetch
Z=bin/native-m68k64/Ports/zlib/zlib
cp "$Z/zconf.h.in" "$Z/zconf.h"
sed -i 's|^#ifdef HAVE_UNISTD_H .*|#if 1|; s|^#ifdef HAVE_STDARG_H .*|#if 1|' "$Z/zconf.h"
mk linklibs-z-static workbench-libs-z-quick
# coolimages.library links the coolimagesstatic linklib (likewise not in README's order)
mk linklibs-coolimagesstatic
mk workbench-libs-asl-catalogs workbench-libs-coolimages-quick workbench-libs-asl-quick
echo "QRX OS One 64 built: $B/bin/native-m68k64"
