# Tor Browser auf QRX OS One: Machbarkeit

Stand 2026-11-06. Getestet auf dem Mac M1 mit ARMithlon.

## Ergebnis

- **Der tor-Daemon läuft als natives 68k-AROS-Programm.** tor 0.4.9.9 baut mit OpenSSL 3.5.4 und libevent 2.1.12 und baut im Gast echte 3-Hop-Circuits auf, bisher nur in einem privaten Testnetz auf dem Mac.
- **Das echte Tor-Netz ist getestet** (2026-11-10, siehe unten): Bootstrap, Circuits, Exit und Onion-Dienst funktionieren. Über die Anonymität im echten Einsatz sagt das nichts aus.
- **Tor Browser selbst läuft nicht.** Ein nativer Gecko-Port auf 68k ist ein Vorhaben von Personenjahren, und ich rate davon ab.
- **Entschieden:** Der Nutzer hat Pfad E mit einem 64-Bit-68100EC64 gewählt, siehe den Abschnitt zur Entscheidung unten.
- Der WEB-Eintrag im App-Menü ist unverändert.

## Was fertig ist

Der Port liegt in `tools/armithlon/tor`. Das Binary liegt in `.build/tor/tor` (8,8 MB, davon 5,5 MB Code).

| Baustein | Lösung |
| --- | --- |
| Netz | Sockets des Macs über Door-Dienst 4, ohne AROSTCP und ohne Netzwerkkarte. Nur IPv4, höchstens 64 Sockets |
| DNS | Die POSIX-Schicht löst nur Zahlenadressen und `localhost` auf, sie schickt also selbst keine DNS-Anfrage am Tor vorbei |
| Zufall | Vom Mac über die Door. Fehlt er, startet tor nicht, es gibt keinen schwachen Ersatz |
| Uhr | Vom Mac über die Door. AROS startet hier ohne Batterieuhr im Jahr 1978, und tor würde damit jeden Konsens ablehnen |
| Speicher | Eigener `malloc` auf einem Exec-Pool mit Semaphor |
| Kern 1 | Optional X25519 auf Kern 1, standardmäßig aus. tor braucht ihn nie |

Der Emulator hat dafür kleine Ergänzungen bekommen: Dienst 4 ohne x86-Seitentabelle, Feature-Register, Zufall, Uhr. Dazu kommen neue Prüfungen in `hostnet_tests`.

### Fehler, die der Port im SDK gefunden hat

Sie betreffen auch künftige große Ports wie Blender oder einen Browser:

| Fehler | Folge | Wie belegt |
| --- | --- | --- |
| Zeiger kommen aus den SDK-Bibliotheken in d0, m68k-elf-gcc liest a0. Die SDK-Liste der Brücken ist unvollständig | u. a. `errno`, `localtime_r`, `getpwuid` falsch | Ausgehend vom Build; `errno` im Test |
| Doubles kommen in d0/d1, gcc liest fp0 | `pow`, `log`, `floor`, `strtod` liefern Unsinn | `pow` ergab 3,33 statt 1024 |
| Test-and-Set in `libpthread` gibt den neuen statt des alten Werts zurück | `pthread_once` und Spinlocks hängen beim ersten Aufruf | Disassembly |
| `SSIZE_MAX` ist 32767 | tor bricht bei Allokationen ab 32 KB ab | Assertion im Gast |
| `sigaction` antwortet ENOSYS | Signale müssen über `signal` laufen | tor-Warnungen |
| `strtoull` meldet keinen Überlauf | Riesige Zahlen wurden als gültig gelesen | tors `buffer/find_contentlen` |
| `environ` hängt beim Programmstart in `strlen` | tor kommt nicht bis `main` | Stack im RAM-Abzug |
| `libcrt`-Heap unter Threads | „Memory header not located“ in `realloc`. Mit eigenem Allokator verschwunden, die genaue Ursache ist nicht bewiesen | Vorher und nachher reproduziert |
| `usleep` | Hauptschleife hing in `select`. Mit eigenem Timer weg, Ursache nicht geklärt | Stack im RAM-Abzug |

## Was geprüft wurde

