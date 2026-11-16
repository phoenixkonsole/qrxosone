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
3. MacOS X als separater PPC-Mach-O-/Darwin-Pfad neben klassischem MacOS.
4. AltiVec/G4-Vektorpfad erst nach belastbaren Integer-/FPU-Tests.

### Mac-OS-X-Pfad: erster Mach-O-Prototyp

Ein separater PPC32-Mach-O-Parser und Mapper (`qrx_macho.c`) erkennt dünne
und universelle ausführbare Dateien, liest `LC_UNIXTHREAD` und bildet
`LC_SEGMENT`-Daten begrenzt im Gastspeicher ab. Der Host-Inspector
`qrx-mach [--map] <datei>` verarbeitet den PPC7400-Teil von QuickTime Player
7.7; er meldet 17 Bibliothekskommandos und vorhandenes dyld. Auch ein
AROS/m68k-Build ist in `tools/qrxppc/build.sh` integriert: `QRXMach` inspiziert
Dateien und startet mit `--run` einen engen statischen PPC-Testfall;
`QRXMachTest` prüft Parser, Segmentmapping und Prozessstart. Im isolierten
ARMithlon-Gast lief ein synthetisches Programm mit Initialstack, `write` und
`exit` sowie Fehlerpfaden mit 0 Testfehlern und Rückgabecode 0. Segmentbasierte
R/W/X-Prüfungen schützen Interpreter und Syscall-Puffer; bei aktiver Prüfung
fällt der JIT auf den Interpreter zurück. Dynamisches Linken, weitere
Darwin-ABI-Funktionen, reguläres crt und Systembibliotheken fehlen weiterhin;
QuickTime und Ableton laufen damit nicht. Framework-Funktionen werden anhand
der tatsächlich getesteten Programme bedarfsorientiert analysiert und
implementiert, statt eine vollständige Framework-Nachbildung vorauszusetzen.

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
prüft Index-Importe, denen `qrx_mix_register()` einen Deskriptor für die
`ProcInfo`-Ableitung mitgibt, und dass die Rohregister nur noch über
`QRX_MIX_RAW` erreichbar sind: Signatur und Deskriptor schließen sich aus, fehlt
beides, bleibt die vorherige Bindung samt Import-Slot unangetastet. Eine
zwanzigste Mutation schaltet die Ableitung in `mix_run()` ab, eine
einundzwanzigste die Prüfung, die den Rohregisterpfad an `QRX_MIX_RAW` bindet;
dieser Fall erkennt beide. Eine zweiundzwanzigste lässt
`qrx_mix_register_proc()` die Ablehnung einer fehlenden Signatur aus; sie endet
stattdessen in `QRX_MIX_ERR_SIG`; `mixed_proc_procinfo_errors` erkennt sie an
der erwarteten `QRX_MIX_ERR_ARG` und daran, dass der abgelehnte Deskriptor
unregistriert bleibt. Eine dreiundzwanzigste lässt `qrx_mix_call()` eine
fehlende Signatur wieder als Rohregisterpfad durchgehen; `mixed_errors` erkennt
sie, weil es die Ablehnung samt unverändertem PC prüft. Eine vierundzwanzigste schneidet in `qrx_cfrg_parse()` die Prüfung gegen
die Daten-Fork-Länge ab (`> fork_size` statt `> fork_size - offset`); `macapp`
erkennt sie an einem `cfrg`, dessen Fragment über das Fork-Ende hinausragt und
trotzdem angenommen würde. Eine fünfundzwanzigste lässt in
`qrx_mm_set_handle_size()` das Wachstum ohne Datenkopie zu (`mm_copy(..., 0)`);
`toolbox_mm` erkennt sie am verlorenen Kopf nach dem Wachstum.
Eine sechsundzwanzigste dreht die ID-Prüfung in `rm_find()` um
(`e->id != id`); `toolbox_rm` erkennt sie an den Holen je ID und am falschen
Treffer. Eine siebenundzwanzigste addiert in `fm_dispatch_io()` 1 auf
`f->size`/`ioActCount` des GetEOF-Pfades; `toolbox_fm` erkennt sie an
`ioActCount = size+1`. Eine achtundzwanzigste lässt `qd_pat_bit()` immer 1
liefern; `toolbox_qd` erkennt sie daran, dass das 50-%-`FillRect` eine volle
Zeile statt `0x0AA00000` malt. Eine neunundzwanzigste setzt in
`wm_composite()` jedes Pixel auf 0 statt auf 1; `toolbox_wm` erkennt sie an
der leeren Komposition nach `ShowWindow` und nach dem Verschieben. Eine
dreißigste dreht in `menu_write_record()` die Enabling-Bedingung um
(`!m->enabled[i]`); `toolbox_menu` erkennt sie an den beiden Hälften von
`enableFlags` im Gastrecord nach `DisableItem`/`EnableItem`.
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
den 68k-Einstiegspunkt als `args->proc`. Siebenundzwanzig synthetische Fälle prüfen
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

