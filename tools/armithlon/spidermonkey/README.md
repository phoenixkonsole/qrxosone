# SpiderMonkey für QRX OS One 64 (m68k64-unknown-aros)

Schritt 7 des Fahrplans für Gecko/Tor Browser (`docs/tor-browser-machbarkeit-2026-11.md`): die JavaScript-Engine
von Firefox ESR 153.8.0 (Basis von Tor Browser 16), nur als Interpreter (`--disable-jit`), ohne Intl.

| Datei | Zweck |
| --- | --- |
| `fetch.sh WORKDIR` | lädt die Firefox-Quelle (805 MB, SHA-256 aus Mozillas `SHA256SUMS`), packt den Teil aus, den Mozillas eigenes SpiderMonkey-Paket nimmt, wendet den Patch an |
| `spidermonkey-153.8.0-aros.patch` | die AROS-Änderungen; `make-patch.py` schreibt ihn aus einem Arbeitsbaum (Originale als `*.qrxorig`, von `qorig.sh` vor der ersten Änderung angelegt) |
| `mozconfig` | `--target=m68k64-unknown-aros --disable-jit --without-intl-api --disable-tests --disable-jemalloc` |
| `configure.sh`, `make.sh` | Umgebung (m68k64-clang mit Sysroot, rustc mit der Zielbeschreibung über `RUST_TARGET_PATH`) und Aufruf |

Rust: Die Standardbibliothek für `m68k64-unknown-aros` liegt fertig im Sysroot des Stufe-1-rustc
(`lib/rustlib/m68k64-unknown-aros/lib`, gebaut mit `cargo -Zbuild-std` über den Zielnamen), Mozillas Build ruft also
nur `rustc --target m68k64-unknown-aros` mit `-Zunstable-options` auf.

## Stand (24.11.2026)

Konfiguration: fertig. Der Patch bringt mozbuild das Ziel bei:

- CPU `m68k64` (64 Bit, Big Endian) und Betriebssystem/Kernel `AROS` (`init.configure`, `constants.py`, `config.sub`);
- Linker: auf AROS über `collect-aros` (der Treiber-Standard), nicht mit `-fuse-ld=lld`, sonst fehlen die Symbolmengen
  und `__eh_frame_start`;
- Rust-Ziel `m68k64-unknown-aros` ohne Abgleich mit `rustc --print target-list`;
- der Cargo-Workspace auf die SpiderMonkey-Mitglieder beschnitten, wie in Mozillas Paketierungsskript;
- `QRX_NO_TESTS=1` lässt den mozbuild-Emitter Test-Manifeste überspringen.

Bau: läuft an, die ersten Übersetzungseinheiten scheitern. Offen, in dieser Reihenfolge:

1. **libc++ mit Lokalisierung und Wide Characters.** mfbt/fmt brauchen `std::ostringstream`, `std::locale`, `std::wstring`.
   Unsere libc++ ist ohne beides gebaut. Nötig sind eine Locale-Anbindung für AROS in libc++ (`__locale_dir/support/aros.h`,
   nur die C-Locale, nach dem Muster von Fuchsia) und AROS-Header, die `wctype_t`/`wctrans_t` definieren und die
   Wide-Char-Funktionen deklarieren. Die fehlenden Funktionen selbst sind fertig: `compat/qrxwchar.c`
   (`btowc`, `wctob`, `isw*`, `wctype`, `wcsto*`, `wcscoll`, `wcsxfrm`, `wcsftime`, `swprintf` mit `%ls`, `getwc`, `ungetwc`),
   im Gast getestet.
2. `double-conversion`: m68k64 in die Liste der Architekturen.
3. `mfbt/Assertions.h`: `pid_t` ist auf AROS 64 `long`, die Ausgabe mit `%d` scheitert an `-Werror`.
4. `sys/mman.h`: AROS hat kein `mmap`. Der GC (js/src/gc/Memory.cpp) braucht ausgerichteten Speicher und `munmap`.
5. Danach mozglue, js/src (Threads, Zeit, Stack-Grenzen) für AROS.

Erledigt dafür im Compiler: `_BitInt(N)` für m68k64 eingeschaltet (libc++ `<bitset>`, fmt), Rechnung im Gast geprüft
(128 und 65 Bit, Ergebnis wie Python).
