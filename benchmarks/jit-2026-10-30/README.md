# Experimental JIT evidence, 2026-10-30

Branch claude/qrx-22.5.11, baseline 25a6057a00db7201a323db80fc40e66cd459cba1.
See ../../docs/QRX86_PERFORMANCE.md and ../../docs/QRX86_COMPATIBILITY_MATRIX.md.
Doom is a deliberate fixed-budget stop (expected 139 verified by the runner),
not a completed timedemo. Frame hash equality is recorded in doom-results.json.
AROS logs require COMPLETED, ABI PASS, native-suite PASS and rc=0. The harness
terminates FS-UAE after DONE; its shell "Killed" message is normal cleanup.
Synthetic PE success is the expected guest ExitProcess(42), not shell rc=0.
QEMU timings were collected without concurrent FS-UAE benchmarks.
Source validation skips native i386 differential tests because gcc -m32 is absent;
strict m68k QEMU and actual AROS native tests are separate, executed evidence.
Host DirectX direct/copy frame equality was checked with cmp; logs are retained.
No physical hardware, completed Doom timedemo or Apple Silicon execution claim.
