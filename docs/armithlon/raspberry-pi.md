# ARMithlon auf dem Raspberry Pi

Stand 2026-11-20. Ziel: dieselbe Engine wie auf dem M1 (`upstream/armithlon/2.6/engine`) auf
einem Raspberry Pi 4 oder 5 mit 64-Bit-Raspberry-Pi-OS.

## Was geprüft ist und was nicht

Geprüft wurde ein **Cross-Build** auf x86-64-Linux. Die Tests liefen unter `qemu-aarch64` mit den
CPU-Modellen der beiden Pis:

- `cortex-a72`: Pi 4, ARMv8.0
- `cortex-a76`: Pi 5, ARMv8.2

Auf echter Pi-Hardware lief noch nichts. Das betrifft Grafiktreiber, Ton, Bildrate und
Geschwindigkeit.

| Prüfung | Pi-4-Modell | Pi-5-Modell |
| --- | --- | --- |
| Build der ganzen Engine (Musashi 68040, 68100-JIT, 68100EC64, QRX86-Door, GL-Door) | ja | ja |
| 68k-JIT gegen Musashi (`jit_a64_differential_against_musashi`, 4000 Vergleiche) | bestanden | bestanden |
| Zweiter 68k-Kern, Atomics und Fehler (`core1_…`) | bestanden | bestanden |
| QRX86-Host-JIT gegen den Interpreter | bestanden | bestanden |
| EC64-JIT, EC64 gegen Musashi 68040, EC64 Long Mode mit zwei Kernen | bestanden | bestanden |
| 20 Python-Integrationstests (echte Musashi-Boots, HUNK-Programme, Intuition-HLE, GUI-Snapshots) über `tools/armithlon/pi/ctest-qemu-python.sh` | 20/20 | 20/20 |
| Neues SDL-Fenster ohne Bildschirm (`linux_sdl_window_rtg_formats_keys_mouse`) | bestanden | bestanden |

Ohne den qemu-Wrapper melden zehn der Python-Tests einen Fehler. Sie starten die
ARM64-Programme dann direkt, und das geht auf einem x86-Rechner nicht. Auf dem Pi selbst läuft
einfach `ctest`.

## Was für den Pi geändert wurde

- **68k-JIT (`full68k/qrx68100_jit.c`)**
  - Er nutzte zwei Befehle, die der M1 hat, die Pis aber nicht:
    - `CFINV`, FEAT_FlagM, ARMv8.4: fehlt auf Pi 4 und Pi 5.
    - `LDAPR`, FEAT_LRCPC, ARMv8.3: fehlt nur auf dem Pi 4.
  - Unter beiden CPU-Modellen brach der JIT deshalb mit „Illegal instruction“ ab.
  - Jetzt prüft er beim Start `getauxval(AT_HWCAP)`. Fehlt ein Befehl, erzeugt er
    ARMv8.0-Ersatz:
    - für `CFINV`: `mrs/eor/msr NZCV` über x18, das unter Linux frei ist;
    - für `LDAPR`: `LDAR`.
  - Auf dem Mac bleibt der Code gleich.
  - `QRX_JIT_ARMV80=1` erzwingt den Ersatz zum Testen.
- **GL-Door (`full68k/qrx_gl68k.c`)**
  - Unter Linux läuft sie über EGL ohne Fenster (Mesa V3D), auf dem Mac weiter über CGL.
  - Die erzeugten Funktionszeiger kommen über `QGL_PROC`. `tools/qrxgl68k/gen.py` erzeugt das
    jetzt so.
- **Host-GPU für Direct3D/Glide (`linux/qrx_hostgpu_gl.c`)**
  - Neu: OpenGL 2.1 auf EGL statt Metal.
  - Zeilenfolge, Tiefenbereich und Viewport entsprechen dem Metal-Weg.
  - Fertige Bilder gehen über einen Puffer an das Fenster.
- **Fenster (`linux/qrx_aros_window_sdl.c`)**
  - Neu: SDL2 statt Cocoa/Metal, für die 32-Bit-QRX-OS-One-Maschine (`--window`).
  - SDL wählt X11/Wayland auf dem Desktop oder KMS/DRM auf der Konsole.
  - Inhalt:
    - RTG-Formate CLUT8, RGB565, RGB555, RGB24 und ARGB32;
    - Tasten als Amiga-Rawkeys (die Windows-Tasten sind die Amiga-Tasten);
    - Maus;
    - AHI-Ton über SDL-Audio;
    - Bilder von Host-API-Programmen;
    - dieselben `QRX_TEST_*`- und `QRX_RTG_CAPTURE`-Hooks wie auf dem Mac.
  - `QRX_FULLSCREEN=1` startet im Vollbild.
  - Das zweite Laufwerk APPS: ist `QRX_APPS_DIR`, sonst `~/.local/share/armithlon/Apps`.
