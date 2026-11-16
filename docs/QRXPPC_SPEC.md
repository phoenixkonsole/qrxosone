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
MMU, PPC-Exceptions, AltiVec, MacOS-Toolbox ausser Memory, Resource, File,
QuickDraw, Window, Menu, Dialog, Text/Font und Event Manager (P2.2, siehe
unten), PEF-
Relokationen/gepackte
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

## Mach-O-Prototyp (Mac OS X, PPC32)

`tools/qrxppc/qrx_macho.c` ist ein von PEF getrennter Loader-Prototyp. Er
wählt aus einem big-endian 32-Bit-Fat-Container den ersten PPC-Slice oder liest
ein dünnes PPC32-`MH_EXECUTE`. Er prüft die Load-Command-Grenzen, die
`LC_SEGMENT`-Datei- und VM-Bereiche sowie den `PPC_THREAD_STATE` aus
`LC_UNIXTHREAD`; dessen PC muss in ausführbaren, dateigestützten Segmentdaten
liegen. `qrx_macho_map()` kopiert Segmente an ihre festen VM-Adressen, füllt
BSS mit Null und überspringt unzugängliche Reservierungen wie `__PAGEZERO`.
Vor dem ersten Schreibzugriff werden Gastgrenzen und Segmentüberlappungen
geprüft. Die Quelldatei muss bis zum Mapping unverändert verfügbar bleiben und
darf den Gastspeicher nicht überlappen.

Der Inspector und der begrenzte Runner lassen sich auf dem Host bauen:

```sh
cc -std=c99 -O2 tools/qrxppc/qrx_macho.c tools/qrxppc/qrx-mach.c \
  tools/qrxppc/qrx_ppc.c tools/qrxppc/qrx_ppc_fpu.c -lm -o qrx-mach
./qrx-mach --map /pfad/zum/Mach-O
./qrx-mach --run /pfad/zum/statischen-testprogramm
```

`--map` bildet die Segmente testweise in höchstens 128 MiB temporärem Speicher
ab; ohne Option werden nur Metadaten ausgegeben. `test_macho.c` deckt PPC-/Fat-
Auswahl, Trunkierung, fehlerhafte Commands, Nullfüllung und die atomare
Ablehnung von Überlappungen und zu wenig Speicher ab. Der reale QuickTime
Player 7.7 (PPC7400-Slice) wurde auf dem Host inspiziert und abgebildet:
Einstieg `0x31e0`, fünf Segmente, 17 Dylib-Kommandos und ein dynamischer
Linker. `tools/qrxppc/build.sh` baut den Inspector als `C:QRXMach` und den
synthetischen Test als `C:QRXMachTest`. Letzterer lief im isolierten
ARMithlon-68040-Gast mit `Mach-O prototype: 0 failure(s)` und Rückgabecode 0;
der Test prüft Parser und Segmentmapping, nicht die Ausführung einer App.
Für isolierte Installationen kann `QRXPPC_INSTALL_TREE` auf eine Kopie des
Systembaums gesetzt werden.

`--run` startet ausschließlich den begrenzten statischen Testpfad im
PPC-Interpreter. Er bildet Segmente ab und legt einen 16-Byte-ausgerichteten
Initialstack mit `argc=1`, `argv[0]="qrxmach"` sowie leeren `env`- und
`apple`-Listen an. `r1` zeigt auf `argc`; alle übrigen Register werden auf
null gesetzt. BSD-`sc` unterstützt nur `write` (Nummer 4, ausschließlich fd 1/2)
und `exit` (Nummer 1). `write` liefert die Bytezahl beziehungsweise EBADF,
EFAULT oder EIO in `r3` und setzt bei Fehlern `CR0.SO`; unbekannte Syscalls,
sonstige Traps und abgelaufenes Instruktionsbudget stoppen den Runner.

Für den Mach-O-Start gelten die `initprot`-Rechte der Segmente: Der Interpreter
prüft bei jedem Instruktionsabruf und jedem Integer-/FPU-Speicherzugriff den
gesamten Bereich auf Ausführ-, Lese- beziehungsweise Schreibrecht. Der Stack
ist les- und schreibbar, aber nicht ausführbar; nicht zugeordnete Bereiche und
`__PAGEZERO` sind unzugänglich. Auch `write`-Puffer müssen vollständig in
einem lesbaren Segment oder im Stack liegen, sonst folgt `EFAULT`. Segmente
einschließlich reservierter Bereiche dürfen sich nicht überlagern. Ein
Zugriff über eine Segmentgrenze wird derzeit auch dann abgelehnt, wenn das
Nachbarsegment dieselben Rechte besitzt. Wenn eine Zugriffskontrolle aktiv
ist, benutzt der JIT den Interpreter; generierter Code umgeht die Kontrolle
nicht.

Der synthetische Mach-O-Test lädt `argv[0]` über `r1`, schreibt es und endet
mit Exit-Code 1 (`argc`). Im isolierten ARMithlon-68040-Gast bestehen dieser
Ablauf und die Schutztests für Lücken, `__PAGEZERO`, Code, Stack, FPU und
Syscall-Puffer (`QRXMachTest`, 0 Fehler). Der Runner weist unter anderem
dyld-/Dylib-Kommandos, unbekannte Load Commands,
Segment-Sections, nichtleere zusätzliche Thread-Zustände und gesetzte
Mach-O-Flags ausdrücklich zurück. Das ist ein eingeschränkter Prozessstart,
kein allgemeiner Darwin-Loader: Dyld, Relokationen, Imports, reguläres crt,
weitere Systemaufrufe, vollständige VM-/Seitentabellen-Semantik, Frameworks,
AltiVec und ein echter Prozess-/Dateideskriptorkontext fehlen. Die Kontrolle
ist segmentbasiert und weder eine vollständige Speicherverwaltung noch eine
Sicherheits-Sandbox; QuickTime und Ableton lassen sich damit nicht starten.

## PEF-Container (P2.1, Lesepfad)

`tools/qrxppc/qrx_pef.c` liest PEF-Container (`Joy!peff`) für `pwpc`:
Container-Header (40 B), Section-Header (28 B), den Loader-Header (56 B),
Import-Bibliotheken (24 B), Import-Symbole (4 B) und die Export-Tabelle
(Hash-Tabelle, Schlüssel-Tabelle, 10-Byte-Symboleinträge). Geprüft werden
Magic, Formatversion, Architektur, Section-Zahl, Container-Grenzen, genau eine
Loader-Section, die Gültigkeit von `main`/`init`/`term`, die Bereichsgrenzen der
Symboltabellen und NUL-terminierte Namen. Jeder Fehler hat einen eigenen Status
(`QRX_PEF_ERR_*`) statt eines Absturzes.

`qrx_pef_load()` legt die Sections bei `base + index * 0x1000` ab und prüft die
Gastgrenzen vor dem ersten Schreibzugriff. Code-, Daten- und Loader-Section
werden verbatim kopiert (die Loader-Section belegt `packedSize` Bytes), eine
pattern-initialisierte Datensektion (Kind 2) wird beim Laden expandiert: die
fünf Opcodes aus „Section Contents" (`zero`, `block copy`, `repeated block`,
`repeat with block`, `repeat with zero`), jeweils mit 7-Bit-`vnum`-Längen, und
jeder Lese- und Schreibzugriff ist auf `packedSize`/`unpackedSize` begrenzt.
Eine beschädigte Beschreibung oder eine Expansion, die nicht exakt
`unpackedSize` ergibt, endet als `QRX_PEF_ERR_PACKED`. Der Rest der Section bis
`totalSize` wird mit Nullen aufgefüllt. `qrx_pef_main_tvector()` liefert die
Adresse des Main-TVec, dessen erstes Wort der Einstiegspunkt ist.

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

Die Relokationshistorie ist ein Ringpuffer (64 Einträge): `SmRepeat`/`LgRepeat`
greifen auch in langen Streams auf die zuletzt ausgeführten Anweisungen zu,
statt an einer vollem Historie abzubrechen. Nicht enthalten: 68k-Container,
Glue-Code-Erzeugung für Mixed Mode und jeder Toolbox-Aufruf. Der Resolver
erhält Bibliothek, Symbolklasse und Namen; eine Export-Hash-Suche oder
Lazy-Bindung gibt es nicht, alle Importe werden beim Laden aufgelöst.

Eine MacOS-Anwendungsdatei öffnet `tools/qrxppc/qrx_macapp.c`:
`qrx_macbin_parse()` prüft den 128-Byte-Kopf (Versionsbyte 0, Namenslänge 1…63,
Fork-Längen, bei MacBinary II ab Byte 122 zusätzlich die CRC-16-CCITT über die
ersten 124 Bytes) und schneidet Daten- und Resource-Fork heraus;
`qrx_rsrc_find()` liest Ressourcen-Kopf, Map, Typ- und Referenzliste und liefert
eine Ressource zu Typ und ID; `qrx_cfrg_parse()` dekodiert das `cfrg`-Resource
(reserviertes Wort 0, Offset und Länge des Containers) und prüft gegen die
Länge des Daten-Forks, dass das Fragment vollständig darin liegt.
`qrx_app_load()` verkettet den Weg: MacBinary → `cfrg` → `qrx_pef_parse()` →
`qrx_pef_load()` → `qrx_pef_relocate()`. Jeder Schritt hat einen eigenen Status
(`QRX_MACAPP_ERR_*`): fehlender Resource-Fork `NO_CFRG`, unbrauchbares Fragment
`PEF`, Fork- oder Map-Grenzen `SHORT`/`RSRC`, Kopf und CRC `HEADER`/`CRC`. Der
Test `macapp` baut ein synthetisches MacBinary mit eingebettetem Fragment, lädt
es und führt es bis zum `sc` aus (Ergebnis 42), und prüft die Ablehnungen für
Kopf, CRC, Fork-Grenzen, Ressourcen-Map und `cfrg`. Werkzeug- und Finder-Felder
werden gelesen (Typ, Creator, Name), aber nicht ausgewertet; ob eine Datei eine
Applikation ist, entscheidet allein, ob sich ein ladbares Fragment findet.

## Toolbox: Memory Manager (P2.2)

`tools/qrxppc/qrx_toolbox.c` ist der erste Toolbox-HLE-Slice: der Memory
Manager. Er verwaltet eine Zone im Gästespeicher wie das Original: Jeder
Block trägt seine logische Größe im Wort vor den sichtbaren Daten (Größe bei
`Daten-4`), ein Handle ist eine Master-Pointer-Zelle (`*Handle` ist die
Datenadresse). Die Vergabe ist First-Fit über freigegebene Blöcke mit
Bump-Schwanz; eine Host-Tabelle führt je Block Basis, logische und physische
Größe und Art. PPC-Code erreicht die Schicht über einen einzigen
registrierten Import (`qrx_mm_dispatch`): `r3` ist die A-Linen-Nummer,
`r4`/`r5` ihre Argumente, das Ergebnis landet in `r3`. Abgedeckt sind
`NewPtr`/`DisposePtr`/`SetPtrSize`/`GetPtrSize`,
`NewHandle`/`DisposeHandle`/`SetHandleSize`/`GetHandleSize` sowie
`FreeMem`/`MaxMem`. `NewPtr`/`NewHandle` liefern 0, wenn nichts passt;
`Get*Size` meldet einen fremden Block als `0xFFFFFFFF`; `Dispose*`/`Set*`
liefern den Fehlercode (0 = `noErr`). Schrumpfen erfolgt in place; Wachstum
eines Handles kopiert und aktualisiert den Master-Pointer, ein
fehlgeschlagenes Wachstum lässt den Block unberührt; Pointer-Blöcke wachsen
nicht (sie sind nicht verschiebbar). Nicht enthalten: Zonen, Purging,
Locking, Kompaktierung; die Buchungen sind auf 64
Blöcke begrenzt. `toolbox_mm` prüft C-API und das laufende PPC-Fragment
(`NewPtr`, Marker über den Pointer, `NewHandle`, `GetHandleSize`) und die
Ablehnungen für Doppelfreigabe, fremde Adressen, unbekannte Trap-Nummern und
die volle Zone.

## Toolbox: Resource Manager (P2.2)

