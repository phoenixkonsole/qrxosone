# ARMITHLON 1.3 – Benchmarks, Linux-X11-Eingaben und QRX-Bootsmoke

## Ziel

Der echte aktuelle GitHub-Stand von `phoenixkonsole/qrxosone` (private Repo,
Branch `claude/qrx-22.5.11`, SHA `cc2ba06...`) enthält die neue eingebettete
Linux-X11-Komponente und QRX86 22.5.12. ARMITHLON 1.3 baut darauf auf und
bleibt ein **kumulatives Entwicklerpaket** mit 68k-Testengine + optionalem
Metal/Cocoa-Frontend. QRX86 selbst wird erst nach GitHub-Authentifizierung
lokal in eine isolierte Kopie eingebaut. Ohne Github-Zugang keine echten
Win32-/Linux-Guest-Emulationsbenchmarks.

## Installation auf dem M1

```sh
xcode-select --install
brew install cmake gh xorriso fs-uae mingw-w64
cd ~/Downloads/ARMITHLON-1.3-macOS
./armithlon build
./armithlon test
./armithlon x11-input-test
```

### QRX86 aus GitHub holen und bauen

```sh
gh auth login
./armithlon fetch
./armithlon build-qrx86-host
./armithlon build-qrx86-linux-host
./armithlon build-crossbench
```

Das GitHub-Pin ist absichtlich exakt. Bei einem neueren abweichenden Branch
muss der Code erneut auf Änderungen geprüft und gepinnt werden.

### CPU-Leistungsvergleich (PE32 gegen ELF32)

```sh
./armithlon benchmark-qrx86 3
open reports/latest/execution-dashboard.html
```

Die Benchmark-Routine kompiliert dasselbe quelloffene C-Testprogramm als
native Host-Version und (bei vorhandenem Crosscompiler) für Windows und
Linux i386. Das Linux-ELF32 liegt außerdem fertig gebaut bei. Es gibt nur
Werte für tatsächlich erfolgreich gemessene Läufe; Parser erwartet dieselbe
Checksum `903be502`. Der Host-Native-Wert kann bei nur 300.000 Iterationen
sehr kurz und damit verrauscht sein; für belastbare Vergleiche die Tests
wiederholen und CPU-Last, Temperatur sowie Host-Takt protokollieren.

### Ausgaben / Metriken

`reports/latest/execution-results.json` enthält Host-OS/ISA, gebaute
Binärdateien-Hashes, jede reale Laufzeit, die Gästeinstruktionen/s und den
Median. `execution-dashboard.html` zeigt die Metriken und (wenn vorher
`./armithlon benchmark` lief) auch QRXGPU/Metal/Vulkan getrennt.
Befehlsencoder-FPS sind **nicht** GPU-FPS. Geekbench 7, glmark2 und vkmark
bleiben aus 1.1 verfügbar, aber deren Scores sind nicht zu MIPS umzurechnen.

### Reale PrBoom-Tests

Auf GitHub liegen bereits Windows-PrBoom-Messungen und Source-/WAD-Archive.
Ein neues Spielverzeichnis braucht `prboom-plus.exe`, `prboom-plus.wad`,
`freedoom1.wad`, `tranmap.dat` (vollständig: 65.793 Bytes).

```sh
./armithlon bench-prboom --windows-game /PFAD/ZU/PRBOOM-DATEIEN --timeout 600
```

Linux-PrBoom braucht den i386 ELF32-Client und einen passenden i386-Rootfs mit
allen i386-Libraries, insbesondere ld-linux.so.2, libc, SDL1.2 und X11.
Die aktuelle embedded-X11-Teilmenge bietet **noch kein vollständiges GLX**
und möglicherweise nicht alle SDL-Anforderungen. Nur eine abgeschlossene
PrBoom-Timedemo wird als PASS erfasst:

```sh
./armithlon bench-prboom \
 --linux-elf /PFAD/prboom-plus \
 --linux-root /PFAD/i386-rootfs \
 --freedoom-wad /PFAD/freedoom1.wad --timeout 600
```

Windows-DirectDraw und Linux-SDL/X11 unterscheiden sich in den Grafikpfaden;
selbst erfolgreiche Messergebnisse benötigen zusätzlich einen Frame-/Bild-
vergleich, bevor wir ein klares Win-vs-Linux-Performance-Ranking erstellen.

### Vergleichs-CPUs

Slots: 486DX2, Pentium, 68060, 68080, Pentium 4, Core i7, Apple M1. Kein
Slot erhält erfundene Hardwareleistung. Um originale gemessene Zeiten aus
einem QRXBENCH-Lauf auf physischer Hardware hinzuzufügen:

```sh
python3 tools/add-hardware-reference.py --machine 'Pentium' \
 --times '1.23,1.24,1.22' --output-log /PFAD/ZUR/ORIGINALAUSGABE.log \
 --source 'Pentium 100 MHz, Windows 95, native PE32, 3 echte Läufe'
./armithlon benchmark-qrx86 3
```

Die Zeiten oben sind **nur Syntaxbeispiele**, keine angeblich gemessenen
Pentium-Zeiten. Keine realen Zahlen ohne das Original-Log und Benchmark-Checksum.

### QRX-Start / Boot

Es fehlen weiterhin die aus früheren QRX-Releases benötigten Original-
Payload-Build-Bestandteile für ein vollständiges QRX-OS-One-ISO.
`./armithlon boot-qrx-compat` baut und bootet daher **AROS Deadwood 68k
plus tatsächlich veröffentlichte QRX 22.5.12 m68k HUNK-Tools** unter FS-UAE.
Der Test verlangt die vom Gast erzeugten Dateien `BOOTED` und `DONE`.
Das ist keine native QRX68100-Bootfähigkeit und keine vollständige QRX-Desktop-
Distribution. Die Ausführung auf deinem M1 konnte hier nicht verifiziert werden.

```sh
./armithlon fetch
./armithlon boot-qrx-compat
```

Wenn du ein **tatsächlich vollständiges QRX OS One ISO** erstellt hast,
kannst du es wie bisher unter `data/images/QRX-OS-One.iso` ablegen und mit
`./armithlon run-qrx` per FS-UAE starten. Der native QRX68100-Kern benötigt
weiterhin vollständige 68k-CPU/Interrupt-/BSP-/RTG-/AHI-/Input-Implementierung.

### Linux-i386-Fenster jetzt mit Eingabe-Brücke

Zwei Terminals:

```sh
./armithlon run-native-qrx86
```

```sh
./armithlon run-linux-x11 benchmarks/linux-i386/qrx-x11-preview.elf
```

Die neue Patchstufe sendet Q86I-Input vom Mac an einen X11-Client (ein aktiver
Client, erstes gemapptes Fenster; Basis-ASCII, Mausposition und drei Tasten).
Multi-Window-Fokus, X11-Layouts/Modifier, GTK/Qt, GLX sind weiterhin offen.
Die vollständige Mac-Ende-zu-Ende-Ausführung ist noch nicht verifiziert.

## Prüfstand dieses Pakets

21/21 lokale CTests unter Linux x86_64 bestanden; native C-CrossBench-Checksum
korrekt. 32-Bit-ELF statisch gebaut; Win32/PrBoom/Metal/M1/FS-UAE auf dem M1
hier **nicht ausgeführt**. Vorhandene PrBoom-Messungen aus GitHub sind im
Referenzbericht gekennzeichnet, ausdrücklich keine neuen 1.3-Messungen.
