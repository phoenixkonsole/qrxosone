# Upstream bases kept in this repository

Everything QRX OS One needs from outside, archived so a build never depends on
download hosts being reachable.  All files are verified by `SHA256SUMS*`.

## aros-deadwood/

Deadwood's AROS (https://github.com/deadwood2/AROS) is the base OS of QRX OS One
(Motorola 68K, `core-amiga-m68k`, rebuild option 12).

**Important:** Deadwood's GitHub releases do **not** contain m68k binaries.  The
newest release (ABIv11 2026.09, 14 Sep 2026) ships only `pc-x86_64`,
`linux-x86_64`, `x86_64-contrib` and an x86_64 SDK; ABIv0 releases ship
i386 only (checked 28 Oct 2026 via the release asset lists).  "Deadwood's latest
68k build" therefore means: Deadwood's newest source (branch `master`) built
for `amiga-m68k` with Deadwood's own `scripts/rebuild.sh` (option 11 toolchain,
option 12 core) - which is what this repository stores.

| item | content |
|------|---------|
| `source/deadwood-AROS-master-5b5fd4cfd4f0.tar.xz` | `git archive` of master `5b5fd4cfd4f039927c0d1bb9cdd7f4fc8644d23e` (2026-10-02), latest m68k-capable Deadwood source |
| `source/deadwood-AROS-29dfac42dd55.tar.xz` | `git archive` of `29dfac42dd55cddf813c695ab603f19ce2108656` (2026-08-15), the commit pinned by the proven QRX boot path (`QRX_DEADWOOD_REF`) |
| `m68k-build-5b5fd4c/` | our build of master for amiga-m68k: LiveCD ISO, ROM/extended ROM, Emergency ADF, build logs, provenance (see its README) |
| `prefetch-deadwood-ports.sh` | stages every third-party archive the Deadwood build fetches (gcc 10.5.0, binutils 2.32, gmp/mpfr/mpc/isl, zlib, png, jpeg, freetype, mesa, ...) from reachable mirrors: Ubuntu archive `.orig` tarballs, GitHub release assets and git tags |

Build from the archived source (example):

```sh
mkdir -p /opt/qrx-os-one/deadwood-aros-build && cd /opt/qrx-os-one/deadwood-aros-build
tar xJf <repo>/upstream/aros-deadwood/source/deadwood-AROS-master-5b5fd4cfd4f0.tar.xz
mv deadwood-AROS-5b5fd4c AROS && cp AROS/scripts/rebuild.sh .
bash <repo>/upstream/aros-deadwood/prefetch-deadwood-ports.sh $PWD/portssources
printf '11\n' | MAKE_JOBS=4 ./rebuild.sh     # toolchain-core-m68k
printf '12\n' | MAKE_JOBS=4 ./rebuild.sh     # core-amiga-m68k
```

The QRX deploy (`qrx-os-one/.../deploy.sh`) still defaults to the proven commit
29dfac42 and needs a git checkout (`$WORKSPACE/AROS/.git`); use
`--aros-ref 5b5fd4cfd4f039927c0d1bb9cdd7f4fc8644d23e` to build QRX on the latest
master.  Changing the default is a separate decision because the Emergency-Boot
geometry and IPrefs path were proven only on 29dfac42.

## reactos/

ReactOS (https://reactos.org), the Win32 userland that QRX86 maps as guest PE
images.

| item | content |
|------|---------|
| `0.4.15/ReactOS-0.4.15-release-1-gdbb43bbaeb2-x86-live.zip.part*` | Live CD (System32 with calc/notepad/sol/kernel32/ntdll/user32/gdi32/advapi32 verified) - the image `base-v5.9/deploy.sh` extracts |
| `0.4.15/ReactOS-0.4.15-release-1-gdbb43bbaeb2-x86-iso.zip.part*` | Boot CD |
| `0.4.15/restore.sh` | reassembles + verifies, links `/tmp/ReactOS-0.4.15-x86-live.zip` for deploy |

Files are split into 45 MiB parts because GitHub rejects files over 100 MB.
Source: `https://downloads.sourceforge.net/project/reactos/ReactOS/0.4.15/`.

**ReactOS 0.4.16** was released (git tag `0.4.16-release`, 2026-08-28), but its
download file name could not be resolved from this build environment
(sourceforge.net project pages and iso.reactos.org are blocked by the network
policy; ~60 file-name variants returned 404).  0.4.15 is therefore the newest
ReactOS release archived here.  To add 0.4.16, download the x86 *live* ZIP,
split it the same way (`split -b 45M -d -a 2 FILE FILE.part`) and add its
SHA-256 to `SHA256SUMS.full`.

## amiga-apps/, doom/

68k Amiga programs tested on AROS (see `amiga-apps/README.md`) and the Windows
game used for the DirectDraw demonstration: PrBoom-plus (Doom engine, GPL) with
SDL 1.2 (LGPL) and the Freedoom IWAD (BSD) - sources, data and licences in
`doom/README.md`.
