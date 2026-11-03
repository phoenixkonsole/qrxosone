# ARMITHLON 1.1 – Komplettes Benchmark-Frontend (DevKit)

**NEU:** glmark2, vkmark und Geekbench 7 sind als **getrennt gekennzeichnete native
Host-Benchmarks** in die bestehende ARMITHLON-Benchmark-Suite integriert. Die eigenen
Metal-, Vulkan-, Glide-artigen/QRX3D-, OpenGL-HLE-, CPU- und Direct3D9-Referenzprogramme
bleiben erhalten. Geekbench ist proprietär und wird separat vom Benutzer installiert.

```sh
cd ~/Downloads/ARMITHLON-1.1-macOS
./armithlon build
./armithlon test
./armithlon benchmark-all 60
./armithlon benchmark-tools status
./armithlon benchmark-geekbench
```

**Ausführliche Einrichtung für Mac und Linux:** `benchmarks/THIRD-PARTY.md`.
**Neue Quellen/Tests:** `tools/external-benchmarks.py` und
`tools/test-external-benchmarks.py`.
**Ergebnisübersicht:** `reports/latest/EXTERNAL-REPORT.md` und
`reports/latest/benchmarks.html` (nach Ausführung).

---

# ARMITHLON 1.0 — GPU-Benchmark-Developer-Kit (macOS M1 / Linux)

**Neu:** Reproduzierbare Demo und Performance-Messung für QRX3D (3dfx-artige
MMIO-Kommandos), QRX86 OpenGL-HLE, native Metal- und Vulkan-GPU-Pipelines sowie
ein originales Windows-Direct3D9-Referenzprogramm. Alle Benchmarks mit freiem,
veröffentlichbarem Quellcode im Paket. **Keine erfundenen FPS.**

```bash
xcode-select --install
brew install cmake python3
cd ~/Downloads/ARMITHLON-1.0-macOS
./armithlon build
./armithlon test
./armithlon benchmark
```

Für Metal ist das Apple-GPU-Framework ausreichend. Für Vulkan müssen ein Vulkan
SDK, Loader und ICD plus `glslangValidator` installiert werden. Übersetzungs-FPS
sind **kein** Ersatz für echte GPU-Render-FPS. Ein fehlendes Backend wird in den
Ergebnissen als SKIP ausgewiesen, nicht als PASS.

Die Ergebnisse liegen in `reports/latest/results.json`, `REPORT.md`, dazu
Referenz-/GPU-Screenshots (PPM). Für eine kurze Prüfung: `./armithlon benchmark-quick`.
Die detaillierte API-Matrix, Bedeutung der Messwerte und Open-Source-Beispiele
stehen in `benchmarks/README.md`.

**Warnung:** Der native QRX68100 emuliert weiterhin nur einen begrenzten Teil
der 68k-CPU und bootet QRX OS One/AROS noch nicht. Die bisherigen FS-UAE-Starts
sind ein separater Kompatibilitätsmodus. `glide2x.dll`, Warp3D und D3D9-HLE
sind noch nicht implementiert. Auch die unveränderte Ausführung einer
Windows-OpenGL-EXE mit QRX86 ist noch nicht auf dem M1 verifiziert.

---

# ARMITHLON 0.9 für macOS Apple Silicon / Linux ARM64 – Entwicklerstand

**Kumulativer Source seit ARMITHLON 0.3.** Der komplette 0.8-Media-/CPU-/Metal-/
QRX86-Quellcode wurde beibehalten; 0.9 ergänzt erste OpenGL-Kommandos und einen
optionalen Vulkan-Grafikpipeline-Test. Es existiert noch kein chipsetloser
QRX-OS-One-/AROS-Boot auf dem QRX68100. FS-UAE bleibt ein separater AROS-
Kompatibilitätsmodus. Weder ARM64-JIT-Vollständigkeit noch Windows-/Linux-
Binärkompatibilität sind erreicht.

## Mac: CPU/Metal-Fenster und OpenGL-Bridge testen

1. Xcode Command Line Tools und CMake installieren:

```sh
xcode-select --install
brew install cmake gh
```

2. Dieses ZIP entpacken. Im Terminal:

```sh
cd ~/Downloads/ARMITHLON-0.9-macOS
./armithlon build
./armithlon test
./armithlon run-native
```

`run-native` ist das bereits aus 0.8 bekannte Metal-/CoreAudio-/Cocoa-
Diagnosefenster. Keine echte QRX-OS-One-Oberfläche. `build-app` erstellt eine
lokale Finder-App. Die macOS-Version wurde hier nicht kompiliert oder gestartet.

## Neue 0.9-Tests (auf macOS und Linux möglich)

```sh
./armithlon qrx86-gl-test
```

