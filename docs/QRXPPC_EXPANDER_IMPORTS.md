# QRXPPC - Importe der StuffIt-Expander-5.5-App

Quelle: `StuffIt Expander™` (PEF im `ST50`-Installer des StuffIt-Expander-5.5-ISO),
381116 Bytes Datenfork, 878389 Bytes Resource-Fork mit `cfrg`. Der PEF-Loader
nennt **289 Importe in fünf Bibliotheken**. Legende:

- **✓ direkt** - der Symbolname entspricht dem A-Line-Trap, den die HLE
  (`qrx_tb_dispatch` in `qrx_toolbox.c`) bereits bedient.
- **~ indirekt** - gleiche HLE-Operation, aber das Symbol ist ein
  InterfaceLib-Wrapper bzw. Inline-Aufruf mit anderer Argumentform; der Stub
  muss die Argumente auf den Trap abbilden.
- **— offen** - keine HLE, kein Trap bekannt/implementiert.

Die Tabelle ist die Arbeitsliste für die Namens→Trap-Abbildung: `✓` und `~`
bekommen sofort einen Stub, `—` fällt beim ersten Aufruf mit Symbolnamen auf.

Stand: 66 von 289 Importen sind über die HLE abgedeckt (54 direkt, 12 indirekt),
223 sind offen.

## Lauf im Gast (2026-11-14)

`C:QRXAppRun SYS:Storage/TestImages/Expander55.mb` lädt das Fragment über den
Namens→Trap-Resolver in `tools/qrxppc/qrx_appstub.c`: 58 Importe erhalten einen
Stub mit ihrem A-Line-Trap, 8 indirekte und 223 offene Importe eine Pseudotrap
`0x7E00 + Importindex`. Mit `Gestalt` und `GetZone` in der HLE läuft die
Initialisierung durch und der Einsprung (0x11924, TOC 0x4D000) kehrt zurück:

```
trap 0x0000A1AD sel=0 r4=0x74686473 r5=0x003FEF8C -> 0x00000000
trap 0x0000A11A sel=0 r4=0x0047D000 r5=0x00010000 -> 0x00001000
trap 0x0000A1AD sel=0 r4=0x6F732020 r5=0x003FEF38 -> 0x00000000
trap 0x0000A11A sel=0 r4=0x0027D000 r5=0x00010000 -> 0x00001000
trap 0x0000A1AD sel=0 r4=0x6F732020 r5=0x003FEF38 -> 0x00000000
trap 0x0000A11A sel=0 r4=0x00100000 r5=0x00010000 -> 0x00001000
trap 0x0000A1AD sel=0 r4=0x6F732020 r5=0x003FEF38 -> 0x00000000
trap 0x0000A1AD sel=0 r4=0x74686473 r5=0x003FEF7C -> 0x00000000
QRXAppRun: main returned after 8 trap(s), 8 implemented
rc=0
```

### Suche nach einer reinen PPC-App (2026-11-14)

`/Users/pascalpapara/Downloads/Test-Apps/macos_ppc` enthält keine klassische
Anwendung, die nur aus einem PEF besteht. Geprüft wurden Daten- und
Resource-Fork jedes klassischen Kandidaten (`unar -forks visible`, AppleDouble
ausgewertet, `qrx_rsrc_find`/`qrx_pef_parse`):

| Datei | `CODE` | `cfrg` | PEF im Datenfork |
| --- | --- | --- | --- |
| `Stuffit-Expander-5-5.iso.zip` → `StuffIt Expander™` | 4 | ja | 4 Engines (kein App-Main) |
| `reader_installer_4.05a.sit` → `Reader Installer` | 23 | nein | keiner |
| `DropStuff-disk1of2.dsk`/`2of2` → `Aladdin DropStuff™ 5.5 Install` | 8 | nein | keiner |
| `QuickTime770Leopard_1.dmg` | — | — | nur `QuickTime770_Leopard.pkg` (Mac OS X) |
| `ableton_live_7018.dmg` | — | — | nur `Live 7.0.18 OS X/Live.app` (Mach-O) |

PPC-Code ist in dem Bestand also vorhanden, aber in zwei Formen, die
`QRXAppRun` nicht laden kann:

* **Mach-O (Mac OS X):** `Live.app/Contents/MacOS/Live` ist ein Universal
  Binary `cafebabe` mit `i386` und `ppc`, `QuickTime Player` (aus dem
  `Payload` von `QuickTime770_Leopard.pkg`) ein Universal Binary `ppc_7400`
  und `i386`. Beide brauchen dyld, Mach-O-Segmente und die OS-X-Frameworks;
  der Runner lädt nur CFM-Container (`Joy!peffpwpc`).
* **StuffIt-InstallerMaker:** Die klassischen PPC-Programme (Acrobat Reader
  4.05, LightWave 6.5) stecken gepackt im Datenfork der Installer. Der
  Datenfork beginnt mit `SVCT` und hat Entropie 7.98 (komprimiert), der
  Installer selbst ist 68k (16 bzw. 23 `CODE`-Ressourcen, kein `cfrg`, kein
  PEF). InstallerMaker komprimiert mit StuffIt-Methoden in einem eigenen
  Container; ein Extraktor dafür existiert nicht (XAD/unar unterstützen nur
  die reinen StuffIt-Archive). Ohne InstallerMaker-Unpacker oder einen Lauf
  des Installers unter echtem Mac OS kommt man an diese PEFs nicht heran.

Ein Lauf „bis zum ersten fehlenden Import“ braucht daher ein
Anwendungsfragment ohne `CODE`-Ressourcen, das in diesem Bestand nicht
vorhanden ist.

### Der Datenfork enthält vier PPC-Engines, keine App

Der weitere Lauf zeigt, warum danach kein fehlender Import mehr kommt: Die
Datei ist keine PPC-Anwendung. Im Datenfork liegen **vier** PEF-Container
(`Joy!peffpwpc` bei 0, 248016, 281152, 337648); das `cfrg`-Resource nennt sie
`UnStuffEnginePPC`, `UUDecodingEnginePPC`, `UnZipEnginePPC` und `HQXEnginePPC`.
Die Anwendung selbst steckt als segmentierte 68k-App im Resource-Fork
(`CODE` 0–3, `CODE` 0 ist die Sprungtabelle, `CODE` 1–3 sind 68k-Code). Auf
einem Power Mac lief die 68k-App und rief die PPC-Engines über CFM/Mixed Mode;
der PEF-Einsprung, den `QRXAppRun` startet, ist der einer Engine und kehrt nach
der Initialisierung zurück. Für einen echten PPC-App-Test braucht es ein
`Joy!peffpwpc`-Anwendungsfragment ohne 68k-`CODE`-Segmente.

## InterfaceLib (268 Importe: 52 direkt, 12 indirekt, 204 offen)

