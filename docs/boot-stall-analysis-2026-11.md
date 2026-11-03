# QRX68100_full — Boot-Stall-Analyse (Nov 2026)

Arbeitskopie: `/Users/pascalpapara/Downloads/qrxosone` (kein git). Engine:
`upstream/armithlon/2.6/engine/full68k/`. Build:

```bash
cd upstream/armithlon/2.6/build-qrx68100-full
cmake --build . --target qrx68100_full_run -j
```

Boot-Kommando siehe "Reproduktion" unten.

## 1. Bild des Stalls

- CPU hängt im Supervisor-Idle-Loop `PC=00FE6866` (`stop #$2000`-Follow-up,
  `jsr (a4=$FE7F68)` Task-Processor, `Connected: Ready-List leer`).
- ExecBase `$C0059C`, ThisTask `$C30F48` (`input.device`), Ready-Liste leer,
  `[FL] DMA start` 0 Events (kein Level-1 DSKBLK, kein DMACON-Arm).
- Wartende Tasks (alle TS_WAIT, in `TaskWait` verkettet):
  `input.device($C30F48) → console.device($C7CA08) → trackdisk.device($C2C6B8)
  → Intuition menu handler($C588F0) → «Screennotify Handler»($C5CE00)
  → Exec Bootstrap Task($C123F8) → $C00744 (SysBase)`.
- PNG `stuck_kick` (trackdisk.task MicroHZ-Req `$C30B48`/Port `$C30AF8`) läuft
  und wird pro Frame erneut abgeschlossen — Boot kommt trotzdem nicht weiter.
- `$C38D20 st=5` war ein Auswertungsfehler (keine gültige TCB).

## 2. Entscheidende Messung: die Wartemaske des Boot-Tasks

Trace `QRX_FULL_TRACE_RAMW=c1240e` (= `$C123F8.tc_SigWait`, exakt ein Event):

```
[RAMW] W32 00c1240e=00008000 PC=00f8a37c instr=12710276
```

Dann nie wieder geschrieben; `QRX_FULL_TRACE_RAMW=c12412` (`tc_SigRecvd`) →
**0 Writes**: der Boot-Task wird nie signalisiert.

Endzustand am Stall (RAM-Dump `QRX_FULL_DUMP_RAM=c00000:180000:...`, korrekte
Offsets — Achtung: Dump-Bytes ab `$C00000`, nicht VM-Offset 0!):

- `$C123F8` = **Exec Bootstrap Task** (Name-PTR `$00F826CE` im main ROM),
  `tc_SPLower=0 / tc_SPUpper=$FFFFFFFF` — exakt wie in
  `rom/exec/exec_init.c` angelegt (`t->tc_SPLower = NULL; tc_SPUpper = ~0;`).
- `st=04` (TS_WAIT), geparkt mit `tc_SPReg=$C122BC`. SP-Stack (Return-Adressen):
  `$FE7970`(Leaf, `Supervisor`-Stub `$FE795C`: `jsr %fp@(-30)`, LVO 5)
  → `$F8EAE0` → `$F8A39E` (`jsr %fp@(-318)`, LVO 53 `Wait(1<<SigBit)` =
  WaitPort-Pattern; bei `$F8A37C` wird `tc_SigWait=0x8000` geschrieben)
  → `$F8A3FE` → `$F8866A` (`AddDevice`, LVO 79, pri-sortierte Insert in
  `$F88662..$F88690`) → ext-ROM `$E62006 / $E62AA0 / $E6298C`
  (DOS-/Resource-Init-Raum, Strings: `ciaa.resource`, `ciab.resource`,
  `input.device`, `timer.device`, `utility.library`, `locale.library`,
  `*.language` bei Dateioffsets `$0x61E00..`).
- An der Stall-Lage existierten **keine** Message-Nodes (NT_MESSAGE=5 oder
  NT_REPLYMSG=7) in RAM, deren `mn_ReplyPort.mp_SigTask == $C123F8` — das
  Warten ist also kein (mehr) verfolgbarer Port-Reply (in-flight).

## 3. Quelltext-Bestätigungen (AROS master 5b5fd4c, lokal im Tarball)

