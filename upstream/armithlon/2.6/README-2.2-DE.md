# ARMITHLON 2.2 — QRX68: AmigaOS-3.x-Kompatibilitätsdienst (erste echte Ausführung)

## Ziel und Architektur

QRX soll AmigaOS-3.x-Anwendungen auch dann weiter nutzen können, wenn AROS68k
bestimmte APIs oder Binärdetails nicht unterstützt. Anders als ein einzelner
HUNK-Loader benötigt das eine **Prozesslaufzeit** mit 68k-CPU, Exec ABI,
AmigaDOS/Intuition/graphics.library, Systemstrukturen, Laufwerksnamen und
Workbench-Startnachrichten. Die Umsetzung ist analog zur Aufgabenteilung
CPU-Übersetzung + OS-ABI-Layer von Rosetta/Win32-HLE, nicht identisch mit Rosetta.

* Pfad A: Standard-68k-HUNK direkt als **AROS-Prozess**. Bestehendes QRXDesktop
  und echte AROS-Libraries bleiben der bevorzugte kompatible und schnellere Pfad.
* Pfad B: **QRX68 isoliert** startet AmigaOS-3.x-HUNK und fängt klassische
  6-Byte-LVO-JMP-Vektoren ab. Ein prüfbarer, kleiner Exec/DOS-HLE-Dienst ist
  im Host lauffähig. Dieser Pfad ersetzt *noch nicht* den AROS-Exec-Prozess,
  ist noch kein QRXDesktop-Gastdienst und verwendet noch den PORTABLEN,
  eingeschränkten 68k-Interpreter.
* Pfad C: für Programme mit direkten Custom-Chip-Registern, selbstgebautem
  Kickstart-Kontext, WHDLoad, privatem Workbench oder MMU-Anforderungen:
  echte AmigaOS3.x-Umgebung auf vollständig emulierter Amiga-Hardware.
  Der bestehende FS-UAE-Fallback bleibt daher nötig.

## Tatsächlich implementierte QRX68-Funktionen

- Strikter HUNK_HEADER CODE/DATA/BSS/RELOC32/RELOC32SHORT-Loader mit Prüfung
  vor der Änderung des Gast-Speichers. Viele HUNK-Varianten fehlen noch.
- Standard-SysBase-Zeiger bei 4.w und klassische 6-Byte-Vektortabelle
  (`4EF9 <trap>`), darunter `JSR -552(A6)` und `JSR -48(A6)`.
- Exec `OpenLibrary` (v40, exec.library/dos.library), `CloseLibrary`,
  `AllocMem`, `FreeMem`, `AvailMem`, `AllocVec`, `FreeVec`, `CopyMem`,
  `CopyMemQuick`, Single-Task-`Forbid`/`Permit`.
- DOS `Open`, `Close`, `Read`, `Write`, `Seek`, `Input`, `Output`, `IoErr`, `PutStr`.
  Die dokumentierten Registerkonventionen werden an der HLE-Grenze benutzt.
- `HOST:`-Freigabe für EIN macOS-/Linux-Verzeichnis, zunächst read-only,
  Schreibzugriff nur per `--share-rw`; keine Symlinks, keine Unterordner,
  keine Pfad-Traversierung, `openat(O_NOFOLLOW)`.
- Isolierter 2-MiB-Gastspeicher, striktes CPU-Step-Budget, keine ungefragten
  Hostprozesse, Diagnose von unerkannten Library-Vektoren statt falscher Erfolge.
- `qrx-app launch APP.HUNK` startet QRX68 nur, wenn `QRX_HOST_68K` explizit gesetzt
  ist. `./armithlon app-launch` stellt den gebauten lokalen Pfad automatisch ein.
- Batch-Kompatibilitäts-Scanner erzeugt maschinenlesbares JSON mit konkreten
  fehlenden LVOs, Faults und Step-Limits. Der Scanner startet nur ausdrücklich
  gewählte HUNK-Dateien mit Host-FS AUS und mit Timeout. PASS heißt nur,
  dass das Programm im kleinen Testsystem zu Ende lief.

