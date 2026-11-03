# ARMITHLON 1.5 — QRX68100 = 2 × 68040 für QRX OS One

Die QRX68100-CPU ist nun als **fiktiver Zweikern-Prozessor** definiert. Beide Kerne besitzen eigene Musashi-68040-Registerzustände und verwenden einen gemeinsamen, kohärenten RAM-/ROM-/MMIO-Bus.

**AmigaOS 3.x:** Beim Einschalten ist ausschließlich Kern 0 sichtbar. Der zweite Kern bleibt geparkt und die QRX-spezifischen SMP-Register sind nicht freigegeben. Bestehende Programme sehen einen normalen 68040-Prozessorkern. Achtung: Das bedeutet noch nicht, dass AmigaOS 3.x auf unserem Board ohne Amiga-Chipsatz bootet.

**QRX OS One:** Ein angepasster QRX-Bootloader/-Kernel kann bei explizit aktiviertem Dual-Modus Kern 1 mit eigener Stack-Adresse und eigenem Entry Point starten. Aufrufe werden derzeit deterministisch in Musashi-Kontexten zeitgeteilt. Das ist **noch keine parallele Ausführung auf zwei Host-Threads**.

Für JIT-Anwendungen gibt es einen **4-fach assoziativen Hot-Block-Cache** mit getrennten Einträgen pro Kern, Adressraumkennung und ISA (68k, x86, PPC, x64, ARM), inklusive Invalidierung bei Selbstmodifikation. Dieser Cache speichert aktuell Metadaten, **noch keinen ausführbaren ARM64-JIT-Code**. Die angestrebten virtuellen 32-KiB-I-/32-KiB-D-Caches und 512-KiB-L2 sind bis zur Implementierung lediglich Architekturziele.

## Testen

```bash
cd ~/Downloads/ARMITHLON-1.5-macOS
./armithlon build
./armithlon test                   # hermetischer Bus-/SMP-Test mit CPU-Mock
./armithlon full68k-setup          # Musashi-Pin laden
./armithlon full68k-build          # echten 68000..68040-Kern bauen
./armithlon dualcore-test          # Test beider echten 68040-Kontexte
./armithlon dualcore-demo          # Legacy: Kern 0
./armithlon dualcore-demo dual     # QRX-Modus: beide Kerne
```

Detaillierte Registerbelegung, JIT-Cache-Kohärenz, Grenzen und der SMP-Kernel-Pfad: `engine/docs/QRX68100_DUALCORE_1_5.md`.

**Kein Erfolg vorgetäuscht:** In der Entwicklungsumgebung wurden 23/23 lokale CTests mit CPU-Mock bestanden. Die echte Musashi-Dual-Ausführung, der AROS-/AmigaOS-Boot sowie native macOS/M1-Benchmarks bleiben zu prüfen.
