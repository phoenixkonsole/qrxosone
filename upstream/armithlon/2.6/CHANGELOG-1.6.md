# ARMITHLON 1.6 — echte Host-Threads für einen eingeschränkten QRX68100-JIT-Pfad

**Verantwortlich abgegrenzt:** Dieses Release macht **nicht** den ganzen Musashi-68040-Kern threadsicher und emuliert keine beliebigen 68k-Programme parallel. Stattdessen werden zwei **unabhängige, geprüfte 68k-ADDQ/BRA-Schleifen** in zwei POSIX-Threads gleichzeitig ausgeführt. Auf AArch64 läuft dabei **selbst erzeugter A64-Maschinencode**, auf anderen Plattformen ein C-Fallback. Die Musashi-Zustände werden erst **nach dem Join** auf dem Besitzer-Thread aktualisiert. Der existierende 68040-Interpreter bleibt der Fallback.

- Neuen Codepfad `engine/full68k/qrx68100_parallel.c` und API `qrx68100_parallel.h` eingeführt; kompilierte 8-Instruction-AArch64-Schleife mit W^X/MAP_JIT und I-Cache-Flush.
- Getrennte JIT-Cache-Einträge pro Kern; Validierung über PC, ADDQ-Inkrement, Gastseiten- und globale Cachegeneration; invalidiert bei Änderungen an Code oder QRX-Modus.
- Einfache voll transparente Übersetzung **nur** von `ADDQ.L #1..8,D0..D7; BRA.S -4` im gemeinsamen RAM, mit 32-Bit-Wraparound und korrekten CCR X/N/Z/V/C Flags. Unsupported, IRQs, Timer, Legacy-Modus: kein Fast-Path.
- Zwei Worker erzeugen keine MMIO/RAM-Schreibzugriffe; alle Gastregisteränderungen erfolgen auf dem Musashi-Owner-Thread nach erfolgreichem Abschluss. Keine Guest-Code-Verschiebung über Seitengrenzen.
- Real-Musashi-Benchmark `qrx68100_parallel_bench`: JSON mit tatsächlich gemessener Wall-Time, Host-Backend und qualifizierter Schleifenrate. Keine angeblichen 68040 MHz oder PrBoom FPS.
- `./armithlon dualcore-parallel-test` (mock state): zwei reale Host-Threads, Flag-/Cachetests; `dualcore-parallel-bench N` benötigt den separaten SHA-gepinnten Musashi-Source.
- Prüfung A64-Maschinencode gegen Clang-Assembler-Referenz möglich (Datei `tests/a64-code-reference.s`).

**Begrenzungen:** Kein echter SMP-Scheduler im Gast, keine echte 68040/68k-Vollparallelität, kein x86/PPC/x64/ARM-Translator, kein QRX-/AmigaOS-Boot auf dem chip-satzlosen Board. Mac M1/Musashi-Gast-Ausführung bleibt außerhalb dieser Linux-Testumgebung zu validieren.
