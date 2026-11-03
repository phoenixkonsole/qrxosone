#ifndef QRX_X11_BRIDGE_H
#define QRX_X11_BRIDGE_H
/* ARMITHLON 1.2: native-host-only Linux i386 X11 window -> QRX GPU frame IPC.
   The source format is X11 server host uint32_t 0x00RRGGBB, top-to-bottom.
   This is NOT an X11 GLX driver or an Amiga/AROS m68k backend. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define QRX_X11_SRC_MAX_W 1024u
#define QRX_X11_SRC_MAX_H 768u
#define QRX_X11_DST_MAX_W 800u
#define QRX_X11_DST_MAX_H 450u
/* Deterministic aspect-preserving nearest-neighbour downscale, no upscaling.
   dst must have >= 800*450 uint32_t pixels, caller owns memory.
   Failure leaves dimensions zero and writes no destination pixels. */
bool qrx_x11_bridge_convert(const uint32_t *src, uint32_t sw, uint32_t sh,
                            uint32_t *dst, size_t dst_capacity_pixels,
                            uint32_t *dw, uint32_t *dh);
/* Does nothing if ARMITHLON_QRX86_SOCKET unset; native macOS/Linux only.
   Refuses source larger than X11 server's documented maximum. */
bool qrx_x11_bridge_present(const uint32_t *pixels, uint32_t width, uint32_t height);
#endif
