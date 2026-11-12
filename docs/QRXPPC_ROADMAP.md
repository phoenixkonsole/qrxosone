# QRXPPC — Roadmap für PPC-JIT und MacOS-HLE

Stand: 2026-11-09  
Status: P0.1-HFS0-Lesepfad und P0.2-DMG lesbar; P1.1–P1.3-Referenzkern sowie P1.4-Tier-1-68k-JIT mit Gast-Differentialtests; MacOS-HLE offen.  
Zielsystem: QRX OS One / AROS m68k auf ARMithlon, ohne Host-Mounting und ohne
Host-Ausführung der Gastsoftware.

## 1. Ziel

QRXPPC soll PowerPC-G4-Code (PPC32, Big Endian) im 68k-Gast ausführen:

```text
PPC32-G4-Gastcode
        │
        ├── Interpreter (Referenz und Fallback)
        │
        └── PPC→68k-JIT  →  ausführbare 68k-Blöcke
                                  │
                                  └── MacOS-9/OS-X-HLE
```

Das ist ausdrücklich **kein** Host-MacOS-Emulator und **keine** Virtualisierung
eines Apple-Rechners. Die teuren Host-Funktionen bleiben dort, wo sie bereits
hingehören; die Gast-CPU und die MacOS-ABI werden im QRX-Gast abgebildet.

Priorität:

1. PPC32-Integer und -FPU.
2. Klassisches MacOS 9 mit Applikations-HLE.
3. MacOS X nur als separater, ausdrücklich gegateter Forschungspfad.
4. AltiVec/G4-Vektorpfad erst nach belastbaren Integer-/FPU-Tests.

## 2. Ausgangslage

| Bereich | Stand am 2026-11-08 | Konsequenz |
|---|---|---|
| `diskimage.device` | Nativer Insert/Eject über `UnitControlA(DITAG_FILENAME)` funktioniert | CPU-Arbeit kann darauf aufbauen |
| HFS-Volumes | `HFS0:` liest DSK und QuickTime-DMG; `ICD0:`/Schreiben offen | P0.1-Lesepfad und M3 erreicht; Rest offen |
| DMG | UDIF-Plugin; QuickTime-DMG liefert ein HFS0-Listing | M3-Lesepfad verifiziert |
| SIT | Nativer `QRXUnSit`-Extraktor: SIT5 Method 0/13/15, klassisch 0/13; Resource-Forks als `.rsrc` | P0.3-Kernabnahme erfüllt; MacBinary/Finder-Info offen |
| HFS-DSK-Split | `QRXDSK` liest HFS-Katalog/Extents und joint `.sit.1/.2` mit 426/100-Headern | DropStuff-Pfad im Gast verifiziert |
| 68k-JIT | `qrx68100_jit` übersetzt 68k → AArch64 auf dem Host | nicht mit PPC verwechseln |
| Cross-ISA-Cache | ISA-Tag `QRX_DUAL_ISA_PPC` existiert nur als Metadaten-Tag | noch kein PPC-Code |
| PPC-Core | Integer-/FPU-Referenzkern + Tier-1-68k-JIT + Gast-Tests | P1.2–P1.4-Baseline; JIT-Ausbau offen |
| MacOS-HLE | keine Toolbox-/Carbon-/Cocoa-HLE | noch kein App-Start |

Die vorhandene 68k→AArch64-Arbeit ist eine wichtige Referenz für Code-Cache,
Differentialtests und Blockinvalidierung. Sie ist aber kein PPC-Backend.

## 3. Architekturentscheidungen

### 3.1 CPU-Profil

- PPC32, Big Endian, G4-Klasse (7400/7410).
- Erste Stufe: Integer, Branch, Condition Register, Load/Store, Trap.
- Zweite Stufe: G4-FPU-Baseline (FPR, FPSCR, Grundarithmetik, Vergleiche,
  Load/Store); Rundungsmodi und Exception-Flags sind umgesetzt,
  Exception-Traps und Reziprokbefehle folgen.
- AltiVec (VMX) und komplexe G4-Spezialpfade später und separat.
- Kein 64-Bit-PPC, kein MacOS-X-Universal-Binary als erstes Ziel.

### 3.2 Zwei Ausführungsebenen

- **Interpreter:** vollständige Referenz, langsam, aber diagnostizierbar.
- **JIT:** erzeugt 68k-Blockcode aus validierten PPC-Basic-Blocks.
- Jede JIT-Erweiterung braucht einen differentiellen Test gegen den Interpreter.
- Bei nicht übersetzbaren Instruktionen fällt der Block auf den Interpreter zurück.
- Niemals stillschweigend Flags, FPU-Status, Faults oder Privilegien ändern.

### 3.3 Speicher und Code-Cache

- 32-Bit-Adressraum, Big Endian, seitenweise Inventarisierung.
- PPC-Code und MacOS-Daten getrennt kennzeichnen.
- Übersetzte Blöcke mit `PC`, `ASID`, `ISA=PPC` und Seitengeneration taggen.
- Schreibzugriffe auf Code invalidieren alle Aliase und die betroffenen Blöcke.
- 68k-Seitengenerationen wie beim vorhandenen QRX-Cache verwenden.
- Kein RWX-Host-JIT; im 68k-Gast gelten die AROS-Speicherregeln.
- Instruktionscache nach Codeänderungen explizit invalidieren.

### 3.4 HLE-Grenze

