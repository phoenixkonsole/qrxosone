#!/bin/sh
# configure (or build with "build") SpiderMonkey for QRX OS One 64
cd /opt/qrx-os-one/gecko/firefox-153.8.0
R=/opt/qrx-os-one/rust/rustc-1.100.0-src
export MOZCONFIG=/opt/qrx-os-one/gecko/mozconfig-js MOZBUILD_STATE_PATH=/opt/qrx-os-one/gecko/mozbuild-state
export CC="/opt/qrx-os-one/aros64/tc/bin/clang --sysroot=/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64/AROS/Development" CXX="/opt/qrx-os-one/aros64/tc/bin/clang++ --sysroot=/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64/AROS/Development" HOST_CC=gcc HOST_CXX=g++
export RUSTC=$R/build/host/stage1/bin/rustc CARGO=$R/build/x86_64-unknown-linux-gnu/stage0/bin/cargo RUSTC_BOOTSTRAP=1
export QRX_NO_TESTS=1
export RUST_TARGET_PATH=/home/user/qrxosone/tools/armithlon/rust RUSTFLAGS=-Zunstable-options
export PATH="/opt/qrx-os-one/gecko/tools/bin:/opt/qrx-os-one/aros64/build-qrx/bin/linux-x86_64/tools:/opt/qrx-os-one/ec64llvm/build/bin:$PATH"
exec ./mach ${1:-configure}
