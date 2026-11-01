# ARMITHLON 2.2 / QRX68 ABI compatibility foundation

- New **real executed 68k HUNK -> Exec and AmigaDOS HLE** runner (qrx68-run).
  Uses standard negative library vector jumps, guest registers, guest stack,
  system base at absolute 4, explicit library versions.
- Additional 68k instruction forms for ABI code: MOVEA.L from abs.w/Dn;
  JSR/JMP with (d16,An). Unknown opcodes still properly fault.
- Exec API subset including allocation/reuse/FreeVec/CopyMem, versioned
  OpenLibrary/CloseLibrary; DOS console and opt-in sandboxed host file I/O.
- HUNK_RELOC32SHORT support and strict transaction-safe parsing.
- Existing host app router now supports opt-in QRX_HOST_68K HUNK routing,
  leaving AROS-native launch policy unchanged by default.
- A tiny original Amiga 68k HUNK executable included, rebuilt from Python
  via tools/build-qrx68-demo.py and exercised in real CPU/HLE tests.
- Bounded directory compatibility scanner for locating unsupported APIs and
  CPU faults without inventing compatibility results.
- 42/42 portable CTests, plus 42/42 sanitizer-backed CTests (final verification
  must be recorded alongside packaged checksum manifest).

**NOT IMPLEMENTED:** full Musashi QRX68 binding, robust AROS Exec/DOS process,
Intuition/graphics library emulation, native QRXDesktop integration, Kickstart
hardware compatibility, 68040 MMU full CPU support, production ARM64 JIT.
