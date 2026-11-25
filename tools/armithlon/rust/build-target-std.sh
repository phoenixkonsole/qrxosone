#!/bin/bash
# Builds the standard library for m68k64-unknown-aros with cargo -Zbuild-std and installs it in the
# sysroot of the stage 1 rustc (lib/rustlib/m68k64-unknown-aros/lib), so that builds which only call
# "rustc --target m68k64-unknown-aros" (Gecko's) find core, alloc, std and the panic runtimes.
# x.py removes that directory when it rebuilds the compiler: run this again after every rustc build.
# Usage: build-target-std.sh <rustc-1.100.0-src dir>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
R="$(cd "$1" && pwd)"
RUSTC="$R/build/host/stage1/bin/rustc"; CARGO="$R/build/x86_64-unknown-linux-gnu/stage0/bin/cargo"
[ -x "$RUSTC" ] && [ -x "$CARGO" ] || { echo "usage: $0 <rustc-1.100.0-src> (with a stage 1 build)"; exit 2; }
T=m68k64-unknown-aros
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
mkdir -p "$W/src"
printf '[package]\nname = "qrxsysroot"\nversion = "0.1.0"\nedition = "2021"\n[lib]\npath = "src/lib.rs"\n' > "$W/Cargo.toml"
echo '#![no_std]' > "$W/src/lib.rs"
# like x.py's rustc shim: panic_abort itself is compiled with the abort strategy, the rest unwinds
cat > "$W/rustc-wrap" <<'EOS'
#!/bin/sh
r="$1"; shift
case " $* " in *" --crate-name panic_abort "*) exec "$r" "$@" -C panic=abort;; esac
exec "$r" "$@"
EOS
chmod +x "$W/rustc-wrap"
cd "$W"
RUSTC_WRAPPER="$W/rustc-wrap" RUSTC="$RUSTC" RUSTC_BOOTSTRAP=1 RUST_TARGET_PATH="$HERE" RUSTFLAGS=-Zunstable-options \
  "$CARGO" build --release --target $T -Zbuild-std=std,panic_unwind,panic_abort
D="$R/build/host/stage1/lib/rustlib/$T/lib"
rm -rf "$D"; mkdir -p "$D"
cp target/$T/release/deps/*.rlib "$D/"
rm -f "$D"/libqrxsysroot-*.rlib
echo "$D: $(ls "$D" | wc -l) libraries"