Der zweite Slice implementiert den Resource Manager in derselben Datei.
Laufzeit läuft er in einer flachen Tabelle von 32 Einträgen, die mit
`AddResource` befüllt wird; systemeigene Resourcedateien gibt es nicht (keine
Apple-Binärdateien — die Schicht wird über die C-API und das Import-Dispatch
geseeded). Ein Eintrag verbindet `type` (OSType), `id` (signed 16) und
optionalen Pascal-Namen mit einem Memory-Manager-Handle; `LoadResource` /
`ReleaseResource` schalten nur die Resident-Markierung (die Daten bleiben im
Handle), `DetachResource`/`RmveResource` entfernen den Tabelleneintrag glatt,
die Daten im Handle bleibt. Über den Import laufen die A-Linen 0xA99C–0xA9AF
(`CountResources`, `GetIndResource`, `CountTypes`, `GetIndType`,
`GetResource`, `GetNamedResource`, `LoadResource`, `ReleaseResource`,
`SizeRsrc`, `GetResAttrs`, `SetResAttrs`, `AddResource`, `RmveResource`,
`DetachResource`, `ResError`); `ResourceDispatch` (0xA822) wird mit
`$7003` (SetResourceSize) bestätigt. `ResError` liefert den klebrigen
`lastError`-Wert. Duplikate (gleiches Type+ID) sind erlaubt, der erste Treffer
gewinnt. `toolbox_rm` prüft anlegen, holen (per ID und per Name), Laden,
Entladen, Entfernen, Zählen/Indizieren, Größen und Attribute sowie
`ResError`.

## Toolbox: File Manager (P2.2)

Der dritte Slice stellt ein virtuelles Volume im Gästespeicher bereit:
„QRXPPC“, VRefNum 1 (0 = Standardvolume). Bis zu 32 Dateien, Namen Pascal
(1..31 Zeichen, case-insensitive); je Datei 16 Byte FInfo und eine
Host-Datenspeicherung (bis zu 32 KiB je Datei, volle Platte = `dskFulErr`).
Pro Access-Pfad (bis zu 16 offen) eine Referenznummer und ein Mark; Lesen und
Schreiben teilen das Mark und schieben es vor (wie auf dem Original). Der
Import unterhält `Open`/`Close`/`Read`/`Write` (A000–A003), `GetVolInfo`
(A007), `Create`/`Delete` (A008/A009), `GetFileInfo`/`SetFileInfo`
(A00C/A00D), `GetEOF`/`SetEOF` (A011/A012). Param Blocks liegen im
Gästespeicher (Layout in `qrx_toolbox.h` dokumentiert); `ioPosMode=fsFromStart`
(1) positioniert über `ioPosOffset`, `fsAtMark` (0) hält das Mark. Die
Param-Block-Lasten (ioBuffer/ReqCount/ActCount) werden aus dem Gästespeicher
gelesen; Dateiinhalte werden über die Host-Puffer kopiert. `toolbox_fm` prüft
Anlegen (inkl. Duplikat und leeren Namens), Öffnen, Schreiben, Rückenlesen
mit Seek-Positionierung, SetEOF/GetEOF, FInfo-Roundtrip, Löschen und die
Ablehnungen (fehlende Datei, Doppelfreigabe-nahes Close, RefNum 999,
fehlgeschlagene ParamBlocks).

## Toolbox: QuickDraw (P2.2)

Der vierte Slice zeichnet in 1-Bit-Bitmaps im Gästespeicher. Ein Port ist in
dieser Fassung nur sein `BitMap`-Kopf: `baseAddr` an 0, `rowBytes` an 4,
`bounds` (4×int16) an 6. Stift- und Musterzustand führt die Schicht selbst
(`qrx_qd_t`), nicht der Gastport; `thePort` wird über `qrx_tb_set_screen()`
gesetzt und von `InitGraf` veröffentlicht. `qrx_tb_init` beginnt mit einem
Null-Port (Base 0), der Gast setzt seinen Screen mit `SetPort`. Über den
Import laufen die Traps 0xA86D–0xA8AD: `InitGraf`/`InitPort`/`OpenPort`/
`SetPort`/`GetPort`, `BackPat`, `SetPt`/`MoveTo`/`Move`/`LineTo`/`Line`,
`HidePen`/`ShowPen`, `GetPen`, `PenSize`/`PenMode`/`PenPat`/`PenNormal`,
`FrameRect`/`PaintRect`/`EraseRect`/`InvertRect`/`FillRect`,
`EqualRect`/`SetRect`/`OffsetRect`/`InsetRect`/`PtInRect`. Gezeichnet wird
immer auf den aktuellen Port, geklippt auf dessen Bounds und auf die Bounds
der Bitmap; Pixel außerhalb werden still verworfen. Muster sind 8 Byte im
Gästespeicher und werden modulo 8 gekachelt; `PenMode` nimmt `srcCopy` (0)
und `patCopy` (1) an und lehnt alles andere mit `paramErr` (-50) ab, gezeichnet
wird aber immer `srcCopy` (dokumentierte Vereinfachung). `InitPort`/`OpenPort`
setzen nur den aktuellen Port, einen Port-Stack gibt es nicht. Linien sind
Bresenham, die Stiftdicke 1..8 wird als Quadrat gestempelt. Weil die Bounds
erst nach `SetPort` bekannt sind, prüft die Schicht jede Adresse mit
`qd_range()` gegen die logische Speichergröße (256 KiB) und lehnt Rect-,
Bitmap- und Pattern-Zeiger außerhalb mit `paramErr` ab. `toolbox_qd` prüft
die Rect-Helfer, `PaintRect`/`EraseRect`/`InvertRect`/`FillRect`/`FrameRect`
an konkreten Screen-Wörtern (inklusive 50-%-Muster), `LineTo` über elf
Spalten, `GetPen`, `PtInRect` und die Ablehnungen. `GetPen` schreibt die
Stiftstelle als (h,v) an +0/+2 — das ist die Hausordnung dieses Slices und
nicht die klassische `Point`-Reihenfolge (v,h); der Event-Slice benutzt
dagegen die klassische Reihenfolge. Offen: Farb-Ports, Regions, `CopyBits`,
RoundRects, Polygone und Arcs.

## Toolbox: Window Manager (P2.2)

Der fünfte Slice legt Fenster als 232-Byte-`WindowRecord` im Gästespeicher ab:
`windowKind` an 0, `visible` an 2, `refCon` an 4, `frame` an 24,
`nextWindow` an 56, `titleLength` an 64, der `GrafPort` (beginnend mit seinem
`BitMap`) an 82. Record und Content-Bitmap kommen aus der Memory-Manager-Zone;
das Bitmap ist auf die Fensterbreite zeilenausgerichtet
(`rowBytes = ((width+15)/16)*2`), maximal 8 Fenster je Zone. Die Verwaltung ist
eine Front-to-Back-Liste über `nextWindow`. Über den Import laufen
`GetWMgrPort`, `InitWindows`, `NewWindow`/`DisposeWindow`/`CloseWindow`,
`ShowWindow`/`HideWindow`, `GetWRefCon`/`SetWRefCon`, `GetTitle`/`SetTitle`,
`MoveWindow`/`SizeWindow`, `SelectWindow`/`BringToFront`/`SendBehind`,
`BeginUpdate`/`EndUpdate`/`InvalRect`, `FrontWindow`, `FindWindow`. Sichtbarkeit
entsteht durch Neuzeichnen statt durch ein Bit: `ShowWindow`, `HideWindow`,
`CloseWindow`, `MoveWindow` und `SizeWindow` komponieren den Screen neu (Desktop
löschen, dann alle sichtbaren Fenster von hinten nach vorn Pixel für Pixel aus
ihrem Content-Bitmap). Das ist bewusst kein Rahmen-System: Titel,
GoAway-Knopf, Scrollbars und Zoom-Box gibt es nicht; `FindWindow` liefert
`inContent` innerhalb des Frames und `inDesk` außerhalb, der Titel wird
host-seitig gehalten und nur seine Länge in den Record gespiegelt. `toolbox_wm`
prüft Anlegen (Kind, Frame, Record-Port), Zeichnen in den Content-Port, die
Komposition nach `ShowWindow`, das Löschen nach `HideWindow`, Verschieben,
Größenänderung mit erneuter Komposition, `FrontWindow`, `FindWindow`,
`Get/SetWRefCon` und das Aufräumen nach `CloseWindow`. Offen: Fensterrahmen,
Menüleistenbereich, echter `BeginUpdate`-Zyklus, Zoom und Regions.

## Toolbox: Menu- und Dialog-Manager (P2.2)

Der sechste Slice hält Menüs in einer Host-Tabelle von 8 Einträgen; ein
`MenuHandle` ist ein Memory-Manager-Handle, dessen Daten der 18-Byte-
`MenuInfo`-Record sind (`menuID` an 0, `menuWidth`/`menuHeight` an 2/4,
`menuProc` an 6, `enableFlags` an 10, `menuData` an 14). `NewMenu` legt den
Record an, `DisposeMenu` gibt ihn über den Memory Manager frei; Item-Titel
liegen host-seitig (maximal 16 Items je Menü, 32 Zeichen), `enableFlags` wird
nach jeder Änderung als Bitmaske (Bit 0 = Item 1) in den Gastrecord
zurückgeschrieben. Über den Import laufen `InitMenus`, `NewMenu`,
`DisposeMenu`, `AppendMenu`, `InsertMenu`, `DeleteMenu`, `ClearMenuBar`,
`DrawMenuBar` (No-Op), `GetMenuBar`/`SetMenuBar`, `CountItems`,
`GetMenuItemText`/`SetMenuItemText`, `CheckItem` (nur Merker, keine Marke im
Text), `EnableItem`/`DisableItem` und `DeleteMenuItem` mit nachrückenden
Indizes. Der Dialog-Slice ist absichtlich minimal: `InitDialogs`, `Alert`,
`StopAlert`, `NoteAlert`, `CautionAlert` und `ModalDialog` (0xA97B,
0xA985–0xA988, 0xA991) liefern sofort Item 1 — es gibt keinen Event-Loop,
keine Dialog-Ressourcen und keine Item-Verwaltung. `toolbox_menu` prüft
`NewMenu`/`AppendMenu`, `CountItems`, `Set/GetMenuItemText`, `DeleteMenuItem`
mit nachrückenden Indizes, `DisableItem`/`EnableItem` über `enableFlags` im
Gastrecord (menuID und beide Hälften des Flags-Worts werden im Fragment
gelesen, weil der Record nach `DisposeMenu` freigegeben ist), `GetMenuBar`,
`DisposeMenu` und die Ablehnungen. Offen: Menüleiste am Bildschirmrand,
Pull-Down-Tracking, Tastaturäquivalente, Dialog-Ressourcen, Item-Hit-Tests und
die Event-Integration.

## Toolbox: Text und Fonts (P2.2/4)