- PPC-Traps und `sc`-artige Übergänge führen in eine klar versionierte HLE-Schicht.
- HLE-Aufrufe validieren Zeiger, Längen, Rechte und Rückgabewerte.
- MacOS-9-Toolbox und MacOS-X-Frameworks sind getrennte HLE-Profile.
- Keine Apple-ROM-, System- oder Framework-Binaries im Repository.
- Kein Crack, kein Dongle-Bypass und keine Umgehung von Kopierschutz.

### 3.5 68k/PPC-Mischbetrieb

- MacOS 9 ist kein reines PPC-System: 68k-Code, PPC-Code und Mixed Mode
  müssen koexistieren.
- ABI, Stackframes, Registerrettung und Trap-Rückkehr werden zuerst mit
  synthetischen Testprogrammen geprüft.
- Ein PPC-Aufruf in 68k-Code und der umgekehrte Weg sind eigene Meilensteine.
- MacOS X wird erst nach stabiler MacOS-9-Mixed-Mode-Basis bewertet.

## 4. Phasen

### P0 — Medien und Dateisysteme

Ziel: Images sind nicht nur eingelegt, sondern lesbar.

#### P0.1 HFS0/ICD0

Stand 2026-11-08: `HFS0:` ist im Lesepfad repariert. `Mount HFS0:` und
`List HFS0:` liefern das DropStuff-Volume; die Ursache war die fehlende
`.aros.startup`-Platzierung im AROS-Handler. `ICD0:` und Schreibzugriff
bleiben offen; der native `QRXDSK`-Pfad bleibt als unabhängiger Split-Pfad.

- Ursache klären, warum `ODFileSystem` nach erfolgreichem Insert kein Volume
  erkennt.
- Getrennt testen: rohes HFS-DSK, HFS-ISO, partitioniertes HFS/ISO.
- `UnitInfo`, `CMD_READ`, Mountlist, `DosType`, `DeviceType` und
  `DITAG_DiskImageType` nur mit belegten Werten testen.
- Kein Host-`hdiutil`, kein Host-Loop-Device.

Abnahme:

- `List HFS0:` liefert das Volume.
- Eine Datei kann gelesen und eine kleine Datei geschrieben werden.
- Dasselbe für `ICD0:` mit einem ISO.
- Kein Hänger bei fehlendem Medium.

#### P0.2 DMG/UDIF

- DMG-Plugin gegen komprimierte UDIF-Images prüfen.
- zlib-/bzip2-/ADC-Pfade und Partition-Map getrennt behandeln.
- Den Hänger bei `List HFS0:` reproduzierbar eingrenzen.
- QuickTime-DMG zuerst nur lesen und listen, nicht ausführen.

Stand 2026-11-08: `QRXMountImage 0 QuickTime770Leopard_1.dmg` wird vom
nativen `Devs/DiskImage/DMG`-Plugin angenommen; `Mount HFS0:` +
`List HFS0:` liefert `QuickTime770_Leopard.pkg` (72.417.093 Byte) ohne
Hänger. M3 ist damit im Lesepfad erreicht; Paketinhalt und MacOS X bleiben P3.

Abnahme:

- QuickTime-DMG lässt sich einlegen und liefert ein lesbares HFS-Volume.
- Fehlerfälle (defektes Image, falsche Unit, kein Medium) liefern einen Fehler
  statt eines Hängers.

#### P0.3 SIT5

- `xadUnFile`-Fehler `filetype is unknown` analysieren.
- Prüfen, ob `xadmaster.library`, `Libs/XAD/StuffIt`, Date Handler und
  SIT5-Client zusammenpassen.
- Falls der alte Client SIT5 nicht zuverlässig erkennt: schlanker eigener
  SIT5-Extraktor oder ein gepatchter/portierter XAD-Client.
- Resource Forks, MacBinary und Finder-Informationen erhalten.
- Nur legale Testarchive verwenden; `LightWave_6.5k.sit` nicht als Crack verwenden.

Stand 2026-11-08:

- `QRXUnSit` parst SIT5 und klassische `SIT!`-Archive nativ im ARMithlon-Gast.
- SIT5: Method 0, 13 und 15. Klassisch: Method 0 und 13.
- Resource-Forks werden als `<dateiname>.rsrc` abgelegt; Dateinamen und
  Verzeichnisstruktur bleiben erhalten.
- Gast-Verifikation: `LightWave6.5_final.sit` (13.101.372 Byte Daten,
  319.244 Byte Resource) und `ClassicFull.sit` (1.734.698 Byte Daten,
  312.599 Byte Resource) sind byte-identisch zu `unar`-Referenzen.
- Ein zweiter Lauf überschreibt die Ziele kontrolliert; die Hashes bleiben
  unverändert.
- Offen: MacBinary-Container und Finder-Informationen (Type/Creator/Flags)
  werden noch nicht geschrieben.

Abnahme:

- `LightWave6.5_final.sit` wird erkannt und vollständig extrahiert.
- Dateinamen, Resource-Forks und Verzeichnisstruktur bleiben erhalten.
- Ein zweiter Lauf überschreibt kontrolliert oder verweigert sauber.

#### P0.4 Testträger

- Testmatrix: DSK, ISO, IMG, DMG, SIT, defekte Datei.
- Jeder Test läuft im echten ARMithlon-Gast, nicht auf dem Host.
- Logs werden nach dem Test entfernt; keine Testhooks im Auslieferungsbaum.

Stand 2026-11-08 (SIT-, DSK- und DMG-Teil):

