# Version note (v0.2, 2026-10-31)

The section below is the preserved original v0.1 implementation brief. A functional synthetic **qrxvirt boot monitor** and limited interpreter/JIT are now implemented in v0.2. Start with README.md, docs/QRXVIRT_BOARD_ABI.md, and docs/NEXT_IMPLEMENTATION.md for accurate current status. **AROS boot, complete 68040, host GPU, AHI and networking are not yet implemented.**

---

# Full implementation brief for Claude Code / Codex

You are the lead engineer for the new `QRX 68100` runtime for `QRX OS One` 68k AROS distribution. Implement a REAL accelerated, Amithlon-like 68k runtime with dedicated Apple Silicon/macOS and Linux host backends. Treat docs/ARCHITECTURE.md as the engineering contract and README as the honest current implementation status.

## Mandatory starting state

- Current new repo is only QRX68100 Foundation v0.1: a tiny interpreter + ARM64 basic-block sample with differential smoke test. It does **not** boot QRX OS One.
- The existing QRX OS One 22.5.10 cumulative source and QRX86 are a different codebase. Preserve all of their functionality and safety gates, especially QRX86's intentionally disabled native x86->68k JIT. Do not overwrite existing images, build scripts or source in-place without explicit additive integration.
- `QRX86` translates x86->m68k. `QRX68100` translates m68k->host ISA; don't merge the execution semantics or misdescribe the direction.
- We want an AROS/QRX 68k distribution on Mac M1 first, then Linux ARM64 and Linux x86_64. Nonclassic virtual board; normal boot needs no custom chips. Optional classic compatibility later.
- Treat 68100 as a QRX brand CPU. Feature flags and documentation must never falsely claim full Apollo 68080/AMMX/SAGA compatibility.

## Delivery goals

1. Audit source and build environment; inventory every guest 68k CPU operation and boot assumption from the actual QRX OS One boot chain. Generate a truth table of current implementation vs target. Run all current tests and store baseline results.
2. Design `qrxvirt-m68k` AROS BSP/boot machine so the m68k AROS desktop can boot without Amiga custom chips. Implement CPU reset/vector path, interrupts/timer, 68k address space, early debug console, virtual boot/storage and 2D framebuffer/HIDD. Until it boots, use an explicitly labeled minimal classic-peripheral shim only for bring-up, and do not claim it is chipset-less.
3. Complete 68k interpreter reference before expanding JIT: instructions, addressing modes, SR/CCR, stack/frame, exceptions, privilege behavior, proper 68020/040 FPU, big-endian memory, self-modifying code and unaligned access. Use public ISA conformance tests. Undefined instruction behavior is deliberate, never host UB.
4. Implement production AArch64 DBT (NOT hardware virtualization): Tier1 direct basic-block translation with register caching, lazy flags, block chaining, low-overhead dispatch; Tier2 optimize hot traces only after differential test success. Cache translated blocks and invalidate on guest code writes, faults, page changes and device remaps. Support macOS MAP_JIT + write protection, signed/hardened app entitlements when applicable, ICACHE invalidation, Linux W^X. Provide interpreter fallback for all missing opcodes.
5. Host bridges: versioned `QRXH1` command transport, guest `qrxgpu.hidd` accelerated framebuffer, AHI→CoreAudio/PipeWire, bsdsocket.library→BSD sockets (or SANA-II), block storage, monotonic timer, input. Host services native ARM64 code; 68k-facing libraries remain genuine 68k binary APIs. No raw host pointer exposure; validate all queue and buffer fields.
6. GPU: native Metal macOS, native Vulkan Linux, shared renderer-neutral command list. Deliver 2D desktop first. Then a guest `Warp3D.library` acceleration frontend, guest MiniGL/OpenGL adapter, optional 3dfx Glide API adapter, and tested subset of SAGA RTG (name exactly which registers/features exist). Do NOT confuse Vulkan, OpenGL, Warp3D, SAGA and 3dfx Voodoo hardware.
7. Define dual virtual 68100 cores in the VM config, but keep vCPU1 **offline**. Multi-thread native host rendering/I/O/JIT now. Enable true guest SMP only when an actual 68k AROS SMP port has correctness and race tests.
8. Once macOS M1 is validated, add Linux ARM64 runtime with the same AArch64 JIT and Linux x86_64 with a separate x86_64 backend or well-supported portable DBT code generator. Create lean initramfs Linux 'Amithlon-style' launcher; Linux remains Linux under the hood.
9. Optional: AMMX decode/NEON translation only after verified instruction reference/ABI and differential tests. The Apollo 68080 is not simply a free alias for an invented CPU, and lack of MMU instruction support must be reflected in behavior.

## Exact milestone quality gates

- M0: Build and run existing foundation (portable tests; M1 JIT test on real M1). Check ARM encoding using assembler; no undefined host behavior or RWX on Linux.
- M1: Interpreter passes mandatory 68k instruction and exception suites and guest/host memory integrity/fuzzing; stable test harness.
- M2: AROS QRX virtual-board serial boot logs, then desktop boot, no Agnus/Denise/Paula required. If legacy boot depends on CIA/vblank provide virtual timer instead of fabricating success.
- M3: AArch64 JIT vs oracle: complete register, flags, PC, exception, memory and FPU state compared, with self-modifying tests, signals/interrupts, no invalid instruction crashes. JIT default OFF until tests green, then ON in developer channel.
- M4: Metal accelerated 2D rendering and pointer/keyboard; AHI audio; TCP DNS/connect/read/write; disk and clean shutdown. Present measured latency and throughput.
- M5: Warp3D host acceleration tested on at least one real 68k app, with explicit GPU feature matrix and fallback modes. SAGA register compatibility only for documented tested subset.
- M6: Real M1 performance benchmarks and workloads; 24h soak; profiler data; no fictitious GHz performance claims; Linux ARM64 and x86_64 targets.

## Engineering rules

Use C/C++ for emulation core, C ABI between guest/host, native Metal/Objective-C++ on macOS and Vulkan on Linux. Keep host-specific APIs out of CPU decoder, ABI and guest drivers. Use locked tests, reproducible builds, CI, sanitizer builds, disassembler-assisted verification, visible error messages and traceable logs. Prefer adding modules rather than replacing previous functionality. Don't pull third-party licensed code without recording origin/license. Don't silently substitute software rendering for requested GPU acceleration. Never claim implementation of unsupported features.

## Output / handoff after EVERY step

Provide cumulative FULL source ZIP (no delta-only patches), git commit/hash, build scripts, dependencies, architecture docs, capability matrix, tests/logs, benchmarks and limitations. State exactly what was tested on Apple M1 vs Linux x86_64 CI vs simulated tests. Ensure QRX OS One old source remains recoverable. Next step should be executable, not just an architectural writeup.
