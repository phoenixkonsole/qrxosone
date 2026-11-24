# Rust für QRX OS One 64 (m68k64-unknown-aros)

Voraussetzung für Gecko/SpiderMonkey, die Rust zwingend brauchen.

| Teil | Datei |
| --- | --- |
| rustc 1.100.0 gegen das gepatchte LLVM 22 (m68k64-Backend) | `build-rustc.sh`, `rustc-1.100.0-m68k64.patch` |
| Zielbeschreibung | `m68k64-unknown-aros.json` (LP64, Big Endian, keine FPU, Linken über clang/collect-aros) |
| std für AROS | `rustc-1.100.0-aros-std.patch` (`std::os::aros`, Plattformschicht), `libc-0.2.189-aros.patch` |
| POSIX-Funktionen, die AROS fehlen | `compat/qrxaroscompat.c`, `compat/build-compat.sh` → `libqrxaroscompat.a` |
| Tests | `tests/rhello` (ohne std), `tests/rstd` (std), `tests/runwind` (Panic-Abwicklung), `tests/run-guest.sh` |

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

Alles getestet im 64-Bit-AROS-Gast auf zwei EC64-Kernen (x86-64-Referenzmaschine `qrxboot`, `tests/run-guest.sh`).

- `core`/`alloc`: `tests/rhello` → `PASS: Rust core+alloc on m68k64`.
- **`std` läuft.** `tests/rstd` → `ALL PASS`: Argumente, Threads mit Mutex, `mpsc`, Condvar mit Timeout und
  Notify, Dateien schreiben/lesen/Metadaten/löschen, `read_dir`, `io::Error` mit AROS-Fehlertext, HashMap
  (Zufallsstartwert), f64 (Soft-Float), Umgebungsvariablen, Systemzeit.
- **Panics wickeln ab** (`panic = "unwind"`, die Vorgabe des Ziels). `tests/runwind` → `UNWIND ALL PASS`:
  `catch_unwind` durch sechs Frames mit Destruktoren, Payload, Panic im Thread → `join()` gibt `Err`,
  20 Panics hintereinander, bei 16 und bei 1 Codegen-Unit.

So wird gebaut (nach `build-rustc.sh` und `compat/build-compat.sh`):

```sh
cd tools/armithlon/rust/tests/rstd
RUSTC=$R/build/host/stage1/bin/rustc RUSTC_BOOTSTRAP=1 PATH=/opt/qrx-os-one/aros64/build-qrx/bin/linux-x86_64/tools:$PATH \
  $R/build/x86_64-unknown-linux-gnu/stage0/bin/cargo build --release -Zjson-target-spec \
  -Zbuild-std=std,panic_unwind -Zunstable-options --target ../../m68k64-unknown-aros.json
../run-guest.sh target/m68k64-unknown-aros/release/rstd
```

Was dafür nötig war:

- `libc`: Typen, Konstanten und Strukturen mit `gen-libc-aros.py` gegen die AROS-64-SDK-Header gemessen
  (Größen, Vorzeichen, Makrowerte, Feld-Offsets aus clangs Record-Layouts). pthread-Typen sind opak in
  AROS-Größe, die Initialisierer Nullbytes wie in AROS. `S_I*` als `mode_t` (u16), `addrinfo` mit `long`-Feldern.
  Bibliotheken linkt der AROS-Treiber von clang, nicht das Crate.
- std: `std::os::aros` (nach NuttX), `errno` über `___geterrnoptr`, Condvar mit `CLOCK_REALTIME` (AROS hat
  kein `pthread_condattr_setclock`), `os_family = unix` mit den AROS-Lücken (kein `openat`, kein `linkat`).
- `libqrxaroscompat.a`: `strerror_r`, `pread`/`pwrite` (über `lseek`, nicht atomar), `poll` (meldet alles bereit;
  AROS' `libcrt` hat auch kein `select`), `arc4random_buf` (**nicht kryptografisch**: AROS hat keine
  Entropiequelle), Sockets/`getaddrinfo`/`mkfifo`/`lchown`/`chroot` → `ENOSYS`.
- Abwickeln: libunwind baut für m68k64 jetzt die `_Unwind_Backtrace`-Familie (`config.h`), und
  `__register_frame`, das der AROS-Start libgcc-artig mit der ganzen `.eh_frame`-Sektion aufruft, macht dort
  nichts (die Bare-Metal-Suche findet die Sektion selbst).
- **Fehler im m68k64-Backend behoben:** In Funktionen mit neu ausgerichtetem Stapel (Objekte mit 16-Byte-
  Ausrichtung, etwa `i128`) lagen die Sicherungsplätze der callee-saved Register im ausgerichteten Bereich.
  Ihre CFI-Offsets stimmten daher nur zufällig, das Abwickeln stellte Register verschoben wieder her.
  Außerdem wurden feste Objekte (Stapelargumente) mit FP-Offset über `sp` adressiert. Jetzt sind die
  Sicherungsplätze feste Objekte unter dem gesicherten FP, und feste Objekte gehen in solchen Funktionen
  über den FP (wie im x86-Backend). Die Backend-, Builtins- und C++-Laufzeittests laufen danach weiter fehlerfrei.

Noch nicht:

- `std::net`: AROS' Sockets leben in der `bsdsocket.library` mit eigenen Deskriptoren. Die Funktionen liefern
  `Unsupported` (os error 78). Für Gecko/Tor kommt das Netz ohnehin über NSPR in C.
- `std::process` (AROS hat kein `fork`/`exec`), `poll` auf Pipes/Konsole (meldet immer bereit).
- Ein Lauf mit dem ARM64-JIT auf dem Pi (bisher nur die x86-64-Referenzmaschine).
- Das übrige AROS-64-System ist noch mit dem LLVM vor der Backend-Korrektur gebaut. Betroffen wären nur
  Funktionen mit neu ausgerichtetem Stapel, die selten sind (C ohne `i128`/`alignas(16)`). Ein Neubau steht aus.