| Fall | Ergebnis im ARMithlon-Gast |
|---|---|
| SIT5, Method 0 + 15 | `LightWave6.5_final.sit`: Daten und Resource byte-identisch |
| klassisches SIT!, Method 13 | `ClassicFull.sit`: Daten-/Resource-Forks, Namen und Verzeichnisse korrekt |
| zweiter Lauf | RC 0, keine Duplikate, Hashes unverändert |
| defektes/abgeschnittenes SIT | noch offen: Gastlauf faultet derzeit ohne sauberen RC/Log |
| DSK-Split, HFS | `QRXDSK` liest `DropStuff 5.5.sit.1/.2` aus den zwei 1,44-MB-DSKs, joint 426/100 → SHA-256 `3bd09b0c64b466b16a0b81be83c94c3ff4ddfc60d158ff846eec45edc7739a42` (1.951.794 B); `QRXUnSit` RC 0, Daten `0e44c15d…`, Resource `1dcb030f…` |
| HFS0-Mount | `Mount HFS0:` + `List HFS0:` liest `Desktop` und `DropStuff 5.5.sit.1` (2 Dateien, 2800 Blocks) |
| DMG | QuickTime770Leopard_1.dmg: `HFS0:` listet `QuickTime770_Leopard.pkg` (72.417.093 Byte) |
| ISO/IMG | noch nicht Teil dieses Laufs |

Die Testlogs, `Broken.sit`, `DSK-JOIN.sit`, `DSK-OUT` und die im aktuellen
Lauf extrahierten Testbäume wurden entfernt; `S/Startup-Sequence` ist wieder
der Produktionsstand,
`S/User-Startup` fehlt.

### P1 — QRXPPC-CPU-Kern

Ziel: PPC32-Code läuft ohne MacOS, zunächst nur gegen eine definierte
Test-ABI.

#### P1.1 Spezifikation und Testvektoren

- CPU-State: GPRs, CR, LR, CTR, XER, MSR, FPSCR, FPRs, SRRs, BATs, DEC.
- Trap- und Exception-Modell.
- Endianness-, Alignment- und Fault-Regeln.
- Testvektoren aus unabhängigen PPC-Assembler-Quellen.
- Trace-Format: PC, Opcode, Register vor/nach, Speicherzugriffe, Trap.

#### P1.2 Interpreter

- Integer-Grundbefehlssatz.
- Load/Store, Branch, Condition Register, Shift/Rotate, Compare.
- Supervisor-/User-Modus und Trap-Grenzen.
- Noch keine MacOS-HLE; nur eine kleine Test-ABI.
- Der Interpreter ist der Oracle für alle späteren JIT-Tests.

Stand 2026-11-08: `tools/qrxppc` implementiert die Integer-Baseline als
portablen C-Kern; `tools/qrxppc/qrx_ppc_test.c` besteht nativ und als
`C:QRXPPC` im ARMithlon-Gast (8 Integer-Fälle, 0 Fehler). FPU ist in
P1.3 umgesetzt; Trace und Exception-Traps sind noch offen. Der P1.4-Tier-1-JIT
ist vorhanden, aber noch nicht vollständig (siehe unten); M4 ist damit
teilweise, nicht vollständig erreicht.

#### P1.3 FPU

- FPRs, FPSCR, Rundungsmodi, NaN/Infinity/Denormal-Verhalten.
- Vergleich gegen eine geprüfte Referenz.
- Sichtbare FPU-Exceptions nicht verschlucken.

Stand 2026-11-08: `C:QRXPPC` führt FPR/FPSCR, `lfs`/`lfd`/`stfs`/`stfd`,
`fadd`/`fsub`/`fmul`/`fdiv`, `fneg`/`fabs`/`fmr`/`fnabs`, `frsp`,
`fctiw`/`fctiwz`, `fcmpu`/`fcmpo`, `fsel` und `mffs`/`mtfsf` mit
FM-Feldmaske aus. Die Gast-Tests umfassen Double-/Single-Load/Store, alle
vier Grundrechenarten, alle vier Rundungsmodi, FPSCR-Transfer, NaN/SNaN,
Infinity, Division durch 0, Overflow/Underflow und die Exception-Bits;
19 Fälle, 0 Fehler im Host und im ARMithlon-Gast.
Noch offen: FPU-Exception-Traps, `mcrfs`/`mtfsfi`, Single-Arithmetik,
`fres`/`frsqrte` und AltiVec. Die P1.3-Baseline ist damit erreicht, aber
keine vollständige G4-FPU.

#### P1.4 PPC→68k-JIT, Tier 1

- Basic-Block-Decoder und 68k-Emitter.
- Register-Mapping und Lazy Condition Codes.
- Direkte Branches, Block-Chaining, indirekte Branch-Lookup.
- Code-Cache mit ISA-Tag PPC und Seitengeneration.
- Selbstmodifizierenden Code und Alias-Schreibzugriffe invalidieren.
- Interpreter-Fallback für jede nicht unterstützte Instruktion.
- Noch keine Spur-Optimierung und kein AltiVec.

Stand 2026-11-09: Tier 1 ist in `tools/qrxppc/qrx_ppc_jit.c` umgesetzt.
Der Emitter erzeugt 68k-Code für `addi`/`addis`, `ori`/`oris`/`xori`/
`xoris`, `add`/`subf`/`and`/`or`/`xor` (RC=0/1, OE=0), `andi.`/`andis.`,
`lwz`/`stw`, `lbz`/`lhz`/`lha`, `stb`/`sth`, `cmp`/`cmpl`/`cmpi`/`cmpli`
(CR0…CR7, L-Form), direkte `b`/`bl`/`ba`/`bla`- und `bc`/`bcl`-Blockenden
sowie `sc`. Die Speicherzugriffe laufen über geprüfte C-Helfer, damit
Alignment-/Bereichsfehler, `fault_addr` und Instruktionszählung mit dem
Interpreter übereinstimmen. Ein 24-Block-Cache
vergleicht die exakten Quellbytes bei jedem Einstieg; Codeänderungen zwischen
Blockläufen führen zur Neuübersetzung. `CacheClearE`/`CacheClearU` werden
nach jedem neuen Block ausgeführt.

