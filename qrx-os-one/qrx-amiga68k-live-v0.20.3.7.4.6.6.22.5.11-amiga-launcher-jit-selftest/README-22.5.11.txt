QRX OS One 0.20.3.7.4.6.6.22.5.11 - Amiga HUNK launcher + opt-in JIT self-test
==============================================================================

Cumulative source release on top of 22.5.10 (nothing removed).  Start here:

  CHANGELOG-22.5.11.txt         what changed and why
  TESTREPORT-22.5.11.txt        what was tested where (host / m68k build /
                                FS-UAE target) and what is still open
  INSTALL-22.5.11.txt           build server, deploy, host tests, emulator test
  SHA-GATE-REPORT-22.5.11.txt   input verification, explained stale manifests
  SHA256SUMS-22.5.11            checksums of every file in this tree

New components
  amiga-runtime-22.5.11/        QRXAmigaRun (amigarun / amigacheck),
                                qrx-jit-selftest, HUNK test programs, target
                                test script, FS-UAE harness, host tests
    AMIGA-LAUNCHER-22.5.11.txt  launcher reference
    JIT-SELFTEST-22.5.11.txt    JIT quarantine status, self-test stages,
                                crash safety, release procedure
  compatibility/PPC-ARCHITECTURE-PLAN-22.5.11.txt   PPC32 plan (plan only)
  tests-22.5.11/                QRX86 interpreter/watchdog/trace regression tests
  test-evidence-22.5.11/        emulator results, logs and screenshots
  validate-22.5.11.py           source/integration gates + host tests (run by deploy.sh)

Highlights
  * amigarun / amigacheck: classic AmigaOS 1.x-3.x HUNK programs with a
    bounds-checked parser, library/device probe, AROS LoadSeg proof, DOS
    process start, real return codes; QRXDesktop terminal integration.
  * qrx-jit-selftest: opt-in; static stages by default, native stages only with
    EXECUTE; passed static + native on emulated 68020/030/040/060.  The QRX86
    JIT itself stays quarantined.
  * QRX86 fixes: 1.7 MB VM state no longer on the process stack (likely cause of
    the old random-PC crash), REP resumable + watchdog-charged, MOV r8,imm8.
  * Leaner, cleaner desktop: RTG wallpaper cache, transparent text, Welcome
    autostart switch, optional own RTG screen, fixed LAlt+F1..F4 exit bug.
  * Base OS: Deadwood AROS master 5b5fd4c built for amiga-m68k; archived with
    ReactOS 0.4.15 under <repo>/upstream (Deadwood ships no m68k binaries).

Honest limits: full deploy.sh needs the external QRX v0.20 payload and the
Deadwood reference tar (not in the handoff); no real-hardware test; no PPC/OS4
execution; QRX86 JIT not enabled.