| Symbol | HLE-Trap | Status |
|---|---|---|
| `AECreateAppleEvent` | — | — offen |
| `AECreateDesc` | — | — offen |
| `AEDisposeDesc` | — | — offen |
| `AEGetSpecialHandler` | — | — offen |
| `AEInstallSpecialHandler` | — | — offen |
| `AEProcessAppleEvent` | — | — offen |
| `AEPutParamDesc` | — | — offen |
| `AERemoveSpecialHandler` | — | — offen |
| `AESend` | — | — offen |
| `AddResource` | `0xA9AB` `QRX_TB_RM_ADD` | ✓ direkt |
| `Alert` | `0xA985` `QRX_TB_DM_ALERT` | ✓ direkt |
| `AllocContig` | — | — offen |
| `Allocate` | — | — offen |
| `AppendDITL` | — | — offen |
| `BackColor` | — | — offen |
| `BeginUpdate` | `0xA922` `QRX_TB_WM_BEGIN_UPDATE` | ✓ direkt |
| `BlockMoveData` | — | — offen |
| `CallComponentUPP` | — | — offen |
| `CallUniversalProc` | — | — offen |
| `CatMove` | — | — offen |
| `CautionAlert` | `0xA988` `QRX_TB_DM_CAUTION_ALERT` | ✓ direkt |
| `ChangedResource` | — | — offen |
| `ClipRect` | — | — offen |
| `CloseComponent` | — | — offen |
| `CloseConnection` | — | — offen |
| `CloseResFile` | — | — offen |
| `CloseWD` | — | — offen |
| `Count1Resources` | `0xA99C` `QRX_TB_RM_COUNT` | ~ indirekt: Inline-Wrapper auf `CountResources` |
| `CountResources` | `0xA99C` `QRX_TB_RM_COUNT` | ✓ direkt |
| `CountTypes` | `0xA99E` `QRX_TB_RM_COUNT_TYPES` | ✓ direkt |
| `Create` | `0xA008` `QRX_TB_FM_CREATE` | ~ indirekt: Trap-Form mit ParamBlock (FSCreate-Wrapper fehlt) |
| `CurResFile` | — | — offen |
| `CustomPutFile` | — | — offen |
| `DebugStr` | — | — offen |
| `Delay` | — | — offen |
| `DetachResource` | `0xA992` `QRX_TB_RM_DETACH` | ✓ direkt |
| `DirCreate` | — | — offen |
| `DisposeDialog` | — | — offen |
| `DisposeHandle` | `0xA023` `QRX_TB_DISPOSE_HANDLE` | ✓ direkt |
| `DisposePtr` | `0xA01F` `QRX_TB_DISPOSE_PTR` | ✓ direkt |
| `DisposeRgn` | — | — offen |
| `DisposeRoutineDescriptor` | — | — offen |
| `DisposeZone` | — | — offen |
| `DragWindow` | — | — offen |
| `Draw1Control` | — | — offen |
| `DrawPicture` | — | — offen |
| `DrawString` | `0xA884` `QRX_TB_TEXT_DRAW_STRING` | ✓ direkt |
| `EndUpdate` | `0xA923` `QRX_TB_WM_END_UPDATE` | ✓ direkt |
| `EqualString` | — | — offen |
| `EraseRect` | `0xA8A3` `QRX_TB_QD_ERASE_RECT` | ✓ direkt |
| `EraseRoundRect` | — | — offen |
| `EventAvail` | `0xA971` `QRX_TB_EV_AVAIL` | ✓ direkt |
| `ExitToShell` | — | — offen |
| `FSClose` | `0xA001` `QRX_TB_FM_CLOSE` | ~ indirekt: Wrapper auf den Close-Trap (ParamBlock) |
| `FSDelete` | `0xA009` `QRX_TB_FM_DELETE` | ~ indirekt: Wrapper auf den Delete-Trap (ParamBlock) |
| `FSMakeFSSpec` | — | — offen |
| `FSOpen` | `0xA000` `QRX_TB_FM_OPEN` | ~ indirekt: Wrapper auf den Open-Trap (ParamBlock) |
| `FSRead` | `0xA002` `QRX_TB_FM_READ` | ~ indirekt: InterfaceLib-Wrapper auf den Read-Trap (ParamBlock) |
| `FSWrite` | `0xA003` `QRX_TB_FM_WRITE` | ~ indirekt: Wrapper auf den Write-Trap (ParamBlock) |
| `FindControl` | — | — offen |
| `FindFolder` | — | — offen |
| `FindWindow` | `0xA92C` `QRX_TB_WM_FIND_WINDOW` | ✓ direkt |
| `FlushEvents` | `0xA032` `QRX_TB_EV_FLUSH` | ✓ direkt |
| `FlushVol` | — | — offen |
| `ForeColor` | — | — offen |
| `FrameRect` | `0xA8A1` `QRX_TB_QD_FRAME_RECT` | ✓ direkt |
| `FrameRoundRect` | — | — offen |
| `FreeMem` | `0xA01C` `QRX_TB_FREE_MEM` | ✓ direkt |
| `FrontWindow` | `0xA924` `QRX_TB_WM_FRONT_WINDOW` | ✓ direkt |
| `Gestalt` | `0xA1AD` `QRX_TB_GESTALT` | ✓ direkt |
| `Get1IndResource` | `0xA99D` `QRX_TB_RM_GET_IND` | ~ indirekt: Inline-Wrapper auf `GetIndResource` |
| `Get1Resource` | `0xA9A0` `QRX_TB_RM_GET` | ~ indirekt: Inline-Wrapper auf `GetResource` (CurResFile) |
| `GetBackColor` | — | — offen |
| `GetClip` | — | — offen |
| `GetControlMaximum` | — | — offen |
| `GetControlReference` | — | — offen |
| `GetControlTitle` | — | — offen |
| `GetCurrentProcess` | — | — offen |
| `GetCursor` | — | — offen |
| `GetDateTime` | — | — offen |
| `GetDeviceList` | — | — offen |
| `GetDialogItem` | — | — offen |
| `GetDialogItemText` | — | — offen |
| `GetEOF` | `0xA011` `QRX_TB_FM_GET_EOF` | ~ indirekt: Wrapper auf den GetEOF-Trap (ParamBlock) |
| `GetFPos` | — | — offen |
| `GetForeColor` | — | — offen |
| `GetFrontProcess` | — | — offen |
| `GetHandleSize` | `0xA025` `QRX_TB_GET_HANDLE_SIZE` | ✓ direkt |
| `GetIndResource` | `0xA99D` `QRX_TB_RM_GET_IND` | ✓ direkt |
| `GetIndString` | — | — offen |
| `GetIndType` | `0xA99F` `QRX_TB_RM_GET_IND_TYPE` | ✓ direkt |
| `GetKeys` | `0xA976` `QRX_TB_EV_GET_KEYS` | ✓ direkt |
| `GetMBarHeight` | — | — offen |
| `GetMainDevice` | — | — offen |
| `GetNewDialog` | — | — offen |
| `GetNextDevice` | — | — offen |
| `GetNextProcess` | — | — offen |
| `GetOSTrapAddress` | — | — offen |
| `GetPicture` | — | — offen |
| `GetPort` | `0xA874` `QRX_TB_QD_GET_PORT` | ✓ direkt |
| `GetProcessInformation` | — | — offen |
| `GetResource` | `0xA9A0` `QRX_TB_RM_GET` | ✓ direkt |
| `GetResourceSizeOnDisk` | — | — offen |
| `GetScrap` | — | — offen |
| `GetScriptManagerVariable` | — | — offen |
| `GetSharedLibrary` | — | — offen |
| `GetToolTrapAddress` | — | — offen |
| `GetVRefNum` | — | — offen |
| `GetWDInfo` | — | — offen |
| `GetWMgrPort` | `0xA910` `QRX_TB_WM_GET_WMGR_PORT` | ✓ direkt |
| `GetWRefCon` | `0xA917` `QRX_TB_WM_GET_REFCON` | ✓ direkt |
| `GetZone` | `0xA11A` `QRX_TB_GET_ZONE` | ✓ direkt |
| `GlobalToLocal` | — | — offen |
| `HCreate` | — | — offen |
| `HCreateResFile` | — | — offen |
| `HDelete` | — | — offen |
| `HGetState` | — | — offen |
| `HLock` | — | — offen |
| `HLockHi` | — | — offen |
| `HNoPurge` | — | — offen |
| `HOpenDF` | — | — offen |
| `HOpenRF` | — | — offen |
| `HOpenResFile` | — | — offen |
| `HPurge` | — | — offen |
| `HRstFLock` | — | — offen |
| `HSetFLock` | — | — offen |
| `HUnlock` | — | — offen |
| `HandleZone` | — | — offen |
| `HideControl` | — | — offen |
| `HideDialogItem` | — | — offen |
| `HiliteControl` | — | — offen |
| `HiliteWindow` | `0xA91C` `QRX_TB_WM_HILITE_WINDOW` | ✓ direkt |
| `HoldMemory` | — | — offen |
| `InitCursor` | — | — offen |
| `InitZone` | — | — offen |
| `InlineGetHandleSize` | — | — offen |
| `InsetRect` | `0xA8A9` `QRX_TB_QD_INSET_RECT` | ✓ direkt |
| `InvalRect` | `0xA928` `QRX_TB_WM_INVAL_RECT` | ✓ direkt |
| `LMGetApplLimit` | — | — offen |
| `LMGetApplZone` | — | — offen |
| `LMGetCurApName` | — | — offen |
| `LMGetEventQueue` | — | — offen |
| `LMGetFinderName` | — | — offen |
| `LMGetGrayRgn` | — | — offen |
| `LMGetHeapEnd` | — | — offen |
| `LMGetMBarHeight` | — | — offen |
| `LMGetResLoad` | — | — offen |
| `LMGetSysMap` | — | — offen |
| `LMGetTheZone` | — | — offen |
| `LMGetTicks` | — | — offen |
| `LMGetTopMapHndl` | — | — offen |
| `LMSetApplLimit` | — | — offen |
| `LMSetApplZone` | — | — offen |
| `LMSetCurDirStore` | — | — offen |
| `LMSetHeapEnd` | — | — offen |
| `LMSetResErr` | — | — offen |
| `LMSetResLoad` | — | — offen |
| `LMSetSFSaveDisk` | — | — offen |
| `LMSetTheZone` | — | — offen |
| `LocalToGlobal` | — | — offen |
| `MaxBlock` | — | — offen |
| `MemError` | — | — offen |
| `MenuSelect` | — | — offen |
| `ModalDialog` | `0xA991` `QRX_TB_DM_MODAL_DIALOG` | ✓ direkt |
| `MoveHHi` | — | — offen |
| `MoveTo` | `0xA893` `QRX_TB_QD_MOVE_TO` | ✓ direkt |
| `MoveWindow` | `0xA91B` `QRX_TB_WM_MOVE_WINDOW` | ✓ direkt |
| `Munger` | — | — offen |
| `NGetTrapAddress` | — | — offen |
| `NMInstall` | — | — offen |
| `NMRemove` | — | — offen |
| `NewAlias` | — | — offen |
| `NewControl` | — | — offen |
| `NewHandle` | `0xA122` `QRX_TB_NEW_HANDLE` | ✓ direkt |
| `NewHandleSys` | — | — offen |
| `NewPtr` | `0xA11E` `QRX_TB_NEW_PTR` | ✓ direkt |
| `NewPtrClear` | — | — offen |
| `NewPtrSys` | — | — offen |
| `NewRgn` | — | — offen |
| `NewRoutineDescriptor` | — | — offen |
| `NoteAlert` | `0xA987` `QRX_TB_DM_NOTE_ALERT` | ✓ direkt |
| `NumToString` | — | — offen |
| `OpenDefaultComponent` | — | — offen |
| `PBCloseSync` | — | — offen |
| `PBDTAddAPPLSync` | — | — offen |
| `PBDTAddIconSync` | — | — offen |
| `PBDTGetAPPLSync` | — | — offen |
| `PBDTGetPath` | — | — offen |
| `PBDTSetCommentSync` | — | — offen |
| `PBDirCreateSync` | — | — offen |
| `PBGetCatInfoSync` | — | — offen |
| `PBGetEOFSync` | — | — offen |
| `PBGetFCBInfoSync` | — | — offen |
| `PBGetFPosSync` | — | — offen |
| `PBGetWDInfoSync` | — | — offen |
| `PBHGetVInfoSync` | — | — offen |
| `PBHGetVolParmsSync` | — | — offen |
| `PBHGetVolSync` | — | — offen |
| `PBReadAsync` | — | — offen |
| `PBReadSync` | — | — offen |
| `PBSetCatInfoSync` | — | — offen |
| `PBSetFPosSync` | — | — offen |
| `PBSetVInfoSync` | — | — offen |
| `PBSetVolSync` | — | — offen |
| `PBWriteAsync` | — | — offen |
| `PBWriteSync` | — | — offen |
| `PaintBehind` | — | — offen |
| `PaintRect` | `0xA8A2` `QRX_TB_QD_PAINT_RECT` | ✓ direkt |
| `ParamText` | — | — offen |
| `PenNormal` | `0xA89E` `QRX_TB_QD_PEN_NORMAL` | ✓ direkt |
| `PenPat` | `0xA89D` `QRX_TB_QD_PEN_PAT` | ✓ direkt |
| `PenSize` | `0xA89B` `QRX_TB_QD_PEN_SIZE` | ✓ direkt |
| `PtInRect` | `0xA8AD` `QRX_TB_QD_PT_IN_RECT` | ✓ direkt |
| `PtrAndHand` | — | — offen |
| `RGBBackColor` | — | — offen |
| `RGBForeColor` | — | — offen |
| `ReadLocation` | — | — offen |
| `RecoverHandle` | — | — offen |
| `RectRgn` | — | — offen |
| `ReleaseResource` | `0xA9A3` `QRX_TB_RM_RELEASE` | ✓ direkt |
| `RemoveResource` | `0xA9AD` `QRX_TB_RM_REMOVE` | ✓ direkt |
| `ResError` | `0xA9AF` `QRX_TB_RM_ERROR` | ✓ direkt |
| `SFPPutFile` | — | — offen |
| `SameProcess` | — | — offen |
| `SectRect` | — | — offen |
| `SelectDialogItemText` | — | — offen |
| `SelectWindow` | `0xA91F` `QRX_TB_WM_SELECT_WINDOW` | ✓ direkt |
| `SetA5` | — | — offen |
| `SetClip` | `0xA879` `QRX_TB_QD_SET_CLIP` | ~ indirekt: HLE bedient `ClipRgn` (0xA879), SetClip ist Inline |
| `SetControlMaximum` | — | — offen |
| `SetControlTitle` | — | — offen |
| `SetControlValue` | — | — offen |
| `SetCurrentA5` | — | — offen |
| `SetCursor` | — | — offen |
| `SetDialogItem` | — | — offen |
| `SetDialogItemText` | — | — offen |
| `SetEOF` | `0xA012` `QRX_TB_FM_SET_EOF` | ~ indirekt: Wrapper auf den SetEOF-Trap (ParamBlock) |
| `SetFPos` | — | — offen |
| `SetHandleSize` | `0xA024` `QRX_TB_SET_HANDLE_SIZE` | ✓ direkt |
| `SetPort` | `0xA873` `QRX_TB_QD_SET_PORT` | ✓ direkt |
| `SetResLoad` | — | — offen |
| `SetResourceSize` | — | — offen |
| `SetWTitle` | `0xA91A` `QRX_TB_WM_SET_TITLE` | ✓ direkt |
| `SetZone` | — | — offen |
| `ShowControl` | — | — offen |
| `ShowDialogItem` | — | — offen |
| `ShowWindow` | `0xA915` `QRX_TB_WM_SHOW_WINDOW` | ✓ direkt |
| `StopAlert` | `0xA986` `QRX_TB_DM_STOP_ALERT` | ✓ direkt |
| `StringWidth` | `0xA88C` `QRX_TB_TEXT_STRING_WIDTH` | ✓ direkt |
| `SysBeep` | — | — offen |
| `SysEnvirons` | — | — offen |
| `SystemZone` | — | — offen |
| `TempDisposeHandle` | — | — offen |
| `TempFreeMem` | — | — offen |
| `TempHLock` | — | — offen |
| `TempNewHandle` | — | — offen |
| `TestDeviceAttribute` | — | — offen |
| `TickCount` | `0xA975` `QRX_TB_EV_TICK_COUNT` | ✓ direkt |
| `TrackControl` | — | — offen |
| `TruncString` | — | — offen |
| `UnholdMemory` | — | — offen |
| `UpdateDialog` | — | — offen |
| `UpdateResFile` | — | — offen |
| `UseResFile` | — | — offen |
| `ValidRect` | — | — offen |
| `WaitNextEvent` | `0xA860` `QRX_TB_EV_WAIT_NEXT` | ✓ direkt |
| `WritePartialResource` | — | — offen |
| `p2cstr` | — | — offen |

