# QRXPPC — PPC32 reference core (P1.1–P1.4)

Stand: 2026-11-09  
Status: Integer-/FPU-Referenzkern und P1.4-Tier-1-68k-JIT mit differentiellen
Gast-Tests; **kein MacOS-Start**.

## Zweck

Der Referenzkern führt PPC32-Integer- und FPU-Code im ARMithlon-68k-Gast aus und dient
als Oracle für den PPC→68k-JIT. Er ist bewusst klein und vollständig
diagnostizierbar: Jede nicht implementierte Instruktion endet mit
`QRX_PPC_ILLEGAL`, jeder Speicher-/Alignmentfehler mit `QRX_PPC_FAULT`.

Nicht enthalten sind FPU-Exception-Traps (MSR-FE0/FE1), `fres`/`frsqrte`,
MMU, PPC-Exceptions, AltiVec, PEF/Code Fragment Manager, MacOS-Toolbox oder
ein MacOS-X-Mach-O-Loader. Ein erfolgreicher
Interpreter-Test ist **keine** Aussage „MacOS läuft“.

## CPU-Zustand

- 32 GPRs, PC, LR, CTR, CR, XER, MSR, SRR0/SRR1.
- 32 FPRs (`uint64_t`, Big-Endian-Rohbits) und FPSCR mit RN, FX/FEX,
  FR/FI, FPRF und den Exception-Bits.
- XER-Bits: SO `0x80000000`, OV `0x40000000`, CA `0x20000000`.
- CR-Felder liegen wie bei `mfcr` in den Nibbles 7…0; CR0 ist das oberste
  Nibble (`cr >> 28`).

## Speichermodell

- Flacher, explizit begrenzter Bytebereich; keine Host-Zeiger.
- Alle Mehrbytezugriffe sind big-endian und alignmentgeprüft.
- `lwz`/`stw`/`lmw`/`stmw` benötigen 4-Byte-, `lhz`/`lha`/`sth` 2-Byte-
  Ausrichtung.
- Ein Zugriff außerhalb des Bereichs setzt `fault_addr` und liefert
  `QRX_PPC_FAULT`.

## Implementierte Integer-Instruktionen (Baseline)

- Arithmetik: `addi`, `addis`, `addic`, `addic.`, `subfic`, `mulli`, `add`,
  `addc`, `adde`, `addme`, `addze`, `subf`, `subfc`, `subfe`, `subfme`,
  `subfze`, `neg`, `mullw`, `mulhw`, `mulhwu`, `divw`, `divwu`.
- Vergleiche: `cmp`, `cmpl`, `cmpi`, `cmpli` (CR0…CR7, L-Form).
- Logik: `and`, `andc`, `or`, `orc`, `xor`, `nand`, `nor`, `eqv`, `andi.`,
  `andis.`, `ori`, `oris`, `xori`, `xoris`.
- Shift/Rotate: `slw`, `srw`, `sraw`, `srawi`, `rlwinm`, `rlwimi`, `rlwnm`,
  `cntlzw`, `extsb`, `extsh`.
- Laden/Speichern: `lwz`, `lwzu`, `lbz`, `lbzu`, `lhz`, `lhzu`, `lha`,
  `lhau`, `stw`, `stwu`, `stb`, `stbu`, `sth`, `sthu`, `lmw`, `stmw`.
- Branch/Sprung: `b`, `bl`, `ba`, `bla`, `bc`, `bcl`, `bca`, `bcla`,
  `bclr`, `bcctr`, `blr`, `bctr`; BO/BI/CTR/CR-Semantik.
- Sprungregister/Status: `mfspr`, `mtspr` (LR, CTR, XER, SRR0, SRR1),
  `mfcr`, `mtcrf`, `mftb` (liefert 0, solange keine Timebase existiert).
- `sc` und `tw`/`twi`: definierter Trap statt stillem Weiterlaufen.
- `sync`, `isync`, `eieio`: No-Op im Referenzkern.

