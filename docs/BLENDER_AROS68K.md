# Blender 2.79b auf QRX OS One (68k-AROS, 68100 mit zwei Kernen)

Plan und Stand der Portierung. Ziel ist Blender 2.79b als natives 68k-AROS-Programm in der
Distribution. Es nutzt den zweiten 68100-Kern, wo es ihn gibt, und läuft auch ohne ihn.

Quellcode: `blender-2.79b.tar.gz` von download.blender.org (50,2 MB, MD5 geprüft). Er liegt
ungetrackt in `.build/blender-2.79b/src`, die Änderungen stehen als Patch in
`tools/blender/patches/`. Das Bauen beschreibt `tools/blender/README.md`.

## Ausgangslage

| Thema | Befund |
|---|---|
| Compiler | `m68k-elf-gcc` 16.2 mit AROS-SDK. C und C++ übersetzen, aber es gibt **keine** `libstdc++` und keine C++-Laufzeit. |
| ABI | GCC liest Zeiger aus `a0` und `double` aus `fp0`. Die SDK-Bibliotheken liefern `d0` und `d0:d1` (Soft-Float). Das gilt auch für `libm`: `sqrt(2.0)` lieferte Unsinn. |
| Threads | AROS-pthreads gibt es, sie laufen aber nur auf Kern 0. `pthread_spin_lock` hing im ersten Versuch fest, die Ursache ist nicht untersucht, weil die Sperren ohnehin ersetzt werden mussten. |
| Kern 1 | Erreicht nur gemeinsames RAM und seine Register. Kein ROM, keine Bibliotheken, keine Interrupts. Fehler parken den Kern und melden die Stelle. `TAS` und `CAS` sind atomar. |
| RAM | Bis 3840 MiB im 32-Bit-Adressraum. Virtuellen Speicher gibt es nicht, `mmap` ist hier eine Kopie. |
| OpenGL | `gl.library` (Mesa, Software) ist vorhanden. Der Host-GL-Weg `qrx_hostapi_gl.c` existiert nur für x86-Programme des Gastes. Die QRX3D-Tür erreicht Metal, ist aber keine GL-API. |
| FPU | Der JIT übersetzt keine FPU-Befehle. Sie laufen einzeln durch den Musashi-Interpreter mit Softfloat. |

## Abhängigkeiten

| Teil | Stufe 1 (`blender -b`) | Später |
|---|---|---|
| Python 3 | aus (`WITH_PYTHON=OFF`) | Pflicht für die Oberfläche, da sie aus Python-Skripten besteht. Blender 2.79 ist auf Python 3.5 ausgelegt. In den AROS Archives gibt es ein experimentelles Python 3.3.7, aber nur für i386 und ohne ladbare Module: als Vorlage für die AROS-Anpassungen nützlich, als Programm hier nicht. Für 68k muss CPython neu gebaut werden. |
| Blender Internal | an, Raytracer mit kleinen STL-Ersatzheadern | bleibt |
| Cycles | aus | braucht OpenImageIO, Boost und einen C++-Standard; 64-Bit-Rechnungen auf 68k sehr langsam. Erst über Host-Tür sinnvoll. |
| FFmpeg, OpenAL, SDL, Jack | aus | nach Bedarf |
| OpenEXR, OpenJPEG, OpenCOLLADA, Alembic, OpenSubdiv, OpenVDB | aus | nur mit C++-Standardbibliothek |
| PNG, JPEG, zlib, FreeType | an, aus dem SDK | bleibt |
| Neuer Abhängigkeitsgraph (C++) | durch Stub ersetzt, der Legacy-Graph macht die Arbeit | mit `libstdc++` |
| Eigen, Bullet, Fluid, Smoke, Boolean, Remesh, IK, Compositor, Freestyle | aus | mit `libstdc++` |

Der größte Einzelposten für alles Weitere ist eine echte C++-Standardbibliothek. Der wahrscheinlichste
Weg ist die Deadwood-Toolchain mit `libstdc++` oder `libstdc++-v3` aus GCC gegen das AROS-SDK.

## Big Endian

Blender war lange auf PowerPC zu Hause, der Code ist vorbereitet: `.blend`-Dateien beschreiben sich
selbst (SDNA) und werden beim Laden gedreht. Offen waren nur Punkte in der Bauweise:

- `makesdna` und `makesrna` schreiben Daten, die von Byte-Reihenfolge und Zeigergröße abhängen. Sie
  laufen deshalb als echte 68k-Programme im Emulator (`tools/blender/run_tool.py`), nicht auf dem Mac.
- SDNA rundet Adressen auf 4 Byte. Auf 68k liegen Arrays aber nur 2-Byte-ausgerichtet, also braucht
  `DNAstr` eine feste Ausrichtung. Ohne sie brach das Lesen mit "TYPE error" ab.
- Maßstab ist ein Bild: eine auf dem Mac (64 Bit, Little Endian) gespeicherte Datei wird im Gast geladen
  und gerendert und mit dem Mac-Bild verglichen.