Der siebte Slice setzt Text über den QuickDraw-Slice. Es gibt genau eine
Schriftfamilie (0, „Chicago"), gespeist aus dem mitgelieferten 8x8-Glyphensatz
`tools/qrxppc/qrx_font8x8.h` (Public Domain, Daniel Hepper/font8x8, auf
ASCII 0x20–0x7E gekürzt). Die Glyphenzelle ist 8x8 Pixel; `TextSize` wählt
einen ganzzahligen Maßstab 1..4 (12 → 1, 18 → 2, 27 → 3, 36 → 4), damit
Metrik und Pixel immer übereinstimmen: `advance = 8*scale`, `ascent =
7*scale`, `descent = 1*scale`, `widMax = 8*scale`, `leading = 0`.
`SpaceExtra` ist ein Fixed (16.16) und wird auf jeden Zeichenvorschub
addiert. Über den Import laufen `InitFonts` (0xA8FE), `DrawChar` (0xA883),
`DrawString` (0xA884), `DrawText` (0xA885), `TextWidth` (0xA886), `TextFont`
(0xA887), `TextFace` (0xA888), `TextMode` (0xA889), `TextSize` (0xA88A),
`GetFontInfo` (0xA88B), `StringWidth` (0xA88C), `CharWidth` (0xA88D),
`SpaceExtra` (0xA88E), `GetFontName` (0xA8FF) und `GetFNum` (0xA900).
Gezeichnet wird über `qd_set_pixel`, also auf den aktuellen Port, geklippt
auf dessen Bounds; der Stift wandert nach jedem Zeichen um
`advance + SpaceExtra`. `TextMode` nimmt 0 (`srcCopy`, komplette Zelle), 1
(`srcOr`, nur gesetzte Bits — die Voreinstellung), 2 (`srcXor`) und 3
(`srcBic`); alles andere ist `paramErr`. `TextFace` wird gespeichert, aber
nicht gerendert; es gibt keine Font-Ressourcen, kein Kerning und keine
Proportionalbreiten. Pascal-Strings werden vor dem Lesen gegen die logische
Speichergröße geprüft; ein Zeiger außerhalb oder eine Größe außerhalb
1..96 endet als `paramErr`. `toolbox_text` prüft Metrik (`GetFontInfo`,
`CharWidth`, `StringWidth` mit und ohne `SpaceExtra`), `DrawString` und
`DrawText` Zeile für Zeile gegen die Glyphendaten, den Stiftvorschub, die
Namensrückrichtung (`GetFontName`/`GetFNum`) und die Ablehnungen.

## Toolbox: Event Manager (P2.2/4)

Der achte Slice ist eine synthetische Ereignisquelle im Gast. Die Warteschlange
hält 8 `EventRecord`s (16 Byte: `what` @0, `message` @2, `when` @6, `where`
als klassischer `Point` v@10/h@12, `modifiers` @14). Über den Import laufen
`GetNextEvent` (0xA970), `EventAvail` (0xA971), `GetMouse` (0xA972),
`StillDown` (0xA973), `Button` (0xA974), `TickCount` (0xA975), `GetKeys`
(0xA976), `WaitMouseUp` (0xA977), `WaitNextEvent` (0xA860), `PostEvent`
(0xA02F), `FlushEvents` (0xA032) und `SystemTask` (0xA9B4). Die Ereignismaske
filtert nach Ereignisart (Bit `1 << what`); die Maske 0 gilt als
`everyEventMask`. Passt kein Ereignis, wird ein `nullEvent` in den Record
geschrieben und 0 zurückgegeben, ohne die Queue zu verändern; `EventAvail`
lässt das gefundene Ereignis in der Queue, `GetNextEvent` entfernt es.
`FlushEvents(from,to)` entfernt alle Ereignisse, deren Typbit im inklusiven
Maskenbereich liegt. `PostEvent` legt ein Ereignis mit aktueller Tickzahl,
Mausposition und Modifikatortasten an und wird bei voller Queue verworfen
(Rückgabe 0). `WaitNextEvent` schläft nicht: es verhält sich wie
`GetNextEvent` und ignoriert `sleep` und `mouseRgn`; das ist die dokumentierte
Grenze dieses Slices. Mausposition, Maustaste, Modifikatoren und Keymap setzt
die Umgebung (`qrx_ev_set_mouse`, `qrx_ev_set_key`, `qrx_ev_set_modifiers`);
`TickCount` liest eine Host-Tickquelle (`qrx_ev_set_tick_source`) oder den
internen Zähler (`qrx_ev_set_ticks`/`qrx_ev_advance_ticks`). `GetMouse`
schreibt die klassische `Point`-Reihenfolge (v an +0, h an +2), `GetKeys`
kopiert die 16-Byte-Keymap. `toolbox_event` prüft Tickzahl, Tastenabfragen,
`GetMouse`/`GetKeys`, Posten, Maskenfilter, `EventAvail` ohne Verbrauch,
`GetNextEvent`, `nullEvent`, `FlushEvents`, Queue-Grenze, ungültige
Record-Zeiger und die API-Helfer. Offen: echte Hardware-Ereignisse, Schlafen
und Aufwachen in `WaitNextEvent`, AutoKey-Wiederholung, `SystemClick`,
Desk-Accessory- und Apple-Event-Pfade.

## Anwendungs-Runner (P2.3)

`tools/qrxppc/qrx_apprun.c` startet eine klassische MacOS-PPC-Anwendung. Der
Runner nimmt ein MacBinary-Bild (Daten- und Resource-Fork in einer Datei) oder
einen nackten PEF-Container, legt die Sections über `qrx_pef_load()` bei
`load_base` ab und liest den Haupt-`TVector`. Die Relokation hat dessen beiden
Wörtern bereits das Section-Delta addiert, sie sind also absolute Adressen; nur
wenn ein Wort in keiner geladenen Section liegt (Fragment ohne Relokation),
verschiebt der Runner es um das Delta der Code-Section. Der Aufruf
läuft über `qrx_mix_call()` mit `r2` = TOC; Import 0 ist der Trap-Seam, `r3`
trägt die A-Line-Nummer und `qrx_tb_dispatch` bedient sie. Jeder Aufruf landet
im Trace (`trap`, `selector` = `r0`, `r4`, `r5`, Ergebnis, `implemented`); der
erste Trap, den die HLE mit `QRX_MM_UNKNOWN` beantwortet, beendet den Lauf über
`qrx_mix_request_stop()` mit `QRX_APPRUN_UNIMPLEMENTED`, statt den PPC-Code mit
einem erfundenen Ergebnis weiterlaufen zu lassen. `qrx_apprun_start()` bildet
den Brückenstatus auf `EXITED`, `UNIMPLEMENTED`, `FAULT`, `LIMIT`,
`ERR_IMPORT` und `TRAP_LOOP` ab und hinterlässt eine Klartext-Erklärung in
`run->detail` („init routine/main returned after n trap(s)“). Liefert der
Loader-Header neben `main` auch einen Init-TVector (Mailboxen 2/3), ruft
`qrx_apprun_start()` zuerst die Initialisierungsroutine und danach `main` auf —
die Reihenfolge, die CFM beim Start eines Fragments einhält.

Das Kommando `QRXAppRun <app> [resource-fork]` liest die Dateien über
AmigaDOS, baut bei getrennten Forks ein MacBinary-Bild im Speicher (Version 0,
ohne CRC) und gibt Trace und Ergebnis aus. Exit-Codes: 0 = `main` kehrte
zurück, 2 = erster nicht implementierter Trap, 3 = keine PEF-Anwendung,
1 = anderer Halt. Der Runner belegt mindestens 4 MiB Gast-Speicher; für
große Fragmente wächst die Belegung auf Image + 16 MiB (höchstens 128 MiB).
Die Zone reicht von 0x1000 bis zur Ladeadresse 0x100000, Glue liegt bei
0x800, der Stack am oberen Ende.

Importe löst `tools/qrxppc/qrx_appstub.c` über eine Namens→Trap-Tabelle auf,
die aus `docs/QRXPPC_EXPANDER_IMPORTS.md` entsteht (70 Symbole: 58 direkt,
12 indirekt). Für jeden Import baut der Runner einen Stub im Gast-Speicher:
zwei Wörter TVector (Einsprung, Kopie des App-TOC), dann `li r0,0`, acht
`mr`-Kopien (`r3…r10` → `r4…r11`), `lis`/`ori r3,<Trap>`, `sc`, `blr`. Ein
direkter Import springt mit seinem A-Line-Trap in die HLE; ein indirekter
Import, dessen Argumentform einen Wrapper bräuchte, und jeder Import ohne
HLE-Eintrag erhalten die Pseudotrap `0x7E00 + Importindex`. Der Lauf stoppt
beim ersten solchen Aufruf mit `QRX_APPRUN_UNIMPLEMENTED` und der Meldung
`missing import <Bibliothek>::<Symbol>`; `qrx_apprun_missing_import()` nennt
Bibliothek und Symbol, `QRXAppRun` gibt zusätzlich die Stub-Zahlen aus
(direkt, indirekt, ohne HLE).

Die Mixed-Mode-Einstiege `NewRoutineDescriptor`,
`NewFatRoutineDescriptor`, `DisposeRoutineDescriptor` und
`CallUniversalProc` werden über die privaten Selektoren 0xEE01–0xEE04
bedient, also außerhalb des A-Line-Toolbox-Bereichs. `NewRoutineDescriptor`
legt einen `RoutineDescriptor` im 2-Byte-gepackten Format aus `qrx_mix.h` in
der Zone ab (ein Record, ISA 68k oder PPC); die Fat-Variante erzeugt zwei
Records für dieselbe Adresse. `DisposeRoutineDescriptor` gibt den Block über
den Memory Manager frei. `CallUniversalProc` löst den PPC-Record auf,
akzeptiert eine direkte Codeadresse oder einen CFM-Transition-Vektor
(Einsprungwort + TOC) und ruft das Ziel über die Brücke auf. Übersetzt werden
nur registerbasierte ProcInfos mit höchstens vier Parametern; eine
stackbasierte oder variadische ProcInfo wird abgelehnt, der Aufruf liefert 0
und `run->detail` nennt den Grund.

## System-HLE: Gestalt und Zone

`Gestalt` (0xA1AD, `QRX_TB_GESTALT`) schreibt einen 32-Bit-Wert an den
Antwortzeiger, nachdem er geprüft hat, dass dieser im Gastbereich liegt; ein
Zeiger außerhalb beendet den Lauf als Trap-Fehler statt in den Hostspeicher zu
schreiben. Beantwortet werden `'thds'` (Thread Manager, 0 = nicht vorhanden),
`'sysv'` (0x00000810 = 8.1.0), `'vers'` (0x00010000 = 1.0) und `'os  '`
(Adresse der OS-Traptabelle, hier 0 = keine). Jeder andere Selektor liefert
`gestaltUndefSelectorErr` (-5551), das ist genau das Verhalten des Mac OS für
unbekannte Attribute — die App entscheidet selbst, ob sie damit weiterläuft,
statt dass der Emulator den Lauf abbricht.

`GetZone` (0xA11A) gibt den Zonenkopf der Runner-Zone (`tb->mm.base`) zurück,
`SetZone` (0xA11B) quittiert mit `noErr` und merkt sich die Zone nicht; der
Runner hat nur eine Zone. Beide sind in `qrx_appstub.c` eingetragen, damit die
Importe über ihre echten A-Line-Traps laufen statt über die Pseudotrap.

Grenzen: 68k-Container werden nicht geladen (eine Applikation mit
`CODE`-Ressourcen und ohne `cfrg` endet als `ERR_68K`, eine
StuffIt-Installer-Nutzlast als `ERR_IMAGE`). Von den 64 abgedeckten Symbolen
sind nur die direkten vollständig nutzbar; die indirekten Wrapper und die 223
offenen Importe beenden den Lauf an ihrem ersten Aufruf. Ein `sc` mit nicht
registriertem Selektor wird als `ERR_IMPORT` mit Selektor und `r3` gemeldet,
nicht als Trap-Trace.

Verifiziert im Gast (`C:QRXAppRun`): eine synthetische MacBinary-App ruft
`TickCount` (0xA975), dann `TEInit` (0xA9CC) und danach `Button` (0xA974); der
Lauf stoppt nach zwei Traps mit `first missing trap 0x0000A9CC` und Exit 2,
`Button` wird nicht mehr erreicht. Der StuffIt-Expander-5.5-Installer aus dem
beiliegenden ISO (APPL/STi0, Daten `ST50…`, Resource-Fork mit `CODE`) wird als
`68k application: 'CODE' resources, no 'cfrg'` abgelehnt (Exit 3). `test_apprun`
in `qrx_ppc_test.c` prüft zusätzlich TVector-/TOC-Übernahme, Trace-Inhalt,
den 68k-Fall und die Ablehnung eines Nicht-MacBinary-Bildes.

Acrobat Reader 4.0 (PPC-PEF, 800 Importe) läuft nach dem Laden bis zu zwei
Aufrufen von `NewRoutineDescriptor` (beide ISA PPC, ProcInfos 0x000003C0 und
0x000FFFC0) und erhält gültige Deskriptoren; danach endet der Lauf in einem
PPC-Fehler in App-Code (`this = -1`, PC 0x00136658). Derselbe Fehler tritt
auf, wenn `NewRoutineDescriptor` einen Nullzeiger liefert — er hängt also
nicht von der Mixed-Mode-HLE ab, sondern von Acrobats weiterem Startcode
(vermutlich fehlende Begleitdateien oder ein noch offener Toolbox-Pfad).
`CallUniversalProc` wird von Acrobat vor diesem Punkt nicht aufgerufen.
Ein synthetisches PEF im Gast prüft den vollständigen Weg: `NewRoutineDescriptor`
liefert einen Deskriptor (0x1004), `CallUniversalProc` ruft damit eine
PPC-Routine auf, die 42 zurückgibt, und `main` kehrt nach zwei Traps mit
Exit 0 zurück.

Größtes reales Fragment: das PPC-Engine-Paket aus dem `ST50`-Installer des
ISO. `unar` packt den Datenfork von `StuffIt Expander™` aus (381116 Bytes,
Resource-Fork 878389 Bytes mit `cfrg`); er enthält **vier** PEF-Container
(`Joy!peffpwpc` bei 0, 248016, 281152, 337648), die das `cfrg`-Resource als
`UnStuffEnginePPC`, `UUDecodingEnginePPC`, `UnZipEnginePPC` und
`HQXEnginePPC` benennt, dazu die 68k-App als `CODE` 0–3 im Resource-Fork.
`QRXAppRun` parst den ersten Container (drei Sections: Code 215340 Bytes,
Datensektion als Pattern-Daten 26206 → 33380 Bytes, Loader 6336 Bytes),
expandiert und relokalisiert ihn (Main-TVector in der Datensektion, Einsprung
0x11924, TOC 0x4D000) und baut für die 289 Importe aus fünf Bibliotheken
58 Trap-Stubs, 8 indirekte und 223 Pseudotrap-Stubs. Der Lauf führt die
Init-Routine und den Einsprung aus: acht Traps (`Gestalt 'thds'`, drei Paare
`GetZone`+`Gestalt 'os  '`, `Gestalt 'thds'`), alle von der HLE beantwortet,
danach kehrt der Einsprung zurück (Exit 0, „main returned after 8 trap(s)“).
Das ist kein App-Start: der PEF ist eine Engine-Bibliothek, die 68k-App ruft
sie über CFM/Mixed Mode. Ein echter PPC-App-Test braucht ein
Anwendungsfragment ohne 68k-`CODE`-Segmente. Vor dem Pattern-Expander war
dieselbe Datei noch `QRX_PEF_ERR_PACKED`, davor scheiterte sie an `cfrg` und an
der Relokation (`QRX_PEF_ERR_RELOC`).


## 68k-MacOS-Runtime (P2.4)

`tools/qrxppc/qrx_m68k.c` führt klassische 68k-MacOS-Anwendungen aus. Statt
eines eigenen Interpreters läuft der 68k-Code auf dem Musashi-68020-Kern, den
das ARMithlon-Engine-Verzeichnis bereits mitbringt
(`upstream/armithlon/2.6/engine/third_party/Musashi`); `build.sh` erzeugt die
Opcode-Tabelle mit `m68kmake` neu, weil der A-Line-Handler dort den
Illegal-Instruction-Callback befragen muss (`-DM68K_ILLG_HAS_CALLBACK=1`).
Dieser Callback ist die Trap-Brücke: Jedes `0xAxxx`-Wort, das die Anwendung
ausführt, landet in `qrx_m68k_dispatch()`.

Speicherbild: ein flacher Block, standardmäßig 16 MiB mit Rückfall auf 8 und
4 MiB (`qrx_m68k_load_limited()` nimmt eine Obergrenze in Bytes,
`QRX68KRun` reicht dafür das optionale dritte Argument `mem-kib` durch). Der
Gast ist eine 32-Bit-Maschine, deshalb ist 16 MiB keine Obergrenze: ein
`mem-kib` über 16 MiB wird als erste Stufe versucht, danach folgen 16, 8 und
4 MiB; zulässig sind 256 KiB bis 4 GiB − 1 (darüber liefe die KiB→Byte-
Rechnung über). Oberhalb von 16 MiB muss die Anwendung 32-Bit-clean sein; sie erkennt das an
`_SysEnvirons` (0xA090, Version 2 samt SysEnvRec) und an `Gestalt 'addr'`,
dessen Bit 0 genau dann gesetzt ist, wenn `MemTop` über 16 MiB liegt. Der Boot allein gibt dem Gast nur ~19 MB (2 MB Chip, 16 MiB Boot-RAM, größter
freier Block 15,9 MiB); erst `C:QRXMoreMemory` macht das Board-RAM ab 32 MiB
nutzbar, dessen Größe ARMithlon mit 1/4 des Host-RAM ansetzt (`C:Avail` im
Testgast danach: 4,0 GB Fast-RAM, größter freier Block 3,99 GB). Im Gast
belegt: `QRX68KRun TestMem.mb "" 65536` meldet MemTop 0x04000000, Zone
0x02000000, Heap-Ende 0x03F00000, `NewPtr(0x1000)` = 0x02000000;
`mem-kib 1048576` (1 GiB) meldet MemTop 0x40000000, Zone 0x20000000,
Heap-Ende 0x3FF00000, `NewPtr(0x1000)` = 0x20000000, beide enden mit
`ExitToShell`. Die Heap-Basis liegt bei `mem_size / 2`, `NewPtr` darf bis
`mem_size` − 1 MiB ausgeben, ein größerer Block vergrößert also auch die
Anwendungszone. Alle vom Emulator gemeldeten
Adressen beziehen sich auf die Größe, die tatsächlich allokiert wurde:
Low Memory ab 0, `MemTop` (0x0108) = `mem_size`, `BufPtr` (0x010C) =
`mem_size` − 0x1000; Ausstiegs-Stub bei 0x1000 (`ILLEGAL`); Stack wächst von
0x80000 nach unten; Segmente beginnen bei 0x100000 und werden seitenweise
hintereinander gelegt, sie müssen unterhalb der Heap-Basis enden; die
Heap-Basis ist `mem_size / 2`, `NewPtr` darf bis `mem_size` − 1 MiB
ausgeben. Ohne klassischen `CODE`-0-Kopf liegt die A5-Welt bei 0x2000 (dort
steht die Kopie der 2-Byte-Fixtures). Mit Near-Model-Kopf wird A5 so hoch
gelegt, dass die deklarierte `below A5`-Größe darunter Platz hat:
`A5 = align4k(0x2000 + below)`. Die Sprungtabelle steht bei
`A5 + CurJTOffset`; `CurrentA5` (0x0904) und `CurJTOffset` (0x0934) werden
entsprechend gesetzt.

Laden: `qrx_m68k_load()` wertet den Resource-Fork aus (Resource-Map:
Typ-Liste, 8-Byte-Typ-Einträge, 12-Byte-Referenzen, Datenoffsets relativ zum
Datenbereich), sammelt alle `CODE`-Ressourcen und kopiert `CODE` 1..n als
Segmente. Trägt `CODE` 0 einen vollständigen Near-Model-Kopf (vier Longwörter:
`above A5`, `below A5`, Sprungtabellengröße, -offset), so wird die Tabelle nach
`A5 + Offset` kopiert und der Einstieg ist die erste Tabellenposition
(`Einstieg+2`, also `MOVE.W #segment,-(SP)`/`_LoadSeg`). Der erste Eintrag muss
auf ein geladenes Segment und einen Offset innerhalb seines Codes zeigen.
Bilder ohne vollen Kopf (die 2-Byte-Fixtures der Host-Tests) starten weiterhin
am Beginn des ersten Segments. `main` kehrt über einen `ILLEGAL`-Stub zurück,
den der Trap-Callback als „main returned“ wertet. `QRX68KRun` gibt Segmente,
A5 und Einstieg aus.

HLE-Tabelle (68k-ABI, Argumente in D0/A0 bzw. auf dem Stack):

| Trap | Name | Verhalten |
| --- | --- | --- |
| `0xA9F0` | `_LoadSeg` | klassischer Near-Model-Aufruf: Segmentnummer als Wort vom Stack, Eintrag über `PC−6`; alle ungeladenen Einträge dieses Segments werden zu Segmentnummer + `JMP abs.l` auf `Segmentbasis+4+Offset` gepatcht, dann wird der `JMP` ausgeführt |
| `0xA9F4` | `_ExitToShell` | beendet den Lauf mit `EXITED` |
| `0xA1AD` | `_Gestalt` | `'thds'`=0, `'sysv'`=0x810, `'vers'`=0x10000, `'os  '`=0, `'addr'`= 1 bei `MemTop` > 16 MiB (sonst 0), sonst -5551; Antwortzeiger wird geprüft |
| `0xA11A`/`0xA11B` | `_GetZone`/`_SetZone` | Zone = Heap-Basis, `SetZone` = `noErr` |
| `0xA11E`/`0xA11F` | `_NewPtr`/`_DisposePtr` | Bump-Allocator zwischen Heap-Basis und `mem_size` − 1 MiB, `NULL` bei Überlauf; Freigeben ist ein No-op |
| `0xA122` | `_NewHandle` | Größe als Long vom Stack; Master-Pointer im Heap, Block darunter; `NULL` bei voller Tabelle (32) oder Heap-Ende |
| `0xA023` | `_DisposeHandle` | Master-Pointer wird 0, der Slot wird wiederverwendet; der Bump-Heap nimmt den Block nicht zurück |
| `0xA024`/`0xA025` | `_SetHandleSize`/`_GetHandleSize` | Vergrößern kopiert in einen neuen Block und führt die Größe mit; unbekannte Handles liefern 0 |
| `0xA029`/`0xA02A` | `_HLock`/`_HUnlock` | No-op (nichts kann wandern), Handle kommt in A0 zurück |
| `0xA997` | `_OpenResFile` | A0 = Pascal-Pfad; erreichbar ist nur die geladene Applikation (letzter Pfadteil = App-Name), Antwort RefNum -1, sonst -43 (`fnfErr`) |
| `0xA994`/`0xA998` | `_CurResFile`/`_UseResFile` | RefNum -1; jede andere RefNum ergibt -193 (`resFNotFound`) |
| `0xA9A0` | `_GetResource` | Typ in D0, ID als Word vom Stack; ein Handle pro Ressource (gecacht) oder `NULL` |
| `0xA9A3` | `_ReleaseResource` | Handle bleibt im Cache, nur der Stack wird bereinigt |
| `0xA000`/`0xA008` | `_Open`/`_Create` | ParamBlock auf dem Stack, Berechtigung in D0; Pascal-Pfad mit ':' wird zum relativen Hostpfad, `ioRefNum` 1..8, `ioResult` gesetzt; fehlende Datei -43 (`fnfErr`), volle Tabelle -42 (`tooManyFiles`) |
| `0xA001` | `_Close` | RefNum wird freigegeben, `ioResult` 0 |
| `0xA002`/`0xA003` | `_Read`/`_Write` | `ioBuffer`/`ioReqCount` im Block geprüft, `ioActCount` gesetzt; Puffer außerhalb des Blocks → -50 (`paramErr`) |
| `0xA010`/`0xA011` | `_GetFPos`/`_SetFPos` | Position in `ioReqCount`; `ioPosMode` 0/3 = ab Mark, 1 = ab Anfang, 2 = ab Ende |
| `0xA016` | `_GetEOF` | Dateigröße in `ioEOF` (Offset 32), die Mark bleibt stehen |
| `0xA090` | `_SysEnvirons` | D0 = gewünschte Version, A0 = SysEnvRec; Version 2 wird gefüllt (environsVersion, machineType 0, System 8.1.0, Prozessor 68020, `hasFPU` 1, `hasColorQD` 1, kein AppleTalk-Treiber, `sysVRefNum` 0) und zurückgegeben, Version 0 füllt nichts, Version > 2 ergibt -5503, ein A0 außerhalb des Blocks -50 |
| `0xA86D`/`0xA86E`/`0xA86F` | `_InitPort`/`_InitGraf`/`_OpenPort` | `_InitGraf` schreibt den aktuellen Port nach `*A0`, die anderen beiden machen das Port-Argument zum aktuellen Port und setzen den Stift auf 0,0 |
| `0xA873`/`0xA874`/`0xA87D` | `_SetPort`/`_GetPort`/`_ClosePort` | Port setzen, nach `*A0` liefern, den aktuellen Port schließen |
| `0xA876`/`0xA877`/`0xA879`/`0xA87C` | `_PortSize`/`_MovePortTo`/`_SetClip`/`_BackPat` | werden angenommen; Port- und Regionsstrukturen parst die HLE nicht |
| `0xA850`/`0xA852`/`0xA853` | `_InitCursor`/`_HideCursor`/`_ShowCursor` | ohne Wirkung, es gibt keinen Zeiger |
| `0xA893`/`0xA891`/`0xA892` | `_MoveTo`/`_LineTo`/`_Line` | Punkt bei A0 (v,h), `_Line` zusätzlich den zweiten Punkt bei A1; gezeichnet wird über den `line`-Hook mit PenMode und PenSize |
| `0xA880`/`0xA896`/`0xA897`/`0xA89A`/`0xA89B`/`0xA89C`/`0xA89D`/`0xA89E` | `_SetPt`/`_HidePen`/`_ShowPen`/`_GetPen`/`_PenSize`/`_PenMode`/`_PenPat`/`_PenNormal` | `_SetPt` mit h in D0 und v in D1, `_PenSize` mit Breite in D0 und Höhe in D1, `_PenPat` mit 8 Musterbytes bei A0, `_PenNormal` setzt 1/1, Modus 0 und Schwarz |
| `0xA8A1`/`0xA8A2`/`0xA8A3`/`0xA8A4`/`0xA8A5` | `_FrameRect`/`_PaintRect`/`_EraseRect`/`_InvertRect`/`_FillRect` | Rect bei A0, `_FillRect` zusätzlich das Muster bei A1; jeder Aufruf geht als `rect`-Hook mit seiner Operationsnummer hinaus |
| `0xA8A6`/`0xA8A8`/`0xA8A9`/`0xA8AD` | `_EqualRect`/`_OffsetRect`/`_InsetRect`/`_PtInRect` | reine Rechnung im Gastblock, `_OffsetRect`/`_InsetRect` mit dh in D0 und dv in D1 |
| `0xA883`/`0xA884`/`0xA885` | `_DrawChar`/`_DrawString`/`_DrawText` | Zeichen in D0, Pascal-String bei A0, Puffer bei A0 mit erstem Byte in D0 und Anzahl in D1; Text geht als `text`-Hook mit Font, Größe und Face hinaus |
| `0xA887`/`0xA888`/`0xA889`/`0xA88A` | `_TextFont`/`_TextFace`/`_TextMode`/`_TextSize` | Argument in D0; Größen außerhalb 1..96 ergeben -50 |
| `0xA975`/`0xA972`/`0xA974`/`0xA973`/`0xA977` | `_TickCount`/`_GetMouse`/`_Button`/`_StillDown`/`_WaitMouseUp` | `_TickCount` zählt bei jedem Aufruf eins weiter, die Maus steht auf 0,0, `_Button` folgt dem letzten `_PostEvent` mit mouseDown/mouseUp |
| `0xA970`/`0xA971`/`0xA860` | `_GetNextEvent`/`_EventAvail`/`_WaitNextEvent` | Maske in D0, EventRecord bei A0; bedient wird die eine Event-Zelle, sonst kommt ein `nullEvent` mit Boolean 0; `_WaitNextEvent` schläft nicht, zählt aber einen Tick |
| `0xA02F`/`0xA032`/`0xA976`/`0xA9B4` | `_PostEvent`/`_FlushEvents`/`_GetKeys`/`_SystemTask` | `_PostEvent` füllt die eine Zelle (what in D0, message in A0), `_GetKeys` liefert 16 Nullbytes |
| `0xA985`/`0xA986`/`0xA987`/`0xA988` | `_Alert`/`_StopAlert`/`_NoteAlert`/`_CautionAlert` | antworten mit Item 1, dem Standardknopf |
| `0xA97B`/`0xA97C`/`0xA981`/`0xA982`/`0xA983` | `_InitDialogs`/`_GetNewDialog`/`_DrawDialog`/`_CloseDialog`/`_DisposeDialog` | `_GetNewDialog` liest 'DLOG' und das darin genannte 'DITL' aus dem Fork, baut daraus Record, Item-Tabelle und je ein Control für die Einträge 4..7; `_DrawDialog` zeichnet über die gfx-Hooks; `_CloseDialog` und `_DisposeDialog` geben die Controls, die Item-Tabelle und den Record an den Heap zurück und tragen den Dialog aus |
| `0xA991`/`0xA97F`/`0xA980`/`0xA98B` | `_ModalDialog`/`_IsDialogEvent`/`_DialogSelect`/`_ParamText` | `_ModalDialog` nimmt einen `mouseDown` aus der Warteschlange, der in einem 'DITL'-Control liegt, meldet dessen Item-Nummer und entfernt das Ereignis (sonst Item 1); `_IsDialogEvent` (Ereignis A0, Dialog A1) sagt 1 für so einen Klick und für `keyDown` bei vorhandenem Control; `_DialogSelect` schreibt die Item-Nummer nach `itemHit` (A2) und liefert 1 bei einem Treffer; `_ParamText` merkt sich vier Pascal-Strings |
| `0xA9C8` | `_SysBeep` | angenommen, ohne Ton |
| `0xA912`/`0xA910` | `_InitWindows`/`_GetWMgrPort` | `_InitWindows` ohne Wirkung, `_GetWMgrPort` legt einmal einen 32-Byte-Port an und schreibt ihn nach `*A0` |
| `0xA913`/`0xA914`/`0xA92D` | `_NewWindow`/`_DisposeWindow`/`_CloseWindow` | `_NewWindow` mit Storage in A0, Bounds bei A1, Titel bei A2, behind in A3 und visible/procID/goAway/refCon in D0..D3; das Fenster kommt nach vorn, `refCon` landet im Record, das Ergebnis in D0. `_DisposeWindow` und `_CloseWindow` geben die Controls, die dieses Fenster als Eigentümer nennen, und danach den 64-Byte-Record an den Heap zurück; ein späteres `_NewWindow` bekommt dieselbe Adresse |
| `0xA915`/`0xA916`/`0xA91F`/`0xA920`/`0xA921` | `_ShowWindow`/`_HideWindow`/`_SelectWindow`/`_BringToFront`/`_SendBehind` | setzen Sichtbarkeit und Front-Fenster, heben das alte Fenster auf und melden die Operation an den `window`-Hook |
| `0xA917`/`0xA918`/`0xA919`/`0xA91A` | `_GetWRefCon`/`_SetWRefCon`/`_GetWTitle`/`_SetWTitle` | `refCon` in D0, Titel als Pascal-String bei A1; der Titel steht auch im Record |
| `0xA91B`/`0xA91D`/`0xA91C` | `_MoveWindow`/`_SizeWindow`/`_HiliteWindow` | `_MoveWindow` mit h in D0 und v in D1, `_SizeWindow` mit Breite in D0 und Höhe in D1, `_HiliteWindow` mit dem Flag in D0 |
| `0xA922`/`0xA923`/`0xA928` | `_BeginUpdate`/`_EndUpdate`/`_InvalRect` | Update-Zyklus als Hook-Operation, `_InvalRect` schickt das Rect als `inval` an den Hook des Front-Fensters |
| `0xA924`/`0xA92C` | `_FrontWindow`/`_FindWindow` | `_FrontWindow` liefert den Record in D0, `_FindWindow` antwortet `inDesk` (0) und schreibt 0 nach `*A1` |
| `0xA930`/`0xA931`/`0xA932`/`0xA933`/`0xA935`/`0xA936`/`0xA934` | `_InitMenus`/`_NewMenu`/`_DisposeMenu`/`_AppendMenu`/`_InsertMenu`/`_DeleteMenu`/`_ClearMenuBar` | `_NewMenu` mit ID in D0 und Titel in A0 liefert einen Handle in A0, `_AppendMenu` hängt den Pascal-String aus A1 an, `_InsertMenu` setzt das Menü vor die ID aus D0 in die Leiste (0 = ans Ende) |
| `0xA937` | `_DrawMenuBar` | meldet jedem Menü der Leiste Titel, Position und Items an den `menu`-Hook — der Gast baut daraus das Intuition-Menü; gezeichnet wird nichts |
| `0xA939`/`0xA93A`/`0xA945` | `_EnableItem`/`_DisableItem`/`_CheckItem` | Item 0 meint das ganze Menü; `_CheckItem` mit dem Flag in D1. Das Ergebnis geht als `item-state` (Bit 0 enabled, Bit 1 checked) an den Hook, der die Flags am bestehenden Menüeintrag setzt, ohne die Leiste neu zu bauen |
| `0xA946`/`0xA947`/`0xA950`/`0xA952` | `_GetMenuItemText`/`_SetMenuItemText`/`_CountMItems`/`_DeleteMenuItem` | Item-Nummer in D0 (1-basiert), Puffer bei A1; `_CountMItems` liefert die Anzahl in D0 |
| `0xA938` | `_HiliteMenu` | Menü-Kennung in D0, 0 hebt jede Hervorhebung auf; der Hook bekommt die Kennung als `hilite`-Meldung |
| `0xA940`/`0xA942`/`0xA944` | `_SetItmIcon`/`_SetItmStyle`/`_SetItemMark` | Menü bei A0, Item in D0, Wert in D1; die Rohwerte gehen als `item-icon`/`item-style`/`item-mark` an den Hook, der sie am lebenden Eintrag anwendet, ohne Rebuild |
| `0xA84E` | `_GetItemCmd` | Menü bei A0, Item in D0, Zeiger auf ein `Char` in A1; schreibt das gespeicherte Command-Zeichen als ein Byte dorthin |
| `0xA84F` | `_SetItemCmd` | Menü bei A0, Item in D0, Zeichen in D1; legt das Command-Zeichen am bestehenden Item ab (`item-cmd`), ohne Rebuild |
| `0xA93D`/`0xA93E` | `_MenuSelect`/`_MenuKey` | `_MenuSelect` nimmt den Startpunkt (zwei Wörter) vom Stack und liefert die zuletzt gemeldete Auswahl in D0, Menü-Kennung im oberen und Item-Nummer im unteren Wort, einmalig; `_MenuKey` bildet das Zeichen aus D0 ohne Rücksicht auf Groß-/Kleinschreibung auf das erste freigegebene Item mit diesem Command-Zeichen ab, sonst 0 |
| `0xA954`/`0xA955`/`0xA956` | `_NewControl`/`_DisposeControl`/`_KillControls` | `_NewControl` mit Eigentümer (Fenster- oder Dialog-Record) in A0, Bounds bei A1, Pascal-Titel bei A2 und visible/value/min/max in D0..D3; zurück kommt ein Handle, hinter dem ein Record dieser Laufzeit steht. `_DisposeControl` gibt genau diesen Record an die Heap-Verwaltung zurück, `_KillControls` alle Controls eines Eigentümers; der nächste `_NewControl` bekommt den Platz wieder |
| `0xA957`/`0xA958`/`0xA959`/`0xA95C`/`0xA95D` | `_ShowControl`/`_HideControl`/`_MoveControl`/`_SizeControl`/`_HiliteControl` | sichtbar, h in D0 und v in D1, Breite in D0 und Höhe in D1, Hervorhebung in D0; sie ändern nur den Record |
| `0xA960`/`0xA961`/`0xA962`/`0xA963` | `_GetControlValue`/`_GetControlMinimum`/`_GetControlMaximum`/`_SetControlValue` | Wert, Minimum und Maximum als Wort in D0, `_SetControlValue` schreibt D0 in den Record |
| `0xA95E`/`0xA95F` | `_GetControlTitle`/`_SetControlTitle` | Pascal-String bei A1, in beide Richtungen |
| `0xA969`/`0xA96D` | `_DrawControls`/`_Draw1Control` | Eigentümer in A0 bzw. Control in A0; Rahmen (bei Hervorhebung gefüllt) und Titel gehen als `rect`/`text` an die gfx-Hooks, also in den Port, den der Gast gesetzt hat |
| `0xA96C`/`0xA968` | `_FindControl`/`_TrackControl` | Punkt (v,h) bei A0 (bei `_FindControl` zusätzlich der Eigentümer in A1 und der Zeiger für den Treffer in A2) bzw. bei A1; liefern `inButton` (10), wenn der Punkt in einem sichtbaren Control liegt, sonst 0; `_TrackControl` setzt dabei die Hervorhebung |
| `0xA9F9`/`0xA9FA`/`0xA9FB` | `_InfoScrap`/`_UnloadScrap`/`_LoadScrap` | `_InfoScrap` antwortet nil (kein Scrap-Record), Laden und Entladen sind ohne Scheibe ohne Wirkung |
| `0xA9FC`/`0xA9FE` | `_ZeroScrap`/`_PutScrap` | `_ZeroScrap` leert die Zwischenablage, `_PutScrap` schreibt Länge in D0, Typ in D1 und Daten bei A0; nur `'TEXT'` hat eine Entsprechung, andere Typen werden still verworfen |
| `0xA9FD` | `_GetScrap` | Typ in D0, Ziel-Handle in A0 (nil fragt nur die Größe), Offset-Zeiger in A1; liefert die kopierte Länge in D0, `noTypeErr` (-102) für andere Typen |

Jeder Trap landet im Trace (Trap, PC, D0, A0, Ergebnis, „ok“/„MISSING“); der
erste Trap ohne HLE-Antwort beendet den Lauf mit `QRX_M68K_UNIMPLEMENTED`.
`C:QRX68KRun <app> [resource-fork]` gibt Trace und Ergebnis aus, Exit-Codes:
0 = zurückgekehrt, 2 = fehlender Trap, 3 = keine `CODE`-Ressourcen, 1 = anderer
Halt.

Verifiziert im Gast (`C:QRX68KRun`): ein synthetisches MacBinary-Programm ruft
`Gestalt` (`'sysv'`, Ergebnis 0, Wert 0x810 an der Antwortadresse) und danach
`TEInit`; der Lauf stoppt bei `0xA9CC`.

Klassischer `CODE`-0-Einstieg (Near Model), Host und Gast geprüft: Ein
synthetisches Programm hat zwei Sprungtabelleneinträge (Einstieg `CODE` 2,
Cross-Segment-Aufruf `CODE` 1), beide Segmente mit 4-Byte-Kopf, in einer
unsortierten Resource-Map (`CODE` 2 vor `CODE` 1). Beim Laden stehen A5 =
0x3000, Sprungtabelle = 0x3020, Einstieg = 0x3022, `CurrentA5` = 0x3000,
`CurJTOffset` = 32. Der Lauf patcht beide Einträge (`0002 4EF9 …`,
`0001 4EF9 …`), ruft `CODE` 1 über `JSR` auf die zweite Tabellenposition auf,
die Routine schreibt 0x11111111 nach 0x3000 und liefert 0xA5A5A5A5, der
Aufrufer schreibt den Wert nach 0x3004 und beendet mit `ExitToShell`. Im
ARMithlon-Gast meldet `QRX68KRun` `2 segment(s), A5=0x00003000,
entry=0x00003022`, zwei `_LoadSeg`-Traps mit Zielen 0x00100024 und 0x00101044
sowie `exited (ExitToShell), 3 trap(s), 3 implemented`; der Runner-Exit-Code
ist in diesem Fall 0, der Gast endet nach seinem Endmarker. Dieselbe Fixture
mit VISE-Kopf in `CODE` 1 wird im Gast mit `'CODE' resources are packed` /
`VISE/InstallerMaker-packed 'CODE' resource (lowest CODE 1, 1 packed
segment(s))` und rc=4 abgelehnt, bevor Code läuft. Der Host-Test
lehnt außerdem Far Model (`0xFFFF`-Marker), Sprungtabellengrößen, die kein
positives Vielfaches von 8 sind, abgeschnittene Tabellen, Einstiegs-Offsets
außerhalb des Segments, fehlende Einstiegssegmente und Einträge ohne
`MOVE.W #segment`/`_LoadSeg`-Form mit `QRX_M68K_ERR_IMAGE` ab; ein
VISE-gepacktes Segment bleibt trotz gültigem `CODE` 0 `ERR_PACKED`.
Nicht implementiert: Far Model, `_UnloadSeg`/Rückbau der Einträge,
Code-Heaps, Verschieben von Segmenten und das Auswerten der
Segmentkopf-Zähler (gepatcht werden alle ungeladenen Einträge mit dieser
Segment-ID). Ein vollständig ungepacktes reales 68k-Programm wurde damit noch
nicht gestartet; der Nachweis ist ein synthetisches Near-Model-Programm.

Reale Installer sind **nicht** ausführbar, weil ihre `CODE`-Ressourcen
gepackt sind: VISE/InstallerMaker (LightWave 6.5, 15 Segmente) stellt jeder
Ressource einen 16-Byte-Kopf voran (`A89F 000C`, CRC, entpackte Größe 0x5FA,
gepackte Größe = Ressourcenlänge), Aladdins ADC-Packer (StuffIt Expander 5.5)
beginnt mit `ADCR`. Beide Rumpfbereiche sind kein 68k-Code (geprüft mit
Disassemblierung ab Offset 4/8/12/16). Der Loader erkennt die Köpfe in
`packed_code()` und liefert `QRX_M68K_ERR_PACKED` mit dem Format in
`m.detail`; `QRX68KRun` gibt „`CODE` resources are packed“ plus Format, die
niedrigste erkannte gepackte `CODE`-ID und die Anzahl erkannter gepackter
Segmente aus und endet mit Exit 4. Die niedrigste ID ist nicht notwendigerweise
der Programmeinstieg; die Zählung umfasst höchstens die 64 erfassten
`CODE`-Ressourcen. Im Gast belegt: LightWave meldet
„VISE/InstallerMaker-packed 'CODE' resource“, StuffIt
„Aladdin ADC-packed 'CODE' resource“, jeweils rc=4. Ungepackte Segmente gibt
es daneben auch (StuffIt `CODE` 14: 4-Byte-Prolog `00000001`, dann
`moveq #0,d3`), sie sind aber kein vollständiges Programm.

Acrobat Reader 4.05a, schreibgeschützt untersucht: Das vom Nutzer bereitgestellte
`reader_installer_4.05a.sit` (SHA-256
`07cf9f61d03986f5a5caa026004da4d063c2c97b6e2c176eb3d6cf945eac940c`)
enthält einen Installer mit Finder-Typ/Creator `APPL`/`VIS3`, einem 5.626.922
Byte großen `SVCT`-Datenfork und einem 438.033 Byte großen Resource-Fork
(SHA-256 `df78e5cc1328adc447ce3cbce8df0d6ac2e542d5ad5f8ef6f5936084a1073719`).
Der Resource-Fork enthält 26 `CODE`-Ressourcen: `CODE` 0 als Sprungtabelle,
23 als VISE-gepackt erkannte positive IDs sowie die zwei nicht mit diesem
Packkopf versehenen IDs 1000 und 1002. Die erste Sprungtabellenposition in
`CODE` 0 nennt ID 1002; der Loader wertet diese Tabelle seit dem
Near-Model-Einstieg aus, verweigert den Installer aber weiterhin wegen der
gepackten Segmente. Kein `cfrg`-Resource ist vorhanden;
`SVCT` wurde nicht entpackt, daher ist über eventuell darin enthaltene
PPC-Programme keine Aussage möglich. Im isolierten ARMithlon-Gast meldet
`QRX68KRun` `VISE/InstallerMaker-packed 'CODE' resource (lowest CODE 1, 23
packed segment(s))`; `QRXAppRun` meldet `68k application: 'CODE' resources,
no 'cfrg'` und verweigert den Start. Beide Antworten sind Ablehnungen vor der
Ausführung, keine erfolgreiche Installation. Der gezielte Host-Test prüft
VISE, ADC und zwei gepackte IDs in unsortierter Resource-Map. Ein Decoder für
VISE/ADC fehlt weiterhin; der ungepackte Near-Model-Einstieg wird inzwischen
ausgeführt, was an Acrobat ohne Decoder aber nichts ändert.

Speicherbudget, im Gast und auf dem Host geprüft (Testprogramm: `Gestalt`,
`NewPtr(0x1000)`, `GetZone`, `NewPtr(0xFFFF0)`, dann `SysEnvirons`):
mit `mem-kib 4096` meldet `QRX68KRun` 4096 KiB, `MemTop=0x00400000`,
Zone 0x00200000, Heap-Ende 0x00300000, `NewPtr(0x1000)` = 0x00200000 und der
zu große `NewPtr(0xFFFF0)` = `NULL` — keine Adresse liegt außerhalb des
Blocks. Ohne Argument scheitert die 16-MiB-Anforderung im Gast und der
Rückfall auf 8 MiB greift: `MemTop=0x00800000`, Zone 0x00400000, Heap-Ende
0x00700000. 2 MiB und 1 MiB werden mit `ERR_MEM` abgelehnt, weil die Segmente
nicht mehr unter die halbe Blockgröße passen.

Der File Manager hat kein eigenes Dateisystem: `qrx_m68k_set_fs()` installiert
fünf Hooks (`open`/`read`/`write`/`seek`/`close`), die Traps übersetzen nur
`ParamBlockRec` und Pfade. Im Gast legt `QRX68KRun` sie über `dos.library` an
und wurzelt sie im optionalen vierten Argument `[host-root]`
(`C:QRX68KRun app "" 16384 SYS:Storage/TestImages/FSRoot` → `out.txt` landet
dort); der Host-Test setzt dieselben Hooks mit `fopen`/`fread`/`fwrite` gegen
ein temporäres Verzeichnis. Pfade mit `..` werden abgelehnt, damit ein Gast
nicht aus dem Share-Baum ausbricht. Grenzen: kein `Rename`/`Delete`/`Create`
von Verzeichnissen, keine Volume- oder Finder-Informationen, keine
`PBGetCatInfo`-Aufzählung; `_GetEOF` schreibt MacOS-konform `ioEOF` auf
Offset 32, das in einem gemeinsam benutzten ParamBlock `ioBuffer` ist — eine
App muss `ioBuffer` vor dem nächsten `_Read` neu setzen (im Testprogramm
sichtbar geworden).

Der Resource Manager liest aus einer Kopie des Forks, die der Loader behält
(`m->rsrc`, `m->rsrc_len`; `qrx_m68k_unload()` gibt Block und Kopie frei); die
App-Namen für `OpenResFile` kommen aus dem MacBinary-Header. Grenzen: kein
Dateisystem (nur die eigene Datei, kein zweiter Resource-Fork, kein Schreiben),
keine benannten Ressourcen (`GetNamedResource`), keine Typlisten
(`CountResources`), und der Bump-Heap gibt bei `DisposeHandle`/`SetHandleSize`
keinen Speicher zurück — ein langer Lauf mit vielen Handles läuft irgendwann in
das Heap-Ende.

Im Gast belegt (`TestFS.mb`, `[host-root]` = `SYS:Storage/TestImages/FSRoot`):
`_Create` 0, `_Write` 0 (6 Bytes), `_Close` 0, `_Open` 0, `_Read` 0,
`_GetEOF` 0, `_GetFPos` 0 (6), `_SetFPos` 0, `_Read` 0 (4 Bytes ab Offset 2),
`_GetFPos` 0 (6), `_Close` 0 — zwölf Traps, alle von der HLE beantwortet,
`ExitToShell`; auf dem Share liegt danach `FSRoot/out.txt` mit `Hallo\n`.

QuickDraw zeichnet nicht selbst: `qrx_m68k_set_gfx()` installiert drei Hooks
(`rect` mit Operationsnummer und 8 Musterbytes, `line` mit PenMode und PenSize,
`text` mit Font, Größe und Face), die Traps übersetzen nur Punkte, Rects und
Zeiger. Im Gast druckt `QRX68KRun` jede Zeichnung ins Log
(`qd rect fill 10,20,30,40`), der Host-Test sammelt sie in einer Liste. Der
Stiftzustand (Position, Modus, Größe, Muster), der aktuelle Port und die
Textattribute liegen im Runtime-Zustand; Port- und Regionsstrukturen im
Gastblock werden nicht geparst, `_InitGraf` schreibt nur `thePort` zurück.
Nicht enthalten: `CopyBits`, Regions, Polygone, Pictures, Farb-QuickDraw,
Font-Ressourcen (Text geht als Zeichenkette an den Hook), Scrollen und
Fensterverwaltung.

Das Dialog-Management baut aus 'DLOG' (procID, visible, refCon, itemsID,
titleID) und dem genannten 'DITL' einen Stub von 64 Bytes: windowKind 2 bei 0,
`visible` bei 2, Item-Anzahl bei 4, `itemHit` bei 6, die aus den Item-Rects
gefaltete Bounds bei 8 und die Adresse der Item-Tabelle bei 16. Jeder Eintrag
der Tabelle ist 12 Bytes (Rect, Typ, Textlänge, Textoffset), die Texte folgen
hinter der Tabelle. Die Einträge der Typen 4..7 (Knopf, Checkbox, Radio,
Control) werden beim Aufbau zu Control-Records mit dem Dialog-Record als
Eigentümer, den Bounds und dem Text des Eintrags und der Item-Nummer im Feld
`item`; `_FindControl`/`_TrackControl`/`_Draw1Control` arbeiten damit auf den
'DITL'-Knöpfen wie auf einem selbst angelegten Control. `_DrawDialog` zeichnet
den Rahmen, für die Knopf-Typen 4..7 je einen Rahmen und jeden Text (die
Records zeichnen sich zusätzlich über `_DrawControls`). `_ModalDialog` nimmt
einen in der Warteschlange liegenden `mouseDown`, dessen Punkt in einem
solchen Control liegt, meldet dessen Item-Nummer als `itemHit` und entfernt
das Ereignis; ohne Treffer bleibt es bei Item 1 als Standardknopf. Weil der
`filterProc` in A0 keinen Dialog nennt, antwortet jedes Control mit
Item-Nummer, also jedes 'DITL'-Control. `_IsDialogEvent` sagt für so einen
Klick 1 (und für einen `keyDown` 1, solange der Dialog ein Control hat),
`_DialogSelect` schreibt die Item-Nummer nach `itemHit` und liefert 1, sonst 0.
`_Alert` und Verwandte antworten weiter mit Item 1: Alerts haben keine
'DITL'-Items in diesem Modell. `_CloseDialog` und `_DisposeDialog` geben den
Dialog vollständig zurück: erst die Controls (wie `_KillControls`), dann die
Item-Tabelle, deren Größe nicht im Record steht und deshalb aus
`count * 12 + Summe(len+1)` rekonstruiert wird, zuletzt den 64-Byte-Record; ein
späteres `_GetNewDialog` landet deshalb wieder auf denselben Adressen. Anders
als unter MacOS, wo `CloseDialog` den Dialog nur verbirgt und für ein späteres
`ShowDialog` behält, ist Schließen hier endgültig. Der Event Manager hat genau
eine Zelle, die nur `_PostEvent` füllt (der Punkt kommt aus dem `where` des
übergebenen Event-Records); alles andere ist ein `nullEvent`. `_WaitNextEvent` schläft nicht, sondern kehrt sofort
zurück und zählt einen Tick, damit Warteschleifen wenigstens Ticks sehen.

Fenster und Menüs hängen an zwei weiteren Hooks derselben Schicht:
`window` (Operation, Frame, Titel) und `menu` (Operation, Menütitel, Item,
Itemtext, Wert). `_NewWindow` legt einen Record von 64 Bytes im Heap an:
windowKind 0, `visible` bei 2, `hilited` bei 3, `refCon` bei 4, Frame bei 8,
`nextWindow` bei 16, Titel als Pascal-String ab 20. Das ist der Record dieses
Runtimes, keine byteweise Kopie der ROM-Struktur — eine Anwendung muss ihre
Daten über die Toolbox holen, nicht über GrafPort-Felder. Die Fensterliste
kennt `QRX_M68K_WINDOW_MAX` (8) Fenster, das Front-Fenster wird gehalten und
beim Wechsel alt/neu gehegt und gehisst. `_DisposeWindow` und `_CloseWindow`
geben den Record an den Heap zurück (der Slot wird geleert, ein Front-Fenster
auf 0 gesetzt und die Controls, die das Fenster als Eigentümer nennen, wie
`_KillControls` freigegeben), damit eine wiederverwendete Record-Adresse keine
alten Controls erbt; das eigentliche Intuition-Fenster schließt der Gast über
den `window`-Hook. Menüs liegen als Handle mit
16-Byte-Block im Gastblock (menuID bei 0, menuHeight bei 4, enableFlags bei
10), Titel und Items bleiben host-seitig; die Leiste hält bis zu acht Menüs.
Grenzen: keine Fenster-Definitionen (WDEF), kein Scrollen, Ziehen, Zoomen
oder Schließen per Klick, keine Controls, kein `_GetNewWindow` aus 'WIND',
keine Tastatur-/Mausbearbeitung der Leiste, kein Aufklappen von Menüs und kein
`_GetMenuBar`/`_SetMenuBar`.

Im AROS-Gast sind Fenster und Zwischenablage keine Protokollzeilen mehr, sondern
Teile des Systems: `QRX68KRun` öffnet für jedes MacOS-Fenster ein echtes
Intuition-Fenster auf dem öffentlichen Schirm (Titel, Lage, Größe, Ziehen-,
Tiefen- und Schließen-Gadget, eigenes IDCMP-Port) und schließt es bei
`_DisposeWindow`/`_CloseWindow` wieder. QuickDraw zeichnet in das RastPort
designierten Fensters: `_SetPort` meldet den Port über den `target`-Hook, der
Guest ordnet ihn dem Fenster zu, `RectFill`/`Move`/`Draw`/`Text` malen direkt.
Die Fenster-Ports werden über den `poll`-Hook in die Event-Queue gespült, den
die Event-Traps vor jeder Antwort aufrufen: Maustasten werden `mouseDown`/
`mouseUp` mit Position, Tastendrücke `keyDown` mit dem Zeichen, Aktivierung
`activateEvt` (Modifier-Bit 0), das Schließen-Gadget ein Klick bei (7,-8) im
Rahmen, und `IDCMP_REFRESHWINDOW` läuft über `BeginRefresh`/`EndRefresh`. Die
Queue hält acht Ereignisse (`QRX_M68K_EVENT_QUEUE`), das älteste fällt heraus;
`_PostEvent` und der Guest-Pump füllen dieselbe Queue, `_FlushEvents` leert sie.
Die Zwischenablage ist `clipboard.device`: `QRX68KRun` schreibt den Mac-Scrap
als FORM `'FTXT'`/`'CHRS'` in die AROS-Clip und liest sie von dort, sodass
Ausschneiden im Mac-Programm und Einfügen in einer AROS-Anwendung (und umgekehrt)
zusammengeht. Fehlt `clipboard.device`, bleiben die Scrap-Traps lokal
beantwortet und das Log sagt es.
Die Menüleiste ist ebenfalls ein Stück AROS: NewMenu/AppendMenu/InsertMenu/
DeleteMenu/SetMenuItemText/DisposeMenu füttern ein Modell im Gast, aus dem
`QRX68KRun` bei jeder Änderung mit `CreateMenus` (gadtools) einen Menü-Strip
baut, ihn über `LayoutMenus` mit dem VisualInfo des Fensterschirms vermisst und
mit `SetMenuStrip` an jedes Mac-Fenster hängt — die Leiste steht also oben am
Schirm wie bei jeder AROS-Anwendung. Ein Fenster, das erst nach `_DrawMenuBar`
entsteht, bekommt den bereits gebauten Strip beim Anlegen mitgehängt; es hängt
also nie ohne Menüleiste da. `_DrawMenuBar` liefert dafür nur noch
Titel, Position (`value`) und Items, es wird nichts mehr gezeichnet. Alle Items
bekommen beim Bau die Häkchenspalte (`CHECKIT`), `_CheckItem` setzt darin das
`CHECKED`-Flag — das Häkchen erscheint also wirklich im Menü. Text, Häkchen und
Freigabe ändern nur Felder des schon vorhandenen Eintrags: `_SetMenuItemText`
schickt `item-text`, `_EnableItem`/`_DisableItem`/`_CheckItem` schicken
`item-state` (Bit 0 enabled, Bit 1 checked), der Gast setzt `ItemFill->IText`
bzw. `ITEMENABLED`/`CHECKED` am lebenden `MenuItem` und lässt Intuition die
Leiste mit `ResetMenuStrip` neu zeichnen — `CreateMenus`/`FreeMenus` laufen
dabei nicht, nur ein neues oder gelöschtes Item baut die Menüs neu. Wählt der
Benutzer ein Item, kommt `IDCMP_MENUPICK` mit der Intuition-Menünummer (fünf
Bits Item, fünf Bits Menü) an; der Gast bildet sie auf Menü-Kennung und
Item-Nummer ab und stellt drei Mac-Ereignisse zu: `menuEvt` (`what` 15, Message
`menuID << 16 | item`) und danach `mouseDown`/`mouseUp` mit der Menü-Kennung in
der Message und der Position des Titels in der Leiste als `where`.
Auswerten kann das Programm die Auswahl wie unter MacOS: `_MenuSelect` holt
seinen Startpunkt (zwei Wörter, `v` zuerst) vom Stack und gibt die zuletzt
gemeldete Auswahl als ein Long zurück, Menü-Kennung im oberen und Item-Nummer
im unteren Wort — danach ist sie verbraucht, ein zweiter Aufruf ohne neue
Auswahl liefert 0. Der Punkt wird dabei nur gelesen (und die vier Bytes vom
Stack genommen), es gibt kein Tracking und kein Blockieren: die Auswahl steht
schon fest, wenn der Gast sie meldet. Command-Zeichen setzt `_SetItemCmd`
(Menü, Item, Zeichen); die HLE merkt sie sich je Item und meldet sie als
`item-cmd` an den Hook, der sie am lebenden Eintrag als `Command` mit
`COMMSEQ` einträgt — der Strip zeigt den Buchstaben also rechts am Item, und
ein späterer Rebuild trägt sie aus dem Modell wieder ein. `_GetItemCmd` liest
dasselbe Feld wieder aus (Zeiger auf ein `Char` in A1, ein Byte geschrieben);
gespeichert wird das Zeichen so, wie es kam, die Groß-/Kleinschreibung gleicht
erst `_MenuKey` an. Für das Aussehen gibt es dieselbe Naht: `_SetItmStyle`
(Mac-Bits, `1` fett, `2` kursiv, `4` unterstrichen) setzt am lebenden
`IntuiText` einen `TextAttr` mit den passenden Font-Bits — Intuition zeichnet
das Item danach in diesem Schnitt; `_SetItemMark` füllt die Merkzeichenspalte,
die Intuition nur als Haken kennt, also steht bei jedem Zeichen ungleich 0 ein
Haken; `_SetItmIcon` zeigt Icon **und** Text. Intuition rendert ein Item aus
genau einer Füllung — entweder ein `IntuiText` oder ein `Image` —, und laut
Intuition-Doku dürfen nur Objekte desselben Typs verkettet werden; eine Kette
aus Bild und Text ist also nicht möglich, und ein zweites Item landet in einer
eigenen Zeile. Der Gast zeichnet deshalb beides selbst in eine kleine Bitmap
(Ziffer vorn, Text zwei Pixel dahinter, Höhe 8 px, Default-Font) und hängt sie
als `Image` in `ItemFill`, mit gelöschtem `ITEMTEXT`; danach `ResetMenuStrip`.
GadTools' `CreateMenus` kann Bild-Items nicht bauen (`GA_Image` ist ein
Gadget-Tag), ein fertiger Strip lässt sich aber so umhängen. Die Mac-Icongrafik
gibt es hier nicht (`'ICON'`/`'SICN'`-Ressourcen fehlen), deshalb steht die
Nummer des Icons als Ziffer davor. Ändert das Programm später den Itemtext,
wird die Bitmap neu gezeichnet, das Icon bleibt also stehen. Die Bitmaps
gehören dem Gast, nicht gadtools: sie werden vor jedem `FreeMenus` (Rebuild) und
beim `_DisposeMenu` des Menüs freigegeben. `_DeleteMenuItem` gibt die Bitmap des
entfernten Items sofort frei (`aros: item 1 deleted, icon bitmap freed`) und
leert danach den freigewordenen Modellplatz, damit kein Bitmap-Zeiger doppelt
freigegeben wird — vorher blieb die Bitmap des gelöschten Items liegen, weil das
Leeren des Platzes den Zeiger nur überschrieb. Ein Rebuild trägt Stil, Marke und Icon wie die Command-Zeichen aus dem
Modell wieder ein. `_MenuKey` bildet
das Zeichen aus D0 ohne Rücksicht auf Groß-/Kleinschreibung auf das erste
freigegebene Item mit passendem Command-Zeichen ab — Menü-Kennung im oberen,
Item-Nummer im unteren Wort — und hebt wie MacOS den Titel des Menüs hervor;
ein Zeichen, das nur zu einem gesperrten Item gehört, liefert 0. Wie unter
MacOS bleibt danach der Titel des
gewählten Menüs hervorgehoben, bis das Programm `_HiliteMenu(0)` ruft — der
übliche Ablauf `MenuSelect`, Befehl ausführen, `HiliteMenu(0)` läuft also
durch: `_MenuSelect` meldet die Kennung des gewählten Menüs, `_HiliteMenu`
setzt oder löscht die Hervorhebung im Modell und meldet sie als `hilite`-Op
(`value` = Kennung, 0 = aus). Intuition hebt einen Titel nur hervor, solange
der Strip verfolgt wird, deshalb ist das ein Zustand plus Logzeile und keine
Zeichnung. Für kopflose Läufe gibt es den Schalter
`C:QRX68K_MENU_TEST`: liegt die Datei da,
wählt der Gast einmal das erste Item des ersten Menüs über denselben Pfad.
Grenzen: `_MenuSelect` führt kein Tracking aus und blockiert nicht, es gibt
also keine Hervorhebung beim Ziehen über die Menüs, `_HiliteMenu` hinterlässt
keine sichtbare Markierung am Bildschirm (nur Modell und Log); Command-Zeichen
kommen ausschließlich über `_SetItemCmd` ('MENU'-Ressourcen werden nicht
gelesen, `_GetItemCmd` schreibt genau ein Byte), Stil nur als
fett/kursiv/unterstrichen (Outline, Shadow, Condense und Extend haben keine
Amiga-Entsprechung), Merkzeichen nur als Haken und Icons nur als
Ziffern-Platzhalter vor dem Text (echte Mac-Icongrafik fehlt; der Text der
Bitmap wird im Default-Font gezeichnet, ein Item-Stil gilt dort nicht), nur
`'TEXT'` (kein `'PICT'`, `'styl'`, `'snd '`), die
Clip wird bei 64 KiB beschnitten, der Mac-Zeichensatz wird nicht nach UTF-8
gewandelt, die Menüleiste kennt keine Untermenüs, und sie erscheint nur
an einem aktiven Mac-Fenster; ein geänderter Itemtext wird nicht neu vermessen
(`LayoutMenus` läuft nur beim Bau), sehr lange Ersatztexte können also über
ihre alte Breite hinauslaufen; Fenster haben keine WDEF (kein Zoomen, Scrollen, Größenziehen),
und die Mac-Koordinaten sind Fensterkoordinaten — der Intuition-Rahmen
verschiebt den Nullpunkt.

