# Roadmap: Fremd-Volumes und Mac-Dateisysteme

Stand 2026-11. Diese Roadmap sammelt, was heute an fremden Dateisystemen im
QRX-OS-One-Gast läuft, wie Amiga-, PC- und Mac-Daten nebeneinander liegen
sollen und welcher Weg zu einem vollständigen Lese- (und später Schreib-)
Pfad für historische Mac-Images führt. Sie ist bewusst unabhängig von der
Toolbox-Roadmap in `docs/QRXPPC_ROADMAP.md`; Berührungspunkt ist nur der
File Manager der PPC-Toolbox, der nichts von Volumes weiß.

## Ist-Stand

| Volume | Handler / Gerät | Quelle | Modus | Grenze |
|--------|-----------------|--------|-------|--------|
| `HOSTSYS:` | `L:QRXHostFS` (`DEVS:Mountlist`) | Host-Verzeichnis über die ARMithlon-Door | lesen/schreiben | nur der freigegebene Host-Baum |
| `APPS:` | `L:QRXAppsFS` (`DEVS:Mountlist`) | Host-Verzeichnis für Anwendungen | lesen | feste Zuordnung |
| `IPFS:` | Assign auf den lokalen IPFS-Node | IPFS-Objekte, Revisionen | lesen/schreiben (lokal) | Node muss laufen |
| `CD0:` / `ICD0:` | `L:ODFileSystem`, `Control = "HFSFIRST"` | optische Medien und Image-Dateien über `diskimage.device` | nur lesen | HFS/HFS+ ohne Resource Forks |
| `HFS0:` | `L:ODFileSystem`, `Control = "HFSFIRST IMAGE=…"` | HFS-Image-Datei direkt, `BlockSize = 2048` | nur lesen | Datenfork, feste Blockgröße |
| `AFD0:`…`AFD3:`, `RAD:` | `afs-handler` | ADF/RAM-Disk über `fdsk.device`/`ramdrive.device` | lesen/schreiben | ADF-Geometrie fest verdrahtet |
| `FAT:` | `fat-handler` | FAT12/16/32-Image über `fdsk.device` | lesen/schreiben | Image, kein Wechselmedium |
| `HOME:` / `USR:` | `emul-handler` | Host-Pfade | lesen/schreiben | Unix-Semantik, keine AmigaDOS-Rechte |
| `SMB:` | `smb-handler` | Netzfreigaben, sofern konfiguriert | lesen/schreiben | Konfiguration im Gast |
| QRX-Volumes | `QRXVolImage` | verschlüsselte Volume-Images (QRX Drive) | lesen/schreiben | nur eigenes Format |

Dazu zwei Sonderwege, die Mac-Daten schon heute lesen, ohne sie zu mounten:
`QRXDSK` (`tools/armithlon/diskimage/qrx-dsk.c`) liest den HFS-Katalog eines
1.44-MB-Images direkt, um ein geteiltes StuffIt-Archiv zu joinen, und
`QRXUnSit` entpackt StuffIt-Archive im Gast. Beide sind Einzweck-Werkzeuge,
kein Dateisystem.

`ODFileSystem` deckt als Handler ISO 9660, High Sierra, Rock Ridge, Joliet,
UDF, HFS und HFS+ ab und wählt auf Hybrid-Discs per `Control = "HFSFIRST"`
die Mac-Sicht. Die dokumentierte Grenze ist für diese Roadmap die wichtigste:
**nur Datenfork, Resource Forks werden nicht als Dateien abgebildet, und
Schreiben gibt es nicht.**
Klassische Mac-Software ist damit oft unvollständig, auch wenn die
Hauptdateien lesbar sind.

Im Desktop (`qrx_desktop.canonical`, `qrx_refresh_mounted_volumes()`) stehen
neben vier festen Zeilen (`SYS:`, `QRX:`, `RAM:`, `T:`) höchstens
`QRX_MOUNTED_VOLUME_MAX = 4` weitere Volumes aus der DOS-Liste; RAM Disk,
Live-CD und Kickstart werden ausgefiltert. Es gibt keine Typkennzeichnung,
kein Read-only-Kennzeichen und keine Kapazitätsanzeige. Mehr als vier fremde
Volumes sind heute schlicht unsichtbar.

## Zielbild

Ein Namespace, alle Plattformen nebeneinander, ohne Sonderfälle im
Dateimanager:

