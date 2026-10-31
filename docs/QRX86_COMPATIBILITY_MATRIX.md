# QRX86 compatibility matrix

Only observed outcomes are listed. Experimental JIT is disabled by default.

| Target / workload | Result | Scope |
| --- | --- | --- |
| QEMU m68020/030/040/060 | PASS unchanged-baseline oracle, selected compiler configuration | Native code/flags, optional leading loads, all constant counts and SIB, carry/Jcc/segments, single-step, SMC/budget/remap/faults |
| AROS/FS-UAE 68020/030/040/060 | PASS 2026-10-31 expanded suite, -O2 with JIT-only size optimization | SDK ABI, LEA/shifts/carry/Jcc/SMC/decoder/single-step, unrestricted-speed correctness mode; optional native loads excluded |
| x86_64 host safety | PASS, 5 groups | Unsupported backend rejection, old helper watchdog/terminal, stack failure, 100k arithmetic references |
| Synthetic Windows PE32 on AROS/FS-UAE 68060 | PASS off/on | Both ExitProcess(42), one actual HLE transition; JIT executes 9,999,925 instructions natively |
| Host DirectX test PE32 | PASS, 0 failures | DirectDraw/DirectSound/DirectInput; direct and copy-path frames byte-identical; host only |
| Legacy interpreter | PASS, 15 cases | Existing host suite |
| Windows PrBoom PE32 | Renders matching frames | 3 runs per mode, 100M under QEMU; final POSIX JIT improves QEMU; actual AROS RTG is slower than interpreter; see performance report; not full timedemo certification |
| Constant 32-bit shifts / LEA / entry-carry integer ops | PASS native | Restricted backend with interpreter fallback outside admitted encodings |
| Native memory / x87 / stack / variable shifts | General support absent | Optional leading-load experiment is separately compiled and tested, but slower and excluded by default |
| Physical 68020/030/040/060 | NOT RUN | No machine attached |
| macOS Apple Silicon | NOT RUN | Docker cross-build recipe provided, no Mac attached |
| Full Win32/Linux application suite | NOT CERTIFIED | Source integration checks do not establish all application behavior |
| AOT / chained blocks | NOT IMPLEMENTED | No compatibility or performance claim |

MUL/IMUL/DIV/IDIV fallback samples include positive inputs; they are not exhaustive
signed-edge or exception validation. A native pass does not certify all x86
encodings or floating-point behavior. The older threaded JIT stays quarantined.

Final AROS executable SHA-256:
`e09d8db33bb49233203c8a8b30e94f80ee655cb049abce3f2ddc67ba8176a4ed`.
This exactly matches the RTG JIT sample (0.123448 FPS). Final interpreter sample:
0.140615 FPS. Both capture frame 10 identically, but these are single bounded
runs with real backend clocks, not full demo or physical-hardware certification.


FS-UAE host-JIT update (2026-10-31): the separate patched FS-UAE3.2.35 boots
this AROS system with CPU JIT enabled. Native68060 differential/safety checks
pass with both JIT layers. The selected host profile interprets FPU instructions:
upstream FPU JIT changes two pixels in the PrBoom control image; CPU-only JIT
matches the no-JIT image exactly. See `tools/qrx86/fsuae/README.md` and the new
host-JIT section of `QRX86_PERFORMANCE.md`; the preceding RTG numbers remain
historical measurements with FS-UAE's own JIT off.
