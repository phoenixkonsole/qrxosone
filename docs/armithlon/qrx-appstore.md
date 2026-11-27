# QRX App Store (Raspberry-Pi-Image)

Große Programme wie Blender sind nicht mehr Teil des Images. Der Nutzer lädt sie bei Bedarf über
den **App Store** (`C:QRXAppStore`, im Home-Menü des Desktops unter „App Store“) und installiert
sie nach `APPS:<id>`, also auf dem Pi nach `/data/Apps/<id>`.

## Aufbau

| Teil | Datei | Aufgabe |
|---|---|---|
| Oberfläche (68k, AROS) | `tools/armithlon/appstore/QRXAppStore.c` | App im Stil des Apple App Store: Seitenleiste mit Suche und Rubriken (Discover, Create, Work, Play, Develop, Updates), großer Titel, Featured-Karte, Liste mit GET/OPEN/UPDATE-Knöpfen, Detailseite mit Info-Leiste, Fortschrittsring beim Laden |
| Host-Seite (BusyBox) | `tools/armithlon/pi/image/rootfs/qrx/appstore/store` | `store-catalog`, `store-get`, `store-status`, `store-remove`; Aufruf über den Host-Dienst 11 der Engine (`/qrx/netctl store-…`) |
| Katalog | `tools/armithlon/pi/image/rootfs/qrx/appstore/catalog.tsv` | eine Zeile pro App, mit fest eingetragenem SHA-256 des Archivs |
| Katalogzeile erzeugen | `tools/armithlon/appstore/add-to-catalog.sh` | misst Größe und SHA-256 eines Archivs und gibt die Zeile aus |

Der Ablauf von GET:

1. `store-get` startet einen Download im Hintergrund (BusyBox `wget`).
2. Der SHA-256 der Datei wird mit dem Katalog verglichen. Weicht er ab, wird nichts installiert („checksum wrong, not installed“).
3. Das Archiv wird mit `xz` und `tar` nach `.store/<id>.new` entpackt und danach als `APPS:<id>` an seinen Platz verschoben.
4. Die App fragt zweimal pro Sekunde `store-status` ab und zeichnet dann nur den Fortschrittsbereich neu.

Mit OPEN startet die App die im Katalog genannte Startdatei. Ein Skript mit gesetztem s-Bit (zum Beispiel Blenders Startskript, das die Python-Pfade setzt) läuft über `Execute`.

## QRX OS One 64 (EC64)

Dieselbe Quelle läuft als m68k64-Programm, gebaut mit dem m68k64-Clang. `build-qrxos64.sh` baut die App mit, wenn
`QRX_CC64` gesetzt ist. Die Tür zum Host ist im 64-Bit-System die Boardadresse `0xFFFFFFFF00000400` statt
`0xFF00088C`, der Dienst 11 und damit `/qrx/netctl store-…` sind dieselben.

Der Katalog hat eine 14. Spalte `arch`: `m68k` für QRX OS One (die Vorgabe, wenn sie fehlt) und `m68k64` für
QRX OS One 64. Jede Fassung der App zeigt nur Apps, die ihr System auch starten kann. Blender ist ein 32-Bit-Programm,
im 64-Bit-Store erscheint es deshalb nicht. Die erste 64-Bit-App im Katalog ist Firefox (siehe unten).

Geprüft (x86-64-Engine `qrx_ec64_run`, 2 Kerne, mit dem 64-Bit-Desktop): Das Fenster öffnet sich auf dem Desktop, der
Katalog kommt über den Host-Dienst. Mit einem Testkatalog aus einem 32- und einem 64-Bit-Eintrag zeigt die 64-Bit-App
nur den 64-Bit-Eintrag und die 32-Bit-App nur Blender.

Grenzen:

- Das 64-Bit-System hat keine Antialias-Schriften (keine `Fonts`, keine `freetype2.library`). Die App schreibt in `topaz`.
- Installiert wird nur, wenn das 64-Bit-System per hostfs aus einem Host-Ordner startet (auf dem Pi `/data/qrx64`
  mit `AROS.boot`, Kernelmodul `qrxsys`). Dann setzt `/qrx/session` `QRX_APPS_DIR=/data/qrx64/Apps`, und die
  Startup-Sequence weist `APPS:` auf `SYS:Apps`. Mit dem alten Image `QRXOS64.img` gibt es kein `APPS:`.

## Sicherheit

