# ARMITHLON 1.1 – sämtliche angesprochenen Benchmark-Werkzeuge

Dieses Entwicklerpaket enthält **alle ARMITHLON-eigenen Benchmarks inklusive Source** und
Starter/Installer/Parser für zwei zusätzliche quelloffene Tools. **Geekbench 7 ist
proprietär** und darf nicht als angeblich quelloffenes Tool mitgeliefert werden.
Die offizielle GUI wird vom Benutzer separat installiert.

## 0. Unterschiede der Messungen

| System | Programm | Was wird gemessen? | Lauffähigkeit |
| --- | --- | --- | --- |
| QRX68100/3dfx-artig | `qrx_bench_api glide-mmio` | Guest-MMIO-Command-Encoding, **keine echte Glide-DLL** | macOS/Linux |
| QRX86 OpenGL HLE | `qrx_bench_api opengl-hle` | Host-HLE-Command-Encodierung, kein PE32-Programm | macOS/Linux |
| Software-Rasterizer | `qrx_bench_api cpu` | Vollständig gerenderte CPU-Pixel | macOS/Linux |
| Apple Metal | `qrx_bench_metal` | GPU Offscreen mit Readback; Ausführung am M1 noch zu bestätigen | macOS (Apple GPU) |
| Native Vulkan | `qrx_vulkan_3d` | GPU Offscreen; SDK/ICD + Shader-Compiler nötig | Linux/Vulkan-on-Metal |
| Windows Direct3D9 | `benchmarks/direct3d9/qrx_d3d9_benchmark.c` | Separate native Win32-Referenz; **nicht** QRX86 D3D9 | Windows + D3D9 |
| glmark2 | offizielles `glmark2` | Linux-Host OpenGL 2.0/ES 2.0 | Linux X11/Wayland |
| vkmark | offizielles `vkmark` | Linux-Host Vulkan | Linux X11/Wayland/KMS |
| Geekbench 7 | offizielles Geekbench 7 | Native CPU-Single-/Multi- + GPU-**Compute** | macOS Apple Silicon, weitere OS |

**Keine Score-Konvertierung zwischen Tools.** Host-Metal/Geekbench-Werte sind
keine Benchmarkwerte innerhalb eines 68k- oder x86-Gasts. Geekbench GPU Compute
ist keine Echtzeit-3D-Leistung. Ein fehlendes Tool wird als SKIP statt 0 FPS geführt.

## 1. macOS M1 / macOS Apple Silicon

```bash
xcode-select --install
brew install cmake python3
./armithlon build
./armithlon test
./armithlon benchmark-all 120
./armithlon benchmark-tools status
./armithlon benchmark-geekbench
```

Metal ist der native Renderpfad. Vulkan erfordert ein separat korrekt eingerichtetes
Vulkan SDK/ICD mit Shader-Compiler. `glmark2` und `vkmark` haben upstream derzeit
**keine offiziell dokumentierten nativen macOS-Backends**: Source und Linux-Build-
Unterstützung sind integriert, aber das Paket behauptet keinen M1-Port.
Falls eine lokal angepasste ausführbare Version existiert, kann man sie explizit
mit `ARMITHLON_GLMARK2_BIN=/path/...` oder `ARMITHLON_VKMARK_BIN=/path/...` starten.

**Geekbench 7** (im Oktober 2026 aktuell): https://www.geekbench.com/download/mac/

- Die **Free Edition** bietet CPU- und GPU-Compute-Tests im grafischen Programm.
- Eine automatisch steuerbare Kommandozeilenversion ist Bestandteil von **Geekbench 7 Pro**;
  deshalb startet das Paket kein nicht dokumentiertes CLI und lädt keine proprietären Dateien herunter.
- Starten: `./armithlon benchmark-geekbench` (nach Installation in `/Applications`).
- Die echten Werte von Geekbench im GUI/Browser ablesen und **mit Quelle** eingeben:

```bash
./armithlon benchmark-import-geekbench \
  --cpu-single 2500 --cpu-multi 8000 --gpu-metal 40000 \
  --source 'Meine Messung in Geekbench 7 am M1 (Beispielwerte ersetzen!)'
```

**Die dargestellten Zahlen sind ausdrücklich synthetische Kommando-Beispiele;
niemals in einem Report als realen Messwert ausgeben.** Das Skript schreibt erst nach
wirklichem Aufruf; es prüft den Score nicht remote. Im Report `status=MANUAL`.
Bei Pro-Lizenz kann eine spätere offizielle CLI-Automatisierung ergänzt werden.

## 2. Linux ARM64 (später ARMITHLON-Native-Vulkan-Host)

Hostabhängige Pakete, beispielsweise Debian/Ubuntu:

```bash
sudo apt install git meson ninja-build pkg-config libpng-dev libjpeg-dev \
  libgl1-mesa-dev libx11-dev libxcb1-dev libxcb-icccm4-dev \
  libvulkan-dev libglm-dev libassimp-dev libwayland-dev \
  wayland-protocols libdrm-dev libgbm-dev
./armithlon benchmark-tools fetch all
./armithlon benchmark-tools build all
./armithlon benchmark-tools run all
```

Die beiden offiziellen Quellen werden **erst bei explizitem `fetch`** lokal geholt,
und der tatsächlich verwendete Git-Commit wird in `data/third_party/sources.lock.json`
festgehalten. Kein automatisches `git pull` bei Build oder Test. Build-Anforderungen
hängen von Linux-Distribution, Grafiksystem und Treiber ab; die Installation wird nicht
garantiert, sondern fehlgeschlagene Builds werden als Fehler ausgegeben.

- glmark2: https://github.com/glmark2/glmark2 — GPL-3.0.
- vkmark: https://github.com/vkmark/vkmark — LGPL-2.1; mehrere Dependencies/Backends.
- Jeweilige Upstream-Lizenz bei Weiterverteilung beachten. Keine kopierten Drittanbieter-Binärdateien im ZIP.

## 3. Ergebnisdateien

```text
reports/latest/
  results.json          # interne Szene: CPU, MMIO, OpenGL HLE, Metal, Vulkan
  REPORT.md             # interne Benchmark-Auswertung
  external-results.json # externe Benchmarks und manuelle Geekbench-Scores
  EXTERNAL-REPORT.md    # Drittanbieter, getrennt nach Messart
  benchmarks.html       # offline lesbare externe Ergebnisübersicht
  glmark2.log           # nur falls tatsächlich ausgeführt
  vkmark.log            # nur falls tatsächlich ausgeführt
```

Das Quellcodepaket enthält keine vorgetäuschten Benchmark-Ergebnisse.

## 4. Offene Integrationsaufgaben

Für einen **Gast-Benchmark** benötigen wir:
1. vollständige 68k-CPU/AROS-Bootfähigkeit bzw. echte PE32-Ausführung über QRX86;
2. binärkompatible Glide/Warp3D- und D3D9-/OpenGL-Import-Adapter;
3. Gast-seitige Tests mit genau definierten Workloads, separater Host-CPU/GPU-Zeit,
   FPS, Frame-Hash und Referenzbild.

Geeksbench/GPU Host-Scores allein belegen keinen dieser Punkte.
