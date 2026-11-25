#!/bin/sh
# Builds and runs rcrates in the AROS 64 guest. Needs the Gecko tree from ../../../spidermonkey/fetch.sh
# (vendored crates with the AROS patch) at the paths in Cargo.toml and .cargo/config.toml, rustc from
# ../../build-rustc.sh and libqrxaroscompat from ../../compat/build-compat.sh.
# Unix path mode: linked with AROS's nixmain.o, so "/T/x" is T:x and Rust's absolute paths work.
set -e
cd "$(dirname "$0")"
R=${QRX_RUST:-/opt/qrx-os-one/rust/rustc-1.100.0-src}
SDK=${QRX_SDK:-/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64/AROS/Development}
export RUSTC=$R/build/host/stage1/bin/rustc RUSTC_BOOTSTRAP=1 RUST_TARGET_PATH=$(cd ../.. && pwd)
export RUSTFLAGS="-Zunstable-options -Clink-arg=$SDK/lib/nixmain.o"
export PATH=$SDK/../../../linux-x86_64/tools:$PATH
$R/build/x86_64-unknown-linux-gnu/stage0/bin/cargo build --release --target m68k64-unknown-aros
QRX_PRE='Set TMPDIR /T\n' ../run-guest.sh target/m68k64-unknown-aros/release/cratetest
