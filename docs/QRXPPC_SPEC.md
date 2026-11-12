# QRXPPC — PPC32 reference core (P1.1–P1.4)

Stand: 2026-11-09  
Status: Integer-/FPU-Referenzkern, P1.4-Tier-1-68k-JIT mit differentiellen
Gast-Tests und PEF-Container-Reader (P2.1, ohne Relokationen und Toolbox);
**kein MacOS-Start**.

## Zweck

Der Referenzkern führt PPC32-Integer- und FPU-Code im ARMithlon-68k-Gast aus und dient
als Oracle für den PPC→68k-JIT. Er ist bewusst klein und vollständig
diagnostizierbar: Jede nicht implementierte Instruktion endet mit
`QRX_PPC_ILLEGAL`, jeder Speicher-/Alignmentfehler mit `QRX_PPC_FAULT`.

Nicht enthalten sind FPU-Exception-Traps (MSR-FE0/FE1), `fres`/`frsqrte`,
MMU, PPC-Exceptions, AltiVec, MacOS-Toolbox, PEF-Relokationen/gepackte
Sections oder ein MacOS-X-Mach-O-Loader. Ein erfolgreicher
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

## PEF-Container (P2.1, Lesepfad)

`tools/qrxppc/qrx_pef.c` liest PEF-Container (`Joy!peff`) für `pwpc`:
Container-Header (40 B), Section-Header (28 B), den Loader-Header (56 B),
Import-Bibliotheken (24 B), Import-Symbole (4 B) und die Export-Tabelle
(Hash-Tabelle, Schlüssel-Tabelle, 10-Byte-Symboleinträge). Geprüft werden
Magic, Formatversion, Architektur, Section-Zahl, Container-Grenzen, genau eine
Loader-Section, die Gültigkeit von `main`/`init`/`term`, die Bereichsgrenzen der
Symboltabellen und NUL-terminierte Namen. Jeder Fehler hat einen eigenen Status
(`QRX_PEF_ERR_*`) statt eines Absturzes.

`qrx_pef_load()` kopiert ausschließlich ungepackte Sections nach
`base + index * 0x1000` und prüft die Gastgrenzen vor dem ersten Schreibzugriff;
`qrx_pef_main_tvector()` liefert die Adresse des Main-TVec, dessen erstes Wort
der Einstiegspunkt ist.

`qrx_pef_relocate()` wendet die Relokationsanweisungen je Section an
(Reloc-Header mit 12-Byte-Einträgen): `BySectDWithSkip`, die Wertegruppe
(`BySectC`/`BySectD`, `TVector12`, `TVector8`, `VTable8`, `ImportRun`), die
Indexgruppe (`SmByImport`, `SmSetSectC`/`SmSetSectD`, `SmBySection`),
`IncrPosition`, `SmRepeat`, `SetPosition`, `LgByImport`, `LgRepeat` und
`LgSetOrBySection`. `sectionC`/`sectionD` sind wie im PEF-Standard die
Section-Adresse abzüglich `defaultAddress`. Alle Importe werden über einen
`qrx_pef_resolver_t` **vor** dem ersten Schreibzugriff aufgelöst; ein
unaufgelöster Import liefert `QRX_PEF_ERR_UNRESOLVED` und lässt den Speicher
unverändert. Unbekannte Opcodes, ungültige Section-Indizes und Reloc-Streams
außerhalb der Loader-Section enden als `QRX_PEF_ERR_RELOC`. Der Gast-Test `pef_loader` baut einen synthetischen
Container mit TVec und Fragment-Code, lädt ihn, führt ihn interpretiert und per
JIT aus und vergleicht den vollständigen Endzustand. `pef_reloc` prüft die
Relokation an einem Drei-Section-Container (Code/Data/Loader): TVector12,
`SmBySection`/`BySectD`- und `SmByImport`-Fixups, dazu die Negativfälle
unaufgelöster Import, unbekannter Opcode, ungültiger Section-Index und
Reloc-Stream außerhalb der Loader-Section.

