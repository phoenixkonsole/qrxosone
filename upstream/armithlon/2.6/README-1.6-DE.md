# ARMITHLON 1.6 — QRX68100 / erste parallele ARM64-JIT-Ausführung

## Warum dieser Weg?

Musashi verwendet im Standardzustand **prozessglobale CPU-Zustände** und ist nicht für das unmittelbare gleichzeitige Ausführen zweier `m68k_execute()`-Aufrufe konzipiert. Zwei Host-Threads auf dasselbe Musashi-Objekt loszulassen, wäre nicht korrekt. Der vorliegende Zwischenschritt hält beide vollständigen 68040-Kontexte serialisiert, **beschleunigt aber ausdrücklich freigegebene reine ALU-Schleifen auf zwei echten Worker-Threads**.

Die erste freigegebene 68k-Mikro-Sequenz: `ADDQ.L #n,Dx; BRA.S -4`. Der Decoder verifiziert Bytefolgen und Seitenbereiche, lehnt jedes andere Opcode ab und ist nur in `QRX_BOOT_QRX_DUAL` aktiv. Die Arbeiter lesen oder schreiben **keine Gast-RAM-Adressen**; selbstmodifizierter Code ändert die Seitengeneration und invalidiert den gecachten nativen Block beim nächsten Aufruf. Interrupts und Timer deaktivieren den beschleunigten Pfad. Die native Routine arbeitet registerbasiert, exakt mit 32-bit Überlauf; Statusbits werden nach dem Parallel-Join korrekt rekonstruiert.

**M1/JIT:** Pro Kern wird eine kleine AArch64-Maschinenfunktion per `mmap(MAP_JIT)`, `pthread_jit_write_protect_np`, `sys_icache_invalidate` und W^X erstellt. Der native Code enthält eine tatsächliche Zählschleife (`ADD`, `SUBS`, `B.NE`) statt eines vorab multiplizierten Ergebnisses. Beide Funktionen laufen mit `pthread_create`/`pthread_join` nebenläufig. Auf x86_64 ersetzt C die ARM64-Emission — dieses Ergebnis darf nicht als M1-JIT-Performance interpretiert werden.

**Legacy:** Beim Booten bleiben Core 1 und QRX-SMP-Hardware für AmigaOS 3.x unsichtbar. Der neue Pfad ist dort niemals aktiv.

## Testbefehle

```bash
cd ~/Downloads/ARMITHLON-1.6-macOS
./armithlon build
./armithlon test
./armithlon dualcore-parallel-test             # lokaler CPU-Mock + echte pthreads
./armithlon full68k-setup                      # sha-pinned Original-Musashi holen
./armithlon full68k-build                      # reale CPU-Tests und Benchmark bauen
./armithlon full68k-test                       # alle CTests, inkl. 68040-Kern
./armithlon dualcore-parallel-bench 10000000   # ein echter Messwert auf deinem M1
```

Der Benchmark schreibt eine JSON-Zeile und markiert seine Messgrößen als **isolierte optimierte 68k-Loop-Operationen**. Die Werte sind nicht vergleichbar mit 68040-Taktfrequenzen, PrBoom, Geekbench oder beliebigen Windows-/Linux-Gastprogrammen.

## Was fehlt für echtes QRX SMP + QRX86 JIT?

1. Einen thread-sicheren 68040/68060-CPU-Backend mit getrennten CPU-Kontexten auch während Exceptions, MMIO, DMA und Interrupts; synchronisierte Speicher-/Atomikmodelle (TAS/CAS) und echtes QRX-SMP-BSP (per-CPU Scheduler, IPI, TLB-Shootdown).
2. Einen gemeinsamen IR-Compiler und Codecache für *tatsächliche* QRX86-x86-, PPC-, x64- und ARM-Basic-Blocks. Die aktuellen ISA-Tags sind reine Metadaten; sie sind **noch kein solcher Translator**.
3. Ende-zu-Ende-Gastbenchmarks und reale macOS-M1-Ausführung. Diese Linux-Umgebung kann den A64-Code nur cross-assemblieren, nicht ausführen.
4. AmigaOS-3.x-Board-Kompatibilität beziehungsweise nativen AROS/QRX-Desktop-Boot; der aktuelle Boot über FS-UAE ist ein eigenständiger Fallback.