## AppearanceLib (10 Importe: 0 direkt, 0 indirekt, 10 offen)

| Symbol | HLE-Trap | Status |
|---|---|---|
| `ActivateControl` | — | — offen |
| `DeactivateControl` | — | — offen |
| `GetControlData` | — | — offen |
| `GetControlDataSize` | — | — offen |
| `GetDialogItemAsControl` | — | — offen |
| `GetRootControl` | — | — offen |
| `HandleControlKey` | — | — offen |
| `IdleControls` | — | — offen |
| `SetControlData` | — | — offen |
| `SetKeyboardFocus` | — | — offen |

## UnicodeConverter (6 Importe: 0 direkt, 0 indirekt, 6 offen)

| Symbol | HLE-Trap | Status |
|---|---|---|
| `ConvertFromTextToUnicode` | — | — offen |
| `ConvertFromUnicodeToText` | — | — offen |
| `CreateTextToUnicodeInfo` | — | — offen |
| `CreateUnicodeToTextInfo` | — | — offen |
| `DisposeTextToUnicodeInfo` | — | — offen |
| `DisposeUnicodeToTextInfo` | — | — offen |

## TextCommon (2 Importe: 0 direkt, 0 indirekt, 2 offen)

| Symbol | HLE-Trap | Status |
|---|---|---|
| `CreateTextEncoding` | — | — offen |
| `GetTextEncodingBase` | — | — offen |