Nicht enthalten: gepackte Sections (`QRX_PEF_ERR_PACKED`), MacBinary/
Resource-Forks, 68k-Container, Glue-Code-Erzeugung für Mixed Mode und jeder
Toolbox-Aufruf. Der Resolver erhält Bibliothek, Symbolklasse und Namen und
liefert die Adresse selbst; eine Export-Hash-Suche oder Lazy-Bindung gibt es
nicht, alle Importe werden beim Laden aufgelöst. Ein geladener PEF-Container
ist daher noch **keine** laufende MacOS-9-App.

## Mixed Mode (P1.5)

`tools/qrxppc/qrx_mix.c` verbindet 68k-Aufrufer mit PPC-Code und zurück. Die
Brücke deckt den PowerOpen-/MacOS-9-Ausschnitt ab, den ein Glue-Stub braucht:
`r1` ist 16-Byte-ausgerichtet, an `0(r1)` liegt die Linkage-Area (Rücksprung-SP,
gespeichertes CR, gespeichertes LR, Reserve), ab `24(r1)` der Parameterbereich;
`r2` ist RTOC, `r3..r10` und `f1..f8` tragen Integer- bzw. FP-Argumente,
Ergebnisse kommen in `r3`/`r4` und `f1` zurück. `qrx_mix_call()` baut den
Glue-Rahmen, setzt LR auf ein Sentinel-Wort (`sc`) und führt aus, bis der
PPC-Code dorthin zurückspringt; ein nicht wiederhergestellter Stackpointer
endet als `QRX_MIX_ERR_SP`.

Argumente beschreibt eine Signatur (`qrx_mix_sig_parse`): pro Parameter ein
Zeichen in Deklarationsreihenfolge, `i`/`p` für 32-Bit-Integer bzw. Pointer,
`l` für 64-Bit-Integer und `d` für Double; der Rückgabetyp ist `v`, `i`, `l`
oder `f`. Integer füllen `r3..r10`, Doubles `f1..f8` mit getrennten Zählern;
was nicht mehr in seine Registerklasse passt, landet im Parameterbereich ab
`24(r1)`, in Argumentreihenfolge, 4-Byte-ausgerichtet (`l`/`d` 8-Byte-
ausgerichtet, hohes Wort zuerst). Ein `l` mit ungeradem Registerzähler wird
verworfen, weil PowerOpen kein Paar teilt. `qrx_mix_call()` prüft die
Argumentzahlen gegen die Signatur und maskiert das Ergebnis auf den
Rückgabetyp (`v` löscht es, `i` liefert nur `r3`, `l` `r3:r4`, `f` nur `f1`).
Eine fehlende Signatur ist `QRX_MIX_ERR_ARG`: den Rohregisterpfad gibt es,
anders als in der ersten Fassung, nur noch auf ausdrückliche Anforderung mit
`QRX_MIX_RAW_SIG` als Signaturwert, und der abgelehnte Aufruf schreibt weder
Rahmen noch Register des Gasts. Damit ist er dieselbe Altlast wie der rohe
Deskriptor in `qrx_mix_register()`.

PPC-Code ruft 68k-Code über `sc` mit dem Import-Index in `r0` auf. Die Brücke
übergibt die von der Import-Signatur benannten Argumente als
`qrx_mix_args_t`: `gpr[]`/`fpr[]` je Klasse in Deklarationsreihenfolge,
`stack[]` als rohe Wörter des Parameterbereichs und `sp` als Gastadresse von
`24(r1)`. Sie ruft den registrierten Callback (im Gast echter 68k-Code) und
setzt hinter dem `sc` das Ergebnis gemäß Rückgabetyp in `r3`, `r3:r4` oder
`f1`; ein Import ohne Signatur erhält weiterhin `r3..r10`/`f1..f8` roh. Verschachtelte Aufrufe (68k-Callback ruft
erneut PPC) bekommen einen eigenen Glue-Rahmen unterhalb des aktuellen PPC-SP;
der komplette PPC-Zustand wird gesichert und wiederhergestellt. Jeder
Fehlerfall hat einen eigenen Status: nicht registrierter Import,
Halt/Fault/Illegal, erschöpftes Instruktionsbudget und Trap-Schleife.

