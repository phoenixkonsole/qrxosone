# QRX GitHub alignment 1.2

Audited GitHub ref: `refs/heads/claude/qrx-22.5.11`, `cc2ba06a0cc76a87bdd49edcd4b475df4227e716`.

This differs from the earlier pinned ARMITHLON 1.1 release `fb1d03e45a09e0fb896712590782011baa76de92`. The GitHub tip includes commits `1e9acda...` (import ARMithlon 1.1 / native builds) and `cc2ba06...` (embedded X11 / AROS display), whereas the main branch remains at `3287d27...`. No attempt was made to merge these diverged refs or overwrite the original JIT implementation.

New code is limited to the **native host** QRX86 path. The stock AROS m68k `qrx-i386` still uses `qrx_x11_aros.h`; Windows Win32 HLE remains separate. `QRX86_JIT` is unaffected and default-off.

The exact `native_present` hook used by `tools/patch-qrx86-x11-host-gpu.py` was checked against live `qrx_x11.c` at cc2ba06. The script uses a staged copy of the source. A newer GitHub commit requires a renewed audit and a new pinned version; `fetch` intentionally does not silently switch to a moving branch.

Next correctness milestones: build the new X11 host on an actual Apple M1, replay the supplied i386 ELF demo, compare the received Metal frame with the X11 PPM reference, and implement the missing host-input → X11-event bridge. Do not claim hardware GPU acceleration from image-upload timings alone. GLX and native Vulkan guest drivers are still separate projects.