Alle Tests liefen im Gast, die Gegenstellen auf dem Loopback des Macs. Das private Netz besteht aus 3 Authorities und 2 Relays, nativ aus denselben Quellen gebaut.

| Prüfung | Ergebnis |
| --- | --- |
| `--version`, `--verify-config`, Control-Port vom Mac aus | In Ordnung |
| Bootstrap im privaten Netz | 100 %, etwa 3 s nach dem Start des Prozesses |
| Datei über 3-Hop-Circuit per SOCKS | SHA-256 bzw. `cmp` identisch |
| 20 MB Durchsatz | 3,5 bis 3,8 MB/s. Einzelne frühere Läufe, teils mit Diagnose, lagen bei 1,2 bis 1,8 MB/s |
| Dauertest 20 Minuten | 143 von 143 kleinen und 7 von 7 großen Abrufen identisch, rund 200 MB gelesen |
| Sauberes Beenden | `rc=0` nach `SIGNAL SHUTDOWN` |
| Datenverzeichnis auf HostFS | Guards im `state`, Neustart mit Cache |
| Verbindungen des Emulators zu anderen Zielen als Loopback | Keine in den `lsof`-Momentaufnahmen. Kein lückenloser Mitschnitt |
| Ohne Entropie vom Host | „Cannot get strong entropy“, Abbruch bei der Krypto-Initialisierung |
| Sauberer Neubau aus den Archiven | Gebaut, das Binary lief den End-to-End-Test |
| Engine-`ctest` | 60 von 60 bestanden. `qrx-win32` mit dem geänderten Emulator nicht erneut geprüft |

**tors eigene Testsuite im Gast:** 185 Tests bestanden, 0 fehlgeschlagen, darunter AES-CTR-Vektoren, SHA, SHA-3, RSA, HKDF, Curve25519, siphash, ntor und SOCKS. Das Bild ist aber unvollständig:

- `crypto/rng/fast`, `crypto/rng/fast_whitebox`, `tortls/address` und `relaycell/streamwrap` wurden ausgeschlossen. Mindestens drei davon hingen, die Ursache habe ich nicht untersucht.
- Der Lauf blieb danach bei `tortls/openssl/log_one_error` hängen. Der Rest von `tortls` und `relaycell` lief nicht.
- Übersprungen wurden die Tests für zstd, lzma und zwei RNG-Pfade, die hier nicht zutreffen.
- `relay`, `dirauth` und `pow` sind nicht eingebaut, ihre Tests entfallen.

### Test im echten Tor-Netz (2026-11-10, vom Nutzer freigegeben)

tor im Gast mit Standardeinstellungen, ohne Testnetz-Optionen, Datenverzeichnis auf HostFS. Der SOCKS-Port wurde vom Mac aus benutzt.

| Prüfung | Ergebnis |
| --- | --- |
| Bootstrap | 100 % nach etwa 35 s beim ersten Start |
| Circuits | 3-Hop-Circuits über öffentliche Relays gebaut, darunter Conflux-Circuits |
| `check.torproject.org/api/ip` über den Gast | `IsTor: true`, eine Exit-Adresse. Ohne Tor vom Mac: `IsTor: false` |
| Onion-Dienst (die Adresse des Tor-Projekts) | HTTP 200 nach 6 s |
| 5 MB über Tor | Vollständig, etwa 0,9 MB/s |
| Sauberes Beenden | `rc=0`. Die einzige Warnung war die absichtlich offene Control-Schnittstelle des Tests |

Damit sind der echte Konsens, die Verzeichnisserver, TLS mit echten Relays, Exits und Onion-Dienste geprüft. Über die Anonymität im Einsatz sagt das nichts. Die Grenzen unten gelten weiter: kein Speicherschutz, Schlüssel und Zufall vom Host, keine Bridges.

### Nicht geprüft

- Dauerbetrieb über 20 Minuten im echten Netz.
- Ob das Verhalten im Netz von einem normalen tor unterscheidbar ist. Das gilt auch für den TLS-Fingerabdruck dieser OpenSSL-Konfiguration.
- Ctrl-C, gleichzeitig laufende x86-Programme, ein Start aus der normalen Desktop-Umgebung.

### Bekannte Grenzen

