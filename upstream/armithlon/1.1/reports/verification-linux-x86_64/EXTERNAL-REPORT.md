# ARMITHLON 1.1 – External Benchmark Report

Host: Linux x86_64 | 2026-10-31T20:57:25+00:00

| Tool | Status | Score | Bedeutung |
|---|---|---:|---|
| glmark2 | SKIP | — | native OpenGL (Linux); eigener Tool-Score |
| vkmark | SKIP | — | native Vulkan (Linux); eigener Tool-Score |
| geekbench7 | NOT RUN | — | native CPU / GPU Compute, keine 3D-FPS |

**Nicht vergleichbar:** Die Punktzahlen der Tools sowie ARMITHLON-CPU-, GPU- und Encoder-FPS messen unterschiedliche Lasten.
Geekbench 7 ist proprietär. GUI-Importwerte sind manuell eingegeben und wurden nicht automatisch verifiziert.
Die hier gemessenen OpenGL/Vulkan/CPU-Werte betreffen den **nativen Host**, nicht den QRX68100/QRX86-Gast.

## Offen gelassene Pfade

- QRX86-D3D9-HLE: nicht implementiert. Die MIT-D3D9-Referenz liegt separat unter `benchmarks/direct3d9`. 
- Binärkompatible 3dfx Glide-, Warp3D- und AROS-Treiber-Tests: fehlen noch. Die vorhandenen QRX-Tests messen eigene Adapter.
