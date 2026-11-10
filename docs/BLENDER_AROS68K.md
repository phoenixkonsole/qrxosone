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
3. Für Stufe 1 ersetzt `intern/qrx/qrx_ghost_stub.c` alles, nur die Pfad-Funktionen sind echt.

## OpenGL im Viewport

Die 2.79-Oberfläche braucht OpenGL 2.1 mit GLSL und zeichnet sie auch für Menüs und Knöpfe.

- **Mesa im Gast** (`gl.library`, softpipe) ist da, rechnet aber auf der 68k-Emulation. Das ist nach
  aller Erfahrung für eine bedienbare Oberfläche zu langsam, gemessen ist es noch nicht.
- **Host-GL-Tür für 68k-Programme:** Die Engine hat schon GL für x86-Gäste (`qrx_hostapi_gl.c`). Eine Tür
  für 68k-Programme würde GL-Aufrufe direkt zum Mac bringen, ähnlich wie QRX3D. Das ist Arbeit in der
  Engine (Kontext pro Fenster, Zeiger in Gast-RAM, GLSL 1.20 auf Metal oder dem GL des Macs) und damit
  **eine Entscheidung für den Nutzer**, siehe unten.

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

Beobachtet und nicht geklärt: Direkt nach `test_tempfile` konnte ein neuer Python-Prozess einmal keinen zweiten Thread
starten, im übernächsten Prozess ging es wieder. Ein Programm im Boot-Skript erbt als aktuelles Verzeichnis
`AROS Kickstart:`, auf das man nicht schreiben kann; Jobs beginnen deshalb mit `CD SYS:`.

### Netz für 68k-Programme (10. November)

68k-Programme bekommen BSD-Sockets auf den Sockets des Macs, über Dienst 4 der ARMithlon-Tür. AROSTCP und eine
Netzwerkkarte sind dafür nicht nötig. Die Schicht (`tools/blender/port/qrx_socket.c`) stammt aus der Tor-Portierung,
löst Namen aber über den Host auf. Die Tür-Änderung für native Programme ist dieselbe wie dort. Damit hat Python echte
`_socket`- und `select`-Module.

Geprüft im Gast mit Python, alle 7 Punkte bestanden: `localhost` auflösen, Zufall vom Host, HTTP-Abruf von einem
Server auf dem Mac, `select`, ein Echo-Server im Gast mit Client in einem zweiten Thread, `example.com` auflösen und
per HTTP laden.

Grenzen: nur IPv4, höchstens 64 Sockets, kein `sendmsg`/`recvmsg`, kein TLS, weil OpenSSL noch nicht gebaut ist. Wer
wartet, fragt alle 2 ms nach. Die Engine im Bundle braucht diese Tür-Änderung, sie kommt mit dem nächsten Bau aus dem
Branch.

### Offene Entscheidungen

1. **Oberfläche und OpenGL:** Für die GUI braucht es eine Host-GL-Tür für 68k-Programme in der Engine, oder man
   nimmt Mesa im Gast. Bei den gemessenen Zeiten halte ich Mesa für unbrauchbar, und der GUI-Weg hängt außerdem an Python 3
   und einer C++-Standardbibliothek.
2. **Distribution:** `blender -b` ist ein Kommandozeilenprogramm. Es lohnt sich erst nach dem Engine-Update, weil
   es sonst falsche Bilder liefert. Ein Eintrag im App-Menü ergibt vor der Oberfläche wenig Sinn.
