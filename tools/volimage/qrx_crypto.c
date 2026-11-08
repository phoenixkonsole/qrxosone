/*
 * qrx_crypto.c - AES-256-CTR, SHA-256, HMAC-SHA256 and PBKDF2 for QRX.
 *
 * Straightforward portable C; the implementation is deliberately small
 * and slow but uses only 32-bit operations so it behaves identically on
 * m68k and on the build host.
 */
#include "qrx_crypto.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* AES-256                                                             */
/* ------------------------------------------------------------------ */

static const unsigned char sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const unsigned char rcon[15] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36,0x6c,0xd8,0xab,0x4d
};

static unsigned int sub_word(unsigned int w)
{
    return ((unsigned int)sbox[(w >> 24) & 0xff] << 24) |
           ((unsigned int)sbox[(w >> 16) & 0xff] << 16) |
           ((unsigned int)sbox[(w >> 8) & 0xff] << 8) |
           ((unsigned int)sbox[w & 0xff]);
}

static unsigned int rot_word(unsigned int w)
{
    return (w << 8) | (w >> 24);
}

void qrx_aes256_init(struct qrx_aes256 *ctx, const unsigned char key[32])
{
    unsigned int w[60];
    int i;

    for (i = 0; i < 8; ++i) {
        w[i] = ((unsigned int)key[4 * i] << 24) |
               ((unsigned int)key[4 * i + 1] << 16) |
               ((unsigned int)key[4 * i + 2] << 8) |
               (unsigned int)key[4 * i + 3];
    }
    for (i = 8; i < 60; ++i) {
        unsigned int temp = w[i - 1];
        if ((i & 7) == 0)
            temp = sub_word(rot_word(temp)) ^ ((unsigned int)rcon[i / 8] << 24);
        else if ((i & 7) == 4)
            temp = sub_word(temp);
        w[i] = w[i - 8] ^ temp;
    }
    for (i = 0; i < 60; ++i) ctx->rk[i] = w[i];
}

static unsigned char gm(unsigned char a, unsigned char b)
{
    unsigned char p = 0;
    int i;
    for (i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        b >>= 1;
        if (a & 0x80) a = (unsigned char)((a << 1) ^ 0x1b);
        else a <<= 1;
    }
    return p;
}

void qrx_aes256_encrypt(const struct qrx_aes256 *ctx,
                        const unsigned char in[16], unsigned char out[16])
{
    unsigned char s[16];
    unsigned char t[16];
    int round, i, c, r;

    memcpy(s, in, 16);
    for (i = 0; i < 4; ++i) {
        unsigned int k = ctx->rk[i];
        s[4 * i]     ^= (unsigned char)(k >> 24);
        s[4 * i + 1] ^= (unsigned char)(k >> 16);
        s[4 * i + 2] ^= (unsigned char)(k >> 8);
        s[4 * i + 3] ^= (unsigned char)k;
    }
    for (round = 1; round <= 14; ++round) {
        for (i = 0; i < 16; ++i) s[i] = sbox[s[i]];

        /* ShiftRows on the column-major state. */
        for (c = 0; c < 4; ++c)
            for (r = 0; r < 4; ++r)
                t[4 * c + r] = s[4 * ((c + r) & 3) + r];
        memcpy(s, t, 16);

        if (round != 14) {
            for (c = 0; c < 4; ++c) {
                unsigned char a0 = s[4 * c], a1 = s[4 * c + 1];
                unsigned char a2 = s[4 * c + 2], a3 = s[4 * c + 3];
                s[4 * c]     = gm(a0, 2) ^ gm(a1, 3) ^ a2 ^ a3;
                s[4 * c + 1] = a0 ^ gm(a1, 2) ^ gm(a2, 3) ^ a3;
                s[4 * c + 2] = a0 ^ a1 ^ gm(a2, 2) ^ gm(a3, 3);
                s[4 * c + 3] = gm(a0, 3) ^ a1 ^ a2 ^ gm(a3, 2);
            }
        }
        for (i = 0; i < 4; ++i) {
            unsigned int k = ctx->rk[round * 4 + i];
            s[4 * i]     ^= (unsigned char)(k >> 24);
            s[4 * i + 1] ^= (unsigned char)(k >> 16);
            s[4 * i + 2] ^= (unsigned char)(k >> 8);
            s[4 * i + 3] ^= (unsigned char)k;
        }
    }
    memcpy(out, s, 16);
}

/* ------------------------------------------------------------------ */
/* SHA-256                                                             */
/* ------------------------------------------------------------------ */

