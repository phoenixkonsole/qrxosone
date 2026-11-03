# ARMITHLON 2.4 – QRXDesktop-Doppelklick für Amiga-HUNK via QRX68/1

## Was diese Version tatsächlich liefert

**NEU:** Ein AROS/m68k-seitiger `QRX68Run`-Starter (Quellcode), ein sicher begrenzter
Hostdienst `qrx68_service.py`, ein gemeinsamer `QRXBRIDGE:`-Verzeichnisdatenträger,
ein nichtdestruktiver Installationshelfer und ein versionsgesicherter Patch für
den **existierenden QRXDesktop 6.6.20**. Die vorhandenen AmigaOS-3.x- und
AROS-Anwendungsrouten, QRX86 PE32/ELF32, JIT und andere Bausteine bleiben
unverändert erhalten.

**Bereits direkt in QRXDesktop 6.6.20 vorhandene Funktion:** Ein Amiga-Icon vom
Typ **WBPROJECT** mit `DefaultTool=C:QRX68Run` startet die neue Laufzeit via
`SystemTags()`; dafür ist **kein Umbau des bestehenden QRXDesktop nötig!**
Der optionale Quellpatch ermöglicht alternativ `QRXMODE=QRX68` (ToolType) und
Dateien mit `.qrx68`-Endung. Die standardmäßige native Ausführung normaler
AROS-68k-HUNK-Dateien bleibt dabei erhalten.

## Architektur

```
AROS / QRXDesktop (im FS-UAE-Amiga-Guest)
      Doppelklick / Icon DefaultTool=C:QRX68Run
                         |
              AROS/m68k QRX68Run
                 | HUNK-Datei kopieren, eindeutige ID
                 v
           QRXBRIDGE:inbox/   [gemountetes echtes Mac-Verzeichnis]
                 |
       QRX68/1 Python-Hostdienst
                 | prüft HUNK-Magic, Größe, Dateiname
                 v
       qrx68-musashi-run --experimental-gui
                 | Exec / DOS / Intuition / Graphics HLE (Teilmenge)
                 v
    QRXBRIDGE:results/ ID.out, ID.err, ID.ppm, ID.status
                 |
             AROS/m68k QRX68Run
                 v
        Intuition-AROS-Fenster (Text oder PPM-Bild)
```

Das ist ein **Gast/Host-Prozess-Bridgepfad unter FS-UAE**. Es ist **kein nativer
AROS-Boot auf QRX68100**, kein vollwertiger AROS-Exec-Dienst und keine
vollständige AmigaOS-3.x-API- oder Hardwarekompatibilität. Der Hostdienst führt
die vollständige Musashi-Laufzeit **nur dann aus, wenn diese auch gebaut wurde**.

## Schnellstart auf dem M1

Benötigt: Xcode Command Line Tools, CMake, Python 3, Git, FS-UAE, Musashi und
einen AROS/m68k-Cross-Compiler inkl. DOS-, Intuition- und CyberGraphX-Headern.
Das entsprechende AROS-Systemverzeichnis (mit `S/`) und QRXDesktop müssen
vorhanden sein. **Keiner dieser Gäste oder das AROS SDK ist im ZIP enthalten.**

```bash
cd ~/Downloads/ARMITHLON-2.4-macOS
./armithlon build
./armithlon test
./armithlon full68k-setup
./armithlon full68k-build
./armithlon full68k-test
```

### 1. AROS-Gaststarter bauen

Mit deinem echten AROS-68k-Cross-Compiler und zum SDK passenden Flags:

```bash
export QRX_AROS_CC=/PFAD/ZUM/m68k-aros-gcc
export QRX_AROS_CFLAGS='-I/PFAD/ZUM/AROS-SDK/include -L/PFAD/ZUM/AROS-SDK/lib'
./armithlon qrx68-aros-build
```

`data/qrx68/guest/QRX68Run` muss ein **Amiga-HUNK-Programm** sein, nicht ELF,
Linux-Binary oder Mach-O. Das Buildszenario ist abhängig vom vorhandenen AROS-SDK
und konnte in dieser Umgebung noch **nicht real cross-kompiliert** werden.

### 2. Starter in ein vorhandenes AROS-Systemverzeichnis installieren

```bash
./armithlon qrx68-aros-install /PFAD/EXTRAHIERTES-AROS-SYS
```

Der Installer schreibt `C/QRX68Run` sowie `QRX/C/QRX68Run`, **wenn dessen
Unterverzeichnis bereits existiert**. Er überschreibt kein fremdes Programm
und verändert weder `S/User-Startup` noch das originale QRXDesktop.

### 3. QRX-Dienst und AROS gemeinsam starten

```bash
./armithlon fetch
./armithlon qrx68-aros-start /PFAD/EXTRAHIERTES-AROS-SYS
```

Der Dienst erzeugt `~/QRX68-Bridge/inbox` und `~/QRX68-Bridge/results`.
FS-UAE bindet den Ordner zusätzlich als **`QRXBRIDGE:`** ein; das übergebene
AROS-Systemverzeichnis erscheint als **`AROSSYS:`**. Der verwendete Amiga muss
von einem mit QRXDesktop ausgestatteten AROS-System starten. Der AROS-LiveCD
alleine enthält **nicht automatisch** den installierten QRXDesktop oder dessen
QRX68-Icon-Zuordnungen.

