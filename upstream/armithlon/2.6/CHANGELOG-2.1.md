# ARMITHLON 2.1 – 1. November 2026

## Echte neue Implementierung

- **Gast-RAM, ein Viertel des effektiven physischen Host-RAMs als Standard.** macOS
  nutzt `hw.memsize`, Linux sysconf und respektiert zusätzlich ein gültiges
  cgroup-Limit, damit die VM in Containern nicht das Hostlimit überschreitet.
  Die Reservierung ist über `mmap` virtuell/sparse, also ohne kilometerweites
  Nullsetzen oder direkte physische Belegung aller Seiten. `--ram auto`,
  `--ram-mib N` und `--ram N` unterstützen 1..3840 MiB.
- **Keine künstliche 8-/256-MiB-Schranke** mehr im QRX-Firmware- bzw.
  Musashi-68040-Allokator; die 32-Bit-Gastadresse begrenzt allerdings
  sinnvoll nutzbaren linearen Speicher auf unter 4 GiB. Der obere Bereich
  bleibt für MMIO/ROM/Folgemodelle reserviert. Über 4 GiB sind ohne
  64-Bit-Banking/virtuelle MMU nicht ehrlich möglich.
- **Schonender Firmware-Boot**: Initialisiert nur die 256-KiB-Firmwarezone,
  statt den gesamten Gastspeicher beim Start auf physische Seiten zu schreiben.
  `--rom` exportiert nun genau diese Firmwarezone, nicht Gigabytes RAM.
- Neue **32-Bit-BSP-Register** für RAM-Kapazität bei `FF000034` (lo),
  `FF000038` (hi) und MMIO-Aliasinformation bei `FF00003C`. Für die klassische
  Kickstart-ROM-Belegung liegt der QRX-Controller sicher bei `FF000000`;
  der alte `00FF0000`-Alias ist dann von der ROM belegt.
- **Amiga HUNK-Dateien (kleine echte 68k-Binärprogramme)** werden sowohl
  aus QRXFS als auch aus dem gemounteten `HOST:`-Verzeichnis erkannt,
  sicher und transaktional geladen (HEADER, CODE, DATA, BSS, RELOC32,
  SYMBOL, DEBUG, END) und durch die 68k-Firmware ausgeführt. Das ist eine
  *Teilmenge des Dateiformats*, kein AmigaDOS/Exec-Kompatibilitätslayer.
- Neuer **Kompatibilitätsstart mit echtem AmigaOS 3.x**, falls Benutzer
  rechtmäßig vorhandene Kickstart-ROM- und Workbench-Dateien mitbringt:
  `./armithlon amigaos3 --kickstart ... --system-dir ... --share ...`.
  Erzeugt eine überprüfte FS-UAE-Konfiguration, startet einen echten
  AmigaOS-68k-Gast auf einem virtuellen A4000/040, Kern 1 bleibt aus.
  Dieses Profil bietet originale Exec-/DOS-/Intuition-Laufzeit statt der
  nur teilkompatiblen API-Nachbildung unter AROS. Es ist **kein nativer
  QRX68100-AROS-Boot** und enthält keine kommerziellen Systemdateien.
- `./armithlon workstation` startet den vorhandenen Firmware-Workspace
  mit ausschließlich dem eigens angelegten `~/QRX-Share` als read-only
  Freigabe (Cocoa unter macOS, CLI anderswo). Write nur explizit.
- macOS-Cocoa-Oberfläche nutzt denselben RAM-Auto-Allocator und zeigt
  die virtuelle RAM-Größe an.

## Getestet in dieser Umgebung (Linux x86_64)

- 39/39 CTests Release; 39/39 unter AddressSanitizer/UBSan.
- Wirklicher 68k-Monitor-Boot mit 1024 MiB Auto-RAM (auf 4-GiB-cgroup-Limit)
  und 96-MiB-Gast; direkter HUNK-Code erzeugte `HK` über virtuelle UART.
- Große sparse 3840-MiB-Reservierung, echtes Gastwort bei Adresse `E0000000`.
- 68040-Bus (Musashi-MOCK) mit 128 MiB, Kickstart-ROM-Schreibschutz und
  separatem QRX-MMIO bei `FF000000` getestet.
- QRX-Workstation mit automatisch angelegter read-only HOST-Freigabe und Auto-RAM getestet.
- FS-UAE-AmigaOS3-Profil generiert und auf Eingabepfade geprüft; **echtes
  AmigaOS nicht ausgeführt**, ROM/Workbench nicht mitgeliefert.

## Nicht als fertig zu melden

Native Deadwood-AROS-Exec-Boot auf QRX68100, SMP-Exec/Task-Scheduling,
AmigaOS3/AROS68k Exec-/DOS-/Intuition-ABI in der eigenständigen Firmware,
realer Musashi-FPU/MMU-Test, Apple-M1-/Metal-Validierung,
AmigaOS3- oder AROS-Gast-Workbench in der QRX68100-VM.
Der bestehende QRXDesktop-6.6.20-Gast bleibt Bestandteil des separaten
AROS-/FS-UAE-Pfades; die Cocoa-Firmwarekonsole ist **nicht QRXDesktop**.
