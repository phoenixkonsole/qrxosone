#!/bin/bash
# Builds rustc 1.100.0 against the patched LLVM 22 (tools/armithlon/ec64-llvm), so that it has the
# m68k64 backend, for the target m68k64-unknown-aros (QRX OS One 64, m68k64-unknown-aros.json here).
#
# Usage: build-rustc.sh <llvm build dir> <work dir>
#   <work dir>/dl/rustc-1.100.0-src.tar.xz   from https://static.rust-lang.org/dist/2026-11-12/
#                                             (SHA-256 checked against the channel manifest)
#
# The LLVM build needs the host backend (rustc compiles build scripts and proc macros for the host)
# and llvm-config: configure it with -DLLVM_TARGETS_TO_BUILD=X86 and build llvm-config and the
# libraries (ninja llvm-config llvm-libraries) besides what build.sh builds.
#
# Disk: without its bundled LLVM, GCC backend and vendored crates the source is ~560 MB;
# the crates come from crates.io. The stage 1 compiler needs several GB more.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
LLVM="$1"; W="$2"; V=rustc-1.100.0-src
[ -x "$LLVM/bin/llvm-config" ] || { echo "no $LLVM/bin/llvm-config (see the comment above)"; exit 2; }
"$LLVM/bin/llvm-config" --targets-built | grep -q X86 || { echo "the LLVM build lacks the host backend X86"; exit 2; }
cd "$W"
echo "90dab90b8084eb57c35ae2b1d0b70c8e5138ace6023b22c60a5a400f586f63b2  dl/$V.tar.xz" | sha256sum -c
if [ ! -d "$V" ]; then
    xz -dc "dl/$V.tar.xz" | tar -x --exclude="$V/src/llvm-project" --exclude="$V/src/gcc" \
        --exclude="$V/vendor"      # tests/ stays: the compiler includes files from it (57 MB)
    patch -p1 -d "$V" < "$HERE/rustc-1.100.0-m68k64.patch"
    # the archive points cargo at its vendor/ directory, which is left out: crates come from crates.io
    mv "$V/.cargo/config.toml" "$V/.cargo/config.toml.vendored"
fi
cat > "$V/bootstrap.toml" <<EOF
change-id = "ignore"
[llvm]
download-ci-llvm = false
[build]
build-stage = 1
docs = false
extended = false
vendor = false
[rust]
channel = "dev"
debug = false
debuginfo-level = 0
lld = false
llvm-tools = false
download-rustc = false
[target.x86_64-unknown-linux-gnu]
llvm-config = "$LLVM/bin/llvm-config"
EOF
cd "$V"
python3 x.py build --stage 1 compiler/rustc library/std
echo "stage 1: $W/$V/build/host/stage1/bin/rustc"
