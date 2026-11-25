# SpiderMonkey für QRX OS One 64 (m68k64-unknown-aros)

Schritt 7 des Fahrplans für Gecko/Tor Browser (`docs/tor-browser-machbarkeit-2026-11.md`): die JavaScript-Engine
von Firefox ESR 153.8.0 (Basis von Tor Browser 16), nur als Interpreter (`--disable-jit`), ohne Intl.

| Datei | Zweck |
| --- | --- |
| `fetch.sh WORKDIR` | lädt die Firefox-Quelle (805 MB, SHA-256 aus Mozillas `SHA256SUMS`), packt den Teil aus, den Mozillas eigenes SpiderMonkey-Paket nimmt, wendet den Patch an |
| `spidermonkey-153.8.0-aros.patch` | die AROS-Änderungen; `make-patch.py` schreibt ihn aus einem Arbeitsbaum (Originale als `*.qrxorig`, von `qorig.sh` vor der ersten Änderung angelegt) |
| `mozconfig` | `--target=m68k64-unknown-aros --disable-jit --without-intl-api --disable-tests --disable-jemalloc` |
| `tests/run-js.sh` | führt `tests/hallo.js` im Gast aus und vergleicht mit `hallo.expected` |
| `fix-checksums.py` | berechnet `.cargo-checksum.json` der gepatchten Crates neu (ruft `fetch.sh` auf) |
| `configure.sh`, `make.sh` | Umgebung (m68k64-clang mit Sysroot, rustc mit der Zielbeschreibung über `RUST_TARGET_PATH`) und Aufruf |

Rust: Die Standardbibliothek für `m68k64-unknown-aros` liegt fertig im Sysroot des Stufe-1-rustc
(`lib/rustlib/m68k64-unknown-aros/lib`, gebaut von `../rust/build-target-std.sh` mit `cargo -Zbuild-std` über den Zielnamen;
x.py löscht das Verzeichnis bei jedem Compiler-Neubau, das Skript also danach wieder laufen lassen), Mozillas Build ruft also
nur `rustc --target m68k64-unknown-aros` mit `-Zunstable-options` auf.

## Stand (25.11.2026)

**Die Engine läuft im Gast.** `js` (41 MB, statisch, ohne GOT/PLT-Relokationen) startet unter QRX OS One 64 auf dem
Referenzrechner (qrxboot, zwei EC64-Kerne) und führt `tests/hallo.js` aus: Arithmetik, Sortieren, JSON, Rekursion,
`Date`, `Math`, reguläre Ausdrücke, Pfeilfunktionen, BigInt. `tests/run-js.sh <js>` vergleicht mit `hallo.expected`:
**JS PASS** (rund 0,4 Mrd. Befehle bis zum Ende). Nur Interpreter, kein JIT, ohne Intl.

Was dafür nötig war, außer dem Patch (Konfiguration, Linker, Rust-Ziel, Workspace, siehe unten):

- **Compiler-Fehler im m68k64-Backend:** Mit `-fstack-protector-strong` (Gecko-Standard) ging die Kopie des
  Rückgabewerts ins zweite Register (a0/d0, LowerReturn) verloren: `findSplitPointForStackProtector` bricht an einer
  Kopie aus einem physischen Register ab. Folge: `refillFreeListAndAllocate` gab den Zeiger nur in d0 zurück, der
  Aufrufer las a0, der GC schrieb Strings über das Zone-Objekt. Behoben in `llvm-22.1.2-m68k64.patch`, Regressionstest
  `ec64-llvm/tests/ssp-return.sh`. Betrifft jeden Code mit Stack-Protector; Rust und AROS bauen ohne.
- **GC-Speicher:** AROS hat kein `mmap`; `js/src/gc/Memory.cpp` nimmt den WASI-Weg (`posix_memalign`). Die
  Adressraum-Suche `FindAddressLimit` (Zufallsadressen abtasten) lief auf AROS endlos, dort sind es fest 47 Bit.
- **libqrxaroscompat:** `sysconf(_SC_PAGESIZE)` (AROS liefert -1), `mbsrtowcs`/`mbstowcs`/`wcsrtombs`/`wcstombs`
  nach C-Standard (AROS beachtete `len` auch bei `dst == NULL`, die Längenabfrage ergab 0), dazu `mmap`-Ersatz,
  `tzset`, `fork` (ENOSYS), Stack-Protector-Wächter aus dem Host-Zufall.
- **Abstürze sichtbar:** `MOZ_CRASH` meldet auf AROS Datei und Zeile und endet über `abort()`; Mozillas
  `abort()`-Umleitung ist auf AROS aus (sie lief mit dem Absturzpfad im Kreis).
- **libc++ mit Lokalisierung und Wide Characters**, AROS-Header (`wctype_t`, `sys/mman.h`, undurchsichtige
  pthread-Typen), `double-conversion` für m68k64, `pid_t`-Ausgabe, kein `pthread_condattr_setclock`, `fork`-Zweige
  der Shell abgeschaltet, `libc`/`getrandom`-Crates mit AROS (`getentropy` über die Host-Tür).

Fehlersuche im Emulator (alle über Umgebungsvariablen, sonst wirkungslos): `QRX_EC64_PCSAMPLE=<datei>` (alle 20 ms
pc und Stack je Kern), `QRX_EC64_FAULTLOG=1` (Register und Speicher beim Busfehler; AROS' Guru-Anzeige schneidet
Register auf 32 Bit ab), `QRX_EC64_WATCH=<adresse>` / `QRX_EC64_WATCHPC=<pc>` (Schreibzugriffe, mit
`QRX_EC64_JIT=0`). Im Gast meldet ein Programm mit `Set QRX_TEXTBASE 1` die Ladeadresse seines Codes.

Nächste Schritte zu Gecko: NSPR und NSS für AROS, dann Layout/Grafik mit einem Intuition-Widget-Backend.

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
