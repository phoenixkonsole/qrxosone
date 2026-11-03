# ARMITHLON 2.0 – QRX68100 Board Support / HOST: / SMP-Fundament

## Stand und ausdrücklich verbleibende Grenzen

**Bootfähig:** eigenständige, echte 68k-Firmware-Konsole (bestehender QRX-Interpreter);
QEX1-Ausführung, QRXFS und seit 2.0 ein **echter eingebundener Host-Ordner**.
Auch **ohne jegliche .img-Datei** startbar (`--volatile-disk`).

**Nicht bootfähig:** ein vollständiges Deadwood-AROS auf nativer QRX68100-Hardware.
FS-UAE bleibt der externe AROS-Kompatibilitätsweg. Auch das experimentelle
Musashi-68040-Backend und die AROS-68k-Cross-Compiler sind in der Linux-
Testumgebung nicht installiert. Ein echter QrxDesktop-Gastprozess, ein
DOS-packet-Handler für ein `HOST:`-Volume und ein AROS Exec SMP-Port sind
**nicht** vorhanden. Die neuen Gast-C-Dateien sind Hardwaretreiber-Bausteine,
keine installierten AROS-ROM-Module und keine als voll funktionsfähig bestätigten
RTG-/AHI-/input.device-Treiber.

### Die Dateien im Paket

- `engine/src/qrx_hostfs.c`, `engine/include/qrx_hostfs.h`: hostseitiger
  Port/Treiber für einen Directory-Filesystem-Mount, Gast-DMA und Dateifunktionen.
- `engine/bsp/qrx_smp_ready.c`, `engine/include/qrx_smp_ready.h`: gemeinsames
  Ready-Queue-Modell mit Priorität, CPU-Affinität, UP-Fallback und atomarem Lock.
- `engine/guest/aros/bsp/QRXShare.c`: AROS-68k-Nutzerprogramm zum Kopieren
  vom/zum QRXHostFS-Controller via MMIO (noch **kein DOS Volume Handler**).
- `engine/guest/aros/bsp/QRXBoardInfo.c`: Board-Probe für QRX (CPU/GPU/HostFS).
- `engine/guest/aros/bsp/QRXSmpInfo.c`: lesender QRX-Dualcore-Status.
- `engine/guest/aros/bsp/qrx_smp_board.h`: CPU1-Start/IPI/ACK Hardware-ABI.
- `engine/guest/aros/bsp/build-aros-board.sh`: Cross-Build **wenn** ein passender
  AROS/68k-SDK installiert ist. In dieser Umgebung nicht ausgeführt.
- `engine/guest/qrxvirt_media.h` und `qrxvirt_board.h`: neues Buildflag
  `QRX_NATIVE_040` zur Wahl der sicheren 32-Bit-MMIO-Adresse `0xFF000000`
  statt der Kickstart-überlappenden alten Adresse `0x00FF0000`.

## 1. Sofort benutzen: Host-Verzeichnis statt Disk-Image

```
./armithlon build
mkdir -p "$HOME/QRX-Share"
printf 'Hallo vom Mac\n' > "$HOME/QRX-Share/NACHRICHT.TXT"
./armithlon boot --volatile-disk --share "$HOME/QRX-Share" --ram-mib 8
```

Im QRX-Firmware-Prompt:

```
qrx> hostinfo
qrx> hostls
qrx> hostcat NACHRICHT.TXT
qrx> hostget NACHRICHT.TXT
qrx> cat NACHRICHT.TXT
```

Bei **bewusster RW-Freigabe** (niemals automatisch):

```
./armithlon boot --volatile-disk --share "$HOME/QRX-Share" --share-rw
qrx> write NOTES.TXT Meine Daten
qrx> hostput NOTES.TXT NOTES-EXPORT.TXT
qrx> hostls
```

Mit `run HOST:DATEI.QEX` können gültige kleine 68k-QEX1-Dateien unmittelbar
vom HOST-Verzeichnis geladen werden. Keine beliebigen EXEs/ELFs vom Host
werden ausgeführt. Wer die Änderungen in QRXFS dauerhaft behalten will,
startet ohne `--volatile-disk` und nutzt das übliche QRXFS-Image zusätzlich.

**Wichtig:** `--volatile-disk` erzeugt wirklich kein Image; der interne QRXFS-
Bereich ist dann nach dem Prozessende weg. **HOST:**-Dateien sind echte Dateien
im ausdrücklich ausgewählten Verzeichnis und bleiben natürlich erhalten.

### Schutzgrenzen des HOST:-Geräts

Der Treiber bindet genau einen vorhandenen Ordner per `open(O_DIRECTORY|
O_NOFOLLOW)` an. Es gibt ausschließlich Dateinamen ohne `/`, `..`-Pfad,
Unterverzeichnisse, Sonderdateien oder Symlink-Following. 96 Zeichen pro
Dateiname, Transfers bis 4 KiB, Dateien bis 64 MiB, Lesen mit Dateioffset,
Schreiben mit Offset und explizite Truncation. Die AROS-Entsprechung muss
noch einen DOS-Handler besitzen, damit `HOST:` überall als Laufwerk erscheint.

Dies ist eine **geteilte Dateisystemfreigabe**, keine VFS-Ausführung
untrusted Fremdcode. Trotzdem kann `--share-rw` reale Dateien im gewählten
Verzeichnis ändern oder löschen. Ein separates Verzeichnis wählen; das Home-
Verzeichnis nicht freigeben. Symlinks und andere Verzeichnisse sind gesperrt;
keine vollständige OS-Sandbox oder Zugriffsrechteersatz.

### Native Mac-Oberfläche

