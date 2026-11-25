#!/bin/bash
# Builds the C++ runtime for m68k64-unknown-aros (QRX OS One 64) from the patched LLVM tree:
# libunwind (DWARF unwinding; bare-metal lookup of __eh_frame_start/__eh_frame_end, which AROS's
# collect-aros provides), libc++abi and libc++ with AROS pthreads. Installs into the LLVM build
# directory, where clang's AROS driver looks: <llvm build>/lib and <llvm build>/include/c++/v1.
# thread_local uses emulated TLS (the AROS default of this clang), whose runtime build-builtins.sh
# adds to the builtins when it is given the SDK.
# Usage: build-runtimes.sh <llvm build dir> <llvm-project source dir> <AROS Development dir> <work dir>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$1"; SRC="$2"; SDK="$3"; W="$4"
[ -x "$B/bin/clang" ] && [ -d "$SRC/runtimes" ] && [ -d "$SDK/include" ] || {
    echo "usage: $0 <llvm build> <llvm-project src> <AROS Development> <work dir>"; exit 2; }
T=m68k64-unknown-aros
cmake -G Ninja -S "$SRC/runtimes" -B "$W" -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
  -DCMAKE_C_COMPILER="$B/bin/clang" -DCMAKE_CXX_COMPILER="$B/bin/clang++" -DCMAKE_ASM_COMPILER="$B/bin/clang" \
  -DCMAKE_C_COMPILER_TARGET=$T -DCMAKE_CXX_COMPILER_TARGET=$T -DCMAKE_ASM_COMPILER_TARGET=$T \
  -DCMAKE_SYSROOT="$SDK" -DCMAKE_SYSTEM_NAME=Generic -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_PROJECT_INCLUDE="$HERE/runtimes/generic-shared.cmake" \
  -DCMAKE_AR="$B/bin/llvm-ar" -DCMAKE_RANLIB="$B/bin/llvm-ranlib" -DCMAKE_NM="$B/bin/llvm-nm" \
  -DCMAKE_INSTALL_PREFIX="$B" -DLLVM_INCLUDE_TESTS=OFF \
  -DLIBUNWIND_IS_BAREMETAL=ON -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_THREADS=ON -DLIBUNWIND_USE_COMPILER_RT=OFF \
  -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_ENABLE_THREADS=ON \
  -DLIBCXXABI_HAS_PTHREAD_API=ON -DLIBCXXABI_USE_COMPILER_RT=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
  -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF \
  -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_HAS_PTHREAD_API=ON -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXX_USE_COMPILER_RT=OFF \
  -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=OFF -DLIBCXX_ENABLE_FILESYSTEM=ON -DLIBCXX_ENABLE_RANDOM_DEVICE=OFF \
  -DLIBCXX_ENABLE_LOCALIZATION=ON -DLIBCXX_ENABLE_WIDE_CHARACTERS=ON \
  -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF
ninja -C "$W"
ninja -C "$W" install