Stand 2026-11-13: `tools/qrxppc/qrx_pef.c` liest Container-Header,
Section-Header, Loader-Header, Import- und Exporttabellen, lädt die Sections in
den Gastspeicher und expandiert pattern-initialisierte Datensektionen (Kind 2)
mit den fünf PEF-Pattern-Opcodes; und `qrx_pef_relocate()` wendet die
PEF-Relokationsanweisungen an: TVec-, Section- und Import-Fixups laufen über
einen Resolver, der alle Importe vor dem ersten Schreibzugriff auflöst.
`pef_loader` und `pef_reloc` führen synthetische Container interpretiert und
per JIT aus. Der Mixed-Mode-Übergang steht als Brücke (P1.5) und nimmt
auch Routine-Deskriptoren als Einsprungpunkt. `qrx_macapp.c` liest inzwischen
den MacBinary-II-Kopf samt CRC und Fork-Grenzen, den Resource-Fork mit Map,
Typ- und Referenzliste und das `cfrg`-Resource; `qrx_app_load()` lädt daraus
den Container und führt ihn bis zum Einsprung. `macapp` prüft den Weg von der
Anwendungsdatei bis zum laufenden Code (Ergebnis 42) und die Ablehnungen für
Kopf, CRC, Fork-Grenzen, Map und `cfrg`. Offen sind 68k-Container, eine Namens→Trap-Abbildung für die 289 Importe
einer echten App, Dispatch-Deskriptoren mit Selektoren, Glue-Code-Erzeugung für
echte MacOS-Aufrufe und die Toolbox selbst.

#### P2.2 Toolbox-Minimum

Stand: Schritt 1 bis 4 sind HLE-bar. Schritt 1/2: `NewPtr`/`NewHandle` mit
echter Master-Pointer-Semantik, Größen-API, `FreeMem`/`MaxMem`, Resource
Manager (flat 32 Einträge, AddResource-Seed, Resident-Markierung) und File
Manager (virtuelles Volume „QRXPPC" ab VRefNum 1, bis zu 32 Dateien,
Pascal-Namen case-insensitive, Access-Pfad-Marks). Schritt 3: QuickDraw
(1-Bit-Bitmaps im Gästespeicher, Port-Bounds-Clipping, Rect-Helfer,
Paint/Erase/Invert/Fill/Frame, Stiftzustand, Bresenham-Linien), Window Manager
(232-Byte-`WindowRecord` aus der MM-Zone, Content-Bitmap, Front-to-Back-Liste,
`ShowWindow`/`HideWindow`/`CloseWindow`/`MoveWindow`/`SizeWindow` komponieren
den Screen neu) sowie Menu- und Dialog-Manager (`MenuInfo` im MM-Handle,
Item-Titel host-seitig, `enableFlags`-Spiegel im Gastrecord; Alerts und
`ModalDialog` liefern sofort Item 1). Schritt 4: Text/Font (eine synthetische
Systemschrift „Chicago" aus dem Public-Domain-8x8-Satz `qrx_font8x8.h`,
`InitFonts`/`DrawChar`/`DrawString`/`DrawText`/`TextWidth`/`TextFont`/
`TextFace`/`TextMode`/`TextSize`/`GetFontInfo`/`StringWidth`/`CharWidth`/
`SpaceExtra`/`GetFontName`/`GetFNum`, Metrik und Pixel aus demselben
Maßstab) und Event Manager (8er-Queue aus `EventRecord`s, `GetNextEvent`/
`EventAvail`/`WaitNextEvent`/`PostEvent`/`FlushEvents`, `TickCount` aus einer
Host-Tickquelle, `GetMouse`/`Button`/`StillDown`/`WaitMouseUp`/`GetKeys`,
`SystemTask`; `WaitNextEvent` schläft nicht). Alles erreichbar über das
vereinheitlichte Import-Dispatch (`qrx_tb_dispatch` mit `r3` =
A-Linen-Nummer, `r4…` = Argumente). Umgesetzt in `tools/qrxppc/qrx_toolbox.c`;
`toolbox_mm`, `toolbox_rm`, `toolbox_fm`, `toolbox_qd`, `toolbox_wm`,
`toolbox_menu`, `toolbox_text` und `toolbox_event` laufen im Gast durch die
`sc`-Naht und prüfen Ablehnungen (Doppelfreigabe, Fremdadresse, unbekannte
Trap-Nummer, volle Zone; Resource-NotFound; fehlende/duplizierte Dateien,
RefNum 999, unvollständige ParamBlocks; Rect-/Bitmap-/Pattern-Zeiger außerhalb
der 256 KiB, falscher Fenstertyp, unbekanntes Menü; Textgröße 0, unlesbare
Pascal-Strings, ungültige Record-Zeiger, volle Event-Queue). Pointer-Blöcke
schrumpfen nur; Handle-Wachstum kopiert, ein Fehlschlag lässt den Block
unberührt. Offen bleiben Zonen, Fensterrahmen, CopyBits, Sound, echte
Hardware-Ereignisse und Schlafen in `WaitNextEvent` —
und MacOS X bleibt der getrennte P3-Forschungspfad (Mach-O/dyld/libSystem).

Reihenfolge:

1. Memory Manager und Resource Manager.
2. File Manager und Apple-Event-Grundlagen.
3. ~~QuickDraw, Window Manager, Dialog Manager, Menu Manager~~ (erledigt;
   Dialog Manager bewusst ohne Event-Loop).
4. ~~Event Manager und Text/Font~~ (erledigt); Sound offen.
5. QuickTime-/Movie-Toolbox nur nach stabilem Fenster- und Dateipfad.

#### P2.3 Testprogramme