- BusyBox-`wget` prüft keine TLS-Zertifikate. Dass niemand ein manipuliertes Archiv unterschiebt, sichert allein der SHA-256-Vergleich mit dem Katalog.
- Der Katalog kommt mit dem Image und wird **nie** aus dem Netz geladen. Neue Apps oder neue Versionen erfordern also ein neues Image bzw. einen neuen Katalog.
- Nur `https://`-Adressen sind erlaubt. App-IDs müssen `[a-z0-9][a-z0-9._-]*` entsprechen, damit kein Pfad aus `APPS:` hinausführt.
- `QRX_STORE_MIRROR=URL` holt die Archive unter demselben Dateinamen von einem Spiegel, für Tests oder eine Offline-Kopie. Die Prüfung des Hashs bleibt dabei dieselbe.

## Blender im Store

Das Archiv liegt als Asset an einem GitHub-Release dieses (öffentlichen) Repositories. Der Pi braucht deshalb keinen Token.
Der Katalog im Image enthält die Zeile schon:

| | |
|---|---|
| Datei | `blender-2.79b-qrx-1.tar.xz`, 10.972.976 Bytes, installiert 40,8 MB |
| SHA-256 | `2b3748cb948e82c727d2b36aeff2fd9ca3e7733dd24a63597dd635ba82f6e1c5` |
| Adresse | `https://github.com/phoenixkonsole/qrxosone/releases/download/appstore-blender-2.79b-1/blender-2.79b-qrx-1.tar.xz` |
| Inhalt | `blender/`: Programm `blender` (Hunk), Startskript `Start`, `lib/python3.5`, `2.79/scripts` mit den Standard-Add-ons, Lizenzen |

**Noch zu tun (von Hand, ich habe kein Werkzeug für Releases):** Auf GitHub ein Release mit dem Tag
`appstore-blender-2.79b-1` anlegen und genau diese Datei als Asset anhängen. Bis dahin meldet der Store
„download failed“ und installiert nichts.

Neu bauen und packen, auf Linux oder dem Mac (`tools/blender/qrxenv.py` findet die Werkzeuge):

```
tools/blender/build.sh                       # Blender, Python 3.5, libstdc++ der eigenen GCC-Version
tools/blender/build_python.sh
tools/blender/package-appstore.sh OUT 1      # reproduzierbar: gleicher Inhalt, gleicher SHA-256
tools/armithlon/appstore/add-to-catalog.sh OUT/blender-2.79b-qrx-1.tar.xz blender Blender ...
```

Eine neue Version bekommt eine neue Revision (`-2`), ein neues Release und eine neue Katalogzeile. Wird ein Asset
ersetzt, ändert sich sein Hash, und der Store verweigert die Installation, bis der Katalog angepasst ist. Das ist so gewollt.

Das Startskript heißt `Start` und nicht `Blender`, weil AROS bei Namen nicht zwischen Groß- und Kleinschreibung
unterscheidet: Es wäre dieselbe Datei wie das Programm `blender`. Es holt den Speicher über 2 GB (`QRXMoreMemory`),
setzt 8 MB Stack und wechselt nach `APPS:blender`, wo Blender seine Skripte sucht. Die App erkennt ein Skript am
fehlenden Hunk-Kopf (`0x3F3`) und startet es mit `Execute`. Das s-Bit übersteht das Entpacken auf dem Dateisystem des
Pi nicht.

## ThunderCat for AROS im Store (QRX OS One 64)

ThunderCat for AROS © 2026 by Pascal Papara, based on Firefox: der Browser für QRX OS One 64, gebaut aus
dem Quelltext von Firefox 153.8.0 mit den QRX-Patches (`tools/armithlon/spidermonkey`). `arch` = `m68k64`,
nur im 64-Bit-Store sichtbar.

| | |
|---|---|
| Datei | `thundercat-153.8.0-qrx64-1.tar.xz`, 92.159.372 Bytes, installiert 547,2 MB |
| SHA-256 | `def34cd83e470c1e333100e91aa673960779c4358c8b2b5388df352b94df500f` |
| Adresse | `https://github.com/phoenixkonsole/qrxosone/releases/download/appstore-thundercat-153.8.0-1/thundercat-153.8.0-qrx64-1.tar.xz` |
| Inhalt | `thundercat/`: Programm `ThunderCat`, `omni.ja`, `browser/omni.ja`, `Fonts/TrueType`, Startskript `Start` (optional eine Adresse: `Execute APPS:thundercat/Start URL`), `start.html`, `logo.png`, AROS-PNG-Icons, `NOTICE` |