```
HOSTSYS:   Host (macOS/Linux)        lesen/schreiben
APPS:      Anwendungen               lesen
MAC0:      Mac-Image (MFS/HFS/HFS+)  lesen, später schreiben
AMIGA0:    ADF/HDF (FFS/OFS/SFS)     lesen/schreiben
WIN0:      FAT12/16/32-Image         lesen/schreiben
CD0:       ISO9660/UDF/HFS-Hybrid    lesen
IPFS:      IPFS-Objekte              lesen/schreiben
```

Der Handler entscheidet über Semantik, der Desktop zeigt nur, was die
DOS-Liste meldet: Name, Dateisystem-Typ, Modus (RO/RW), freier Platz,
Quelle (Gerät oder Image-Datei). Ein Volume ist ein Objekt wie ein Ordner;
Kopieren zwischen zwei Volumes ist ein normaler `Copy`-Aufruf, auch wenn
Quelle und Ziel verschiedene Namens-, Zeit- und Fork-Regeln haben.

## Arbeitspakete

### V1 — Mount-Registry und mehr Volumes

- Desktop: Volumeliste dynamisch aus der DOS-Liste, Grenze von 4 auf 16
  anheben, feste Zeilen `SYS:`/`QRX:`/`RAM:`/`T:` in dieselbe Liste
  überführen.
- Je Volume Typ, Modus und Kapazität ermitteln (`Info()`, `DosList`-Flags,
  `DOSType`) und im Files-Fenster als Badge zeigen.
- Neues `C:QRXVolumes` als CLI: listet Volumes mit Handler, `DosType`,
  RO/RW, Image-Pfad; `--json` für Tests.
- `QRXMountImage` von „Unit + Datei" zu „Volume-Name + Image" heben
  (sucht freie `diskimage.device`-Unit, schreibt DOSDriver-Eintrag,
  mountet, meldet Fehler). Heute sind die Units 0…9 fest
  (`QRX_IMAGE_UNITS = 10` in `QRXLoadDevice`) — das ist die Obergrenze
  gleichzeitiger Images und muss mitwachsen oder verwaltet werden.

### V2 — Amiga-Volumes

- ADF/ADZ/EXE-ADF über `diskimage.device` mit den vorhandenen
  `afs-handler`-Einträgen; Geometrien 880 K, 1.76 M und HDF statt der
  fest verdrahteten 80/2/11-Konfiguration.
- FFS/OFS lesen (`afs-handler` aus dem ROM-Filesystem-Resource), SFS
  lesen/schreiben (`L:sfs-handler`); PFS3 ist nicht Teil des Ziels.
- Ziel ist nicht neue Amiga-Funktionalität, sondern dieselbe Bedienung wie
  bei den anderen Plattformen (V1-Metadaten, V5-Kopieren).

### V3 — PC-Volumes

- FAT12/16/32 über `fat-handler` mit echten Image-Dateien statt nur
  `fdsk.device`; ISO/UDF über `ODFileSystem`; feste VHD-Rohimages.
- Kein NTFS-Schreibpfad (nur lesen, wenn überhaupt), kein exFAT.

### V4 — Mac-Images und historische Mac-Dateisysteme

Das ist der Kern dieser Roadmap. Reihenfolge nach Nutzen und Risiko:

Stand 2026-11: Der Container-Layer und der MFS-Leser sind als M1 umgesetzt
(`tools/armithlon/macimage/`, read-only). Erkannt werden rohe Sektorimages,
DiskCopy 4.2 (84-Byte-Kopf, Prüfsumme), unkomprimierte NDIF-Datenforks (mit
`bcem`-Resource im `.rsrc`-Sidecar) und Apple-Partition-Maps mit mehreren
Einträgen; der MFS-Leser öffnet 400-K-Volumes, listet die flache
Dateiliste und liest Daten- wie Resource-Forks über die 12-Bit-Blockkette.
Das Guest-Werkzeug `C:QRXMacFS` bietet `INFO`, `LIST`, `CAT` und `EXTRACT`.
Synthetische Images (400 K MFS, DC42, NDIF-Sidecar, APM, kaputte Prüfsumme,
abgeschnittener NDIF-Datenfork) entstehen im Build; der Host-Test prüft 68
Zusicherungen, der Gast-Test 16 Zeilen im ARMithlon über die `sc`-Naht.

