# ARMITHLON 1.1 – Benchmark Suite Erweiterung

## Implementiert

- Neue `benchmark-tools` Befehlsfamilie für **glmark2** und **vkmark**: Status,
  optionaler Git-Checkout, Quellcode-Build (Linux), native Ausführung, Log/Score-Parser.
- Upstream-Lizenzen getrennt von eigenem MIT-Quellcode. Keine externen Binaries/Archive
  oder Geekbench-Dateien ungefragt in die Distribution kopiert.
- Kein automatisches Drittanbieter-Update; Git-Commit-Provenienz in einer Lock-Datei.
- Geekbench **7**, aktuelle Produktversion, als proprietären separaten GUI-Benchmark
  dokumentiert. Expliziter GUI-Starter für macOS; manuelle, als unverifiziert
  gekennzeichnete CPU-/GPU-Score-Übernahme mit Benutzer-Quellenangabe.
- `benchmark-all`: eigene CPU/GPU/Glide-/OpenGL-Komponenten und installierte
  Drittanbieter-Benchmarks; nicht vorhandene Backends werden ehrlich übersprungen.
- Separate Ergebnisberichte `external-results.json`, `EXTERNAL-REPORT.md` und
  offline lesbare `benchmarks.html` zur Vermeidung falscher FPS/Score-Vergleiche.
- 11 hermetische Unit-Tests für Parsing, Fehlerstatus, MANUAL-/SKIP-Kennzeichnung,
  Quellbericht und rohe Logs; als eigener CTest eingebunden.
- Optionaler neuer Windows-PE32-OpenGL-Smoke-Benchmark mit echtem `opengl32`-Import-
  Quellcode und MinGW-Buildskript. Der bestehende HLE-Gastpfad bleibt unverändert;
  M1-Gast-Lauftest steht aus.
- Keine Änderung an CPU, JIT, Metal-, Vulkan-, QRX86-/AROS-Laufzeit-Quellcode.

## Ehrliche Grenzen

- glmark2/vkmark sind upstream Linux-X11/Wayland/KMS-orientiert; kein nativer
  M1-GUI-Build im Paket. Mac nutzt den vorhandenen Metal-Benchmark.
- Geekbench 7 Free enthält keinen offiziellen Automations-CLI-Zugang.
- Hier keine M1-GPU/Geekbench/glmark2/vkmark-Ausführung; Tests prüfen die
  Integration und vorhandene Headless-Qrx-Komponenten auf Linux x86-64.
- QRX86-OpenGL-PE32, Glide-binärcompat und AROS-Boot weiterhin ausstehend.
