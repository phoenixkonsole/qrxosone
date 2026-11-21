# Sources of the Raspberry Pi appliance's libraries

The QRX ARMithlon Pi 4 image (`docs/armithlon/raspberry-pi-image.md`) takes these libraries as
Ubuntu 24.04 arm64 binaries. Their sources are here, exactly the versions in the image, so they can
later be built trimmed for the Pi 4 and tuned for speed. Nothing here is built yet.

| Source | Version in the image | What the image uses it for | Trimming aim |
| --- | --- | --- | --- |
| Mesa | 25.2.8-0ubuntu0.24.04.5 | EGL and OpenGL for the host GPU (Direct3D/Glide) and the GL door, V3D driver | only `v3d`/`vc4` (+`kmsro`), no LLVM (`-Dllvm=disabled`), no X11/Wayland platforms: about 200 MB less (libLLVM 133 MB, ICU 30 MB) |
| libdrm | 2.4.125-1ubuntu0.1~24.04.2 | needed by Mesa | only the core and `vc4` |
| alsa-lib | 1.2.11-1ubuntu0.3 | sound (linux/qrx_lio.c), incl. the IEC958 framing HDMI needs on the Pi | without the plugins the image does not use |
| BusyBox | 1.36.1-6ubuntu3.1 | init, shell, DHCP, tools | only the applets the image's scripts call |

Each comes as Ubuntu ships it: `.dsc`, upstream tarball (`.orig`, with the upstream signature
`.asc` where there is one) and Ubuntu's patch set (`.debian.tar.xz`).

**Checked on 2026-11-21:**
- Ubuntu's `noble-updates` `InRelease` has a good signature from the Ubuntu archive key
  (`/usr/share/keyrings/ubuntu-archive-keyring.gpg`).
- The `main/source/Sources.xz` listed in it matches its SHA256.
- Every `.dsc` here is in that index with its SHA256.
- Every file matches the SHA256 its `.dsc` lists.
- `SHA256SUMS` records all of them:

```sh
cd upstream/pi-sources && sha256sum -c SHA256SUMS
```

Not here, on purpose:
- **SDL2:** the appliance no longer uses it (linux/qrx_lio.c talks to the kernel directly).
- **The kernel:** Raspberry Pi Linux, too big for the repository.
  - `tools/armithlon/pi/kernel/fetch-kernel.sh` fetches the exact commit the image was built from.
  - The configuration and build script are in the same directory.