Mit M2 liest derselbe Pfad auch HFS: Master Directory Block, Katalog-B-Baum
über alle Blattknoten, Extents-Overflow-Baum für Forks mit mehr als drei
Extents, Resource Forks und die Finder-Info (Typ, Creator, Flags, Icon-ID).
`INFO`/`LIST` zeigen Volumen und Katalogeinträge mit Eltern-CNID, `CAT`/
`EXTRACT` nehmen Pfade wie `Docs:Note`. Ein synthetisches 800-K-HFS-Image
liefert einen verschachtelten Ordner, einen Resource Fork und eine Datei,
deren vierte Fragment-Extent nur im Overflow-Baum steht; Host-Test (121
Zusicherungen) und Gast-Test (27 Zeilen) laufen beide grün. Read-only, ohne
jede Apple-Binärdatei.

Mit M3 folgen HFS+ und HFSX: Volume-Header, Katalog-B-Baum mit UTF-16BE-
Namen, Extents-Overflow für Forks über acht Extents und der Attribute-B-Baum
als Rückfall für Resource Forks (`com.apple.ResourceFork`, Fork-Records).
Ein HFS-Wrapper (Mac OS 8.1–9) wird aufgelöst, damit ein Image ein Volume
bleibt; HFSX erkennt der Leser an der Signatur und vergleicht Namen dann
groß-/kleinschreibungssensitiv. Ein gesetztes Journal wird nur gemeldet
(`journal=1`), nie replayed. Testimages: 2 MiB HFS+ mit neun Fragment-
Extents, Resource Fork im Katalog und im Attribut-Baum, 2 MiB HFSX mit
`Hello`/`hello`, dazu derselbe Inhalt hinter einem HFS-Wrapper. Host-Test
195 Zusicherungen, Gast-Test 40 Zeilen — beide grün.

Mit M4 kommen die Fork-Container dazu, in denen Mac-Dateien außerhalb eines
Mac-Volumes reisen: AppleSingle (`0x00051600`) und AppleDouble
(`0x00051607`, Datenfork darf fehlen) mit Namens-, FInfo- und Datums-
Einträgen, MacBinary II (128-Byte-Kopf, 128-Byte-Padding der Forks,
CRC-16/XMODEM über die ersten 125 Bytes) und BinHex 4.0 (Textformat,
6-Bit-Alphabet, 0x90-Run-Length). `QRXMacFS FORK <datei> [ziel] [--rsrc]`
zeigt Name, Typ/Creator, Flags, Fork-Längen und den CRC-Zustand und
extrahiert wahlweise Daten- oder Resource-Fork. Synthetische Container für
alle vier Formate liegen im Testbaum; der Host-Test prüft 256 Zusicherungen,
der Gast-Test 48 Zeilen. Weiterhin gilt: nichts wird in ein Mac-Image
geschrieben, und keine Apple-Datei ist im Baum.

Nicht unterstützt und ausdrücklich abgelehnt: komprimierte/gechunkte NDIF-
Images (`bcem`-Chunkmap wird nicht dekodiert), HFS/HFS+-Schreiben,
Journal-Replay und das Schreiben von Fork-Containern (AppleDouble-Sidecars
erzeugen, MacBinary/BinHex kodieren) — das bleibt V5.

1. **Container zuerst.** Ein Mac-Image ist nicht nur ein Dateisystem:
   - rohe `.dsk`/`.img`/`.hfv`/`.toast`/`.cdr`,
   - DiskCopy 4.2 (84-Byte-Kopf, Prüfsumme, Tag-Größe),
   - NDIF (Disk Copy 6),
   - Apple Partition Map (Block 0 = `ER`, Partitionen `Apple_HFS`,
     `Apple_Free`, Treiberpartitionen),
   - Hybrid-CD (ISO 9660 + HFS in einer Session) — heute über `HFSFIRST`
     lesbar, soll aber beide Sichten gleichzeitig anbieten.
   Ein Container-Layer (`QRXMacImage`) erkennt Format und Partitionen und
   gibt dem Dateisystem-Handler einen Blockbereich, keinen Dateinamen.
2. **MFS** (Macintosh File System, 1984, 400 K): flacher Katalog, keine
   Ordner, ein Extent pro Datei. Kleiner, klar abgegrenzter Lesezugriff;
   dient als erste echte Eigenimplementierung und als Test für den
   Container-Layer.
