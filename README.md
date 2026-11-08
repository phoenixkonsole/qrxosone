# QRX OS One

Native 68K desktop operating environment on Deadwood AROS (Motorola 68K).

| path | content |
|------|---------|
| `qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest/` | current cumulative source **22.5.11** (start with `README-22.5.11.txt`) |
| `qrx-os-one/handoff-input/` | the unchanged 22.5.10 handoff ZIP this release is based on |
| `dist/` | `QRX-OS-One-22.5.11-CUMULATIVE-SOURCE.zip`, prebuilt 22.5.11 m68k HUNK tools, `SHA256SUMS` |
| `upstream/` | archived bases: Deadwood AROS sources (latest master + pinned commit), our Deadwood amiga-m68k build (ROMs, LiveCD ISO, ADF, SDK), ReactOS 0.4.15, port prefetch script - see `upstream/README.md` |

Experimental QRX86 m68k JIT work (default **off**):
[audit](docs/QRX86_JIT_AUDIT.md), [measurements](docs/QRX86_PERFORMANCE.md),
[compatibility](docs/QRX86_COMPATIBILITY_MATRIX.md),
[cross-build and FS-UAE](docs/QRX86_BUILD.md).
PrBoom currently runs slower with this experimental QRX86 backend.
On ARMithlon the [QRX86 host door](docs/armithlon/performance-2026-11-03.md) replaces it.
FS-UAE’s separate host m68k JIT now works using the
[patched emulator and validated profile](tools/qrx86/fsuae/README.md).

ARMithlon 1.1 cumulative source and native host integration:
[build and M1 validation](docs/ARMITHLON_INTEGRATION.md).

ARMithlon on Apple Silicon (68k JIT, Metal RTG, HostFS, AHI, QRX86 host door):
[measurements, changes and open work](docs/armithlon/performance-2026-11-03.md).
FPU in the 68k JIT and two FPU faults fixed: [docs/armithlon/fpu-jit-2026-11-07.md](docs/armithlon/fpu-jit-2026-11-07.md).

Linux i386 GUI work: [embedded X11 subset and AROS windows](docs/QRX_X11.md).

Blender 2.79b as a 68k AROS program, headless stage with the second 68100 core:
[plan, state and measurements](docs/BLENDER_AROS68K.md), [build](tools/blender/README.md).

IPFS client with a complete guest-side CLI (`QRXIPFSPub`: add, ls, get,
pin/unpin, version, repo), native local 68K node (`QRXIPFSD`), native
libp2p peer (`QRXIPFSP2P`: Noise, Yamux, Bitswap 1.2, Kademlia), public
`IPFS:` drive (assign) with revisions, and encrypted volume images for
QRX Drive: [docs/QRX_IPFS.md](docs/QRX_IPFS.md).