## Speicher

- Der Gast hat 4 GB, ohne Auslagerung. Eine eigene Halde (`port/qrx_heap.c`) nimmt beim Start einen
  großen Block. Sie arbeitet ohne AROS-Aufrufe, weil Kern 1 sonst nicht allozieren könnte.
- Dieser Block ist derzeit auf knapp 2 GiB begrenzt: `AllocMem` kam mit 3 GiB nicht zurück. Die Ursache ist
  nicht untersucht. Mehr Speicher für Blender heißt, das zu klären oder mehrere Blöcke zu verwenden.
- Der Blender dieser Stufe hat 8,5 MB Code und 5 MB Daten, ohne Python und ohne die abgeschalteten Teile.
- Große Szenen stoßen an den 32-Bit-Adressraum, wie früher auf 32-Bit-PCs.

## Zweiter Kern

AROS hat keinen SMP-Scheduler, Kern 1 bekommt also nur gezielte Aufträge. Tile-Rendering ist dafür gut
geeignet: `do_part_thread` arbeitet fast nur auf Speicher.

- Sperren (`BLI_mutex`, `BLI_spin`, `BLI_rw_mutex`) sind auf `TAS` umgestellt. Auf Kern 0 gibt ein
  Wartender die CPU ab, weil der Inhaber eine andere AROS-Task sein kann.
- Tiles erreichen Kern 1 über zwei Fächer im Render-Hauptthread, denn auf die Warteschlangen mit
  Bedingungsvariablen kann er nicht warten.
- `malloc`, `mem*`, `str*`, Mathematik und die Uhr sind so gebaut, dass Kern 1 keine Bibliothek berührt.
- Ohne Kern 1 (oder mit `QRX_NO_CORE1`, `-t 1`) läuft alles auf Kern 0.
- Parkt Kern 1 mit einem Fehler, meldet Blender die Stelle und Kern 0 übernimmt die Kachel.

Vorerst nicht auf Kern 1: alles, was vor dem Rendern läuft (Laden, Szene aufbereiten, BVH-Bau), also
der Teil, der nicht über Kacheln geht.

## Fenster-Layer (GHOST)

GHOST ist C++ ohne großen Bedarf an der Standardbibliothek. Für die Oberfläche braucht es:

1. `GHOST_SystemAROS` mit Fenstern über Intuition oder direkt über die QRX-RTG-Anzeige, plus Maus,
   Tastatur und Uhr aus `input.device` und `QRXHostInput`.
2. Einen GL-Kontext für diese Fenster, siehe nächster Abschnitt.

Stand 12. November: gebaut, siehe „Oberfläche über die GL-Tür“ weiter unten.

## OpenGL im Viewport

Die 2.79-Oberfläche braucht OpenGL 2.1 mit GLSL und zeichnet sie auch für Menüs und Knöpfe.

- **Mesa im Gast** (`gl.library`, softpipe) ist da, rechnet aber auf der 68k-Emulation. Das ist nach
  aller Erfahrung für eine bedienbare Oberfläche zu langsam, gemessen ist es noch nicht.
- **Host-GL-Tür für 68k-Programme:** Die Engine hat schon GL für x86-Gäste (`qrx_hostapi_gl.c`). Eine Tür
  für 68k-Programme würde GL-Aufrufe direkt zum Mac bringen, ähnlich wie QRX3D. Das ist Arbeit in der
  Engine (Kontext pro Fenster, Zeiger in Gast-RAM, GLSL 1.20 auf Metal oder dem GL des Macs).
  **Entschieden am 11. November: Host-GL-Tür.** Gebaut, siehe unten.

## Stufen

1. **`blender -b` lädt eine `.blend` und rendert mit Blender Internal**, mit Kern 1. Stand: siehe unten.
2. C++-Standardbibliothek, dann neuer Graph, Eigen, Compositor, Modifier.
3. Python 3 auf AROS, dann `WITH_PYTHON=ON`, Add-ons und UI-Skripte.
4. GHOST für AROS und GL-Weg, dann die Oberfläche.
5. Eintrag in Distribution und App-Menü, sobald ein Nutzer damit etwas anfangen kann.

## Stand von Stufe 1 (7. November 2026)

`blender -b` läuft: Es lädt eine auf einem 64-Bit-Little-Endian-Rechner gespeicherte `.blend`, rendert mit
Blender Internal, verteilt Kacheln auf beide Kerne und schreibt ein PNG. **In Distribution und App-Menü ist
es noch nicht**, siehe unten.

### Bilder

Die Referenz ist ein nativer arm64-Build aus demselben gepatchten Quellcode (`configure-host.sh`). Der
offizielle Mac-Build wurde geladen, startet hier aber nicht, weil Rosetta nicht installiert ist. Die Referenz prüft
also Byte-Reihenfolge, Zeigergröße, Compiler, Mathe-Bibliothek und FPU, **aber keine Fehler, die in meinen
Patches selbst stecken**.

