# Linux i386 GUI support: embedded X11 subset

`qrx-i386` now serves local display `:0` inside the Linux compatibility layer.
An i386 client connects to `/tmp/.X11-unix/X0` (pathname or Linux abstract Unix
socket). No external X server, AROS TCP stack, host network listener or Linux
kernel is required. Set the guest display explicitly:

```sh
qrx-i386 -e DISPLAY=:0 /path/to/i386-client
```

On AROS, each supported top-level X window becomes an Intuition window on the
public screen. Truecolor uses CyberGraphX WritePixelArray; palette/planar output
uses acquired pens and WriteChunkyPixels, following the existing Win32 backend's
presentation approach. Window resources and ports are owned by this backend.
On other hosts the current backend is headless. Set the **host** variable
`QRX_X11_DUMP_DIR` to an existing directory to save window PPMs.

This is an initial, bounded X11 implementation, **not a complete Xorg server or
verified GTK/Qt desktop**. Its exact wire subset is useful for small clients that
create top-level windows and draw rectangles or ZPixmap images. Programs outside
this subset receive X errors; they must not be described as compatible.

## Implemented surface

- X11 protocol 11 setup, little- and big-endian clients, stream fragmentation,
  sequence numbers, bounded request/reply queues, protocol errors.
- Local AF_UNIX/SOCK_STREAM socket/connect, old i386 socketcall and direct socket
  syscalls; read/write, readv/writev, sendmsg/recvmsg without ancillary data,
  descriptor sharing/close, FIONREAD and O_NONBLOCK.
- poll and select recognize actual X reply/event availability. Waiting for AROS
  input does not trigger the cooperative scheduler's pipe-deadlock termination.
- Create/Destroy/Map/UnmapWindow, a restricted ChangeWindowAttributes,
  GetWindowAttributes, GetGeometry, QueryTree, InternAtom, WM_NAME/STRING title,
  GetProperty for WM_NAME, QueryPointer (stationary fallback), GetInputFocus.
- Create/Change/FreeGC with GXcopy, full plane mask, foreground/background;
  ClearArea, PolyFillRectangle, depth-24/32-bpp ZPixmap PutImage.
- Expose and MapNotify; native mouse motion/buttons, basic character key events
  and close notification. Key modifiers, full keymap/focus semantics and
  WM_DELETE_WINDOW negotiation remain incomplete.
- QueryExtension/ListExtensions explicitly report no extensions.

Limits: 32 top-level windows per connection, 64 GCs, 64 dynamic atoms, 1024x768
maximum backing image, 1 MiB input/output queues. Connections have independent
resource namespaces. Authentication is not negotiated because this endpoint is
an in-process compatibility service, not a network server.

Not implemented: child windows/reparenting, resize/configure requests, pixmaps,
fonts/text requests, arbitrary properties/selections/clipboard, window manager
protocols, RENDER, SHM, GLX, XInput, XRandR or shared resources across clients.
The existing pselect6/ppoll stubs are not qualified for X clients; use poll/select.
General Xlib/XCB applications and toolkit applications have NOT been qualified.
PutImage is CPU rendering; this is not a new Linux 3D acceleration claim.

## Reproducible checks

From the repository root on Linux x86_64 with native GCC/binutils and sanitizers:

```sh
ROOT="$PWD/qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest"
bash "$ROOT/x86-runtime-22.5.12/tests/run-x11-tests.sh" /tmp/qrx-x11-check
```

The protocol test runs with AddressSanitizer/UBSan and checks both byte orders,
fragmentation, invalid setup, unsupported requests and all 256 truncated opcodes.
The client is an actual freestanding static ELF32/i386 binary compiled with
`-m32 -nostdlib`, run through QRX, not a host C mock. It checks socketcall,
fragmented setup, writev, recvmsg, FIONREAD, poll/select readiness, EAGAIN,
Expose sequence and BadDrawable. The test checks every one of the 3072 pixels
of the resulting window.

The normal `build-target.sh` also links this server and the AROS backend into
qrx-i386. AROS validation uses the existing FS-UAE scenario harness, original
checksummed AROS ROM/ISO and the experimental SDK adapter. Reproduction inputs
and results are in `benchmarks/armithlon-integration`; the screenshot run holds
the client open for capture and is distinct from the finite client test.

## Observed qualification

- Sanitized protocol tests and actual i386 client with pixel comparison: PASS.
- 68060 AROS cross-build: PASS.
- Actual i386 client inside AROS under stock FS-UAE 3.1.66, host JIT off: PASS.
- Native public-screen window, palette output: observed in screenshot.
- Native keyboard/mouse interaction and truecolor presentation: NOT RUN.
- Physical Amiga, macOS frontend, arbitrary Linux GUI applications: NOT RUN.

A full desktop compatibility layer needs the missing X11 primitives and tests
against real Xlib/XCB/toolkit clients before broader compatibility is claimed.
