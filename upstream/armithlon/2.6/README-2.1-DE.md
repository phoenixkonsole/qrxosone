# ARMITHLON 2.1 – RAM-Ausbau und Amiga-Binärkompatibilität

## Sofort ausprobieren

Auf dem Mac (nach Installation von Xcode Command Line Tools + CMake):

```sh
cd ~/Downloads/ARMITHLON-2.1-macOS
./armithlon build
./armithlon test
./armithlon workstation
```

`workstation` öffnet die existierende QRX68100-**Firmwarekonsole** als
macOS-AppKit-Fenster und bindet `~/QRX-Share` **nur lesend** ein. Dies ist
**nicht** die originale QRXDesktop-6.6.20-Workbench. In der Firmware:

```
mem
hostinfo
hostls
ls
run HELLO.QEX
```

Auf Linux lädt `workstation` die CLI-Firmware. Eine ganz ohne disk.img
laufende Session:

```sh
mkdir -p "$HOME/QRX-Share"
./armithlon boot --ram auto --volatile-disk --share "$HOME/QRX-Share"
```

Das Dateisystem enthält keine AmigaDOS-Volumes. Normale Host-Dateien
können über `HOST:` wie in `README-2.0-DE.md` gelesen werden.
Bei `--share-rw` sind echte Host-Dateiänderungen möglich – niemals das
Home-Verzeichnis oder andere wertvolle Ordner direkt freigeben.

## Virtueller Arbeitsspeicher

Der Gast reserviert standardmäßig **ein Viertel des effektiven Host-RAMs**.
Die macOS-Quelle ist `hw.memsize`; Linux berücksichtigt zusätzlich
Containergrenzen. Der maximale ansteuerbare lineare Bereich wird bewusst
auf **3840 MiB** begrenzt. Ein 68040 besitzt einen 32-Bit-Adressraum –
"unbegrenzt" geht auf dieser Hardwaredefinition nicht. Noch größere
virtuelle RAM-Pools müssten eigene 64-Bit-Adressübersetzung und MMU-/Banking-
APIs nutzen, ohne die 32-Bit-Programmadressen zu verändern. Der gesamte
reservierte Speicher wird **nicht** physisch voralloziert.

Beispiele:

```sh
./armithlon boot --ram auto
./armithlon boot --ram-mib 1024
./armithlon boot --ram 2048
./armithlon boot --volatile-disk --command mem --no-interactive
```

Der normale AROS-/AmigaOS3-Speicherverwalter kann diesen Speicher noch nicht
vollständig verwenden. Dazu muss der künftige QRX-BSP die RAM-Karte korrekt
registrieren und die klassischen Kickstart- und Chipset-Speicherregionen
sicher auslassen. Der neue Qrx-Board-ABI meldet die Bytezahl über
`0xFF000034`/`0xFF000038`, die sichere MMIO-Adresse liegt bei `0xFF000000`.

## Zwei bewusst getrennte Kompatibilitätswege

**AROS68k und eigene QRX68k-Firmware:** Die eigenständige Firmware startet,
unterstützt kleine QEX1-Programme und ab 2.1 einfache Amiga-HUNK-Binärdateien
(HUNK_HEADER/CODE/DATA/BSS/RELOC32); ein realer 68k-HUNK-Test wurde im
Emulator ausgeführt. HUNK-Programme mit AmigaOS-Systemaufrufen
(Exec, DOS, Intuition, GadTools, AHI, Datatypes) laufen auf dieser Firmware
noch nicht. Für sie wird ein richtiger AROS-Kernel gebraucht.

**Echte AmigaOS3.x-Kompatibilität:** Besser als eine unvollständige
AmigaOS-API-Emulation ist für Altprogramme ein Fallback auf das **echte
AmigaOS3.x** mit seinen Originalbibliotheken und Original-Kickstart. Dafür
wird ein getrenntes, grundsätzlich einkerniges FS-UAE-Profil erzeugt:

```sh
./armithlon amigaos3 \
  --kickstart "/PFAD/zu/eigenem/kickstart3.1.rom" \
  --system-dir "/PFAD/zum/installierten/Workbench-3.x-System" \
  --share "$HOME/QRX-Share"
```

Zum bloßen Generieren/Validieren ohne Emulator `--dry-run` ergänzen.
Nötig sind **eigene lizenzierte Kickstart-/AmigaOS-Installationsdateien** und
FS-UAE. Keine ROM- oder Workbench-Binärdateien liegen bei. Das Profil wurde
nur auf Testverzeichnis- und Inputbehandlung geprüft, nicht gegen eine
vorhandene echte AmigaOS-Installation oder auf Apple M1 gebootet.

Der normale AROS-68k-FS-UAE-Desktop ist weiterhin über
`./armithlon desktop` erreichbar, nachdem die erforderlichen AROS-Basisdateien
per `./armithlon fetch` bezogen wurden. Der originale QRXDesktop existiert
im GitHub-Quellstand. Die seit 1.9 vorgesehene sichere, asynchrone Weitergabe
von PE32/ELF32-Doppelklicks an QRX86 ist erhalten, aber noch nicht als
vollständig lauffähiger AROS-Desktop-Gastpfad nachgewiesen.

## Was weiterhin fehlt

Der **native AROS-68k-/QRX68100-Boot**, echter dualer AROS-Exec-Scheduler,
implementierte AROS-`HOST:`-DOS-Gerätehandler und zertifizierte RTG/AHI/Input-
Treiber. Das kleine HUNK-Ladeformat alleine ersetzt kein AmigaOS. Die
68040-FPU und CPU-Ausnahmen auf echtem Musashi wurden hier noch nicht mit
Realsoftware getestet; bei vorhandenen Musashi-Quellen steht
`./armithlon full68k-test` weiterhin zur Verfügung.

## Tests

```sh
./armithlon build
./armithlon test
./armithlon boot --ram auto --volatile-disk --command mem --no-interactive
```

Linux x86-64: **39/39 CTests bestanden**, zusätzlich **39/39 mit
AddressSanitizer/UBSan**. 96-MiB-HUNK-Boot, 1024-MiB-Autobooot, große sparse
3840-MiB-Reservierung und 128-MiB-68040-Busprüfung durchgeführt.
Das ist keine Apple-M1-/AmigaOS-/AROS-Desktop-Abnahme.
