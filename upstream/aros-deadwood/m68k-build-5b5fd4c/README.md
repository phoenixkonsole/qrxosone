# Deadwood AROS amiga-m68k build of master 5b5fd4c

Built 2026-10-28 in the QRX 22.5.11 session with Deadwood's own
`scripts/rebuild.sh` (option 11 `toolchain-core-m68k`, option 12
`core-amiga-m68k`) from `deadwood2/AROS` master
`5b5fd4cfd4f039927c0d1bb9cdd7f4fc8644d23e` (2026-10-02, newest m68k-capable
commit; Deadwood publishes no m68k release binaries).  Source snapshot:
`../source/deadwood-AROS-master-5b5fd4cfd4f0.tar.xz`.

| file | content |
|------|---------|
| `aros-amiga-m68k-rom.bin`, `aros-amiga-m68k-ext.bin` | AROS Kickstart replacement ROM + extended ROM (512 KiB each); usable as `kickstart_file` / `kickstart_ext_file` in FS-UAE/WinUAE |
| `aros-amiga-m68k-5b5fd4c.iso.xz` | `make distfiles EBFHUNKROM=no` LiveCD (109 MB uncompressed, HUNK binaries; SHA-256 of the ISO in `SHA256SUMS.iso`) |
| `bootdisk-amiga-m68k-3520.adf` | Emergency-Boot ADF with the QRX-proven 3520-sector geometry (1 802 240 bytes) |
| `aros-m68k-sdk-5b5fd4c.tar.xz` | `Development/` (headers, startup.o, link libraries) used to build the QRX m68k tools |
| `portssources.SHA256SUMS`, `zlib.PROVENANCE` | exact third-party archives used (staged by `../prefetch-deadwood-ports.sh`) |

Toolchain: gcc 10.5.0 + binutils 2.32 (Deadwood m68k defaults) + vasm; not
stored (1 GB unpacked, 255 MB xz) - rebuild with option 11 from the archived
source and the prefetch script.

Build notes (reproducible deviations, all in the prefetch script):
* Debian/Ubuntu `.orig` tarballs replace ftp.gnu.org downloads; gmp's DFSG
  tarball gets an empty `doc/Makefile.in`; libtiff's git-export tarball gets the
  generated `tiffvers.h`.
* Chromium `third_party/zlib` is pinned to tag 140.0.7339.0 (googlesource
  `lkgr` is unreachable and moving; Deadwood's `zlib-aros.diff` applies cleanly
  to that tag).
* `arch/m68k-amiga/boot/floppy` ADF size 1760 -> 3520 sectors (same patch as
  QRX `base-v5.9/deploy.sh`); with 1760 sectors the boot disk overflows.

Emulator verification: see `qrx-os-one/.../TESTREPORT-22.5.11.txt`
(FS-UAE A4000 with 2 MB Chip + 16 MB motherboard + 1 GB Z3 Fast + uaegfx RTG).
