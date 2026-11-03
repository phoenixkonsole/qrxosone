# Building a *real* QRX OS One ISO, not a relabeled AROS image

Source audited: private `phoenixkonsole/qrxosone`, branch `claude/qrx-22.5.11`, commit `fb1d03e45a09e0fb896712590782011baa76de92`.

## Available in that Git branch

- AROS amiga-m68k 5b5fd4c base ISO (109 MB uncompressed) + AROS ROM/ext ROM and boot ADF.
- 22.5.11 cumulative QRX source ZIP and 22.5.11 m68k HUNK-program ZIP.
- On the **same named branch**, 22.5.12 sources/binaries also exist; this kit deliberately pins 22.5.11 artifacts.
- The 22.5.11/22.5.12 deploy entry point is the tracked
  `qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest/deploy.sh`.

## Required external inputs for real ISO

The current release pipeline *requires both of these*, and neither is contained
in the audited GitHub branch:

1. `/tmp/amiga-m68k-classic-20260815-012049.tar.gz` — verified reference system tarball (SHA-256 `c2d7362aea32232952365cba6ab01848d62eb1ea70bb47d3726d4e7d211f8ab9`).
2. The original QRX v0.20 payload build tree, including `build-qrx-amiga68k-live-v0.20.sh`, `src/`, and the actual QRXDesktop payload. Pass `--qrx-v020-root PATH`.

ReactOS 0.4.15 live ZIP, Deadwood source archives and toolchain build instructions
are already in `upstream/` of the repository. Release compilation requires an
**x86-64 Linux** build environment, root/sudo, build dependencies, substantial
disk space and a validated Deadwood toolchain. On M1, a Linux/amd64 Docker VM
is possible but expensive; the build has not been validated there.

## Real release command (only when both external inputs exist)

```bash
# Run ON x86-64 Linux after cloning all the required repository files,
# restoring ReactOS (see upstream/reactos/0.4.15/restore.sh)
ROOT=qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest
sudo bash "$ROOT/deploy.sh" \
  --reference-tar /tmp/amiga-m68k-classic-20260815-012049.tar.gz \
  --qrx-v020-root /ABSOLUTE/PATH/TO/QRX-V020-PAYLOAD \
  --reactos-file /tmp/ReactOS-0.4.15-x86-live.zip \
  --output /tmp/QRX-OS-One.iso
```

The script `deploy.sh` in this branch actually includes 22.5.12 validations,
so the ISO output is a build of the *current branch*, not clean 22.5.11 only.
It must pass all the existing validators and ISO extraction gates before use.
Do not turn off required build-time checks to force an ISO.

The resulting ISO is still for the **Amiga m68k machine profile**, NOT for the
chipset-less experimental QRX68100 qrxvirt board. The latter needs a complete
68k interpreter/JIT, AROS BSP, display/audio/network host drivers and bootloader.

## Why no ready-to-download QRX ISO in this kit?

The repository does not include a verified finished QRX OS One ISO, and the
release pipeline's required external inputs are missing from it. The bundled
AROS image is authentic AROS, not a QRX OS One release. A fake QRX ISO would
hide this incompatibility and be useless for distribution.
