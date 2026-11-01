# ARMITHLON 2.3 — QRX68 Musashi CPU + AmigaOS 3.x GUI-HLE-Vorbereitung

## Zweck

Der bisherige `qrx68-run` kann nur eine sehr kleine 68000-Instruktionsmenge.
**Neu ist ein tatsächlich implementierter QRX68-Musashi-Ausführungsadapter**:
Amiga-HUNK-Laden, 68040-Maschinencode, FPU/CPU-Exceptions (Musashi),
Exec-/DOS-LVO-HLE, Intuition-/Graphics-Teilmenge und Renderer teilen sich
jeweils den **gleichen QRX68-Gastspeicher und HLE-Dispatcher**.

Musashi ist aus Lizenz-/Provenienz- und Umfangsgründen NICHT im ZIP. Die
geprüfte Version wird von `full68k-setup` anhand Git-Commit
`313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd` extern bezogen.
**Ohne diesen externen Source kann die vollständige CPU-ISA NICHT gebaut
oder als auf dem Zielsystem nachgewiesen bezeichnet werden.** Der alte
subset-Interpreter bleibt als tatsächlich offline getesteter Fallback erhalten.

## Schnellstart — Apple Silicon

```bash
xcode-select --install
brew install cmake git
cd ~/Downloads/ARMITHLON-2.3-macOS
./armithlon build
./armithlon test
./armithlon qrx68-demo
./armithlon qrx68-gui-demo
open qrx68-screen.ppm
```

Echte Musashi-Integration auf einem internetfähigen Mac:

```bash
./armithlon full68k-setup
./armithlon full68k-build
./armithlon full68k-test
./armithlon qrx68-musashi --ram 256 data/qrx68/QRX68-Hello.hunk
./armithlon qrx68-musashi --ram auto --experimental-gui \
  --snapshot qrx68-full.ppm data/qrx68/QRX68-GUI.hunk
```

Nach erfolgreichem `full68k-build` nutzt `./armithlon qrx68 ...`
automatisch die vollständige Musashi-Laufzeit im separaten Build-Verzeichnis.
`--ram auto` reserviert höchstens ein Viertel des effektiven Host-RAM,
auf den 32-Bit-Gast-Adressraum begrenzt (max. 3.840 MiB). Gast-RAM wird
sparse gemappt. Mit `--ram MIB` kann explizit ein anderer Wert gewählt werden.

**Bekannte, überprüfte Original-Gastprogramme**:
`data/qrx68/QRX68-Hello.hunk`, `data/qrx68/QRX68-GUI.hunk`.
Der Quellgenerator für die Grafikdemo ist `tools/build-qrx68-gui-demo.py`.

## QRX68 Architektur 2.3

```
Amiga HUNK (bis 32 MiB Code+Data+BSS, 16 Segmente)
       | HUNK Loader: RELOC32, RELOC32SHORT, CODE/DATA/BSS
       v
68k-CPU:       echter Musashi 68040, WENN extern installiert
               sonst: alter PORTABLER TEILINTERPRETER
       | JS(R) -LVO(A6) klassischer Amiga Library-Vektor
       v
Exec/DOS HLE:  OpenLibrary, AllocMem, Open/Read/Write u.a.
       | opt-in `--experimental-gui`
       v
Intuition-HLE: OpenWindow, CloseWindow — nur ohne IDCMP
Graphics-HLE:  SetAPen, RectFill, Move, Draw, WritePixel, ReadPixel
       |             \
       v              v
  RGB-PPM-Datei       Q86F-Framebuffer-IPC --> vorhandene Metal-GUI
```

**Keine Fälschung von API-Erfolgen:** nicht implementierte LVOs schlagen
fehl. Das experimentelle Intuition-HLE unterstützt **keine** IDCMP-Events,
Gadgets, Menüs, Screens, Layer und keine Workbench-Startnachrichten. 
`OpenLibrary("graphics.library")` und `OpenLibrary("intuition.library")`
sind deshalb nur mit `--experimental-gui` verfügbar.

`QRX68-GUI.hunk` wurde unter dem Offline-Teilinterpreter tatsächlich
gebootet und zeichnet ein blaues Rechteck in ein 640×256-Framebuffer.
Das Q86F-Protokoll ist identisch zum vorhandenen QRX86-Metal-GPU-Socket.
Wer den auf dem Mac vorhandenen Metal-Renderer zeigen möchte:

Terminal 1:
```bash
./armithlon run-native-qrx86
```
Terminal 2:
```bash
export ARMITHLON_QRX86_SOCKET="${TMPDIR:-/tmp}/armithlon-qrx86-$(id -u).sock"
./armithlon qrx68-gui-demo
```
Das ist ein statischer Frame, keine vollständig interaktive Amiga-GUI.
Das GPU-Paket wurde unter Linux per Unix-Socket byteweise geprüft;
ein echter Metal-GPU-M1-Renderlauf ist noch nicht nachgewiesen.

## QRXDesktop unverändert lassen, optional erweitern

QRXDesktop 6.6.20 existiert bereits in `phoenixkonsole/qrxosone`.
Der neue geschützte Source-Patch kann die vorhandene Oberfläche erweitern:

```bash
gh auth login
./armithlon fetch
./armithlon qrxdesktop-qrx68-patch
```

Er erzeugt `data/qrxdesktop/qrx_desktop-2.3.c` aus dem **gepinnnten**
Original-Source, unter Beibehaltung von Dateimanager, Doppelklick und
QRX86-Startregeln. Nur Amiga-HUNKs mit **explizitem Icon-ToolType
`QRXMODE=QRX68`** sollen an `QRX:C/QRX68Run` übergeben werden; native
AROS-HUNK-Ausführung bleibt ansonsten Standard. Der Patch prüft, ob der
AROS-Gast-Launcher überhaupt installiert ist und verweigert bei Fehlen
sicher den Start. Das **AROS-Gastprogramm QRX68Run und dessen IPC zur
Host-Laufzeit sind NOCH NICHT vorhanden**; der Source-Patch allein ist
also keine funktionierende Doppelklick-Ausführung unter nativem AROS.
Der GitHub-Branch bleibt unverändert.

## Sicherheit und Grenzen

- Noch kein vollständiges AmigaOS-3.x oder AROS Exec/intuition-DOS.
- Musashi CPU und FPU kommen aus gepinntem Third-Party-Source; ohne ihn
  nur der portable Teilinterpreter. Apple-M1-/AROS-Hardwaretests ausstehend.
- Realer 68040-CPU-Exception/RTE-Test ist **bei vorhandenem Musashi** als
  CTest registriert; er konnte hier wegen fehlendem Third-Party-Source
  nicht ausgeführt werden. Er darf nicht als PASS ausgegeben werden.
- Keine von AROS unabhängige interaktive Workbench, keine layers.library,
  icon.library oder workbench.library HLE. Diese API-Familien sind offen.
- Keine JIT-Blöcke für beliebige 68040-Instruktionen oder PPC/x86 auf ARM64;
  Musashi ist hier ein CPU-Interpreter. Bisherige kleine ARM64-JIT-Loops
  sind eine getrennte Diagnose-Teilimplementierung.
- Kein nativer AROS-Board-Boot oder AROS-Exec-SMP-Nachweis.
- Keine direkte Chipset-/WHDLoad-Kompatibilität ohne zusätzliche Hardwareemulation.
- Gast-Dateizugriff nur auf separat freigegebenes Verzeichnis (`--share`),
  standardmäßig read-only, `--share-rw` bewusst opt-in. Keine Symlinks.
- DOS-/Exec-HLE kann ggf. andere nicht unterstützte Programme nicht starten.
- Es gibt keinen DRM- oder kommerziellen Original-ROM-Code im Paket.

## Verifikation

Auf dieser Maschine (Linux x86_64): **47/47 CTests**, mit echten HUNK-
Ausführungen der portablen Teilmenge, grafischem Framebuffer, realen
Q86F-Unix-Sockets und großem HUNK-/Heap-Grenztest. Zusätzlich wurde die
Musashi-Adapter-API **nur gegen einen nichtausführenden Teststub** kompiliert
und gelinkt. Ohne Musashi bedeutet dies KEIN bestandener 68040/FPU-Test.
Siehe `VERIFICATION-2.3.txt`.

## Referenzen

- Externes Musashi-CPU-Projekt: https://github.com/kstenerud/Musashi
- Entwicklerdokumentation AmigaOS 3.x: https://developer.amigaos3.net/autodocs/
- Vorhandener QRXDesktop 6.6.20: GitHub `phoenixkonsole/qrxosone`,
  Branch `claude/qrx-22.5.11`, Commit `cc2ba06a0cc76a87bdd49edcd4b475df4227e716`.