## ThreadsLib (3 Importe: 0 direkt, 0 indirekt, 3 offen)

| Symbol | HLE-Trap | Status |
|---|---|---|
| `NewThread` | — | — offen |
| `SetThreadState` | — | — offen |
| `YieldToThread` | — | — offen |


## Lücken nach Thema (223 offene Importe)

- **Speicher:** `NewPtrClear`, `NewPtrSys`, `NewHandleSys`, `RecoverHandle`,
  `MoveHHi`, `HoldMemory`, `UnholdMemory`, `TempNewHandle`, `TempHLock`,
  `TempFreeMem`, `TempDisposeHandle`, `InlineGetHandleSize`, `HandleZone`,
  `SystemZone`, `InitZone`, `DisposeZone`, `Allocate`,
  `AllocContig`, `MaxBlock` (Trap 0xA061, nicht 0xA11D), `MemError`,
  `BlockMoveData`.
- **Resource Manager:** `CurResFile`, `UseResFile`, `CloseResFile`,
  `HOpenResFile`, `HCreateResFile`, `ChangedResource`, `SetResourceSize`,
  `WritePartialResource`, `GetResourceSizeOnDisk`, `UpdateResFile`,
  `GetIndString`.
- **File Manager:** alle `PB*`-Aufrufe (`PBReadSync`, `PBWriteSync`, `FSOpen`-
  Familie ist indirekt abgedeckt), `FSMakeFSSpec`, `HOpen*`, `DirCreate`,
  `CatMove`, `GetVRefNum`, `GetWDInfo`, `FlushVol`, `SetFPos`, `GetFPos`,
  `GetVol`, `FindFolder`.