Ein Aufruf kann statt über eine nackte Einstiegsadresse über einen
Mixed-Mode-Routine-Deskriptor im GastSpeicher laufen. `qrx_mix_rd_resolve()`
prüft den 2-Byte-gepackten `RoutineDescriptor` (Trap-Wort `0xAAFE`, Version 7,
`routineDescriptorFlags` 0, Record-Tabelle vollständig im Speicher, höchstens
acht Records) und wählt den ersten Record mit passender ISA (`kM68kISA` 0,
`kPowerPCISA` 1). `procDescriptor` wird absolut oder – mit
`kProcDescriptorIsRelative` – relativ zur Deskriptoradresse aufgelöst und muss
danach ausgerichtet und im Speicher liegen; Reservefelder und `selectorInfo`
werden ignoriert. `qrx_mix_rd_call()` gibt die aufgelöste Adresse an
`qrx_mix_call()`, sodass Signatur, Stack-Argumente und Rückgabemaskierung
unverändert gelten. Abgelehnt werden Dispatch-Deskriptoren (Flags ungleich 0),
`kFragmentNeedsPreparing`, Selektor-Flags, unbekannte Flag-Bits und ein
fehlender PPC-Record. Das `ProcInfo` eines register-basierten Records wird auf
Wunsch dekodiert (`qrx_mix_rd_procinfo_sig`): Konvention in Bits 0…3
(`kRegisterBased` = 2), Ergebnisgröße in Bits 4…5, danach 5-Bit-Felder pro
Parameter in Reihenfolge (Größe in Bits 0…1, Registerhinweis in Bits 2…4).
Registerhinweise und Ergebnisregister bleiben ungenutzt, weil PowerOpen
`r3`…`r10` und `f1`…`f8` nach Position vergibt; platziert wird nach den
Größen: 1, 2 und 4 Byte als `i`, eine Ergebnisgröße von 0 als `v`.
`qrx_mix_rd_invoke_procinfo()` löst Deskriptor und `ProcInfo` auf und ruft
ohne eigene Signatur. Abgelehnt werden andere Konventionen (Stack-basiert,
Dispatch, `kSpecialCase`), ein Feld mit Registerhinweis aber Größe 0 und ein
fünfter Parameter, dessen Bits jenseits von Bit 31 liegen. Doppelte Werte,
64-Bit-Integer und mehr als vier Parameter beschreibt ein `ProcInfo` nicht;
dafür bleibt eine explizite Signatur nötig. Derselbe Weg steht
`qrx_mix_rd_invoke()` offen: ohne Signatur leitet es die Platzierung aus dem
`ProcInfo` des PPC-Records ab, und `qrx_mix_rd_invoke_procinfo()` ist genau
dieser Aufruf; `qrx_mix_rd_call()` verhält sich bei `NULL` genauso, prüft aber
zuerst die ISA.

Dieselben Deskriptoren adressieren auch Imports: PPC-Code legt statt eines
Import-Index die Adresse eines registrierten Deskriptors in `r0` (`r0 < 16`
bleibt Index), die Brücke löst daraus den 68k-Record auf und übergibt dessen
Einstiegspunkt als `args->proc`. `qrx_mix_register_proc()` prüft den Deskriptor
schon bei der Registrierung, erlaubt höchstens acht Deskriptor-Importe und
weist Duplikate ab; jeder Aufruf löst den Deskriptor erneut auf, damit
Änderungen im Gastspeicher nicht unbemerkt bleiben. Ein Deskriptor ohne
68k-Record oder ein nach der Registrierung ungültig gewordener Deskriptor
endet als `QRX_MIX_ERR_RD`, ein nicht registrierter Deskriptor als
`QRX_MIX_ERR_IMPORT`. Auch hier ist die Platzierung explizit: `QRX_MIX_PROCINFO`
an Stelle der Signatur verlangt die Ableitung aus dem `ProcInfo` — vorzugsweise
aus dem PPC-Record, bei einem reinen 68k-Deskriptor aus dem Record, den der
Import aufruft — und liest sie bei jedem Aufruf neu; eine Signatur entscheidet
allein. Ein `ProcInfo`, das die Platzierung nicht beschreibt, wird schon bei der
Registrierung mit `QRX_MIX_ERR_RD` abgelehnt. Eine fehlende Signatur ist
`QRX_MIX_ERR_ARG` und wird vor dem Deskriptorzugriff abgewiesen, sodass eine
abgelehnte Registrierung keinen Slot belegt.

