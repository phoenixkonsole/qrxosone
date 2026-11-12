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

## Reihenfolge

1. **Classic:** Das 32-Bit-System als eigene Instanz in der 64-Bit-Maschine starten, mit gemeinsamem HostFS und Fenster auf dem Mac. Setzt voraus, dass der EC64 im Emulator läuft. Das ist Sache der AROS-64-Arbeit auf `claude/tor-browser-aros68k-343c56`.
2. **Vorarbeit:**
   - Freddi im 64-Bit-Mac-Build klären.
   - Die Host-Tests als 64-Bit-Lauf in die Regressionsprüfung aufnehmen.
3. **Übersetzen:** qrx-win32 und den QRX86-Core mit `m68k64-clang` übersetzen, sobald die C-Bibliothek des 64-Bit-AROS steht. Zuerst ohne JIT und headless auf der EC64-Referenzmaschine.
4. **Long-Mode-JIT** für QRX86. QRXPPC folgt in Absprache.
5. **Backend:** CyberGraphics und AHI auf AROS 64. Danach läuft die Spieleliste nativ und wird mit der Survey gegen Classic verglichen.