Stand 2026-11-14: Der App-Runner steht. `tools/qrxppc/qrx_apprun.c` lädt ein
MacBinary-Bild oder einen nackten PEF-Container, legt die Sections bei
`load_base` ab, liest den TVector (nach der Relokation absolut; nur bei
Fragmenten ohne Relokation wird das Code-Delta addiert), ruft ihn über die
Mixed-Mode-Brücke mit `r2` = TOC auf und protokolliert jeden Toolbox-Trap
(Trap, Selektor, `r4`, `r5`, Ergebnis). Importe löst `qrx_appstub.c` über eine
Namens→Trap-Tabelle auf; jeder Import bekommt einen Stub im Gast-Speicher,
unbekannte Symbole eine Pseudotrap `0x7E00 + Index`. Der erste Trap ohne
HLE-Antwort beendet den Lauf über `qrx_mix_request_stop()` mit
`missing import <Bibliothek>::<Symbol>`; das Gast-Kommando `C:QRXAppRun` gibt
Trace, Stub-Zahlen und Klartextgrund aus (Exit 2 = fehlender Trap, 3 = keine
PEF-Anwendung). `test_apprun` prüft den Weg im Gast: `TickCount` → `TEInit`
(fehlt) → `Button` (wird nicht mehr erreicht), dazu 68k-Ablehnung und ein
Nicht-MacBinary-Bild.

Mixed-Mode-Grundlagen, Stand 2026-11-15: `NewRoutineDescriptor`,
`NewFatRoutineDescriptor`, `DisposeRoutineDescriptor` und
`CallUniversalProc` sind als HLE vorhanden (private Selektoren
0xEE01–0xEE04). `CallUniversalProc` übersetzt registerbasierte ProcInfos mit
bis zu vier Parametern, erkennt direkte Codeadressen und CFM-TVektoren und
ruft das Ziel über die Brücke auf. Ein Host-Harness prüft direkten Aufruf,
TVector, Fat-Deskriptor, Freigabe und die Ablehnung einer nicht
registerbasierten ProcInfo. Acrobat Reader 4.0 kommt damit über den ersten
fehlenden Import hinaus: zwei `NewRoutineDescriptor`-Aufrufe werden bedient,
danach folgt ein app-interner PPC-Fehler (`this = -1`), der auch bei
Rückgabe 0 auftritt. Nächste Schritte: stackbasierte/variadische ProcInfos,
68k-Records ausführbar machen und Acrobats Begleitdateien bereitstellen.

- PPC-„Hello World“ über die Test-ABI: als `macapp`-Suite vorhanden.
- PPC-Toolbox-Programm mit Fenster und Text: Fenster- und Text-Slices sind
  einzeln abgenommen (`toolbox_wm`, `toolbox_text`); ein kombiniertes
  App-Programm fehlt noch.
- Ein 68k-/PPC-gemischtes Programm: `mixed_*`-Suiten decken die Brücke ab,
  ein gemischtes Anwendungsprogramm fehlt.
- MacBinary-/Resource-Fork-Test: `macapp`-Suite und `apprun`-Suite.
- Erst danach: QuickTime und LightWave mit lizenzierten Datenträgern.

Reales PPC-Fragment, geprüft 2026-11-14: Im `ST50`-Installer des
StuffIt-Expander-ISO steckt kein PPC-Programm, sondern ein Engine-Paket.
`unar` packt den Datenfork aus; er enthält vier `Joy!peffpwpc`-Container
(Offset 0, 248016, 281152, 337648), die das `cfrg`-Resource als
`UnStuffEnginePPC`, `UUDecodingEnginePPC`, `UnZipEnginePPC` und
`HQXEnginePPC` ausweist, während die Anwendung als segmentierte 68k-App
(`CODE` 0–3) im Resource-Fork liegt. `C:QRXAppRun` parst, expandiert und
relokalisiert den ersten Container (3 Sections, Datensektion als Pattern
26206 → 33380 Bytes), baut für die 289 Importe aus fünf Bibliotheken
58 Trap-Stubs, 8 indirekte und 223 Pseudotrap-Stubs, führt Init-Routine und
Einsprung aus und beendet mit Exit 0: acht Traps (`Gestalt`, `GetZone`), alle
von der HLE beantwortet, dann kehrt die Engine zurück. `Gestalt` (0xA1AD:
`'thds'`, `'sysv'`, `'vers'`, `'os  '`, sonst -5551) und
`GetZone`/`SetZone` (0xA11A/0xA11B) sind seit diesem Lauf in der HLE, die
Namens→Trap-Abbildung steht in `tools/qrxppc/qrx_appstub.c`, die vollständige,
nach Bibliothek gruppierte Liste mit HLE-Markierung in
`docs/QRXPPC_EXPANDER_IMPORTS.md` (66 von 289 abgedeckt, 223 offen). Nächster
Schritt für einen echten App-Start: ein Anwendungsfragment ohne
68k-`CODE`-Segmente (z. B. eine reine PowerPC-App) besorgen und dort den Lauf
bis zum nächsten fehlenden Import wiederholen; die Engine-Traps selbst sind
abgearbeitet.

