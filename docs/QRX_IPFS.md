# IPFS auf QRX OS One

QRX OS One bekommt einen nativen 68K-IPFS-Client, einen lokalen 68K-Node,
einen nativen libp2p-Peer, ein oeffentliches `IPFS:`-Laufwerk und
verschluesselte Volume-Images im DMG-Stil. `QRXIPFSD` speichert Bloecke
und UnixFS-Verzeichnisse direkt im Gast; `QRXIPFSP2P` spricht
multistream-select, Noise XX, Yamux, Bitswap 1.2 und Kademlia.

## Was gebaut wurde

| Programm | Zweck |
|---|---|
| `C:QRXIPFS` | Fenster mit Knotenstatus, Datei-/Drawer-Upload, Revisionen, Volume-Status und oeffentlicher Warnung |
| `C:QRXIPFSPub` | Kommandozeile: `STATUS`, `ADD`, `CAT`, `LS`, `GET`, `PIN`, `UNPIN`, `SYNC`, `VERSION`, `REPO`, `WARN` |
| `C:QRXIPFSSync` | Hintergrunddienst: veroeffentlicht Aenderungen aus `IPFS:` und schreibt Revisionen |
| `C:QRXIPFSD` | Lokaler Node-Core: Blockstore, CIDv1/UnixFS, Kubo-kompatible HTTP-API und `/ipfs/`-Gateway |
| `C:QRXIPFSP2P` | Nativer Peer: TCP, Noise XX, Yamux, Bitswap 1.2 und Kademlia-DHT; teilt den Blockstore mit `QRXIPFSD` |
| `C:QRXVolImage` | Fenster zum Anlegen, Pruefen, Entpacken und IPFS-Sichern verschluesselter Images |
| `C:QRXVol` | Kommandozeile fuer dieselben Image-Operationen |
| QRX Drive | `IPFS`-Knopf in der Titelleiste, Veroeffentlichungsmenue, PUBLIC-Banner, Start des Volume-Editors |

Quellen:

```
tools/ipfs/qrx_ipfs.[ch]        HTTP-API, Multipart-Upload, Pinning, Revisionen
tools/ipfs/QRXIPFS.c            GUI
tools/ipfs/qrx_ipfs_pub.c       CLI
tools/ipfs/qrx_ipfs_sync.c      Sync-Dienst
tools/ipfs/qrx_ipfs_pub.c       vollständiger CLI-Client (ADD/LS/GET/PIN/UNPIN)
tools/ipfs/QRXIPFSD.c           lokaler Node-Core und HTTP-Server
tools/ipfs/qrx_p2p.[ch]         libp2p, Noise, Yamux, Bitswap, Kademlia
tools/ipfs/QRXIPFSP2P.c         Peer-Prozess und lokaler Kontroll-Endpunkt
tools/ipfs/vendor/              Monocypher 4.0.1 (BSD-2-Clause/CC0)
tools/ipfs/build.sh             Bauen mit dem AROS-SDK
tools/ipfs/install-startup.py   Startup-Sequence idempotent verdrahten
tools/volimage/qrx_crypto.[ch]  AES-256, SHA-256, HMAC, PBKDF2
tools/volimage/qrx_volimage.[ch] Image-Format, Packen, Pruefen, Entpacken
tools/volimage/QRXVolImage.c    GUI
tools/volimage/qrx_vol_cli.c    QRXVol
tools/armithlon/desktop/ipfs-drive.c  QRX-Drive-Integration
```

## Das `IPFS:`-Laufwerk (Assign)

`IPFS:` ist ein Assign auf den lokalen Drawer `SYS:Storage/IPFS`
(kein eigener DOS-Handler). QRX Drive zeigt es als Laufwerk unter
**Locations** und auf dem Desktop; andere Programme erreichen es ueber
den Assign-Pfad. Ordner anlegen, Dateien kopieren, umbenennen und
loeschen funktionieren wie in einem normalen Drawer. Im IPFS-Pfad
erscheint zusaetzlich ein rotes **PUBLIC**-Band.

### IPFS-Funktionen in QRX Drive

Beim Wechsel in das `IPFS:`-Laufwerk zeigt QRX Drive unter dem
PUBLIC-Band eine Statuszeile:

- **gruener Punkt, `IPFS BEREIT | P2P: n`**: `QRXIPFSP2P` laeuft und hat
  die Bootstrap-Peers geladen. Fuer Transfers ist weiterhin eine
  Internetverbindung erforderlich.
- **roter Punkt, `IPFS OFFLINE`**: der P2P-Prozess ist nicht aktiv oder
  die Statusdatei fehlt. Dann zuerst `C:QRXIPFSP2P` beziehungsweise den
  Bootvorgang pruefen.

Die Statusdatei schreibt `QRXIPFSP2P` unter
`IPFS:.qrx-node/p2p-status`. Der Desktop liest sie direkt; er muss dafuer
keinen TCP-Stack in den Filemanager laden.

Im `IPFS:`-Laufwerk stehen zusaetzlich diese Aktionen zur Verfuegung:

- **CID** in der Werkzeugleiste oder **Datei per CID laden...** im
  IPFS-Menue: CID eingeben, Enter; das Ziel wird im aktuellen Ordner
  angelegt. Der Download wird automatisch gepinnt und ueberlebt das
  Loeschen der lokalen Kopie nicht (Pin bleibt bestehen).
- **Objekt pinnen** und **Pin entfernen** im IPFS-Menue: eine Datei oder
  ein Drawer wird ueber `PINPATH` veroeffentlicht und gepinnt bzw. ueber
  `UNPINPATH` entfernt.
- Ein kleines gruenes **Pin-Symbol** markiert gepinnte Dateien und
  Drawer in der Icon- und Listenansicht. Die Zuordnung lokaler Pfade zu
  CIDs liegt in `IPFS:.qrx-node/path-pins`; die massgebliche Pin-Liste
  bleibt `IPFS:.qrx-node/pins`. Unpin haengt dort `-CID` an; der letzte
  Eintrag pro CID entscheidet, damit laufende Leser die Datei nicht
  blockieren.
- **IPFS-Einstellungen...** im IPFS-Menue oder in **QRX Settings** unter
  **IPFS: NETZWERK, GATEWAY UND PINS**: Node-/Peer-Status, Gateway,
  Anzahl gepinnter CIDs, Bootstrap-Peer und der Schalter
  **AUTO-DOWNLOAD**. Die Einstellung wird in
  `SYS:QRX/Prefs/ipfs.cfg` gespeichert und von `QRXIPFSD` beim naechsten
  Start gelesen; ein Neustart des Nodes ist erforderlich.

Die Startup-Sequence legt den Drawer an und setzt das Assign:

```
If NOT EXISTS "SYS:Storage/IPFS"
    MakeDir "SYS:Storage/IPFS"
EndIf
Assign "IPFS:" "SYS:Storage/IPFS"
If EXISTS "C:QRXIPFSD"
    Run >NIL: C:QRXIPFSD
EndIf
If EXISTS "C:QRXIPFSP2P"
    Run >NIL: C:QRXIPFSP2P
EndIf
If EXISTS "C:QRXIPFSSync"
    Run >NIL: C:QRXIPFSSync
EndIf
```

`QRXIPFSD` startet direkt nach dem Assign und wartet beim Boot, bis
`bsdsocket.library` bereit ist. Danach startet `QRXIPFSSync`, wartet auf
`IPFS:` und das Netzwerk und liest den Drawer alle zehn Sekunden. Geaenderte
Dateien gehen ueber `/api/v0/add` an den lokalen Node, werden gepinnt und
erzeugen pro Datei eine Zeile in `IPFS:REVISIONS`:

```
07-Nov-26 14:02:11  Webseite/index.html  bafy...  4211
```

Weil IPFS-Inhalte ueber ihren CID unveraenderlich adressiert sind, ist
jede Zeile eine echte Revision: die alte Fassung bleibt unter ihrem
alten CID abrufbar und gepinnt. Der Sync-Dienst ignoriert versteckte
Dateien und `REVISIONS`; der Node-Blockstore liegt versteckt unter
`IPFS:.qrx-node`.

### Kompletter CLI-Client

`QRXIPFSPub` ist der gastseitige Kommandozeilen-Client. Er arbeitet
entweder gegen `QRXIPFSD` oder gegen einen kompatiblen Kubo-Daemon:

```
C:QRXIPFSPub STATUS
C:QRXIPFSPub ADD   IPFS:Webseite
C:QRXIPFSPub LS    bafy...
C:QRXIPFSPub GET   bafy... RAM:Webseite
C:QRXIPFSPub CAT   bafy... RAM:datei.html
C:QRXIPFSPub PIN   bafy...
C:QRXIPFSPub UNPIN bafy...
C:QRXIPFSPub GETPIN bafy... IPFS:ziel
C:QRXIPFSPub PINCID bafy... IPFS:ziel
C:QRXIPFSPub PINPATH IPFS:datei
C:QRXIPFSPub UNPINPATH IPFS:datei
C:QRXIPFSPub SYNC  IPFS:
C:QRXIPFSPub VERSION
C:QRXIPFSPub REPO
```

`GET` legt bei einem UnixFS-Verzeichnis das Zielverzeichnis an und
schreibt alle Dateien rekursiv; vorhandene Dateien werden ueberschrieben.
`GETPIN` macht dasselbe und pinnt den CID anschliessend. `PINPATH`
veroeffentlicht einen lokalen Pfad und pinnt ihn; `PINCID` pinnt einen
bereits vorhandenen CID und merkt sich den Zielpfad; `UNPINPATH` sucht
den CID in `path-pins` und entfernt den Pin. `LS` zeigt Name, Typ,
Groesse und CID jedes Eintrags. Der Abruf ist auf
32 Verzeichnisebenen und 512 Eintraege pro Verzeichnis begrenzt
(Speicher auf 68K). `CAT` liest genau einen Block; fuer Verzeichnisse
ist `GET` zu verwenden.

### Lokaler Node-Core

`QRXIPFSD` ist die 68K-Node-Anwendung im Gast. Er bietet:

- einen Blockstore unter `IPFS:.qrx-node/blocks` und eine Pin-Liste in
  `IPFS:.qrx-node/pins`;
- CIDv1 fuer Rohdateien (raw) und UnixFS-Verzeichnisse (DAG-PB) mit
  sortierten Links;
- die Kubo-kompatiblen Endpunkte `id`, `version`, `add`, `cat`,
  `ls`, `pin/add`, `pin/ls`, `pin/rm`, `block/get`, `block/stat` und
  `repo/stat` unter `http://127.0.0.1:5001/api/v0/`;
- ein Gateway unter `http://127.0.0.1:5001/ipfs/<cid>[/pfad]`, das
  Verzeichnisse auflistet und `index.html` als Website-Startseite liefert.
  Es bindet auf allen IPv4-Adressen (`INADDR_ANY:5001`), damit Safari
  auf dem Host `http://127.0.0.1:5001/ipfs/<cid>` oeffnen kann.

`QRXIPFSD` bleibt die lokale HTTP-/Gateway-Schnittstelle. Die
Netzwerkfunktionen laufen im separaten Prozess `QRXIPFSP2P`, der
denselben Blockstore benutzt. Der Peer lauscht auf TCP-Port 4001 und
bietet lokal auf `127.0.0.1:5002` die Endpunkte `/status`,
`/fetch?arg=<cid>` und `/provide?arg=<cid>` an. Fehlt ein Block lokal,
ruft `QRXIPFSD` den Peer ueber `/fetch` auf; der Peer holt ihn per
Bitswap und legt ihn im gemeinsamen Store ab.

### Nativer Peer `QRXIPFSP2P`

Der Peer ist eine bewusst synchrone 68K-Implementierung, kein Port von
go-libp2p. Er verarbeitet eine TCP-Verbindung nach der anderen, damit
der Speicherbedarf auf 68K beherrschbar bleibt:

```
TCP -> multistream-select /noise
    -> Noise_XX_25519_ChaChaPoly_SHA256
    -> multistream-select /yamux/1.0.0
    -> /ipfs/bitswap/1.2.0 oder /ipfs/kad/1.0.0
```

Eigenschaften:

- Ed25519-PeerID aus `peer.seed`; die Datei liegt unter
  `IPFS:.qrx-node/peer.seed` und wird beim ersten Start erzeugt.
- Noise XX mit der libp2p-Payload (`identity_key`, `identity_sig`),
  ChaCha20-Poly1305-IETF und 2-Byte-Framing.
- Yamux-Streams mit SYN/ACK/FIN/RST, 16-KiB-Datenrahmen,
  256-KiB-Empfangsfenster und Window-Updates.