- **QuickDraw/Text:** `DrawText` fehlt als Import, aber `ClipRect`,
  `FrameRoundRect`, `EraseRoundRect`, `DrawPicture`, `GetPicture`,
  `DisposeRgn`, `NewRgn`, `OpenRgn`, `SectRect`, `GlobalToLocal`,
  `LocalToGlobal`, `RGBForeColor`, `RGBBackColor`, `BackColor`, `ForeColor`,
  `GetBackColor`, `GetForeColor`, `PenMode` (nicht importiert), `InitCursor`,
  `SetCursor`, `GetCursor`, `NumToString`, `p2cstr`, `EqualString`,
  `TruncString`, `Munger`.
- **Controls/Dialoge:** `NewControl`, `DisposeControl`, `Draw1Control`,
  `HiliteControl`, `ShowControl`, `HideControl`, `MoveControl`,
  `SizeControl`, `TrackControl`, `FindControl`, `GetControlTitle`,
  `SetControlTitle`, `SetControlValue`, `GetControlMaximum`,
  `SetControlMaximum`, `GetControlReference`, `GetNewDialog`,
  `DisposeDialog`, `AppendDITL`, `GetDialogItem`, `SetDialogItem`,
  `GetDialogItemText`, `SetDialogItemText`, `SelectDialogItemText`,
  `HideDialogItem`, `ShowDialogItem`, `UpdateDialog`, `ParamText`.