CR0 wird lazy geführt: Der generierte Code hält den Nibble in 68k-D2 und
committet ihn erst an Blockgrenzen oder vor potenziell faultenden
Speicherzugriffen nach `cpu.cr`. Mehrere CR0-Schreiber im Block erzeugen nur
den letzten Wert; der D2-Save/Restore im Prolog/Epilog hält die C-ABI ein.
Compare-Instruktionen schreiben das gewählte CR-Feld direkt; bedingte Branches
committen zuerst einen pending CR0, werten BO/BI/CTR aus und setzen PC sowie
bei LK=1 und genommenem Branch LR.

Direkte und bedingte Branches verketten Blöcke per Tail-Jump: Jeder Block hat
einen eigenen Chain-Einstieg ohne zusätzlichen D2-Frame; unbekannte Ziele
laufen über den Block-Epilog in den Runner zurück. Nach der Übersetzung des
Zielblocks patcht `jit_link_block()` die Sprungziele auf dessen Chain-Einstieg.
Der Chain-Einstieg prüft Quellbytes und Restbudget, damit SMC und `max_insns`
innerhalb der Kette gelten; Blöcke mit Stores werden nicht als Kettenquelle
verwendet, und eine Quellbyte-Änderung verwirft den gesamten Cache. Die
Instruktionszählung erfolgt im generierten Code (`chain_insns`).

`bclr`/`bcctr` haben kein statisches Ziel. Der Block endet mit einem Lookup im
zweifach assoziativen Ziel-Cache (`QRX_PPC_JIT_CACHE_SETS` ×
`QRX_PPC_JIT_CACHE_WAYS` = 8 Sätze × 2 Wege, Index `(pc >> 2) & 7`); ein
Treffer springt in den Chain-Einstieg, ein Fehltreffer geht mit gesetztem PC
zurück in den Runner. Zwei Ziele teilen sich einen Satz, ohne einander zu
verdrängen; erst ein drittes Ziel im selben Satz ersetzt den zuletzt nicht
benutzten Weg. Der Einstieg vergleicht `cpu.pc` mit der Blockadresse, damit ein
veralteter Eintrag nach einer Invalidierung keinen fremden Block ausführt;
nicht genommene indirekte Branches nutzen den statischen Fallthrough-Link.