Alternativ kann der Dienst getrennt gestartet werden:

```bash
./armithlon qrx68-service
# anderer Prozess: QRX68_BRIDGE_DIR="$HOME/QRX68-Bridge" ./armithlon run-aros
```

Für einen explizit **unvollständigen Entwicklungs-CPU-Fallback**:

```bash
QRX68_RUNTIME="$PWD/build-qrx68100/qrx68-run" ./armithlon qrx68-service
```

Dieser Fallback beherrscht nur eine begrenzte 68k-Instruktionsmenge; die
Kompatibilitätsaussagen gelten nur für die tatsächlich ausgeführten Funktionen.

### 4. Amiga-Anwendung per Doppelklick zuweisen

Im **unveränderten QRXDesktop 6.6.20** das passende WBPROJECT-Icon bearbeiten:
`DefaultTool` auf **`C:QRX68Run`** setzen. Ein Doppelklick verwendet dann die
bestehende Workbench-DefaultTool-Ausführung, die die gewünschte HUNK-Datei als
Argument übergibt. Andere Dateien behalten ihren bisherigen Standardstarter.

Optionaler Quellpatch für zusätzliche Zuordnungen:

```bash
./armithlon qrxdesktop-qrx68-patch
# data/qrxdesktop/qrx_desktop-2.4.c mit deiner EXISTIERENDEN
# QRXDesktop/AROS-Toolchain bauen und bewusst installieren.
```

Nach Einbau dieser **optional separat zu kompilierenden** Version funktionieren
`QRXMODE=QRX68` und HUNK-Dateien mit der Endung `.qrx68` als Auswahl. Kein
separates neues Desktop-Design wird angelegt. Der Source-Patch ersetzt auch
keine fehlenden AROS-Programme durch Platzhalter.

## Interaktion und Kompatibilitätsgrenzen

**Getestet auf dem Host:** HUNK-Upload per Datei-Queue, echte 68k-HUNK-Ausführung
im **portablen** `qrx68-run`, AmigaDOS/Exec-HLE, Ausgabe-Dateien und fertiger
640×256-Framebuffer aus der grafischen Demo, fehlerhafte HUNK-Dateien,
verweigerte Symlinks, Quellpatch und nichtdestruktiver Gastinstaller.

**Nicht auf AROS getestet:** Cross-Compilation von `QRX68Run`, dessen `OpenWindow`
und `WritePixelArray` innerhalb des AROS-Gasts, tatsächlicher QRXDesktop-
Doppelklick und FS-UAE-/M1-End-to-End-Ausführung. Dafür sind realer AROS-SDK,
FS-UAE und ein gestarteter QRXDesktop nötig.

**Wesentliche Funktionslücke:** Die Grafikansicht ist gegenwärtig **ein einmaliger
Framebuffer nach Beenden** der emulierten Amiga-Anwendung. Der originale
QRX68-Intuition-HLE-Kern unterstützt noch **keine IDCMP-Eingaben, Gadgets,
Screens, Layers, Workbench-WBStartup-Nachrichten, Multitasking oder laufende
Fensterupdates**. Normale CLI-Programme der vorhandenen API-Teilmenge sind
verwendbar; komplexe AmigaOS-GUI-Anwendungen sind damit **noch nicht interaktiv
benutzbar**. Eine vollständige Rosetta-/AmigaOS-3.x-Kompatibilität ist nicht
nachgewiesen. Diese Einschränkungen sind keine Testausfälle, sondern noch
fehlende Funktionalität.

## Sicherheitsgrenzen

Der Dienst verwendet **keine Shell-Ausführung**, erlaubt nur begrenzte
Amiga-HUNK-Dateien in einem eigens freigegebenen Verzeichnis und führt diese in
einem separaten Kindprozess mit Zeit- und Ausgabelimits aus. Datei-Zugriffe des
emulierten Amiga sind standardmäßig nicht auf den Mac freigegeben. Die
Hostdateien können vom Gast dennoch verändert werden, wenn FS-UAE das Verzeichnis
schreibbar einbindet. Es existiert **keine vollständige OS-Sandbox** für
fehlerhafte oder bösartige 68k-Programme: Nur Programme aus vertrauenswürdigen
Quellen benutzen. Führe den Dienst niemals mit Administratorrechten aus.

## Stand der Entwicklung

- **AROS-/AmigaOS-3.x-Kompatibilität:** QRX68 isolierte API-Teilmenge, echter
  Musashi-Build weiterhin extern, AmigaOS-Originalbibliotheken nicht eingebaut.
- **QRXDesktop:** Sourcecode des vorhandenen 6.6.20 erhalten, DefaultTool-Pfad
  genutzt, optionale Version-Pin-Sourcepatch-Datei verfügbar.
- **AROS als Betriebssystem:** Weiterhin FS-UAE-Kompatibilitätsmodus; nativ
  unter QRX68100 noch BLOCKED.
- **QRX86 Win32/Linux:** Bestehende Routen nicht verändert.
- **Nächster realistischer Meilenstein:** `QRX68Run` mit echtem AROS-SDK bauen,
  GUI-Doppelklick unter FS-UAE/M1 verifizieren, **dann** bidirektionale
  IDCMP-/Input-/Multi-Frame-Protokolle und volle Exec/DOS/Intuition-ABI.