- `rom/timer/lowlevel.c` — `ciaint_timer` (CIA-A TimerA-Underflow): Wake-Pfad
  des MICROHZ-, `ciab_eclock` (Timer B/CIA-A-B) Reads `tb_eclock_lo/hi` bei
  `SetICR(res,0x80|bit)` („force interrupt now" = `CheckTimer`).
  `*LIBBASE->tb_micro_cr = 0x08;` in `arch/m68k-amiga/timer/timer_init.c`
  entspricht dem beobachteten `[CRA] w=08 PC=$00FAB12C` (kein Bug: Timer A
  steht zwischen Requests still).
- `arch/m68k-amiga/timer/timer_init.c` — Timer-B = **E-Clock** (`$BFE601/
  $BFE701`, `$BFEF01`), Timer-A = **MicroHZ** ($BFE401/$BFE501/$BFEE01).
- `arch/m68k-amiga/devs/trackdisk/trackdisk_hw.c` — `td_readwritetrack()`:
  `dsklen`-Double-Write (INIT-Bit `$8000`) — exakt die `[FL] DMA start`-
  Bedingung in `amiga_trap_write($DFF024)`.
- `rom/devs/input/processevents.c` — `input.device` bewaffnet **UNIT_VBLANK**
  (100 ms) und wartet auf `commandsig | kbdsig| gpdsig | timersig | keytimersig
  | ResetSig` — Warten dort ist normal, kein Boot-Killer.
- LVO-Nummern gegen `rom/exec/*.c` gecheckt: `Supervisor`=5 (-30), `WaitIcon`=53
  (-318), `AddTail`=45 (-270), `AddHead`=44 (-252), `OpenDevice`=49 (-294),
  `AddDevice`=79 (-474). -474/$F88656 und -294/$F8869C.

## 4. Implementierte Fixes (Stand 2026-11-02)

1. **CIA-A Timer B** (`qrx68100_full.c/h`): TBLO/TBHI/CRB-Writes/Reads
   ($BFE601/$BFE701 + flat $BFE009/$BFE00B), Auto-Start bei TBHI-Reload,
   `cia_b_tick()` (ICR-Bit 1 → Level-2 → `paula_recompute`). Verifiziert im
   100M-Lauf: `[TBHI] w=ff latch=ffff auto-start`, `[CRB] w=11`,
   6 TimerB-Underflows, Level-2-INTACKs 3→9.
2. **Level-2 INTACK-Fix** (bestehend): Loop 519944→3 acks, VBLANK läuft
   (173 acks/Run).
3. **boot_reply_wake()** (neu, in `stuck_kick` vor dem ThisTask-Name-Gate):
   - Identifikation des Boot-Tasks: NT_TASK(1) + TS_WAIT(4) +
     `SPLower=0 / SPUpper=$FFFFFFFF` + `SigRecvd&SigWait==0` + Ready leer.
   - Pfad a: bereits geantwortete Message (`NT_REPLYMSG`) auf einem Port im
     Eigentum des Task → Signal(sigwait) + TS_READY + Ready-Enqueue
     (+QuantumOver `SysFlags |= 0x20`).
   - Pfad b: bewaffneter `timer.device` `TR_ADDREQUEST` im Eigentum des
     Boot-Task → Kanal wie stuck_kick: unlink, NT_REPLYMSG, io_Error=0,
     Port-Queue, Signal, Ready-Enqueue.
   - Fallback (fuer den beobachteten Fall — kein verfolgbarer Reply): direkt
     `Signal(sigwait)` ausliefern. Log: `[KICK] boot wake done src=signal`.
   - Trace-Env: `QRX_FULL_BOOTKICK_TRACE=1` (Port-/Message-Enumeration,
     Fallback-Erkennung), Haupt-Env wie bisher `QRX_FULL_STUCKKICK=0`.
   - Einmal ausgelöst: instructions im final stall 14,205,166 → 14,166,253
     (System läuft einen Schritt weiter), danach re-parkt (neuer Stall,
     jetzt mit leerem `tc_SigWait=0`, SP unverändert `$C122BC` — d. h. die
     nächste Hürde liegt zwischen `$F8A37C`-Wait und dem mittlerweile
     erledigten ersten Reply).
4. Experiment „wake input.device" wurde getestet (wirkte nicht progressiv)
   und wieder entfernt. `stuck_kick`-Names-Check bleibt
   `trackdisk.device`/`bootstrap`.

## 5. Reproduktion

```bash
cd upstream/armithlon/2.6/build-qrx68100-full
R=/Users/pascalpapara/Downloads/qrxosone/upstream/aros-deadwood/m68k-build-5b5fd4c
# normaler Lauf mit Kick + Trace
QRX_FULL_BOOTKICK_TRACE=1 QRX_FULL_ADF=$R/bootdisk-amiga-m68k-3520.adf \
  ./qrx68100_full_run --cpu 68040 --rom $R/aros-amiga-m68k-rom.bin \
  --ext-rom $R/aros-amiga-m68k-ext.bin --cycles 150000000 2>&1 | \
  grep -E 'BOOTKICK|KICK\] boot|^\[FL\]|result='
# Stall-Punkt RAM-Dump (Achtung: VA c00000.., = slow RAM, n<=c.vm.ram.size)
QRX_FULL_DUMP_RAM=c00000:180000:/tmp/ram_now.bin ... --cycles 150000000
```

Dump-Handling (python): Base `$C00000`, `data[addr-$C00000]`; `r32`
muss auf gdb-offsets laufen, *nicht* auf Gastadressen.

## 6. Offene Punkte (Reihenfolge des nächsten Arbeitsschritts)

1. **Zweiter Hänger nach dem ersten Boot-Wake**: Boot-Task erneut TS_WAIT,
   `SigWait=0` (wie `Wait(0)`-Signatur), SP unverändert. Verdacht: die
   nächste Warte-Gate ist dieselbe `$F8A37C`-Site, aber mit
   **signalSet=0** — d. h. ein `GetMsg`-/`CheckIO`-loop, der ohne Message-
   Fund zurückgibt? oder ein zweiter `Wait(0x8000`-Aufruf, dessen Reaction-
   Puffer nicht verfolgt wird, weil `mp_SigTask` im RAM von einem Aufräumer
   ausgelöscht wurde.
2. **`Supervisor`-Leaf `$FE795C`** (LVO 5, `jsr %fp@(-30)`) — die Funktion,
   die `a5 = ExecBase-0x36` übergibt, klären (AROS-stubs?!). Das 0x36-Offset
   (54) würde in den *User-Signals-Teil* von ExecBase zeigen — relevant,
   wenn der leaf die `$C122BC`-Stack-Content-Read.utf —
3. `Ext-ROM` `$E61E00..$E62070` (Res-Tabelle `ciaa.resource` etc.) lesen,
   der ext-Init-Flow bei `$E62006/$E62AA0/$E6298C` direkt greppen.
4. Falls ein zweiter Kick nötig wird: `stuck_kick`-/`boot_reply_wake`-Logik
   so erweitern, dass *mehrere* Wake-Muster (z. B. nach SigRecvd-clear)
   erlaubt sind (derzeit bricht der Scan bei `SigRecvd&SigWait!=0` ab).

Logs chronologisch: `/tmp/t_flV.log` → `/tmp/t_flAK.log` (z. B. die
letzten: t_flAK = erster erfolgreichtes boot wake; t_flAL = 400M-Run,
1 Wake, PHIST `0fe68xx:1680`).

## 7. Fortsetzung (Session 2): Quelltext-Verifikation des Boot-Pfad

### 7.1 Boot-Task = `dosboot_BootBlock()`-Loop (Quellcode)

`rom/dosboot/bootstrap.c` des bootenden AROS-9398 (`5b5fd4c`): Der
„Exec Bootstrap Task“ wird in `rom/exec/exec_init.c` (line 184–210)
gebaut — exakt die Signatur, die unser `boot_reply_wake()` checkt
(`tc_SPLower=NULL, tc_SPUpper=~0, tc_SigAlloc=0xFFFF, Name-PTR`) —
und läuft dann den Boot-Flow:

1. `CreateMsgPort()` (bootstrap.c:319) → mp_SigTask = Boot-TCB;
2. `CreateIORequest(msgport, sizeof(IOStdReq))` → io_Message.mn_ReplyPort
   = der Port aus (1);
3. `dosboot_DevicePresent()` (bootstrap.c:157–178):
   `OpenDevice(bootDevName, unit, io, 0)` →
   `io->io_Command = TD_CHANGESTATE` → **`DoIO(io)`**;
4. `dosboot_BootBlock()` (bootstrap.c:184+):
   `io_Command=CMD_READ, io_Length=512` → **`DoIO(io)`** auf dem
   Boot-Block; danach `TD_MOTOR`-`DoIO`.

**Achtung:** `tc_SigAlloc=0xFFFF` im Boot-TCB bedeutet: `AllocSignal()`
findet *kein* freies Bit — `CreateMsgPort()` schlägt in *dieser* Boot-
TCB-Konfiguration fehl, wenn kein anderes Signal-Clearage dazwischen-
kam. Die Slot-Basis für `mp_SigTask=$C123F8` (`SigWait=0x8000` →
1<<15) muss daher die **Device-TCB** verwenden, nicht den Boot.

### 7.2 Disassembled Wait/Signal-Kode aus dem ROM (Insight)

$F8A124ff = **`Signal(task, signals)`-Brief** (aus dem ROM-Disasm):

```m68k
f8a132: jsr %fp@(-120)     ; Disable
f8a12e: moveal %fp@(276),%a4                 ; a4 = ExplicitSignal?
f8a138: orl %a2@(26),%d0   ; d0 = signalSet | tc_SigRecvd
f8a13c: movel %d0,%a2@(26) ; tc_SigRecvd = d0
f8a166: orl %a2@(22),%d1   ; d1 |= tc_SigWait
f8a16a: andl %d1,%d0
f8a16e: cmpib #4,%a2@(15)  ; == TS_WAIT?
        ... RemTask(waitlist); tc_State=3 (TS_READY); AddTail(TaskReady)
```

$F8A1B4ff = **`AllocSignal`/`BitSet`-Funktion** (`fp@+550/546` =
`SigAlloc`-Pointer, die Loop zählt Kostenbits ab LSB).

ROM-Modell: exec.library-1 ist implementiert mit *zwei* Signalcheidencodes
(`tc_SigRecvd`-Bits und e-`ExplicitSignal`, Siehe auch mount-semaphores
in `rom/dosboot/` — das ist der untersuchte AROS-Stand, nicht der
m68k-Classic-ABI).

### 7.3 Konsequenz für den nächsten Fix-Trigger

Nach dem ersten Wake re-parkt der Boot-Task in einem Wait mit
`tc_SigWait=0` (= Signatur **„Wait(TDNestCnt.PATH)“-late-Fail-check** oder
ein `DoIO` der den nósig-Wait-Path nimmt). Nach der 7.2-Interpretation
gibt es zwei konsistente Erklärungen:

- (A) Der Boot-Task wartet *schon* auf ein `TD_CHANGESTATE`-`DoIO`:
  der Trackdisk-Task hat die Message noch nicht verarbeitet, weil er
  selbst in seinem MicroHZ-Timer-Wait steckt (31x-Spin, docs §1) —
  d. h. **die eigentliche Blockier-Quelle** ist der Timer-Kanal,
  nicht der Boot.
- (B) Der Boot-Task hat die Signal bereits bekommen, aber `Wait()`
  re-entered die loop mit `signalSet=0` — ein Bug-Stub in der
  m68k-ROM-Compilierung.

Test, um (A) und (B) zu trennen: Trace `QRX_FULL_TRACE_RAMW=c30b48`
und das `io_Error`-Byte des Req `$C30B48` verfolgen: steigt der Wert,
signaliert der Timer-Task? bzw. `io_Timer`-Addr `$C30B48+0x28`/
`io io ` ... (io_Length/cpp field) — und Dasselbe für den DoIO-Kandidat
`$C95B38` (Msg-Nodes aus dem SP-stack dump: SP+18=$C95A44, SP+1C=$C95B38).

## 8. Session 2 — Ergebnis und verworfene Fallback-Strategie

### 8.1 Was der erste Wake tat (Fehlbewertung und Rücknahme)

Der erste Wake (`src=signal`, Fallback: `SigRecvd|=sigwait` ohne echten
Reply) feuerte **zu früh**: Im 400M-Run re-parkte der Boot-Task danach
(`tc_SigWait=0`, SP unverändert `$C122BC`). Auswertung des frischen
RAM-Dumps zeigt, dass zu späterer Zeit zwar ein echter gequeueter
Reply bei `$C95B38` existiert, aber der Fake-Signal überlistete das
`DoIO`-Wartmuster (`WaitPort`→`GetMsg→NULL`→weiter ohne IO-Ergebnis).
**Verhalten gefixt:** Fallback nur noch bei *echtem* gequeuedem
`NT_REPLYMSG` (`io_Error`-Byte=7-Typ-Check), sonst Return ohne Wake
(`waiting for device`). Logs: `/tmp/t_flAM.log` — der Wake feuert
jetzt garnicht mehr (korrekt), Run bleibt im Stall bei instr 14,205,166.

### 8.2 Port/Message-Enumeration korrigiert

Diag-Loop (beide Schleifen) capte bei 8/10 Treffern — TCB-Felder
(MemEntry-Pointer) produzierten Junk-"Ports" vor dem echten
`$C95B10`-Port. Cap jetzt 64, Scan läuft komplett durch (`np=12`
Candidate-Ports im Launch-Stadium, `nm=0` — `CreateMsgPort` noch nicht
erstattet). Logs `/tmp/t_flAM.log`.

### 8.3 Neuer Punkt für die nächste Session

Der `sigwait=0x8000`-Wait des Boot-Tasks ist **nicht** die
DoIO-ReplyPort-Warte (der Später-Zeitpunkt-Port hat `mp_Flags=0,
mp_SigBit=255=0xFF` — gar kein Signal zugewiesen!). Suche: wo kommt
`Wait(0x8000)` her? Am SP `$C122BC`: `SP+18=$C95A44` (NT_LIBRARY-Node,
Name→ext-ROM `E62F21`) — *OpenLibrary* lief sicher, dann Warte auf
Signal-Bit 15. Verdächter: SIG-Antwortkette der aufgestarteten lib/
resource-Init (`expansion.library`/`dos.library` init_trampoline?) —
Challenge: den Fruh-Boot-Flow bis `bootstrap.c` verstehen, zu welchem
Subschritt die Warte gehört und wer das Signal liefern müsste.

## 9. Session 3 — Verifizierte Daten (Logs /tmp/t_flAP–AR.log, Dump /tmp/ram_flAQ.bin)

### 9.1 Prüfmittel
- RAMW-Trace mit Stack/Reg-Dump erweitert (QRX_FULL_TRACE_RAMW=c1240e +
  QRX_FULL_TRACE_RAMW_STACK=1): Bei jedem Write auf Boot-TCB+
  0x16 (tc_SigWait) werden D0–D3/A0–A2/A6/SP + 28 Longwords vom
  Gast-SP gedumpt. Live-Bau: `full68k/qrx68100_full.c` Zeile
  ca. 765 (function `qrx_full_bus_write`).
- INTENA-Trace (`QRX_FULL_VBLANK_TRACE`) + Dump mit frischer
  RAMW-PC-Erweiterung t_flAQ.log; die `$DFF09A`-Writes in der
  ROM-Phase (PC ab $F80000) zeigen - das Gast-ROM schreibt
  INTENA NUR als `$C000` (SET Master-bit) im Idle-Loop
  `$FE685A`. Die `$7FFF`-Writes (CLEAR alles + master) kommen
  alle aus der QRX-FIRMWARE-Phase (PC $00088xxx) vor dem
  ROM-Handoff, NICHT aus dem ROM.

### 9.2 Der finale Halt Vanessa — verifiziert
1. Der CPU-Halt ist **nicht** der Boot-Task, sondern der
   **BackColor-Idle-Stop des ROM-Kernels**: PC `$FE6866` (bra
   `$FE683E`), Opcode `STOP #$2000` bei `$FE6862` (IPL=0, S=1).
2. Der Call vor dem Stop (`jsr $FE7F68`, ROM-Kernel "steal work")
   liest `SysBase+368/ (=0x186+?)` TaskReady-head `$C00590+0x14`
   -> wenn keine Task bereit: `TDNestCnt=-1`, `INTENA=$DFF09A=$C000`
   -> `STOP #$2000`.
3. Der letzte Sync-Zaun:`[VBLANK] assert frame=618 intena=4024
   instr=14003900` (VBLANK-Lieferkette sync-funktionsfähig
   bis dahin: 174 asserts, alle mit intena=0x4024).
4. Die INTENA-Gate-Bedingung für VBLANK im Engine-Code ist
   `(intena & 0x4020u) == 0x4020u` — korrekt für Paula-Semantic
   (bit15 SET, bit14 Master). Der Halt ergibt sich NUR weil
   keine Task mehr mit TS_READY existiert (TaskReady war leer
   beim letztem Idle-Scan).

### 9.3 De herstelde Sozfall-Struktur im finalen Dump
- **ThisTask $C30F48**: tc_State=4 (TS_WAIT), SigAlloc=$3FFFFF,
  **SigWait=$3F0000 = Signale 16..21**, SigRecvd=0. Name-PTR
  zeigt außerhalb des RAM-Gastbereichs ($77A80804) — daher
  Debug-Interpretation: vermutlich Konsole/-serielle Empfangs-
  Task-Objekt aus der QRX-Firmware-Region, kein aus Guest-ROM-
  gebautes AROS Task objekt.
- **TCB $C123F8 (historisch "Boot-Task")**: SigWait=0, nur
  Name-PTR=$26CE0804 out-of-RAM. Kein echter AROS-TCB am Ende.
- **MsgPort $C95B10** war mit SigTask=$C123F8, sigbit-Word=$0000,
  head/tail-pred=$C95B38 mit msg-list head und tail-pred beide
  >>im-loop-signalisierbar<<. Die ReplyMsg $C95B38 ist
  NT_REPLYMSG (type=07) und io_Device-ZFrog-Feld zeigt auf
  $C30EBC (kein Trackdisk).
- **Konklusion: die "Boot-Task-Stall"-These der Session 1+2
  war von der Struktur der FW-$-C1-Feldw parsen fehlcoded:**
  $C123F8 + $C30F48 sind beide *nicht* die Code-Blocking-
  Task-Objekte, die AROS beim físico-Boot erzeugt. Die
  eigentliche Fortsetzung des Boot-Ablaufs beim QRX-OS-One
  liegt NICHT im AROS-Rom-Boot, sondern (seit 9.2 Punkt 4)
  im LAUF der QRX-Firmware-Own-Layer ohne Gast-ROM.

### 9.4 Konsequenzen für den künftigen Ansatz
- Der"boot_reply_wake"-Mechanismus (Session 1) und
  "stuck_kick"-Fixes (Session 2) entretiten den Falspuren:
  Sie hängen an TCB-Signaturen ($C123F8/$C30F48_CAPAA), die
  von der QRX-Firmware stammen, nicht vom Gast-ROM.
- **Neue Richtung:** die Endstall-Historie "t[FL] DMA start"
  (Session-1-Ziel) läuft hier NICHT über den AROS-Boot; der
  Zielmarker kommt aus der QRX-FIFTM-Fl-DMA-Codeebene,
  deren Code und Anker zu identifizieren ist. Der
  Boot-provider-Parameter "QRX_FULL_BOOTKICK_TRACE" braucht
  andere Signalization.

## 10. Session 4/5 (Fix 1: Ready-List-Orphan + Fix 2: int_ack-Flag-Erhaltung)

- **Fix 1 (Ready-List-Reparatur)**: Boot-TCB `$C123F8` lag verwaist READY vor
  (ln_Pred/ln_Succ = Sentinel-ah_e, aber Liste leer). Sweeps hatten `sb+406`
  blank geschrieben; die "already runnable"-Prüfung verhinderte Re-Enqueue.
  Implementiert: `kick_ready_contains()` (max 16 Knoten), `kick_ready_enq()`
  (Tail-Append + Neighbor-Backreference-Check), `kick_ready_orphan_resweep()`
  (NT_TASK+TS_READY+Sentinel-Links+Name-Range → Re-Append).
- **Ergebnis**: Boot weit vorangekommen (44M→141M instr ohne Fix→mit Fix),
  Boot-Task dispatched, ThisTask=`$FAACE6`-trackdisk, [FL]-Signals
  `/intena`-Bilder in `/tmp/boot_v8..v26.log`.
- **Fix 2 (`int_ack` L2-Flag-Erhaltung)**: ROM-exec-Level-2 (`$FE609E`→`$FE6204`)
  braucht CIA-Flags; alte int_ack-Prävention machte `tb_Elapsed`/`tb_CurrentTime`
  ($C29B6E) jedesmal 0 → input.device hart in CmpTime-Loop `$FAACE6→$FAB1E0`.
  Level-2 räumt jetzt nichts mehr weg (Gast macht ICR-Read + `$DFF09C`).
  Neue INTREQR-Emulation: `$DFF01C` gibt `v |= irq_level>=2 ? 0x4000 : 0`
  (bit14 L2-in-flight), `$DFF01E` unverändert.
- **Verifiziert im AROS-Mirror** (`/tmp/AROS-sagagfx/AROS-master`):
  `cia_init.c:163`: `AddIntServer(INTB_PORTS, &base->ciaint)` — CIA-INTs
  laufen über die PORTS-Line (level 2). TimerBase-Registratur (EClock,
 microHZ, vblank) liegt in `timer/lowlevel.c`.

## 11. Session 6 (heute): TimerBase-Layout, ports_asserted-Fix, [PCWATCH]-Beweis

### 11.1 Was neu verifiziert ist

1. **TimerBase-Layout (ROM $FAACCE-$FAAFF0 init)**: Die RAM-RUN mit
   `QRX_FULL_TRACE_RAMW_RANGE=c29b30,c29bb0` (v27) zeigt die echte Struktur
   nahe `$C29B30` (TimerBase):
   - `+0x52/$C29B82` = `tb_cia[?]`-Pointer `$C25200` (CIABase A)
   - `+0x56/$C29B86` = `$C25278` (zweiter CIABasezeiger)
   - `+0x5A/$C29B8A` = `$C25200`
   - `+0x5E/$C29B8E` = `$BFE001` (CIA-A-Basis)
   - `+0x62/$C29B92` = `$BFEE01` (TimerA/EClock-CR)
   - `+0x66/$C29B96` = `$BFE601` (TimerA-LO), `+0x6A` = `$BFE701` (HI)
   - `+0x8E` = `tb_eclock_rate` (709379 PAL / 715909 NTSC, ROM-$FAAF48)
   - `+0xF2` = `tb_vblank_rate` (60/50), `+0xF4` = `tb_vblank_micros`
   - `+0xF8` = `92385`/`91542`, `+0xFC` = `23245`/`23459` (PAL/NTSC)
   - Das Trio `$C29B6A/$C29B6E/$C29B72`: ROM-Init schreibt dort
     MinList-Sentinel-Muster (`lea a2+0x3A; movel→a2+0x3A; clrl a2+0x3E`),
     aber später schreibt exec-Code dorthin Pointer-Werte (`$C30B48`).
     Nach AROS `timer_intern.h` wäre +0x3A `tb_CurrentTime.tv_secs` —
     widersprüchlich. **$C29B6E ist KEIN verlässlicher
     tb_CurrentTime.tv_secs-Marker**; die frühere Vorstellung, es sei ein
     Sekunden-Counter, bleibt offen/unbestätigt.
2. **`$C37C00`-Region (v28)**: nur Allocaten (PC $FFB64A) bei 12.83M, keine
   Zeitschreib-Jites. Die Zukunft des tb_Zeit-writes liegt woanders.
3. **[PCWATCH] auf `$FAB1E0` (v30)**: 24 Hits bei 12.8M-14.26M, alle aus
   der Task `$C30F48 'input.device'` (st=2 RA=$FAACEC). D0 wechselt
   6→3→0x1A→1; SW 0→0x3F0000→0x2F0000; SR 0000/0010. Das input.device ist
   NICHT in einem Endlosspin-Tod, sondern ruft der ROM-Timer-Funktionen
   (trampolines `$FAACC0-$FAAD10`), extrem ruhig — der CPU läuft beim
   TimerVerkehr, *nicht* im L AVCapture-Stuck.
4. **[INTENA]-Write-Trace (v31)**: 1326 Writes, die frühen aus
   PC `$0008xxxx` (QRX-Firmware-Phase), PC `$FE68xx` im ROM (Idle-Loop
   `$FE685A` über INTENA `$C000`). Die `instr=4.3B`-Anomalie betraf
   der Counter der AUX CPU (die `instr`-Feld spiegelt die
   `active`-Struktur bei AUX-Zeiten). **Kein Guest-Bug**.
5. **AROS-Quelltext** (`timer/lowlevel.c`): `ciab_eclock`-Handler macht
   `tb_CurrentTime.tv_secs++` PRO `tb_eclock_rate/ECLOCK_BASE`
   EClock-Int (bei 709379/0x10000 ≈ 10.8 INT/s). `ciaint_timer`-Handler
   (UNIT_MICROHZ) inkrementiert `tb_micro_count` und ReplyMsg-extrahiert
   fertige Requests. Ohne die beiden INTs kein SysTime-Fortschritt.

### 11.2 Implementierter Fix (diese Session): `ports_asserted=1` on CIA-Timer

In `cia_a_tick` / `cia_b_tick` / `ciab_tb_tick` (qrx68100_full.c ~130-190):
beim Underflow-Flag zusätzlich `c->ports_asserted = 1`, weil AROS die CIA-
INTs über `AddIntServer(INTB_PORTS, ...)` hängt — der PORTS-Lane-INTREQ
muss high gehen, damit der ROM-Level-2 (`$FE609E`) mit `$DFF01C`-bit14
den CIA-Server-Verzweigung sauber sieht (statt nur über die
CIA-Flag-Lane ohne INTENA-Gate).
Build: 5/5 Tests PASS. Läufe v29 (ICR+PAULA), v30 (PCWATCH/IRQ),
v31 (INTENA) gleiche Boot-PC-Endpunkte wie v25/v26: unter 45M instr
steckt der CPU noch im ROM-timer-Komplex (`$FAA3xx`), noch kein
RTG/Ausgabe. Der input.device CmpTime-Poll läuft, aber
`tb_CurrentTime`-advancement wird NICHT beobachtet (offener Punkt).

### 11.3 Offene Punkte / nächster Schritt

1. **Finden des echten `tb_CurrentTime`-Feldes**: RAMW_RANGE über die
   restliche TimerBase-Region (z.B. `c29b40-c29b60` und `c37xxx`), um
   die `tb_eclock/tb_micro_count`-Zeitfeld-Offsets zu finden, und
   verifizieren, dass nach dem ports_asserted-Fix ein Zähler wirklich
   pro EClock-INT inkrementiert.
2. **Der Definitive Beweis für den CmpTime-Loop-Exits**: Wenn ein
   tb_Zeitfeld läuft, sollte `CmpTime`-D0 der [PCWATCH]-Hits von
   `6/3/0x1A/1` auf `0`/negativ wechseln und der Task aus dem Loop
   austreten — der Boot sollte dann Richtung disk-Init/RTG-Board
   fortschreiten.
3. **Anmerkung Tools**: `QRX_FULL_TRACE_RAMW_RANGE=X0,X1[,max]` neu
   implementiert (Region schreibt mit PC+instr, ab v27).

## 12. Session 6b — TimerBase-Layout vollständig entschlüsselt + INTENA-Trace-Debug

### 12.1 `struct TimerBase` @ `$C29B30` — exakte Offset-Karte
(passt exakt zu AROS `timer/lowlevel.c`/`timer_intern.h`, `tb_Device`=34 B):
| Off | Addr | Feld | Wert aus v27/v32 |
|----|--------|----------------------|------------------------------|
|+0x22|`$C29B52`| `tb_CurrentTime.tv_secs` | **niemals geschrieben** |
|+0x26|`$C29B56`| `tb_CurrentTime.tv_micro` | niemals |
|+0x2A|`$C29B5A`| `tb_Elapsed.tv_secs` | niemals |
|+0x32|`$C29B62`| `tb_lastsystime` | niemals |
|+0x3A|`$C29B6A`| `tb_Lists[0].lh_Head` | Request-Enqueue/Dequeue-Traffic |
|+0x52|`$C29B82`| `tb_cia[0]` | `$C25200` (CIA-A-Resource) |
|+0x56|`$C29B86`| `tb_cia[1]` | `$C25278` (CIA-B-Resource) |
|+0x5A|`$C29B8A`| `tb_eclock_res` | `$C25200` |
|+0x5E|`$C29B8E`| `tb_eclock_cia` | `$BFE001` |
|+0x62|`$C29B92`| `tb_eclock_cr` | `$BFEF01` (CIA-A Timer-B CR!) |
|+0x66|`$C29B96`| `tb_eclock_lo` | `$BFE601` |
|+0x6A|`$C29B9A`| `tb_eclock_hi` | `$BFE701` |
|+0x76|`$C29BA6`| `tb_ciaint_eclock`+ | is_Code≈$FAA6E, is_Data=$C29B30 |
|+0x8E|`$C29BBE`| `tb_eclock_rate` | 709379 PAL / 715909 NTSC |
|+0xF8|`$C29BE8`| `tb_eclock_micro_mult` | 92385 / 91542 |
|+0xFC|`$C29BEC`| `tb_micro_eclock_mult` | 23245 / 23459 |

Erkenntnis: **EClock = CIA-A Timer B**, `tb_CurrentTime`/`tb_Elapsed`
TV-SECS-Adresse = `$C29B52`/`$C29B5A`. Formal bewiesen (v27/v32):
die ROM-`ciab_eclock`-ISR schreibt sie **kein einziges Mal** — der
CPU-Level-2-Durchlauf bis zur CIA-ISR endet vor `ciab_eclock`.

### 12.2 [INTENA]-Trace-Bug gefunden und gefixt
Die bisherige Zeile `"[INTENA] w=%04x -> %04x PC=%08x instr=%llu"` hatte
**keinen PC-Parameter**: `PC=` druckte den Lauf-Basiswert des Zählers,
`instr=` Müll. Neu: echtes `m68k_get_reg(PC)` + `ctx=G/?-Marker`
(G = Gast-Runner `c==active`). Resultat v34: **alle** 1326 Writes sind
`ctx=G` — jedes INTENA-Write vom Gast selbst; kein AUX-Foreign-Bug.
Die "impossible instr"-Anomalie (4.3B/6.1B) war einfach der
verschluckte Parameter. Mehrgewicht: die [INTENA]-PC-spalten
bisheriger Logs (`$00088xxx`/`$DAD5D2`) waren Zählerwerte, keine PCs.

### 12.3 Der entscheidende INTENA-Endzustand (v34)
```
14.281M w=c000→4000  PC=$F8609E   (exec L2 stub: Master an / INTEN-RA)
14.281M w=4000→0000  PC=$F86068   (Disable)
14.341M w=4000→0000  PC=$FE6714   5×  (exec Disable-Loop)
14.341M w=c000→4000  PC=$FE7958   (exec Enable/LVO)
14.341M w=8004→4004  PC=$F880B2   (SET INTF_DSKBLK)
--- danach: NIE WIEDER ein INTENA-Write,
Endzustand `intena=0x4004` forever
```
`0x4004` = Master + DSKBLK — **VBLANK-Bit (0x0020) und PORTS-Bit
(0x0008) sind nie wieder an**. Damit scheitert:
- `(intena & 0x4020) == 0x4020` → **kein Level-3/VBLANK-INT je wieder**
  (174 VBLANK-asserts, alle vor 14.34M),
- der ROM-exec `$FE6204`-Level-2-Wait-Pfad hat keine Fortsetzung
  CIA-Server-seitig (`ciab_eclock` wird nie gerufen),
- `tb_CurrentTime` bleibt 0 → der input.device CmpTime-Poll
  spinnt ewig — das GATE des Boot-Stralls.

### 12.4 Nächster konkreter Schritt
Der Boot braucht INTENA-Bits `0x0020` (VBLANK) / `0x4008` (PORTS+Master)
wieder. Optionen:
1. **Host-Emulation-Fix**: Unser `paula_recompute`-Level-3-Lane
   das INTENA-VBLANK-Bit (`0x0020`) im ROM-Boot-Window von
   `intena=0x4004` aus weglassen und viaguest-Write (`$DFF098/09A`) —
   d.h. ein ASSERT: sobald das KickCode-End-Disable alle Bits löscht
   und kein Gast-Enable kommt (timeout ~0.5 s simulated), Engine-seitig
   INTENA-VBLANK+PORTS-Bits re-assert (Kick-Fix, mit QRX_FULL-Flag).
2. **ROM-Feintuning**: Der Boot-Kick-Task in der Queue wieder vorziehen,
   so wie Fix 1 einen disabled Task wieder bereitet — der Boot-Task
   muss das `Enable()` machen, das im RC-Previs-Window verloren ging.
3. Einfachster:true-ISA-Fix: `paula_recompute`-Lvl-3-Lane auf
   `vblank_asserted` alleine (`intena & 0x4000`-Master nur) umschreiben —
   ROM-Kick-Code nutzt trotzdem andere Felder — **MUSS geprüft werden**,
   welcher der 3 CANDIDATES dem Aros-Boot-Verhalten entspricht.

## 13. Architektur-Korrektur (Session 7): Amithlon-Modus statt Paula-INTENA-Gate

**Prinzip** (User-Richtlinie): Armithlon emuliert anders als UAE *keine*
Chipsets: Display only via RTG-Board (FSAGA), Dateisystem/AHI via Host,
und die interrupt-relevanten Host-Pfade werden selbst vom Board getrieben.
Der aktuelle Stall war der einzige Punkt, an dem die Engine in ein Paula/INTENA-INTREQ-Gating gegangen ist — Ursache: der AROS-m68k ROM
(Contact: `cia.resource + timer.device + exec`-Interrupt) *pollt* klassische
 Regs, unsere Shim-Stücke wuchsen damit in was chipset-emulatives hinein.

**Fix (implementiert, v35)**: `paula_recompute()` decodiert jetzt
`QRX_FULL_AMITHLON` (Default 1): alle Level-Entscheidungen fallen
*ohne* INTENA-Schatten-Gate — `vblank_asserted → lvl=3`,
`ports_asserted → lvl=2`, CIA-Flags → lvl=2, `dskblk_asserted → lvl=1`.
Der gastseitige INTENA-Schatten bleibt nur Buchhaltung (tracekiller-Prinzip).
`QRX_FULL_AMITHLON=0` schaltet den alten chipset-gated Pfad (für Vergleichs-
Laufe) zurück.

**Ergebnis v35** (Schrittzeile `/tmp/boot_v35.log`): Endpunkt unverändert
(PC=$FAA360, 44.2M instr, kein weiterer Nachlauf). Die PAULA-Probe zeigt jetzt
sonderbarerweise korrekt: amt die Host-Weck-Schaltung aktiv (lvl=2 bei
flags 02/0b) aber der ROM bootet am gleichen Punkt fest. Damit:
- Der Trace/Debug-Weg ist architecture-clean (keine chipset-gated Grate mehr),
- aber die eigentliche Boot-Pause (ciab_eclock/tb_CurrentTime-Never-Write)
  ist UNVERÄNDERT offen — sie zeigt, dass hier nicht ein INTENA-Gate,
  sondern das fehlende `tb_eclock`-Wachs (CIA-A-TimerB-CIA-Fire → der
  `ciab_eclock`-INTZweig des guest-Codes) blockiert.

**Nächster konkreter Schritt (Amithlon-Weg)**: Den ROM-aufgelegten
CIA-EClock-Pfad HOST-seitig anbinden, nicht chipset-faithful:
1. Der Host-Tick (Engine-Timer, `QRX_FULL_*` MMIO) liefert die EClock-
   Fortschritte selbst — der Gast-CIA-ICR-READ wird vom Host-Fake
   geliefert; kein "CIA-A TimerB Underflow" nötig.
2. Der `ciab_eclock`-INT-Zweig des guest-Code wird — wie bei Amithlon —
   durch ein *Host-Register*-aufruf ersetzt, nicht durch den klassischen
   CIA-ICR-Irre-Vektor: setze den ROM-bindeten Cache in den
   exec-level-2-Chain-Zweig ein, der den Host-EClock-Ausrückplatz direkter
   unlockt (die [ICR-R]-69-Poll-loop)    das ist der Weg, den Amithlon in der ARM-amithlon-version via
   `AddIntServer`- und `exec_restoring`-Workaround baute.

## 14. Session 8 (v43–v55): TimerBase-Erkennung + Host-Eclock funktioniert

**TimerBase zuverlässig identifiziert**: der ROM-SchreibSequenz
`$BFE001, $BFEF01, $BFE601, $BFE701` an vier aufeinanderfolgende Long-Fields
(tb+0x5E/62/66/6A, v27-ROM-Log) pinnt die TimerBase unmissverständlich:
`[HCLOCK] TimerBase=00c29b30 detected` + `rate=709379 (PAL, from field tb+0x8E)`.
Die Erkennung sitzt in `qrx_full_bus_write` und untersucht die
`movel %d0, tb_field`-Sequenz des ROM-`set_eclock`, vgl. unten.

**Host-EClock implementiert (v43+)**: `host_eclock_tick()` rechnet
`tb_Total / 709379` pro `0x10000`-Wrap und schreibt in den Gast:
`tb_CurrentTime`(tb+0x22/26), `tb_Elapsed`(tb+0x2A/2E). Der Tick
greift im `cia_b_tick`-Wind und läuft je 262144 Instr ~92 ms. Der
Log zeigt sauberen Sekunden-Fortschritt:
  sec=0 micro=92385 instr=12.77M, sec=1 @15.39M, … sec=11 @44.25M.

## 15. v47–v56: Trap in `[tb_vb_count]-Advance`-Hypothese

`beginio(TR_ADDREQUEST, UNIT_WAITUNTIL)` im AROS-m68k
(`arch/m68k-amiga/timer/beginio.c:96-134`) konvertiert die
(Relative)RelativeLayout—Zeit mit `convertunits(TB, tr_time, UNIT_VBLANK)` in
ein **_FRAMES**-Ziel und setzt es auf tb_Lists[UNIT_VBLANK]. Der CIAB-vbint—ISR
(`ciab_vbint`, lowlevel.c:264) zählt `inc64(tb_vb_count)` und replied
alle Aufrufe, bei denen `cmp64(tb_vb_count, tr_time)` TRUE zurückgibt.

**Implementierter Advance (v47–v52)**: host-seitig `tb_vb_count` via
`tb+0xEE` (secs) / `tb+0xF2` (micros) hochzählen. Beobachtung invalid:
  - `[TBMAP]`-Einmal-Snapshot v55: `vbl_rate@F6=1, vbl_micros@F8=1` —
    der Felder sind **nicht die erwarteten Ratenkonstanten**;
  - `[RAM]-Trace` tb+0xF2: `W16=0x32 (50 PAL) @PC=$FAAF5C`,
    `R16 0x32→0x41→0x8D @PC=$FAA7CC` — tb+0xF2 ist die
    **VBlank-Rate (50 Hz)** bzw. ein dynamischer Counter, **nicht**
    `tb_vb_count.tv_micro`.

**Fazit**: die m68k-amiga `struct TimerBase`-Offset-Karte ist **ELSEwhere**
als unsere Ansätle aus `compiler/rom/timer/timer_intern.h` (.deltaTime).
Der beide Advance-Pfad schrieb in ein falsches Feld. **Default OFF seit
v56** (`QRX_FULL_HOST_VB_COUNT=1` als Laufzeit-Opt-in).

## 16. v56: Effekt des Advance-Disable

Nach Abschalten des `tb_vb_count`-Advance (**v56** frische Basis) ist der
Boot-Verlauf **strukturidentisch zu v45** — `input.device`-Task dreht im
`[ROM]-Base`-Loop `$FAACF0 ↔ $FAA360` (je 256 `[PCWATCH]`-hits, keine
`$FAB1E0`-Treffer mehr). D0 rotiert 6/3/0x1A/1-Hit-Ergebnis.
Kein `[RO]`-Write-Verlust-Log-Fund, kein KickstartROM-Write entr
(org rom_read bleibt readonly).

## 17. Session 9 (v58–v68): korrekte Layout-Tabelle + 32-Bit-Overflow-Fix + `ciab_eclock`-ISR läuft

**Layout korrigiert** (v50-`[RAMWR]`-Trace + v55-`[TBMAP]`- + v59-Read-Beweise
über alle bis tb+0x110 beobachteten Felder):
```
+0x96  tb_micro_res
+0x9A  tb_micro_cia
+0x9E  tb_micro_cr
+0xA2  tb_micro_lo
+0xA6  tb_micro_hi
+0xAA  tb_ciaint_timer (22B)
+0xC0  tb_micro_intbit (UWORD)
+0xC2  tb_micro_started (ULONG)
+0xC6  tb_micro_count (timeval 8B)
+0xCE  tb_micro_micros (ULONG)
+0xD2  tb_micro_on (BOOL)
+0xD4  tb_vbint (Interrupt 22B)
+0xEA  tb_vb_count.tv_secs  (ULONG)
+0xEE  tb_vb_count.tv_micro (ULONG — FRAMES-Zähler)
+0xF2  tb_vblank_rate (UWORD — v55 verify: 50 = PAL)
+0xF4  tb_vblank_micros (UWORD — v59 verify: 20000 = 1e6/50)
+0xF6  tb_vblank_on (BOOL)
+0xF8  tb_eclock_micro_mult (ULONG)
+0xFC  tb_micro_eclock_mult (UWORD)
+0x100 lastsystimetweak (ULONG)
```
`[TBMAP]`-Snapshot v59: `vbl_rate@F2=0x32 (50), vbl_micros@F4=0x4E20 (20000),
vblank_on@F6=1` — **exakt die AROS-m68k-Konstanten**.
Damit die m68k-amiga `struct TimerBase`-Kartierung ist **bewiesen**.

**FY-Bug v60**: `65536u*1000000u/c->host_tb_rate` war in `uint32_t`
übertreten (`u32 overflow`) → nur `1566` statt `92 383` µs/Tick; jeder Host-
Tick sprang nur 1 Frame. **Fix**: 64-bit:
`acc = c->host_vb_acc + (uint32_t)(((uint64_t)65536u*1000000u/rate) * wraps);`

**v65/v66**: 262+ Frames in 44.25M instr, `readback`-probe bestätigt
`bmicro` wird korrekt übernommen. Der `[PCWATCH]`-Befund:
  - `$FAA5EE`: `A0=$BFE701 (CIA-B TBHI), A1=$BFE601 (TBLO), A2=$C29B30 (TB),
    RA@$C29B30` und `D0 = 0x17B40→0x18AB7` **wächst** —
    **das ist die m68k `ciab_eclock`-ISR-executing** — der ReadEClock-Loop
    greift direkt auf die CIA-B-Regs zu und liest dabei `tb_CurrentTime`
    (27 Reads je `$FAA5EE`, kein ROM-Write auf `$C29B52`).
  - `input.device` verlässt den `$FAACF0/$FAA360`-Wait-Loop
     und wechselt zwischen `trackdisk.device`-Tasks (430) und
     `input.device`-Hits (107) — der Scheduler podsta starke Durchlaufen.
  - Boot endet bei `$FAACEC` — dem `AddTail`-Teil der TimerList-Insertion;
    **kein ROM-Soft-Fehler, keine Shutdown, `fault=0`**.

**Fazit** (Session 9): Die Host-Advance-Route läuft jetzt **korrekt**
(vrate/vmicros/vbcount → 4–5 Frames je Host-Tick). Der ROM
läuft in der AROS-m68k-eClock-Routine (`ciab_eclock`-disassem-Code zu
`$FAA5EE`: direkt CIA-B-TimerB-Regs lesen) und die `input.device`-
Task verlässt den `$FAAB1E0`-Loop — **aber der eigentliche Desktop-Boot
bleibt weiter am `$FAACEC`-`AddTail`-Punkt hängen**:
`fault=0`, `shutdown=0`, der Inhalt
`result PC=00faacec D0=00000001 A0=00c37c30 A1=00c30b40 instructions=51.46M`
(v68, 300M cycles).

**Der `ciab_vbint` (VBlank-ISR) tritt noch nicht auf** und der
`WaitUnit`-gedeigene States nicht vollgültig. **Nächster Schritt**:
der `tb_vb_count`-Field (tb+0xEE) wird für `A0`/`A1`-Pointer einer
`$FAAACE`-`$FAAACCC`-AddTail-Route gelesen.
So erreichen wir das erste `input.device`-Signal im `$FAAA360`-dispatch-loop.
