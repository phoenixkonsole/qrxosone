# ARMITHLON 2.5 – QRX68/2 Live-Anwendungen im bestehenden QRXDesktop

## Ziel und derzeitiger Nachweis

QRXDesktop 6.6.20 bleibt die Benutzeroberfläche. Der bereits vorhandene
Workbench-`DefaultTool`-Pfad (`C:QRX68Run`) startet einen AROS/m68k-Client,
der ein Amiga-HUNK über `QRXBRIDGE:` an einen **dauerhaft laufenden** QRX68-
Hostprozess übergibt. QRX68 interpretiert 68k-Instruktionen, stellt bestimmte
AmigaOS-3.x-Systemaufrufe bereit, zeichnet sein Bild fortlaufend und empfängt
Maus-/Tastatur-/Fensterereignisse zurück.

**Echt lokal nachgewiesen:** Zwei gleichzeitig laufende originale Amiga-HUNK-
Programme (portable 68k-Teil-CPU) warten auf echte HLE-`WaitPort()`-Aufrufe,
nehmen per `GetMsg()` Nachrichten entgegen, geben sie via `ReplyMsg()` frei,
ändern beim Mausklick die Grafik und beenden sich nach `IDCMP_CLOSEWINDOW`.
Die Python-Bridge prüft Ereignisse vor der Weitergabe. Der Host kopiert Frame-
Snapshots atomar, hält maximal vier Sitzungen getrennt und übergibt keine
freien Shell-Befehle oder Host-Pfade an den Gast.

**Nicht nachgewiesen:** AROS/m68k-Cross-Build und FS-UAE-Doppelklick auf einem
Mac, native Bootfähigkeit des QRX68100-AROS, vollständige Musashi-/FPU-Ausführung,
komplette `intuition.library`, Gadgets/Layers/Menu, Wait/Signals, echte
AmigaOS-3.x-Kompatibilität beliebiger Anwendungen. Die Bridge verwendet eine
640×256-Framebuffer-Teilimplementierung; Fensterpositionen und Interaktionen
mit einem komplexen echten Intuition-Desktop sind noch begrenzt.

## Was neu ist

- `engine/qrx68/qrx68_hle.c`: klassisches `OpenWindow()` mit IDCMPFlags-
  Teilmenge, Window.UserPort (+86), `Exec/GetMsg -372`, `ReplyMsg -378`,
  `WaitPort -384` (gibt CPU-Zeitscheibe an Host zurück); begrenzter
  IntuiMessage-Pool mit Class/Code/Qualifier/MouseX/MouseY und FIFO.
- `engine/qrx68/qrx68_main.c`: `--live-events DIR --live-frame FILE.ppm`.
  Der Gast bleibt im Speicher und publiziert Bildänderungen fortlaufend.
- `engine/qrx68/qrx68_musashi.c`: persistente CPU-Sitzung `begin/slice/end`
  statt FPU-Zustand bei jedem Interaktionsintervall zurückzusetzen.
  Die reale Ausführung mit gepinntem Musashi steht weiterhin aus.
- `integration/qrx68-bridge/qrx68_service.py --interactive`: QRX68/2,
  pro Anwendung ein isolierter Prozess, atomische PPM-Veröffentlichung, feste
  Event-Tupel, Größenlimits, Zeitlimits, Status zuletzt, STOP-Protokoll.
- `engine/guest/aros/qrx68/QRX68Run.c`: vorhandener AROS-Launcher jetzt mit
  lebendem Intuition-Fenster, regelmäßigem Frame-Refresh und Weitergabe von
  MouseButtons, MouseMove, RawKey, VanillaKey und CloseWindow.
- `data/qrx68/QRX68-Live.hunk`: **originales 68k-Testprogramm**, das erst blau
  zeichnet und nach einem Mausklick zusätzlich rot; es endet bei CloseWindow.
- `tools/test-qrx68-live25.py`: Hostintegration inklusive zwei parallelen
  Gastprozessen und Ablehnung fehlerhafter Ereignisse.

## Offline-Test (Mac oder Linux)

```bash
cd ~/Downloads/ARMITHLON-2.5-macOS
./armithlon build
./armithlon test
./armithlon qrx68-live-test
```