Reale Medien, geprüft 2026-11-13: Das `Stuffit-Expander.iso` enthält auf dem
HFS-Volume „Stuffit Expander 5.5“ nur den Installer
(APPL/STi0, Datenfork `ST50…`, Resource-Fork mit `CODE`-Ressourcen) — eine
68k-Anwendung ohne `cfrg`, die der Runner mit
`68k application: 'CODE' resources, no 'cfrg'` ablehnt. `LightWave6.5_final.sit`
enthält nach dem Entpacken nur ein `SVCT`-Installer-Archiv, keine Applikation;
`QuickTime770Leopard_1.dmg` liefert ein Mac-OS-X-Paket (P3). Ein echtes
PPC-/PEF-Programm liegt damit noch nicht vor; der Trap-Trace ist über die
synthetische App abgenommen, die Liste der fehlenden Traps einer echten App
lässt sich erst mit einem solchen Binary messen.

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

#### P2.4 68k-MacOS-Runtime (2026-11-14)

Der 68k-Pfad steht als Grundgerüst: `tools/qrxppc/qrx_m68k.c` lädt die
`CODE`-Ressourcen einer klassischen Anwendung in einen flachen 16-MiB-Mac-
Adressraum (Rückfall 8/4 MiB), legt die Segmente ab 0x100000 ab und führt sie
auf dem Musashi-68020-Kern aus. Trägt `CODE` 0 einen vollständigen
Near-Model-Kopf, wird die Sprungtabelle nach `A5 + CurJTOffset` kopiert, A5
oberhalb der deklarierten Globals platziert und im ersten Eintrag gestartet.
`QRX68KRun` protokolliert jeden A-Line-Trap; MemTop, Heap-Basis und die
`NewPtr`-Grenze leiten sich aus der tatsächlich allokierten Blockgröße ab
(`mem-kib` als optionales Argument, Rückfall 16 → 8 → 4 MiB; ein `mem-kib`
über 16 MiB wird als erste Stufe versucht, nicht geklemmt); die HLE beantwortet `_LoadSeg`
(0xA9F0) nach klassischer Semantik (Segmentwort vom Stack, Eintrag über
`PC−6`, Patchen auf `JMP abs.l` zu `Segmentbasis+4+Offset`), `_ExitToShell`,
`_Gestalt`, `_GetZone`/`_SetZone` und `_NewPtr`/`_DisposePtr`, alles andere
beendet den Lauf. Im Gast geprüft: ein
synthetisches MacBinary-Programm bekommt `Gestalt` beantwortet und stoppt bei
`TEInit` (0xA9CC, Exit 2); der LightWave-6.5-Installer lädt 15 Segmente und
wird als VISE-gepackt abgelehnt (`ERR_PACKED`, Exit 4). Mit `mem-kib 4096`
bleiben MemTop (0x400000), Zone (0x200000), Heap-Ende (0x300000) und `NewPtr`
innerhalb des Blocks. Ohne `C:QRXMoreMemory` hat der Gast nur ~19 MB und fällt
auf 8 MiB zurück; mit dem Board-RAM (1/4 Host-RAM, hier 4,0 GB) greift das
Standard-Sizing von 16 MiB, und größere Blöcke sind verifiziert: 64 MiB
(MemTop 0x04000000, Zone 0x02000000) und 1 GiB (MemTop 0x40000000, Zone
0x20000000), beide bis `ExitToShell`. `tools/qrxppc/test_m68k_heap.sh` prüft
MemTop, Zone, `GetZone` und das `NewPtr`-Limit für 16 MiB (Standard), 64, 8
und 4 MiB auf dem Host und dass 2 MiB mit `ERR_MEM` abgelehnt werden.
Die Annahme, die `CODE`-Köpfe seien nur Prologe, ist widerlegt: die
Segmente sind **gepackt**. Bei VISE/InstallerMaker (LightWave 6.5) beginnt
jede `CODE`-Ressource mit `A89F 000C`, CRC, entpackter und gepackter Größe
(16 Byte Kopf, danach gepackte Daten — die Bytes sind kein 68k-Code, auch
nicht ab Offset 4/8/12/16); Aladdins Installer (StuffIt Expander 5.5) beginnt
mit `ADCR`. Der Loader erkennt beides und liefert `ERR_PACKED` samt Format
(`QRX68KRun` Exit 4), statt beim ersten Kopfwort einen fehlenden Trap zu
melden. Nur einzelne Segmente sind ungepackt (StuffIt `CODE` 14 trägt einen
4-Byte-Prolog `00000001`, danach `moveq #0,d3` / `nop` / `tst.l d3`); ein
echter Installer-Lauf braucht also den Packer des jeweiligen Herstellers.

Der klassische `CODE`-0-Einstieg für ungepackte Near-Model-Programme ist
implementiert und auf Host und im ARMithlon-Gast geprüft. Ein synthetisches
MacBinary hat zwei Sprungtabelleneinträge (Einstieg `CODE` 2, Cross-Segment-
Aufruf `CODE` 1), beide Segmente mit 4-Byte-Kopf, in einer unsortierten
Resource-Map. Beim Laden stehen A5 = 0x3000, Sprungtabelle = 0x3020,
Einstieg = 0x3022; `CurrentA5` (0x0904) und `CurJTOffset` (0x0934) sind
gesetzt. Der Lauf patcht beide Einträge auf `JMP abs.l` (Ziele 0x00100024 und
0x00101044), die aufgerufene Routine schreibt 0x11111111 nach 0x3000 und
liefert 0xA5A5A5A5, der Aufrufer prüft den Wert und beendet mit
`ExitToShell`. Im Gast meldet `QRX68KRun` `2 segment(s), A5=0x00003000,
entry=0x00003022`, zwei `_LoadSeg`-Traps und `exited (ExitToShell), 3 trap(s),
3 implemented`; der Exit-Code ist in diesem Fall 0. Der Host-Test prüft
zusätzlich die Ablehnung von Far Model (`0xFFFF`-Marker), ungültigen
Sprungtabellengrößen, abgeschnittenen Tabellen, Einstiegs-Offsets außerhalb
des Segments, fehlenden Einstiegssegmenten und falsch geformten Einträgen
(`QRX_M68K_ERR_IMAGE`); ein VISE-gepacktes Segment wird trotz gültigem
`CODE` 0 im Host-Test und im Gast (`rc=4`, `'CODE' resources are packed`)
abgelehnt. Grenzen: nur Near Model,
kein `_UnloadSeg`/Rückbau, keine Code-Heaps oder Segmentverschiebung; die
Segmentkopf-Zähler werden nicht ausgewertet, gepatcht werden alle ungeladenen
Einträge mit der Segment-ID. Ein vollständig ungepacktes reales 68k-Programm
wurde noch nicht gestartet; der Nachweis ist synthetisch.

