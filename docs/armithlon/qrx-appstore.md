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

## Firefox im Store (QRX OS One 64)

Die erste 64-Bit-App, `arch` = `m68k64`, nur im 64-Bit-Store sichtbar:

| | |
|---|---|
| Datei | `firefox-153.8.0-qrx64-1.tar.xz`, 91.732.384 Bytes, installiert 546,4 MB |
| SHA-256 | `9b6b938b3e83142724dbf40e3274e21fb152c680909e1aa1b665cd3513ca59cc` |
| Adresse | `https://github.com/phoenixkonsole/qrxosone/releases/download/appstore-firefox-153.8.0-1/firefox-153.8.0-qrx64-1.tar.xz` |
| Inhalt | `firefox/`: Programm `firefox` (ELF, libxul gelinkt, ohne Debug-Abschnitte), `omni.ja`, `browser/omni.ja` (mit den AROS-Prefs `qrx-aros.js`), `Fonts/TrueType` (Bitstream Vera, Open Sans), Startskript `Start`, `start.html`, `NOTICE` |

`Start` setzt 2 MB Stack, mountet `PIPE:`, weist `FONTS:` auf die mitgelieferten Schriften (oder hängt sie an ein
vorhandenes `FONTS:` an), legt das Profil in `SYS:Home/Firefox` an (außerhalb des App-Ordners, ein Update behält es),
setzt `TMPDIR`, `HOME` und `MOZ_FORCE_DISABLE_E10S` und öffnet Firefox mit 1000×680 im Fenster auf dem Desktop.

Voraussetzungen: QRX OS One 64 startet per hostfs aus einem Host-Ordner (Profil und Omni-Archive brauchen lange
Dateinamen), 2 GB Gast-RAM.

Neu bauen (Linux, mit dem Gecko-Bau aus `tools/armithlon/spidermonkey`):

```
tools/armithlon/spidermonkey/package-appstore.sh OBJ-BROWSER OUT 1   # reproduzierbar, xz -9 single-threaded
tools/armithlon/appstore/add-to-catalog.sh OUT/firefox-153.8.0-qrx64-1.tar.xz firefox Firefox ...   # + Spalte m68k64
```

**Noch zu tun (von Hand):** Release mit dem Tag `appstore-firefox-153.8.0-1` anlegen und genau diese Datei anhängen.

Geprüft (27. November, x86-64-Engine `qrx_ec64_run`, 2 Kerne, 2 GB, System per hostfs): Archiv über das
Store-Skript von einem lokalen Spiegel installiert (Hash geprüft), dann wie OPEN `Execute APPS:firefox/Start`
auf dem 64-Bit-Desktop mit frischem Profil. Das Fenster öffnet sich mit Tab-Leiste, Adressleiste und der
Startseite und läuft 25 Minuten ohne Absturz ([Screenshot](screenshots/firefox-appstore-qrxos64.png)).
Vorher stürzte Firefox in IndexedDB ab: ein Fehler des m68k64-Compilers bei Stackrahmen über 32 KB
(`tools/armithlon/ec64-llvm/README.md`). Offen: QuotaManager meldet beim ersten Einrichten des Speichers
`NS_ERROR_STORAGE_IOERR`.

Stand: **Vorschau.** Das Fenster nimmt noch keine Maus- und Tastatureingaben an, lässt sich nicht in der Größe ändern
und nicht schließen. Kein offizieller Mozilla-Build; „Firefox“ ist eine Marke der Mozilla Foundation.

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