3. **HFS** (1985): B*-Baum für Katalog und Extents, Resource Forks über
   den `RsrcFork`-Eintrag, Finder-Info, Type/Creator, 1904-Epoche. Lesen
   vollständig inklusive Forks; Schreiben hinter einem Schalter, erst
   nach Konsistenzprüfung (`QRXMacFSCheck`) und nur mit Journal-freien
   Volumes.
4. **HFS+ / HFSX**: Katalog-B-Baum mit Unicode-Namen, Extent-Overflow,
   Attribute-B-Baum (Resource Forks als `com.apple.ResourceFork`).
   Nur lesen; ein nicht leerer Journal-Kopf führt zu einer Warnung und
   Read-only, nicht zu einem Replay. HFSX (case-sensitive) wird über den
   Key-Compare unterschieden.
5. **HFS-Wrapper**: HFS+-Volumes mit `HFSPlus`-Signatur im HFS-Wrapper
   (Mac OS 8.1–9) müssen als ein Volume erscheinen, nicht als zwei.
6. **Metadaten überall**: 1904-Epoche ↔ AmigaDOS-Datum, MacRoman ↔
   Amiga-Zeichensatz, Finder-Flags, Type/Creator als Dateikommentar oder
   Sidecar.

### V5 — Kopieren zwischen Plattformen

- Namensregeln: 31 Zeichen, keine `:`/`/`, Groß-/Kleinschreibung. Kollisionen
  bekommen `~2`, `~3` (wie `ODFileSystem` es für CD-Namen macht); die
  Umbenennung wird gemeldet, nicht verschwiegen.
- Forks auf Zielen ohne Forks: AppleDouble-`._name`-Sidecar (auf FAT/Host)
  oder Ablehnung mit klarer Meldung — nie stiller Verlust.
- Encodings: AppleSingle, AppleDouble, MacBinary II und BinHex 4.0 lesen
  und schreiben. `.sit` entpackt bereits `QRXUnSit` im Gast; die
  Fork-Container sind neu und teilen sich eine Metadata-Schicht.
- Text: kein automatischer Zeilenende-Tausch, aber eine Option im
  Kopierdialog (Mac CR ↔ Amiga LF).

### V6 — Tests und Nachweise

- Synthetische Images, vom Host gebaut (wie `QRXDSK`/`QRXUnSit` es tun):
  MFS 400 K, HFS 800 K/1.44 M mit Forks, HFS+ mit Extent-Overflow,
  APM mit zwei Partitionen, DiskCopy 4.2 mit Prüfsumme, Hybrid-CD.
  Keine Apple-Systemdateien, keine ROM-Inhalte.
- Byte-Vergleiche je Datei (Daten- und Resource-Fork), Katalog-Listings,
  Datums- und Namensprüfungen; Negativfälle: beschädigter B-Baum,
  falsche Prüfsumme, Journal gesetzt, Fremdpartition.
- Jeder neue Handler bekommt einen Gast-Test, der durch die
  `sc`-Naht läuft, plus einen Host-Referenzlauf — dieselbe Regel wie bei
  den Toolbox-Slices.

## Meilensteine

| # | Inhalt | Nutzen | Risiko |
|---|--------|--------|--------|
| M0 | V1: 16 Volumes im Desktop, Typ/RO/Kapazität, `QRXVolumes` | sofort sichtbar, klein | gering |
| M1 | V4.1 + V4.2: Container-Layer + MFS lesen — **erledigt**, `tools/armithlon/macimage/` | erster echter Mac-Pfad | gering |
| M2 | V4.3: HFS lesen mit Forks — **erledigt**, Host-Test ersetzt `QRXMacFSCheck` | klassische Mac-Software vollständig | mittel |
| M3 | V4.4/4.5: HFS+/HFSX lesen, Wrapper — **erledigt**, `tools/armithlon/macimage/qrx_hfsplus.c` | moderne Mac-Images | mittel |
| M4 | V5: AppleSingle/AppleDouble/MacBinary/BinHex **lesen — erledigt** (`qrx_forkfile.c`, `FORK`); Kopieren mit Fork-Erhalt offen | Daten bewegen statt nur ansehen | mittel |
| M5 | V2/V3: Amiga- und PC-Images im selben Modell | „nebeneinander legen" vollständig | gering |
| M6 | HFS-Schreiben hinter Flag, Wechselmedien-Automount, letzte Images merken | Alltag | hoch |

M0 ist bewusst vor die Mac-Arbeit gelegt: Es macht den heutigen Zustand
sichtbar und ist unabhängig von jedem Dateisystem-Handler.

