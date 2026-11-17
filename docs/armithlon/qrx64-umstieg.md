# QRX OS One 64 Bit: Umstieg von qrx-win32, QRX86 und QRXPPC

Stand 2026-11-11.

## Entscheidung

Am 2026-11-11 hat der Nutzer entschieden: Classic jetzt, nativ später.

- **Keine HLE für 32-Bit-AROS-Programme**, also kein WoW64 für AmigaOS.
  - AmigaOS-Programme lesen Strukturen wie `Window`, `RastPort`, `IntuiMessage`, `Process` und `TagItem` direkt. Mit 8-Byte-Zeigern sind sie anders aufgebaut. Jede Bibliothek und jede Struktur müsste deshalb in beide Richtungen übersetzt werden, auch geteilte Strukturen und Hooks.
  - Der 68100EC64 kann außerdem bei einem Bibliotheksaufruf nicht zwischen 32-Bit- und Long-Mode wechseln.
- **Classic:** Das ganze 32-Bit-QRX-OS-One läuft als eigene Instanz weiter, mit dem 68100 im 32-Bit-Modus, dem M1-JIT und seinen eigenen Bibliotheken. Zwischen den Systemen werden nur Bildschirm, Eingabe, Dateien (HostFS) und Ton verbunden. Alle heutigen Spiele und Programme laufen dort unverändert.
- **Nativ später:** qrx-win32, der QRX86-Core und QRXPPC sind eigener C-Code. Sie werden für `m68k64` übersetzt, sobald drei Voraussetzungen erfüllt sind:
  - Der EC64 hat einen Long-Mode-JIT und eine FPU.
  - Das 64-Bit-AROS hat dos, graphics, CyberGraphics, AHI und die Doors.
  - Die JIT-Backends von QRX86 und QRXPPC erzeugen Long-Mode-Code.

Warum nicht jetzt nativ: Der EC64 ist ein Interpreter mit 41 bis 77 Millionen Befehlen pro Sekunde. Der 32-Bit-68100 läuft mit dem M1-JIT. Spiele wären auf 64 Bit heute um ein Vielfaches langsamer.

## Was der native Umstieg im eigenen Code braucht

| Teil | Bedarf |
| --- | --- |
| Gast-Adressen | Die Adressen von x86 und PPC bleiben 32 Bit (`uint32_t`), daran ändert sich nichts |
| Zeiger im Host-Code | Siehe die Prüfung unten: qrx-win32 und der Core sind schon LP64-sauber |
| x86-JIT (`core/qrx86jit.h`) | Er erzeugt 68040-Code. Im Long Mode arbeiten `JSR`, `RTS`, `PEA` und `LINK` mit 8 Byte, `.L` auf Adressregistern wird vorzeichenerweitert, und `.L`-Schreibzugriffe löschen die obere Hälfte. Er braucht ein eigenes Long-Mode-Backend oder wird anfangs abgeschaltet |
| PPC-JIT (`tools/qrxppc`) | Dasselbe. Dieser Teil gehört zur parallelen PPC-Arbeit und wird nur nach Absprache angefasst |
| x87 und PPC-FPU | Doubles. Soft-Float aus compiler-rt geht, wäre aber langsam. Besser ist eine FPU im EC64 |
| Backend | `backend_aros.c` nutzt CyberGraphics und Screens, AHI kommt hinzu. Auf dem 64-Bit-AROS fehlt beides noch |
| Threads | `threads.c` auf AROS wechselt den Stack von Hand und muss für 8-Byte-Frames geprüft werden |

## Prüfung: qrx-win32 als 64-Bit-Programm auf dem Mac (2026-11-11)

Gebaut wurde mit dem Host-Clang auf arm64 (LP64, Little Endian) aus `core/qrx86core.c` und `win32/*.c`, mit `backend_headless.c`. Ohne JIT, denn der gilt nur für 68k.