- Bitswap 1.2 mit Wantlist, `sendDontHave`, Block-Payload und
  Cancel; eingehende Wuensche werden aus dem lokalen Blockstore bedient.
  Der Client akzeptiert auch vom Peer neu geoeffnete Antwort-Streams,
  wie sie go-libp2p fuer Blockantworten verwendet.
- TCP und DNS laufen im Gast; der Host stellt nur die Socket-Door
  bereit. IPFS, Noise, Yamux, Bitswap und DHT bleiben vollstaendig im
  68K-Gast.
- Kademlia `FIND_NODE`, `GET_PROVIDERS` und `ADD_PROVIDER`; die
  Provider-Liste wird im Speicher gehalten.
- Bootstrap-Adressen stehen zeilenweise in
  `IPFS:.qrx-node/bootstrap` oder in `QRX_IPFS_BOOTSTRAP` (mit Komma
  oder Semikolon getrennt), zum Beispiel:

  ```
  /ip4/192.168.1.20/tcp/4001/p2p/12D3KooW...
  ```

- `QRX_IPFS_ANNOUNCE` setzt die IPv4-Adresse, die der Peer im DHT
  ankündigt (Standard `127.0.0.1`). Fuer zwei Rechner im LAN muss hier
  die LAN-Adresse des Gasts stehen.

Der Peer pinnt nicht selbst und startet keine GC. `QRXIPFSP2P` liest
die Pin-Liste von `QRXIPFSD` und kuendigt die ersten vier Pins beim
Tick an. Fehlt ein Block lokal, ruft `QRXIPFSD` den Peer ueber
`/fetch?arg=<cid>` auf und liest den Block nach dem Abruf aus dem
gemeinsamen Store.

Der Peer-Code wird mit dem AROS-SDK gebaut. Der direkte Laufzeittest
im ARMithlon-Gast gegen den oeffentlichen libp2p-Peer `46.224.1.55:4001`
ist erfolgreich: Noise XX, Yamux, Bitswap und der Abruf von
`QmT78zSuBmuS4z925WZfrqQ1qHaJ56DQaTfyMUF7F8ff5o` funktionieren ohne
Host-Kubo. Ein Test gegen einen lokalen Kubo-Daemon und ein
Mehrpeer-Test stehen noch aus. Ein Upload ist weiterhin auf 16 MiB und
512 Dateien pro Vorgang begrenzt.

### Veroeffentlichen aus QRX Drive

Der `IPFS`-Knopf sitzt in der Titelleiste des Dateimanagers:

1. **In IPFS veroeffentlichen** - ausgewaehlte Datei oder aktueller Drawer.
2. **Als Webseite veroeffentlichen** - aktueller Drawer rekursiv; der
   zurueckgegebene CID ist der Website-Root (Gateway oder DNSLink davor).
3. **Verschluesseltes Volume...** - startet `QRXVolImage` mit dem
   ausgewaehlten Pfad als Quelle.
4. **IPFS Anwendung oeffnen** - startet `QRXIPFS`.

Vor jedem Upload erscheint die Warnung im Fenster. Sie ist kein
einmaliger Dialog: Dateien, die nach IPFS gehen, sind weltweit lesbar,
behalten ihren CID und lassen sich nicht durch lokales Loeschen
zuruecknehmen. Es gibt keine Zugriffsbeschraenkung in IPFS.

## Verschluesselte Volume-Images

`QRXVolImage` erzeugt eine einzelne Datei `*.qrxvol`, die einen ganzen
Drawer enthaelt und mit **Benutzername + Passwort** geschuetzt ist. Das
Format ist bewusst ohne externe Kryptobibliothek implementiert, weil das
AROS-68K-SDK keine enthaelt:

```
Header (96 Byte)  "QRXVOL1", Version, Iterationen, Benutzername,
                  Salt (16), IV (16), Nutzlastlaenge, Zeitstempel
Nutzername        klar, damit das Image seinen Benutzer nennen kann
Nutzlast          AES-256-CTR-verschluesselte QPAK-Datei
HMAC-SHA256       ueber Header, Benutzername und Ciphertext
```

Schluesselableitung:

```
enc_key || mac_key = PBKDF2-HMAC-SHA256(benutzer || 0x00 || passwort,
                                       salt, 12000, 64 Byte)
```