static const unsigned int k256[64] = {
    0x428a2f98UL,0x71374491UL,0xb5c0fbcfUL,0xe9b5dba5UL,0x3956c25bUL,0x59f111f1UL,
    0x923f82a4UL,0xab1c5ed5UL,0xd807aa98UL,0x12835b01UL,0x243185beUL,0x550c7dc3UL,
    0x72be5d74UL,0x80deb1feUL,0x9bdc06a7UL,0xc19bf174UL,0xe49b69c1UL,0xefbe4786UL,
    0x0fc19dc6UL,0x240ca1ccUL,0x2de92c6fUL,0x4a7484aaUL,0x5cb0a9dcUL,0x76f988daUL,
    0x983e5152UL,0xa831c66dUL,0xb00327c8UL,0xbf597fc7UL,0xc6e00bf3UL,0xd5a79147UL,
    0x06ca6351UL,0x14292967UL,0x27b70a85UL,0x2e1b2138UL,0x4d2c6dfcUL,0x53380d13UL,
    0x650a7354UL,0x766a0abbUL,0x81c2c92eUL,0x92722c85UL,0xa2bfe8a1UL,0xa81a664bUL,
    0xc24b8b70UL,0xc76c51a3UL,0xd192e819UL,0xd6990624UL,0xf40e3585UL,0x106aa070UL,
    0x19a4c116UL,0x1e376c08UL,0x2748774cUL,0x34b0bcb5UL,0x391c0cb3UL,0x4ed8aa4aUL,
    0x5b9cca4fUL,0x682e6ff3UL,0x748f82eeUL,0x78a5636fUL,0x84c87814UL,0x8cc70208UL,
    0x90befffaUL,0xa4506cebUL,0xbef9a3f7UL,0xc67178f2UL
};