| Prüfung | Ergebnis |
| --- | --- |
| Übersetzen mit `-Wall -Wpointer-to-int-cast -Wint-to-pointer-cast` | Keine Fehler. 12 Warnungen, keine davon betrifft Zeigerbreiten |
| `-Wshorten-64-to-32` | Keine Kürzung von 64 auf 32 Bit. Nur 3 Hinweise auf veraltete `ucontext`-Funktionen |
| Fflush.exe (MS Puzzle Collection), 16 s | Fenster, Menü und Grafik richtig |
| Freddemo.w32 | Richtig, nach der Korrektur unten. Vorher schwarz, und das war kein 64-Bit-Fehler |

### Freddi beim ersten Start (2026-11-12)

Freddi blieb im 64-Bit-Build schwarz, im Gast dagegen nicht. Der Grund lag nicht an 64 Bit, sondern am Zustand:

- Der Gast hatte aus früheren Läufen eine `SYS:WINDOWS/hegames.ini`, der Mac-Lauf nicht.
- Ohne diese INI misst Freddi zuerst Ton, Laufwerk und Grafik und zeigt dazu einen Dialog.
- Wenn der Dialog aktiv wird, pausiert das Spiel den Ton mit `waveOutPause`.
- Unter Windows aktiviert das Schließen des Dialogs wieder das Hauptfenster. Das Hauptfenster bekommt dann `WM_ACTIVATE`, und das Spiel setzt den Ton mit `waveOutRestart` fort.
- `win_destroy` hatte dafür zwar ein neues aktives Fenster gewählt, ihm aber keine Nachricht geschickt. Der Ton blieb pausiert, und weil das Spiel seine Bilder am Ton ausrichtet, kam nie ein Bild.

Dasselbe galt im 32-Bit-Gast für jeden ersten Start, also für jede frische Installation. Jetzt aktiviert `win_destroy` den Besitzer, sonst das nächste sichtbare Hauptfenster, und schickt `WM_ACTIVATE` und `WM_SETFOCUS`. Danach zeigt Freddi beim ersten Start sowohl im 64-Bit-Build als auch im Gast die erste Szene.

Survey im Gast (67 Einträge, gegen den vorigen Lauf): keine Regression. Die Bildraten sind gleich oder etwas höher, das liegt im Rahmen der üblichen Schwankung. Freddi zeigt jetzt Bilder.

Für Tests mit dem Mac-Build heißt das: Den Zustand des Gastes, etwa INI-Dateien in `SYS:WINDOWS`, gleich herstellen, bevor man Unterschiede auf 64 Bit schiebt.

Grenzen: Der Mac-Build ist Little Endian, der m68k64 Big Endian. Ein Test, der beides zusammen prüft, braucht `m68k64-clang` und eine C-Bibliothek für das 64-Bit-AROS, und die gibt es noch nicht.

## Erster nativer Lauf auf dem 68100EC64 (2026-11-12)

qrx-win32 und der QRX86-Core laufen als native 64-Bit-Programme auf dem 64-Bit-AROS. Getestet wurde auf der Referenzmaschine mit zwei Kernen, Start von einer FFS-Platte, mit der C-Bibliothek des AROS 64.

| Teil | Stand |
| --- | --- |
| Build | `build-ec64.sh` mit dem m68k64-clang. Headless (`-DQRX_NO_AROS_BACKEND`), ohne x86-JIT. Am Quelltext war nur der Fiber-Wechsel in `threads.c` neu (`__m68k64__`: D2-D7/A2-A6 mit `movem.q`, keine FPU-Register) |
| Fflush.exe, Skript `wait 16000`, `snap` | Das Bild ist **Byte für Byte gleich** mit dem 64-Bit-Mac-Build (1.447.413 Byte). Das gilt mit dem korrigierten clang, ohne Umgehung im Quelltext. Damit sind 64 Bit und Big Endian zusammen für diesen Pfad geprüft |
| Der 32-Bit-Gast-Build | Unverändert, die HUNK-Datei hat denselben SHA-256 wie vorher |

Gefunden auf dem Weg:

- **Fehler im m68k64-Backend:** Die Stack-Neuausrichtung im Prolog (`BuildStackAlignAND`) nahm immer D0 als Hilfsregister und zerstörte so ein Argument in D0. Ausgelöst wurde sie von `jmp_buf`, das im AROS-Header `aligned(16)` ist. Getroffen hat es `run_loop` mit `stop_eip`, und der verschachtelte Aufruf von `DllMain` lief deshalb über seinen Haltepunkt hinaus. Die Tor-Sitzung hat das im Backend behoben, mit Regressionstest. Programme müssen mit dem neuen clang gebaut werden. Die AROS-Bibliotheken waren zu dem Zeitpunkt noch mit dem alten gebaut.
- **Werkzeug:** Mit einer Befehls- und Aufrufspur, die man zwischen dem Mac-Build und dem EC64 vergleicht, findet man solche Fehler schnell. Der Mac-Build dient dabei als Referenz.

Nicht geprüft:

- Threads und der Ton-Dienst, also der neue Fiber-Wechsel. Fflush braucht beides in diesen 16 Sekunden nicht.
- Andere Spiele, DirectDraw und x87-lastiger Code. Gleitkomma läuft als Soft-Float.
- Die Geschwindigkeit: Das Bild lag innerhalb einer Laufzeit von 25 Minuten vor, für 16 Sekunden virtuelle Zeit. Wann genau, ist nicht gemessen. Der EC64 ist ein Interpreter, darin läuft der x86-Interpreter, und Gleitkomma wird in Software gerechnet.

## Stand 2026-11-16: 64-Bit-Bundle und Mac-Pfad

Geprüft in dieser Umgebung:

| Prüfung | Ergebnis |
| --- | --- |
| `launch64` (EC64, 2 Kerne, 2048 MB, `QRXOS64.img`) | Bootet bis `[EC64] 2 cores, ... disk in RAM` |
| `qrx_ec64_tests` (Interpreter, SMP/CAS über 4 GB) | `all checks passed` |
| `qrx_ec64_jit_tests 3000` | 71947 Instruktionen, 0 Abweichungen zum Interpreter |
| `qrx_ec64_board_tests`, `qrx_ec64_diff_tests` | 0 Fehler (nur Abdeckungshinweise) |

Der Mac-Pfad (`qrx_macho.c`, `qrx_dyld.c`, `qrx_ppc.c`) ist reiner C-Code ohne
68k-Abhängigkeit und damit für `m68k64` übersetzbar, liegt aber noch nicht im
64-Bit-Bundle. Dafür fehlen zwei Dinge, die hier nicht vorhanden sind:

1. Der `m68k64-clang`-Wrapper (`tc/bin/clang`, siehe
   `tools/armithlon/aros-qrx/README.md`) samt `libclang_rt.builtins-m68k64.a`.
2. Ein AROS-64-SDK-Baum (`bin/native-m68k64/AROS/Development`) als Sysroot.

Ohne beides lässt sich kein 64-Bit-`QRXPPC`/`QRXMach` erzeugen; ein Ersatz
wäre eine Behauptung. Der PPC-JIT (`qrx_ppc_jit.c`) erzeugt außerdem
68040-Code und braucht für Long Mode ein eigenes Backend; bis dahin läuft der
Mach-Pfad dort im Interpreter (`ppc->access` erzwingt das bereits).

## QRXDesktop auf 64-Bit (2026-11-16)

Geprüft auf dem 2-Kern-EC64-Gast (Headless, `econsole-startup`):

- Die vorhandene QRXDesktop ist 32-bit (HUNK/ELF32). Neu für m68k64 gebaut
  aus `.build/qrx-desktop/qrx_desktop.armithlon.c` (Build selbst fehlerfrei,
  nur Warnungen) → `C:QRXDesktop` (ELF64) im Bundle.
- Vorher: kein Start, stiller Abbruch. `main()` kehrt mit rc=20 zurück, wenn
  `intuition.library`/`graphics.library`/`dos.library` nicht öffnen.
  Probe im Gast: alle Desktop-Libraries fehlten.
- Nachgebaut für m68k64 (Tor-Build): `gmake kernel-graphics kernel-intuition
  kernel-oop` — bauen sauber. `icon`/`cybergraphics` scheitern am zlib-Port
  (implicit-int unter aktuellem clang); beide sind für QRXDesktop optional.