- QRX86-OpenGL-Teilmenge: Farben, Dreiecke/Quads, Z-/Blendflags,
  Befehlsreihenfolge, Grenzwerte.
- Unix-Socket `Q86C`: Eingefangene CPU-unabhängige GPU-Befehle werden auf
  Client-/Server-Seite binär übertragen und wieder aufgebaut.
- Optionaler OpenGL-Import-Patch: wiederholbare Anwendung und Quellcode-
  Signaturprüfung.

## OpenGL-Importe von QRX86 (experimentell)

QRX86 22.5.12 besitzt weiterhin seine DirectDraw-/DirectSound-/DirectInput-
Win32-HLE. Der neue lokale Patch erweitert das Importverzeichnis um eine
**kleine** Teilmenge `opengl32.dll` (`glClear`, `glBegin`, `glEnd`,
`glVertex2f/3f`, `glColor3f/4f`, `glOrtho`, `glEnable/Disable`, `glFlush`),
WGL-Ein-Kontext-Stubs sowie `gdi32.SwapBuffers`. Das ist **keine universelle
OpenGL-Kompatibilität**; Programme mit glMatrixMode, glTexImage2D, GL_EXT,
mehreren Kontexten oder modernen Shadern werden nicht funktionieren.

GitHub-Zugriff auf das private Repository über GitHub CLI:

```sh
brew install gh
gh auth login
./armithlon fetch
./armithlon build-qrx86-host
```

Der Adapter wird ausschließlich in der lokalen Source-Kopie installiert; der
Branch `claude/qrx-22.5.11` wird nicht verändert. Die Buildumgebung hier kann
nicht bestätigen, dass der gesamte native QRX86-Build oder eine Windows-EXE
bereits funktioniert. Der vollständige Test benötigt dein M1.

Zum Anzeigen von Befehlen/GDI-Bildern im Metal-Fenster:

**Terminal 1**:

```sh
./armithlon run-native-qrx86
```

**Terminal 2** (nach erfolgreichem `build-qrx86-host`):

```sh
./armithlon run-qrx86 /PFAD/ZU/einer_unterstuetzten_32bit.exe
```

Die OpenGL-EXE muss selbst die oben genannte Teilmenge verwenden. Ein
willkürliches Windows-Spiel oder `glide2x.dll` ist ausdrücklich nicht
unterstützt. Teste bekannte Win32-HLE-Beispiele zuerst.

## Vulkan 3D – getrennte native GPU-Prüfung

Das offscreen-Vulkan-3D-Programm erstellt Grafik-Pipelines mit
Vertex-/Fragment-SPIR-V aus eigenen GLSL-Dateien. Dreiecke werden als
**echte Vulkan `vkCmdDraw`**-Befehle eingeplant, nicht als CPU-gerasterte
Pixel an eine GPU übertragen. Zustandsvarianten für Tiefentest/-schreiben und
Alpha-Blending sind angelegt. Texturen, WSI/Swapchain und Fensterausgabe
fehlen noch.

Benötigt: Vulkan-SDK mit Vulkan-Loader und einem lauffähigen ICD (macOS:
Vulkan-on-Metal-Treiber), zusätzlich `glslangValidator`. Das SDK liefert
gewöhnlich die `glslangValidator`-CLI; bei Bedarf separat installieren.

```sh
./armithlon build
./armithlon vulkan3d-test
```

Screenshot: `data/vulkan/qrx-vulkan3d-0.9.ppm`. Ein Prüfer kontrolliert Format,
Hintergrund und Dreieck. Dieser Grafikpfad wurde hier **nicht** kompiliert oder
GPU-getestet; wir haben in der Arbeitsumgebung weder Vulkan-SDK noch ICD.
Behebe auftretende Pipeline-/Shaderfehler vor jeglicher Distribution.

Der ältere 2D-Transfer-Test bleibt verfügbar:

```sh
./armithlon vulkan-test
```

## Vertriebsziel und Integrationsgrenzen

- macOS: eigenes Metal-Frontend als Ausgangsbasis.
- Linux ARM64: künftiges Vulkan-Frontend; das aktuelle Vulkan-Programm ist
  nur ein Offscreen-Diagnosetest, kein Linux-Desktop-Frontend.
- ARMITHLON: vollständiges AROS Bootziel und RTG/AHI/Input-Treiber fehlen.
- Windows (QRX86): nur experimentelle Win32-API-Teilimplementierung; keine
  DLL-Übernahme von DXVK/vkd3d-proton ohne korrektes x86→native ABI.
- Sicherheit: lokale Development-Sockets sind **keine** Privilegiengrenze.

Siehe `CHANGELOG-0.9.md` für den verifizierten Stand und `integration/QRX86-OPENGL-0.9.md`
für ABI und Architektur der Import-Hooks.