Der Test `mixed_call` prüft Argumente, Ergebnis, Glue-Wort, Back-Chain und das
gespeicherte LR im Callee-Rahmen, `mixed_stack` die ersten acht Register- plus
zwei Stack-Argumente, `mixed_fpu` die FP-Argumente und die `f1`-Rückgabe,
`mixed_import` den 68k-Callback samt Speicherzugriff, `mixed_nested` den Pfad
68k→PPC→68k→PPC und `mixed_errors` die Negativfälle. `mixed_sig_call` prüft
zehn Integer-Parameter mit zwei Stack-Wörtern, `mixed_sig_fp` neun Doubles mit
einem 8-Byte-ausgerichteten Stack-Double (`lfd` aus dem Parameterbereich),
`mixed_sig_import` die gemischte Liste `iipd` über Register und Pointer,
`mixed_sig_stack` zehn Integer bei einem Import (Stack-Wörter und `sp` im
Callback) und `mixed_sig_errors` kaputte Signaturen, falsche Argumentzahlen
und einen nicht 16-Byte-ausgerichteten Stackpointer. `mixed_rd` prüft einen
Einzel-Record-Deskriptor (absolute Adresse, ignorierte Reservefelder, Aufruf
mit Stack-Wort), einen Fat-Deskriptor mit 68k- vor PPC-Record, die ISA-Auswahl
und einen relativen `procDescriptor`; `mixed_rd_errors` falsches Trap-Wort,
falsche Version, Dispatch-Flags, zu viele Records, eine abgeschnittene
Tabelle, einen Deskriptor außerhalb des Speichers, den fehlenden PPC-Record,
`kFragmentNeedsPreparing`, Selektor-Flags, unbekannte Flags, unbrauchbare
Einstiegspunkte, relativen Unterlauf und dass nicht gewählte Records nicht
geprüft werden sowie die Auflösung selbst keinen Gastcode ausführt.
`mixed_rd_nested` lässt einen 68k-Callback über `qrx_mix_rd_invoke()` wieder in
PPC-Code springen: Fat-Deskriptor mit 68k-Record zuerst, zwei und neun
Parameter (das neunte Wort liegt im verschachtelten Parameterbereich),
Back-Chain auf den Rahmen des aufrufenden PPC-Codes und unveränderter
CPU-Zustand nach der Rückkehr; `mixed_rd_nested_errors` prüft dabei einen
68k-only-Deskriptor, eine Nicht-Deskriptor-Adresse, ein zu kleines
Instruktionsbudget und einen Callee, der `r1` nicht zurückstellt, jeweils mit
unverändertem CPU-Zustand danach. `mixed_rd_procinfo` ruft über
`qrx_mix_rd_invoke_procinfo()` drei 4-Byte-Parameter auf (Ergebnis 15), dann
einen parameterlosen Deskriptor mit `v`-Ergebnis und einen mit 1- und
2-Byte-Parametern; geprüft werden die abgeleitete Signatur und das Ergebnis,
vier Parameter sind der letzte darstellbare Fall. `mixed_rd_procinfo_errors`
prüft die Ablehnung stack-basierter, Dispatch- und `kSpecialCase`-Konventionen,
eines Feldes mit Registerhinweis ohne Größe, eines fünften Parameters und
leerer Zeiger; die Signatur bleibt bei Fehlern unangetastet, und ein
abgelehnter `ProcInfo` führt keinen Gastcode aus.
`mixed_proc_import` ruft einen 68k-Callback über einen Fat-Deskriptor auf
(acht Registerargumente, ein Stack-Wort, `args->proc` = 68k-Einstieg), und
`mixed_proc_errors` prüft zu niedrige Adressen, Nicht-Deskriptoren, fehlenden
68k-Record, kaputte Signatur, Duplikat, unregistrierten Deskriptor und einen
nach der Registrierung veränderten Deskriptor. `mixed_proc_procinfo`
registriert Deskriptor-Importe ohne eigene Signatur: bei einem Fat-Deskriptor
mit undurchsichtigem `ProcInfo` im 68k-Record gewinnt der PPC-Record (drei
4-Byte-Parameter), ein 68k-only-Deskriptor fällt auf seinen eigenen Record
zurück, eine ausdrückliche Signatur hat Vorrang, und ein im Gastspeicher
geändertes `ProcInfo` ändert schon den nächsten Aufruf (zwei statt drei
Argumente). `mixed_proc_procinfo_errors` prüft die Ablehnung, wenn das
`ProcInfo` des PPC-Records stack-basiert ist oder ein 68k-only-Deskriptor gar
keines beschreibt, dass ein abgelehnter Deskriptor keinen Import-Slot belegt,
dass ein nachträglich kaputtes `ProcInfo` den Aufruf mit `QRX_MIX_ERR_RD`
beendet, ohne den Callback zu rufen, und dass die Reparatur den nächsten Aufruf
wieder bedient. `mixed_rd_invoke_procinfo` prüft denselben Weg über
`qrx_mix_rd_invoke()` ohne Signatur (drei Parameter mit Ergebnis 66, gleiches
Ergebnis über `qrx_mix_rd_invoke_procinfo()`, Vorrang einer ausdrücklichen
Signatur, `v`-Ergebnis behält `r3`), `mixed_rd_invoke_procinfo_errors` die
Ablehnung eines stack-basierten `ProcInfo`, eines 68k-only-Deskriptors und eines
fünften Parameters, jeweils mit unverändertem CPU-Zustand vor dem Gastcode, und
die erneute Bedienung nach der Reparatur. `mixed_rd_call_procinfo` prüft die
Ableitung über `qrx_mix_rd_call()` (Ergebnis 66, Vorrang einer ausdrücklichen
Signatur), `mixed_rd_call_procinfo_errors` die ISA-Sperre vor jedem
Deskriptorzugriff, ein stack-basiertes `ProcInfo`, einen 68k-only-Deskriptor und
die erneute Bedienung nach der Reparatur. `mixed_glue_procinfo` bildet den
Glue-Fall durchgängig ab: PPC ruft einen Import, der 68k-Callback hält nur eine
UniversalProcPtr (die Deskriptoradresse) und springt ohne eigene Signatur über
das `ProcInfo` des PPC-Records in PPC-Code, der seine beiden Registerargumente
im Gastspeicher ablegt; geprüft werden Platzierung, Rückgabewert, Back-Chain des
verschachtelten Rahmens, unveränderter CPU-Zustand und die Ablehnung eines
zweiten Deskriptors, dessen `ProcInfo` keine Platzierung beschreibt. Ein Index-Import trägt selbst kein `ProcInfo`;
`qrx_mix_register()` verlangt deshalb eine ausdrückliche Platzierung: entweder
eine Signatur, die dann allein entscheidet (ein Deskriptor daneben ist
`QRX_MIX_ERR_ARG`), oder einen Deskriptor, aus dem die Platzierung wie beim
Deskriptor-Import abgeleitet und bei jedem Aufruf neu gelesen wird
(`mixed_import_procinfo`). Der frühere Rohregisterpfad ist eine Altlast und nur
noch über `QRX_MIX_RAW` als Deskriptorwert erreichbar; ohne Signatur und ohne
Deskriptor ist die Registrierung `QRX_MIX_ERR_ARG` und lässt die vorherige
Bindung unverändert.

