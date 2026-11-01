# ARMITHLON 1.4: QRX68100 vollständigerer CPU-Unterbau (Musashi)

Ziel: Den bisherigen kleinen 68k-Interpreter durch eine technisch belastbare zweite CPU-Option ergänzen. ARMITHLON 1.4 integriert dafür einen etablierten Motorola-680x0-Kern, ohne den experimentellen QRX-JIT oder deine bestehenden QRX86-/Windows-/Linux-Benchmarks zu entfernen.

## Voraussetzungen auf Apple M1/M2/M3/M4

```bash
xcode-select --install
brew install cmake git
cd ~/Downloads/ARMITHLON-1.4-macOS
./armithlon full68k-setup
./armithlon full68k-build
./armithlon full68k-test
./armithlon full68k-run --cpu 68040
```

`full68k-setup` klont die öffentlich verfügbare, quelloffene Musashi-Bibliothek von GitHub und wechselt in einen festen Commit. Es ist keine Anmeldung an deinem privaten QRX-Repository nötig. Kein Code wird automatisch in deinem GitHub-Repository geändert.

**Warum wird Musashi separat heruntergeladen?** Das Quellpaket enthält nur den eigenen QRX-Virtual-Machine-Adapter und die kumulativ gelieferten ARMITHLON-Dateien. Die kompletten Musashi-C-Sourcen werden erst lokal durch `full68k-setup` bezogen. Ohne Download läuft das Paket weiterhin mit dem bisherigen, begrenzten Interpreter; `full68k-build` meldet fehlende Quellen offen.

Der neue CPU-Test überprüft: UART-Ausgabe durch echte 68k-Instruktionen, DBRA/MULU, RTE/TRAP, ROM-Schutz und IRQ des virtuellen Boards. Er meldet einen Fehler, wenn der Test nicht durchläuft. Eine erfolgreiche CPU-Demo bedeutet noch keinen AROS-Boot.

## AROS ROM nur diagnostisch prüfen

Wenn du eine legale 512-KB-ROM-Datei besitzt, kannst du sie dem CPU-Kern geben:

```bash
./armithlon full68k-probe-rom /PFAD/ZUR/aros-amiga-m68k-rom.bin --cpu 68040 --cycles 1000000
```

Der **ROM-Probe ist kein Bootversprechen**. Er versucht Reset-Vektor und Opcodes auszuführen, aber AROS wird dabei noch nicht als vollständiges OS gebootet. Das Programm gibt bewusst Exitcode `3` zurück, wenn die ROM-Probe nicht mit einem validierten OS-Boot verwechselt werden darf. Faults werden mit PC und Adresse protokolliert. Für den bereits funktional unterstützten Bootpfad der vorhandenen Deadwood-AROS-Basis nutze weiterhin `./armithlon boot-qrx-compat` mit FS-UAE.

## Einordnung: Welche CPU ist nun emulierbar?

| Modell | Backend | Ergebnis |
|---|---|---|
| Motorola 68000 | Musashi | Codepfad integriert, auf M1 noch ungeprüft |
| 68010 | Musashi | Codepfad integriert, auf M1 noch ungeprüft |
| 68020 / 68EC020 | Musashi-020 | 68020 auswählbar |
| 68030 | Musashi | Codepfad integriert, auf M1 noch ungeprüft |
| 68040 | Musashi | Codepfad integriert, auf M1 noch ungeprüft |
| 68060 | Kein vollständiges Backend | fehlt |
| Apollo 68080 | Kein vollständiges Backend | fehlt |
| ARM64-JIT | alter QRX68100 Mini-Interpreter | vorhandene Teilimplementierung, nicht mit Musashi kombiniert |

Wichtig: Das Modell `QRX68100` ist eine **Projektbezeichnung für das virtuelle System**, keine Motorola-CPU-Baureihe mit eigener vollständiger ISA. Für AROS 68k benötigen wir zunächst eine korrekt implementierte 68020+-CPU und danach ein kompatibles komplettes Board.

## MMIO und ROM

Der neue Kern benutzt `0xFF000000` als zusätzliche 32-Bit-Adresse für QRX-Geräte, weil `0x00FF0000` mit einem klassischen ROM-Bereich `0x00F80000–0x00FFFFFF` kollidiert. Für die alten Monitortests ohne Kickstart bleibt der ursprüngliche Port erreichbar. Das überarbeitete Busmodell und alle Register stehen unter `engine/full68k/qrx68100_full.h`.

Die ROM-Bereiche sind als Leseabbild `0x00F80000` (Haupt-ROM, 512 KB) und `0x00E00000` (Ext-ROM, 512 KB) ausgelegt; die ersten acht Byte des Haupt-ROM werden für Reset-Vektoren optional bei Adresse 0 gespiegelt. Diese Belegung ist ein **erster QRXBoard-Prototyp**, keine fertig verifizierte Amiga-Memory-Map.

## Bestehende Features

Die bisherigen Befehle wie `./armithlon build`, `./armithlon test`, `./armithlon benchmark-qrx86`, `./armithlon benchmark-all`, `./armithlon boot-qrx-compat` bleiben erhalten. Windows-/Linux-QRX86 und Metal/Vulkan sind unabhängige Laufzeitsysteme.
