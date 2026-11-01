# ARMITHLON 1.7 – QRX68100 Vampire-inspiriertes 2×68040-Design

**Ziel:** QRX68100 hat zwei unabhängige 68040-kompatible CPU-Kontexte (Kern 0 für AmigaOS 3.x, Kern 1 nur QRX-SMP) und erhält jetzt eigene Erweiterungen nach dem Vorbild des Apollo 68080/Vampire. Es wird **kein proprietärer Apollo-FPGA-Kern** übernommen. Die Schnittstelle `QRX-VEC64` ist **nicht binärkompatibel mit Apollo AMMX**.

## Echte Änderungen im Source

- **16 KiB Instruktionscache + 32 KiB Datencache pro Kern:** tatsächlich implementierte 4-fach assoziative, 64-Byte-Zeilen umfassende, kohärente L1-Caches für **QRX-JIT-/Decoder-Zugriffe**. Abfrage via `qrx_vamp_cached_read`, pro-Kern Hit-/Miss-Zähler; Bus- und DMA-Schreibvorgänge invalidieren betroffene Cachezeilen aller Kerne. Die vorhandenen Musashi-68040-Fetches verwenden diesen Cache noch **nicht**. Nicht verwechseln mit einem simulierten 68080-Zeit-/Pipeline-Modell.
- **Dynamischer Branch Predictor:** pro Kern 256 2-Bit-Sättigungszähler und Hit-/Miss-Diagnose. QRX-JIT kann die API verwenden; nicht mit gemessener 68080-Branch-Prediction-Performance gleichsetzen.
- **24×64-Bit-Vektorregister pro Kern:** neue QRX-spezifische Extension `QRX-VEC64`, frei adressierbar über eine MMIO-Registerbank und eine native JIT-API. SIMD-Operationen: `PADDB`, `PADDW`, `PSUBB`, `PXOR`, `PAND`, `POR`, `PAVGB`, `PADDSB`; außerdem unsigned `MUL64` (niedrige 64 Bit) und `DIV64` (Division durch 0 wird abgelehnt). Drei unabhängige Registerindizes `src`, `src2`, `dst` erlauben destruktionsfreie Vektorberechnung.
- **JIT-Fusion/Loop-Optimierung:** die vorhandene, streng geprüfte Dual-Worker-Schleife `ADDQ.L #n,Dx; BRA.S -4` wurde um **`SUBQ.L #n,Dx; BRA.S -4`** erweitert, inklusive exakter 32-Bit-X/N/Z/V/C-Flags und neu übersetztem ARM64-`SUB`. Keine allgemeine Instruktionsfusion und kein superskalarer 68040-Core.
- **Legacy-Schutz:** beim Power-on und im AmigaOS-3.x-Modus sind Kern 1 sowie die neuen `0xFF000700`-QRX-Register unsichtbar. Muss explizit vom QRX-Bootloader freigeschaltet werden. Umschalten in den Legacy-Modus löscht die erweiterten Vektorregister; Neustart von Kern 1 löscht dessen Vektorzustand.
- **FPU:** Das 68040-Musashi-FPU-Backend bleibt erhalten. Die spezielle 68080-64-Bit-FPU und AMMX-Opcodes, 68060-Erweiterungen, MMU-Details und die superskalare Apollo-Pipeline wurden **nicht** nachimplementiert.

## Testen (Apple M1 oder Linux)

```bash
cd ~/Downloads/ARMITHLON-1.7-macOS
./armithlon build
./armithlon test
./armithlon vampire-test
./armithlon vampire-bench 1000000
./armithlon dualcore-parallel-test
```

Optional – echter 68040/Musashi-Kern, nur bei vorhandener Internetverbindung:

```bash
./armithlon full68k-setup
./armithlon full68k-build
./armithlon full68k-test
./armithlon dualcore-parallel-bench 10000000
```

Der Microbenchmark `vampire-bench` misst **native Host-C-API-Ausführung mit einem Musashi-Stub** – **keine 68k-/Apollo-68080-/M1-JIT-Performance**. Ein isolierter A64-JIT wird weiterhin nur für die zuvor definierten Loop-Muster erzeugt. GPU und Geekbench behalten ihre bestehenden getrennten Messungen.

## QRX-Register / Programmiermodell

Siehe `engine/full68k/qrx68100_vampire.h` sowie `engine/docs/QRX68100_VAMPIRE_1_7.md`. Neue QRX-only MMIO-Basis `0xFF000700`, innerhalb des bekannten VRAM-freien QRX-I/O-Fensters. Hardwarezugriffe auf `CTRL` und explizites `CACHE_INV` erfordern 68040-Supervisor-Rechte; normale Vektoroperationen sind im QRX-Modus auch aus Userspace erlaubt. Die CPU-Identifikation bleibt im Legacy-Modus unverändert **68040**, kein neues AmigaOS-CPU-Erkennungsbit.

## Vorbild und Unterschied

Die Apollo-Team-Spezifikation nennt für den 68080 u. a. 16 KiB I-Cache, 32 KiB D-Cache, AMMX 64-Bit-SIMD, 64-Bit-Multiplikation/Division, Branch Prediction, Instruction Fusing und bis zu 4 Instruktionen/Takt (generationabhängig); daneben 68040/68060-FPU-Unterstützung. Quelle: https://wiki.apollo-computer.com/doku.php?id=apollo_core:start und https://www.apollo-computer.com/features.php. Die Vampire-SAGA-Grafik ist ein **Chipset**, kein CPU-Befehlssatz – deshalb weiter über unser QRXGPU-/Metal-/Vulkan-Projekt getrennt.

## Ehrliche Grenzen

- Kein 68080-kompatibles AMMX-Line-F-Opcode-Encoding; Apollo-spezifische Software erfordert weiterhin Anpassungen.
- Cache-Frontend ist funktional und kohärent, aber **kein** vollständiger 68040/68080-Pipeline-/Bus-Timing-Emulator und kein belegter IPC-Gewinn.
- Kein physikalisches SAGA, Maggie, Akiko oder Full-Amiga-Chipset.
- Kein echtes gleichzeitiges Musashi-`m68k_execute()` auf 2 Threads; nur qualifizierte JIT-Loop-Blöcke laufen parallel.
- x86/PPC/x64/ARM-Tags bezeichnen JIT-Cache-Metadaten; kein fertiger fremd-ISA-zu-ARM64-Übersetzer.
- Vollständiger nativer QRX-/AmigaOS-3.x-/AROS-Boot und reale Apple-M1-PrBoom-Messungen stehen aus.

**Verifikation:** Linux x86-64: 25/25 lokale CTests und 25/25 unter AddressSanitizer/UBSan, inklusive Cache-Kohärenz, 64-Bit-Rechenoperationen, Vektorisierung, Legacy/SMP-Isolation und ADDQ/SUBQ-JIT-Fallback. Musashi-Source wurde in dieser Testumgebung nicht heruntergeladen; die neuen Extensiontests verwenden eine CPU-Testattrappe, keine vollständige 68k-ISA-Ausführung.