Welche Variante welche Platzierungsquelle wählt und was eine Mischform kostet:

| Variante | Platzierungsquelle | Mischform / Fehlgebrauch |
| --- | --- | --- |
| `qrx_mix_register(mix, i, 0, fn, ctx, &sig)` | Signatur | Deskriptor ungleich 0 daneben: `QRX_MIX_ERR_ARG` |
| `qrx_mix_register(mix, i, rd, fn, ctx, NULL)` | `ProcInfo` des Deskriptors (PPC-Record, sonst 68k-Record) | `rd` unter `QRX_MIX_IMPORTS`: `ERR_ARG`; unlesbarer Deskriptor oder `ProcInfo`: `ERR_RD` |
| `qrx_mix_register(mix, i, QRX_MIX_RAW, fn, ctx, NULL)` | Rohregister | `QRX_MIX_RAW` zusammen mit Signatur: `ERR_ARG` |
| `qrx_mix_register(mix, i, 0, fn, ctx, NULL)` | keine | `ERR_ARG`, vorherige Bindung samt Import-Slot bleibt |
| `qrx_mix_register_proc(mix, rd, fn, ctx, &sig)` | Signatur | `rd` unter `QRX_MIX_IMPORTS`, doppelt registriert oder kein Slot frei: `ERR_ARG` |
| `qrx_mix_register_proc(mix, rd, fn, ctx, QRX_MIX_PROCINFO)` | `ProcInfo` des Records | `ERR_RD`, wenn das `ProcInfo` keine Platzierung beschreibt |
| `qrx_mix_register_proc(mix, rd, fn, ctx, NULL)` | keine | `ERR_ARG` vor dem Deskriptorzugriff, kein Slot belegt |
| `qrx_mix_call(mix, entry, &sig, args, …)` | Signatur | `args` passt nicht zur Signatur oder `entry` ist 0: `ERR_ARG` |
| `qrx_mix_call(mix, entry, QRX_MIX_RAW_SIG, args, …)` | Rohregister, Zähler wörtlich | Zähler über `QRX_MIX_ARGS`/`QRX_MIX_STACK_WORDS`: `ERR_ARG` |
| `qrx_mix_call(mix, entry, NULL, args, …)` | keine | `ERR_ARG`, weder Rahmen noch Register geschrieben |