| Szene | Größe | mit und ohne Kern 1 gegen Mac |
|---|---|---|
| `startup.blend` (Standardwürfel) | 960x540 | 0 Pixel weichen ab |
| `preview.blend` (Prozedurale Texturen) | 640x640 | 1 von 409.600 Pixeln weicht ab, um 1/255 |
| `preview.blend` mit Raytracing, Strahlschatten, Spiegeln | 200x200 | 0 Pixel weichen ab |

Das eine Pixel ist derselbe Wert mit und ohne Kern 1. Die Ursache ist nicht untersucht, vermutlich eine
Rundung zwischen 68k-FPU und arm64. Wiederholte Läufe im Gast liefern identische Bilder.

### Zeiten mit FPU-JIT

Gemessen am 7. November auf der Engine mit nativen FPU-Befehlen im JIT (Render-Schritt, Last um 5 bis 10, ein Lauf
pro Szene außer beim Raytracing). Die Bilder sind unverändert, die Vergleichszeilen oben gelten auch hier.

| Szene | nur Kern 0 | mit Kern 1 | Faktor | Kacheln auf Kern 1 |
|---|---|---|---|---|
| Raytracing 200x200 | 4,88 / 4,79 / 4,87 s | 3,03 / 3,03 / 3,01 s | 1,6 | 15 von 36 |
| Prozedural 640x640 | 8,15 s | 7,71 s | 1,06 | 62 von 324 |
| Standardwürfel 960x540 | 1,08 s | 1,68 s | 0,64, also langsamer | 11 von 135 |

Bei kurzen Kacheln kostet Kern 1 mehr, als er bringt, denn der Hauptthread reicht Kacheln nur alle paar Millisekunden
weiter und holt sie ebenso ab. Der Würfel ist mit einem Lauf schwach belegt, die Richtung passt aber dazu. Hier gibt
es Spielraum: mehrere Kacheln im Voraus an Kern 1 geben oder Kern 1 selbst aus einer Warteschlange ziehen lassen.

Dieselbe Raytracing-Szene mit 640x640 wurde in dieser Messreihe nicht bis zum Ende gerendert.

### Zeiten ohne FPU-JIT

Gemessen im selben Lauf auf dem M1, Last um 5 durch andere Arbeit auf dem Mac, also mit Streuung. Es ist die
Zeit des Render-Schritts, ohne Laden und Speichern. Reihenfolge abwechselnd.

| Szene | nur Kern 0 | mit Kern 1 | Faktor | Kacheln auf Kern 1 |
|---|---|---|---|---|
| Raytracing 200x200 | 46,6 / 50,8 / 45,7 s | 29,5 / 34,1 / 28,1 s | 1,49 bis 1,63 | 15 bis 17 von 36 |
| Prozedural 640x640 | 29,5 s | 24,2 s | 1,22 | 111 von 324 |
| Standardwürfel 960x540 | 13,4 s | 11,1 s | 1,21 | 25 von 135 |

Dasselbe mit `QRX_NO_CORE1` (Raytracing): 47,9 s. Der native Mac-Build braucht für den Würfel unter 0,2 s
und für die Raytracing-Szene etwa 0,3 s, der Gast ist hier also grob 100- bis 170-mal langsamer.

Was die Zeit begrenzt:

- **FPU im Interpreter.** Der JIT übersetzt keine FPU-Befehle, jeder läuft einzeln über Musashi und Softfloat. Bei
  dieser Arbeit fällt die Engine auf rund 50 MIPS, sonst schafft sie rund 600. Native FPU-Befehle im JIT wären der
  mit Abstand größte Hebel, sie würden allen 68k-Programmen helfen.
- **Speichern ist seriell.** Die Umrechnung von Float nach 8 Bit vor dem PNG nimmt beim Würfel 15 s auf Kern 0.
- **Kleine Kacheln.** Der Hauptthread reicht Kacheln nach Kern 1 weiter, das kostet bei kurzen Kacheln
  anteilig mehr, und jede Kachel schreibt eine Log-Zeile.

### Geprüft

- Laden: Würfel-Eckpunkte und Zählerstände stimmen, das Bild auch.
- `makesdna` und `makesrna` laufen im Gast (9 s und 13 s), daraus entsteht der Build.
- Kern 1 rendert echte Kacheln, Sperren und Halde laufen auf beiden Kernen.
- Blender läuft ohne Kern 1 (`-t 1`, `QRX_NO_CORE1`).
- Fehler auf Kern 1 (`QRX_CORE1_FAULT_TEST`, illegaler Befehl): Blender meldet die Stelle, Kern 0 übernimmt die
  Kachel, das Bild bleibt pixelgleich.
- Engine: `qrx68100_core1_tests`, `qrx68100_jit_diff_tests`, `qrx68100_full_tests` bestehen. Der Gesamtlauf
  von `ctest` wurde nicht ausgeführt.
