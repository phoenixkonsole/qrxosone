/*
 * qrx_crypto.h - small, dependency-free crypto for QRX volume images.
 *
 * AES-256-CTR + HMAC-SHA256 + PBKDF2-HMAC-SHA256.  No libcrypto, no
 * OpenSSL: the 68k AROS SDK does not ship either, and a volume image
 * must be openable on a plain QRX OS One installation.
 */
#ifndef QRX_CRYPTO_H
#define QRX_CRYPTO_H

#define QRX_AES256_KEYLEN   32
#define QRX_SHA256_LEN      32
#define QRX_HMAC_LEN        32
#define QRX_SALT_LEN        16
#define QRX_IV_LEN          16

struct qrx_aes256 {
    unsigned int rk[60];      /* 15 round keys, 4 bytes each */
};

struct qrx_sha256 {
    unsigned int h[8];
    unsigned long long bits;
    unsigned char buf[64];
    unsigned int used;
};

struct qrx_hmac256 {
    struct qrx_sha256 inner;
    struct qrx_sha256 outer;
};

struct qrx_ctr {
    struct qrx_aes256 aes;
    unsigned char counter[QRX_IV_LEN];
    unsigned char stream[16];
    unsigned int pos;
};

void qrx_aes256_init(struct qrx_aes256 *ctx, const unsigned char key[32]);
void qrx_aes256_encrypt(const struct qrx_aes256 *ctx,
                        const unsigned char in[16], unsigned char out[16]);

void qrx_sha256_init(struct qrx_sha256 *ctx);
void qrx_sha256_update(struct qrx_sha256 *ctx, const void *data, unsigned int len);
void qrx_sha256_final(struct qrx_sha256 *ctx, unsigned char out[32]);
void qrx_sha256(const void *data, unsigned int len, unsigned char out[32]);

void qrx_hmac256_init(struct qrx_hmac256 *ctx, const unsigned char *key, unsigned int keylen);
void qrx_hmac256_update(struct qrx_hmac256 *ctx, const void *data, unsigned int len);
void qrx_hmac256_final(struct qrx_hmac256 *ctx, unsigned char out[32]);

void qrx_pbkdf2_sha256(const unsigned char *pass, unsigned int passlen,
                       const unsigned char *salt, unsigned int saltlen,
                       unsigned int iterations,
                       unsigned char *out, unsigned int outlen);

void qrx_ctr_init(struct qrx_ctr *ctx, const unsigned char key[32],
                  const unsigned char iv[16]);
void qrx_ctr_crypt(struct qrx_ctr *ctx, void *data, unsigned int len);

/* Best effort entropy on AROS/AmigaOS; mixed through SHA-256. */
void qrx_random_bytes(void *out, unsigned int len);

#endif