## Implementierte FPU-Instruktionen (P1.3-Baseline)

- Laden/Speichern: `lfs`, `lfsu`, `lfd`, `lfdu`, `stfs`, `stfsu`, `stfd`,
  `stfdu` (4-Byte-Alignment, Big-Endian).
- Arithmetik: `fadd`, `fadds`, `fsub`, `fsubs`, `fmul`, `fmuls`, `fdiv`,
  `fdivs`, `fneg`, `fnabs`, `fabs`, `fmr`, `fsel`.
- Konvertierung: `frsp`, `fctiw` (RN/RZ/RM/RP), `fctiwz` (truncate).
- Vergleich/Status: `fcmpu`, `fcmpo` (CR0…CR7, LT/GT/EQ/UN), `mffs`,
  `mtfsf` mit FM-Feldmaske.

Die FPU-Helfer liegen in `tools/qrxppc/qrx_ppc_fpu.c`. RN/RZ/RM/RP werden
von Arithmetik, `frsp` und `fctiw` ausgewertet. Die Exception-Bits FX, FEX,
VX, OX, UX, ZX, XX, VXSNAN, VXISI, VXIDI, VXZDZ, VXIMZ und VXVC werden
gesetzt; FR/FI und FPRF werden gepflegt. NaN-Operationen liefern den
PPC-Default-QNaN, SNaN setzt VXSNAN, ungültige Operationen setzen die
jeweilige VX-Detailmaske.

Grenzen: FPU-Exception-Traps (MSR-FE0/FE1) sind nicht modelliert; die
Exception-Bits sind sticky, lösen aber keinen PPC-Interrupt aus.
`fadds`/`fsubs`/`fmuls`/`fdivs` rechnen weiterhin intern in Double; die
Single-Rundung ist nur über `frsp` abgedeckt. `fres`/`frsqrte`, `mcrfs`/
`mtfsfi` und AltiVec fehlen. Diese Grenzen sind bewusst dokumentiert, damit
die Gast-Tests nicht mit „G4-FPU vollständig“ verwechselt werden.

## P1.4 Tier-1-68k-JIT

`tools/qrxppc/qrx_ppc_jit.c` und `.h` übersetzen einen geraden PPC-Basic-Block
in m68k-Code. Der erzeugte Funktionskörper hat die C-Signatur
`void f(qrx_ppc_t *)`; `move.l 4(%sp),%a0` lädt den Kontext, D0/D1 sind
Scratch, die GPRs liegen direkt im `qrx_ppc_t`. `lwz`/`stw`, `lbz`/`lhz`/
`lha` und `stb`/`sth` rufen geprüfte C-Helfer auf; damit bleiben Alignment-/
Bereichsfehler und `fault_addr` identisch zum Interpreter. `cmp`/`cmpl`/
`cmpi`/`cmpli` schreiben das gewählte CR-Feld, direkte bedingte Branches
(`bc`/`bcl`) werten BO/BI/CTR aus und beenden den Block.

Übersetzt werden:

- `addi`, `addis`, `ori`, `oris`, `xori`, `xoris`;
- `add`, `subf`, `and`, `or`, `xor` (RC=0 oder RC=1, OE=0);
- `andi.`, `andis.` (implizites CR0-Update);
- `lwz`/`stw`, `lbz`/`lhz`/`lha` und `stb`/`sth` (ohne Update-Form);
- `cmp`/`cmpl`/`cmpi`/`cmpli` (CR0…CR7, L-Form, signed/unsigned);
- `b`, `bl`, `ba`, `bla` sowie `bc`/`bcl`/`bca`/`bcla` als Blockende;
- `sc` als Blockende/Trap.

