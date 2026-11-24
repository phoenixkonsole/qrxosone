# Rust für QRX OS One 64 (m68k64-unknown-aros)

Voraussetzung für Gecko/SpiderMonkey, die Rust zwingend brauchen.

| Teil | Datei |
| --- | --- |
| rustc 1.100.0 gegen das gepatchte LLVM 22 (m68k64-Backend) | `build-rustc.sh`, `rustc-1.100.0-m68k64.patch` |
| Zielbeschreibung | `m68k64-unknown-aros.json` (LP64, Big Endian, keine FPU, Linken über clang/collect-aros) |
| Testprogramm ohne `std` | `tests/rhello` |

```sh
tools/armithlon/rust/build-rustc.sh /opt/qrx-os-one/ec64llvm/build /opt/qrx-os-one/rust
R=/opt/qrx-os-one/rust/rustc-1.100.0-src
cd tools/armithlon/rust/tests/rhello
RUSTC=$R/build/host/stage1/bin/rustc RUSTC_BOOTSTRAP=1 \
  $R/build/x86_64-unknown-linux-gnu/stage0/bin/cargo build --release -Zjson-target-spec \
  -Zbuild-std=core,alloc -Zbuild-std-features=compiler-builtins-mem -Zunstable-options \
  --target ../../m68k64-unknown-aros.json
```

Das LLVM braucht dafür zusätzlich das Host-Backend und `llvm-config` (`-DLLVM_TARGETS_TO_BUILD=X86`,
`ninja llvm-config llvm-libraries`). Die rustc-Patches: rustc_llvm kennt die Komponente `m68k64`; für
m68k mit 64-Bit-Zeigern schreibt rustc kein Metadaten-Objekt, weil das `object`-Crate m68k nur als ELF32
kennt (die Metadaten bleiben roh im rlib, lld meldet das nur als Warnung).

## Stand (24.11.2026)

- `core`, `alloc` und `compiler_builtins` übersetzen ohne Fehler.
- `tests/rhello` läuft im 64-Bit-AROS-Gast auf zwei EC64-Kernen (x86-64-Referenzmaschine): Vec, BTreeMap,
  `format!`, Gleitkomma, 128-Bit-Ganzzahlen; Allokator über `malloc` der AROS-C-Bibliothek. Ausgabe
  `PASS: Rust core+alloc on m68k64`.
- Noch nicht: `std` für AROS (Plattformschicht und Bindungen im `libc`-Crate), Abwickeln von Panics
  (das Testprogramm nutzt `panic=abort`), ein Lauf mit dem ARM64-JIT auf dem Pi.
