# ARMITHLON 2.6 – QRX68: praktische AmigaOS-3.x-Kompatibilität erweitern

## Architekturentscheidung

Das Ziel ist **QRXDesktop + AROS68k + QRX68-HLE/68040**, nicht ein zweiter Desktop oder ein unverifiziertes „AmigaOS läuft bereits“. AROS-kompatible HUNK-Anwendungen laufen weiterhin nativ im AROS-Gast; Anwendungen, die QRX68 benötigen, werden via `DefaultTool=C:QRX68Run` an den vorhandenen QRX68/2-Hostdienst übergeben. Dieser nutzt noch die kontrollierte Host-Verzeichnis-Bridge. Eine echte AROS-Integration bleibt separat zu validieren.

## Neu in 2.6

- Exec-Task: echte Gastadresse mit den klassischen Signal-Feldern `tc_SigAlloc`, `tc_SigWait`, `tc_SigRecvd`. Der Task-Speicher liegt **oberhalb der HUNK- und Heap-Region**. `Wait` unterbricht die Ausführung, statt Busy-Waiting oder einen vorgetäuschten Erfolg zu verwenden.
- `CreateMsgPort`, `PutMsg`, `GetMsg`, `ReplyMsg`, `WaitPort` und korrespondierende Signale; Port-Datenstrukturen nach AmigaOS-32-Bit-ABI. Noch keine Multitasking-Exec-Kernelintegration, kein allgemeines Message-Passing zwischen QRX68-Prozessen.
- `OpenWindowTagList` (`WA_Left`, `WA_Top`, `WA_Width`, `WA_Height`, `WA_IDCMP`, `WA_Flags`, `WA_Gadgets` und ausgewählte weitere); unbekannte/ungültige Tags führen zu einem Fehler.
- `AddGadget`, `OnGadget`, `OffGadget`, `RefreshGadgets`/`RefreshGList`: einfache rechteckige Gadget-Darstellung und IDCMP-GADGETDOWN/-GADGETUP mit korrekt gesetztem `IAddress` bei Mausklicks. Kein BOOPSI, kein GadTools und keine komplexen System-Gadgets.
- `SetMenuStrip`, `ClearMenuStrip`, `IDCMP_MENUPICK`: einfache, einlagige Menüs über rechte Maustaste (MENU DOWN/UP). **Nur primitive geometrische Menüs**, keine echten Submenüs, Menünamen-Text oder Dynamik wie im originalen Intuition.
- Layerartige Fensterkomposition: getrennte Pixeloberflächen, Z-Ordnung, MoveWindow/ChangeWindowBox, WindowToFront/Back, Refresh und Close. **Nicht identisch mit `layers.library`** und nicht mit echten Amiga BitMap-/Blitter-Datenstrukturen.
- AmigaDOS-Prozessmetadaten (`GetProgramName`, `SetProgramName`, `GetCurrentDirName`, `SetCurrentDirName`, `GetArgStr`, `SetArgStr`, `Cli`, `SetIoErr`, `Exit`). Keine echten Kindprozesse; `CreateProc` und `CreateNewProc` bleiben nicht unterstützt.

## Auf dem Mac starten

```bash
cd ~/Downloads/ARMITHLON-2.6-macOS
./armithlon build
./armithlon test
./armithlon qrx68-compat-test
./armithlon qrx68-live-test
```

Die Tests sind portable Linux/macOS-Tests des **68k-Interpreter-/Host-Brückenpfads**. Sie bestätigen **keinen** echten ARM64/Musashi- oder AROS-Desktop-Boot.

Für den realen Musashi-68040-Pfad wird weiterhin der extern gepinnte Quellcode benötigt:

```bash
./armithlon full68k-setup
./armithlon full68k-build
./armithlon full68k-test
```

Die AROS-seitigen Starter sind weiterhin im Paket (`engine/guest/aros/qrx68/QRX68Run.c`), benötigen aber einen Cross-Compiler:

```bash
export QRX_AROS_CC=/PFAD/ZUM/m68k-aros-gcc
./armithlon qrx68-aros-build
./armithlon qrx68-aros-install /PFAD/ZUM/AROS-SYSTEM
./armithlon qrx68-aros-start /PFAD/ZUM/AROS-SYSTEM
```

Im vorhandenen QRXDesktop bei geeigneten HUNK-Programmen `DefaultTool=C:QRX68Run` setzen. Komplexe Anwendungen scheitern derzeit an nicht implementierten APIs; bitte die Fehlermeldung mit `failed_lib`, `failed_lvo` aufheben und nicht als erfolgreichen Start werten.

## Was für Directory Opus, Final Writer etc. weiterhin nötig ist

Eine systematische Kompatibilitätsmatrix mit **legitim vorliegenden echten Programmbinaries** ist notwendig. Funktionen wie Text-/Font-Rendering, GadTools, BOOPSI, vollständige `layers.library`/Intuition, Dateisperren und `Examine/ExNext`, DOS-Pakete/Process-/CLI-Basis, `LoadSeg/CreateProc`, Workbench-/Icon-ABI, ASL-Requester, Scheduler und Speicherschutz sind keine Kleinigkeiten. Echte `Directory Opus`- oder `Final Writer`-Binaries wurden nicht bereitgestellt oder getestet. **Diese Version garantiert keine Ausführung beider Programme.**

AROS m68k besitzt selbst hohe API-Kompatibilität, daher soll QRX68 nur dort mit einer isolierten API-Schicht eingreifen, wo AROS abweicht oder LVO-/ABI-Bedürfnisse dies erfordern. Unbekannte API-Funktionen dürfen nicht als erfolgreich zurückkehren.

### Quellen für ABI-Abgleich

- AmigaOS 3.x `exec.library` LVO-Tabelle: https://arisamiga.github.io/amiga-bootcamp/14_references/exec_lvo_table/
- Klassische LVO-Tabellen inklusive Intuition und DOS: https://github.com/jotd666/amiga68ktools/blob/master/tools/LVOs.i
- Offizielle Task-/Signal-Semantik: https://wiki.amigaos.net/wiki/Exec_Signals

### Status der Tests

Die einzelnen Nachweise stehen in `VERIFICATION-2.6.txt`. ASan/UBSan und normale Tests wurden unter Linux x86-64 ausgeführt. Mangels Cross-SDK und FS-UAE/Apple-M1-Host wurden keine echten AROS-Doppelklick- oder Musashi/FPU-Gastbenchmarks durchgeführt.