Optional unterstützt `./armithlon gui` `QRX_HOST_SHARE=/absoluter/ordner`.
Schreibzugriff zusätzlich nur mit `QRX_HOST_SHARE_RW=1`.
Das Cocoa-Programm konnte hier **nicht** auf einem M1 kompiliert/getestet werden.

## 2. AROS-seitiges Board: existierender Stand und neue Treiber

Die wirkliche virtuelle QRX68100-68040-Maschine benutzt den
**oberen MMIO-Alias `0xFF000000`**, der nicht mit dem Amiga-Kickstart bei
`0x00F80000` kollidiert. Gastgeräte:

| Gerätebereich | Aufgabe | Qualifizierungsstand |
|---|---|---|
| `FF000000` | Board-ID, Features, UART, Timer | CPU-Bus vorhanden, kein AROS timer.device |
| `FF000100` | 2D-FB/Video | QRX-GPU-Protokoll vorhanden, kein `graphics.hidd` |
| `FF000200` | PCM/DMA-Audio | QRX-Protokoll vorhanden, kein AHI-Treiber |
| `FF000300` | Eingabe/Ereignisse | QRX-Protokoll vorhanden, kein input.device |
| `FF000400` | GPU2D-Befehle | Gast-ABI/Host-Pfad vorhanden, RTG fehlt |
| `FF000500` | QRX3D | virtuelle 3D-Schnittstelle, kein Warp3D |
| `FF000600` | 2x 68040, IPI, JIT-Cache | Musashi-Cooperative-Mode, kein Exec SMP |
| `FF000700` | Sektorblockgerät (512B DMA) | getestet, kein DOS-Dateisystemtreiber |
| `FF000800` | HOST:-Ordner DMA | jetzt getestet, kein DOS-Dateisystemtreiber |

Der Quellcode der Gasttools ist bewusst nicht auf fremder Amiga-Hardware
lauffähig; deren Adressen würden dort Busfehler auslösen. AROS Cross-Build:

```
AROS_CC=/pfad/zum/m68k-aros-gcc \
  bash engine/guest/aros/bsp/build-aros-board.sh /tmp/qrx-aros-tools
```

Auf **einem bereits mit diesem Board gebooteten** AROS-68k könnten diese
Tools später als `QRXShare`, `QRXSmpInfo`, `QRXBoardInfo` eingesetzt werden.
Der derzeitige Bootpfad erreicht AROS auf QRX68100 nicht. Es ist daher
keine bereits in AROS getestete Installation enthalten.

## 3. SMP: genau welche offiziellen AROS-Ansätze werden berücksichtigt?

AROS hat einen experimentellen `pc-x86_64-smp`-Port. Die offizielle AROS-
Dokumentation beschreibt eine gemeinsame Runnable-Liste und CPU-Affinität,
auch die aktuellen AROS-Quellen enthalten x86_64-Architekturfunktionen für
CPU-Dispatch und SMP-interne Task-Informationen.

In ARMITHLON 2.0 ist die **portable, atomar geschützte gemeinsame Ready-Queue**
mit 1-/2-Kern-Affinität als QRX-BSP-Grundlage neu implementiert und unter
zwei echten Host-pthreads getestet. CPU1 bleibt im klassischen Modus verborgen.
Im QRX-Erweiterungsmodus hat der bestehende Dual-68040-Bus IPI- und
Core1-Start-Register.

Das ist **noch keine portierte AROS `exec.library`**. Die nächsten Kernel-
Arbeiten erfordern vor allem: getrennte `ThisTask`-/Supervisor-Kontexte,
64/32-Bit Atomics bzw. 68040-TAS/CAS für alle Exec-Listen,
Interrupt-/Reschedule-Entry pro Kern, Taskeintragswechsel und Exit,
Affinity in den AROS-Taskdaten, Cache-/MMU-Fences, FPU-Kontextwechsel,
Priorität und Kontexte unter Preemption. Wegen Musashis globaler CPU-State-
API werden die beiden 68040-Kerne derzeit zeitlich verschachtelt; der neue
ready-queue-pthread-Test beweist nur die Algorithmik, nicht die parallele
Ausführung kompletter AROS-Gastsysteme.

Offizielle Grundlage:
- https://aros.org/introduction/ports.html (`pc-x86_64-smp` experimentell)
- https://aros.org/el/news/archive/2020.html (gemeinsame Ready-Liste/Affinität)
- https://github.com/aros-development-team/AROS/tree/master/arch/x86_64-pc/kernel
- https://github.com/aros-development-team/AROS/blob/master/boot/modular_kickstart.txt

## 4. Tests

```
./armithlon build
./armithlon test
./armithlon hostfs-test
./armithlon smp-ready-test
./armithlon aros-native-status
```

Status in dieser Umgebung: **33/33 portable CTests bestanden** auf Linux x86-64,
inklusive tatsächlichem 68k-Firmwarestart ohne Image, Host-Dateitransfer,
Readonly-/RW-Modus, Symlink-/Traversal-Schutz, CPU-MMIO-Alias und
SMP-Ready-Queue. Weitere Tests mit ASan/UBSan siehe Release-Protokoll.

Der AROS-Boot-Gate-Bericht bleibt bewusst `BLOCKED`. Für die finale
Verwendbarkeit fehlen auch eine qualifizierte 040-MMU/FPU und die Einbindung
von `QRXShare` als DOS-Volume-Handler sowie `graphics.hidd`, AHI und Input-
Treibern in den AROS-68k-ROM-Build. Ungeprüfte Behauptungen wie „bootet
AROS nativ“ oder „AmigaOS 3.x SMP“ sind ausdrücklich nicht Teil dieser Version.