### Boot/Demo (kein Fremd-ROM erforderlich)

```sh
./armithlon build
./armithlon qrx68-test
./armithlon qrx68-demo
./armithlon qrx68 /Pfad/zur/AmigaOS3-APP.HUNK
./armithlon qrx68 --share ~/QRX-Share /Pfad/zur/APP.HUNK
./armithlon qrx68-compat-scan ~/AmigaProgramme --out reports/compat-scan.json
./armithlon app-launch data/qrx68/QRX68-Hello.hunk
```

Die Demo ist ein wirkliches Motorola-68k-HUNK mit `MOVEA.L 4.w,A6`,
`OpenLibrary`, `Output`, `Write`, `RTS`; die Ausgabe kommt aus dem Guestcode,
nicht aus einer hostseitig fingierten Demo.

## Entscheidend für die maximal mögliche Kompatibilität (NICHT fertig)

1. **Vollständiger CPU-Interpreter/JIT unter QRX68:** Den vorhandenen Musashi-
   68040-/FPU-Kern mit dem QRX68-Library-Dispatcher verbinden; danach ein
   verifizierter ARM64-Codecache mit Invalidierung, Exception-Verhalten, MMU,
   Supervisor-/User-Modi und eigenständigen FPU-Registern je Prozess.
   Aktuell hat QRX68 noch den bewusst unvollständigen Foundation-Interpreter.
2. **AROS-seitiger Dienst:** QRX68 in AROS als *eigene Kompatibilitäts-
   Prozessklasse* starten und OS-Aufrufe zuerst an kompatible AROS-Libraries,
   sonst an einen bewusst implementierten HLE-Adapter weitergeben.
   Keinesfalls das globale `SysBase` eines laufenden AROS überschreiben.
3. **Libs:** intuition.library, graphics.library/RTG, layers, icon,
   workbench, asl, gadtools, datatypes, locale, timer, input, filesystem,
   `bsdsocket.library`, AHI, tooltypes, WBStartup, SegLists und DOS-Packets.
   Nur echte nachgewiesene Semantik melden. Aktuell NICHT implementiert.
4. **Chipset und Spezialfälle:** Custom-Chip-Hardware (Paula/Copper/Blitter),
   CHIP-RAM, RomTag, WHDLoad, private Hardwarezugriffe. Das benötigt bei Bedarf
   vollständige Amiga-Hardwareemulation, die HLE allein nicht ersetzen kann.
5. **QRXDesktop:** vorhandenen Dateimanager und Doppelklicklogik weiterverwenden.
   Automatische Backendwahl und der echte Host-/AROS-IPC-Dienst fehlen noch.

## Tests und Grenzen

`ctest`: 42/42 auf Linux x86_64. Der neue HUNK-Test führt tatsächliche 68k-
Instruktionen aus und prüft OpenLibrary/Output/Write, DOS Open/Read/Write,
AllocVec/FreeVec, Versionsgrenzen, strikten Fehlerfall bei unbekannten LVOs.
Das ist KEIN Beleg für vollständige AmigaOS-3.x-Kompatibilität. Ein M1-Messlauf
und die Vollintegration in Musashi sowie AROS/QRXDesktop stehen noch aus.

## Relevante ABI-Dokumentation

- https://developer.amigaos3.net/autodocs/exec.library/OpenLibrary.html
- https://developer.amigaos3.net/autodocs/exec.library/AllocVec.html
- https://developer.amigaos3.net/autodocs/dos.library/Write.html
- https://developer.amigaos3.net/autodocs/dos.library/PutStr.html
- https://arisamiga.github.io/amiga-bootcamp/14_references/exec_lvo_table/
- https://arisamiga.github.io/amiga-bootcamp/14_references/dos_lvo_table/
