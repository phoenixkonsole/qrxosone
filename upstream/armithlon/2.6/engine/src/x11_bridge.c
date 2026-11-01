/* ARMITHLON 1.2: Linux X11 i386 compatibility framebuffer -> Metal IPC.
 * Built only into native host QRX86, never into Amiga m68k guest binaries.
 * No guest pointers cross the socket. Bounds are checked prior to scaling.
 */
#include "qrx_x11_bridge.h"
#include "qrx86_ipc.h"
#include <stdlib.h>
#include <stdint.h>

bool qrx_x11_bridge_convert(const uint32_t *src, uint32_t sw, uint32_t sh,
                            uint32_t *dst, size_t capacity,
                            uint32_t *dw, uint32_t *dh) {
    if (dw) *dw=0;
    if (dh) *dh=0;
    if (!src || !dst || !dw || !dh || !sw || !sh ||
        sw>QRX_X11_SRC_MAX_W || sh>QRX_X11_SRC_MAX_H) return false;
    uint32_t w=sw,h=sh;
    if (w>QRX_X11_DST_MAX_W || h>QRX_X11_DST_MAX_H) {
        if ((uint64_t)sw*QRX_X11_DST_MAX_H > (uint64_t)sh*QRX_X11_DST_MAX_W) {
            w=QRX_X11_DST_MAX_W;
            h=(uint32_t)(((uint64_t)sh*w+sw/2)/sw);
        } else {
            h=QRX_X11_DST_MAX_H;
            w=(uint32_t)(((uint64_t)sw*h+sh/2)/sh);
        }
        if (!w) w=1;
        if (!h) h=1;
    }
    if (capacity < (size_t)w*h || w>QRX_X11_DST_MAX_W || h>QRX_X11_DST_MAX_H) return false;
    for (uint32_t y=0;y<h;y++) for (uint32_t x=0;x<w;x++) {
        uint32_t sy=(uint32_t)((uint64_t)y*sh/h);
        uint32_t sx=(uint32_t)((uint64_t)x*sw/w);
        dst[(size_t)y*w+x]=src[(size_t)sy*sw+sx]&0x00ffffffu;
    }
    *dw=w; *dh=h;return true;
}

bool qrx_x11_bridge_present(const uint32_t *pixels, uint32_t width, uint32_t height) {
    const char *socket_path=getenv("ARMITHLON_QRX86_SOCKET");
    if (!socket_path || !*socket_path) return false;
    if (!pixels || !width || !height || width>QRX_X11_SRC_MAX_W || height>QRX_X11_SRC_MAX_H)
        return false;
    /* Common X11 windows fit the host frame transport: avoid the extra
       allocation/resampling on every PutImage/PolyFillRectangle call. */
    if (width <= QRX_X11_DST_MAX_W && height <= QRX_X11_DST_MAX_H)
        return qrx86_ipc_send(socket_path,pixels,width,height);
    uint32_t w,h;
    uint32_t *tmp=(uint32_t*)malloc((size_t)QRX_X11_DST_MAX_W*QRX_X11_DST_MAX_H*sizeof(*tmp));
    if (!tmp) return false;
    bool ok=qrx_x11_bridge_convert(pixels,width,height,tmp,
                                  (size_t)QRX_X11_DST_MAX_W*QRX_X11_DST_MAX_H,&w,&h);
    if (ok) ok=qrx86_ipc_send(socket_path,tmp,w,h);
    free(tmp);
    return ok;
}
