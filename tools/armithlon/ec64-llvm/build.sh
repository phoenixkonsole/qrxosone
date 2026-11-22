#!/bin/bash
# Unpacks LLVM 22.1.2, adds the m68k64 target and builds clang, lld, llc, llvm-mc and the tools the tests use.
# The archive comes from https://github.com/llvm/llvm-project/releases: llvm-project-22.1.2.src.tar.xz.
# Usage: build.sh <work dir>   with the archive in <work dir>/dl
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; W="$(cd "$1" && pwd)"; V=llvm-project-22.1.2.src
cd "$W/dl"; shasum -a 256 -c "$HERE/SHA256SUMS"; cd "$W"
[ -d "$V" ] && rm -rf "$V"      # a fresh tree: the patches apply once
tar xJf "dl/$V.tar.xz" --exclude="$V/clang-tools-extra" --exclude="$V/lldb" --exclude="$V/flang" --exclude="$V/mlir" --exclude="$V/polly" --exclude="$V/libc" --exclude="$V/openmp" --exclude="$V/bolt" --exclude='*/test/*'
python3 "$HERE/make-fork.py" "$V/llvm/lib/Target"
patch -p1 -d "$V" <"$HERE/llvm-22.1.2-core.patch"
patch -p1 -d "$V" <"$HERE/llvm-22.1.2-m68k64.patch"
cmake -G Ninja -S "$V/llvm" -B build -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD= -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=M68k64 \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_OPTIMIZED_TABLEGEN=ON \
  "-DLLVM_ENABLE_PROJECTS=clang;lld" -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_INCLUDE_TESTS=OFF \
  ${LLVM_CMAKE_EXTRA:-}   # e.g. -DLLVM_PARALLEL_LINK_JOBS=1 on machines with little memory
ninja -C build clang lld llc llvm-mc llvm-ar llvm-ranlib llvm-as llvm-strip \
  llvm-objcopy llvm-nm llvm-objdump llvm-readelf