- Jetzt: `OpenLibrary("graphics.library")` faultet im Init mit Bus Fault
  (`Utility_66_SetMem`, Zieladresse im Kernel-Text). Gleiches für Intuition.
  Das ist ein AROS-64-Init-Bug in graphics, kein QRXDesktop-Bug.
  (Ein einzelner solcher Fault war nicht reproduzierbar; danach kehrt
  Graphics sauber mit MISSING zurück, solange gfx.hidd fehlt.)
- Nachgebaut für m68k64: `kernel-hidd kernel-hidd-gfx
  kernel-hidd-gfx-headlessgfx`. Ein Compile-Bug im Headless-Treiber
  (IPTR→Pointer-Return, `-Werror`) ist im Tor-AROS-Baum gefixt:
  `ReturnPtr(..., OOP_Object *, o)` in `headlessgfx_bitmapclass.c`.
  Die drei `.hidd` liegen in `System64/Devs/Drivers/`.
- Lade-Reihenfolge (Kettenfehler gefunden): `hiddclass.hidd` registriert
  `hwclass`; `gfx.hidd` braucht sie als Superklasse, sonst
  `METADBG no-superptr`. Korrekt: hiddclass → gfx → headless → graphics.
- Aktueller Blocker: `hiddclass.hidd` faultet deterministisch im eigenen
  Init (Bus Fault, PC 0x266C6E, zweimal identisch) während OOP-
  Klassenaufbau/Method-Dispatch; D0 enthält String-Fragmente („etho“).

  **Debug-Befund 2026-11-16 (Tor-Worktree, EC64 SMP, m68k64):** Minimaler
  OOP-Test (`oophidd`: eigene HIDD-Klasse via HiddMeta, `my_get(0x1234)=4242`)
  PASS -> OOP/HIDD-Dispatch mit 64-bit-IPTR funktioniert; Pointer sind auf
  m68k64 64-bit (`sizeof IFMethod=16`, IPTR=8, ptr=8), die ABI-
  Mismatch-Hypothese ist damit widerlegt. Reine C-Mimikrys der
  Iterateifs-Schleife (1 TU, 3 Ebenen, getrennte TUs, -O1/-O2) laufen im
  Gast korrekt. Der Hang repro: `oophidd3` (Kette Hidd/HW/HWRoot wie
  `hiddclass`) hängt in `get_info_on_ifs` (inline in
  `hiddmeta_allocdisptabs`): `while (meta_iterateifs(...))` kehrt nie mit
  NULL zurück. Instrumentierung (Trap-Prints in `hiddmeta_iterateifs`) +
  DEBUG-Build funktionieren; Clean -O2/-O1 hängen, -O0 crasht, Fence/
  Clobber-Fence/Dummy-Call ändern nichts. Hand-verifizierter -O2-Codegen
  von `hiddmeta_iterateifs`, `HIDD_DoMethod`, `privatestubs` und
  `hiddmeta_allocdisptabs` ist korrekt (inkl. `movn`=flagloses Copy,
  Gast-Probe PASS, weite Branches PASS, exakte Loop-Backedge PASS);
  Interpreter==JIT. Schluss: kein AROS-ABI-Bug, sondern der bekannte
  m68k64-clang-Backend-Codegen-Fehler (wie `qrx_toolbox.c` -O2) bzw. eine
  EC64-Engine-Kante, die nur durch genau diese Instruktionsmischung
  getriggert wird.

  **Metaprobe-Befund (2026-11-16, vor MyHWRoot-Erzeugung):** `metaprobe2`
  gedruckt: `numinterfaces=1` (korrekt, kein Datenkorrupt), `mid_fresh=
  0x402 == cached_ob_M_meta_iterateifs` (Offset 1152 in IntOOPBase), aber
  der Direkt-Dispatch `OOP_DoMethod(MyHW, {mid=0x402})` erreicht im
  Clean-Build NICHT `hiddmeta_iterateifs`, sondern Meta::New
  (`basemeta_new`): METADBG no-instsize, NULL-Rückgabe, `iterval` unverändert.
  Im Debug/Instrumentierungs-Build erreicht derselbe Dispatch
  `hiddmeta_iterateifs` (ITER-Trace). Metaclass-Objekt: `mc+56 cl_DoMethod=
  0x2647d2`, `MyHW+56=0x263c86` (verschieden); `mc+40 InstOffset=112`.
  Der exakte mmake-Build-Befehl für `hiddmetaclass.o` (inkl. `-fno-common`,
  `__OOP_NOLIBBASE__`, `-include oop_deflibdefs.h`) reproduziert das
  Build-`.text` byteidentisch; mein handverifiziertes -S ist also das
  laufende Code. Isolation: exakte Loop-Backedge-Asm-Probe (movem/jsr/
  Stack-Args/cmpi/movn/beq-far) -> 99 PASS; `movn` flaglos PASS (11); weit
  `beq` PASS (5); Pattern/Chain (1 TU, 3 Ebenen, getrennte TUs, -O1/-O2)
  PASS; Interpreter==JIT. Schluss: Kein AROS-ABI/State-Bug (Werte exakt);
  der Dispatch in `hiddmetaclass.o`
  geht fehl (Clean -O2/-O1 Hang, -O0 Backend-Crash, nur echte Calls/Trap in
  `hiddmeta_iterateifs` maskieren; Fence/Clobber/Dummy-Call ändern nichts).
  Die EC64 führt isoliert alle Instruktionsmuster korrekt aus; der
  verbleibende Fehler sitzt im m68k64-clang-Codegen von `hiddmetaclass.o`
  (Backend), Engine nur insofern, als ihre Ausführung gegen dieses Codegen
  bei der Dispatch-Auflösung fehlschlägt.
  **IntCallMethod/Meta-Coerce Probe-Befund (2026-11-16, korrigierte
  Nachrichten-Struktur):** `metaprobe5` nutzt jetzt die echte
  `P_meta_iterateifs`-Struktur aus `private.h` (`OOP_MethodID` = 32-bit).
  Instrumentierte Build-Werte: `mid_fresh=0x402`, `cached=0x402`;
  `MCDBG *msg=0x402, mid=0x2, ifid=0x400, hashmask=0x2,
  directptr=0xcb150/0xd41e0, bucket=..., MCFNDBG fn=0x263be0` und
  `ITERDBG idx=0/1, numif=1` — d.h. der Bucket `InterfaceID=0x400`
  liefert als `MethodTable[2]` die echte `hiddmeta_iterateifs`.
  Clean -O2 mit derselben korrigierten Probe crasht bereits beim ersten
  `OOP_NewObject` in `NEWOBJDBG cl/o`: `classPtr=0xd89f8`,
  `o=0x265104` (Code-Bereich) statt instrumentiert `o=0xcb0a8`
  (Heap-Objekt). Die Gästetabellen-Readings (Meta-Bucket
  `MethodTable[2]`, `directptr`) sind in beiden Builds gleich
  (`fn2`= iterateifs-Adresse); der falsche Wert ist also kein
  Tabelleninhalt, sondern der kompilierte Dispatch/pRoot_New-Pfad liest/
  gibt in clean -O2 einen falschen Operanden/Return (`o=0x265104`).
  Damit ist der Fehler auf den m68k64-clang-Codegen im OOP-Dispatch-Pfad
  eingegrenzt; die EC64 führt die isolierten Instruktionsmuster korrekt
  aus, und nur die Clean-O2-Register/Stack-Allokation im Dispatch
  (IntCallMethod/Meta_Coerce/basemetaclass New) kippt.