Eigenschaften:

- Der Benutzername ist im Klartext, das Passwort **nie**.
- AES-256-CTR verschluesselt die Nutzlast; HMAC-SHA256 erkennt falsches
  Passwort und jede Manipulation, bevor entpackt wird.
- Entpacken prueft zuerst den HMAC und lehnt Pfade mit `..`, `/` oder
  `:` ab (kein Ausbruch aus dem Zielordner).
- Leere Ordner werden als Verzeichniseintrag mitgespeichert; Dateien
  und Ordner bleiben erhalten. Versteckte Eintraege werden uebersprungen.
- Das Image kann als normale Datei kopiert und in IPFS gesichert werden.
  Der Inhalt bleibt auch dann verschluesselt, wenn das Image oeffentlich
  abrufbar ist.

Kommandozeile:

```
C:QRXVol CREATE  IPFS:backup.qrxvol QRX:Documents -user pascal
C:QRXVol INFO    IPFS:backup.qrxvol
C:QRXVol VERIFY  IPFS:backup.qrxvol -user pascal
C:QRXVol EXTRACT IPFS:backup.qrxvol RAM:restore -user pascal
```

Das Passwort kommt aus `QRXVOL_PASSWORD` (so startet es die GUI) oder
wird auf der Konsole abgefragt. In der GUI: Felder ausfuellen, `ERSTELLEN`,
`OEFFNEN`, `PRUEFEN` oder `IN IPFS`.

## Bauen und installieren

```bash
tools/ipfs/build.sh
```

Das Skript nutzt `.build/qrx-sdk/bin/aros-gcc` und `elf2hunk`, baut alle
sieben Programme nach `.build/qrx-ipfs` und kopiert sie in den aktiven
ARMithlon-Systembaum `.build/bundle/SystemFull` (`C` und `QRX/C`).
`ARMithlon.app/Contents/MacOS/launch` startet genau diesen Baum; es wird
kein zweiter Bundle-Baum angelegt. Der Desktop wird mit
`tools/armithlon/build-desktop.py` gebaut, bindet
`tools/armithlon/desktop/ipfs-drive.c` ein und wird nach
`SystemFull/QRX/C/QRXDesktop` installiert.

Voraussetzungen im Gast:

1. Fuer den lokalen Betrieb ist kein Host-Daemon noetig: `QRXIPFSD`
   lauscht auf `127.0.0.1:5001`, und `QRXIPFS`/`QRXIPFSSync` sprechen
   diese API an. Abweichender API-Port fuer den Client:
   `QRX_IPFS_API=host:port`; der Node selbst liest
   `QRX_IPFS_NODE_PORT`.
2. Fuer das oeffentliche Peer-Netz optional Kubo starten. Bildet der
   ARMithlon-Gast 127.0.0.1 nicht auf den Mac ab, die LAN-Adresse des
   Macs eintragen und Kubo nur dort lauschen lassen, nicht ungeschuetzt
   im Internet. Dann muss der lokale `QRXIPFSD` auf einem anderen Port
   laufen oder beendet werden, weil beide Port 5001 belegen.
3. `C:QRXIPFSPub`, `C:QRXIPFSD`, `C:QRXIPFSP2P` und `C:QRXVolImage`
   liegen im Pfad (bzw. unter `QRX:C/`). TCP-Port 4001 muss frei sein;
   der Kontroll-Port 5002 bleibt auf `127.0.0.1` beschraenkt.
4. Fuer das Laufwerk: `Assign IPFS: SYS:Storage/IPFS` aus der
   Startup-Sequence. `QRXIPFS` und `QRXIPFSSync` legen den Drawer und
   das Assign auch selbst an, falls es fehlt.

## Grenzen und Sicherheit

- `QRXIPFSD` ist der lokale HTTP-Node-Core; `QRXIPFSP2P` ist der
  Netzwerk-Peer. Ohne Bootstrap-Peer bleiben fremde CIDs unerreichbar.
  Der Peer sammelt Bloecke per Bitswap ein; bei einem fehlenden Block
  ruft `QRXIPFSD` ihn automatisch ueber den lokalen Kontroll-Endpunkt auf.