## Grenzen und Nicht-Ziele

- Keine Apple-ROM-, System- oder Installer-Binärdateien im Baum; Images
  kommen vom Nutzer.
- Journaled HFS+ wird nicht replayed. Ein gesetztes Journal bedeutet
  Read-only mit Warnung.
- NDIF wird nur als unkomprimierter Datenfork gelesen. Die `bcem`-Chunkmap
  ist nicht dekodiert; ein Datenfork, der kürzer ist als die dort genannte
  Disk, wird abgelehnt statt als Rohimage missverstanden.
- Kein APFS, kein Time Machine, kein Sparse Bundle, kein Fusion Drive:
  nicht „historisch" und ein eigener Forschungspfad.
- Kein Schreiben auf MFS (das Format ist 1984 eingefroren und der
  Schreibpfad bringt keinem heutigen Anwendungsfall etwas).
- AmigaDOS-Grenzen (31 Zeichen, `:`/`/` verboten, keine echten
  Groß-/Kleinschreibungs-Unterschiede) bleiben bestehen; sie werden
  dokumentiert und beim Kopieren aufgelöst, nicht abgeschafft.
- HFS-Dateien über 2 GB und HFS+-Volumes über 2 TB bleiben außerhalb;
  die 68k-Adressierung setzt dort die Grenze.
- Der HFS-Leser puffert den Katalog bis 64 Einträge (`truncated`-Flag
  darüber), akzeptiert Node-Größen bis 4096 Byte und lehnt einen
  Extents-Baum ab, der selbst über den Overflow-Baum laufen müsste, statt
  die Zuordnung zu raten. Namen werden ohne Rücksicht auf Groß-/
  Kleinschreibung verglichen, weil das Mac-Volume selbst so sortiert.
- HFS+/HFSX: Node-Größen bis 8192 Byte, ebenfalls 64 Katalog-Einträge. Die
  Namensfaltung ist ASCII (echtes HFS+ faltet Unicode); Umlaute werden als
  Latin-1-Byte ausgegeben, Zeichen über U+00FF als `?`. Inline-Attribute
  gelten nicht als Fork — nur `com.apple.ResourceFork`-Fork-Records werden
  gelesen, und nur dann, wenn der Katalog-Record keinen Resource Fork hat.
  Dateien über 4 GiB werden auf 4 GiB geklemmt (68k-Adressierung) und ein
  Journal wird gemeldet, aber nie replayed.
- Fork-Container: BinHex braucht einen Arbeitspuffer (der dekodierte Strom
  liegt dort), die 0x90-Run-Length folgt der üblichen Deutung (Zähler =
  zusätzliche Wiederholungen) und ist nur gegen den eigenen Encoder
  verifiziert. Ein MacBinary-CRC-Fehler wird gemeldet (`crc=bad`), die
  Datei aber nicht abgelehnt; der BinHex-Header-CRC wird nicht geprüft und
  AppleSingle-Einträge außerhalb der Datei führen zur Ablehnung des
  Containers. Sekundäre MacBinary-Header (Byte 127 ≠ 0) werden nicht
  berücksichtigt — die Forks beginnen immer bei 128.

## Offene Entscheidungen

1. **Eigener Handler oder Upstream?** HFS/HFS+ stecken heute in
   `ODFileSystem` (optisch, read-only, Datenfork). Ein eigener
   `L:QRXMacFS` kann Forks und Schreiben, kostet aber Pflege; ein
   Upstream-Patch an `ODFileSystem` teilt die Arbeit, bleibt aber an
   dessen „optical first"-Modell gebunden. Vorschlag: Container-Layer
   und MFS/HFS im eigenen Handler, HFS+ zunächst weiter über
   `ODFileSystem`, bis der eigene Katalog-Pfad steht.
2. **Gast oder Host?** Ein host-seitiger Mac-Image-Leser über die
   ARMithlon-Door wäre schneller zu bauen, würde aber die reine
   Gast-Semantik brechen (HostFS ist bereits so ein Sonderfall). Die
   Roadmap bleibt gast-seitig; der Host liefert nur Testimages.
3. **Wo lebt die Volume-Registry?** `DEVS:Mountlist` plus DOSDrivers ist
   die AmigaDOS-Antwort, aber schreibfaul und ohne Zustand. Eine
   `ENVARC:QRX/volumes`-Liste mit Zuordnung Image → Volume-Name wäre
   robuster und ist Voraussetzung für M6.