Shift/Rotate, Multiplikation/Division, Update-Formen der Speicherzugriffe, FPU
und AltiVec bleiben beim Interpreter. Direkte und bedingte Branches (`b`,
`bc`/`bcl` und die `ba`-Varianten) sowie die indirekten Formen `bclr`/`bcctr`
beenden den Block und springen per Tail-Jump direkt in den verketteten
Zielblock. Ein Block endet außerdem an jeder nicht unterstützten Instruktion;
der Interpreter führt sie und den Rest aus. Damit ist der Interpreter
weiterhin der Oracle und es gibt keinen stillen Fallback.

CR0 wird lazy gehalten: Nach einer RC- oder `andi.`-Instruktion berechnet der
JIT den CR0-Nibble und legt ihn in 68k-D2 ab, statt sofort `cpu.cr` zu
schreiben. D2 wird vom generierten Prolog gesichert. Erst an einer
Blockgrenze oder vor einem geprüften Speicherhelfer, der fehlschlagen kann,
wird der letzte CR0-Wert nach `cpu.cr` committet. Mehrere CR0-Schreiber
innerhalb eines Blocks materialisieren daher nur den letzten Wert; bei einem
Fault ist der bis dahin gültige CR0-Stand bereits sichtbar. Compare-
Instruktionen schreiben das ausgewählte CR-Feld direkt; vorher wird ein
möglicherweise pending CR0 committet. Bedingte Branches committen CR0 ebenfalls,
bevor BO/BI/CTR ausgewertet werden, und setzen PC sowie bei LK=1 und
genommenem Branch LR.

Der Code-Cache hält bis zu 24 Blöcke in einer vom Aufrufer gelieferten
64-KiB-Arena. Jeder Block speichert seine exakten Quellbytes und wird bei
jedem Einstieg verglichen. Ändert sich der PPC-Code, wird der Block neu
übersetzt; damit ist SMC zwischen Blockläufen erfasst. `CacheClearE`/
`CacheClearU` werden nach jedem neuen Block aufgerufen. Der Gast-Test
alloziert die Arena mit `AllocVec(..., MEMF_PUBLIC|MEMF_CLEAR)`.

Jeder Block hat neben dem normalen C-Einstieg einen eigenen Chain-Einstieg,
der keinen zweiten D2-Frame aufbaut. Endet ein Block mit einem direkten oder
bedingten Branch, springt der generierte Code per Tail-Jump in den bereits
übersetzten Ziel- oder Fallthrough-Block; unbekannte Ziele laufen über den
Block-Epilog in den Runner zurück. Der Chain-Einstieg prüft vor dem Lauf die
Quellbytes des Zielblocks und das verbleibende Instruktionsbudget, damit SMC
und `max_insns` auch innerhalb einer Kette gelten. Links werden nach der
Übersetzung des Zielblocks per Backpatch auf dessen Chain-Einstieg gesetzt;
ändert sich ein Quellbyte, wird der gesamte Cache verworfen. Blöcke mit Stores
werden nicht als Kettenquelle verkettet, weil ein Store den bereits
übersetzten Nachfolger ungültig machen kann. Die Instruktionszählung erfolgt
im generierten Code (`chain_insns`), sodass auch verkettete Läufe exakt
`cpu.insns` fortschreiben.

Indirekte Branches haben kein statisch bekanntes Ziel. Ihr Block endet
stattdessen mit einem Blick in einen kleinen, zweifach assoziativen
Ziel-Cache (8 Sätze × 2 Wege, Index `(pc >> 2) & 7`): Ein Treffer springt per
`jmp (a0)` in den Chain-Einstieg des Zielblocks, ein Fehltreffer kehrt mit
bereits gesetztem PC in den Runner zurück, der den Zielblock übersetzt und den
Cache füllt. Beide Wege eines Satzes halten verschiedene Ziele, sodass zwei
heiße Rücksprungstellen mit gleichem Satzindex einander nicht mehr verdrängen;
ein voll besetzter Satz ersetzt den zuletzt nicht benutzten Weg. Der
Chain-Einstieg vergleicht zusätzlich `cpu.pc` mit seiner eigenen Blockadresse,
damit ein veralteter Cache-Eintrag niemals fremden Code ausführt; bei SMC
werden Cache und Blöcke gemeinsam verworfen. Wie bei direkten Zielen verketten
Blöcke mit Stores auch indirekt nicht. Der Cache zählt Treffer und Fehltreffer
in `cache_hits`/`cache_misses`.

