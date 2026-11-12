# Blender 2.79b für QRX OS One: Bauen und Testen

Plan, Stand und Messungen stehen in [docs/BLENDER_AROS68K.md](../../docs/BLENDER_AROS68K.md).

```bash
tools/blender/build.sh
```

Das lädt `blender-2.79b.tar.gz` von download.blender.org (50,2 MB) und `gcc-16.2.0.tar.xz` von ftp.gnu.org
(107,2 MB), wendet den Patch an und baut in das
ungetrackte `.build/blender-2.79b`. Der Bau startet die Engine im Hintergrund, denn `makesdna` und
`makesrna` laufen als 68k-Programme im Gast.

| Datei | Zweck |
|---|---|
| `patches/blender-2.79b-qrx.patch` | alle Änderungen am Blender-Quellcode |
| `aros_cc.py`, `aros-c++`, `toolchain.cmake`, `configure.sh` | Compiler-Treiber und CMake für das 68k-Ziel |
| `build_port.py`, `port/`, `shim/` | Port: Halde, Kern 1, libc, ABI-Brücken, msun, fehlende POSIX-Header |
| `build_libstdcxx.sh` | `libstdc++` aus GCC 16.2 für das AROS-SDK, ohne Exceptions, RTTI und Threads |
| `build_python.py`, `patches/python-3.5.10-qrx.patch` | CPython 3.5.10 statisch für 68k-AROS |
| `port/qrx_socket.c` | BSD-Sockets auf den Sockets des Macs über die ARMithlon-Tür |
| `merge-sections.ld` | fasst die Abschnitte pro Funktion zusammen, die sonst alle zu AROS-Hunks würden |
| `run_tool.py` | führt `datatoc` auf dem Mac, `makesdna` und `makesrna` im Gast aus |
| `ghost/` | GHOST für AROS: Intuition-Fenster, Ereignisse, GL-Kontext über die Tür (`ghost_aros_native.c` ist der Intuition-Teil) |
| `../qrxgl68k/` | die GL-Tür für 68k-Programme: Generator, Gastbibliothek `libqrxgl.a`, Testszene |
| `run-gui.sh` | Blender mit Oberfläche im Engine-Fenster starten, Bilder vom Gast-Bildschirm und aus der GL-Tür aufnehmen |
| `gen_missing.py` | Stubs aus der Liste undefinierter Symbole (aus der kopflosen Stufe, für GHOST und GL nicht mehr nötig) |
| `elf2hunk-pc32.diff` | `elf2hunk` für große Programme |
| `run-guest.sh`, `run-job.sh` | Gast ohne Fenster booten und ein AmigaDOS-Skript ausführen |
| `configure-host.sh`, `host_stubs.sh` | gleiche Konfiguration nativ auf dem Mac, als Referenzbild |
| `compare_png.py`, `pixeldiff.py` | Bilder pixelweise vergleichen |
| `guest_stack.py` | Task-Listen und Stacks aus einem RAM-Abzug lesen, wenn der Gast hängt; statt der Adresse von `main` geht auch `-` |
| `autolog.py` | Log-Punkte in eine Funktion schreiben, die im Engine-Log landen |

## Eine Szene im Gast rendern

Job-Skript, zum Beispiel `job.txt`:

```
C:QRXMoreMemory
C:Stack 8000000
SetEnv QRX_STDERR_TO_STDOUT 1
SYS:blender -b APPS:test/startup.blend -o APPS:out/bild_#### -F PNG -f 1 >SYS:bl.log
```

```bash
tools/blender/run-job.sh job.txt 300
```

`APPS:` ist `.build/blender-2.79b`, `SYS:` der Testbaum `tree`. `-t 1` oder `SetEnv QRX_NO_CORE1 1` lässt Kern 1 weg.

Nur zum Testen, weil ein Build ohne Python Szenen nicht anders ändern kann: `QRX_RENDER=BxHxTILE` setzt
Bild- und Kachelgröße, `QRX_RENDER_RAY=1` schaltet Raytracing, Strahlschatten und Spiegel ein.
`QRX_CORE1_FAULT_TEST=1` lässt Kern 1 nach drei Kacheln absichtlich mit einem Fehler stehen bleiben.

## Mit Oberfläche

```bash
QRX_TEST_CLICKS=100:1000:300,112:64:30 tools/blender/run-gui.sh .build/blender-2.79b/jobs/gui.txt 125 105 2
```

Startet Blender im Engine-Fenster und nimmt nach 105 s zwei Bilder des Gast-Bildschirms auf (`guitree/gui.ppm.N`, 14 s Abstand
mit `QRX_RTG_CAPTURE_STEP=14`). `guitree/glframe.NNNN` sind die Bilder, die Blender über die GL-Tür präsentiert hat. Der Job braucht
`CD SYS:`, sonst findet Blender seine Skripte nicht. `QRX_GHOST_TRACE` (im Job mit `SetEnv`) protokolliert GHOST-Ereignisse,
`QRX_GL68K_TRACE=n` die ersten n GL-Aufrufe im Engine-Log.

## Python

```bash
tools/blender/build_python.sh
```

Das lädt `Python-3.5.10.tar.xz` von python.org (15,4 MB), wendet den Patch an, konfiguriert und baut nach
`.build/blender-2.79b/py/build/python.elf`. Im Testbaum gehört das Programm nach `SYS:python` und `Lib` nach
`SYS:lib/python3.5`. Achtung bei `-c` in AmigaDOS: `*` ist dort das Escape-Zeichen, `"print(6*7)"` gibt 67 aus.

## Fallen

- Ein Programm ohne Bildschirm hängt bei jedem DOS-Requester, deshalb setzt der Port `pr_WindowPtr` auf -1.
- Ein AROS-Absturz steht im Engine-Log (`tree/engine.log`) als "Software Failure". Hängt der Gast ohne das,
  hilft `QRX_FULL_DUMP_RAM=0:10000000:ram.bin` mit `guest_stack.py`; die Lade-Adresse von `main` steht mit
  `QRX_STDERR_TO_STDOUT` im Engine-Log.
- Ein Abzug braucht `C:QRXMoreMemory` im Job, sonst ist das RAM kleiner als der angeforderte Bereich und nichts wird geschrieben.
- Neue SDK-Funktionen, die Zeiger oder `double` liefern, brauchen eine Brücke. `build_port.py` erzeugt sie aus
  den Prototypen, sucht aber nur in den Headern und Bibliotheken, die dort aufgelistet sind.