- Der Patch lässt sich auf ein frisch entpacktes Original anwenden und ergibt genau den gebauten Quellcode.
  `tools/blender/build.sh` wurde dagegen nicht von null durchlaufen: Die Schritte stammen aus dem echten Bau, das Skript
  selbst ist ungetestet.

### Nicht geprüft oder nicht vorhanden

- Eine Raytracing-Szene mit 640x640 lief im Gast nicht bis zum Ende durch: auf Kern 0 war sie nach 114 s bei
  Kachel 70 von 324. Sie sieht langsam aus, nicht falsch, ist aber unbelegt.
- Dateien mit Bildtexturen, Animationen über mehrere Frames, Partikel, Modifier jenseits des Üblichen.
- `.blend`-Dateien, die der Gast selbst schreibt. Ohne Python gibt es dafür keinen einfachen Weg.
- Andere Rechner als ARMithlon: `qrx_core1_available` liest die SMP-Register direkt. Auf einer Maschine ohne
  dieses Board, etwa FS-UAE, ist das ein Zugriff ins Leere und nicht getestet.
- Kern 1 arbeitet nur beim Tile-Rendering. Laden, BVH-Bau und Speichern laufen auf Kern 0. Lesezugriffe von
  Kern 1 aufs ROM lösten im Test **keinen** Fehler aus, ein falscher Aufruf dorthin würde also nicht sicher
  gemeldet werden. Dass kein Tile-Pfad Bibliotheken aufruft, ist nur durch die Tests belegt, nicht statisch.
- Abgeschaltet oder Stub: Python, neuer Abhängigkeitsgraph, Eigen (Löser beenden das Programm mit Meldung), Haar-Solver
  `BPH_hair_volume_solve_divergence`, GHOST, OpenGL, Cycles, Compositor, Freestyle, Boolean, Remesh, Fluid, Smoke, Bullet, IK,
  OpenEXR und andere Formate. Die `RNA_...not found`-Zeilen beim Start kommen vom fehlenden Python.

### Gefundene Fehler außerhalb von Blender

Beide stecken im Musashi-Kern und treffen jedes 68k-Programm mit FPU. Sie sind in
`upstream/armithlon/2.6/tools/patch-musashi-fsave-an.py` behoben. Das lokale Bundle hat seit dem 7. November eine
Engine mit diesen Korrekturen und dem FPU-JIT. Die alte liegt daneben als `armithlon-engine.vor-fpu-jit-2026-11-07`.

- `FSSQRT` und `FDSQRT` liefen als `FINT`: `sqrt(2.0)` ergab 2.0.
- `FMOVEM` mehrerer FPU-Steuerregister mit `(An)` schrieb alle auf dieselbe Adresse. AROS sichert so bei jedem
  Task-Wechsel, also gingen FPCR und Bedingungscodes verloren. Zeigte sich als einzelne falsche Pixel:
  ein Wechsel zwischen `FCMP` und `FBcc` nahm den falschen Sprung.

Außerdem kann `elf2hunk` große Programme mit PC-relativen Sprüngen nicht abbilden (`elf2hunk-pc32.diff`).

### C++-Standardbibliothek (7. November)

`tools/blender/build_libstdcxx.sh` baut `libstdc++` aus GCC 16.2, derselben Version wie der Compiler, ohne Exceptions, RTTI und
Threads. Im Gast laufen damit Container, `string`, `sstream`, `iostream`, `function`, `shared_ptr` und globale Konstruktoren.
Ein `throw` beendet das Programm mit Fehlercode 20. Damit sind in Blender die Ersatzteile für den Abhängigkeitsgraph, Eigen und den
Cache-Limiter wieder durch die echten ersetzt, auch der Haar-Löser ist wieder drin. Standard bleibt der Legacy-Graph.

| Szene | nur Kern 0 | mit Kern 1 | Bild gegen Mac |
|---|---|---|---|
| Raytracing 200x200 | 4,46 / 4,47 s | 2,88 / 2,76 s | 0 Pixel |
| Prozedural 640x640 | 4,36 s | 4,02 s | 1 Pixel um 1/255 |
| Standardwürfel 960x540 | 0,80 s | 1,19 s | 0 Pixel |
| Standardwürfel mit `--enable-new-depsgraph` | | 1,07 s | 0 Pixel |

Grenzen:

- Mit Threads ist `libstdc++` nicht gebaut: `shared_ptr` zählt nicht atomar, `std::thread` und `std::mutex` fehlen. Für Blender und
  Python muss das noch geprüft werden, der neue Graph rechnet mit Threads nur auf Kern 0.
- Nur der Würfel lief über den neuen Graphen. Eigen-Löser, Cloth und Haar-Löser sind gebaut, aber nicht aufgerufen worden.
- Der Native Build hing nach dem Umbau in einer Read-Write-Sperre, weil Blender `R = *re` eine Sperre mit Zustand kopiert. Die Kopie
  wird jetzt zurückgesetzt. Das Problem zeigte sich nur auf dem Mac.

### Python 3.5 (10. November)

