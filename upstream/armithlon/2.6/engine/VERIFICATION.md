# Reproducible v0.2 checks

Linux x86-64 build & virtual monitor:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/qrx68100_vm
```

Linux x86-64 sanitizers:

```sh
cmake -S . -B build-sanitized -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -O1' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-sanitized --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-sanitized --output-on-failure
```

Apple M1: `sh tools/test-m1.sh` for an actual JIT execution gate. Runs `vm_unit_tests`, which compares VM state to the interpreter and tests one-block cache reuse and invalidation on guest writes. A macOS code-signing entitlement may be required for hardened distributions.

No M1 execution results are provided with this archive.