Wer sich auf Anonymität verlässt, muss diese Punkte kennen:

- **Kein Speicherschutz:** AROS trennt Programme nicht. Jedes Gastprogramm kann tors Schlüssel lesen.
- **Host ist vertrauenswürdig:** Zufall, Uhr und Sockets kommen vom Mac. Schlüssel und Guards liegen über HostFS unverschlüsselt im Mac-Dateisystem.
- **SOCKS-Port offen für den Mac:** Der Port liegt auf dem Loopback des Macs, jedes Mac-Programm kann ihn benutzen.
- **Keine Bridges:** Ohne `fork` gibt es keine Pluggable Transports. In zensierten Netzen ist das ein echtes Hindernis.
- **Kein automatisches Update:** Sicherheitsupdates von tor müssen von Hand gebaut werden.
- **32-Bit-`time_t`:** Funktioniert bis 2038.
- **Weitere Lücke:** Die Door prüft bei den Socket-Aufrufen die Pufferadressen nicht gegen die RAM-Größe. Ein Gastprogramm könnte so Host-Speicher ansprechen. Das bestand schon vorher und sollte behoben werden.
- **Alter Emulator:** Mit einem älteren Emulator stoppt das Binary die ganze Emulation mit einem Speicherfehler.

### Zweiter Kern

X25519 auf Kern 1 lieferte in 50 von 50 Fällen dasselbe Ergebnis wie Kern 0, und auch im Betrieb lief es (123 Jobs, keiner abgelehnt). Es lohnt sich aber nicht:

| Messung | Kern 1 aus | Kern 1 an |
| --- | --- | --- |
| X25519 | 1,5 ms | 2,9 ms inklusive Start und Warten |
| 20 MB durch tor (je ein Lauf) | 3,8 MB/s | 1,4 MB/s |
| Kleine Abrufe (je ein Lauf) | 8 von 8 | 7 von 8 |

Der Grund ist nicht geklärt. Die Dokumentation nennt geordnete Speicherzugriffe auf Kern 0, sobald Kern 1 einmal lief, als mögliche Kosten. Deshalb bleibt die Hilfe aus (`QRX_TOR_CORE1`). Ein tor-Client verbringt hier wenig Zeit mit X25519.

## Tor Browser: was jede Schicht bräuchte