`Start` setzt 2 MB Stack, mountet `PIPE:`, weist `FONTS:` auf die mitgelieferten Schriften, legt das Profil in
`SYS:Home/ThunderCat` an und startet ThunderCat. Das erste Fenster ist 1000×700 groß und lässt den Desktop
frei; eine selbst gewählte Größe merkt sich ThunderCat. Maus, Rad, Tastatur, Popups (Menüs, Vorschläge der
Adressleiste), Fenstergröße und Schließen gehen über Intuition.

Voraussetzungen: QRX OS One 64 startet per hostfs aus einem Host-Ordner, 2 GB Gast-RAM, und die
68100EC64-Engine mit dem Gleitkommabefehl `$AFnn` (siehe [68100ec64.md](68100ec64.md#host-floating-point-afnn)).
ThunderCat rechnet damit Gleitkomma auf dem Host; auf einer älteren Engine bricht es beim ersten Gleitkommabefehl
ab. Der AROS-Kern braucht den Stand mit der Port-Sperre und dem pthread-Fix (`aros-5b5fd4c-qrx-m68k64.patch`).

Gemessen (Host x86-64, 2 Kerne, Engine mit JIT): qrx.rocks ist nach dem Start von ThunderCat in weniger als
600 s vollständig dargestellt. Vorher hing die Seite: Soft-Float kostete etwa 30 % der Zeit, und zwei
AROS-Fehler (Signale an beendete Threads, Port-Sperre in uninitialisiertem Speicher) brachten das System zum
Stehen.

Neu bauen: `tools/armithlon/spidermonkey/branding/install-branding.sh <Quellbaum>`, Gecko bauen, dann
`tools/armithlon/spidermonkey/package-appstore.sh OBJ-BROWSER OUT 1` und
`tools/armithlon/appstore/add-to-catalog.sh` (dazu die Spalte `m68k64`).

**Noch zu tun (von Hand):** Release mit dem Tag `appstore-thundercat-153.8.0-1` anlegen und genau diese Datei
anhängen. Das frühere Firefox-Archiv (`appstore-firefox-153.8.0-1`) ist durch ThunderCat ersetzt.

Kein Mozilla-Produkt; „Firefox“ ist eine Marke der Mozilla Foundation.

## Tests (Stand 2026-11-24)

Getestet auf der nativen x86-Engine (`qrx68100_full_run`) mit dem QRX-Systembaum, ein Testarchiv von 3 MB über einen gedrosselten lokalen Spiegel:

- Detailseite, GET: Fortschrittsring und „downloading 1.9 MB of 3.0 MB“ sind sichtbar, danach installiert und OPEN.
- Die Dateien liegen in `APPS:blender`, `.qrx-store` enthält die Version.
- Falscher Hash: „checksum wrong, not installed“, `APPS:` bleibt unverändert.
- Ungültige ID: wird abgewiesen. `store-remove`: entfernt die App.
- Leerer Katalog: „Nothing here yet.“

Blender aus dem Archiv (Linux-Bau, 24. November):

- Store-Skript mit dem echten Archiv über einen lokalen Spiegel: Hash geprüft, nach `apps/blender` installiert, `.qrx-store` = `2.79b`.
- Im Gast mit der **Pi-Engine** (aarch64-Build, JIT, Kern 1) unter qemu-aarch64: `APPS:blender/blender -b` aus dem
  installierten Ordner lädt seine Skripte aus `APPS:blender/2.79/scripts`. `bpy` arbeitet, die Startszene wird gerendert
  (4 von 6 Kacheln auf Kern 1) und als PNG gespeichert.

Noch nicht getestet:

- Blender mit Oberfläche auf dem Pi: Die GL-Tür der Pi-Engine nutzt EGL (Mesa V3D). Hier gibt es keine GPU, auf dem Gerät ist das offen.
- Der Lauf auf echter Pi-Hardware mit echtem Netz und dem GitHub-Release (das Release ist noch nicht angelegt).

Bekannt:

- `RPTAG_FgColor` wird von der Textausgabe dieser AROS-Version ignoriert. Der Text bekam die Farbe eines einzigen Stifts (orange). Die App nimmt deshalb `ObtainBestPen`.
- Einzelne Glyphenspalten der Antialias-Schriften fehlen stellenweise („NSTALLED“). Das ist ein Darstellungsfehler der Schriftausgabe, nicht der App.
