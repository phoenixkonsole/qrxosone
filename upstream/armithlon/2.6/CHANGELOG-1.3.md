# ARMITHLON 1.3 – QRX86 CPU/GPU-Prüfung und FS-UAE-Bootvorbereitung

## Basis

GitHub `phoenixkonsole/qrxosone`, `claude/qrx-22.5.11`, Commit
`cc2ba06a0cc76a87bdd49edcd4b475df4227e716` (31.10.2026 21:37 UTC).
Das aktuelle QRX86 22.5.12 ist ein x86-Interpreter mit Windows-HLE und
Linux-i386-Personality/X11. Bestehende QRX86-JIT-Arbeit bleibt unverändert.
Das 1.3-Paket enthält weiterhin den vollständigen ARMITHLON-1.2-Source; QRX
private Repo-Inhalte werden authentifiziert und gepinnt erst auf dem Host geladen.

## Neuer Code

- `tools/patch-qrx86-x11-input.py`: Mac Q86I-Tastatur/-Maus → eingebettetes
  Linux-X11-Protokoll (begrenzter Single-Client-Adapter, wörtliche Eventmasken,
  skalierte Mauskoordinaten). Nur im isolierten Host-Staging-Build, kein Patch
  des GitHub-Repositories. X11/GTK/Qt bleibt unvollständig.
- `tools/build-crossbench.sh`, `benchmarks/qrx86-cross/*`: eine echte gemeinsame
  C-Workload als native Host-Version, freestanding Linux-ELF32 (vorcompiliert)
  und MinGW-PE32 (wenn installiert). Integer-ALU/Rotation und 32-KiB-Speicher,
  exakt 300.000 Iterationen, verifizierter Checksum `903be502`.
- `tools/bench-qrx86-execution.py`: je bis 10 Wiederholungen, tatsächliche
  QRX86-Interpreter-Laufzeit, Gastinstruktionsdurchsatz, Median/Logs/Hashes,
  SKIP statt erfundener Werte, HTML-Dashboard mit optionalen GPU-Ergebnissen.
- `tools/add-hardware-reference.py`: validierte Referenzaufnahme für 486DX2,
  Pentium, 68060, 68080, Pentium 4, Core i7 und Apple M1. Gleichwertige C-
  Workload und SHA-/Checksum-Nachweise sind zwingend. Keine automatische
  Umrechnung von Frequenz oder Geekbench zu FPS/MIPS.
- `tools/bench-qrx86-prboom.py`: tatsächliche Gast-PrBoom-Timedemo-Versuche,
  Windows DirectDraw und Linux SDL/X11, nachvollziehbare Fehlerdiagnose und
  Log-Erfassung. Unvollständige Demo bzw. fehlende i386-Libraries sind kein PASS.
- `tools/prepare-qrx-compat-boot.py` + `tools/run-qrx-compat-boot.py`: prüfen
  HUNK-22.5.12-SHA256, extrahieren eine echte AROS-LiveCD mit xorriso in eine
  neue Systemkopie, installieren HUNK-C:-Tools und führen einen echten
  FS-UAE-68060-AROS-Bootsmoke mit BOOTED/DONE-Dateien durch. NICHT: QRX68100
  native AROS/QRX-Bootfähigkeit, NICHT: vollständiges QRX-OS-One-ISO.

## Prüfung hier

- Linux x86_64: portable ARMITHLON-CMake-Engine erfolgreich gebaut.
- 21/21 CTests bestanden, neue Patch-/Benchmark-/Bootstrap-Gates eingeschlossen.
- Native 64-Bit-Host-Version des CrossBench kompiliert und gelaufen: richtiger
  Checksum 903be502; Linux-ELF32 statisch kompiliert und per `file` erkannt.
- Die Build-Umgebung kann ELF32 wegen fehlendem i386-Kernel-ABI nicht nativ
  ausführen; kein QRX86 Win32/Linux-Host aus privatem Repo in der Umgebung.
- Windows-PE32 (kein MinGW), echte PrBoom-Demo, FS-UAE-AROS-Boot, macOS M1,
  Metal, Vulkan und physische historische CPUs: NICHT ausgeführt.
- Vergleichbare hardwarebezogene Leistungsbalken werden nur nach realer
  validierter Messung angezeigt; M1- und historischer Hardware-FPS nicht erfunden.

## Bereits belegte historische PrBoom-Messungen (GitHub)

`benchmarks/prboom-2026-10-30/README.md` und JSON im QRX-Repo:
FS-UAE mit emuliertem 68060: 43 Frames / 370,4497 s = 0,1161 FPS;
Linux-Xeon-8370C-Host QRX86 Win32: 325 Frames pro Lauf / Median 29,94
FPS (3 Wiederholungen, starke Streuung); Full Timedemo 7343 Frames /
148,4575 s = 49,46 FPS, **andere Messstrecke**. Keine 486/Pentium/68080-
Hardware-Extrapolation zulässig. Für Linux-PrBoom existiert noch kein
vergleichbar abgeschlossener Lauf.