`_SysEnvirons` (0xA090) ist implementiert: Version 2 in einen 14-Byte-
SysEnvRec (System 8.1.0, Prozessor 68020, `hasFPU`/`hasColorQD` 1,
machineType 0), Version 0 füllt nichts, Version > 2 ergibt -5503. Das
32-Bit-Adressieren meldet `Gestalt 'addr'` mit Bit 0 genau dann, wenn `MemTop`
über 16 MiB liegt; im Gast bei 16 MiB (0) und 64 MiB (1) geprüft, dazu
`SysEnvirons`-Fehlerwert und alle Record-Felder.

Resource Manager und Handle-Allokatoren sind implementiert: `OpenResFile`
(nur die geladene Applikation, RefNum -1, sonst -43), `CurResFile`,
`UseResFile`, `GetResource` (Typ in D0, ID vom Stack, ein gecachtes Handle pro
Ressource), `ReleaseResource`, `NewHandle`, `DisposeHandle`, `GetHandleSize`,
`SetHandleSize`, `HLock`/`HUnlock`. Der Loader behält eine Kopie des Forks; im
Gast belegt: `GetResource('CODE',1)` → Handle 0x008000C8, Größe 0x00C8,
`NewHandle(0x100)`/`SetHandleSize(0x200)` mit Datenerhalt, `DisposeHandle`
leert den Master-Pointer (neun Traps, alle beantwortet, Exit 0).

Der File Manager läuft über eine Callback-Schicht (`qrx_m68k_set_fs()` mit
`open`/`read`/`write`/`seek`/`close`): im Gast `dos.library` in einem
`[host-root]`-Verzeichnis, im Host-Test stdio. Implementiert sind `_Open`,
`_Create`, `_Close`, `_Read`, `_Write`, `_GetFPos`, `_SetFPos`, `_GetEOF` über
`ParamBlockRec`, mit RefNums 1..8, `ioResult`/`ioActCount`/`ioEOF`,
`fnfErr`/`ioErr`/`paramErr`/`tooManyFiles` und `..`-Schutz. Im Gast belegt:
zwölf Traps, alle beantwortet, danach liegt `FSRoot/out.txt` mit `Hallo\n` auf
dem Share. Offen: `Rename`, `Delete`, Verzeichnisse, Volume-/Finder-Infos.

QuickDraw, Dialog und Event laufen über dieselbe Bauform: `qrx_m68k_set_gfx()`
installiert `rect`/`line`/`text`, die Traps übersetzen Punkte, Rects und
Zeiger. Implementiert sind Ports (`_InitGraf`, `_InitPort`, `_OpenPort`,
`_SetPort`, `_GetPort`, `_ClosePort`), Stift (`_MoveTo`, `_LineTo`, `_Line`,
`_PenSize`, `_PenMode`, `_PenPat`, `_PenNormal`, `_HidePen`, `_ShowPen`,
`_GetPen`, `_SetPt`), die fünf Rect-Operationen plus `_EqualRect`,
`_OffsetRect`, `_InsetRect`, `_PtInRect`, Text (`_DrawChar`, `_DrawString`,
`_DrawText`, `_TextFont`, `_TextFace`, `_TextMode`, `_TextSize`), Cursor,
`_SysBeep`, die Dialog-Traps (`_GetNewDialog` liest 'DLOG'/'DITL' und macht aus
den Einträgen 4..7 Controls, `_DrawDialog` zeichnet Rahmen und Texte,
`_ModalDialog` meldet den getroffenen Knopf, `_Alert` antwortet mit Item 1)
und der Event Manager mit genau einer Event-Zelle aus `_PostEvent`. Im Gast
belegt: `TestQD.mb` fährt 31 Traps, alle beantwortet, das Log zeigt elf
Zeichnungen (`qd line 20,10 -> 20,50`, `qd rect fill 10,20,30,40`,
`qd text 12,10 "Ready"` …), der Dialog kommt aus den Ressourcen, sein Knopf ist
ein Control (`qd rect frame 30,10,45,60`, gezeichnet über `_Draw1Control`), und
`_ModalDialog` meldet den Klick darauf. Offen: Fensterverwaltung, Menüs, echte
Eingabe (Maus/Tastatur), `CopyBits`/Regions/Pictures/Farben,
Schrift-Ressourcen, ein blockierender modaler Dialog und eigene Control-Arten
außer dem Knopf.

