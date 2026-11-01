# ARMITHLON 1.9 – reale Integrationsschritte statt eines ungetesteten OS-Boot-Versprechens

**Basis:** Version 1.8, GitHub-Projekt `phoenixkonsole/qrxosone`, ausgewerteter
Entwicklungsstand `claude/qrx-22.5.11` SHA `cc2ba06a0cc76a87bdd49edcd4b475df4227e716`.
**Kein GitHub-Push:** Die Änderungen liegen als kumulatives lokales Archiv vor.

## CPU / Musashi

- Optionaler 68040-Backend-Aufruf `qrx_full_run_subroutine()`: Ein echtes QEX1-
  Gastprogramm kann im heruntergeladenen Musashi-Kern über einen begrenzten
  Supervisor-Stack-Rücksprung laufen. Firmware-PC/-SP werden wiederhergestellt.
- `qrx68100_boot_shell_full` verwendet diesen Aufruf jetzt für `run` und `bench`,
  statt QEX1 unter Musashi pauschal abzulehnen.
- Neue **optionale** End-to-End-Testpflicht mit *echtem* Musashi für HELLO.QEX,
  BENCH.QEX, Neustart und zwei Musashi-Kontexte; wird nur registriert, wenn
  Musashi wirklich installiert ist. **Hier nicht ausgeführt.**
- Musashi ist nicht im Archiv vendored; gepinnter Upstream-Source-Checkout bleibt
  bewusst separat. Vollständige 68040-FPU/MMU-Konformität ist weiterhin NICHT belegt.

## Dual-Core

- `./armithlon boot-full --dual` aktiviert **optional** das erweiterte
  QRX68100-Profil. Kern 1 startet vom eigenen 68k-Programmzähler und Stack,
  läuft als echter 68040-Maschinencode-Kontext im zeitscheibengeplanten
  Musashi-Dual-Kern und speichert sein Ergebnis im gemeinsamen Gastspeicher.
- Auslesen des Worker-Zählers beim Beenden. Klassischer Boot bleibt **Kern 0
  allein**; keine AmigaOS-3.x-Multiprozessor-Behauptung.
- **Nicht** zwei gleichzeitig laufende vollwertige Musashi-Threads; QRXNative
  Threads/JIT-Loop-Test aus älteren Versionen bleibt separat.

## Native virtuelle Board-Peripherie

- Echte neue `QRXB`-Blockgeräte-Schnittstelle: 512-Byte-Sektoren, DMA-Zugriff
  auf Gast-RAM, Statusregister, Dateidatenträger auf Host, Schutz vor
  Größenabweichung, readonly und versehentlichem Überschreiben bestehender Images.
- DMA-Schreibzugriffe melden dem JIT/Cache die geänderten RAM-Seiten. Der
  Controller ist auch unter `0xFF000700` für 68040-Gäste sichtbar.
- `./armithlon boot --raw-disk DATEI --raw-sectors 16 --create-raw-disk`
  montiert einen separaten leeren Sektor-Datenträger. QRXFS-1 bleibt erhalten;
  **kein AmigaDOS RDB/FFS und kein fertiger AROS-Storage-Treiber**.
- Neues Header für spätere AROS-68k-BSP-Treiber:
  `engine/guest/qrxvirt_block.h`.

## Desktop / Windows / Linux

- **Bestehenden QRXDesktop beibehalten.** Der aktuelle GitHub-Source
  `qrx_desktop.canonical-6.6.20.c` besitzt bereits Dateimanager, Icons,
  Doppelklick-Logik sowie PE32-/ELF32-Erkennung. Es gibt keinen zweiten,
  parallelen Desktop-Nachbau.
- Guarded idempotenter Patch schaltet vorhandene PE32-/Linux-ELF32-
  Doppelklick-Routen auf **asynchrones** `launch_native()` um und weist
  nicht sicher zitierbare Dateinamen / zu lange Kommandos ab. Pinned SHA
  wird vor Patch überprüft; keine Upstream-Quellen werden überschrieben.
- `qrx-app` erkennt Windows-PE32/i386, Linux-ELF32/i386, m68k-ELF,
  Amiga-HUNK und QEX1 anhand von Binär-Headern, nicht nur Endungen. Nur
  unterstützte Win32/ELF32-Gastdateien werden mit `execv` (ohne Shell)
  an konfigurierte QRX86-Hostlaufzeiten übergeben. Andere Formate bekommen
  eine klare Ablehnung. Linux erhält `DISPLAY=:0`.
- Dies ist die **Host-/AROS-Launcher-Ebene**, noch kein vollständiger
  Gastbetriebssystemdienst, der alle Windows- oder Linux-Programme ausführen kann.

## Validation

- Portable Linux-x86-64-Build: **29/29 CTests PASS**, darunter echte 68k-
  Firmware, Block-DMA, Format-Erkennung und guarded QRXDesktop-Patch-Fixture.
- AddressSanitizer/UBSan: **29/29 CTests PASS** mit dieser Portablen Konfiguration.
- Gast-Firmware mit zusätzlich montierter Rohdisk und HELLO.QEX: tatsächlich
  ausgeführt; Rohdisk bleibt auch nach Kaltstart erhalten.
- **Nicht durchgeführt:** echter Musashi-68040-Build, FPU-Differentialtest,
  macOS/M1-Metal, echter AROS-Boot auf QRX68100, QRXDesktop-AROS-Gaststart,
  Windows/Linux-Doppelklick gegen vollwertiges OS. Verfügbare Tools in dieser
  Umgebung enthalten keine echten Musashi-/AROS-SDK-/FS-UAE-Laufzeiten.

Der Statusbericht `./armithlon aros-native-status` meldet deshalb bewusst
`native_aros_on_qrx68100: BLOCKED`; er ist kein Boot-Test.
