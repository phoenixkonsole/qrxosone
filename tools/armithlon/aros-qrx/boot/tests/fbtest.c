/* Draws into the ARMithlon 68100EC64 board's framebuffer: a gradient and a frame, 320 x 200 XRGB32.
 * There is no MMU, so a program reaches the board's registers in the device window directly. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define REG(o) ((volatile void *)(0xFFFFFFFF00000000UL + (o)))
#define R32(o) (*(volatile uint32_t *)REG(o))
#define R64(o) (*(volatile uint64_t *)REG(o))

int main(void)
{
    const uint32_t w = 320, h = 200;
    uint32_t *fb;
    uint32_t x, y;

    if (R64(0x0000) != 0x5152584543363442UL) { printf("no ARMithlon board\n"); return 20; }
    fb = malloc(w * h * 4);
    if (!fb) return 20;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            fb[y * w + x] = (x == 0 || y == 0 || x == w - 1 || y == h - 1) ? 0xFFFFFF : (x * 255 / w) << 16 | (y * 255 / h) << 8 | 0x80;
    R64(0x0100) = (uint64_t)(uintptr_t)fb;
    R32(0x0108) = w;
    R32(0x010C) = h;
    R32(0x0110) = w * 4;
    R32(0x0114) = 5;
    R32(0x0118) = 1;
    R32(0x011C) = 1;
    printf("framebuffer %u x %u at %p, frames %u\n", (unsigned)w, (unsigned)h, (void *)fb, (unsigned)R32(0x011C));
    return 0;
}
