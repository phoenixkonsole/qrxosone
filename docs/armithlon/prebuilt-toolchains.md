# Fertige Compiler und Quellen (Zweig `prebuilt-toolchains`)

Damit ein neuer Rechner die Compiler nicht neu bauen muss, liegen sie fertig im Zweig `prebuilt-toolchains`.
Dieser Zweig hat keine gemeinsame Geschichte mit den Arbeitszweigen, ein normaler Checkout lädt ihn nicht mit.

```sh
git fetch origin prebuilt-toolchains
git worktree add ../qrx-prebuilt origin/prebuilt-toolchains
../qrx-prebuilt/unpack.sh /opt/qrx-os-one        # setzt zusammen, prüft SHA-256, packt aus
```

Inhalt (Stand 24.11.2026, gebaut auf x86-64-Ubuntu 24.04):

| Archiv | Inhalt |
| --- | --- |
| `m68k-aros-gcc-10.5.0-linux-x86_64.tar.xz` | Deadwood-AROS-m68k-Crosscompiler GCC 10.5.0 (QRX OS One, 32 Bit) |
| `m68k64-llvm-22.1.2-cxxrt-linux-x86_64.tar.xz` | LLVM 22.1.2 mit m68k64-Backend (clang, lld, llc, …) und der C++-Laufzeit für m68k64-unknown-aros |
| `aros64-5b5fd4c-qrx-m68k64.tar.xz` | AROS 64: Systembaum mit SDK, Kernel und Kickstart-Module, collect-aros und elf2hunk |
| Quellen | LLVM 22.1.2, rustc 1.100.0, die Fremdarchive des AROS-Baus (alle mit Prüfsummen) |

Die Teile sind kleiner als 100 MB (Grenze von GitHub). `unpack.sh` prüft jedes Archiv gegen `SHA256SUMS`,
bevor es etwas auspackt. Die Patches, mit denen die Compiler gebaut sind, liegen im Arbeitszweig unter
`tools/armithlon/ec64-llvm/` und `tools/armithlon/aros-qrx/`.