- **Menüs/Events:** `MenuSelect`, `MenuKey`, `GetMBarHeight`,
  `LMGetMBarHeight`, `DragWindow`, `ValidRect`, `PaintBehind`, `Delay`,
  `SystemTask`/`WaitMouseUp`/`GetMouse` sind vorhanden, aber nicht importiert.
- **AppleEvents/Komponenten/Prozesse:** `AECreateAppleEvent`, `AESend`,
  `AEProcessAppleEvent`, `AECreateDesc`, `AEDisposeDesc`,
  `AEPutParamDesc` + vier `AE*Handler`, `OpenDefaultComponent`,
  `CloseComponent`, `CallComponentUPP`, `CallUniversalProc`,
  `NewRoutineDescriptor`, `DisposeRoutineDescriptor`, `GetSharedLibrary`,
  `GetCurrentProcess`, `GetFrontProcess`, `SameProcess`,
  `GetProcessInformation`, `GetNextProcess`.
- **System/LowMem:** `SysEnvirons`, `SysBeep`, `ExitToShell`,
  `DebugStr`, `ReadLocation`, `SetA5`, `SetCurrentA5`, `GetOSTrapAddress`,
  `GetToolTrapAddress`, `NGetTrapAddress`, `NMInstall`, `NMRemove`,
  `TestDeviceAttribute`, `HoldMemory`, alle `LMGet*`/`LMSet*`,
  `SetResLoad`, `GetDateTime`, `NewAlias`, `GetScrap`.
- **Bibliotheken ohne jede HLE:** `AppearanceLib` (10), `UnicodeConverter` (6),
  `TextCommon` (2), `ThreadsLib` (3).