Abnahme im ARMithlon-Gast: 65 Fälle, davon 19 bestehende Integer-/FPU-Fälle,
der CR0-Test (`jit_cr0`), der Compare-/CR-Test (`compare_cr`), der
Branch-Test (`jit_branch` mit genommenem/nicht genommenem Pfad, CR1, Link und
CTR-Schleife), der Chaining-Test (`jit_chain`, ein nativer Einstieg für einen
direkten und einen bedingten Branch nach dem Übersetzen aller Blöcke), die
drei indirekten Ketten-Tests (`jit_chain_blr`, `jit_chain_bcctr`,
`jit_chain_beqlr`), der Zwei-Wege-Test (`jit_chain_cache_2way`, beide
Rücksprungstellen bleiben resident, der zweite Lauf ist rein nativ) und der
Drei-Ziel-Test (`jit_chain_cache_thrash`: drei Rücksprungadressen im selben
Satz, Treffer und LRU-Verdrängungen im Wechsel, jede Verdrängung kehrt korrekt
über den Runner zurück), der Mid-Block-SMC-Test (`jit_smc_midblock`: ein Store
in die eigenen Quellbytes beendet den Block, ein Store außerhalb nicht), der
deterministische Fuzz-Test (`jit_fuzz`, 64 Fälle × 16
zufällige JIT-Instruktionen), der Fault-Fuzz (`jit_fault_fuzz`, 96 Fälle mit
zufälligen 32-Bit-Adressen, RAM-Grenz- und Wrap-Adressen sowie CR0-Commit vor
dem Fault), der Breiten-/Grenztest (`jit_widths`), der logische Limit-Test
(`jit_widths_limit`), zwei gezielte Fault-Differentialfälle (OOB und
unaligned) und der SMC-/Cache-Test (`jit_cache_smc`), der PEF-Ladetest (`pef_loader`, synthetischer Container mit
TVec, interpretiert und per JIT) und der Relokationstest (`pef_reloc`, TVec-,
Import- und Section-Fixups mit Resolver, interpretiert und per JIT) sowie die
siebenundzwanzig Mixed-Mode-Fälle (`mixed_call`, `mixed_stack`, `mixed_fpu`,
`mixed_import`, `mixed_nested`, `mixed_errors`, `mixed_sig_call`,
`mixed_sig_fp`, `mixed_sig_import`, `mixed_sig_stack`, `mixed_sig_errors`,
`mixed_rd`, `mixed_rd_errors`, `mixed_proc_import`, `mixed_proc_errors`,
`mixed_rd_nested`, `mixed_rd_nested_errors`, `mixed_rd_procinfo`,
`mixed_rd_procinfo_errors`, `mixed_proc_procinfo`, `mixed_proc_procinfo_errors`,
`mixed_rd_invoke_procinfo`, `mixed_rd_invoke_procinfo_errors`,
`mixed_rd_call_procinfo`, `mixed_rd_call_procinfo_errors`, `mixed_glue_procinfo`,
`mixed_import_procinfo`);
die Deskriptor-Importe und beide Deskriptor-Einstiegspunkte ohne Signatur leiten
die Platzierung aus dem `ProcInfo` ab, `mixed_glue_procinfo` prüft den
durchgängigen Glue-Fall (PPC → Import → 68k-Callback mit UniversalProcPtr →
PPC ohne Signatur) samt verschachteltem Rahmen und abgelehntem `ProcInfo`.
Dieser Fall hängt an derselben Funktion wie die drei `qrx_mix_rd_invoke()`-
Mutanten und braucht deshalb keine eigene Mutation. `mixed_import_procinfo`
prüft Index-Importe, denen ein Deskriptor für die `ProcInfo`-Ableitung
zugeordnet ist; eine zwanzigste Mutation schaltet genau diese Ableitung in
`mix_run()` ab und wird von diesem Fall erkannt.
`QRXPPC: 0 failure(s)`.
Die bestehenden Fälle laufen zusätzlich per JIT auf einer separaten
Speicherkopie und vergleichen den vollständigen CPU-/Speicherzustand. Der
Gast-Mutationstest `tools/qrxppc/mutation_test.sh` baut sechs Varianten mit
entferntem Byte-/Halfword-Range- oder Halfword-Alignment-Check; alle sechs
werden von `jit_widths_limit` erkannt. Eine siebte Variante durchsucht im
Ziel-Cache nur den ersten Weg (wieder direkt abgebildet); sie wird von
`jit_chain_cache_2way` erkannt, eine achte schaltet die Mid-Block-SMC-Prüfung ab
und wird von `jit_smc_midblock` erkannt. Eine neunte akzeptiert jede
Deskriptor-Version (`byte > 7` statt `!= 7`); sie wird von `mixed_rd_errors`
erkannt. Eine zehnte überspringt die Deskriptorprüfung bei der Registrierung
eines Imports; sie wird von `mixed_proc_errors` erkannt. Eine elfte lässt
`qrx_mix_rd_invoke()` den 68k-Record wählen und wird von `mixed_rd_nested`
erkannt, eine zwölfte stellt den CPU-Zustand nach einem verschachtelten Aufruf
nicht wieder her und ebenfalls von `mixed_rd_nested`. Eine dreizehnte dreht die
Konventionsprüfung in `qrx_mix_rd_procinfo_sig()` um und wird von
`mixed_rd_procinfo` erkannt, eine vierzehnte lässt die Größenprüfung eines
Parameterfeldes fallen und wird von `mixed_rd_procinfo_errors` erkannt. Eine
fünfzehnte sucht die Import-Platzierung nur im 68k-Record und wird von
`mixed_proc_procinfo` erkannt, eine sechzehnte legt die abgeleitete Signatur nur
bei der Registrierung zugrunde und wird von `mixed_proc_procinfo_errors`
erkannt. Eine siebzehnte schaltet die `ProcInfo`-Ableitung in
`qrx_mix_rd_invoke()` ab und wird von `mixed_rd_invoke_procinfo_errors` erkannt,
eine achtzehnte zieht das `ProcInfo` auch einer ausdrücklichen Signatur vor und
wird von `mixed_rd_invoke_procinfo` erkannt. Eine neunzehnte schaltet die
Ableitung in `qrx_mix_rd_call()` ab und wird von
`mixed_rd_call_procinfo_errors` erkannt.

Der Testbuild selbst stößt an eine Grenze von `elf2hunk`: Liegt der Text der
Binärdatei über 64 KiB, kann das Werkzeug eine PC-relative Relokation
(`R_68K_PC32`) nicht mehr als `RELRELOC32` ausdrücken und meldet
`relocation offset N is too big for RELRELOC32`. Solche Relokationen entstehen
aus Tail-Calls (`return f(...)`), die GCC als `bra.l` übersetzt; deshalb bauen
`tools/qrxppc/build.sh` und `build_variant` in `mutation_test.sh` mit
`-fno-optimize-sibling-calls`. Dann bleiben nur absolute `R_68K_32`-Bezüge
übrig, und der Hunk ist wieder `loadseg()`-fähig. Die Flagge ist kein
Geschwindigkeitsschalter und darf nicht entfernt werden, solange der Text die
64 KiB überschreitet.

Block-Chaining für direkte, bedingte und indirekte Branches ist damit
umgesetzt. Offen für den vollständigen P1.4-Ausbau: ISA-Tag und
Seitengeneration und Alias-Invalidierung, FPU-JIT und
Trace-/Performance-Ausbau. Tier 1 ist ein belastbarer Anfang, keine
vollständige P1.4-Abnahme.

#### P1.5 ABI und Mixed Mode

- PPC-Calling-Convention.
- Übergang 68k → PPC und PPC → 68k.
- Stackframes, Register-Rettung, Fehler- und Trap-Rückkehr.
- Synthetische Mixed-Mode-Tests.

