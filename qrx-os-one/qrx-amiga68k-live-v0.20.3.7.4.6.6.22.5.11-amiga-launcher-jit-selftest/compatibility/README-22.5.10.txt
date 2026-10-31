QRX OS One 22.5.10 – Amiga HUNK and JIT Preflight

Host-side tools only. Existing desktop, native Amiga launch semantics, and QRX86 runtime are preserved unchanged.

python3 compatibility/test-foundation.py
python3 compatibility/qrx-hunk-audit.py /path/to/classic-amiga-program
python3 compatibility/qrx-jit-preflight.py

Classic Amiga HUNK programs can be attempted via the native AROS DOS Execute mechanism, subject to API/library/hardware compatibility. No universal compatibility claim.

The JIT test checks big-endian JSR abs.l / RTS byte emission but cannot test I-cache coherence, native ABI, memory execution, or exception recovery. The native QRX86 JIT remains quarantined. Do not re-enable it on target based on these host-only tests.

PowerPC execution (AmigaOS 4 / Linux PPC) is NOT implemented.
