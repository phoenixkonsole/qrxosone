QRX OS One Amiga 68K v0.20.3.7.4.6.6
QRX Diagnostics Center / Desktop Foundation

This is an incremental release on top of the proven v0.20.3.7.4.6.5.9
Flat Retro / requester-free desktop baseline.

Adds:
- Native AROS/m68k QRXDiagnostics executable
- OCS/ECS-safe low-colour diagnostics window
- mounted-volume test via LockDosList/NextDosEntry (no blind USB probing)
- subsystem presence/status matrix
- report in T:QRX-System-Test.txt
- optional QRX:SystemTest script for active runtime probes
- diagnostics drawer marker and desktop-foundation release contract

The deployer intentionally does not reintroduce Wanderer/requester hacks or the
old AmigaOS ABI. It requires the existing same-commit Deadwood workspace from
.5.9 and rebuilds the ISO after installing the diagnostics payload.

6.6.22.5 QRX86 execution extension:
- QRXGRT1 mapped ReactOS runtime is executable through QRX:C/QRX86GRTExec
- dependency-ordered TLS/DllMain/EXE-entry scheduler
- FS/TEB + minimal PEB bootstrap
- concrete ntdll host bridge dispatch
- IA-32 interpreter fallback with unsupported-opcode module/EIP trace
- m68k executable native threaded-JIT block cache (foundation for later direct emission)
- winjit / wintrace terminal commands and ReactOS calculator/notepad tests use the new executor
