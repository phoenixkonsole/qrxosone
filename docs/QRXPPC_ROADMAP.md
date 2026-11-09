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
`lwz`/`stw`, `lbz`/`lhz`/`lha`, `stb`/`sth`, direkte
`b`/`bl`/`ba`/`bla`-Blockenden und `sc`. Die Speicherzugriffe laufen über
geprüfte C-Helfer, damit Alignment-/Bereichsfehler, `fault_addr` und
Instruktionszählung mit dem Interpreter übereinstimmen. Ein 24-Block-Cache
vergleicht die exakten Quellbytes bei jedem Einstieg; Codeänderungen zwischen
Blockläufen führen zur Neuübersetzung. `CacheClearE`/`CacheClearU` werden
nach jedem neuen Block ausgeführt.

CR0 wird lazy geführt: Der generierte Code hält den Nibble in 68k-D2 und
committet ihn erst an Blockgrenzen oder vor potenziell faultenden
Speicherzugriffen nach `cpu.cr`. Mehrere CR0-Schreiber im Block erzeugen nur
den letzten Wert; der D2-Save/Restore im Prolog/Epilog hält die C-ABI ein.

Abnahme im ARMithlon-Gast: 26 Fälle, davon 19 bestehende Integer-/FPU-Fälle,
der CR0-Test (`jit_cr0`), der deterministische Fuzz-Test (`jit_fuzz`, 64
Fälle × 16 zufällige JIT-Instruktionen), der Fault-Fuzz (`jit_fault_fuzz`,
96 Fälle mit zufälligen 32-Bit-Adressen, RAM-Grenz- und Wrap-Adressen sowie
CR0-Commit vor dem Fault), der Breiten-/Grenztest (`jit_widths`), zwei
gezielte Fault-Differentialfälle (OOB und unaligned) und der
SMC-/Cache-Test (`jit_cache_smc`); `QRXPPC: 0 failure(s)`. Die bestehenden
Fälle laufen zusätzlich per JIT auf einer separaten Speicherkopie und
vergleichen den vollständigen CPU-/Speicherzustand.

Offen für den vollständigen P1.4-Ausbau: übrige CR-Felder und
Compare-Instruktionen, bedingte Branches/Block-Chaining/indirekte
Branch-Lookups, ISA-Tag und Seitengeneration, Mid-Block-SMC und
Alias-Invalidierung, FPU-JIT und Trace-/Performance-Ausbau. Tier 1 ist ein belastbarer Anfang, keine
vollständige P1.4-Abnahme.

#### P1.5 ABI und Mixed Mode

- PPC-Calling-Convention.
- Übergang 68k → PPC und PPC → 68k.
- Stackframes, Register-Rettung, Fehler- und Trap-Rückkehr.
- Synthetische Mixed-Mode-Tests.

Abnahme P1:

- Ein PPC-Testprogramm läuft interpretiert und per JIT mit identischem
  Endzustand.
- Randomisierte Instruktionstests und Fault-Tests bestehen.
- Codeänderungen invalidieren den Cache nachweislich.
- Es gibt noch keine Aussage wie „MacOS läuft“.

Stand 2026-11-09: Der Tier-1-Satz besteht differentiell, inklusive Fault-
und SMC-Test. Randomisierte JIT-Instruktionstests, bedingte Branches,
Mid-Block-SMC und der volle P1.4-Ausbau bleiben offen.

### P2 — MacOS 9 HLE

Ziel: Eine kontrollierte klassische MacOS-9-App startet ohne vollständige
MacOS-Installation im Gast.

#### P2.1 Loader und Laufzeit

- PEF/Code Fragment Manager, MacBinary und Resource-Forks.
- 68k-/PPC-Codefragmente und Mixed-Mode-Übergänge.
- Speicherverwaltung, Handles, Zeigervalidierung.
- Fehlerbehandlung statt Hänger.

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
8. 68k-JIT-Tier-1 mit Code-Cache und SMC-Invalidierung (Tier 1 mit Lazy-CR0
   umgesetzt; übrige CR-Felder, bedingte Branches, Seitengenerationen und
   FPU-JIT offen).
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