- Danach fehlt ohnehin noch ein Framebuffer-Treiber (input/console)
  für m68k64, sonst kein Screen/Workbench.

## Stand 2026-11-17: Bild, Ton, Netz und 3D auf QRX OS One 64

Der Blocker vom 16.11. war der Compiler, nicht AROS: m68k64 gab Zeiger in A0 und Ganzzahlen in D0
zurück. AROS ruft OOP-Methoden und Bibliotheksvektoren aber über Prototypen mit der jeweils anderen
Art auf (auf x86-64 ist beides RAX). Das falsche `o=0x265104` war der Inhalt des anderen Registers.
Jetzt liefert m68k64-clang jeden einzelnen 64-Bit-Rückgabewert in D0 *und* A0 (ec64-llvm,
`-mllvm -m68k64-dual-return`, Standard an). Alle Toolchain-Tests laufen ohne Fehler, und AROS 64
ist damit komplett neu übersetzt.

Geprüft auf 2 Kernen, ohne Fenster, mit `--capture`/`--audio-wav`, und im Mac-Fenster:

- **Bild:** `ec64gfx.hidd` (vesagfx auf dem Framebuffer der Platine, 1024x768x24) mit graphics,
  layers, Intuition, input/keyboard/gameport, console, con-handler und cybergraphics im Kickstart.
  QRXDesktop startet aus der Startup-Sequence.
