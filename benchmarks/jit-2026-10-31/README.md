# Optimization evidence, 2026-10-31

Branch claude/qrx-22.5.11, base25a6057a00db7201a323db80fc40e66cd459cba1.
Final source measurements: final-qemu-results.json, final-rtg-off-results.json,
rtg-fused-results.json (identical final AROS HUNK). Final oracle/AROS/safety,
emitter and host DirectX logs have final-/release- prefixes. Earlier selected/
checkpoint/tuning files are exploratory or intermediate, not final releases.

QEMU and FS-UAE results are different execution layers. Do not present the QEMU
speedup as an Amiga gain. AROS correctness tests use unlimited/warp mode; RTG
FPS runs use normal settings, real graphics, no emulator JIT, no -s option and
host monotonic START/END observation. Capture frame10 raw PPM hashes match.
Budget rc139 is deliberate only with exact100M, explicit budget fault, positive
frames and completed harness. No completed timedemo claim.

Negative attempts: -s accidentally selected headless (aborted, excluded); one
reference overlapped host checks (aborted, excluded); global-Os target failed
to reach capture30 (no qualified wall row); captures now use frame10. The early
selected-interpreter run had a client starting/ready event, with the same PID
tree verified alive; final release-off is a fresh run without that event.
Some minimum-block screening overlapped compilation; use final repeated data,
not exploratory single runs, for quantitative QEMU gains.

The harness kills FS-UAE after DONE; shell Killed messages following COMPLETED
are cleanup, not guest crashes. Native i386 differential suite is SKIP because
gcc-m32 libc support is absent. Physical Amiga and Apple Silicon are NOT RUN.
Rejected decoder-case source is retained only for audit/reproduction; it is not
the active core. No global maximum or 50-FPS claim.
