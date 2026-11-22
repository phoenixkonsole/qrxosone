/* Host test: decodes the icons of the files given and writes one PPM montage
 * over a grey background. tools/armithlon/desktop/test-wicon.sh runs it. */
#include <stdio.h>
#include "qrx_wicon.h"
static int rd(void *c, uint32_t off, void *b, uint32_t n) {
    FILE *f = c;
    return !fseek(f, (long)off, SEEK_SET) && fread(b, 1, n, f) == n;
}
int main(int argc, char **argv) {
    enum { COLS = 12, CELL = 40 };
    int n = argc - 2, rows = (n + COLS - 1) / COLS, ok = 0, W = COLS * CELL, H = rows * CELL;
    unsigned char *px = malloc((size_t)W * H * 3);
    FILE *o = fopen(argv[1], "wb");
    memset(px, 128, (size_t)W * H * 3);
    for (int i = 0; i < n; i++) {
        uint32_t a[QRX_WICON_SIZE * QRX_WICON_SIZE];
        FILE *f = fopen(argv[i + 2], "rb");
        size_t l = strlen(argv[i + 2]);
        int is_ico, got;
        if (!f) continue;
        is_ico = l > 4 && !strcmp(argv[i + 2] + l - 4, ".ico");
        got = is_ico ? qrx_wicon_from_ico(rd, f, a) : qrx_wicon_from_pe(rd, f, a);
        fclose(f);
        printf("%s %s\n", got ? "icon" : "none", argv[i + 2]);
        if (!got) continue;
        ok++;
        for (int y = 0; y < QRX_WICON_SIZE; y++)
            for (int x = 0; x < QRX_WICON_SIZE; x++) {
                uint32_t v = a[y * QRX_WICON_SIZE + x], al = v >> 24;
                unsigned char *p = px + (((i / COLS) * CELL + y + 4) * W + (i % COLS) * CELL + x + 4) * 3;
                p[0] = (unsigned char)((((v >> 16) & 255) * al + 128 * (255 - al)) / 255);
                p[1] = (unsigned char)((((v >> 8) & 255) * al + 128 * (255 - al)) / 255);
                p[2] = (unsigned char)(((v & 255) * al + 128 * (255 - al)) / 255);
            }
    }
    fprintf(o, "P6\n%d %d\n255\n", W, H);
    fwrite(px, 3, (size_t)W * H, o);
    fclose(o);
    printf("%d of %d have an icon\n", ok, n);
    return 0;
}