Grenzen des Tier 1: Lazy Condition Codes sind auf CR0 und die übersetzten
RC-/`andi.`-Formen begrenzt; Compare-Instruktionen schreiben die übrigen
CR-Felder direkt. Direkte und bedingte Branches (`b`, `bc`/`bcl` und die
`ba`-Varianten) verketten bis zu zwei statische Ziele pro Block, indirekte
Branches (`bclr`/`bcctr`) laufen über einen zweifach assoziativen Ziel-Cache (8 Sätze ×
2 Wege). Ein Store, der in die Quellbytes seines eigenen Blocks schreibt, beendet den
Block direkt nach dem Store: `jit_smc_overlap` vergleicht die effektive Adresse
mit dem Quellbereich des laufenden Blocks, und der generierte Code kehrt in
den Runner zurück, statt eine bereits übersetzte Folgeinstruktion auszuführen.
Damit ist Mid-Block-SMC abgedeckt. Offen sind ISA-Tags und
Seitengenerationen, FPU-JIT und AltiVec. Die Roadmap führt diese Punkte als offene
P1.4-Arbeit.

## Status und Abbruch

| Status | Bedeutung |
|---|---|
| `QRX_PPC_OK` | Instruktion ausgeführt |
| `QRX_PPC_TRAP` | `sc` oder erfüllte Trap-Bedingung |
| `QRX_PPC_HALT` | PC wurde 0 |
| `QRX_PPC_FAULT` | Speicher-/Alignmentfehler oder Division durch 0 |
| `QRX_PPC_ILLEGAL` | nicht implementierter Opcode |

`qrx_ppc_run()` zählt Instruktionen und bricht bei jedem Nicht-OK-Status ab.
Der Testkern verwendet `sc` als definierten Endpunkt.

## Testvektoren

`tools/qrxppc/qrx_ppc_test.c` legt handcodierte PPC-Instruktionen bei `0x1000`
ab, Daten bei `0x2000`, und prüft Register sowie Big-Endian-Speicher. Abgedeckt
sind Arithmetik, CTR-Schleife, CR-Vergleich/Branch, Load/Store, Multiplikation/
Division, Rotate/Mask, logische CR0-Aktualisierung und `bl`/`blr`. Die
FPU-Fälle prüfen Double- und Single-Load/Store, `fadd`/`fsub`,
`fmul`/`fdiv` sowie `fcmpu`/`fcmpo` mit LT/GT/EQ. Die Randfälle prüfen
alle vier Rundungsmodi (`fadd`, `frsp`, `fctiw`), FPSCR-Transfer
(`mtfsf`/`mffs`), NaN/SNaN, Infinity, Division durch 0, Overflow/Underflow
und die Exception-Bits. Insgesamt 19 Basis-Fälle.