Stand 2026-11-09: `tools/qrxppc/qrx_mix.c` implementiert den Übergang in beide
Richtungen. `qrx_mix_call()` baut den PowerOpen-Glue-Rahmen (16-Byte
augerichteter SP, Linkage-Area, Parameterbereich), legt Argumente in
`r3..r10`/`f1..f8` und führt bis zum Sentinel-`sc` aus; PPC-Code ruft
68k-Callbacks über `sc` mit dem Import-Index in `r0` auf, inklusive
verschachtelter Aufrufe mit gesichertem PPC-Zustand. `qrx_mix_sig_t` beschreibt
Parameter (`i`/`p`/`l`/`d`) und Rückgabetyp; `r3..r10` und `f1..f8` werden
getrennt gefüllt, überzählige Argumente landen 8-Byte-ausgerichtet im
Parameterbereich ab `24(r1)`, und Imports erhalten die Stack-Wörter samt `sp`
im Callback. `qrx_mix_rd_resolve()`/`qrx_mix_rd_call()` akzeptieren zusätzlich
einen Routine-Deskriptor im GastSpeicher: Trap-Wort, Version, Flags und
Record-Tabelle werden geprüft, der erste Record passender ISA gewählt und
`procDescriptor` absolut oder deskriptor-relativ aufgelöst. `qrx_mix_register_proc()`
registriert einen 68k-Callback unter der Deskriptoradresse, den PPC-Code mit
dieser Adresse in `r0` statt eines Import-Index anspricht; der Callback erhält
den 68k-Einstiegspunkt als `args->proc`. Fünfzehn synthetische Fälle prüfen
Rahmen, Register- und Stack-Argumente, gemischte Integer-/FP-Listen, Rückgabe,
68k-Speicherzugriff, Deskriptoren und die Fehlerpfade. Offen: Glue-Code-Erzeugung, Aggregate/Varargs in Signaturen,
Dispatch-Deskriptoren mit Selektoren, CFM-Fragmentvorbereitung und gemischte
Codefragmente.

Abnahme P1:

- Ein PPC-Testprogramm läuft interpretiert und per JIT mit identischem
  Endzustand.
- Randomisierte Instruktionstests und Fault-Tests bestehen.
- Codeänderungen invalidieren den Cache nachweislich.
- Es gibt noch keine Aussage wie „MacOS läuft“.

Stand 2026-11-09: Der Tier-1-Satz besteht differentiell, inklusive Fault-,
SMC-, Compare-/CR-, Branch- und Chaining-Tests für direkte und indirekte
Branches, inklusive Mid-Block-SMC. Randomisierte JIT-Instruktionstests und der
volle P1.4-Ausbau bleiben offen.

### P2 — MacOS 9 HLE

Ziel: Eine kontrollierte klassische MacOS-9-App startet ohne vollständige
MacOS-Installation im Gast.

#### P2.1 Loader und Laufzeit

- PEF/Code Fragment Manager, MacBinary und Resource-Forks.
- 68k-/PPC-Codefragmente und Mixed-Mode-Übergänge.
- Speicherverwaltung, Handles, Zeigervalidierung.
- Fehlerbehandlung statt Hänger.

Stand 2026-11-09: `tools/qrxppc/qrx_pef.c` liest Container-Header,
Section-Header, Loader-Header, Import- und Exporttabellen und lädt ungepackte
Sections in den Gastspeicher, und `qrx_pef_relocate()` wendet die
PEF-Relokationsanweisungen an: TVec-, Section- und Import-Fixups laufen über
einen Resolver, der alle Importe vor dem ersten Schreibzugriff auflöst.
`pef_loader` und `pef_reloc` führen synthetische Container interpretiert und
per JIT aus. Der Mixed-Mode-Übergang steht als Brücke (P1.5) und nimmt
auch Routine-Deskriptoren als Einsprungpunkt; offen sind gepackte
Sections, MacBinary/Resource-Forks, 68k-Container, Dispatch-Deskriptoren
mit Selektoren und Glue-Code-Erzeugung für echte MacOS-Aufrufe.

#### P2.2 Toolbox-Minimum

Reihenfolge:

1. Memory Manager und Resource Manager.
2. File Manager und Apple-Event-Grundlagen.
3. QuickDraw, Window Manager, Dialog Manager, Menu Manager.
4. Event Manager, Text/Font und Sound.
5. QuickTime-/Movie-Toolbox nur nach stabilem Fenster- und Dateipfad.

#### P2.3 Testprogramme

- PPC-„Hello World“ über die Test-ABI.
- Ein PPC-Toolbox-Programm öffnet ein Fenster und zeichnet Text.
- Ein 68k-/PPC-gemischtes Programm.
- Ein MacBinary-/Resource-Fork-Test.
- Erst danach: QuickTime und LightWave mit lizenzierten Datenträgern.

Abnahme P2:

- Eine kleine MacOS-9-PPC-App startet, zeichnet, nimmt Events an und beendet
  sich sauber.
- Speicher-, Datei- und Resource-Fehler führen zu definierten Fehlern.
- Kein Crack und kein Dongle-Bypass im Testpfad.

### P3 — MacOS X PPC (separater Forschungspfad)

MacOS X ist nicht „MacOS 9 mit anderer Toolbox“. Benötigt werden mindestens:

- Mach-O-PPC-Loader und dyld-Semantik.
- libSystem/POSIX, CoreFoundation, Carbon/Cocoa-Teilmenge.
- Quartz/CoreGraphics und WindowServer-Ersatz.
- Framework- und Bundle-Laden, Objective-C-Runtime.
- QuickTime-7.7-Frameworks und ihre Abhängigkeiten.

Dieser Pfad wird erst nach P2 als Machbarkeitsprojekt freigegeben. Ein
erfolgreicher QuickTime-DMG-Mount ist noch kein MacOS-X-Launcher.