Im Gast belegt (`TestRes.mb`, MacBinary-Name `TestRes`): `OpenResFile` → -1,
`GetResource('CODE',1)` → Handle 0x008000C8, `GetHandleSize` → 0x00C8,
`NewHandle(0x100)` → Handle 0x008001CC, `GetHandleSize` → 0x100,
`SetHandleSize(h,0x200)` → `GetHandleSize` 0x200, `DisposeHandle` → 0; neun
Traps, alle von der HLE beantwortet, Ende mit `ExitToShell`.

Im Gast mit `C:QRXMoreMemory` belegt (`TestEnv.mb`, gleiches Programm bei 16 und
64 MiB): `SysEnvirons(3)` → 0xFFFFEA81 (-5503), `SysEnvirons(2)` → 2, der
SysEnvRec enthält 0x0002/0x0000 (Version, machineType), 0x0810/0x0002 (System
8.1.0, 68020), 0x01/0x01 (FPU, Color QuickDraw) und 0x0000/0x0000 (AppleTalk,
SysVRefNum); `Gestalt 'addr'` liefert 0 bei MemTop 0x01000000 und 1 bei
0x04000000. Beide Läufe enden mit `ExitToShell`, 9 Traps, alle von der HLE
beantwortet.

Der Host-Test `tools/qrxppc/test_m68k_heap.sh` hält das fest: er lädt ein
synthetisches 68k-Programm (`NewPtr(0x1000)`, `GetZone`, `NewPtr(0xFFFF0)`,
`ExitToShell`) mit Standard-Sizing (16 MiB) sowie mit 8 und 4 MiB Budget und
prüft MemTop, BufPtr, Zone-Basis, `GetZone`-Ergebnis und das `NewPtr`-Limit
gegen die allokierte Blockgröße; ein Lauf mit 2 MiB muss mit `ERR_MEM`
abgelehnt werden, ohne einen Block zu hinterlassen. Ein zweites Programm ruft
`SysEnvirons(3)` und `SysEnvirons(2)` auf und fragt `Gestalt 'addr'`; der Test
prüft den Fehlerwert -5503, alle SysEnvRec-Felder und dass `'addr'` bei 16 MiB
0 und bei 64 MiB 1 ist. Ein drittes Programm öffnet den eigenen Resource-Fork,
holt `'CODE'` 1 als Ressource und arbeitet mit Handles; der Test vergleicht den
Handle-Inhalt mit dem Segment, prüft `GetHandleSize` vor und nach
`SetHandleSize`, dass die Daten den Umzug überstehen und dass `DisposeHandle`
den Master-Pointer leert. Ein viertes Programm fährt den File Manager über die
stdio-Hooks: `_Create`/`_Write`/`_Close`/`_Open`/`_Read`/`_GetEOF`/`_GetFPos`/
`_SetFPos`/`_Read`/`_Close`, prüft `ioResult`, `ioActCount`, `ioEOF` und die
Positionen, dass der zweite Lesevorgang ab Offset 2 beginnt und dass die Datei
im Wurzelverzeichnis wirklich `Hallo\n` enthält. Ein fünftes Programm fährt QuickDraw, Dialog
und Event über aufzeichnende Hooks: `MoveTo`/`LineTo` kommen als Linie
10,20 → 50,20 an, `FillRect` mit Rect 10,20,30,40 und Muster 0xFF, `FrameRect`,
`TextSize 12`/`TextFont 4`/`DrawString "Hi"` als Text bei 50,20, `OffsetRect`
verschiebt 1,1,5,5 auf 3,2,7,6 und `EraseRect` zeichnet dort, `Alert` liefert
1, `GetNewDialog` baut aus 'DLOG' 128 und 'DITL' 128 einen Dialog mit zwei
Items, `DrawDialog` zeichnet dessen Rahmen (10,10,45,100) und die Texte
`Ready` und `Go`, `ModalDialog` setzt ohne Ereignis `itemHit` 1, dann setzt das
Programm das `where` seines Event-Records auf 37,35 (Mitte des Knopfs), ruft
`PostEvent(mouseDown)`, `IsDialogEvent` liefert 1, `FindControl` auf denselben
Punkt `inButton` (10) mit dem Handle des 'DITL'-Knopfs und `ModalDialog`
danach `itemHit` 2 — der Klick ist damit verbraucht, `GetNextEvent` liefert 0,
nach einem weiteren `PostEvent` 1, `Button` danach 1, `TickCount` zählt. Am
Ende schließt das Programm den Dialog (`trap 0xA983 … a0=0x00800000`), baut
denselben 'DLOG' erneut (`trap 0xA97C … -> 0x00800000 ok`, also derselbe Record,
die Tabelle liegt wieder bei `Record+64`) und schließt ihn mit `trap 0xA982 …
a0=0x00800000 -> 0x00000000 ok`. Ein sechstes Programm fährt Fenster
und Menüs: `NewWindow` kommt als `create` 10,10,60,200 mit Titel `Test` an,
`ShowWindow`/`SelectWindow`/`BeginUpdate`/`EndUpdate` als eigene Operationen,
`InvalRect` als `inval` 12,14,30,40, `GetWRefCon` liefert `$1234` (auch im
Record), `FrontWindow` den Record, `NewMenu` erscheint als `new "Datei"`,
zwei `AppendMenu` als Items `Open` und `Quit`, `CountMItems` liefert 2,
`GetMenuItemText` den Text `Open`, `DrawMenuBar` den Leistentitel mit Position,
`DisposeWindow` schließt mit `dispose`. Dazu öffnet das Programm
ein echtes Intuition-Fenster (`aros: window "Test" on the public screen`,
am Ende `1 window(s) created, 0 left open`), schreibt `Hallo` per `PutScrap`
in die AROS-Clip, liest die Länge 5 und den Text über `GetScrap` in ein Handle
zurück, und der Pump liefert nach `TickCount` zwei Ereignisse: `mouseDown`
und `keyDown`. Aus `CreateMenus`, `LayoutMenus` und `SetMenuStrip` wird
`aros: menu bar 1 menu(s), 2 item(s) attached`; ein zweites Fenster
`Zweite` (70,10,160,260), das nach `DrawMenuBar` geöffnet wird, meldet
`aros: menu strip attached to "Zweite"` und wird als eigenes Intuition-Fenster
geführt. `_CheckItem`, `_DisableItem` und `_SetMenuItemText` erscheinen als
`aros: item 0 "Open" checked 1 enabled 1 (patched, check column)`, `... checked
1 enabled 0 ...` und `aros: item 0 text "Oeffnen" (patched)` — ohne eine
weitere Zeile `menu bar ... attached`, also ohne Rebuild. Mit gesetztem
Schalter `C:QRX68K_MENU_TEST` erscheint `aros: menu pick "Datei" item 1
(menu 128, event 15)`, danach `aros: 1 menu pick(s)`; alle drei `GetNextEvent`
finden ein Ereignis. Dieselbe Auswahl holt das Programm danach mit
`_MenuSelect` ab: `trap 0xA93D pc=0x1001F2 d0=0x00000001 a0=0x001002E0 ->
0x00800001 ok` (Menü 128, Item 1). Der Titel bleibt danach hervorgehoben
(`aros: menu 128 hilited ("Datei")`), bis `_HiliteMenu(0)` (`trap 0xA938 … ->
0x00000000 ok`) mit `aros: menu highlight cleared (HiliteMenu 0)` aufräumt;
ohne den Schalter liefert `_MenuSelect` `0x00000000` und es wird nichts
hervorgehoben. Danach setzt das Programm Command-Zeichen (`trap 0xA84F`),
bekommt `aros: item 0 "Oeffnen" command key 'O' (patched)` und `aros: item 1
"Quit" command key 'Q' (patched)` aus dem Strip zurück, liest das Zeichen von
Item 2 mit `trap 0xA84E … d0=0x00000002 a0=0x00800050 -> 0x00000000 ok` wieder
aus und schreibt es auf Item 1 — das Log zeigt danach `aros: item 0 "Oeffnen"
command key 'Q' (patched)`, also kam wirklich `Q` zurück. `_MenuKey` mit dem
kleinen `q` antwortet `trap 0xA93E … d0=0x00000071 -> 0x00800002 ok` (Menü 128,
Item 2; Item 1 trägt dasselbe Zeichen, ist aber gesperrt und wird
übersprungen), mit `O` `-> 0x00000000 ok`. Zuletzt setzt das Programm das
Aussehen: `trap 0xA942 … d0=0x00000002 -> 0x00000000 ok` mit `aros: item 1
"Quit" style 1 (patched, font style 0x2)`, `trap 0xA940 … -> 0x00000000 ok` mit
`aros: item 0 "Oeffnen" icon 3 (digit plus text in a 68x8 image, 118 text
pixels, ITEMTEXT 0)` — die Pixelzahl beweist, dass der Text wirklich mit im
Bild steht — sowie `aros: item 0 text "Neu" redrawn with icon 3`, wenn der Text
danach wechselt, `aros: menu "Datei" disposed, 1 icon bitmap(s) freed` beim
`_DisposeMenu` (`trap 0xA932 … -> 0x00000000 ok`) und
`trap 0xA944 … -> 0x00000000 ok` mit `aros: item 1 "Quit" mark 'X' (patched as
check)`. Danach löscht es Item 2 (`trap 0xA952 … -> 0x00000000 ok` mit `aros:
item 1 deleted, icon bitmap freed`), hängt `Ab` an und schließt das Menü:
`aros: menu "Datei" disposed, 1 icon bitmap(s) freed` — genau eine Bitmap,
nämlich die des verbliebenen Items, also weder ein Leck noch ein zweites Free.
Zum Schluss legt das Programm einen Knopf an: `trap 0xA954 pc=0x1002DA
d0=0x00000001 a0=0x00800000 -> 0x008010A0 ok` (Eigentümer ist der
Fenster-Record, Bounds 100,20,120,60, Titel `OK`, Wert 1, min 0, max 1),
`trap 0xA969 … -> 0x00000000 ok` zeichnet ihn (`qd rect frame 100,20,120,60`,
`qd text 22,100 "OK"`), `trap 0xA960 … -> 0x00000001 ok` liest den Wert,
`trap 0xA963 …` setzt ihn auf 2, `trap 0xA95E …` holt den Titel zurück,
`trap 0xA96C … d0=0x00000000 a0=0x001004D8 -> 0x0000000A ok` findet den Knopf
zum Punkt 110,30 (`inButton`), und `trap 0xA968 … -> 0x0000000A ok` trackt ihn
ebenso. Zum Schluss gibt das Programm die Records zurück: `trap 0xA955
pc=0x10035C … a0=0x008010A0 -> 0x00000000 ok` und der nächste `_NewControl`
antwortet wieder `-> 0x008010A0 ok`, ein zweiter `-> 0x008010A8 ok`,
`trap 0xA956` (`_KillControls`) räumt beide ab und der folgende `_NewControl`
landet erneut auf `0x008010A0`; der Host-Test prüft dieselben Adressen und dass
der Bump-Zeiger danach wieder auf der ersten steht. 32 Traps im Ring, 64
implementierte Traps. Er braucht keinen
AROS-SDK, nur `cc` und die Musashi-Quellen (`tools/qrxppc/m68k_gen.sh` erzeugt dafür die
Opcode-Tabelle). `tools/qrxppc/build.sh` ruft ihn am Ende jedes Builds auf, ein
Layout-Fehler bricht den Build ab.

