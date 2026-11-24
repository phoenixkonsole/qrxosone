# Prebuilt toolchains and sources for QRX OS One

This branch (`prebuilt-toolchains`) holds built compilers and the source archives they need, so that a
new machine does not have to build them again. It has no history in common with the working branches;
fetch it on its own:

```sh
git fetch origin prebuilt-toolchains
git worktree add ../qrx-prebuilt origin/prebuilt-toolchains   # or: git archive / git show
cd ../qrx-prebuilt && ./unpack.sh /opt/qrx-os-one             # joins, checks SHA-256, unpacks
```

Files are split into parts below GitHub's 100 MB limit (`split -b 90M`); `unpack.sh` joins them and
checks every archive against `SHA256SUMS` before it unpacks anything. Built on x86-64 Ubuntu 24.04
(2026-11-24). LLVM and the C++ runtime rebuilt after the m68k64 frame-lowering fix (callee-saved slots in realigned frames), with `libqrxaroscompat.a`.

| Archive | What | Unpacks to (in the target dir) |
| --- | --- | --- |
| `m68k-aros-gcc-10.5.0-linux-x86_64.tar.xz` | Deadwood AROS m68k cross toolchain, GCC 10.5.0 (32-bit QRX OS One, `.build/qrx-sdk/bin/aros-gcc`) | `toolchain-core-m68k/` (was `deadwood-aros-build/toolchain-core-m68k`) |
| `m68k64-llvm-22.1.2-cxxrt-linux-x86_64.tar.xz` | LLVM 22.1.2 with the m68k64 backend (clang, lld, llc, llvm-mc, binutils tools), compiler-rt headers, and the C++ runtime for m68k64-unknown-aros (libc++, libc++abi, libunwind) | `ec64-llvm-22.1.2/` (was `ec64llvm/build`) |
| `rust-1.100.0-m68k64-linux-x86_64.tar.xz` | rustc 1.100.0 (stage 1) against this LLVM, with the host std, cargo, and the library sources for `-Zbuild-std`: `library/` with the AROS std patch and `libc-aros` (`lib/rustlib/src/rust`) | `rust-1.100.0-m68k64/` (was `rust/rustc-1.100.0-src/build/host/stage1`) |
| `aros64-5b5fd4c-qrx-m68k64.tar.xz` | AROS 5b5fd4c with the qrx-m68k64 patch: the 64-bit system tree with its SDK (`AROS/Development`), kernel and kickstart modules (`gen/rom`), host tools (collect-aros, elf2hunk) | `native-m68k64/`, `linux-x86_64/tools` (was `aros64/build-qrx/bin`) |

| Source | Checked against |
| --- | --- |
| `llvm-project-22.1.2.src.tar.xz` | `tools/armithlon/ec64-llvm/SHA256SUMS` |
| `rustc-1.100.0-src.tar.xz` | the Rust channel manifest of 2026-11-12 (`tools/armithlon/rust/build-rustc.sh`) |
| `aros-portssources.tar` | the third-party archives of the AROS build with their own `portssources.SHA256SUMS` |

How they were made: `tools/armithlon/ec64-llvm/build.sh`, `build-builtins.sh`, `build-runtimes.sh`,
`tools/armithlon/aros-qrx/build-linux.sh`, and the Deadwood toolchain build. The compilers come with the
patches of the working branch (`tools/armithlon/ec64-llvm/*.patch`, `tools/armithlon/aros-qrx/*.patch`)
at commit of 2026-11-24 applied.

Paths: the clang wrapper scripts of the AROS 64 build (`/opt/qrx-os-one/cc64`, `aros64/tc/bin/clang`)
point at absolute paths; after unpacking elsewhere, call `ec64-llvm-22.1.2/bin/clang --target=m68k64-unknown-aros
--sysroot <native-m68k64/AROS/Development>` directly.

Rust: build for QRX OS One 64 with the target spec of the working branch (`tools/armithlon/rust/m68k64-unknown-aros.json`):

```sh
RUSTC=$DIR/rust-1.100.0-m68k64/bin/rustc RUSTC_BOOTSTRAP=1 PATH=$DIR/linux-x86_64/tools:$PATH \
  $DIR/rust-1.100.0-m68k64/bin/cargo build --release -Zjson-target-spec -Zbuild-std=std,panic_unwind \
  -Zunstable-options --target m68k64-unknown-aros.json
```

The spec names the linker `/opt/qrx-os-one/aros64/tc/bin/clang`, the sysroot and `-L /opt/qrx-os-one/ec64llvm/build/lib`
(libunwind, libqrxaroscompat) by absolute path; elsewhere, adjust those three entries in a copy of the spec.