- Der Peer verarbeitet eine TCP-Verbindung gleichzeitig und hat keine
  Relay-/Circuit-Relay-Unterstuetzung. IPv4 und direkte TCP-Adressen
  sind der getestete Pfad; IPv6, DNS-Advertisements und QUIC fehlen.
- Es gibt noch keine Garbage Collection. Bloecke bleiben im lokalen
  Repo, auch wenn eine Datei im Drawer geloescht wird; das ist fuer
  Revisionen gewollt, kostet aber Platz.
- Ein Upload ist auf 512 Dateien pro Vorgang begrenzt (Speicher auf
  68K); groessere Baeume in Teilen veroeffentlichen.
- Leere Ordner werden beim Upload nicht abgebildet.
- Pinning ist lokal. Ob ein anderer Knoten die Daten behaelt, haengt
  vom Netz ab; fuer dauerhafte Archivierung mehrere Pins/Provider
  verwenden.
- Die Verschluesselung schuetzt den Inhalt des Images, nicht die
  Tatsache, dass ein Image existiert. Benutzername und Groesse sind
  sichtbar. Das Passwort nicht in Skripten oder Tooltypes ablegen.
- Das Image wird entpackt, nicht als Live-Volume gemountet. Ein
  entsperrtes Image liegt danach als normale Dateien im Zielordner und
  muss dort geschuetzt werden.
- Metadaten (Dateinamen im Image) sind Teil der verschluesselten
  Nutzlast; der Benutzername im Header ist es nicht.

## Kubo (optional)

Kubo ist die Referenzimplementierung eines IPFS-Knotens in Go. Er ist
fuer Interop-Tests und als vollstaendiger Relay/QUIC-Peer weiterhin
nuetzlich, aber nicht mehr Voraussetzung fuer den lokalen 68K-Peer.

`QRXIPFSD` liefert die API-Oberflaeche fuer `add`, `cat`, `ls`, `pin`,
`id` und `repo/stat`; `QRXIPFSP2P` liefert den Netzwerk-Transport. Die
68K-Programme koennen entweder gegen `QRXIPFSD` oder gegen Kubo laufen;
beide belegen standardmaessig Port 5001 und duerfen nicht gleichzeitig
gebunden werden. Der Peer belegt zusaetzlich TCP 4001.

## Roadmap

Stand 2026-11-07. Die Node-Core-Phase A ist umgesetzt; die verbleibenden
Punkte sind bewusst noch nicht implementiert und gehoeren nicht zur
aktuellen Auslieferung.

| # | Punkt | Ziel | Abnahmekriterien |
|---|---|---|---|
| 1 | Echter AROS-DOS-Handler fuer `IPFS:` | Eigenes Volume statt Assign auf `SYS:Storage/IPFS` | `Info IPFS:` meldet ein Volume; Lock/Examine/ExNext/Open/Read/Write/Seek/Close/CreateDir/Delete/Rename funktionieren; QRX Drive und Shell kommen ohne Sonderpfad aus |
| 2 | Live-Mounting fuer `.qrxvol` | Image entsperren, als temporaeres Lese-/Schreib-Volume mounten, beim Unmount wieder verschluesselt schreiben | Mount/Unmount, falsches Passwort, Read-only, Absturz/Recovery und keine Klartextreste auf Platte |
| 3 | Vollwertiger 68K-IPFS-Node | Node im Gast ohne Host-Kubo: Blockstore, UnixFS, libp2p, Bitswap, DHT | Zwei Nodes tauschen Bloecke; add/cat/pin im Gast; Gateway; kein Host-Daemon noetig |

### 1. Echter AROS-DOS-Handler fuer `IPFS:`

Heute ist `IPFS:` ein Assign. Der Handler muss ein eigenes DosPacket-
Volume bereitstellen und die DOS-Operationen inklusive Fehlercodes
abbilden. Der Sync darf nicht mehr pollen, sondern muss an
Schreibvorgaengen des Handlers ansetzen und Revisionen erzeugen.
Besonders zu klaeren sind Lock-Lebensdauer, Speicherverwaltung auf
68K, Verhalten bei fehlendem Netzwerk und die Abgrenzung zum lokalen
Drawer. Das ist ein eigener Systembaustein, keine UI-Erweiterung.

### 2. Live-Mounting fuer `.qrxvol`

