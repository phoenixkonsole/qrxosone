# ARMITHLON 1.8 – QRX68100 kann jetzt wirklich starten und benutzt werden

## Zwei **unterschiedliche** Betriebsmodi

**A – QRX68100 Standalone-Firmware (neu, ohne Internet / Fremd-ROM):**

```sh
./armithlon boot
```

Das startet ein wirklich ausgeführtes 68k-Programm mit Resetvektoren und einem
Terminal, einer eigenen sehr kleinen Shell, persistentem QRXFS-1-Image und
zwei ausführbaren 68k-QEX1-Demoprogrammen. Die Instruktionen werden vom
bereits vorhandenen begrenzten QRX68100-Fallback-Interpreter verarbeitet.
Auf Linux x86-64 wurden der vollständige Boot, Dateizugriffe, Kaltstart,
Neustart und die ausgeführten 68k-Demoprogramme geprüft. Auf Apple M1 ist
Build/Lauf noch zu prüfen.

**B – Tatsächlicher grafischer AROS-Amiga-Desktop (bestehender FS-UAE-Pfad):**

```sh
brew install cmake gh xz fs-uae
./armithlon desktop
```

Dieser Modus startet eine extern heruntergeladene, anhand hinterlegter SHA-256
validierte Deadwood-AROS-68k-LiveCD/ROM-Konfiguration mit FS-UAE. Der QRX68100
wird dabei **nicht** als CPU verwendet: FS-UAE emuliert eine Amiga 4000/040
mit uaegfx-RTG. Das System benötigt Zugriff auf das private GitHub-Repository,
`gh auth login` und die unveränderten Upstream-Dateien. Dieser AROS-GUI-Pfad
konnte in der aktuellen Linux-Entwicklungsumgebung nicht gestartet werden.
Ein eigener QRX68100-basierter AROS-GUI-Boot ist **noch nicht realisiert**.

## Mac – bequem starten

1. `Start-QRX.command` doppelklicken und **1** für die sofort startende
   Offline-Firmware, **2** für AROS/FS-UAE oder **3** für das neue
   native QRX-Firmware-Fenster (`./armithlon gui`) auswählen. Falls macOS die
   Ausführung blockiert, Terminal öffnen und `./armithlon boot` verwenden.
2. Bei erstmaligem Start wird der kleine Quellcode automatisch mit CMake gebaut.
3. Die Konsole beginnt mit `qrx> ` und lässt sich mit `shutdown` verlassen.
4. Das QRXFS-1-Laufwerk wird **außerhalb des Downloadordners** gespeichert:
   `~/Library/Application Support/QRX OS One/qrxfs-1.img`.
   Bei einem Programm-Update bleiben die Benutzerdaten deshalb bestehen.
   Unter Linux ist es `$XDG_DATA_HOME/qrxosone/qrxfs-1.img` (standardmäßig
   `~/.local/share/qrxosone/qrxfs-1.img`).

## Kommandos in der QRX-Firmware

| QRX-Kommando | Wirkung |
| --- | --- |
| `help` | Hilfe anzeigen |
| `ver`, `cpu`, `mem`, `stats`, `uptime`, `date`, `disk` | Systeminformationen |
| `ls` | Dateien auf dem QRXFS-Laufwerk anzeigen |
| `cat README.TXT` | Datei lesen |
| `write TEST.TXT Ein Text` | Datei erstellen oder überschreiben |
| `append TEST.TXT Noch eine Zeile` | Daten anhängen |
| `rm TEST.TXT` | Datei löschen |
| `run HELLO.QEX` | echtes 68k-Demoprogramm mit UART und RTS ausführen |
| `bench` / `run BENCH.QEX` | tatsächliche Gast-68k-Schleife messen |
| `hex APP.QEX 51455831000000024e75` | QEX1-68k-Programm aus Hexbytes schreiben |
| `run APP.QEX` | eigenes 68k-QEX1-Programm ausführen |
| `reboot` | CPU-Reset und Neustart der Firmware; Datenträger bleibt |
| `shutdown` | virtuelles System beenden |

Das Dateisystem heißt QRXFS-1 und ist **kein AmigaDOS, FAT, ext4 oder SFS**.
Es besitzt 16 Slots zu je 4 KiB, einfache Prüfsummen und atomaren Austausch
mittels temporärer Datei + `fsync`. Eingaben, Dateinamen, Dateigrößen und
Gast-Dateizeiger sind begrenzt. Ein beschädigtes Datenträger-Image wird
**nicht** automatisch formatiert.

