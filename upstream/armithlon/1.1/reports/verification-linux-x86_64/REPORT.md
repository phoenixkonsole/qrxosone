# ARMITHLON 1.0 – Benchmark-Ergebnisse

Zeitpunkt: 2026-10-31T20:55:00.919498+00:00 | Host: Linux x86_64 6.18.57

Auflösung: 800 × 450; 240 deckende + 16 transparente Dreiecke; 1 Clear pro Frame.

| API | Messart | Frames | FPS | Status |
|---|---|---:|---:|---|
| cpu | cpu_reference | 10 | 317.3 | PASS |
| glide-mmio | command_translation_only | 10 | 25417.9 | PASS |
| opengl-hle | command_translation_only | 10 | 38299.8 | PASS |
| metal | — | — | — | SKIP |

> metal: macOS mit erfolgreich kompiliertem Metal-Backend erforderlich

| vulkan | — | — | — | SKIP |

> vulkan: Vulkan SDK + glslangValidator + GPU-Treiber erforderlich

| direct3d9-native | — | — | — | SKIP |

> direct3d9-native: Separates Windows/Direct3D9-Programm in benchmarks/direct3d9; QRX86 D3D9-HLE fehlt

| warp3d | — | — | — | SKIP |

> warp3d: Keine Warp3D-Bibliotheksanbindung in ARMITHLON; QRX3D ist nicht binärkompatibel

| glide2x.dll | — | — | — | SKIP |

> glide2x.dll: Originale 3dfx Glide-DLL nicht implementiert; glide-mmio testet nur QRX3D-Adapter


**Wichtig:** Encoder-FPS, CPU-Referenz-FPS und native GPU-FPS sind verschiedene Messgrößen.
QRX86-OpenGL-HLE wird hier auf Hostseite gemessen, nicht als komplette emulierte Windows-Anwendung.
Metal/Vulkan-Werte umfassen CPU-Submit und GPU-Wartezeit (offscreen), keine Display-VSync.
Direct3D9 läuft ausschließlich als separates natives Windows-Referenzprogramm; kein QRX86-D3D9 vorhanden.
3dfx = QRX3D-MMIO im Stil von Glide, keine originale Glide-DLL.
