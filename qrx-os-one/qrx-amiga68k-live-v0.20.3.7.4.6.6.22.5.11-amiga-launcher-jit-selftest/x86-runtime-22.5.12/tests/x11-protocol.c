#include "../linux/qrx_x11.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void endian(int be) {
    qxx *x = qxx_new();
    unsigned char h[12] = {0}, b[4096], r[8] = {43, 0, 0, 0};
    int n;
    assert(x);
    h[0] = be ? 'B' : 'l';
    h[be ? 3 : 2] = 11;
    assert(qxx_write(x, h, 1) == 1 && qxx_ready(x) == 0);
    assert(qxx_write(x, h + 1, 11) == 11);
    n = qxx_read(x, b, sizeof(b));
    assert(n == 128 && b[0] == 1 && b[be ? 3 : 2] == 11);
    r[be ? 3 : 2] = 1;
    assert(qxx_write(x, r, 4) == 4);
    assert(qxx_read(x, b, 32) == 32 && b[0] == 1 && b[be ? 3 : 2] == 1);
    /* Unsupported opcode and invalid zero length are distinct errors. */
    r[0] = 250;
    assert(qxx_write(x, r, 4) == 4);
    assert(qxx_read(x, b, 32) == 32 && b[0] == 0 && b[1] == 1 && b[10] == 250);
    r[2] = r[3] = 0;
    assert(qxx_write(x, r, 4) < 0);
    qxx_free(x);
}
int main(void) {
    qxx *x;
    unsigned char h[12] = {'l', 0, 11};
    unsigned char b[4096], p[4] = {1, 0, 1, 0};
    unsigned i;
    endian(0);
    endian(1);
    x = qxx_new();
    assert(x);
    assert(qxx_write(x, h, 12) == 12);
    assert(qxx_read(x, b, sizeof(b)) == 128);
    /* Every opcode with only a four-byte header: no out-of-bounds reads. */
    for (i = 0; i < 256; i++) {
        p[0] = (unsigned char)i;
        assert(qxx_write(x, p, 4) == 4);
        assert(qxx_read(x, b, sizeof(b)) >= 0);
    }
    qxx_free(x);
    x = qxx_new();
    h[0] = '?';
    assert(qxx_write(x, h, 12) < 0);
    qxx_free(x);
    puts("PASS X11 framing, both byte orders, errors, all truncated opcodes");
    return 0;
}