CPython 3.5.10, die Version, auf die Blender 2.79 ausgelegt ist, baut statisch für 68k-AROS
(`tools/blender/build_python.py`, Änderungen in `tools/blender/patches/python-3.5.10-qrx.patch`). Ein Host-Python
braucht es nicht, weil Grammatik und eingefrorene `importlib` im Quellpaket liegen. Das Programm hat 4,2 MB, die
Bibliothek liegt unter `PROGDIR:lib/python3.5`.

Was für AROS angepasst ist:

- Pfade: `PYTHONPATH` trennt mit `;`. `os.path` kennt Volumes und Assigns (`SYS:foo` ist absolut), `os.devnull` ist
  `NIL:`, temporäre Dateien liegen in `T:`, und `os.listdir()` liest `""`, AROS' aktuelles Verzeichnis.
- Kodierung: Dateisystem, Ein- und Ausgabe in UTF-8. `mbstowcs` und Verwandte sind dafür neu, denn die SDK-Versionen
  scheiterten schon an ASCII.
- Prozesse: `fork` und `exec` gibt es nicht. `subprocess` lädt, der Start eines Prozesses scheitert mit `OSError`.
- Zufall: vom Host über die Tür. Ohne Tür bleibt ein zeitbasierter Generator, gut genug für Hash-Werte, nicht für Kryptographie.
- `faulthandler` überspringt Signale, die AROS nicht zustellen kann.

Geprüft im Gast: ein eigener Test mit 18 Prüfungen (Mathematik, Float-Darstellung, große Zahlen, `struct`, `re`, `json`,
`pickle`, Hashes, `zlib`, Datum, Zufall, `decimal`, `fractions`, vier Threads, `sleep`, Datei mit Umlauten). Alles
bestanden. Die Umwandlung von Zahl-Strings stimmt in 16 Grenzfällen bitgenau mit dem Mac überein.

CPythons eigene Tests, einzeln im Gast gestartet:

| Modul | Ergebnis | Woran die Fehler liegen |
|---|---|---|
| test_grammar, test_list, test_string, test_heapq, test_collections, test_hashlib, test_zlib | bestanden | |
| test_struct, test_dict, test_itertools | bestanden, seit `_testcapi` mitgebaut ist | |
| test_json | 150 Tests, 5 Fehler | starten `python -m json.tool` als Unterprozess |
| test_threading | 148 Tests, 10 Fehler | Unterprozesse und Subinterpreter |
| test_tempfile | 82 Tests, 2 Fehlschläge, 8 Fehler | AROS löscht keine offenen Dateien, Dateirechte |
| test_socket | 648 Tests, 376 übersprungen, 7 Fehlschläge, 97 Fehler | fehlende Socket-Optionen und -Attribute, IPv6, Unix-Sockets |
| test_math, test_float | nicht bis zum Ende gelaufen | sehr umfangreich, `test_float` stand nach 2 Minuten |

Dass direkt nach `test_tempfile` ein neuer Python-Prozess einmal keinen zweiten Thread starten konnte, ist inzwischen
erklärt, siehe „Zweiter Lauf in derselben Shell“. Ein Programm im Boot-Skript erbt als aktuelles Verzeichnis
`AROS Kickstart:`, auf das man nicht schreiben kann; Jobs beginnen deshalb mit `CD SYS:`.

### Blender mit Python (10. November)

Blender ist jetzt mit eingebettetem Python 3.5 gebaut (`WITH_PYTHON=ON`, 11 MB Code). Im Gast lädt `blender -b` Blenders
eigene Python-Skripte in etwa 5 Sekunden, `bpy` arbeitet. Geprüft:

- `bpy.app.version_string` und die Objekte der Startszene stimmen.
- Ein Skript (`tree/scene.py`) dreht den Würfel, legt über Operatoren eine Kugel mit Spiegelmaterial und eine Ebene mit
  Wolkentextur an, schaltet Strahlschatten ein, speichert eine `.blend` und rendert. Kern 1 übernimmt dabei 10 Kacheln.
- Die gespeicherte Datei beginnt mit `BLENDER_V279`, ist also 32 Bit und Big Endian. Der native Mac-Build lädt sie, und
  sein Bild ist **pixelgleich** mit dem Gast-Render. Damit ist der Weg über `.blend`-Dateien in beide Richtungen geprüft.

Weitere Stubs für GL kamen durch das Python-Modul `bgl` dazu. `tools/blender/install_tree.sh` bringt Blender, Python und
die Skripte in den Testbaum.

### Netz für 68k-Programme (10. November)

68k-Programme bekommen BSD-Sockets auf den Sockets des Macs, über Dienst 4 der ARMithlon-Tür. AROSTCP und eine
Netzwerkkarte sind dafür nicht nötig. Die Schicht (`tools/blender/port/qrx_socket.c`) stammt aus der Tor-Portierung,
löst Namen aber über den Host auf. Die Tür-Änderung für native Programme ist dieselbe wie dort. Damit hat Python echte
`_socket`- und `select`-Module.