- **Übrige Mac-only-Stellen**
  - `qrx86_door.c`: `mach_absolute_time` ersetzt durch `clock_gettime`.
  - `qrx86_hostjit.c`: der Profiler (`QRX86_PROF`) tastet unter Linux per `SIGPROF` ab.
  - `qrx_ec64_board.c`: Benutzer-Tresor und GL-Dienst auch unter Linux; der Board-Name kommt aus
    `/proc/device-tree/model`.
  - Ein Test liest den Signal-Kontext jetzt plattformgerecht.
- **CMake**
  - Musashis Generator läuft bei Cross-Builds über den Emulator.
  - qmath bekommt seine eigenen Header (wie `build-qmath.sh`).
  - Unter Linux werden EGL, GL und SDL2 eingebunden, OpenSSL ≥ 3.5 kommt aus dem System.
  - Ohne OpenSSL wird der Tresor-Test nicht registriert.
- **Fehler, der auf allen Plattformen bestand (`full68k/full_main.c`)**
  - Der RTG-Selbsttest (`QRX_FULL_FSAGA_TEST`) sprang per `goto done` über die Initialisierung
    von `rom`.
  - Beim Aufräumen wurde dann ein zufälliger Zeiger freigegeben. Unter Linux brach das Programm
    sofort ab.

## Bauen

Auf dem Pi (64-Bit-Raspberry-Pi-OS):

```sh
sudo apt install cmake gcc python3 git libsdl2-dev libegl-dev libgl-dev libssl-dev
bash tools/armithlon/pi/build-pi.sh
```

Für Benutzerkonten mit verschlüsselten Homes braucht der Tresor OpenSSL 3.5 oder neuer.
Raspberry Pi OS 13 „Trixie“ hat es, „Bookworm“ hat 3.0. Ohne OpenSSL 3.5 läuft alles andere,
nur ohne Benutzer.

Cross-Build auf x86-64-Ubuntu:

- Voraussetzungen:
  - `gcc-aarch64-linux-gnu` und `qemu-user`;
  - arm64-Multiarch mit `libsdl2-dev:arm64`, `libegl-dev:arm64` und `libgl-dev:arm64`.
- Aufruf:

```sh
bash tools/armithlon/pi/build-pi.sh --cross        # QRX_PI_CPU=cortex-a76 für das Pi-5-Modell
```

## Noch offen

- **QRX OS One 64 (68100EC64) hat unter Linux noch kein Fenster.**
  - `qrx_ec64_run` läuft dort ohne Bildschirm.
  - Ein SDL-Gegenstück zu `macos/qrx_ec64_window.m` fehlt noch.
- **Der beschleunigte QRX3D-Strom** (auf dem Mac ein Metal-Renderer) ist unter Linux nicht
  angeschlossen. Programme bekommen dann den Software-Weg.
- **MIDI-Ausgabe** (auf dem Mac der Systemsynthesizer) ist unter Linux noch stumm.
- **Bundle mit ROM und System-Volume:**
  - Ein startfertiges Paket wie `ARMithlon.app` mit gepatchtem ROM und `SystemFull` gibt es für
    Linux noch nicht.
  - Das System-Volume wird auf dem Mac lokal gebaut und liegt nicht im Repository.
- **Echte Hardware:**
  - Bildrate, JIT-Geschwindigkeit und das Verhalten von Mesa V3D sind ungemessen.
  - Dasselbe gilt für die Spiegelung der Host-GPU (Front-Face-Winding) und den
    Tastatur-Fokus auf der Konsole.
  - Der Pi 5 läuft standardmäßig mit 16-KiB-Seiten. Die Engine fragt die Seitengröße zur
    Laufzeit ab; auf der Hardware geprüft ist das noch nicht.
- **Mac-Build nicht neu geprüft:** Die macOS-Zweige wurden nicht verändert, aber gemeinsame
  Dateien schon (CMake, GL-Door, `full_main.c`). Er muss auf dem M1 einmal neu gebaut und
  getestet werden.