Fenster und Menüs laufen über zwei weitere Hooks derselben Schicht (`window`
mit Operation, Frame und Titel, `menu` mit Operation, Menütitel, Item und
Wert). Implementiert sind `_InitWindows`, `_GetWMgrPort`, `_NewWindow`,
`_DisposeWindow`, `_CloseWindow`, `_ShowWindow`, `_HideWindow`,
`_SelectWindow`, `_BringToFront`, `_SendBehind`, `_GetWRefCon`,
`_SetWRefCon`, `_GetWTitle`, `_SetWTitle`, `_MoveWindow`, `_SizeWindow`,
`_HiliteWindow`, `_BeginUpdate`, `_EndUpdate`, `_InvalRect`, `_FrontWindow`,
`_FindWindow` sowie `_InitMenus`, `_NewMenu`, `_DisposeMenu`, `_AppendMenu`,
`_InsertMenu`, `_DeleteMenu`, `_ClearMenuBar`, `_DrawMenuBar`, `_EnableItem`,
`_DisableItem`, `_CheckItem`, `_GetMenuItemText`, `_SetMenuItemText`,
`_CountMItems`, `_DeleteMenuItem`. Fenster-Records (64 Bytes, acht Fenster)
und Menü-Handles (16-Byte-Block, acht Menüs, 24 Items) liegen im Gastblock,
die Leiste hält acht Menüs. `_DisposeWindow`/`_CloseWindow` geben den
Fenster-Record samt seinen Controls an den Heap zurück: `trap 0xA914 …
a0=0x00800000 ok`, danach `trap 0xA913 … -> 0x00800000 ok` (dieselbe Adresse)
und `trap 0xA92D … a0=0x00800000 -> 0x00000000 ok`; im Host-Test sind danach
keine Fenster-Slots mehr belegt, das Front-Fenster ist 0, und in der
Freiliste liegt genau der Record (`free blocks after CloseWindow`). Sie hängen nicht mehr nur am Log: `QRX68KRun` öffnet für jedes Mac-Fenster ein
echtes Intuition-Fenster auf dem öffentlichen Schirm, QuickDraw zeichnet in
dessen RastPort, die IDCMP-Ports werden in die Event-Queue gepumpt (Maus,
Tasten, Aktivierung, Schließen-Gadget, Refresh), und der Mac-Scrap liegt über
`clipboard.device` in derselben Zwischenablage wie die AROS-Anwendungen —
Ausschneiden im Mac-Programm, Einfügen im AROS-Editor und umgekehrt. Die
Menüleiste ist ein echter Menü-Strip: NewMenu/AppendMenu/InsertMenu/DeleteMenu
füllen ein Modell, `CreateMenus`/`LayoutMenus`/`SetMenuStrip` bauen und hängen
ihn an jedes Fenster, und eine Auswahl kommt als `menuEvt` (15, Menü-Kennung
und Item in der Message) plus `mouseDown`/`mouseUp` mit der Menü-Kennung beim
Programm an; ein Fenster, das nach `DrawMenuBar` entsteht, bekommt den
bestehenden Strip beim Anlegen. Häkchen, Freigabe und Itemtext ändern nur den
vorhandenen Eintrag: `_CheckItem` setzt `CHECKED` in der Häkchenspalte
(`CHECKIT`), `_DisableItem` nimmt `ITEMENABLED` weg, `_SetMenuItemText` tauscht
das `IntuiText` — danach `ResetMenuStrip`, ohne `CreateMenus`/`FreeMenus`.
Auswerten lässt sich die Wahl wie unter MacOS: `_MenuSelect` gibt die gemeldete
Menü-Kennung im oberen und die Item-Nummer im unteren Wort eines Longs zurück
(`trap 0xA93D … -> 0x00800001`). Command-Zeichen kommen über `_SetItemCmd`
(0xA84F) ins Modell und als `item-cmd` in den Strip (`aros: item 1 "Quit"
command key 'Q' (patched)`) und lassen sich mit `_GetItemCmd` (0xA84E) wieder
auslesen — im Gast belegt, indem das gelesene `Q` auf ein zweites Item
geschrieben wird; `_MenuKey` bildet das Zeichen unabhängig von der Schreibweise
auf das erste freigegebene Item ab (`trap 0xA93E … d0=0x00000071 ->
0x00800002`, gesperrtes Item → 0). Dasselbe Muster trägt das Aussehen:
`_SetItmStyle` (0xA942) stellt dem `IntuiText` einen gestylten `TextAttr` hin
(`aros: item 1 "Quit" style 1 (patched, font style 0x2)`), `_SetItemMark`
(0xA944) belegt die Merkzeichenspalte mit dem Haken (`aros: item 1 "Quit" mark
'X' (patched as check)`), `_SetItmIcon` (0xA940) zeichnet Icon und Text in
eine Bitmap und hängt die als `Image` in `ItemFill` mit gelöschtem `ITEMTEXT`
(`aros: item 0 "Oeffnen" icon 3 (digit plus text in a 68x8 image, 118 text
pixels, ITEMTEXT 0)`) — Intuition erlaubt nur eine Füllung je Item und keine
gemischte Kette, ein zweites Item käme in eine eigene Zeile; Textwechsel
zeichnet die Bitmap neu (`aros: item 0 text "Neu" redrawn with icon 3`), echte
Mac-Icongrafik gibt es nicht. Die Bitmaps sind Eigentum des Gasts und werden
vor jedem `FreeMenus` sowie beim `_DisposeMenu` freigegeben (`aros: menu "Datei"
disposed, 1 icon bitmap(s) freed`) — vorher blieb der Speicher dort liegen.
`_DeleteMenuItem` gibt die Bitmap des entfernten Items selbst frei (`aros: item
1 deleted, icon bitmap freed`); das war der zweite Leck dieses Pfads, denn das
Leeren des Modellplatzes überschrieb den Zeiger nur. Der Titel bleibt wie unter MacOS hervorgehoben, bis
`_HiliteMenu(0)` ihn wieder aufhebt — der Ablauf `MenuSelect` → Befehl →
`HiliteMenu(0)` läuft damit durch, die Hervorhebung lebt aber nur im Modell
(`aros: menu 128 hilited ("Datei")` / `aros: menu highlight cleared`), weil
Intuition einen Titel nur während des Trackings markiert. Im Gast belegt:
`TestWin.mb` fährt 64 implementierte Traps, alle beantwortet; das Log zeigt `aros: window "Test" on the public screen`,
`aros: menu bar 1 menu(s), 2 item(s) attached`, `win inval 12,14,30,40`,
`PutScrap`/`GetScrap` mit `Hallo`, zwei Ereignisse aus dem Pump (`mouseDown`,
`keyDown`), für das späte Fenster `Zweite` `aros: menu strip attached to
"Zweite"`, die gepatchten Items (`aros: item 0 "Open" checked 1 enabled 0
(patched, check column)`, `aros: item 0 text "Oeffnen" (patched)`) und mit dem
Schalter `C:QRX68K_MENU_TEST` den Menü-Pfad `aros: menu pick "Datei" item 1
(menu 128, event 15)` / `1 menu pick(s)` und dieselbe Auswahl über `_MenuSelect`
(`0x00800001`, ohne den Schalter `0`) samt `_HiliteMenu(0)`, am Ende
`2 window(s) created, 0 left open`.
Der Control Manager läuft mit einem eigenen Kern: `_NewControl` (0xA954) legt
einen Record mit Eigentümer, Bounds, Titel, Wert und Grenzen an und liefert
einen Handle, `_ShowControl`/`_HideControl`/`_MoveControl`/`_SizeControl`/
`_HiliteControl` (0xA957..0xA95D) ändern ihn, `_GetControlValue`/`_SetControlValue`
und Minimum/Maximum (0xA960..0xA963) sowie `_GetControlTitle`/`_SetControlTitle`
(0xA95E/0xA95F) lesen und schreiben die Felder, `_DrawControls`/`_Draw1Control`
(0xA969/0xA96D) zeichnen Rahmen und Titel über die gfx-Hooks, und
`_FindControl`/`_TrackControl` (0xA96C/0xA968) treffen den Knopf mit
`inButton` (10). Die 'DITL'-Einträge eines Dialogs sind seit dem nächsten
Schritt selbst Controls: `_GetNewDialog` legt für jeden Eintrag der Typen 4..7
einen Record mit den Bounds und dem Text des Eintrags an, `item` merkt sich die
Item-Nummer, und `_ModalDialog` nimmt einen `mouseDown` im Knopf, meldet seine
Nummer als `itemHit` und konsumiert das Ereignis — ohne Klick bleibt Item 1.
`_IsDialogEvent` bejaht so einen Klick (und `keyDown` bei vorhandenem Control),
`_DialogSelect` schreibt die Nummer und liefert 1. Im Gast belegt (`TestQD.mb`,
31 implementierte Traps): `trap 0xA97C … -> 0x00800000 ok` baut den Dialog,
`trap 0xA991 … -> 0x00000000 ok` meldet ohne Klick Item 1, `trap 0xA97F … ->
0x00000001 ok` erkennt den Klick als Dialogereignis, `trap 0xA96C … -> 0x0000000A
ok` findet den Knopf mit `inButton`, `trap 0xA96D … -> 0x00000000 ok` zeichnet
ihn (`qd rect frame 30,10,45,60`, `qd text 12,30 "Go"`), und der zweite
`ModalDialog` meldet Item 2 (im Host-Test geprüft, das Gastlog zeigt die
Trap-Antwort). Grenzen: kein `filterProc`, kein blockierendes Tracking, keine
Command-Tasten-Äquivalente, keine Hervorhebung während des Klicks, Alerts ohne
Items. `_DisposeDialog` (0xA983) und `_CloseDialog` (0xA982) schließen den
Kreis: sie geben die Controls des Dialogs (wie `_KillControls`), die
Item-Tabelle (Größe aus `count * 12 + Summe(len+1)`, weil der Record sie nicht
speichert) und den 64-Byte-Record an den Heap zurück und tragen den Dialog aus
`dialog_rec` aus. Im Gast belegt: `trap 0xA983 … a0=0x00800000 ok`, danach
`trap 0xA97C … -> 0x00800000 ok` (derselbe Record) und `trap 0xA982 …
a0=0x00800000 -> 0x00000000 ok`; im Host-Test liegt die Item-Tabelle wieder auf
`Record+64`, das Control auf seiner alten Adresse, `nheap_free` ist 0, der
Bump-Zeiger steht wieder auf dem Record und kein Dialog bleibt offen.
`_DisposeControl` und `_KillControls` geben den Record an die
Heap-Verwaltung zurück: Blöcke aus `NewPtr` tragen ihre Größe jetzt in einer
Host-Tabelle, angrenzende freie Blöcke verschmelzen, und ein Block am oberen
Rand zieht den Bump-Zeiger wieder herunter — der Zyklus „anlegen, benutzen,
freigeben“ braucht so keinen neuen Speicher (dieselbe Leck-Klasse wie bei den
Menü-Icons, nur diesmal gleich mit Freigabe). Im Gast belegt (dasselbe
`TestWin.mb`, jetzt 64 implementierte
Traps): `trap 0xA954 … -> 0x008010A0 ok` legt den Knopf an, `trap 0xA969 … ok`
zeichnet ihn (`qd rect frame 100,20,120,60`, `qd text 22,100 "OK"`), `trap
0xA960 … -> 0x00000001 ok`, `trap 0xA963 … ok` setzt 2, `trap 0xA95E … ok` holt
`OK` zurück, `trap 0xA96C … a0=0x001004D8 -> 0x0000000A ok` findet und `trap
0xA968 … -> 0x0000000A ok` trackt ihn. Grenzen: nur Knöpfe — keine
CDEF-Prozeduren, keine Scrollbar/Checkbox/Radiogruppe, kein blockierendes
`_TrackControl` mit Action-Proc, und die 'DITL'-Einträge eines Dialogs werden
noch nicht zu Controls (`_ModalDialog` meldet weiter Item 1). Freigegeben werden
Control-Records, Dialog-Record samt Item-Tabelle und Fenster-Record;
`_DisposePtr` bleibt ohne Wirkung (der Trap nennt keine Größe), und
`_CloseDialog`/`_CloseWindow` sind hier endgültig statt nur verbergend. Im Host-Test geprüft: nach
`_DisposeControl` liefert `_NewControl` dieselbe Adresse und der Bump-Zeiger
steht wieder auf ihr (`heap top after DisposeControl`), `_KillControls` räumt
zwei Controls ab (`nheap_free == 0`), und die dritte Anlage landet wieder auf
der ersten Adresse.
Offen: WDEFs (Zoomen, Scrollen, Größenziehen), weitere Control-Arten (Checkbox
und Radio werden angelegt, aber nicht als eigener Zustand gezeichnet),
Untermenüs,
Command-Zeichen aus 'MENU'-Ressourcen (nur `_SetItemCmd` setzt sie), echte
Mac-Icongrafik (nur eine Ziffer vor dem Text, im Default-Font gezeichnet) und beliebige
Merkzeichen in der Leiste (der Strip kennt nur Haken), die Stil-Bits
Outline/Shadow/Condense/Extend, das Menü-Tracking in `_MenuSelect`
(die Auswahl kommt fertig vom Gast, der Aufruf blockiert nicht) und eine
sichtbare Titelhervorhebung (`_HiliteMenu` pflegt nur den Zustand),
das Neuvermessen geänderter Itemtexte, weitere
Scrap-Typen als `'TEXT'` und die Mac-Zeichensatz-Umwandlung.