Geprüft im Gast mit Python, alle 7 Punkte bestanden: `localhost` auflösen, Zufall vom Host, HTTP-Abruf von einem
Server auf dem Mac, `select`, ein Echo-Server im Gast mit Client in einem zweiten Thread, `example.com` auflösen und
per HTTP laden.

Grenzen: nur IPv4, höchstens 64 Sockets, kein `sendmsg`/`recvmsg`, kein TLS, weil OpenSSL noch nicht gebaut ist. Wer
wartet, fragt alle 2 ms nach.

Seit dem 11. November liegt der Stand in `claude/qrx-22.5.11`, und die Engine im Bundle ist daraus neu gebaut
(`.build/m1-merged`, 59 von 60 Engine-Tests bestanden; der eine, `IDCMP_mouse_redraw_close_concurrent`, scheiterte nur
im parallelen Lauf mit `-j8` und bestand einzeln dreimal). Mit ihr besteht der Netztest im Gast 7 von 7, mit der alten
blieb er stehen. Die alte Engine liegt daneben als `armithlon-engine.vor-blender-merge-2026-11-11`.

### Zweiter Lauf in derselben Shell (11. November)

Ein zweites `blender -b` in derselben Shell hing beim Rendern, ebenso Blender nach einem Python-Lauf, nach `C:Avail`
oder einem kleinen Testprogramm aber nicht. Ohne Kern 1 hing es jedes Mal, mit Kern 1 meist. Ursachen, von außen nach
innen:

- **`environ` aus der C-Bibliothek beschädigt die Shell.** Wer `environ` benutzt (Python, damit auch Blender), zieht aus
  `libcrtprog` eine Startfunktion, die die `crt`-Bibliothek das Array bauen lässt. Die Bibliothek zählt die lokalen
  Variablen der Shell mit einem `ScanVars`-Hook, der seine Argumente von der falschen Stelle nimmt: Er zählt in die
  Knoten der Variablen selbst hinein (gefunden mit `QRX_FULL_TRACE_RAMW_RANGE` auf den ersten Knoten, Schreiber im
  Code der aus `LIBS:` geladenen Bibliothek). Die Shell lebt länger als das Programm. Danach hatten `process`, `RC` und
  `Result2` keine Namen mehr, und `CreateNewProc`, das die Variablen in jeden neuen Prozess kopiert, schlug fehl, ohne
  `IoErr` zu setzen. `pthread_create` meldete `EAGAIN`, und Blender startet seinen Render-Thread ohne Prüfung.
  Abhilfe: `tools/blender/port/qrx_posix.c` definiert `environ` selbst und füllt es aus den lokalen Variablen, so wird
  der Code der Bibliothek gar nicht erst gelinkt. Python sieht in `os.environ` dieselben Einträge wie vorher.
- **`pthread_cond_timedwait` mit verstrichener Frist wartet ewig.** AROS rechnet die Frist in eine relative Zeit um und
  prüft nur, ob sie null ist. Eine schon vergangene Frist ergibt eine negative Zeit, die `timer.device` als rund 136 Jahre
  liest. Blenders Hauptschleife wartet in Schritten von 5 ms, ein Taskwechsel dazwischen genügte.
  `tools/blender/port/qrx_pthread.c` meldet dann sofort `ETIMEDOUT` und wartet sonst relativ. Das traf Blender mit Kern 1:
  Ohne Render-Thread auf Kern 0 rendert Kern 1 dann eben alle 80 Kacheln, aber nur, wenn die Hauptschleife nicht in
  diesem Warten stecken bleibt.
- **Übergabe an Kern 1.** Kern 1 leerte erst `todo` und meldete dann `done`. Dazwischen konnte Kern 0 schon die nächste
  Kachel vergeben, und war Kern 1 mit ihr fertig, bevor der Hauptthread wieder an der Reihe war, überschrieb er eine
  fertige Kachel. Jetzt meldet er zuerst und wartet, bis Kern 0 die vorige abgeholt hat. Diesen Fall habe ich nicht
  beobachtet, er folgt aus dem Code.

Geprüft: zweimal Blender hintereinander in einer Shell, je fünf Läufe ohne und mit Kern 1, alle fertig (mit Kern 1 je
8 bis 11 von 80 Kacheln auf Kern 1). Netztest und danach zweimal Blender in einer Shell mit der neuen Bundle-Engine.
Nach Python stimmen die Shell-Variablen. Das Bild der Python-Szene ist weiter **pixelgleich** mit dem Mac-Render
(0 von 76.800 Pixeln). Nicht geprüft: der Fall nach `test_tempfile` selbst, und ob die `crt`-Bibliothek
außerhalb dieses Ports weitere Programme trifft. Vermutlich betrifft es jedes AROS-Programm, das `environ` benutzt.

### Offene Entscheidungen