### P4 — Applikations-Bring-up

- QuickTime 7.7 (MacOS X) erst nach P3.
- LightWave 6.5 (MacOS 9) nur mit gültiger Lizenz und Dongle.
- Jede App bekommt eine eigene Kompatibilitätsmatrix.
- Keine pauschale Aussage „MacOS-9/X-Apps laufen“, solange die Matrix nicht
  reproduzierbar ist.

## 5. Meilensteine

| ID | Meilenstein | Abnahme |
|---|---|---|
| M0 | Nativer Insert/Eject | `QRXMountImage` legt DSK/ISO ein und wirft aus |
| M1 | Lesbares HFS | HFS0-Lesen verifiziert; ICD0 und Schreiben offen |
| M2 | SIT5 | Testarchiv wird vollständig und korrekt extrahiert |
| M3 | DMG | QuickTime-DMG liefert ein HFS0-Listing ohne Hänger (Lesepfad) |
| M4 | PPC-Interpreter | PPC-Testprogramm läuft mit Trace und Fehlermodell |
| M5 | PPC-JIT | Tier 1: identischer Endzustand, Lazy-CR0-, Valid-/Fault-Fuzz-, SMC- und Fault-Tests bestanden; vollständiger Ausbau offen |
| M6 | Mixed Mode | 68k↔PPC-Aufrufe und Trap-Rückkehr |
| M7 | MacOS-9-Minimum | PPC-Toolbox-Fenster zeichnet und beendet sauber |
| M8 | MacOS-9-App | QuickTime/LightWave-Test mit lizenzierten Medien |
| M9 | MacOS-X-Loader | Mach-O/dyld-Testprogramm, noch keine GUI |
| M10 | MacOS-X-App | QuickTime 7.7 als separater, messbarer Forschungserfolg |

M0 bis M3 sind **Voraussetzungen** für die PPC-Arbeit. M4 bis M6 dürfen parallel
zur HFS-/DMG-/SIT-Arbeit als isolierter CPU-Testkern beginnen, aber nicht als
„MacOS-Unterstützung“ bezeichnet werden.

## 6. Test- und Messstrategie

- **Instruktionstests:** jede PPC-Instruktion, Edge Cases, Alignment, Overflow.
- **Differentialtests:** Interpreter vs. JIT nach jedem Block; vollständiger
  CPU-State-Hash.
- **Fault-Tests:** ungültige Adresse, Protection, Trap, FPU-Exception.
- **SMC-Tests:** Code schreiben, Cache invalidieren, neuen Block ausführen.
- **Mixed-Mode-Tests:** 68k→PPC→68k mit Register- und Stackprüfung.
- **HLE-Tests:** Zeiger-, Längen- und Rechtevalidierung; Fehlercodes.
- **App-Tests:** definierte Start-, Zeichen-, Datei- und Exit-Szenarien.
- **Performance:** nur mit vollständigen Workloads und reproduzierbaren Images;
  keine theoretischen MIPS- oder GHz-Aussagen.
- **Logs:** Jeder Test hinterlässt PC, Opcode, Trap und Fehlercode; keine
  stillen Fallbacks.

## 7. Risiken und Gegenmaßnahmen

| Risiko | Wirkung | Gegenmaßnahme |
|---|---|---|
| 68k-JIT ist für PPC-G4 zu langsam | MacOS X wird unpraktikabel | P1-Performance-Gate; MacOS 9 zuerst; X separat |
| Selbstmodifizierender Code | falsche oder alte Blöcke | Seitengenerationen, vollständige Alias-Invalidierung |
| PPC-Exception-/MMU-Semantik | MacOS-Toolbox bricht | kleinteilige Fault-Tests vor HLE |
| Mixed Mode | undefinierte Register-/Stackzustände | synthetische ABI-Tests vor echter App |
| Ressource-Forks/Dateitypen | Apps finden ihre Daten nicht | HFS-/MacBinary-Pfad in P0/P2 |
| QuickTime-Frameworks | riesige Abhängigkeitskette | QuickTime erst nach stabilem HLE-Minimum |
| Apple-ROM/OS-Rechte | keine verteilbare Basis | nur lizenzierte lokale Testmedien, keine Binaries im Repo |
| Crack/Dongle | rechtlich und technisch falsche Abkürzung | nicht verwenden; lizenzierte LightWave-Installation |
| MacOS X HLE | Aufwand sprengt Projekt | eigener Forschungspfad mit Abbruchkriterien |

## 8. Rechtliche Leitlinien

- Keine Umgehung von Dongle-, Seriennummer- oder Kopierschutzprüfungen.
- Keine Apple-System-, ROM- oder Framework-Binaries im Repository.
- Testmedien und Applikationen nur mit nachweisbarer Lizenz.
- HLE-Schnittstellen clean-room aus dokumentiertem Verhalten implementieren.
- Herkunft und Lizenz jedes übernommenen Decoders, Extraktors oder Testvektors
  dokumentieren.
- Der QuickTime-DMG- und LightWave-Pfad ist erst dann ein Produktmerkmal, wenn
  die Lizenzfrage geklärt ist.

## 9. Arbeitspakete und Reihenfolge

### Sofort

1. HFS0-Mount repariert; ICD0 und Schreibpfad offen; DSK-Testmatrix steht,
   Split-SIT über `QRXDSK` bleibt nutzbar.
2. SIT5-Extraktion repariert; Resource-Forks geprüft; MacBinary/Finder-Info offen.
3. DMG-Hänger beseitigt; QuickTime-DMG listbar; Paketinhalt bleibt P3.
4. Testhooks und kopierte Testmedien aus dem Auslieferungsbaum entfernen.