- **Eingabe:** Das Fenster meldet Maus und Modifier; `C:EC64Input` setzt das in Amiga-Rohtasten und
  Zeigerereignisse für input.device um.
- **Ton:** Ein Audio-Ring der Platine ($500, 48 kHz Stereo) geht über CoreAudio. qrx-win32 mischt
  DirectSound und waveOut selbst (`win32/sound_ec64.c`, ganzzahlig, die EC64 hat keine FPU).
  `wavetest.exe` ergibt 1,0 s Ton bei 439 Hz.
- **Netz:** `QRXHostSocket` (bsdsocket.library über HOST_CALL Dienst 4) wird beim Start geladen.
- **3D:** HOST_CALL Dienst 8 ist die GL-Tür (`qrx_gl68k.c`), mit einer Sperre für mehrere Kerne.
  `C:EC64GLDemo` zeigt einen beleuchteten Würfel, gerendert über Metal ("Apple M1, 2.1 Metal").
- **Mac:** QRX68KRun, QRXAppRun, QRXMach, QRXMachTest und QRXPPC (`tools/qrxppc/build64.sh`) liegen
  in C:.

Gefundene und behobene Fehler: Ohne Chip-RAM blockierte `AllocBitMap` für planare Bitmaps in einer
Spinlock-Selbstblockade; der Kernel markiert jetzt allen RAM zusätzlich als `MEMF_CHIP`. Kickstart-Module
mit `stdlib.library` (auf der Disk) starteten beim Kaltstart still nicht; sie linken jetzt `stdc.static`.
Ohne Skript stand die Uhr von qrx-win32, sobald ein Programm nur schlief; ohne `-s` gilt jetzt die
Echtzeit.

Offen:

- Der JIT-Fehler beim Fflush-Test (Abbruch bei 847872 Bytes) besteht weiter. Er tritt ohne JIT und
  mit `QRX_EC64_JIT_OFF=1` nicht auf, wohl aber mit `0x2` oder `0x20` allein. Der Zufallstest
  (200000 Programme) findet ihn nicht.
- qrx-win32 läuft auf 64 Bit noch ohne eigenes Fenster (headless). Der Ton und die Uhr funktionieren.
  Als Nächstes kommt `backend_aros.c` auf cybergraphics.
- `asl.library` braucht die Katalog-Erzeugung, zlib die `zconf.h`. Der Kernel-Test von ec64-llvm hängt
  etwa einmal in 300 Läufen in seinem eigenen Konsolen-Lock, auch ohne JIT.

## Stand 2026-11-17 (Nachmittag): Desktop-Shell, echtes SMP, Eingabe

- **SMP verteilt jetzt:** Neue Tasks durften bisher nur auf dem erzeugenden Kern laufen (AROS-Standard),
  also lief fast alles auf Kern 0. Auf der EC64 bekommen sie alle Kerne. Zwei Rechenschleifen brauchen
  zusammen 40 s statt 78 s.
- **Konsole verhungerte das System:** econsole wartete mit `Reschedule()` auf Tasten, über der Priorität
  normaler Programme. Solange eine Shell auf Eingabe wartete, liefen QRXDesktop und qrx-win32 nicht.
  Jetzt wartet sie 10 ms über timer.device.