Im ARMithlon-Gast führt `run()` jeden Fall zusätzlich per JIT auf einer
separaten Speicherkopie aus und vergleicht Status, GPRs/FPRs, PC, LR/CTR/CR/
XER/MSR/SRR0/SRR1, FPSCR, `fault_addr`, Instruktionszahl und den gesamten
256-KiB-Speicher. Die P1.4-Fälle ergänzen zwei Fault-Differentialtests
(`jit_fault_oob`, `jit_fault_unaligned`), den Breiten-/Grenztest (`jit_widths`),
den Grenztest mit kleinem logischem Limit (`jit_widths_limit`), den
SMC-/Cache-Test (`jit_cache_smc`), den CR0-Test (`jit_cr0`), der EQ/LT/GT,
das XER-SO-Bit und den CR0-Commit vor einem Fault prüft, den Compare-/CR-Test
(`compare_cr`), den Branch-Test (`jit_branch`), der genommenen und nicht
genommenen `beq`-Pfade, CR1, `bcl`-Link und eine `bdnz`-Schleife prüft, den
Chaining-Test (`jit_chain`), der nach dem Übersetzen aller Blöcke einen
einzigen nativen Einstieg für einen direkten und einen bedingten Branch
nachweist, die drei indirekten Ketten-Tests (`jit_chain_blr` mit
`bl`/`blr`-Rücksprung über zwei Runden, `jit_chain_bcctr` mit `mtctr`/`bcctr`
und `jit_chain_beqlr` für den nicht genommenen `bclr`-Fall), sowie den
deterministischen Fuzz-Test (`jit_fuzz`). `jit_widths` prüft `lbz`/`lhz`/
`lha`/`stb`/`sth`, die letzten gültigen Byte-/Halfword-/Wortzugriffe und
Bereichsfehler bei `0x40000` ohne vorgelagerten Alignment-Fehler.
`jit_widths_limit` wiederholt die letzten gültigen Zugriffe sowie OOB- und
Unaligned-Fehler an einer logischen Grenze von `0x3000`; dadurch bleiben
Mutationen der Prüfungen innerhalb des 256-KiB-Backing-Arrays. Der
Gast-Mutationstest `tools/qrxppc/mutation_test.sh` entfernt nacheinander den
Range-Check bei `lbz`/`lhz`/`stb`/`sth` und den Alignment-Check bei
`lhz`/`sth`; alle sechs Mutanten müssen `jit_widths_limit` auf FAIL bringen.
`jit_fuzz`
erzeugt mit Seed `0x9E3779B9` 64 Fälle mit je 16 zufälligen,
JIT-unterstützten Instruktionen und vergleicht den kompletten Interpreter-
und JIT-Zustand. Der Fault-Fuzz (`jit_fault_fuzz`)
erzeugt mit Seed `0xC0DEF00D` 96 Fälle: 32 beliebige ungültige
32-Bit-Adressen, 32 zusätzlich erzwungen unaligned, 16 Wortzugriffe an der
256-KiB-RAM-Grenze (`0x3FFFD`–`0x40004`) und 16 am 32-Bit-Adresswrap
(`0xFFFFFFF8`–`0xFFFFFFFF`). Die 64 Zufallsfälle sind je zur Hälfte Load und
Store, die 32 Grenzfälle ebenfalls. Jeder Fall schreibt unmittelbar vor dem
faultenden Zugriff CR0 über eine zufällige der sechs CR0-schreibenden Formen;
damit wird der Lazy-CR0-Commit vor dem Speicherhelfer geprüft. Im Host laufen 23 Fälle plus
Byte-Selbsttest; der JIT wird dort nur als Byte-Emitter geprüft, nicht
ausgeführt. Im ARMithlon-Gast laufen 37 Fälle, weil der SMC-Test, der
Breiten-/Grenztest, der kleine-Limit-Test, der Branch- und die
Chaining-Tests sowie die Fuzz-Differentialtests dort den ausführbaren
68k-Code benötigen.

Bauen und Gast-Test:

```sh
tools/qrxppc/build.sh
C:QRXPPC
```

Der Test gibt `QRXPPC: 0 failure(s)` aus; im Gastlauf wurden 0 Fehler
protokolliert. Er läuft als normales 68k-AROS-Programm im Gast; es wird keine
Gastsoftware auf dem Host ausgeführt.

## Nächste Schritte

1. P1.3-Rest: FPU-Exception-Traps, `mcrfs`/`mtfsfi`, Single-Arithmetik und
   `fres`/`frsqrte`.
2. P1.4-Rest: ISA-Tag/Seitengeneration, Update-Formen der Speicherzugriffe
   und FPU-JIT.
3. P1.5: 68k↔PPC-Mixed-Mode-ABI.
4. Erst danach P2: PEF/Code Fragment Manager und MacOS-9-Toolbox-HLE.
