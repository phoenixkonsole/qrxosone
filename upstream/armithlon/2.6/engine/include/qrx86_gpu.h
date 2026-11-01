#ifndef QRX86_GPU_H
#define QRX86_GPU_H
/* Native-side image upload ABI for the existing QRX86 Win32 GDI/DirectDraw
 * surface provider. This is not a Direct3D DLL, GL driver or ABI trampoline. */
#include "qrx_accel.h"
#include <stdint.h>
#include <stdbool.h>
/* 32bpp: host uint32_t values 0x00RRGGBB (qsurf.pix).
 * 16bpp: host uint16_t values RGB565; 8bpp: palette indices + host RGB888
 * palette values. Host-pitch is in BYTES, positive, rows top-to-bottom. */
#define QRX86_PIX_XRGB32 1
#define QRX86_PIX_RGB565 2
#define QRX86_PIX_PAL8 3
void qrx86_gpu_attach(QRXAccelStream *gpu);
/* Hook for QRX86 headless backend present() (32-bit qsurf.pix). */
void qrx86_gpu_backend_present(const uint32_t *pixels,int width,int height);
bool qrx86_gpu_present(QRXAccelStream *gpu, const void *pixels,
                       uint32_t width,uint32_t height,uint32_t pitch,
                       unsigned format,const uint32_t *palette);
#endif