### Danach

5. QRXPPC-Spezifikation und PPC-Testvektoren anlegen.
6. Interpreter für PPC32-Integer und FPU.
7. Differentialtest-Harness und Trace-Format.
8. 68k-JIT-Tier-1 mit Code-Cache und SMC-Invalidierung (Tier 1 mit Lazy-CR0,
   Compare-Feldern und Block-Chaining für direkte, bedingte und indirekte
   Branches umgesetzt; Seitengenerationen und FPU-JIT offen).
9. Mixed-Mode-ABI.
10. MacOS-9-HLE-Minimum.
11. Erst dann QuickTime/LightWave-Bring-up.

### Nicht in dieser Roadmap

- Vollständige MacOS-9- oder MacOS-X-Systemvirtualisierung.
- Apple-Hardwareemulation (CUDA, NVRAM, ADB, ATA usw.) als erster Schritt.
- AltiVec-/G4-Vektorbeschleunigung vor Integer-/FPU-Stabilität.
- Host-Mounting oder Host-Ausführung von Gastsoftware.
- Crack-/Dongle-Bypass.

## 10. Definition of Done

QRXPPC v0.1 ist erreicht, wenn:

- PPC32-Testprogramme interpretiert und per JIT laufen,
- Interpreter und JIT denselben CPU-/Speicherzustand liefern,
- Code-Cache-Invalidierung und Faults getestet sind,
- 68k↔PPC-Mixed-Mode-Aufrufe funktionieren,
- eine kleine MacOS-9-PPC-Toolbox-App ohne Host-MacOS startet.

QRXPPC ist **nicht** erreicht, wenn nur ein PPC-Disassembler, ein einzelner
Testblock oder ein eingelegtes DMG existiert. Jede Stufe wird separat
abgenommen; die Bezeichnung „MacOS-HLE“ wird erst ab M7 verwendet.


## P0-Status vor dem HFS0-Fix (historisch)

- `diskimage.device` selbst funktioniert: `UnitInfo`, `TD_CHANGESTATE`,
  `TD_GETGEOMETRY` und `CMD_READ` liefern für das StuffIt-ISO
  (`45520200...`, 27900 Zylinder) und für das HFS-DSK (`cyl=80, heads=2,
  trk=18`) gültige Daten.
- Ursache für verschwindende Images gefunden: Eine diskimage-Unit wird
  zerstört und das Image ausgeworfen, sobald der letzte Opener schließt.
  `QRXLoadDevice` hält deshalb jetzt die Units 0..9 offen;
  `QRXMountImage` setzt zusätzlich `DITAG_Error` und für Unit 4 den
  CD-Gerätetyp.
- Verbleibender Blocker: `HFS0:`/`ICD0:` melden nach dem Insert weiterhin
  „no disk in drive“. Ein Mount des DOSDrivers nach dem Insert
  (`C:Mount ... FROM DEVS:DOSDrivers/...`) blockiert; ein Leseprobe direkt
  am Device ist dagegen erfolgreich. Damit liegt der Fehler in der
  DOS-Handler-/Mount-Schicht (insbesondere `L:ODFileSystem` 1.0-beta3 und
  der optischen 2048-Byte-Geometrie), nicht mehr im Image-Insert.
- Für die beiden DropStuff-DSKs ist der Mount nicht mehr nötig: `QRXDSK`
  liest HFS-Katalog und Datei-Extents direkt, joint die Segmente und übergibt
  das Archiv an `QRXUnSit`. Der HFS0-Mount bleibt der offene P0.1-Punkt.
  Aufruf: `C:QRXDSK <disk1.dsk> <disk2.dsk> <joined.sit>`; Quelle:
  `tools/armithlon/diskimage/qrx-dsk.c`.
- Nächster konkreter Schritt: `ODFileSystem`-Mountpfad für 512-Byte-HFS
  und Apple-Partition-Map-Offsets prüfen bzw. den Handler mit einer frischen
  Unit nach dem Insert starten. Der Split-DSK-Pfad ist bereits nutzbar;
  erst nach dem Mountfix folgen StuffIt Expander und Adobe Reader aus den
  Images.

## Fortschritt 2026-11-08 (nach HFS0-Fix)

- HFS0-Lesepfad: `Mount HFS0:` + `List HFS0:` liest das DropStuff-DSK mit
  `Desktop` und `DropStuff 5.5.sit.1` (2 Dateien, 2800 Blocks). Die
  `.aros.startup`-Sektion des AROS-Handlers liegt jetzt an Hunk-Offset 0.
- DMG: `QRXMountImage 0 QuickTime770Leopard_1.dmg` + `Mount HFS0:` +
  `List HFS0:` liefert `QuickTime770_Leopard.pkg` (72.417.093 Byte) ohne
  Hänger. Das ist der M3-Lesepfad, kein MacOS-X-Start.
- P1.1/P1.2: `tools/qrxppc` enthält den portablen PPC32-Integer-Referenzkern
  und `C:QRXPPC`; die acht Gast-Testfälle bestehen mit 0 Fehlern.
- Zum damaligen Stand offen: ICD0/Schreibzugriff, P1.3-FPU, P1.4-JIT,
  P1.5-Mixed-Mode sowie P2 (PEF/MacBinary, Toolbox-HLE). Ohne diese Schichten
  startet noch kein MacOS-Programm; der DMG-Mount allein ist ausdrücklich kein
  Launcher.
