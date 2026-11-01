# ARMITHLON 2.6 / QRX68 Workbench Compatibility Foundation

Der Stand basiert kumulativ auf ARMITHLON 2.5. Keine AROS/GitHub-Dateien wurden überschrieben; die QRXDesktop-6.6.20-HUNK-Route und die QRX68/2-Bridge bleiben unverändert.

## Konkrete Implementierung

- Klassische 680x0-Exec-LVOs nach AmigaOS-3.x-Schnittstellen: `FindTask -294`, `Forbid -120`, `Permit -126`, `Disable -132`, `Enable -138`, `SetSignal -306`, `Wait -318` (blockiert per kooperativem Host-Yield), `Signal -324`, `AllocSignal -330`, `FreeSignal -336`, `PutMsg -366`, `CreateMsgPort -666`, `DeleteMsgPort -672`. Bereits vorhandene `GetMsg -372`, `ReplyMsg -378`, `WaitPort -384` verwenden echte gebundene Ports statt ungeprüfte opaque Zeiger.
- Minimales Task-ABI: `FindTask(NULL)` verweist auf eine echte isolierte Gast-Task-Struktur, `tc_SigAlloc(+18)`, `tc_SigWait(+22)`, `tc_SigRecvd(+26)` werden im Gastspeicher synchronisiert. Task liegt außerhalb der HUNK-Ladezone und des wachsenden Exec-Heaps.
- `struct MsgPort` initialisiert `mp_SigBit(+15)`, `mp_SigTask(+16)` und eine leere Listenstruktur. Port-Events setzen das zugehörige Signalbit. `PutMsg/GetMsg/ReplyMsg` unterstützen einfache Gast-zu-Gast-Nachrichten im selben Prozess.
- `intuition.library`: `OpenWindowTagList -606` unterstützt ausgewählte `WA_*`-Tags (begrenzte Liste, kein beliebiges BOOPSI); `ModifyIDCMP -150`; `AddGadget -42`, `RefreshGadgets -222`, `RefreshGList -432`, `OffGadget -174`, `OnGadget -186`; `SetMenuStrip -264`, `ClearMenuStrip -54`; `ActivateWindow -450`, `MoveWindow -168`, `ChangeWindowBox -486`, `WindowToFront -312`, `WindowToBack -306`, `BeginRefresh -354`, `EndRefresh -366`.
- Hit-Test auf einfache Classic-Gadgets; SELECTDOWN/SELECTUP führt zu echten IDCMP_GADGETDOWN/GADGETUP-Nachrichten mit `IAddress` (Gadget-Zeiger). Rechte Maustaste öffnet ein einfaches, gezeichnetes Menü und liefert einen klassischen IDCMP_MENUPICK-Code (eine Menüebene; ohne Text-/Submenu-Widget-Engine).
- Fenster nutzen jetzt eigene opake Oberflächen und werden in Z-Reihenfolge neu komponiert. Überdeckte Inhalte bleiben beim Schließen des oberen Fensters erhalten. Gaststrukturen `Window.LeftEdge(+4), TopEdge(+6), Width(+8), Height(+10), Flags(+24), MenuStrip(+28), RPort(+50), FirstGadget(+62), IDCMPFlags(+82), UserPort(+86)` werden geschrieben.
- AmigaDOS: eingeschränkte Prozessmetadaten `GetArgStr`, `SetArgStr`, `GetProgramName`, `SetProgramName`, `GetCurrentDirName`, `SetCurrentDirName`, `Cli` (keine CLI), `SetIoErr`, `Exit` (endet ohne RTS). `CreateProc`, `CreateNewProc`, `LoadSeg` und `RunCommand` bleiben **nicht** emuliert.
- Neue ABI-Regressionssuite `engine/tests/qrx68_workbench_compat26.c`: prüft echte negative Vektoren, Messages/Reply-Port, Wait-Signale, Fensterstrukturen, Gadget-/Menüklicks, Fensterkomposition/Resize, Win-TagLists, Prozessmetadaten und fail-closed bei fehlendem CreateProc.

## Harte Grenzen

Dies ist **keine vollständige AmigaOS-3.x-Implementation** und kein Nachweis für Directory Opus oder Final Writer. Es fehlen u. a. GadTools, BOOPSI, Text/Fonts, vollständige Layers-Library/API, komplexe Menüs, Dateilocks/Packet-Handler, LoadSeg/CreateProc/Multitasking, echte AmigaOS-Prozessstrukturen, Workbench-Startup, ASL-Requester sowie Blitter-/Chipset-Timing und FPU-Validierung. Vektoraufrufe ohne Implementierung brechen weiterhin nachvollziehbar ab.

Das bestehende QRXDesktop kann QRX68Run weiterhin per DefaultTool aufrufen. Der neue HLE-Funktionsumfang ist im Hostdienst verfügbar; echter AROS-m68k-Build, FS-UAE-Doppelklick und ARM64-Musashi-Ausführung wurden auf diesem Buildhost nicht gemessen.
