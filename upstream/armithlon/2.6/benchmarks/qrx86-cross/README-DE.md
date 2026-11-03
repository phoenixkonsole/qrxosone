# QRX86-CrossBench – Windows, Linux und native Hosts

`crossbench.c` ist MIT-lizenziert und enthält einen einzigen, gleichwertigen,
über beide i386-Gastsysteme reproduzierbaren, ganzzahligen Algorithmus: 300.000
Iterationen mit Rotationen/Multiplikationen und Zugriffen auf einen 32-KiB-Puffer.
Die Ausgabe ist `QRXBENCH v=1 loops=300000 checksum=903be502`. Jeder andere
Checksum, fehlende Ausgabe oder Exitcode != 0 ist FAIL, nicht ein vermeintlich
schneller Benchmark. Gastinstruktionszahlen erfasst der QRX-Interpreter getrennt.

`./armithlon build-crossbench` kompiliert den aktuellen nativen Host-Baseline.
Auf Linux mit `gcc -m32` wird ELF32 erzeugt; auf macOS liegt ein auf Linux x86_64
kompiliertes, statisches ELF32 im Paket. Auf macOS kann die Windows-Version mit
`brew install mingw-w64` gebaut werden. ELF32 wurde im Linux-Crosscompiler erzeugt,
jedoch ist der direkte native Start in der Build-Umgebung mangels i386-ABI nicht
möglich. QRX86-Ausführung folgt erst nach erfolgreichem Runtime-Build.

`./armithlon benchmark-qrx86` führt den nativen Host sowie die vorhandenen
Windows- und Linux-Gäste jeweils dreimal aus und speichert
`reports/latest/execution-results.json` und `execution-dashboard.html`.

## Fairer Hardwarevergleich

Ohne reale Messungen keine Balken. Die Liste 486DX2, Pentium, 68060, 68080,
Pentium 4, Core i7 und Apple M1 ist ausdrücklich eine Vergleichs-Vorlage.
Ein emulierter 68060 unter FS-UAE ist kein physischer 68060; native Geekbench-
Scores lassen sich nicht in x86-Gastinstruktionen/s oder PrBoom-FPS umrechnen.

Für einen echten 486/Pentium kannst du die Windows-PE32-Gastdatei auch **nativ
auf der Hardware** starten, vorausgesetzt Windows/API-Importe sind unterstützt.
Für 68060/68080 ist die `QRX_BENCH_HOST`-Variante mit einem geeigneten m68k
Compiler zu bauen (passendes C-Laufzeitsystem erforderlich); gleiche Iterationen
und checksum verifizieren. M1/i7 als native Host-Baseline hat andere ISA-
Befehlszahlen: nur Iterationen/s vergleichen, nicht MIPS direkt.

Beim PrBoom-Lauf muss zusätzlich dieselbe Version, Freedoom-WAD, Demo, Auflösung,
Soundkonfiguration, Renderbackend und Frame-Ermittlung verwendet werden.
