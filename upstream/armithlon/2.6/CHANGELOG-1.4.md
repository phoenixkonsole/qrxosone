# ARMITHLON 1.4 – QRX68100 full-instruction core foundation

## Implementiert

- **Zweiter 68k-Backend-Pfad:** Anbindung des etablierten, MIT-lizenzierten Musashi 4.60 CPU-Kerns für 68000/68010/68020/68030/68040. Der bisherige QRX68100-Toy-Interpreter/JIT bleibt erhalten und wird nicht stillschweigend ersetzt.
- **Deterministisch gepinnte Quelle:** `kstenerud/Musashi`, Commit `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd` vom 08.03.2026, wird opt-in mit `./armithlon full68k-setup` lokal geklont. Der ZIP enthält ausschließlich eigenen Integrationscode, nicht das noch nicht heruntergeladene Musashi-Projekt.
- **CMake Build:** generiert `m68kops.c/.h` aus `m68k_in.c` über `m68kmake`, baut CPU, softfloat und QRXBoard glue. Kein vorgetäuschter Build, wenn Third-Party-Quelle nicht vorhanden ist.
- **Busmodell:** Big-Endian, RAM, separate unveränderbare Haupt-/Ext-ROM-Bereiche und Reset-Vektorspiegel; CPU-seitige MMIO Weiterleitung an UART, Grafik, Audio und Input des bisherigen virtuellen Boards.
- **Neue 32-Bit-Board-Adresse:** `0xFF000000–0xFF0007FF` als künftiger disjunkter Portbereich; die alte `0x00FF0000`-Adresse kollidiert mit F80000-Kickstart-ROM und bleibt nur für bisherige ohne ROM laufende Monitore erhalten.
- **IRQ-/Timer-Controller:** level-sensitive IRQ 1–7, Autovektoren, Timer mit periodischer oder einmaliger Auslösung, Pending/ACK-Register und per MMIO deaktivierbarem ROM-Reset-Overlay.
- **CPU-/MMIO-Integrations-Tests:** UART-Monitor, MULU.W/DBRA-Loop, TRAP/RTE, ROM-Protektion/Vektoren, Timer-IRQ. Nur ausführbar, **wenn Musashi heruntergeladen und tatsächlich gebaut wurde**.
- **Host-Laufzeit:** `qrx68100_full_run`, Optionen `--cpu`, `--cycles`, `--rom`, `--ext-rom`. ROM-Probe liefert absichtlich keinen erfolgreichen Desktop-Bootstatus.

## Verifiziert in dieser Umgebung

- 22/22 ARMITHLON CTests auf Linux x86_64: PASS (inklusive neuem **CPU-Mock**-Bus-Test).
- Neue C-Dateien per C11/Wextra/Wpedantic/Werror gegen einen *nur syntaktischen* externen API-Stub geprüft: PASS.
- Bash-Launcher-Syntax und Archivintegrität: PASS.
- 22/22 mit AddressSanitizer und UndefinedBehaviorSanitizer: PASS.
- Echte Musashi-Binärkompilierung/Instruktions-Tests: **nicht ausgeführt**, weil hier kein Netzwerkzugriff auf den nicht vorhandenen Third-Party-Quellcode besteht.
- Native macOS M1/Metal und IRQ-Laufzeit: **nicht ausgeführt**.

## Weiterhin offen – nicht als erledigt ausweisen

- Native 68060-CPU, vollständige 68040-MMU/FPU-Testabdeckung und CPU-genaues Timing; Musashi erfüllt nicht automatisch eine Motorola-68060-Zertifizierung.
- Einordnung und Emulation der Amiga Custom-Chips (Paula, CIA, Agnus, Denise), Interruptprioritäten, Reset-Mapping und AROS HIDD/RTG/AHI/Input-BSP für qrxvirt.
- Boot eines echten QRX-OS-One-LiveCD oder Workbench unter unserem eigenen 68100. CPU-Kern + ROM-Probe alleine reichen hierfür **nicht**.
- Eine beschleunigte, differenziell getestete JIT-Implementierung für den neuen CPU-Kern. Die bestehende ARM64-JIT-Teilimplementierung wurde nicht ohne ISA-Nachweis auf Musashi umgelegt.

Quellen-/Lizenzinfo: https://github.com/kstenerud/Musashi (MIT-style Lizenztext in den Musashi-Quelldateien). Keine ungeprüften Leistungszahlen.