Im Test werden zwei echte HUNK-Gastprozesse gestartet und kontrolliert, dass
beide gleichzeitig ihre eigenen Fensterbilder und Ereigniswarteschlangen
besitzen. **Der Test emuliert AROS-Eingaben nur über QRXBRIDGE; FS-UAE ist dabei
nicht beteiligt.**

## Bestehenden QRXDesktop benutzen (Mac mit AROS-SDK)

Voraussetzung: `fs-uae`, ein echtes AROS/m68k-Cross-SDK, Deadwood-AROS-Image,
Musashi-Quellen aus dem vorgesehenen gepinnten Setup und ein vorhandenes
QRXDesktop 6.6.20. Die benötigten AROS-/Musashi-Binärdateien sind nicht im ZIP.

```bash
./armithlon full68k-setup
./armithlon full68k-build
export QRX_AROS_CC=/PFAD/ZUM/m68k-aros-gcc
export QRX_AROS_CFLAGS='-I/PFAD/ZUM/SDK/include -L/PFAD/ZUM/SDK/lib'
./armithlon qrx68-aros-build
./armithlon qrx68-aros-install /PFAD/ZUM/EXTRAHIERTEN/AROS-SYSTEM
./armithlon qrx68-aros-start /PFAD/ZUM/EXTRAHIERTEN/AROS-SYSTEM
```

In QRXDesktop das Workbench-**Projekt-Icon** einer zulässigen HUNK-Anwendung
auf `DefaultTool=C:QRX68Run` setzen und doppelklicken. Der vorhandene
QRXDesktop-Codepfad benötigt dafür keinen Umbau. Als optionale Zuordnung
`QRXMODE=QRX68` lässt sich weiterhin der Source-Patch bauen:

```bash
./armithlon qrxdesktop-qrx68-patch
```

Eine komplett kompilierte gepatchte QRXDesktop-Binärdatei ist nicht enthalten.
AROS sollte `QRXBRIDGE:` auf das freigegebene echte Hostverzeichnis abbilden;
`qrx68-aros-start` verwendet den vorhandenen FS-UAE-Bootpfad.

## Protokoll / Sicherheitsregeln

Spool: `QRXBRIDGE:inbox/<id>.hunk`, `<id>.req`; live-Antworten im Verzeichnis
`results`: `<id>.live`, `<id>.ppm`, `<id>.frame`, `<id>.status`.

Gast schreibt `<id>.<10stelligeNummer>.evt` mit exakt
`QRX68/2 WINDOW CLASS CODE QUALIFIER X Y\n` (Zahlen dezimal); maximal
96 Byte. Der Host prüft alle Grenzen, Dateitypen, IDs und Symlinks, leitet
nur unterstützte IDCMP-Klassen weiter und trennt pro Sitzung die Eingabedateien.
`.frame` wird **nach** `.ppm` atomisch veröffentlicht. `.status` kommt erst
nach dem Ende; so kann QRXDesktop während der Ausführung rendern.

Ein Close-Gadget sendet zuerst ein echtes IDCMP_CLOSEWINDOW. Endet die
Anwendung nicht, wird ein STOP-Marker übertragen. Der Host beendet dann nur
die betroffene Sitzung. Standardmäßig sind keine Verzeichnis-Freigaben für die
Amiga-HUNK-Anwendung aktiv.

Die Bridge ist ein **gezielt beschränkter Kompatibilitätsprototyp** und kein
vertrauenswürdiger Sandbox-Ersatz für beliebige fremde Programme; für solche
Gastdateien sind zusätzlich OS-Prozesssandboxing und Berechtigungsgrenzen nötig.

## Nächste noch offene Stufen

1. AROS/m68k-Cross-Compile, FS-UAE-Szenariotest mit tatsächlichem QRXDesktop
   und mehreren echten AmigaOS-3.x-Programmen durchführen.
2. Musashi-68040/FPU-Session unter denselben UI- und CPU-Regressionstests
   gegen den portablen Interpreter differenziell validieren.
3. Exec Wait/Signals, Timer, Ports, Aufgaben-/Prozessobjekte, DOS-Pakete,
   Intuition-Layers, Gadgets, Menüs und Fenster-Resize implementieren.
4. Echtes `QRX68Run` als AROS-Systemdienst auf dem späteren nativen QRX68100
   anbieten, statt den FS-UAE-Verzeichnistransport zu benötigen.
