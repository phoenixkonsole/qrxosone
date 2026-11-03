# ARMITHLON 1.7 – QRX68100 Vampire-inspired 68040 enhancement

1. Independent QRX-only 64-bit vector unit with 24 registers/core and 10 tested commands, exposing per-core MMIO and safe native translator APIs. Not Apollo AMMX binary compatible.
2. Functional 16 KiB I + 32 KiB D 4-way caches with 64-byte lines and bus/DMA coherence. Used by QRX native JIT decode; general Musashi fetch not accelerated.
3. Per-core 256-entry saturating two-bit branch predictor and counters; safe optional prefetch.
4. Extension state allocated per board; cleared on QRX mode changes, reset and secondary-core restart. Legacy AmigaOS3 hides SMP and extension MMIO.
5. Native ARM64 fast-loop emitter extended from ADDQ to SUBQ with correctly preserved 68k CCR, safe C fallback and code invalidation.
6. New regression `qrx68100_vampire_mock_tests` plus host-side vector microbenchmark; commands `vampire-test` and `vampire-bench`.
7. Source remains cumulative from ARMITHLON 0.3 to 1.6; no modifications to user's GitHub repository.

Tests on Linux x86_64: 25/25 portable CTests passed; 25/25 with ASan/UBSan. Actual Musashi-backed 68040/AmigaOS/AROS boot and native M1/A64 rendering/benchmark tests remain unverified.