Eine ungültige Signatur selbst endet als `QRX_MIX_ERR_SIG`. `qrx_mix_rd_call()`
und `qrx_mix_rd_invoke()` leiten bei NULL weiterhin eine Signatur aus dem
`ProcInfo` des Records ab; das ist der verbliebene implizite Pfad. Beim Bau
dieser Tests
fiel auf, dass `addi`/`addis`/`addic` mit `RA=0` fälschlich `r0` statt des
Literals 0 addierten; der Kern folgt jetzt der ISA.

Nicht enthalten: Glue-Code-Erzeugung, Routine-Deskriptoren für Dispatch mit
Selektoren, CFM-Fragmentvorbereitung, 68k-Einstiegspunkte (der Kern führt nur
PPC aus), Aggregate/Unions und Varargs in
Signaturen, eine Unterscheidung von `float`
und `double` (jedes `float` wird als Double übergeben), eine über die
`r1`-Prüfung hinausgehende Stack-Abwicklung, gemischte Codefragmente und das
Umschalten zwischen 68k- und PPC-Code in einem laufenden Prozess.

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
damit wird der Lazy-CR0-Commit vor dem Speicherhelfer geprüft. Im Host laufen 52 Fälle plus
Byte-Selbsttest; der JIT wird dort nur als Byte-Emitter geprüft, nicht
ausgeführt. Im ARMithlon-Gast laufen 65 Fälle plus JIT-Emit-Selbsttest, weil der SMC-Test, der PEF-Lade- und der
Relokationstest, die siebenundzwanzig Mixed-Mode-Fälle, der
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

Der Testbuild stößt an eine Grenze von `elf2hunk`: Sobald der Text der
Binärdatei 64 KiB überschreitet, lässt sich eine PC-relative Relokation
(`R_68K_PC32`) nicht mehr als `RELRELOC32` ausdrücken, und das Werkzeug meldet
`relocation offset N is too big for RELRELOC32` gefolgt von
`1 relocation(s) failed in HUNK #0`. Solche Relokationen entstehen aus
Tail-Calls (`return f(...)`), die GCC als `bra.l` übersetzt. Deshalb bauen
`tools/qrxppc/build.sh` und der `build_variant` in
`tools/qrxppc/mutation_test.sh` mit `-fno-optimize-sibling-calls`: Dann bleiben
nur absolute `jsr`/`R_68K_32`-Bezüge übrig, und `elf2hunk` erzeugt wieder einen
`loadseg()`-fähigen Hunk. Die Flagge ist kein Geschwindigkeitsschalter; sie
darf nicht entfernt werden, solange der Text über 64 KiB liegt.

## Nächste Schritte

1. P1.3-Rest: FPU-Exception-Traps, `mcrfs`/`mtfsfi`, Single-Arithmetik und
   `fres`/`frsqrte`.
2. P1.4-Rest: ISA-Tag/Seitengeneration, Update-Formen der Speicherzugriffe
   und FPU-JIT.
3. P1.5-Rest: Glue-Code-Erzeugung, Dispatch-Deskriptoren mit Selektoren und
   gemischte Codefragmente.
4. Erst danach P2: PEF/Code Fragment Manager und MacOS-9-Toolbox-HLE.
