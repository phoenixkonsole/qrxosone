#!/bin/sh
# make in the SpiderMonkey objdir with the environment of jsconf.sh
cd /opt/qrx-os-one/gecko/obj-js
R=/opt/qrx-os-one/rust/rustc-1.100.0-src
export RUSTC=$R/build/host/stage1/bin/rustc CARGO=$R/build/x86_64-unknown-linux-gnu/stage0/bin/cargo RUSTC_BOOTSTRAP=1
export QRX_NO_TESTS=1 RUST_TARGET_PATH=/home/user/qrxosone/tools/armithlon/rust RUSTFLAGS=-Zunstable-options
export PATH="/opt/qrx-os-one/gecko/tools/bin:/opt/qrx-os-one/aros64/build-qrx/bin/linux-x86_64/tools:/opt/qrx-os-one/ec64llvm/build/bin:$PATH"
exec make -j4 -k "$@"
