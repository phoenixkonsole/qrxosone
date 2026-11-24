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

## Sicherheit

- BusyBox-`wget` prüft keine TLS-Zertifikate. Dass niemand ein manipuliertes Archiv unterschiebt, sichert allein der SHA-256-Vergleich mit dem Katalog.
- Der Katalog kommt mit dem Image und wird **nie** aus dem Netz geladen. Neue Apps oder neue Versionen erfordern also ein neues Image bzw. einen neuen Katalog.
- Nur `https://`-Adressen sind erlaubt. App-IDs müssen `[a-z0-9][a-z0-9._-]*` entsprechen, damit kein Pfad aus `APPS:` hinausführt.
- `QRX_STORE_MIRROR=URL` holt die Archive unter demselben Dateinamen von einem Spiegel, für Tests oder eine Offline-Kopie. Die Prüfung des Hashs bleibt dabei dieselbe.

## Blender in den Store bringen

Das Archiv liegt als Asset an einem GitHub-Release dieses (öffentlichen) Repositories. Der Pi braucht deshalb keinen Token.

1. Blender 2.79b bauen (`tools/blender/`) und den Ordner `blender/` samt Startskript `Blender` (s-Bit gesetzt) packen:
   `tar -cJf blender-2.79b-qrx-1.tar.xz blender`
2. Auf GitHub ein Release mit dem Tag `appstore-blender-2.79b-1` anlegen und die Datei als Asset anhängen.
3. Die Katalogzeile erzeugen und an `catalog.tsv` anhängen:
   ```
   tools/armithlon/appstore/add-to-catalog.sh blender-2.79b-qrx-1.tar.xz blender Blender \
     "3D modelling, animation and rendering" Create 2.79b <installierte Bytes> \
     https://github.com/phoenixkonsole/qrxosone/releases/download/appstore-blender-2.79b-1/blender-2.79b-qrx-1.tar.xz \
     Blender "Blender Foundation, port by QRX" GPL-2.0 "Blender 2.79b with Python 3.5 for QRX OS One: …"
   ```
4. Das Image neu bauen.

Wird das Asset später ersetzt, ändert sich sein Hash und der Store verweigert die Installation, bis der Katalog angepasst ist. Das ist so gewollt.

## Tests (Stand 2026-11-24)

Getestet auf der nativen x86-Engine (`qrx68100_full_run`) mit dem QRX-Systembaum, ein Testarchiv von 3 MB über einen gedrosselten lokalen Spiegel:

- Detailseite, GET: Fortschrittsring und „downloading 1.9 MB of 3.0 MB“ sind sichtbar, danach installiert und OPEN.
- Die Dateien liegen in `APPS:blender`, `.qrx-store` enthält die Version.
- Falscher Hash: „checksum wrong, not installed“, `APPS:` bleibt unverändert.
- Ungültige ID: wird abgewiesen. `store-remove`: entfernt die App.
- Leerer Katalog: „Nothing here yet.“

Noch nicht getestet:

- Der Lauf auf echter Pi-Hardware mit echtem Netz und dem GitHub-Release, denn das Blender-Archiv gibt es noch nicht.
- Eine Ausgabe für QRX OS One 64 (EC64): Die App spricht die Host-Tür über die 32-Bit-Boardadresse an.

Bekannt:

- `RPTAG_FgColor` wird von der Textausgabe dieser AROS-Version ignoriert. Der Text bekam die Farbe eines einzigen Stifts (orange). Die App nimmt deshalb `ObtainBestPen`.
- Einzelne Glyphenspalten der Antialias-Schriften fehlen stellenweise („NSTALLED“). Das ist ein Darstellungsfehler der Schriftausgabe, nicht der App.
