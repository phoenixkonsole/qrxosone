# ARMITHLON 1.2 – QRX OS One GitHub Stand 31.10.2026

Dieses kumulative Entwicklerpaket baut auf ARMITHLON 1.1 auf und **verwendet die neuere GitHub-Entwicklung einschließlich des eingebetteten Linux/X11-Kompatibilitätsservers**. Es enthält nicht das gesamte private QRX-Repository; die zusätzlichen QRX-Dateien werden mit deiner bestehenden GitHub-Anmeldung von Commit `cc2ba06a` geholt und lokal kompiliert. Nichts wird zurück nach GitHub geschrieben.

## Was du am Apple M1 ausführst

Einmalig:

```bash
xcode-select --install
brew install cmake gh xz
cd ~/Downloads/ARMITHLON-1.2-macOS
./armithlon build
./armithlon test
./armithlon x11-bridge-test
```

GitHub-Anmeldung (privates Repo):

```bash
gh auth login
./armithlon fetch
./armithlon build-qrx86-linux-host
```

Im ersten Terminal das native macOS-Metal-Fenster öffnen:

```bash
./armithlon run-native-qrx86
```

Im zweiten Terminal die mitgelieferte **Linux i386 X11-Demo** starten:

```bash
./armithlon run-linux-x11 "$PWD/benchmarks/linux-i386/qrx-x11-preview.elf"
```

Die Demo verbindet sich nicht mit XQuartz: QRX86 stellt die lokale X11-Teilmenge selbst bereit. Wenn sie funktioniert, erscheint ein farbiges Rechteckmuster im Metal-Fenster, der Gastprozess beendet sich mit Status 0. Die Umsetzung bietet zurzeit einen einfachen Pixel-Framebuffer, keine native GLX-Beschleunigung.

**Wichtig:** Der M1-Build der aktuellen QRX86/Linux-Laufzeit ist noch nicht verifiziert. Falls der Compiler einen Fehler meldet, diesen samt `uname -m` und `clang --version` aufbewahren; dies ist ein Entwicklungs-Build, keine garantierte App-Kompatibilität.

## Windows-Programme und Benchmarks

Bisherige Befehle bleiben erhalten:

```bash
./armithlon build-qrx86-host
./armithlon run-native-qrx86
# In einem anderen Terminal:
./armithlon run-qrx86 /Pfad/zum/Programm.exe
./armithlon benchmark-all 120
./armithlon benchmark-geekbench
```

Geekbench 7 ist proprietär und wird **nicht** im ZIP mitgeliefert. glmark2 und vkmark werden als Linux-Open-Source-Benchmarks bei Bedarf installiert; ein native M1-Build dieser Projekte wird nicht behauptet.

## Dokumentation und Grenzen

Siehe `CHANGELOG-1.2.md`, `docs/QRX_GITHUB_ALIGNMENT_1.2.md` und `engine/include/qrx_x11_bridge.h`. Der normale ARMITHLON-68k-VM-Kern bootet weiterhin **kein** vollständiges QRX OS One; die AROS-Kompatibilität läuft separat über FS-UAE.