QEX1 ist ein kleiner eigener Binärcontainer mit `QEX1`, Big-Endian-Länge und
68k-Maschinencode; er ist **kein Amiga-HUNK**, ELF oder Windows-EXE. Er wird
auf Gastadresse 0x10000 geladen und per echtem 68k `RTS` in die Firmware
zurückgeführt. Der Programmstart ist derzeit im Fallback-Interpreter möglich;
Musashi kann den BIOS-Shellcode ausführen, die QEX1-Rücksprungtrampolin-Anbindung
fehlt beim optionalen Musashi-Modus noch.

### Offline-Reproduktionslauf

```sh
./armithlon build
./armithlon boot-test
./armithlon boot-rom
./armithlon boot --disk /tmp/my-qrxfs.img --script data/boot/first-boot.qrx
./armithlon boot --disk /tmp/my-qrxfs.img --command 'cat FIRST.TXT'
```

`boot-rom` legt `data/boot/QRX68100-bootram-v1.8.bin` mit 256 KiB an. Das ist
**ein Gast-RAM-Startabbild, kein Amiga-Kickstart-ROM und kein bootfähiges ISO**.

## Host-/Gastabgrenzung

Die experimentelle macOS-Cocoa-Oberfläche ist ein **Host-Fenster für die echte
68k-Firmware-Shell**, keine Gast-Workbench. Sie bietet Terminalausgabe,
Eingabefeld, Schnellaktionen und denselben persistenten Datenträger; die
Cocoa-App benötigt einen echten Mac-Build und ist noch nicht auf M1 geprüft.
Mit `./armithlon build-qrx-app` lässt sich eine lokale `QRX OS One.app` erzeugen.

Die Firmware-Schleife ist echter 68k-Maschinencode. Sie liest die virtuelle UART,
sammelt die Eingabe im **Gast-RAM** und schreibt einen Kommandopuffer-Zeiger
über QRX-MMIO an eine kleine **hostimplementierte Firmware-Dienstschnittstelle**.
Diese Dienstschnittstelle parst Shell-Kommandos und greift auf das isolierte
QRXFS-1-Image zu. Die Dateisystem- und Command-Parserlogik ist also **noch
kein nativer 68k-Betriebssystemkern**. Es gibt noch keine Tasks/Exec, AmigaOS-
Bibliotheken, HUNK-Loader, RTG-Workbench, AHI, native PPC/x86-JIT-Prozesse,
Netzwerk oder eine vollständige Amiga-Hardware-Emulation. Kern 1 bleibt
im Startprofil geparkt. Die 68040-, Vampire-/AMMX- und parallelen JIT-Teile
verbleiben separat in ihrem bislang getesteten Entwicklungsstatus.

## Tests und tatsächlicher Status

- **26/26 portable CTests bestanden** (Linux x86-64; QRX68100-Firmware inkl.
  echtem Interpreter, QRXFS-1, Kaltstart, RAM/IO, Dateilimits und Korruption).
- Das 68k-Demoprogramm `HELLO.QEX` gibt `Hello QRX` über die emulierte UART aus.
- Der 68k-Benchmark `BENCH.QEX` läuft mit 10.002 tatsächlich ausgeführten
  Gäste-Instruktionen (Wert ist nicht automatisch CPU-Äquivalent oder GPU-FPS).
- macOS M1, Metal, Musashi 040 im aktuellen Build, FS-UAE-GUI in dieser
  Umgebung **nicht** ausgeführt. Keine Performance-Aussagen für diese Pfade.

### Entwicklungsziel für den nächsten wirklichen OS-Meilenstein

Für einen AmigaOS-3.x-/AROS-Desktop **direkt** auf dem QRX68100 braucht die
Emulationsumgebung zusätzlich einen bootfähigen 68040 mit geprüfter FPU/MMU-
Semantik und Interrupts, einen vollständigen Amiga-Chipset-/Autoconfig-Pfad
oder ein dafür gebautes AROS-BSP, den RTG-/AHI-/Input-Treibersatz und einen
persistenten Amiga-Dateisystem- oder Blockdevice-Layer. Die bestehende
Musashi-Anbindung ist ein möglicher CPU-Backend-Baustein, kein fertiger
Betriebssystem- oder Hardware-Unterbau.