Heute wird ein Image nach der HMAC-Pruefung entpackt; es gibt kein
Live-Volume. Fuer echtes Mounten muss das Format random-access-faehig
werden: AES-256-CTR kann springen, aber das aktuelle HMAC deckt die
ganze Nutzlast ab. Fuer Schreibbetrieb ist daher ein chunked MAC oder
ein Format v2 noetig; alternativ muss beim Unmount die gesamte Nutzlast
neu verschluesselt und authentifiziert werden. Zusaetzlich sind
Read-only-Mount, Absturz-Recovery, Schutz vor Klartextresten und die
Performance auf 68K zu loesen.

### 3. Vollwertiger 68K-IPFS-Node

Kubo ist in Go geschrieben und laesst sich nicht als 68K-Programm
uebersetzen. Der lokale Node-Core steht seit 2026-11-07 als
`C:QRXIPFSD`; die Netzwerkphasen muessen darauf aufbauen:

- **A - Node-Core:** lokaler Blockstore, CIDv1/UnixFS, HTTP-API
  (`add`, `cat`, `pin`, `id`, `repo/stat`) und Gateway. **Umgesetzt als
  `C:QRXIPFSD`:** `QRXIPFS` laeuft damit ohne Host-Kubo, aber noch ohne
  Peers.
- **B - Transport:** TCP, multistream-select, Noise XX und Yamux sind
  in `qrx_p2p.c` implementiert und im ARMithlon-Gast gegen einen
  oeffentlichen libp2p-Peer getestet.
- **C - Exchange:** Bitswap 1.2 mit Wantlist und Block-Transfer ist
  implementiert; der Blockabruf von einem oeffentlichen Peer und das
  Gateway sind getestet.
- **D - Routing:** Kademlia `FIND_NODE`/`GET_PROVIDERS`/`ADD_PROVIDER`
  und ein Bootstrap-Peerstore sind implementiert; mDNS fehlt.
- **E - Betrieb:** GC/Pinning, Repo-Migration, Metriken, Neustart,
  Ueberlastschutz auf 68K.

Abnahme: Zwei QRX-Nodes finden sich, tauschen einen CID aus, und ein
`add` im Gast liefert einen CID, den der andere per `cat` liest - ohne
Kubo auf dem Host. Der Host-Kubo ist fuer den getesteten Pfad nicht
mehr erforderlich; ein Test zwischen zwei QRX-Nodes steht noch aus.

## Stand der Pruefung

Die sieben 68K-Programme und der gepatchte Desktop wurden mit dem
AROS-SDK uebersetzt (`tools/ipfs/build.sh`,
`tools/armithlon/build-desktop.py`). Die Krypto-Implementierung wurde
auf dem Host gegen NIST-AES-256, SHA-256 (`abc`), HMAC-SHA256
(RFC 4231) und PBKDF2-HMAC-SHA256 sowie einen CTR-Roundtrip geprueft:
alle Vektoren bestehen.

Der Boot-Test im ARMithlon-Gast, der HTTP-/Gateway-Test gegen
`QRXIPFSD` und der direkte P2P-/Bitswap-Abruf eines fremden CID von
einem oeffentlichen libp2p-Peer sind erfolgreich. Der automatische
Abruf beim Gateway-Aufruf eines lokal fehlenden Blocks wurde ebenfalls
getestet. Ein Test gegen einen lokalen Kubo-Daemon, der
ASL-Dateirequester, der Hintergrund-Sync und die Darstellung auf dem
QRX-Pub-Screen stehen noch aus. `QRXIPFSP2P` wurde mit `-Oz -m68000`
gebaut, damit die grosse Code-Hunk ohne unaufloesbare
RELRELOC32-Relocations auskommt; das erzeugte Hunk wurde mit `file` als
AmigaOS-loadseg()-Binary erkannt.
Der gepatchte Desktop liegt nach dem Build in
`SystemFull/QRX/C/QRXDesktop`; das ist der Baum, den
`ARMithlon.app/Contents/MacOS/launch` startet.

Der `NODE`-Knopf in `QRXIPFS` startet `C:QRXIPFSD`, falls der Node beim
Boot noch nicht lief; der `IPFS`-Knopf in QRX Drive startet weiterhin
`QRXIPFS`. Der Gateway ist getestet; Laufzeittests fuer `LS`, `GET`
und `UNPIN` stehen noch aus.
