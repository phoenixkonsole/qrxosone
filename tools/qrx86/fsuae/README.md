# FS-UAE AROS diagnostic ROM investigation

The original harness keeps FS-UAE's own m68k JIT off because Debian FS-UAE
3.1.66 cannot boot this Deadwood AROS build with it enabled. This switch is
independent of the QRX86 x86-to-m68k JIT inside the guest.

Reproduction: `QRX_FSUAE_JIT=1`, CPU68060, MMU off, directory-backed AROS
and QRXOUT drives. The log reports `JIT=CPU/FPU=8192`, then an access fault at
`00E92000` in `Filesystem autoconfig`. With indirect JIT memory accesses the
CPU instead halts at `00F85954`, reason3. Neither case reaches User-Startup.
Changing Z3 mapping to UAE does not resolve it.

`src/expansion.cpp` installs executable diagnostic code at offset0x2000 in
this board's ROM (including JMP entry points), but its bank uses
`default_xlate` and dummy instruction-read functions. AROS copies this ROM
while booting; default_xlate rejects the translation required by the JIT.
The attached patch provides translation/checks only within the initialized
0x3000-byte ROM range and uses its existing read handlers for instruction
fetches. The I/O flags and write callbacks remain intact.

A second bug occurs on x86-64: `VirtualAlloc(NULL, ..., MEM_COMMIT)` allocates
these expansion banks without `UAE_VM_32BIT`. A diagnostic trace showed a bank
pointer `0x7f9be81e6000`, whereas the JIT's native-address registers carry only
32 bits. With JIT enabled the patch requests low-address allocations. After
this change, translation produced `0x41f35000` for the diagnostic ROM and AROS
booted successfully. Merely adding a translation function is insufficient.

`enable.sh` selects `comp_trustnaddr=indirect` so bulk accesses use the bank's
translation instead of assuming its ROM lies in NATMEM. Ordinary byte/word/
long accesses retain their direct defaults. No MOVEM opcode is disabled and
FPU instructions remain interpreted in the selected profile (see below). Temporary MOVEM fallback and translation logging used
during diagnosis are absent from the final patch.

Pinned upstream: FS-UAE v3.2.35,
`4ae7ddaec50b567ed80d71ffbff067cb58e945a3`.
Build dependencies are the Debian FS-UAE build dependencies: autotools, m4,
gettext, pkg-config, C/C++ compiler, SDL2/GLib/OpenAL/Freetype/X11/PNG/zlib/
JPEG/GLEW development packages and zip. `build.sh NEW-WORKDIR` builds a separate
installation. Rootless extraction needs tool data paths and pkg-config sysroot
configured for its extraction directory; the script assumes standard installed
development packages. The build deliberately retains JIT, FPU and RTG support.

Use `QRX_FSUAE_BIN=/absolute/path/to/fs-uae` to select a separate emulator in
the scenario runner. `bench-aros-doom.py --fsuae-jit on` explicitly enables
FS-UAE JIT and verifies its effective state in the emulator log; `--jit`
independently selects QRX86 mode. A setting in a configuration file alone is
not proof of a successful AROS boot or a valid FPS result.

Verified target: CPU68060, MMU off, FS-UAE v3.2.35 with the attached patch.
Both the SDK ABI probe and the complete native QRX86 differential/safety suite
pass under FS-UAE JIT (including SMC, remapping, fault and budget checks).
Use the patched executable with:

```sh
source tools/qrx86/fsuae/enable.sh /absolute/install/bin/fs-uae
```

The profile enables FS-UAE JIT, unlimited CPU speed, non-cycle-exact execution,
warp off and dummy host audio. It is for emulator throughput measurements,
not physical Amiga cycle accuracy. To measure QRX86, pass `--fsuae-jit on`
and either `--jit off` or `--jit experimental` explicitly. Use `--no-wipe`
for matching rendered game snapshots: time-dependent screen melt makes early
frame hashes differ between fast runs. The selected config uses a relative
Windows filename; an Amiga `SYS:` path passed into the Windows executable
silently fails to load the file.

QEMU10's m68k system targets are AN5206, MCF5208, NeXTcube, Q800 and virt:
https://github.com/qemu/qemu/blob/v10.0.0/hw/m68k/meson.build
There is no Amiga machine. This Amiga-specific AROS HUNK/ROM build therefore
cannot run unchanged under either qemu-m68k Linux user mode or stock
qemu-system-m68k. A port to a supported machine needs corresponding platform
and device support. AROS x86 under QEMU is a different guest architecture.

### FPU qualification

With CPU+FPU JIT, all six QRX86 off/on runs match one another, but the
no-host-JIT control differs in two pixels out of64,000: positions(125,0) and
(233,124). Turning off only `uae_compfpu` makes the entire frame byte-identical
to the interpreter control. `enable.sh` therefore selects CPU JIT with FPU
fallback. CPU-only probe: 32frames/16.180904s, 1.977640FPS. No-host-JIT control:
33frames/193.064745s, 0.170927FPS. Both snapshot SHA-256:
`6cea8144e940b8f60ce5a2340f9d1459047c9f3230260b6890587c15e48520e1`.
The small CPU+FPU timing advantage does not override the observed image
mismatch. FPU instructions are still emulated, not disabled.
