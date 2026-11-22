#!/bin/sh
# Build the Mac compatibility tools for the m68k64 AROS target
# (QRX OS One 64-bit SMP, 68100EC64).
#
#   ./build64.sh <aros tc/bin dir> <aros build-qrx/bin dir> <output dir>
#
# On Apple Silicon the toolchain tools sit under the "darwin-aarch64/tools"
# of the AROS tree; the sysroot is the AROS-64 development tree with headers
# and static libraries.
#
# The binaries are left as plain ELFs; the caller packs them via mkffsimg.
#
# Note: the m68k64 clang backend miscompiles qrx_toolbox.c at -O2 (and
# crashes at -O0 on this tree), so that one file is compiled with -O1 here.
set -eu

TC_BIN=${1:?"path to the AROS tc/bin directory"}
AROS_BIN=${2:?"path to the AROS build-qrx/bin directory"}
OUT_DIR=${3:?"output directory"}
shift 3
[ $# -eq 0 ] || { echo "usage: $0 <tc/bin> <build-qrx/bin> <out-dir>"; exit 2; }

BASE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

export PATH="$AROS_BIN/darwin-aarch64/tools:$PATH"
clang() { "$TC_BIN/clang" --sysroot "$AROS_BIN/native-m68k64/AROS/Development" "$@"; }

PPC_SRCS="qrx_ppc.c qrx_ppc_fpu.c qrx_ppc_jit.c"
PEF_SRCS="qrx_pef.c qrx_macapp.c"
MM_SRCS="qrx_toolbox.c qrx_mix.c"
APP_SRCS="qrx_apprun.c qrx_appstub.c"

MUSASHI="$BASE/../../upstream/armithlon/2.6/engine/third_party/Musashi"
GEN_DIR=${MUSASHI_GEN:-$OUT_DIR/m68kgen}

build()
{
    # build <output name> <source>...
    out=$1; shift
    objs=""
    i=0
    for f in "$@"; do
        i=$((i + 1))
        obj="$OUT_DIR/.$out-$i.o"
        if [ "$f" = "qrx_toolbox.c" ]; then OPT="-O1"; else OPT="-O2"; fi
        INC="-I$BASE -I$BASE/.. -I$MUSASHI -I$GEN_DIR -DM68K_ILLG_HAS_CALLBACK=1"
        src=$f
        case $src in
            */*) : ;;
            *)   src="$BASE/$f" ;;
        esac
        clang $OPT $INC -c "$src" -o "$obj"
        objs="$objs $obj"
    done
    clang -o "$OUT_DIR/$out" $objs
    rm -f $objs
}

mkdir -p "$OUT_DIR"

build QRXMach qrx_macho.c qrx_dyld.c qrx_ppc.c qrx_ppc_fpu.c qrx-mach.c
build QRXMachTest qrx_macho.c qrx_dyld.c test_macho.c qrx_ppc.c qrx_ppc_fpu.c qrx_ppc_jit.c
build QRXPPC $PPC_SRCS $PEF_SRCS $MM_SRCS $APP_SRCS qrx_ppc_test.c
build QRXAppRun qrx_ppc.c qrx_ppc_fpu.c $PEF_SRCS $MM_SRCS $APP_SRCS qrx-app.c

# 68k classic runtime (Musashi). The opcode table must already have been
# generated (build.sh does it into .build/qrxppc/m68kgen); pass MUSASHI_GEN
# to point elsewhere.
if [ -f "$MUSASHI/m68kcpu.c" ] && [ -f "$GEN_DIR/m68kops.c" ]; then
    build QRX68KRun qrx_m68k.c qrx_macapp.c qrx_pef.c qrx_ppc.c qrx_ppc_fpu.c "$MUSASHI/m68kcpu.c" "$MUSASHI/softfloat/softfloat.c" "$GEN_DIR/m68kops.c" qrx-68k.c
else
    echo "note: QRX68KRun needs Musashi + generated m68kops.c ($GEN_DIR/m68kops.c)" >&2
fi

echo "ok: $OUT_DIR/QRXMach $OUT_DIR/QRXMachTest $OUT_DIR/QRXPPC $OUT_DIR/QRXAppRun"