1. **Oberfläche und OpenGL:** entschieden für die Host-GL-Tür (11. November), gebaut am 12. November.
2. **Distribution:** `blender -b` ist ein Kommandozeilenprogramm. Die Engine im Bundle ist seit dem 11. November auf
   dem nötigen Stand. Ein Eintrag im App-Menü ergibt vor der Oberfläche trotzdem wenig Sinn.

### Oberfläche über die GL-Tür (12. November)

**Blender 2.79b läuft mit Oberfläche im Gast und lässt sich bedienen.** Getestet sind: Splash schließen, Menü
„File“ öffnen, Tab in den Edit-Modus, F12 rendern (10,8 s, Ergebnis im Bild-Editor) und Strg+Alt+U (Einstellungen
als zweites Fenster mit eigenem, geteiltem Kontext). Alle Eingaben kamen als eingespielte Klicks und Tasten der Engine
(`QRX_TEST_CLICKS`, `QRX_TEST_KEYS`); von Hand bedient hat es noch niemand.

**GL-Tür (Engine `full68k/qrx_gl68k.c`, Dienst 8, Feature-Bit 2).** Die Gastbibliothek `tools/qrxgl68k/guest`
(`libqrxgl.a`, ersetzt `libGL`/`libGLU` beim Linken) schreibt jeden Aufruf in einen Befehlsstrom im Gast-RAM. Die
Engine arbeitet ihn auf einem echten OpenGL-2.1-Kontext des Macs ab (Legacy-Profil, GLSL 1.20, „2.1 Metal“ auf dem M1).
Beide Hälften erzeugt `tools/qrxgl68k/gen.py` aus den GLEW-Prototypen der Funktionen, die der Mac hat: 1256 Funktionen
mit einer Nummer, 1060 vollständig unterstützt. Was fehlt, steht in `unsupported.txt`: Evaluatoren, VAOs, Instancing,
UBOs. GLEW meldet diese Erweiterungen als nicht vorhanden.

- Ein Aufruf wartet nur, wenn er etwas zurückgibt oder die Engine Gast-Speicher lesen oder schreiben muss.
  Vektor- und Uniform-Arrays fester Größe gehen inline mit.
- Gast-Adressen gehen als 64-Bit-Werte in den Strom. Ein 64-Bit-Gast (siehe `docs/armithlon/qrx64-umstieg.md`)
  braucht kein neues Protokoll; nur die Gastbibliothek setzt das obere Wort auf 0.
- Byte-Reihenfolge:
  - Pixeldaten tauscht GL selbst (`GL_UNPACK/PACK_SWAP_BYTES` auf dem Host an, für den Gast als aus gemeldet).
  - Vertexdaten haben keinen solchen Schalter. Pufferobjekte liegen deshalb in der Engine als Kopie in
    Gast-Reihenfolge, und jeder Zeichenaufruf wandelt die Vertices, die er braucht, in Client-Arrays um.
  - `glMapBuffer` gibt eine Kopie im Gast-Speicher, die beim Unmap zurückgeschrieben wird.
- Jedes Fenster ist ein FBO in Fenstergröße, das für Framebuffer 0 steht. Die Engine bildet dafür `glBindFramebuffer`,
  `glDrawBuffer`, `glReadBuffer` und die zugehörigen `glGet` ab.
- `QRX_GL68K_CAPTURE=pfad` speichert jedes präsentierte Bild als PPM, `QRX_GL68K_TRACE=n` protokolliert die ersten n
  Aufrufe im Engine-Log.

Prüfung der Tür allein: Eine Testszene (`tools/qrxgl68k/test`) enthält Immediate Mode, Texturen in Bytes, 5_6_5 und
Floats, VBO- und Index-Puffer mit `glMapBuffer`, Client-Arrays, GLSL mit Uniforms, Display-Listen, FBO, `glReadPixels`
und Blenders Mesh-Pfad (`GL_SHORT`-Normalen, `GL_UNSIGNED_INT`-Indizes, Licht, Tiefe). Als 68k-Programm ist sie
**bitgleich** mit derselben Szene nativ auf dem Mac, wenn auch dort ein FBO der Framebuffer ist. Gegen ein echtes
Mac-Fenster weichen 389 von 256.000 Pixeln ab, und zwar nur an Rasterisierungs-Grenzfällen (Texel genau auf
Pixelmitte, Diagonalen). Das ist eine Eigenschaft des Macs, nicht der Tür.

**GHOST für AROS (`tools/blender/ghost`).**
- `ghost_aros_native.c` ist der Intuition-Teil:
  - Fenster auf dem Workbench-Bildschirm, die sich einen IDCMP-Port teilen.
  - Tasten über `MapRawKey`, Mausrad über `RAWKEY_NM_WHEEL_*`.
  - Das Bild der Tür kommt mit `WritePixelArray` ins Fenster.
- Ist der Workbench-Bildschirm kleiner als 1024×600 oder ohne Truecolor, öffnet GHOST einen eigenen RTG-Bildschirm
  (1280×800, `QRX_BLENDER_SCREEN=BxH`).