Tor Browser 16.0 basiert auf Firefox ESR 153 ([Download-Verzeichnis](https://dist.torproject.org/torbrowser/16.0/), [Bericht zur Version](https://www.it-connect.tech/tor-browser-16-0-upgrades-to-firefox-esr-153-adds-linux-arm64-support-and-can-hide-window-titles/)). Das Verzeichnis enthält Linux nur noch als x86-64 und aarch64. 32 Bit gibt es noch für Windows (`i686`) und Android (`armv7`). 32-Bit-Gecko existiert also, aber nur auf Architekturen mit JIT und gepflegtem Rust.

Aufwände sind Schätzungen, nicht gemessen.

| Schicht | Bedarf | Stand im Repo | Blocker |
| --- | --- | --- | --- |
| tor | C, OpenSSL, libevent | Läuft | Nein, Test im echten Netz fehlt |
| C++ | libstdc++, Exceptions, Threads | `m68k-elf-g++` ist da, aber kein libstdc++ im SDK | Groß, aber lösbar |
| Rust | Pflicht für Gecko (Stylo, WebRender) | m68k ist [Tier 3](https://rust-lang.github.io/rustup-components-history/index.html), nicht vorinstalliert, kein AROS-Ziel bekannt | Sehr groß: eigenes `std`, LLVM-m68k |
| Ausrichtung | Gecko und Rust nehmen meist natürliche Ausrichtung an | `alignof(long long)` ist hier 2 | Unbekannt viele Stellen |
| Big Endian | Mozilla testet das nicht offiziell (mein Kenntnisstand) | m68k ist Big Endian | Große Fehlerquelle |
| SpiderMonkey | Ohne m68k-JIT nur Interpreter, nach meinem Kenntnisstand ohne WebAssembly | | Viele Seiten langsam oder defekt, trotz schnellem Emulator |
| Prozesse | Content-Prozesse, Sandbox, geteilter Speicher | Kein `fork`, kein MMU-Schutz | Sehr groß, Fission-Sicherheit entfällt |
| Grafik | WebRender, Software-Renderer als Ausweg | RTG und Metal-Door vorhanden | Mittel |
| Speicher | libxul ist groß, ein 32-Bit-Link scheitert [oft am Adressraum des Linkers](https://bugzilla.redhat.com/show_bug.cgi?id=1658940) | Bauen ginge auf dem Mac. Laufzeit teilt sich höchstens rund 4 GB mit dem ganzen System | Knapp, aber wohl nicht das Hauptproblem |
| Pflege | Sicherheitsupdates etwa alle vier Wochen | | Ein veralteter Port wäre selbst ein Sicherheitsrisiko |

## Pfade

| | Pfad | Was man bekommt | Aufwand | Ist es Tor Browser? |
| --- | --- | --- | --- | --- |
| A | Nur tor als Dienst | Ein SOCKS-Proxy im Gast | Tage für die Einbindung | Nein |
| B | Kleine Engine hinter echtem tor (z. B. NetSurf, oder das vorhandene QRXWeb als erster Schritt) | Nativer 68k-Browser mit wenig JavaScript | Wochen, Port nicht geprüft | **Nein.** Kein Fingerprint-Schutz, keine Isolation zwischen Seiten. Die Nutzer wären leicht erkennbar. Müsste auch so benannt sein |
| C | Echter macOS-Tor-Browser, aus dem QRX-Menü per Door gestartet | Der echte Browser sofort, im eigenen Mac-Fenster | Tage | **Ja**, aber kein 68k-Programm. Der Gast startet ihn nur |
| D | Windows-`i686`-Tor-Browser unter QRX86 und `qrx-win32` (Hinweis des Nutzers) | Die originale Binärdatei im Gast | Viele Monate, Ausgang offen | Binärdatei ja. Ob der Schutz unter Emulation erhalten bleibt, ist unbekannt. x86, kein 68k |
| E | Gecko nativ für 68100 | Das eigentlich gewünschte Ziel | Personenjahre | Nur nach Abgleich mit dem Tor-Projekt |

Zu D: Firefox setzt vermutlich Windows 10 voraus und nutzt Sandbox, viele Prozesse und moderne Grafik-APIs. `qrx-win32` kann heute Kindprozesse, aber `TerminateProcess` und prozessübergreifende Mutexe fehlen noch, und MFC-Programme laufen nicht. Ein günstiger erster Schritt wäre, das Archiv (Größe vorab prüfen) zu laden und nur die Importe von `firefox.exe` und `xul.dll` gegen `qrx-win32` zu zählen, ohne etwas auszuführen. Das dauert etwa einen Tag und zeigt die Größenordnung.

### 64 Bit und SMP

- **64-Bit-Befehle im 68100** beheben keinen der großen Blocker. Mehr als 4 GB brauchen 64-Bit-Zeiger, also neue ABI, neuen Compiler und neu übersetztes System. Rust-`std`, libstdc++ und Prozessmodell blieben offen.
- **AROS x86-64 übernehmen** bringt für 68k-Programme wenig, denn es ist x86-64-Code und QRX86 kann nur 32 Bit.
- **SMP-Scheduler** ist als eigenes Projekt sinnvoll, unabhängig vom Browser. Die AROS-Quellen liegen nicht im Repo, das ist also nicht am Code geprüft. Kern 1 bräuchte Interrupts sowie Zugriff auf ROM und Chipset, und `Forbid()`/`Disable()` müssten über Kerne hinweg gelten. Zuerst wäre zu klären, warum tor mit laufendem Kern 1 langsamer wurde.

## Empfehlung

1. **Jetzt C:** Damit gibt es sofort den echten Tor Browser, klar als Mac-Fenster benannt.
2. **tor als Dienst einbauen (A)**, nach einem freigegebenen Test im echten Netz.
3. **Den Import-Vergleich für D** machen und danach entscheiden. D ist der einzige Weg, auf dem das Original im Gast laufen könnte.
4. **B nur, wenn ein nativer 68k-Browser ein eigenes Ziel ist**, und nie unter dem Namen Tor Browser.
5. **E nicht beginnen.**

## Entscheidung des Nutzers (2026-11-06): 68100EC64 und Gecko nativ

Gewählt ist Pfad E auf einer 64-Bit-Erweiterung mit SMP, also 68100EC64 und ein 64-Bit-AROS, gegen die Empfehlung oben. Der Befehlssatz steht in [68100ec64.md](armithlon/68100ec64.md).

Was daraus folgt:

- **Neues System neben dem alten.** AROS x86-64 ist nach meinem Kenntnisstand 64-Bit-sauber und hat SMP-Code, das ist die Grundlage. Heutige 68k-Programme laufen darauf nicht ohne eigene Brücke, QRX OS One 32 Bit bleibt also bestehen.
- **Mehr als 4 GB sind noch kein Gewinn.** Der Gast bekommt heute ein Viertel des Host-RAM, auf diesem Mac also rund 4 GB. Für mehr müsste diese Regel fallen.
- **Sicherheit:** AROS trennt Programme weiterhin nicht. Ein Gecko ohne Prozess-Isolation ist kein gleichwertiger Tor Browser, und eine Abstimmung mit dem Tor-Projekt ist nötig, bevor dieser Name benutzt wird.

### Fahrplan

Die Aufwände sind grobe Schätzungen für einen Entwickler in Vollzeit, ohne Erfahrungswerte aus diesem Projekt.

| Schritt | Inhalt | Aufwand |
| --- | --- | --- |
| 1 | Referenzkern, Long Mode, zwei Kerne | **Erster Stand fertig** |
| 2 | Bitfelder, Exceptions, Supervisor, Interrupts, Signale zwischen Kernen | **Erster Stand fertig**, FPU und Einbau in den Emulator offen |
| 3 | LLVM-Ziel `m68k64` auf dem vorhandenen M68k-Backend, dazu `clang`, `llvm-mc`, `lld` | **Erster Stand fertig**, Rest Monate |
| 4 | JIT für Long Mode auf AArch64 | Monate |
| 5 | AROS-Port `m68k64` mit SMP-Scheduler | Viele Monate |
| 6 | C-Bibliothek, C++ (libc++), Rust-`std` für AROS | Monate |
| 7 | SpiderMonkey ohne JIT, dann Gecko in einem Prozess, zuerst nur Software-Grafik | 1 Jahr oder mehr |
| 8 | Tor-Browser-Patches, Anschluss an den tor-Daemon, laufende ESR-Updates | Dauerhaft |

### Erster Stand

| Prüfung | Ergebnis |
| --- | --- |
| 32-Bit-Modus gegen Musashi, Zufallsbefehle einzeln | 296.708 verglichen, 0 Abweichungen |
| Long-Mode-Regeln: Größen, Flags, Register D8 bis A15, Stack, Atomics, Modus-Wechsel | Bestanden |
| FNV-1a über 1 MiB bei 5 GiB, Code und Stack über 4 GB, assembliert mit `m68k-elf-as` | Gleich wie die Berechnung in C auf dem Mac |
| Zwei Threads, `CAS.Q` über 4 GB | 2.000.000 von 2.000.000. Ohne CAS gehen wie erwartet Zählungen verloren (rund 1,1 bis 1,3 Mio.) |
| Engine-`ctest` | 62 von 62 |

Grenzen dieses Stands:

- Der Vergleich prüft nur Befehle, die der neue Kern hat, und jeweils einen Befehl auf einmal. Etwa ein Viertel der Zufallsbefehle meldet „nicht vorhanden“, darunter echte, aber noch fehlende Befehle.
- Für den 64-Bit-Modus gibt es keine Referenz. Die Tests prüfen ihn nur gegen die Regeln in der Spezifikation.
- Der Kern läuft bisher nur in den Tests, nicht im Emulator und nicht unter AROS. 41 Millionen Befehle pro Sekunde sind ohne JIT deutlich zu wenig.
- `rustc` listet ein M68k-Ziel bis M68040 auf. Dass es brauchbaren Code erzeugt, ist hier nicht geprüft.

### Zweiter Stand: LLVM und clang

`clang` übersetzt C für `m68k64`, Quelle und Build stehen in `tools/armithlon/ec64-llvm`. Das ABI ist LP64 mit natürlicher Ausrichtung, das Problem `alignof(long long) = 2` entfällt damit.

| Prüfung | Ergebnis |
| --- | --- |
| Handgeschriebene IR, 16 Aufrufe, `-O0` bis `-O3`, Maschinencode-Verifier an | 16 von 16 auf jeder Stufe, gleich wie die Rechnung auf dem Mac |
| C-Testprogramm, dieselbe Quelle auf dem Mac und im Referenzkern, `-O0` bis `-O3` und `-Os`, Verifier an | 10 von 10 auf jeder Stufe: Globals, Zeiger in Daten, Funktionszeiger, Sprungtabellen, Strukturen, Varargs, alle Breiten, SHA-256 über 8 KB, Quicksort, verkettete Liste |
| Code, Daten und Stack | Alles über 4 GB geladen |

Gefunden auf dem Weg:

- **`MOVE` setzt Flags:** Der Register-Allokator kann eine Kopie zwischen Vergleich und Sprung setzen. Der Befehlssatz hat dafür jetzt das N-Präfix.
- **Fehler aus dem Original-Backend:** Ein Zugriff über Frame-Index plus Register verlor den Index. `DIVS.L` war so kodiert, dass der Rest D0 überschreibt. Beide stehen auch im unveränderten M68k-Backend, dort habe ich sie nicht geprüft.
- **PC-relatives Schreiben:** Das Backend erzeugt es, ein echter 68k kann es nicht. Der Long Mode erlaubt es jetzt.

Grenzen dieses Stands:

- **Kleine Tests:** Das sind von mir geschriebene Tests in einer Datei, kein großes Programm und keine Compiler-Testsuite. Weitere Fehler sind zu erwarten.
- **Kein Linker:** Ein kleiner Lader im Testprogramm setzt die Relokationen. Die Relokation für 64-Bit-Zeiger hat eine eigene Nummer (240).
- **Nicht gelaufen:** Gleitkomma (dem Referenzkern fehlt die FPU), Atomics im erzeugten Code, TLS, C++, Register D8 bis A15 im Allokator.
- **Kein System:** Weder C-Bibliothek noch AROS laufen auf dem neuen Modus.

### Dritter Stand: Kern, Assembler, Linker

| Teil | Stand |
| --- | --- |
| Kern | Supervisor- und User-Modus, Exceptions über eine Vektortabelle mit 64-Bit-Frames, sieben Interrupt-Ebenen, ein Timer je Kern, Signale zwischen Kernen, Start weiterer Kerne. Dazu `MOVE SR`, `MOVE USP`, `RTE`, `STOP`, `RTD`, `RTR`, `TRAPV`, `TRAPcc`, `CHK`, `CHK2`, `CMP2`, `CAS2`, `MOVEP`, BCD, `PACK`, `UNPK` und Bitfelder |
| Maschine | Mehrere Kerne auf eigenen Host-Threads, wartende Kerne schlafen mit `STOP` |
| Assembler | `llvm-mc` übersetzt m68k64-Assembler. Privilegierte Befehle kennt er noch nicht, die stehen als `.short` im Quelltext |
| Linker | `ld.lld` linkt statisch für m68k64: ELF64, Big Endian, mit Linker-Skripten |
| Compiler | 64-Bit-Atomics (`cas.q`) |

| Prüfung | Ergebnis |
| --- | --- |
| 32-Bit-Modus gegen Musashi, mit den neuen Befehlen | 306.263 verglichen, 0 Abweichungen |
| Exceptions, Frames, User-Modus, Interrupts, `STOP`, Double Fault, zwei Kerne mit Timer und Signal | Bestanden |
| Test-Kernel, mit `llvm-mc`, `clang` und `ld.lld` gebaut, auf zwei Kernen, `-O0` bis `-O3` und `-Os` | Auf jeder Stufe alle Prüfungen bestanden |
| Davon im Einzelnen | 8 Tasks im User-Modus mit eigenen Stacks, vom Timer unterbrochen, verteilt über eine gemeinsame Warteschlange unter Spinlock. 4 Tasks liefen auf beiden Kernen und rechneten dasselbe wie der Mac. 80.000 atomare Additionen von zwei Kernen kamen vollständig an. 3 absichtliche Fehler (privilegierter Befehl, Lesen außerhalb des RAM, Division durch null) beendeten nur ihre Task. Ausgabe per Syscall aus dem User-Modus |

Gefunden auf dem Weg:

- **Zeiger in A0:** Die Konvention gibt Zeiger in A0 zurück. Mein eigener Assembler-Rahmen las D0, das hat der Test-Kernel gezeigt.
- **`cas.q` ohne W-Präfix:** Weil der Befehlsname mit Leerzeichen statt Tab notiert war, fehlte das Präfix. Jede 64-Bit-Atomic traf nur das obere Wort.
- **Auslagern von Registern:** 64-Bit-Register fehlten bei der Auswahl, Byte-Register liefen über eine Wort-Anweisung mit falscher Registerklasse. Bei `-O2` fiel beides nur zufällig nicht auf.

Grenzen bleiben:

- Kein MMU, also keine Speichertrennung.
- Keine FPU im neuen Kern.
- Speicher-indirekte Adressierung fehlt.
- Kein JIT, das sind weiterhin 41 bis 77 Millionen Befehle pro Sekunde.
- Der Kern läuft noch nicht im Emulator.
- Weder AROS noch eine C-Bibliothek laufen auf dem neuen Modus.
- Der Test-Kernel ist mein eigener kleiner Test, kein Betriebssystem.

### Vierter Stand: C++-Laufzeit (24. November)

Die C++-Laufzeit für QRX OS One 64 steht. Beide Prüfprogramme für den Gecko-Port bestehen auf zwei EC64-Kernen:
`GeckoPThread` (pthread-Schlüssel und ihre Destruktoren) und `GeckoRuntime` (C++-Ausnahmen über Rahmen hinweg, Threads,
`thread_local` mit Destruktoren).

| Teil | Stand |
| --- | --- |
| Compiler | Die Aufrufrahmen-Information (CFI) des m68k64-Backends war nicht abwickelbar: falsche CFA, kein Rücksprungregister, falsche Vorzeichen, gesicherte Register fehlten. Jetzt richtig, geprüft am Assembler mit und ohne Frame-Pointer. EH-Register d0/d1 für clang, emulierter TLS für AROS |
| libunwind | Neues Ziel m68k64, Bare-Metal-Suche über die Symbole, die der AROS-Linker anlegt |
| libc++abi, libc++ | Statisch, mit AROS-pthreads. Noch ohne Dateisystem, Lokalisierung und Wide Characters |
| TLS | Emuliert über pthread-Schlüssel (`emutls`), Destruktoren beim Thread-Ende laufen |

Damit ist Schritt 6 des Fahrplans für C++ erledigt. Offen bleiben dort Rust-`std` für AROS und die
Lokalisierung von libc++. Ein großes C++-Programm ist noch nicht gelaufen; das wäre der nächste Prüfstein
(SpiderMonkey ohne JIT, Schritt 7).

### Bedarf für die nächsten Schritte

- Quellen: LLVM ist geladen. Für den AROS-Port fehlt `deadwood2/AROS` (rund 221 MB, GitHub).
- Plattenplatz: Es sind noch 42 GB frei, vor dem LLVM-Build waren es 57 GB. Andere Sitzungen liefen dabei mit, der Anteil des Builds ist nicht getrennt gemessen. Gecko braucht schätzungsweise deutlich mehr, das wird eng.

## Quellen

- [Tor Browser 16.0: Download-Verzeichnis](https://dist.torproject.org/torbrowser/16.0/)
- [it-connect: Tor Browser 16.0 auf Firefox ESR 153](https://www.it-connect.tech/tor-browser-16-0-upgrades-to-firefox-esr-153-adds-linux-arm64-support-and-can-hide-window-titles/)
- [Rust: Komponenten und Tier-3-Ziele](https://rust-lang.github.io/rustup-components-history/index.html)
- [Red Hat Bugzilla 1658940: Firefox-Link auf ARM ohne Speicher](https://bugzilla.redhat.com/show_bug.cgi?id=1658940)