- **Leerlauf messbar:** Der Leerlauf-Task schreibt das Platinen-Register IDLE ($318). Die Platine zählt daraus
  die Leerlaufzeit je Kern ($320+8n), dazu Kernzahl ($31C), RAM ($360), Laufzeit ($368) und Host-CPU ($380).
  Schlafen im Leerlauf (`QRX_EC64_IDLE_SLEEP=1`) spart Mac-CPU, zeigte aber Hänger und einmal eine
  Ausnahme in `core_Switch`. Es bleibt deshalb aus.
- **QRXInfo:** „Über QRX OS One“ mit OS, Prozessor, Host, Speicher, Display, Ton, Netz, 3D, Laufzeit
  und live der Last je Kern. `QRXInfo TEXT` gibt dasselbe in der Shell aus. Gibt es auch für Classic.
- **Desktop-Shell** (`tools/armithlon/desktop/qrx_shell.c`, über `build-desktop.py`):
  - Taskleiste im Stil von polybar: QRX-Home-Knopf, Übersicht-Knopf, ein Knopf je Fenster mit
    Live-Vorschau beim Zeigen, Module für CPU je Kern, RAM, Netz und Uhr. Sie kommt nach vorn, wenn der
    Zeiger den unteren Rand berührt.
  - Home-Menü wie in Windows 11: angeheftete Apps (nur vorhandene), Suche durch Tippen, Ein/Aus mit
    Herunterfahren und Neustart.
  - Übersicht wie Exposé: über den Knopf oder die Ecke oben links. Ein Klick holt das Fenster nach vorn.
  - Hintergrund per Rechtsklick auf ein Bild im QRX Drive. PNG, BMP und PPM liest die Shell selbst,
    JPEG, GIF und ILBM liest sie über Datatypes, wo es sie gibt (Classic).
  - Der Desktop ist jetzt immer ein Backdrop-Fenster, er verdeckt keine Programme mehr.
  - `C:OpenWB` öffnet den Workbench-Bildschirm vor QRXDesktop.
- **qrx-win32 auf 64 Bit mit Fenster** (AROS-Backend über cybergraphics), Ton auch dort.
- **Eingabe (Classic und 64 Bit):**
  - Solange ein ARMithlon-Fenster aktiv ist, sind die globalen Kurzbefehle von macOS aus. Ctrl+Pfeil öffnete
    Mission Control bzw. App-Exposé.
  - Cmd-Kombinationen und ihr keyUp gehen an den Gast, Cmd+Q bleibt beim Mac.
  - qrx-win32: DirectInput liefert eine echte Ereignis-Warteschlange; direkte Tastenabfragen holen
    wartende Eingaben vorher ab; ein Loslassen geht nicht mehr verloren.
- **JIT:** Der Prüfmodus `QRX_EC64_JIT_VERIFY=1` vergleicht jeden Block mit dem Interpreter. Damit lief
  Fflush bitgenau durch, ohne einen abweichenden Block. Der Fehler liegt also zwischen den Blöcken
  (Verkettung, Budget, Interrupt-Zeitpunkt), nicht in der Übersetzung.

Offen: der seltene Hänger mit mehreren Kernen (etwa einer von acht Läufen, auch ohne Schlafen im
Leerlauf), der JIT-Fehler zwischen den Blöcken, asl.library und zlib.

## Reihenfolge

1. **Classic:** Das 32-Bit-System als eigene Instanz in der 64-Bit-Maschine starten, mit gemeinsamem HostFS und Fenster auf dem Mac. Setzt voraus, dass der EC64 im Emulator läuft. Das ist Sache der AROS-64-Arbeit auf `claude/tor-browser-aros68k-343c56`.
2. **Vorarbeit:**
   - Freddi im 64-Bit-Mac-Build klären.
   - Die Host-Tests als 64-Bit-Lauf in die Regressionsprüfung aufnehmen.
3. **Übersetzen:** qrx-win32 und den QRX86-Core mit `m68k64-clang` übersetzen, sobald die C-Bibliothek des 64-Bit-AROS steht. Zuerst ohne JIT und headless auf der EC64-Referenzmaschine.
4. **Long-Mode-JIT** für QRX86. QRXPPC folgt in Absprache.
5. **Backend:** CyberGraphics und AHI auf AROS 64. Danach läuft die Spieleliste nativ und wird mit der Survey gegen Classic verglichen.