Grenzen: kein Mac-ROM, kein Resource Manager, File Manager, QuickDraw, Event
Manager oder Dialog; der Trap-Vorrat ist die Tabelle oben. Ein Control ist ein
Knopf-Record dieser Laufzeit: es gibt keine CDEF-Prozeduren, keine Scrollbar,
Checkbox oder Radiogruppe, kein `_TrackControl`-Action-Proc (der Aufruf
blockiert nicht und ruft nichts zurück) und noch keine Verbindung von den
'DITL'-Einträgen eines Dialogs zu Controls (die Knopf-Typen 4..7 sind jetzt
Controls). `_ModalDialog` ruft keinen `filterProc` und blockiert nicht, kennt
keine Doppelklicks, keine Command-Tasten-Äquivalente je Knopf und keine
Item-Hervorhebung während des Klicks (der Treffer wird gemeldet, das Control
nicht neu gezeichnet); `_Alert`-Ressourcen mit eigenen 'DITL's und `_ParamText`
bleiben Text. `_CloseDialog` ist endgültig (kein verborgenes Fenster, kein
`_ShowDialog`), und ein Dialog, dessen Record der Gast selbst weggelegt hat,
lässt sich nicht mehr freigeben. Control-Records, Dialog-Record samt
Item-Tabelle und Fenster-Record gehen wirklich an den Heap zurück;
`_DisposePtr` kann das nicht, weil der Trap keine Größe mitbringt, und
`_GetWindow`/`_NewWindow` aus 'WIND'-Ressourcen gibt es nicht. Die `CODE`-Ressourcen
der untersuchten Installer können gepackt sein: Die VISE- und ADC-Packköpfe
werden erkannt und vor der Ausführung abgelehnt, aber nicht dekomprimiert.
`SVCT`-Installer-Payloads sind nicht erschlossen; ob und welche PPC-Programme
sie enthalten, ist ohne Decoder oder laufenden Installer nicht belegt.


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
`f1`; nur ein Import mit `QRX_MIX_RAW`-Deskriptor ohne Signatur erhält
`r3..r10`/`f1..f8` roh, ein ProcInfo-Import ohne Signatur die abgeleitete
Platzierung (bei jedem Aufruf neu gelesen). Verschachtelte Aufrufe (68k-Callback ruft
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
4. P2-Rest: Sound; die Toolbox-Slices Memory, Resource, File, QuickDraw,
   Window, Menu, Dialog, Text/Font und Event stehen (siehe oben), der
   App-Runner (P2.3) ebenfalls. Offen sind echte Anwendungen (68k-Container,
   Import-Auflösung/InterfaceLib, Mixed Mode) und die
   QuickTime-/Movie-Toolbox (P2.2/5).