- Läuft der QRX-Desktop nicht, nennt GHOST diesen Bildschirm „QRX“. Grund: `C:QRXHostInput` setzt den Zeiger
  immer relativ zum öffentlichen Bildschirm „QRX“; ohne ihn kam keine Mausbewegung bei Blender an.
- `QRX_GHOST_TRACE` protokolliert Ereignisse.
- Noch nicht da:
  - eigene Mauszeiger-Formen (nur der Warte-Zeiger)
  - Zeiger versetzen (fortlaufendes Ziehen bei Zahlenfeldern)
  - Zwischenablage zum Mac (nur innerhalb von Blender)
  - Minimieren

Start: `tools/blender/run-gui.sh` (siehe `tools/blender/README.md`). Der Job braucht `CD SYS:`, sonst findet Blender
seine Python-Skripte nicht. Der Batch-Baum `tree` hat keine Schriften; die Oberfläche läuft im eigenen Baum `guitree`.

**Mac-Referenz.** Das offizielle Blender 2.79b für den Mac ist ein Intel-Programm, und auf diesem Mac fehlt Rosetta.
Die Referenz ist deshalb derselbe Quellcode nativ für arm64 (`tools/blender/configure-host-gui.sh`, Abhängigkeiten aus
`build-hostdeps.sh`: Python 3.5.10 und FreeType 2.12.1). Sie benutzt dieselben GHOST-Klassen; nur die Intuition-Schicht
(`hostref/ghost_host_native.c`) und die GL-Schicht (`hostref/qgl_host.c`, das GL des Macs direkt, FBO als Framebuffer
wie die Tür) sind ersetzt. Sie bekommt dieselben Klicks nach derselben Zahl präsentierter Bilder. Pro Bild
1272×771 = 980.712 Pixel:

| Zustand | Pixel verschieden | was |
|---|---|---|
| erstes Bild | 0 | |
| Splash | 894 | Speicheranzeige im Kopf, Ecke unten links |
| nach Klick (Splash zu) | 1.008 | dazu das gestrichelte Muster am 3D-Cursor |
| Menü „File“ offen | 12.827 | dazu die Tastenkürzel: die Referenz ist mit `__APPLE__` gebaut und zeigt „Cmd“ statt „Ctrl“, das Menü wird dadurch anders breit |

Das sind keine Fehler des Gasts: Die Speicherwerte sind andere, und das Strichmuster rechnet der Mac in einem
Software-Rückfall („Fallback to SW vertex for line stipple“). Die Streifen der Bereichsecke unten links zeigt nur der
Gast; ich vermute Hover-Zustand oder Mausposition, geprüft ist es nicht. Die Schriftmaße sind trotz FreeType 2.14 im
Gast gegen 2.12.1 in der Referenz gleich (`blf.dimensions`). Renders mit dem neuen Build sind weiter **pixelgleich** mit
dem Mac: Python-Szene 0 von 76.800, Startszene 960×540 0 von 518.400.

**Compilerfehler in GCC 16 für m68k.** Der Vergleich fand zwei Fehler im Gast:
- Die Outliner-Namen lagen 20 Pixel zu weit links, unter den Icons.
- In der Zeitleiste fehlten die Bildnummern.

Die Ursache ist dieselbe. GCC sichert ein aufrufflüchtiges FPU-Register (`fp0`/`fp1`) um einen Aufruf als 96-Bit-Extended
(`fmovex`) und liest es danach als 32-Bit-Float 8 Byte weiter zurück, also aus den unteren Mantissenbytes. Im Outliner
wurde so aus `offsx += UI_UNIT_X + 2 * ufac` (20 + 22) die 22. Das passiert mit dem alten Reload und mit LRA, also
unabhängig vom Allokator, und es ist kein Fehler der Engine (JIT und Interpreter rechnen gleich). `aros_cc.py` übersetzt
jetzt alles mit `-fno-caller-saves`. libstdc++, Python, die Port-Objekte und Blender sind damit neu gebaut. Von 1.077
solchen Sicherungen in Blender bleiben 26, alle in echten `long double`-Funktionen (libstdc++-Ausgabe,
`__isinfl`). Nicht neu geprüft: CPythons eigene Tests (vorher 59/60).

Beim Neubau fiel auf, dass `shim/include/sys/uio.h` kein `extern "C"` hatte. Der neu gebaute libstdc++ verlangte deshalb
ein C++-`writev`. Das ist korrigiert.

**Nicht erledigt:**
- Die Bundle-Engine hat die GL-Tür noch nicht. Die Tests liefen mit der Engine aus diesem Zweig
  (`.build/blender-2.79b/engine-build`). Für Nutzer braucht es erst den Merge nach `claude/qrx-22.5.11` und dann eine
  neue Bundle-Engine; die alte wird vorher gesichert.
- Geschwindigkeit ist nicht gemessen. Der Start bis zur Oberfläche dauerte in den Testläufen unter 100 s.