#define ROR32(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(struct qrx_sha256 *ctx, const unsigned char *p)
{
    unsigned int w[64], a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; ++i)
        w[i] = ((unsigned int)p[4 * i] << 24) | ((unsigned int)p[4 * i + 1] << 16) |
               ((unsigned int)p[4 * i + 2] << 8) | (unsigned int)p[4 * i + 3];
    for (i = 16; i < 64; ++i) {
        unsigned int s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned int s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];
    for (i = 0; i < 64; ++i) {
        unsigned int s1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        unsigned int ch = (e & f) ^ ((~e) & g);
        unsigned int t1 = h + s1 + ch + k256[i] + w[i];
        unsigned int s0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void qrx_sha256_init(struct qrx_sha256 *ctx)
{
    ctx->h[0] = 0x6a09e667UL; ctx->h[1] = 0xbb67ae85UL;
    ctx->h[2] = 0x3c6ef372UL; ctx->h[3] = 0xa54ff53aUL;
    ctx->h[4] = 0x510e527fUL; ctx->h[5] = 0x9b05688cUL;
    ctx->h[6] = 0x1f83d9abUL; ctx->h[7] = 0x5be0cd19UL;
    ctx->bits = 0;
    ctx->used = 0;
}

void qrx_sha256_update(struct qrx_sha256 *ctx, const void *data, unsigned int len)
{
    const unsigned char *p = (const unsigned char *)data;
    ctx->bits += (unsigned long long)len * 8ULL;
    while (len) {
        unsigned int room = 64 - ctx->used;
        unsigned int take = len < room ? len : room;
        memcpy(ctx->buf + ctx->used, p, take);
        ctx->used += take;
        p += take;
        len -= take;
        if (ctx->used == 64) {
            sha256_block(ctx, ctx->buf);
            ctx->used = 0;
        }
    }
}

void qrx_sha256_final(struct qrx_sha256 *ctx, unsigned char out[32])
{
    unsigned int i;
    ctx->buf[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        while (ctx->used < 64) ctx->buf[ctx->used++] = 0;
        sha256_block(ctx, ctx->buf);
        ctx->used = 0;
    }
    while (ctx->used < 56) ctx->buf[ctx->used++] = 0;
    for (i = 0; i < 8; ++i)
        ctx->buf[63 - i] = (unsigned char)(ctx->bits >> (8 * i));
    sha256_block(ctx, ctx->buf);
    for (i = 0; i < 8; ++i) {
        out[4 * i]     = (unsigned char)(ctx->h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(ctx->h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(ctx->h[i] >> 8);
        out[4 * i + 3] = (unsigned char)ctx->h[i];
    }
}

void qrx_sha256(const void *data, unsigned int len, unsigned char out[32])
{
    struct qrx_sha256 ctx;
    qrx_sha256_init(&ctx);
    qrx_sha256_update(&ctx, data, len);
    qrx_sha256_final(&ctx, out);
}

/* ------------------------------------------------------------------ */
/* HMAC-SHA256 and PBKDF2                                              */
/* ------------------------------------------------------------------ */

void qrx_hmac256_init(struct qrx_hmac256 *ctx, const unsigned char *key, unsigned int keylen)
{
    unsigned char k[64];
    unsigned char pad[64];
    unsigned char tmp[32];
    int i;

    memset(k, 0, sizeof(k));
    if (keylen > 64) {
        qrx_sha256(key, keylen, tmp);
        memcpy(k, tmp, 32);
    } else {
        memcpy(k, key, keylen);
    }
    for (i = 0; i < 64; ++i) pad[i] = (unsigned char)(k[i] ^ 0x36);
    qrx_sha256_init(&ctx->inner);
    qrx_sha256_update(&ctx->inner, pad, 64);
    for (i = 0; i < 64; ++i) pad[i] = (unsigned char)(k[i] ^ 0x5c);
    qrx_sha256_init(&ctx->outer);
    qrx_sha256_update(&ctx->outer, pad, 64);
}

void qrx_hmac256_update(struct qrx_hmac256 *ctx, const void *data, unsigned int len)
{
    qrx_sha256_update(&ctx->inner, data, len);
}

void qrx_hmac256_final(struct qrx_hmac256 *ctx, unsigned char out[32])
{
    unsigned char inner[32];
    qrx_sha256_final(&ctx->inner, inner);
    qrx_sha256_update(&ctx->outer, inner, 32);
    qrx_sha256_final(&ctx->outer, out);
}

void qrx_pbkdf2_sha256(const unsigned char *pass, unsigned int passlen,
                       const unsigned char *salt, unsigned int saltlen,
                       unsigned int iterations,
                       unsigned char *out, unsigned int outlen)
{
    unsigned int block = 1;
    unsigned int done = 0;

    if (iterations < 1) iterations = 1;
    while (done < outlen) {
        struct qrx_hmac256 ctx;
        unsigned char u[32], t[32];
        unsigned char counter[4];
        unsigned int i, take;

        counter[0] = (unsigned char)(block >> 24);
        counter[1] = (unsigned char)(block >> 16);
        counter[2] = (unsigned char)(block >> 8);
        counter[3] = (unsigned char)block;

        qrx_hmac256_init(&ctx, pass, passlen);
        qrx_hmac256_update(&ctx, salt, saltlen);
        qrx_hmac256_update(&ctx, counter, 4);
        qrx_hmac256_final(&ctx, u);
        memcpy(t, u, 32);
        for (i = 1; i < iterations; ++i) {
            qrx_hmac256_init(&ctx, pass, passlen);
            qrx_hmac256_update(&ctx, u, 32);
            qrx_hmac256_final(&ctx, u);
            {
                int j;
                for (j = 0; j < 32; ++j) t[j] ^= u[j];
            }
        }
        take = outlen - done < 32 ? outlen - done : 32;
        memcpy(out + done, t, take);
        done += take;
        ++block;
    }
}

/* ------------------------------------------------------------------ */
/* CTR mode and entropy                                                */
/* ------------------------------------------------------------------ */

void qrx_ctr_init(struct qrx_ctr *ctx, const unsigned char key[32],
                  const unsigned char iv[16])
{
    qrx_aes256_init(&ctx->aes, key);
    memcpy(ctx->counter, iv, 16);
    ctx->pos = 16;             /* force a fresh keystream block */
}

static void ctr_increment(unsigned char counter[16])
{
    int i;
    for (i = 15; i >= 0; --i) {
        if (++counter[i] != 0) break;
    }
}

void qrx_ctr_crypt(struct qrx_ctr *ctx, void *data, unsigned int len)
{
    unsigned char *p = (unsigned char *)data;
    while (len--) {
        if (ctx->pos >= 16) {
            qrx_aes256_encrypt(&ctx->aes, ctx->counter, ctx->stream);
            ctr_increment(ctx->counter);
            ctx->pos = 0;
        }
        *p++ ^= ctx->stream[ctx->pos++];
    }
}

void qrx_random_bytes(void *out, unsigned int len)
{
    unsigned char seed[64];
    unsigned char digest[32];
    struct DateStamp ds;
    struct Task *task = FindTask(NULL);
    unsigned int off = 0;
    unsigned int mix = 0;
    static unsigned int calls = 0;

    DateStamp(&ds);
    memset(seed, 0, sizeof(seed));
    memcpy(seed, &ds, sizeof(ds));
    memcpy(seed + 20, &task, 4);
    memcpy(seed + 24, &len, 4);
    memcpy(seed + 28, &off, 4);
    mix = (unsigned int)AvailMem(MEMF_ANY);
    memcpy(seed + 32, &mix, 4);
    memcpy(seed + 36, &calls, 4);
    ++calls;

    while (off < len) {
        unsigned int take;
        qrx_sha256(seed, sizeof(seed), digest);
        take = len - off < 32 ? len - off : 32;
        memcpy((unsigned char *)out + off, digest, take);
        off += take;
        memcpy(seed, digest, 32);      /* chain for the next block */
        ++seed[63];
    }
}