Acrobat Reader 4.05a aus dem bereitgestellten `reader_installer_4.05a.sit`
(SHA-256 `07cf9f61d03986f5a5caa026004da4d063c2c97b6e2c176eb3d6cf945eac940c`)
wurde schreibgeschützt untersucht. Der Installer (`APPL`/`VIS3`) hat einen
`SVCT`-Datenfork und 26 `CODE`-Ressourcen: `CODE` 0 enthält eine Sprungtabelle,
deren erster Eintrag auf ID 1002 verweist; 1000 und 1002 tragen keinen
bekannten Packkopf, 23 andere positive IDs dagegen den VISE-Kopf. Ein `cfrg`
wurde nicht gefunden. Die neuen Angaben zu Anzahl und niedrigster erkannter
gepackter ID im 68k-Runner helfen, diesen Befund einzuordnen; sie stellen
keinen Programmeinstieg fest und prüfen höchstens 64 `CODE`-Ressourcen.

Im isolierten ARMithlon-Gast mit `QRXMoreMemory` meldete `QRX68KRun`:
`VISE/InstallerMaker-packed 'CODE' resource (lowest CODE 1, 23 packed
segment(s))`; `QRXAppRun` meldete `68k application: 'CODE' resources, no
'cfrg'` und `68k application (no PEF fragment)`. Beide Runner lehnten die App
vor Ausführung ab; der Gastlauf erreichte seinen Endmarker ohne gemeldeten
CPU-Fault. Der Host-Test prüft VISE, ADC und die Zählung bei unsortierten
Ressourcen. Das Originalarchiv und die produktive `Startup-Sequence` blieben
bytegleich; temporäre Testkopien und Logs werden entfernt.

Nächste Schritte bleiben: (1) einen vollständig ungepackten realen
68k-Test-App finden oder einen überprüften VISE-/ADC-Decoder implementieren;
(2) `SVCT` erschließen, bevor Aussagen über darin enthaltene PPC-Programme
möglich sind. Dialog- und Installer-Kompatibilität
ist weiterhin unvollständig; der Acrobat-Installer startet nicht.

PPC-Stand gleichzeitig: die Gast-Suite `C:QRXPPC` läuft vollständig durch
(`QRXPPC: 0 failure(s)`), inklusive `pef_loader`, `pef_reloc`, `apprun`,
Toolbox (MM/RM/FM/QD/WM/Menü/Text/Event), JIT und FPU.
