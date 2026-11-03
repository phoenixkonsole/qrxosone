# ARMITHLON 1.9 – Integrationsstufe für QRX OS One (DE)

**Wichtig:** Dies ist ein real gebautes und lokal getestetes Entwicklerpaket,
**keine fertig bootende AROS-/AmigaOS-3.x-Distribution auf QRX68100**.
Die eigenständige 68k-Firmware (UART/QRXFS/QEX1) bootet weiterhin. Der separate
AROS/QRXDesktop-Boot funktioniert nur über FS-UAE und die vorhandenen AROS-Roms.

## 1) Was mit dem bereits vorhandenen QRXDesktop passiert

QRXDesktop **existiert wirklich** und enthält bereits Doppelklick-Handler für
PE32/i386 und ELF32/i386 sowie Datei-Icons, Dateimanager und Programmstarter.
Deshalb wird nichts neu erfunden. Der bestehende Quellcode aus GitHub wird nur
an drei Stellen ergänzt: Die Win32-/Linux-Emulation startet asynchron, damit
QRXDesktop während der Gast-Ausführung reagiert. Außerdem werden problematische
AmigaDOS-Dateinamen abgelehnt, anstatt an die AmigaDOS-Shell weiterzureichen.

Auf deinem M1 (GitHub-Zugriff erforderlich):

```sh
cd ~/Downloads/ARMITHLON-1.9-macOS
brew install git gh cmake
# gh auth login nur falls dein Repo privat ist/keine Anmeldung besteht
./armithlon qrxdesktop-patch
```

Das Ergebnis liegt in `data/qrxdesktop/qrx_desktop-1.9.c` und kann mit der
bestehenden AROS-68k-SDK-Konfiguration von QRX OS One gebaut werden. **Das
Script kompiliert und installiert den AROS-Desktop nicht selbst.** Der ursprüngliche
GitHub-Branch bleibt unberührt. Für das laufende FS-UAE-AROS ist weiterhin
der alte Desktop verwendbar, bis die neu gebaute Fassung installiert wurde.

## 2) QRX86-Dateien sauber erkennen und starten

```sh
./armithlon build
./armithlon app-inspect /PFAD/PROGRAM.EXE
./armithlon app-inspect /PFAD/PROGRAM.ELF
# vorher: ./armithlon fetch und beide Host-Runtimes bauen
./armithlon build-qrx86-host
./armithlon build-qrx86-linux-host
./armithlon run-native-qrx86       # Metal-Empfänger in Terminal 1
./armithlon app-launch /PFAD/PROGRAM.EXE  # Terminal 2
./armithlon app-launch /PFAD/PROGRAM.ELF
```

`app-launch` ist ein **expliziter Host-Vorgang**. Ein untrusted 68k-Gast
kann damit nicht heimlich beliebige Host-Prozesse starten. Nur x86-PE32 und
Linux-i386-ELF32 werden weitergegeben; Amiga-HUNK, m68k-ELF, x64, ARM64,
QEX1 benötigen eigene Ausführungspfade. Die QRX86-Laufzeit bleibt auf ihre
wirklich implementierten Win32-/Linux-HLE-APIs beschränkt. Keine falschen
Kompatibilitätsversprechen für beliebige EXEs.

## 3) QRX68100 – Musashi, Boot, Dual-Core

```sh
./armithlon boot                    # Offline-Firmware, alter geprüfter Interpreter
./armithlon full68k-setup          # GitHub-Source Musashi SHA-gepinnt beziehen
./armithlon full68k-build
./armithlon full68k-test           # umfasst optionale echte Gast-Firmware-Tests
./armithlon boot-full --command 'run HELLO.QEX'
./armithlon boot-full --command 'run BENCH.QEX'
./armithlon boot-full --dual --script data/boot/first-boot.qrx
```

Die `boot-full`-Version benötigt den tatsächlichen Musashi-68040-Kern. Sie führt
QEX1 jetzt mittels guest Stack-Trampolin aus, statt sie abzulehnen. In `--dual`
läuft zusätzlich ein zweiter, getrennter 68040-Guest-Kontext als Worker; der
Scheduler ist **noch kein AROS Exec SMP**. `--dual` ist im klassischen 68040-
Ein-Kern-Modus standardmäßig deaktiviert, damit AmigaOS-3.x-Software nur
Kern 0 sieht.

## 4) Neues virtuelles Blocklaufwerk für einen späteren AROS-BSP

```sh
./armithlon boot --raw-disk /tmp/aros-raw.img --raw-sectors 4096 \
  --create-raw-disk --command 'ls'
# Folgelauf OHNE --create-raw-disk (existierendes Image bleibt erhalten):
./armithlon boot --raw-disk /tmp/aros-raw.img --raw-sectors 4096 \
  --command 'cat README.TXT'
```

Die Block-MMIO-Register liegen bei `0xFF000700` (68020+/68040) beziehungsweise
`0x00FF0700` (älterer Virtboard-Alias). Es gibt einen 512-Byte-Sektor-DMA-
Pfad, LBA, Status und Rohdiskdatei. Ein AROS-`scsi.device`-/`trackdisk.device`-
Treiber, DOS-Handler, RDB/FFS und das AROS-Root-Dateisystem sind noch zu
implementieren. Gleiches gilt für nativen QRXGPU-RTG, AHI und Input-BSP.

## 5) Native AROS-Boot-Qualifizierung

```sh
./armithlon aros-native-status
```

Der Report zeigt **BLOCKED**, bis der AROS-BSP, FPU-/MMU-Qualifikation,
Betriebssystem-RAM-/IRQ-Setup, Block-/RTG-/AHI-/Input-Treiber und ein
wirklich gestarteter **QRXDesktop als 68k-Gastprozess** bestätigt sind.
Das vorhandene FS-UAE-Screenshot ist keine gültige native QRX68100-Evidenz.

## 6) Lokale Tests und Werkzeuge

```sh
./armithlon build
./armithlon test
./armithlon boot-test
python3 tools/test-qrxdesktop-patch19.py
# Optional echter 68040/Musashi (erst nach full68k-setup):
./armithlon full68k-test
```

In dieser Entwicklungsumgebung: 29/29 reguläre Linux-x86-64-Tests bestanden;
29/29 Sanitizer-Tests bestanden. Mac- und FPU-Tests bleiben ausdrücklich offen.
