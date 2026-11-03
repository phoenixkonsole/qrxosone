# Native Boot / Desktop / QRX86 Implementation Gates

| User goal | Current concrete implementation | Proved here? | Missing blocking dependency |
|--|--|--|--|
| Musashi 68040 full ISA + FPU, exceptions | Pinned upstream integration; optional real CPU tests incl TRAP/RTE; v1.9 guest QEX1 subroutine trampoline | CPU source cannot be downloaded by local build; actual Musashi tests not run | FPU/MMU differential tests, exception regression on M1 |
| 2x 68040 kernel CPU tasks | Existing Musashi context-switch scheduler; new opt-in core1 guest worker with shared RAM and MMIO; legacy core1 parked | C mock + portable regression only | real Musashi dual guest execution, real Exec tasks/scheduler, address-space isolation |
| Native QRX68100 AROS Boot | Real QRX RAM firmware; 1.9 native 512-byte block DMA; 68040 timer/IRQ MMIO | Firmware & block DMA: YES; AROS: NO | AROS BSP, MMU/page protection, interrupt vectors, boot ROM loader, DOS block driver |
| QRXDesktop guest Workbench | Real existing GitHub QRXDesktop canonical 6.6.20 (not duplicate); guarded async-launch patch | Fixture patch tested, source hash pinned | Compile patched AROS desktop with SDK; get full AROS boot + RTG/input drivers |
| Double-click Win32/Linux programs | Existing AROS QRXDesktop PE32/ELF i386 dispatch; new `qrx-app` strict binary header router; local `execv` host runtime, no shell | parser + source patch: YES; full guest apps: NO | Deliver actual AROS QRXWinRun/QRX86Run programs and working QRX86 runtime inside guest OS; API coverage |

## Required real completion gate

A successful native QRX68100 AROS boot must include a guest UART log generated
from the same Musashi CPU with a fully qualified 68040 FPU/MMU context, verified
native mouse/keyboard and QRXGPU RTG pixels, and an actual QRXDesktop task
appearing on the guest screen (not the FS-UAE/host AppKit shell). For Windows
and Linux double-click compatibility, also require two real PE32/ELF32 guest
programs, runtime API/error logs, recorded frame/screenshot, nonzero instructions
and correctly terminated processes.
