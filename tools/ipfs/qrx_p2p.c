/*
 * qrx_p2p.c - small native libp2p peer for QRX OS One.
 *
 * This is not a port of go-libp2p.  It implements the interoperable
 * subset the 68K node needs:
 *
 *   TCP + multistream-select + Noise_XX_25519_ChaChaPoly_SHA256
 *   + yamux/1.0.0
 *   + Bitswap 1.2.0
 *   + Kademlia FIND_NODE / GET_PROVIDERS / ADD_PROVIDER
 *
 * All cryptography comes from the audited, portable Monocypher library
 * (BSD-2-Clause/CC0) plus qrx_crypto's SHA-256/HMAC.  The implementation
 * is synchronous and handles one TCP connection at a time; that is a
 * deliberate 68K memory/latency trade-off, not a hidden background
 * daemon.
 */
#include "qrx_p2p.h"
#include "qrx_crypto.h"

#include "vendor/monocypher.h"
#include "vendor/monocypher-ed25519.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "qrx_net.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define P2P_MAX_FRAME      32768
#define P2P_MAX_STREAM_MSG 262144UL
#define P2P_MAX_PROTO      96
#define P2P_MAX_BOOTSTRAP  16

#ifndef QRX_P2P_DEBUG
#define QRX_P2P_DEBUG 0
#endif
#if QRX_P2P_DEBUG
#define P2P_TRACE(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#else
#define P2P_TRACE(...) ((void)0)
#endif

struct p2p_peer {
    char id[128];
    unsigned char id_bin[64];
    int id_len;
    char host[QRX_P2P_MAX_HOST];
    int port;
    int used;
    int tried;
};

struct p2p_provider {
    char key[160];              /* hex multihash */
    char peer_id[128];
    char host[QRX_P2P_MAX_HOST];
    int port;
};

struct qrx_p2p {
    char dir[256];
    char peer_id[128];
    unsigned char peer_id_bin[64];
    int peer_id_len;
    unsigned char ed_priv[64];
    unsigned char ed_pub[32];
    unsigned char ed_seed[32];
    struct qrx_p2p_backend be;
    int listen_port;
    int listen_fd;
    char announce_host[QRX_P2P_MAX_HOST];
    struct p2p_peer peers[QRX_P2P_MAX_PEERS];
    int npeers;
    struct p2p_provider providers[QRX_P2P_MAX_PROVIDERS];
    int nproviders;
    unsigned long ticks;
};

struct p2p_buf {
    unsigned char *p;
    unsigned long len;
    unsigned long cap;
};

struct p2p_reader {
    const unsigned char *p;
    unsigned long len;
    unsigned long pos;
};

struct p2p_conn {
    int fd;
    int initiator;
    unsigned long stream;
    struct noise_state *noise;
    unsigned long rpos, rlen;
};

static int conn_write(struct p2p_conn *c, const void *data, unsigned long len);
static int conn_read(struct p2p_conn *c, void *data, unsigned long cap,
                     unsigned long *outlen);

struct noise_state {
    int initiator;
    unsigned char s_priv[32], s_pub[32];
    unsigned char e_priv[32], e_pub[32];
    unsigned char re[32], rs[32];
    unsigned char h[32], ck[32];
    unsigned char k[32];
    unsigned long long n;
    int has_k;
    unsigned char send_key[32], recv_key[32];
    unsigned long long send_n, recv_n;
    unsigned long npos, nlen;
};

/* ------------------------------------------------------------------ */
/* small file helpers                                                  */
/* ------------------------------------------------------------------ */

static int p2p_mkdir(const char *path)
{
    BPTR l = CreateDir((CONST_STRPTR)path);
    if (l) UnLock(l);
    l = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (l) { UnLock(l); return 1; }
    return 0;
}

static int p2p_read_file(const char *path, void *data, int cap)
{
    BPTR f = Open((CONST_STRPTR)path, MODE_OLDFILE);
    LONG n;
    if (!f) return -1;
    n = Read(f, data, cap);
    Close(f);
    return (int)n;
}

static int p2p_write_file(const char *path, const void *data, int len)
{
    BPTR f = Open((CONST_STRPTR)path, MODE_NEWFILE);
    LONG n;
    if (!f) return 0;
    n = Write(f, (APTR)data, len);
    Close(f);
    return n == len;
}

/* ------------------------------------------------------------------ */
/* base58 / base32 / hex                                               */
/* ------------------------------------------------------------------ */

static const char b58alpha[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static int b58_value(int c)
{
    int i;
    for (i = 0; i < 58; ++i)
        if (b58alpha[i] == c) return i;
    return -1;
}

static int b58_encode(const unsigned char *in, int len, char *out, int cap)
{
    unsigned char d[128];
    int nd = 0, i, j, zeros = 0, n = 0;

    if (!out || cap < 2 || len < 0) return 0;
    while (zeros < len && in[zeros] == 0) ++zeros;
    for (i = zeros; i < len; ++i) {
        int carry = in[i];
        for (j = 0; j < nd; ++j) {
            carry += d[j] * 256;
            d[j] = (unsigned char)(carry % 58);
            carry /= 58;
        }
        while (carry) {
            if (nd >= (int)sizeof(d)) return 0;
            d[nd++] = (unsigned char)(carry % 58);
            carry /= 58;
        }
    }
    for (i = 0; i < zeros; ++i) {
        if (n + 1 >= cap) return 0;
        out[n++] = '1';
    }
    for (i = nd - 1; i >= 0; --i) {
        if (n + 1 >= cap) return 0;
        out[n++] = b58alpha[d[i]];
    }
    out[n] = 0;
    return n;
}

static int b58_decode(const char *in, unsigned char *out, int cap)
{
    unsigned char d[128];
    int nd = 0, n = 0, zeros = 0, i, j;
    if (!in || !out || cap < 1) return 0;
    while (in[zeros] == '1') ++zeros;
    for (i = zeros; in[i]; ++i) {
        int v = b58_value((unsigned char)in[i]);
        int carry;
        if (v < 0) return 0;
        carry = v;
        for (j = 0; j < nd; ++j) {
            carry += d[j] * 58;
            d[j] = (unsigned char)(carry & 0xff);
            carry >>= 8;
        }
        while (carry) {
            if (nd >= (int)sizeof(d)) return 0;
            d[nd++] = (unsigned char)(carry & 0xff);
            carry >>= 8;
        }
    }
    for (i = 0; i < zeros; ++i) {
        if (n >= cap) return 0;
        out[n++] = 0;
    }
    for (i = nd - 1; i >= 0; --i) {
        if (n >= cap) return 0;
        out[n++] = d[i];
    }
    return n;
}

static const char b32alpha[] = "abcdefghijklmnopqrstuvwxyz234567";

static int b32_decode_char(int c)
{
    int i;
    for (i = 0; i < 32; ++i)
        if (b32alpha[i] == c) return i;
    return -1;
}

static int b32_decode(const char *in, unsigned char *out, int cap)
{
    unsigned long acc = 0;
    int bits = 0, n = 0;
    if (!in || !out) return 0;
    while (*in) {
        int v = b32_decode_char((unsigned char)*in++);
        if (v < 0) return 0;
        acc = ((acc << 5) | (unsigned long)v) & 0xffffUL;
        bits += 5;
        while (bits >= 8) {
            bits -= 8;
            if (n >= cap) return 0;
            out[n++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }
    return n;
}

static int b32_encode(const unsigned char *in, int len, char *out, int cap)
{
    unsigned long acc = 0;
    int bits = 0, i, n = 0;
    for (i = 0; i < len; ++i) {
        acc = ((acc << 8) | in[i]) & 0xffffUL;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            if (n + 1 >= cap) return 0;
            out[n++] = b32alpha[(acc >> bits) & 31];
        }
    }
    if (bits) {
        if (n + 1 >= cap) return 0;
        out[n++] = b32alpha[(acc << (5 - bits)) & 31];
    }
    out[n] = 0;
    return n;
}

static void hex_encode(const unsigned char *in, int len, char *out, int cap)
{
    static const char h[] = "0123456789abcdef";
    int i, n = 0;
    for (i = 0; i < len && n + 2 < cap; ++i) {
        out[n++] = h[in[i] >> 4];
        out[n++] = h[in[i] & 15];
    }
    out[n] = 0;
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *in, unsigned char *out, int cap)
{
    int n = 0;
    while (in[0] && in[1]) {
        int a = hex_value((unsigned char)in[0]);
        int b = hex_value((unsigned char)in[1]);
        if (a < 0 || b < 0 || n >= cap) return 0;
        out[n++] = (unsigned char)((a << 4) | b);
        in += 2;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* protobuf helpers                                                    */
/* ------------------------------------------------------------------ */

static int pb_varint(struct p2p_buf *b, unsigned long v)
{
    if (b->len + 10 > b->cap) return 0;
    while (v >= 0x80) {
        b->p[b->len++] = (unsigned char)((v & 0x7f) | 0x80);
        v >>= 7;
    }
    b->p[b->len++] = (unsigned char)v;
    return 1;
}

static int pb_tag(struct p2p_buf *b, unsigned field, unsigned wire)
{
    return pb_varint(b, ((unsigned long)field << 3) | wire);
}

static int pb_bytes(struct p2p_buf *b, unsigned field, const void *data,
                    unsigned long len)
{
    if (!pb_tag(b, field, 2) || !pb_varint(b, len)) return 0;
    if (b->len + len > b->cap) return 0;
    if (len) memcpy(b->p + b->len, data, len);
    b->len += len;
    return 1;
}

static int pr_varint(struct p2p_reader *r, unsigned long *out)
{
    unsigned long v = 0;
    int shift = 0;
    while (r->pos < r->len && shift < 63) {
        unsigned char c = r->p[r->pos++];
        v |= (unsigned long)(c & 0x7f) << shift;
        if (!(c & 0x80)) { *out = v; return 1; }
        shift += 7;
    }
    return 0;
}

static int pr_bytes(struct p2p_reader *r, unsigned long len,
                    const unsigned char **out, unsigned long *outlen)
{
    if (r->pos + len > r->len) return 0;
    *out = r->p + r->pos;
    *outlen = len;
    r->pos += len;
    return 1;
}

static int pr_field(struct p2p_reader *r, unsigned long *field,
                    unsigned long *wire, unsigned long *vlen,
                    const unsigned char **v)
{
    unsigned long tag;
    if (!pr_varint(r, &tag)) return 0;
    *field = tag >> 3;
    *wire = tag & 7;
    *v = NULL;
    *vlen = 0;
    if (*wire == 0) {
        if (!pr_varint(r, vlen)) return 0;
    } else if (*wire == 2) {
        unsigned long n;
        if (!pr_varint(r, &n) || !pr_bytes(r, n, v, vlen)) return 0;
    } else if (*wire == 5) {
        if (!pr_bytes(r, 4, v, vlen)) return 0;
    } else if (*wire == 1) {
        if (!pr_bytes(r, 8, v, vlen)) return 0;
    } else {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* identity: Ed25519 -> libp2p PeerID                                   */
/* ------------------------------------------------------------------ */

static int identity_public_pb(const unsigned char pub[32],
                              unsigned char out[64], int *outlen)
{
    struct p2p_buf b;
    b.p = out;
    b.len = 0;
    b.cap = 64;
    if (!pb_tag(&b, 1, 0) || !pb_varint(&b, 1)) return 0;  /* Ed25519 */
    if (!pb_bytes(&b, 2, pub, 32)) return 0;
    *outlen = (int)b.len;
    return b.len == 36;
}

static int peer_id_from_public_pb(const unsigned char *pb, int len,
                                  char *text, int textcap,
                                  unsigned char *bin, int *binlen)
{
    unsigned char id[64];
    int n = 0;
    if (!pb || len <= 0 || len > 42 || n + 2 + len > (int)sizeof(id))
        return 0;
    id[n++] = 0x00;                               /* identity multihash */
    id[n++] = (unsigned char)len;
    memcpy(id + n, pb, len);
    n += len;
    if (bin) {
        memcpy(bin, id, n);
        *binlen = n;
    }
    return b58_encode(id, n, text, textcap) > 0;
}

static int parse_identity_pb(const unsigned char *pb, int len,
                             unsigned char pub[32])
{
    struct p2p_reader r;
    unsigned long f, w, vl;
    const unsigned char *v;
    int got_type = 0, got_pub = 0;
    r.p = pb;
    r.len = len;
    r.pos = 0;
    while (r.pos < r.len) {
        if (!pr_field(&r, &f, &w, &vl, &v)) return 0;
        if (f == 1 && w == 0) got_type = (vl == 1);
        else if (f == 2 && w == 2 && vl == 32) {
            memcpy(pub, v, 32);
            got_pub = 1;
        }
    }
    return got_type && got_pub;
}

static int derive_identity(struct qrx_p2p *p2p)
{
    unsigned char pb[64];
    int pblen;
    if (!identity_public_pb(p2p->ed_pub, pb, &pblen)) return 0;
    if (!peer_id_from_public_pb(pb, pblen, p2p->peer_id,
                                sizeof(p2p->peer_id), p2p->peer_id_bin,
                                &p2p->peer_id_len)) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Noise helpers                                                       */
/* ------------------------------------------------------------------ */

static void nonce12(unsigned long long n, unsigned char nonce[12])
{
    int i;
    memset(nonce, 0, 12);
    for (i = 0; i < 8; ++i) nonce[4 + i] = (unsigned char)(n >> (8 * i));
}

static void hmac256(const unsigned char *key, unsigned long keylen,
                    const unsigned char *data, unsigned long len,
                    unsigned char out[32])
{
    struct qrx_hmac256 h;
    qrx_hmac256_init(&h, key, (unsigned int)keylen);
    qrx_hmac256_update(&h, data, (unsigned int)len);
    qrx_hmac256_final(&h, out);
}

static void hkdf2(const unsigned char ck[32],
                  const unsigned char *ikm, unsigned long ikmlen,
                  unsigned char out1[32], unsigned char out2[32])
{
    unsigned char prk[32], t[64];
    hmac256(ck, 32, ikm, ikmlen, prk);
    t[0] = 1;
    hmac256(prk, 32, t, 1, out1);
    memcpy(t, out1, 32);
    t[32] = 2;
    hmac256(prk, 32, t, 33, out2);
    crypto_wipe(prk, sizeof(prk));
    crypto_wipe(t, sizeof(t));
}

static int aead_encrypt(const unsigned char key[32], unsigned long long n,
                        const unsigned char *ad, unsigned long adlen,
                        const unsigned char *pt, unsigned long ptlen,
                        unsigned char *out)
{
    unsigned char nonce[12], polykey[32], zero[64], lens[16], pad[16];
    crypto_poly1305_ctx pc;
    unsigned long i;
    unsigned long next;

    nonce12(n, nonce);
    memset(zero, 0, sizeof(zero));
    crypto_chacha20_ietf(polykey, zero, 32, key, nonce, 0);
    crypto_chacha20_ietf(out, pt, ptlen, key, nonce, 1);
    crypto_poly1305_init(&pc, polykey);
    if (adlen) crypto_poly1305_update(&pc, ad, adlen);
    if (adlen & 15) {
        memset(pad, 0, sizeof(pad));
        crypto_poly1305_update(&pc, pad, 16 - (adlen & 15));
    }
    if (ptlen) crypto_poly1305_update(&pc, out, ptlen);
    if (ptlen & 15) {
        memset(pad, 0, sizeof(pad));
        crypto_poly1305_update(&pc, pad, 16 - (ptlen & 15));
    }
    memset(lens, 0, sizeof(lens));
    for (i = 0; i < 8; ++i) {
        lens[i] = (unsigned char)(adlen >> (8 * i));
        lens[8 + i] = (unsigned char)(ptlen >> (8 * i));
    }
    crypto_poly1305_update(&pc, lens, 16);
    crypto_poly1305_final(&pc, out + ptlen);
    crypto_wipe(polykey, sizeof(polykey));
    crypto_wipe(&pc, sizeof(pc));
    (void)next;
    return 1;
}

static int aead_decrypt(const unsigned char key[32], unsigned long long n,
                        const unsigned char *ad, unsigned long adlen,
                        const unsigned char *ct, unsigned long ctlen,
                        unsigned char *out)
{
    unsigned char nonce[12], polykey[32], zero[64], lens[16], pad[16];
    unsigned char mac[16];
    crypto_poly1305_ctx pc;
    unsigned long i;

    if (ctlen < 16) return 0;
    ctlen -= 16;
    nonce12(n, nonce);
    memset(zero, 0, sizeof(zero));
    crypto_chacha20_ietf(polykey, zero, 32, key, nonce, 0);
    crypto_poly1305_init(&pc, polykey);
    if (adlen) crypto_poly1305_update(&pc, ad, adlen);
    if (adlen & 15) {
        memset(pad, 0, sizeof(pad));
        crypto_poly1305_update(&pc, pad, 16 - (adlen & 15));
    }
    if (ctlen) crypto_poly1305_update(&pc, ct, ctlen);
    if (ctlen & 15) {
        memset(pad, 0, sizeof(pad));
        crypto_poly1305_update(&pc, pad, 16 - (ctlen & 15));
    }
    memset(lens, 0, sizeof(lens));
    for (i = 0; i < 8; ++i) {
        lens[i] = (unsigned char)(adlen >> (8 * i));
        lens[8 + i] = (unsigned char)(ctlen >> (8 * i));
    }
    crypto_poly1305_update(&pc, lens, 16);
    crypto_poly1305_final(&pc, mac);
    if (crypto_verify16(mac, ct + ctlen) != 0) {
        crypto_wipe(polykey, sizeof(polykey));
        crypto_wipe(&pc, sizeof(pc));
        return 0;
    }
    if (ctlen) crypto_chacha20_ietf(out, ct, ctlen, key, nonce, 1);
    crypto_wipe(polykey, sizeof(polykey));
    crypto_wipe(&pc, sizeof(pc));
    return 1;
}

static void mix_hash(struct noise_state *n, const void *data, unsigned long len)
{
    struct qrx_sha256 s;
    qrx_sha256_init(&s);
    qrx_sha256_update(&s, n->h, 32);
    if (len) qrx_sha256_update(&s, data, (unsigned int)len);
    qrx_sha256_final(&s, n->h);
}

static void mix_key(struct noise_state *n, const unsigned char dh[32])
{
    unsigned char k1[32], k2[32];
    hkdf2(n->ck, dh, 32, k1, k2);
    memcpy(n->ck, k1, 32);
    memcpy(n->k, k2, 32);
    n->n = 0;
    n->has_k = 1;
    crypto_wipe(k1, sizeof(k1));
    crypto_wipe(k2, sizeof(k2));
}

static int dh_shared(unsigned char out[32], const unsigned char sk[32],
                     const unsigned char pk[32])
{
    unsigned char z = 0;
    int i;
    crypto_x25519(out, sk, pk);
    for (i = 0; i < 32; ++i) z |= out[i];
    return z != 0;
}

static int noise_encrypt(struct noise_state *n, const unsigned char *pt,
                         unsigned long len, unsigned char *out)
{
    if (!n->has_k) return 0;
    if (!aead_encrypt(n->k, n->n, n->h, 32, pt, len, out)) return 0;
    ++n->n;
    mix_hash(n, out, len + 16);
    return 1;
}

static int noise_decrypt(struct noise_state *n, const unsigned char *ct,
                         unsigned long len, unsigned char *out)
{
    if (!n->has_k || len < 16) return 0;
    if (!aead_decrypt(n->k, n->n, n->h, 32, ct, len, out)) return 0;
    ++n->n;
    mix_hash(n, ct, len);
    return 1;
}

static void noise_split(struct noise_state *n)
{
    unsigned char k1[32], k2[32];
    hkdf2(n->ck, NULL, 0, k1, k2);
    if (n->initiator) {
        memcpy(n->send_key, k1, 32);
        memcpy(n->recv_key, k2, 32);
    } else {
        memcpy(n->send_key, k2, 32);
        memcpy(n->recv_key, k1, 32);
    }
    n->send_n = n->recv_n = 0;
    crypto_wipe(k1, sizeof(k1));
    crypto_wipe(k2, sizeof(k2));
}

static int noise_generate_ephemeral(struct noise_state *n)
{
    qrx_random_bytes(n->e_priv, 32);
    crypto_x25519_public_key(n->e_pub, n->e_priv);
    return 1;
}

static int noise_generate_static(struct noise_state *n)
{
    qrx_random_bytes(n->s_priv, 32);
    crypto_x25519_public_key(n->s_pub, n->s_priv);
    return 1;
}

static int noise_payload(struct qrx_p2p *p2p, const unsigned char static_pub[32],
                         unsigned char *out, int cap, int *outlen)
{
    unsigned char identity_pb[64];
    unsigned char sig[64], signed_data[80];
    struct p2p_buf b;
    int pb_len, n;
    const char *prefix = "noise-libp2p-static-key:";
    int plen = (int)strlen(prefix);

    if (!identity_public_pb(p2p->ed_pub, identity_pb, &pb_len)) return 0;
    memcpy(signed_data, prefix, plen);
    memcpy(signed_data + plen, static_pub, 32);
    crypto_ed25519_sign(sig, p2p->ed_priv, signed_data, plen + 32);

    b.p = out;
    b.len = 0;
    b.cap = cap;
    if (!pb_bytes(&b, 1, identity_pb, pb_len)) return 0;
    if (!pb_bytes(&b, 2, sig, 64)) return 0;
    n = (int)b.len;
    *outlen = n;
    return n > 0;
}

static int noise_verify_payload(struct qrx_p2p *p2p,
                                const unsigned char *payload, int len,
                                const unsigned char static_pub[32],
                                const char *expected_peer,
                                char *remote_peer, int remote_cap)
{
    struct p2p_reader r;
    unsigned long f, w, vl;
    const unsigned char *v;
    const unsigned char *idpb = NULL, *sig = NULL;
    unsigned long idlen = 0, siglen = 0;
    unsigned char edpub[32], signed_data[80], bin[64];
    char text[128];
    int binlen, plen = (int)strlen("noise-libp2p-static-key:");

    r.p = payload;
    r.len = len;
    r.pos = 0;
    while (r.pos < r.len) {
        if (!pr_field(&r, &f, &w, &vl, &v)) return 0;
        if (f == 1 && w == 2) { idpb = v; idlen = vl; }
        else if (f == 2 && w == 2) { sig = v; siglen = vl; }
    }
    if (!idpb || !sig || siglen != 64 || idlen <= 0 || idlen > 64) return 0;
    if (!parse_identity_pb(idpb, (int)idlen, edpub)) return 0;
    memcpy(signed_data, "noise-libp2p-static-key:", plen);
    memcpy(signed_data + plen, static_pub, 32);
    if (crypto_ed25519_check(sig, edpub, signed_data, plen + 32) != 0)
        return 0;
    if (!peer_id_from_public_pb(idpb, (int)idlen, text, sizeof(text), bin, &binlen))
        return 0;
    if (expected_peer && *expected_peer && strcmp(expected_peer, text) != 0)
        return 0;
    if (remote_peer && remote_cap > 0) {
        strncpy(remote_peer, text, remote_cap - 1);
        remote_peer[remote_cap - 1] = 0;
    }
    (void)bin;
    (void)binlen;
    return 1;
}

/* ------------------------------------------------------------------ */
/* transport I/O                                                       */
/* ------------------------------------------------------------------ */

static int p2p_write_raw(int fd, const void *data, unsigned long len)
{
    const unsigned char *p = (const unsigned char *)data;
    while (len) {
        LONG n = send(fd, p, (LONG)len, 0);
        if (n <= 0) {
            P2P_TRACE("raw: send fd=%d len=%lu err=%d", fd, len,
                      qrx_net_last_error());
            return 0;
        }
        p += n;
        len -= (unsigned long)n;
    }
    return 1;
}

static int p2p_read_raw(int fd, void *data, unsigned long len)
{
    unsigned char *p = (unsigned char *)data;
    while (len) {
        LONG n = recv(fd, p, (LONG)len, 0);
        if (n <= 0) {
            P2P_TRACE("raw: recv fd=%d len=%lu err=%d", fd, len,
                      qrx_net_last_error());
            return 0;
        }
        p += n;
        len -= (unsigned long)n;
    }
    return 1;
}

static int noise_write_raw(struct noise_state *n, int fd,
                           const unsigned char *data, unsigned long len)
{
    static unsigned char frame[P2P_MAX_FRAME + 32];
    static unsigned char cipher[P2P_MAX_FRAME + 32];
    unsigned long clen;
    if (len > P2P_MAX_FRAME - 16) return 0;
    if (!aead_encrypt(n->send_key, n->send_n, NULL, 0, data, len, cipher))
        return 0;
    ++n->send_n;
    clen = len + 16;
    frame[0] = (unsigned char)(clen >> 8);
    frame[1] = (unsigned char)clen;
    memcpy(frame + 2, cipher, clen);
    return p2p_write_raw(fd, frame, clen + 2);
}

static int noise_read_raw(struct noise_state *n, int fd,
                          unsigned char *data, unsigned long cap,
                          unsigned long *outlen)
{
    static unsigned char frame[P2P_MAX_FRAME + 32];
    static unsigned char plain[P2P_MAX_FRAME + 32];
    unsigned char hdr[2];
    unsigned long clen, take;

    /* One Noise transport frame can contain several yamux frames.  Keep the
       decrypted remainder available to the next read instead of dropping it. */
    if (n->npos < n->nlen) {
        take = n->nlen - n->npos;
        if (take > cap) take = cap;
        memcpy(data, plain + n->npos, take);
        n->npos += take;
        *outlen = take;
        return 1;
    }

    for (;;) {
        if (!p2p_read_raw(fd, hdr, 2)) return 0;
        clen = ((unsigned long)hdr[0] << 8) | hdr[1];
        if (clen < 16 || clen > P2P_MAX_FRAME + 16) return 0;
        if (!p2p_read_raw(fd, frame, clen)) return 0;
        if (!aead_decrypt(n->recv_key, n->recv_n, NULL, 0, frame, clen, plain))
            return 0;
        ++n->recv_n;
        clen -= 16;
        n->npos = 0;
        n->nlen = clen;
        if (clen) break;
    }
    take = clen > cap ? cap : clen;
    memcpy(data, plain, take);
    n->npos = take;
    *outlen = take;
    return 1;
}

/* ------------------------------------------------------------------ */
/* multistream-select                                                  */
/* ------------------------------------------------------------------ */

static int ms_send(struct p2p_conn *c, const char *proto)
{
    unsigned char hdr[8], msg[320];
    unsigned long len = (unsigned long)strlen(proto) + 1;
    unsigned long plen = (unsigned long)strlen(proto);
    int n = 0;
    while (len >= 0x80) {
        hdr[n++] = (unsigned char)((len & 0x7f) | 0x80);
        len >>= 7;
    }
    hdr[n++] = (unsigned char)len;
    if ((unsigned long)n + plen + 1 > sizeof(msg)) return 0;
    memcpy(msg, hdr, (unsigned long)n);
    memcpy(msg + n, proto, plen);
    msg[n + plen] = '\n';
    return conn_write(c, msg, (unsigned long)n + plen + 1);
}

static int ms_recv(struct p2p_conn *c, char *out, int cap)
{
    unsigned long len = 0, got;
    int shift = 0, n = 0;
    unsigned char b;
    for (;;) {
        if (!conn_read(c, &b, 1, &got) || got != 1) return 0;
        len |= (unsigned long)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 28) return 0;
    }
    if (len < 1 || len > (unsigned long)cap) return 0;
    while (n < (int)len) {
        if (!conn_read(c, &b, 1, &got) || got != 1) return 0;
        out[n++] = (char)b;
    }
    if (n == 0 || out[n - 1] != '\n') return 0;
    out[n - 1] = 0;
    return 1;
}

static int ms_negotiate(struct p2p_conn *c, const char *wanted, int initiator)
{
    char buf[256];
    P2P_TRACE("ms: want=%s init=%d", wanted, initiator);
    if (!ms_send(c, "/multistream/1.0.0")) {
        P2P_TRACE("ms: hello send failed");
        return 0;
    }
    if (!ms_recv(c, buf, sizeof(buf))) {
        P2P_TRACE("ms: hello recv failed");
        return 0;
    }
    P2P_TRACE("ms: hello='%s'", buf);
    if (strcmp(buf, "/multistream/1.0.0")) return 0;
    if (initiator) {
        if (!ms_send(c, wanted)) {
            P2P_TRACE("ms: wanted send failed");
            return 0;
        }
        if (!ms_recv(c, buf, sizeof(buf))) {
            P2P_TRACE("ms: wanted recv failed");
            return 0;
        }
        P2P_TRACE("ms: reply='%s'", buf);
        return !strcmp(buf, wanted);
    }
    if (!ms_recv(c, buf, sizeof(buf))) return 0;
    if (!strcmp(buf, wanted)) return ms_send(c, wanted);
    return ms_send(c, "na") && 0;
}

/* ------------------------------------------------------------------ */
/* yamux                                                               */
/* ------------------------------------------------------------------ */

static int ym_write_frame(struct p2p_conn *c, unsigned char type,
                          unsigned short flags, unsigned long sid,
                          unsigned long length, const void *data)
{
    unsigned char h[12];
    h[0] = 0;
    h[1] = type;
    h[2] = (unsigned char)(flags >> 8);
    h[3] = (unsigned char)flags;
    h[4] = (unsigned char)(sid >> 24);
    h[5] = (unsigned char)(sid >> 16);
    h[6] = (unsigned char)(sid >> 8);
    h[7] = (unsigned char)sid;
    h[8] = (unsigned char)(length >> 24);
    h[9] = (unsigned char)(length >> 16);
    h[10] = (unsigned char)(length >> 8);
    h[11] = (unsigned char)length;
    P2P_TRACE("ym: write type=%u flags=%u sid=%lu len=%lu",
              (unsigned)type, (unsigned)flags, sid, length);
    if (!noise_write_raw(c->noise, c->fd, h, 12)) return 0;
    if (length && data) {
        if (!noise_write_raw(c->noise, c->fd, data, length)) return 0;
    }
    return 1;
}

static int ym_read_frame(struct p2p_conn *c, unsigned char *type,
                         unsigned short *flags, unsigned long *sid,
                         unsigned long *length, unsigned char *data,
                         unsigned long cap)
{
    unsigned char h[12];
    unsigned long n, got = 0;
    while (got < sizeof(h)) {
        if (!noise_read_raw(c->noise, c->fd, h + got, sizeof(h) - got, &n) ||
            !n) return 0;
        got += n;
    }
    *type = h[1];
    *flags = (unsigned short)(((unsigned short)h[2] << 8) | h[3]);
    *sid = ((unsigned long)h[4] << 24) | ((unsigned long)h[5] << 16) |
           ((unsigned long)h[6] << 8) | h[7];
    *length = ((unsigned long)h[8] << 24) | ((unsigned long)h[9] << 16) |
              ((unsigned long)h[10] << 8) | h[11];
    P2P_TRACE("ym: read type=%u flags=%u sid=%lu len=%lu",
              (unsigned)*type, (unsigned)*flags, *sid, *length);
    if (*type == 0) {
        if (*length > cap) return 0;
        got = 0;
        while (got < *length) {
            if (!noise_read_raw(c->noise, c->fd, data + got, *length - got, &n) ||
                !n) return 0;
            got += n;
        }
    }
    return 1;
}

/* Advertised receive window for a yamux stream.  A SYN with length 0 tells
   the peer that it may not send anything, so use a real window and replenish
   it as data is consumed. */
#define YM_WINDOW 262144UL

static int ym_open(struct p2p_conn *c)
{
    unsigned long sid = (c->initiator ? 1UL : 2UL);
    if (!ym_write_frame(c, 1, 1, sid, YM_WINDOW, NULL)) return 0;
    c->stream = sid;
    return 1;
}

static int ym_accept(struct p2p_conn *c)
{
    static unsigned char data[P2P_MAX_FRAME];
    unsigned char type;
    unsigned short flags;
    unsigned long sid, len;
    for (;;) {
        if (!ym_read_frame(c, &type, &flags, &sid, &len, data, sizeof(data)))
            return 0;
        if (type == 2) continue;
        if (type == 1 && (flags & 1)) {
            if (!ym_write_frame(c, 1, 2, sid, YM_WINDOW, NULL)) return 0;
            c->stream = sid;
            return 1;
        }
        if (type == 0 && (flags & 1)) {
            if (!ym_write_frame(c, 1, 2, sid, YM_WINDOW, NULL)) return 0;
            c->stream = sid;
            return 1;
        }
    }
}

static int ym_write(struct p2p_conn *c, const unsigned char *data,
                    unsigned long len)
{
    unsigned long sid = c->stream;
    while (len) {
        unsigned long take = len > 16384UL ? 16384UL : len;
        if (!ym_write_frame(c, 0, 0, sid, take, data)) return 0;
        data += take;
        len -= take;
    }
    return 1;
}

static int ym_read(struct p2p_conn *c, unsigned char *data,
                   unsigned long cap, unsigned long *outlen)
{
    static unsigned char tmp[16384];
    unsigned char type;
    unsigned short flags;
    unsigned long sid, len;
    for (;;) {
        if (!ym_read_frame(c, &type, &flags, &sid, &len, tmp, sizeof(tmp)))
            return 0;
        if (type == 2) continue;
        if (type == 3) return 0;
        if (type != 0) continue;
        if (sid != c->stream) continue;
        if (len > cap) return 0;
        memcpy(data, tmp, len);
        *outlen = len;
        /* Give the peer credit for the bytes we just consumed. */
        if (len && !ym_write_frame(c, 1, 0, sid, len, NULL)) return 0;
        if ((flags & 4) && !len) return 0;
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* connection / Noise handshake                                        */
/* ------------------------------------------------------------------ */

static int conn_write(struct p2p_conn *c, const void *data, unsigned long len)
{
    if (c->stream) return ym_write(c, (const unsigned char *)data, len);
    if (!c->noise) return p2p_write_raw(c->fd, data, len);
    return noise_write_raw(c->noise, c->fd, (const unsigned char *)data, len);
}

static int conn_read(struct p2p_conn *c, void *data, unsigned long cap,
                     unsigned long *outlen)
{
    static unsigned char pending[P2P_MAX_FRAME + 32];
    unsigned long got, take;

    /* Multistream reads one byte at a time.  Noise transport frames and
       yamux data frames, however, arrive as a whole frame, so keep the
       remainder of the last frame in the connection buffer. */
    if (c->rpos < c->rlen) {
        take = c->rlen - c->rpos;
        if (take > cap) take = cap;
        memcpy(data, pending + c->rpos, take);
        c->rpos += take;
        if (outlen) *outlen = take;
        return 1;
    }

    if (!c->noise && !c->stream) {
        if (!p2p_read_raw(c->fd, data, cap)) return 0;
        if (outlen) *outlen = cap;
        return 1;
    }

    if (c->stream) {
        if (!ym_read(c, pending, sizeof(pending), &got)) return 0;
    } else {
        if (!noise_read_raw(c->noise, c->fd, pending, sizeof(pending), &got))
            return 0;
    }
    c->rpos = 0;
    c->rlen = got;
    take = got > cap ? cap : got;
    memcpy(data, pending, take);
    c->rpos = take;
    if (outlen) *outlen = take;
    return 1;
}

static int conn_write_all(struct p2p_conn *c, const void *data, unsigned long len)
{
    const unsigned char *p = (const unsigned char *)data;
    while (len) {
        unsigned long n = len > 16384UL ? 16384UL : len;
        if (!conn_write(c, p, n)) return 0;
        p += n;
        len -= n;
    }
    return 1;
}

static int hs_write(int fd, struct noise_state *n,
                    const unsigned char *data, unsigned long len)
{
    unsigned char h[2];
    h[0] = (unsigned char)(len >> 8);
    h[1] = (unsigned char)len;
    if (!p2p_write_raw(fd, h, 2)) return 0;
    return p2p_write_raw(fd, data, len);
}

static int hs_read(int fd, struct noise_state *n,
                   unsigned char *data, unsigned long cap, unsigned long *len)
{
    unsigned char h[2];
    if (!p2p_read_raw(fd, h, 2)) return 0;
    *len = ((unsigned long)h[0] << 8) | h[1];
    if (*len > cap) return 0;
    return p2p_read_raw(fd, data, *len);
}

static int noise_begin(struct noise_state *n, int initiator,
                       unsigned char *static_seed)
{
    static const char proto[] = "Noise_XX_25519_ChaChaPoly_SHA256";
    memset(n, 0, sizeof(*n));
    n->initiator = initiator;
    /* Noise InitializeSymmetric: a protocol name no longer than HASHLEN is
       copied into h and zero-padded; only longer names are hashed.  The
       chaining key is that initial h, before the (empty) prologue mix. */
    memset(n->h, 0, 32);
    if (strlen(proto) <= 32)
        memcpy(n->h, proto, strlen(proto));
    else
        qrx_sha256(proto, (unsigned int)strlen(proto), n->h);
    memcpy(n->ck, n->h, 32);
    mix_hash(n, NULL, 0);
    noise_generate_static(n);
    noise_generate_ephemeral(n);
    (void)static_seed;
    return 1;
}

static int noise_initiator_handshake(struct qrx_p2p *p2p, struct noise_state *n,
                                     int fd, const char *expected,
                                     char *remote, int remote_cap)
{
    unsigned char msg[1024], plain[1024], payload[512];
    unsigned long mlen;
    int paylen;

    /* message 1: e */
    if (!hs_write(fd, n, n->e_pub, 32)) return 0;
    mix_hash(n, n->e_pub, 32);
    mix_hash(n, NULL, 0);
    P2P_TRACE("noise: message 1 sent");

    /* message 2: e, ee, s, es */
    if (!hs_read(fd, n, msg, sizeof(msg), &mlen) || mlen < 32) return 0;
    P2P_TRACE("noise: message 2 len=%lu", mlen);
    memcpy(n->re, msg, 32);
    {
        char hx[160];
        hex_encode(n->e_priv, 32, hx, sizeof(hx));
        P2P_TRACE("noise: epriv=%s", hx);
        hex_encode(n->e_pub, 32, hx, sizeof(hx));
        P2P_TRACE("noise: epub=%s", hx);
        hex_encode(n->re, 32, hx, sizeof(hx));
        P2P_TRACE("noise: re=%s", hx);
        if (mlen >= 80) {
            hex_encode(msg + 32, 48, hx, sizeof(hx));
            P2P_TRACE("noise: static_ct=%s", hx);
        }
    }
    mix_hash(n, n->re, 32);
    if (!dh_shared(plain, n->e_priv, n->re)) return 0;
    {
        char hx[160];
        hex_encode(plain, 32, hx, sizeof(hx));
        P2P_TRACE("noise: dh_ee=%s", hx);
    }
    mix_key(n, plain);
    {
        char hx[160];
        hex_encode(n->k, 32, hx, sizeof(hx));
        P2P_TRACE("noise: key_ee=%s", hx);
    }
    {
        char hx[160];
        hex_encode(n->h, 32, hx, sizeof(hx));
        P2P_TRACE("noise: h_static=%s", hx);
    }
    if (!noise_decrypt(n, msg + 32, 48, n->rs)) return 0;
    P2P_TRACE("noise: remote static ok");
    if (!dh_shared(plain, n->e_priv, n->rs)) return 0;
    mix_key(n, plain);
    if (mlen < 80 + 16) return 0;
    if (!noise_decrypt(n, msg + 80, mlen - 80, plain)) return 0;
    P2P_TRACE("noise: payload len=%lu first=%02x", mlen - 96, plain[0]);
    if (!noise_verify_payload(p2p, plain, (int)(mlen - 80 - 16), n->rs,
                              expected, remote, remote_cap)) return 0;
    P2P_TRACE("noise: remote verified %s", remote);

    /* message 3: s, se; the payload has no inner length prefix. */
    if (!noise_encrypt(n, n->s_pub, 32, msg)) return 0;
    if (!dh_shared(plain, n->s_priv, n->re)) return 0;
    mix_key(n, plain);
    if (!noise_payload(p2p, n->s_pub, payload, sizeof(payload), &paylen)) return 0;
    if (!noise_encrypt(n, payload, (unsigned long)paylen, msg + 48)) return 0;
    if (!hs_write(fd, n, msg, 48 + (unsigned long)paylen + 16)) return 0;
    P2P_TRACE("noise: message 3 sent");
    noise_split(n);
    return 1;
}

static int noise_responder_handshake(struct qrx_p2p *p2p, struct noise_state *n,
                                     int fd, const char *expected,
                                     char *remote, int remote_cap)
{
    unsigned char msg[1024], plain[1024], payload[512];
    unsigned long mlen;
    int paylen;

    /* message 1 */
    if (!hs_read(fd, n, msg, sizeof(msg), &mlen) || mlen != 32) return 0;
    memcpy(n->re, msg, 32);
    mix_hash(n, n->re, 32);
    mix_hash(n, NULL, 0);

    /* message 2 */
    if (!noise_generate_ephemeral(n)) return 0;
    memcpy(msg, n->e_pub, 32);
    mix_hash(n, n->e_pub, 32);
    if (!dh_shared(plain, n->e_priv, n->re)) return 0;
    mix_key(n, plain);
    if (!noise_encrypt(n, n->s_pub, 32, msg + 32)) return 0;
    if (!dh_shared(plain, n->s_priv, n->re)) return 0;
    mix_key(n, plain);
    if (!noise_payload(p2p, n->s_pub, payload, sizeof(payload), &paylen)) return 0;
    if (!noise_encrypt(n, payload, (unsigned long)paylen, msg + 80)) return 0;
    if (!hs_write(fd, n, msg, 80 + (unsigned long)paylen + 16)) return 0;

    /* message 3 */
    if (!hs_read(fd, n, msg, sizeof(msg), &mlen) || mlen < 48 + 16) return 0;
    if (!noise_decrypt(n, msg, 48, n->rs)) return 0;
    if (!dh_shared(plain, n->s_priv, n->re)) return 0;
    mix_key(n, plain);
    if (!noise_decrypt(n, msg + 48, mlen - 48, plain)) return 0;
    if (!noise_verify_payload(p2p, plain, (int)(mlen - 48 - 16), n->rs,
                              expected, remote, remote_cap)) return 0;
    noise_split(n);
    return 1;
}

/* ------------------------------------------------------------------ */
/* CID helpers                                                         */
/* ------------------------------------------------------------------ */

static int cid_parse(const char *s, unsigned char *out, int cap)
{
    int n;
    if (!s || !*s) return 0;
    if (s[0] == 'Q' && s[1] == 'm') {
        n = b58_decode(s, out, cap);
        return (n >= 2 && out[0] == 0x12) ? n : 0;
    }
    if (s[0] == 'b') {
        n = b32_decode(s + 1, out, cap);
        return (n >= 4 && out[0] == 0x01) ? n : 0;
    }
    return 0;
}

static int cid_format(const unsigned char *cid, int len, char *out, int cap)
{
    if (len < 2) return 0;
    if (cid[0] == 0x12) return b58_encode(cid, len, out, cap) > 0;
    if (cid[0] == 0x01) {
        if (cap < 3) return 0;
        out[0] = 'b';
        return b32_encode(cid, len, out + 1, cap - 1) > 0;
    }
    return 0;
}

static const unsigned char *cid_multihash(const unsigned char *cid, int len,
                                           int *mhlen)
{
    int off;
    if (len < 2) return NULL;
    off = (cid[0] == 0x01) ? 2 : 0;
    if (off >= len) return NULL;
    *mhlen = len - off;
    return cid + off;
}

static int cid_from_prefix(const unsigned char *prefix, int plen,
                           const unsigned char *data, unsigned long dlen,
                           unsigned char *cid, int cap, int *cidlen)
{
    unsigned char digest[32];
    int n = plen;
    int off = 0;
    if (!prefix || plen < 2 || plen > 64) return 0;
    /* Bitswap 1.1+ prefixes include the CID version and codec.  CIDv0 is
       represented there as 00 70 12 20, but its canonical byte form is just
       12 20 <digest>.  Strip the version/codec for CIDv0. */
    if (plen >= 4 && prefix[0] == 0x00 && prefix[1] == 0x70) off = 2;
    if (plen - off < 2 || cap < plen - off + 32) return 0;
    memcpy(cid, prefix + off, (size_t)(plen - off));
    n = plen - off;
    if (prefix[plen - 2] == 0x12 && prefix[plen - 1] == 0x20) {
        qrx_sha256(data, (unsigned int)dlen, digest);
        memcpy(cid + n, digest, 32);
        n += 32;
    } else {
        return 0;
    }
    *cidlen = n;
    return 1;
}

static int cid_verify(const unsigned char *cid, int cidlen,
                      const unsigned char *prefix, int plen,
                      const unsigned char *data, unsigned long dlen)
{
    unsigned char made[128];
    int made_len;
    if (!cid_from_prefix(prefix, plen, data, dlen, made, sizeof(made), &made_len))
        return 0;
    return made_len == cidlen && memcmp(made, cid, cidlen) == 0;
}

/* ------------------------------------------------------------------ */
/* peerstore                                                           */
/* ------------------------------------------------------------------ */

static struct p2p_peer *peer_find(struct qrx_p2p *p2p, const char *id)
{
    int i;
    for (i = 0; i < p2p->npeers; ++i)
        if (!strcmp(p2p->peers[i].id, id)) return &p2p->peers[i];
    return NULL;
}

static int peer_add(struct qrx_p2p *p2p, const char *id,
                    const char *host, int port)
{
    struct p2p_peer *p;
    if (!id || !*id || port <= 0 || port > 65535) return 0;
    p = peer_find(p2p, id);
    if (!p) {
        if (p2p->npeers >= QRX_P2P_MAX_PEERS) return 0;
        p = &p2p->peers[p2p->npeers++];
        memset(p, 0, sizeof(*p));
        strncpy(p->id, id, sizeof(p->id) - 1);
        p->id_len = b58_decode(id, p->id_bin, sizeof(p->id_bin));
        p->used = 1;
    }
    if (host && *host) {
        strncpy(p->host, host, sizeof(p->host) - 1);
        p->port = port;
        p->tried = 0;
    }
    return 1;
}

static int parse_multiaddr(const char *ma, char *host, int hostcap,
                           int *port, char *peer, int peercap)
{
    char tmp[QRX_P2P_MAX_ADDR];
    char *p, *proto, *value, *next;
    int nhost = 0, nport = 0;
    if (!ma || !*ma || strlen(ma) >= sizeof(tmp)) return 0;
    strcpy(tmp, ma);
    host[0] = 0;
    peer[0] = 0;
    *port = 0;
    p = tmp;
    while (p && *p) {
        while (*p == '/') ++p;
        if (!*p) break;
        proto = p;
        value = strchr(p, '/');
        if (value) *value++ = 0;
        next = value ? strchr(value, '/') : NULL;
        if (next) *next++ = 0;
        if (!strcmp(proto, "ip4") || !strcmp(proto, "dns4") ||
            !strcmp(proto, "dns")) {
            if (!value || !*value) return 0;
            strncpy(host, value, hostcap - 1);
            host[hostcap - 1] = 0;
            nhost = 1;
        } else if (!strcmp(proto, "tcp")) {
            if (!value) return 0;
            *port = atoi(value);
            nport = 1;
        } else if (!strcmp(proto, "p2p") || !strcmp(proto, "ipfs")) {
            if (!value) return 0;
            strncpy(peer, value, peercap - 1);
            peer[peercap - 1] = 0;
        }
        p = next;
    }
    return nhost && nport && peer[0] && *port > 0 && *port < 65536;
}

static int own_multiaddr(struct qrx_p2p *p2p, unsigned char *out, int cap,
                         int *outlen)
{
    struct in_addr a;
    unsigned char *p = out;
    const char *host = p2p->announce_host;
    if (!host || !*host) host = "127.0.0.1";
    a.s_addr = inet_addr((char *)host);
    if (a.s_addr == INADDR_NONE) return 0;
    if (cap < 10) return 0;
    *p++ = 0x04;
    memcpy(p, &a, 4); p += 4;
    *p++ = 0x06;
    *p++ = (unsigned char)(p2p->listen_port >> 8);
    *p++ = (unsigned char)p2p->listen_port;
    *outlen = (int)(p - out);
    return 1;
}

static int decode_multiaddr(const unsigned char *ma, int len,
                            char *host, int hostcap, int *port)
{
    int pos = 0;
    host[0] = 0;
    *port = 0;
    while (pos < len) {
        int code = ma[pos++];
        if (code == 0x04 && pos + 4 <= len) {
            sprintf(host, "%u.%u.%u.%u", ma[pos], ma[pos + 1],
                    ma[pos + 2], ma[pos + 3]);
            pos += 4;
        } else if (code == 0x06 && pos + 2 <= len) {
            *port = (ma[pos] << 8) | ma[pos + 1];
            pos += 2;
        } else {
            return host[0] && *port;
        }
    }
    return host[0] && *port;
}

/* ------------------------------------------------------------------ */
/* TCP                                                                 */
/* ------------------------------------------------------------------ */

static int tcp_connect_host(const char *host, int port)
{
    struct hostent *he;
    struct sockaddr_in sa;
    int fd;
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = inet_addr((char *)host);
    if (sa.sin_addr.s_addr == INADDR_NONE) {
        he = gethostbyname((char *)host);
        if (!he || he->h_length != 4 || !he->h_addr_list[0]) return -1;
        memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        CloseSocket(fd);
        return -1;
    }
    return fd;
}

static int p2p_conn_connect(struct qrx_p2p *p2p, const char *host, int port,
                            const char *expected, struct p2p_conn *c,
                            struct noise_state *ns, char *remote, int rcap)
{
    int fd = tcp_connect_host(host, port);
    if (fd < 0) {
        P2P_TRACE("p2p: tcp connect failed %s:%d", host, port);
        return 0;
    }
    P2P_TRACE("p2p: tcp connected %s:%d fd=%d", host, port, fd);
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->initiator = 1;
    if (!ms_negotiate(c, "/noise", 1)) {
        P2P_TRACE("p2p: noise multistream failed");
        goto fail;
    }
    P2P_TRACE("p2p: noise selected");
    noise_begin(ns, 1, NULL);
    if (!noise_initiator_handshake(p2p, ns, fd, expected, remote, rcap)) {
        P2P_TRACE("p2p: noise handshake failed");
        goto fail;
    }
    P2P_TRACE("p2p: noise ok remote=%s", remote);
    c->noise = ns;
    if (!ms_negotiate(c, "/yamux/1.0.0", 1)) {
        P2P_TRACE("p2p: yamux failed");
        goto fail;
    }
    P2P_TRACE("p2p: yamux ok");
    return 1;
fail:
    CloseSocket(fd);
    return 0;
}

static int p2p_conn_accept(struct qrx_p2p *p2p, int fd, struct p2p_conn *c,
                           struct noise_state *ns, char *remote, int rcap)
{
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->initiator = 0;
    if (!ms_negotiate(c, "/noise", 0)) return 0;
    noise_begin(ns, 0, NULL);
    if (!noise_responder_handshake(p2p, ns, fd, NULL, remote, rcap)) return 0;
    c->noise = ns;
    if (!ms_negotiate(c, "/yamux/1.0.0", 0)) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* application streams                                                 */
/* ------------------------------------------------------------------ */

static int stream_open_out(struct p2p_conn *c, const char *proto)
{
    if (!ym_open(c)) return 0;
    return ms_negotiate(c, proto, 1);
}

static int stream_open_in(struct p2p_conn *c, const char *proto)
{
    if (!ym_accept(c)) return 0;
    return ms_negotiate(c, proto, 0);
}

static int stream_read_exact(struct p2p_conn *c, unsigned char *out,
                             unsigned long len)
{
    unsigned long got, off = 0;
    while (off < len) {
        if (!conn_read(c, out + off, len - off, &got) || got == 0) return 0;
        off += got;
    }
    return 1;
}

static int stream_varint(struct p2p_conn *c, unsigned long *v)
{
    unsigned long x = 0;
    int shift = 0;
    unsigned char b;
    for (;;) {
        if (!stream_read_exact(c, &b, 1)) return 0;
        x |= (unsigned long)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *v = x; return 1; }
        shift += 7;
        if (shift > 28) return 0;
    }
}

static int stream_write_msg(struct p2p_conn *c, const unsigned char *data,
                            unsigned long len)
{
    unsigned char h[10];
    int n = 0;
    unsigned long x = len;
    P2P_TRACE("stream: write len=%lu", len);
    while (x >= 0x80) {
        h[n++] = (unsigned char)((x & 0x7f) | 0x80);
        x >>= 7;
    }
    h[n++] = (unsigned char)x;
    if (!conn_write(c, h, (unsigned long)n)) return 0;
    return conn_write(c, data, len);
}

static int stream_read_msg(struct p2p_conn *c, unsigned char *data,
                           unsigned long cap, unsigned long *len)
{
    if (!stream_varint(c, len) || *len > cap) return 0;
    P2P_TRACE("stream: read len=%lu", *len);
    return stream_read_exact(c, data, *len);
}

/* ------------------------------------------------------------------ */
/* Bitswap 1.2.0                                                       */
/* ------------------------------------------------------------------ */

static int bs_want_message(const unsigned char *cid, int clen,
                           int cancel, unsigned char *out, int cap,
                           unsigned long *outlen)
{
    unsigned char eb[512], wb[512];
    struct p2p_buf e, w, b;
    e.p = eb; e.len = 0; e.cap = sizeof(eb);
    if (!pb_bytes(&e, 1, cid, clen)) return 0;
    if (!pb_tag(&e, 2, 0) || !pb_varint(&e, 1)) return 0;
    if (cancel) {
        if (!pb_tag(&e, 3, 0) || !pb_varint(&e, 1)) return 0;
    }
    if (!pb_tag(&e, 4, 0) || !pb_varint(&e, 0)) return 0;
    if (!pb_tag(&e, 5, 0) || !pb_varint(&e, 1)) return 0;

    w.p = wb; w.len = 0; w.cap = sizeof(wb);
    if (!pb_bytes(&w, 1, eb, e.len)) return 0;
    if (!pb_tag(&w, 2, 0) || !pb_varint(&w, 1)) return 0;

    b.p = out; b.len = 0; b.cap = cap;
    if (!pb_bytes(&b, 1, wb, w.len)) return 0;
    *outlen = b.len;
    return 1;
}

static int bs_send_want(struct p2p_conn *c, const unsigned char *cid, int clen,
                        int cancel)
{
    unsigned char msg[1024];
    unsigned long len;
    if (!bs_want_message(cid, clen, cancel, msg, sizeof(msg), &len)) return 0;
    return stream_write_msg(c, msg, len);
}

static int bs_parse_payload(struct qrx_p2p *p2p, const unsigned char *b,
                            unsigned long len, const unsigned char *cid,
                            int clen, unsigned char **data,
                            unsigned long *dlen)
{
    struct p2p_reader r;
    unsigned long f, w, vl;
    const unsigned char *v, *prefix = NULL, *block = NULL;
    unsigned long plen = 0, blen = 0;
    unsigned char made[128];
    int mlen;
    r.p = b; r.len = len; r.pos = 0;
    while (r.pos < r.len) {
        if (!pr_field(&r, &f, &w, &vl, &v)) return 0;
        if (f == 1 && w == 2) { prefix = v; plen = vl; }
        else if (f == 2 && w == 2) { block = v; blen = vl; }
    }
    if (!prefix || !block) return 0;
    if (!cid_from_prefix(prefix, (int)plen, block, blen, made, sizeof(made), &mlen))
        return 0;
    if (mlen != clen || memcmp(made, cid, clen)) return 0;
    if (blen > QRX_P2P_MAX_MESSAGE) return 0;
    *data = AllocVec((ULONG)blen, MEMF_ANY);
    if (!*data) return 0;
    memcpy(*data, block, blen);
    *dlen = blen;
    return 1;
}

/* go-bitswap peers may open a fresh stream to return the block instead of
   writing on the stream that carried the want.  Accept that stream and
   complete its multistream negotiation before reading the response. */
static int bs_accept_response_stream(struct p2p_conn *c)
{
    unsigned char type, tmp[64];
    unsigned short flags;
    unsigned long sid, len;
    for (;;) {
        if (!ym_read_frame(c, &type, &flags, &sid, &len, tmp, sizeof(tmp)))
            return 0;
        if (type == 2) continue;
        if (type == 1 && (flags & 1) && sid != c->stream) {
            if (!ym_write_frame(c, 1, 2, sid, YM_WINDOW, NULL)) return 0;
            c->stream = sid;
            P2P_TRACE("bitswap: accepted response stream %lu", sid);
            return 1;
        }
        if (type == 3) return 0;
    }
}

static int bs_read_response(struct qrx_p2p *p2p, struct p2p_conn *c,
                            const unsigned char *cid, int clen,
                            unsigned char **data, unsigned long *dlen,
                            int *have)
{
    unsigned char *msg = AllocVec(QRX_P2P_MAX_MESSAGE, MEMF_ANY);
    unsigned long mlen;
    struct p2p_reader r;
    unsigned long f, w, vl;
    const unsigned char *v;
    int i;
    *have = 0;
    if (!msg) return 0;
    for (i = 0; i < 4; ++i) {
        if (!stream_read_msg(c, msg, QRX_P2P_MAX_MESSAGE, &mlen)) break;
        r.p = msg; r.len = mlen; r.pos = 0;
        while (r.pos < r.len) {
            if (!pr_field(&r, &f, &w, &vl, &v)) break;
            if (f == 2 && w == 2 && vl <= QRX_P2P_MAX_MESSAGE) {
                *data = AllocVec((ULONG)vl, MEMF_ANY);
                if (*data) {
                    memcpy(*data, v, vl);
                    *dlen = vl;
                    *have = 1;
                }
                FreeVec(msg);
                return *have;
            }
            if (f == 3 && w == 2) {
                if (bs_parse_payload(p2p, v, vl, cid, clen, data, dlen)) {
                    *have = 1;
                    FreeVec(msg);
                    return 1;
                }
            }
        }
    }
    FreeVec(msg);
    return 0;
}

static int bs_fetch_peer(struct qrx_p2p *p2p, struct p2p_peer *peer,
                         const unsigned char *cid, int clen,
                         unsigned char **data, unsigned long *dlen)
{
    struct p2p_conn c;
    struct noise_state ns;
    char remote[128];
    int have = 0;
    int ok = 0;
    if (!peer || !peer->host[0] || peer->port <= 0) return 0;
    P2P_TRACE("bitswap: connect %s:%d", peer->host, peer->port);
    if (!p2p_conn_connect(p2p, peer->host, peer->port, peer->id, &c, &ns,
                          remote, sizeof(remote))) {
        P2P_TRACE("bitswap: connect failed");
        return 0;
    }
    P2P_TRACE("bitswap: connected");
    if (!stream_open_out(&c, "/ipfs/bitswap/1.2.0")) {
        P2P_TRACE("bitswap: protocol open failed");
        goto done;
    }
    P2P_TRACE("bitswap: protocol open");
    if (!bs_send_want(&c, cid, clen, 0)) {
        P2P_TRACE("bitswap: want send failed");
        goto done;
    }
    if (!bs_accept_response_stream(&c)) {
        P2P_TRACE("bitswap: response stream not accepted");
        goto done;
    }
    {
        char proto[256];
        if (!ms_recv(&c, proto, sizeof(proto)) ||
            !ms_send(&c, proto) ||
            !ms_recv(&c, proto, sizeof(proto)) ||
            !ms_send(&c, proto)) {
            P2P_TRACE("bitswap: response stream negotiation failed");
            goto done;
        }
        P2P_TRACE("bitswap: response stream negotiated");
    }
    ok = bs_read_response(p2p, &c, cid, clen, data, dlen, &have);
    P2P_TRACE("bitswap: response ok=%d have=%d", ok, have);
    (void)bs_send_want(&c, cid, clen, 1);
    if (ok && have) {
        char cs[QRX_P2P_MAX_CID];
        if (cid_format(cid, clen, cs, sizeof(cs)))
            p2p->be.block_put(cs, *data, *dlen);
    }
 done:
    CloseSocket(c.fd);
    return ok && have;
}

static int bs_serve(struct qrx_p2p *p2p, struct p2p_conn *c)
{
    unsigned char *msg = AllocVec(QRX_P2P_MAX_MESSAGE, MEMF_ANY);
    unsigned char *pmsg = NULL, *top = NULL;
    unsigned long mlen, i;
    struct p2p_reader r;
    unsigned long f, w, vl;
    const unsigned char *v;
    int handled = 0;
    if (!msg) return 0;
    if (!stream_read_msg(c, msg, QRX_P2P_MAX_MESSAGE, &mlen)) {
        FreeVec(msg);
        return 0;
    }
    r.p = msg; r.len = mlen; r.pos = 0;
    while (r.pos < r.len) {
        if (!pr_field(&r, &f, &w, &vl, &v)) break;
        if (f != 1 || w != 2) continue;
        {
            struct p2p_reader wr;
            unsigned long wf, ww, wvl;
            const unsigned char *wv;
            wr.p = v; wr.len = vl; wr.pos = 0;
            while (wr.pos < wr.len) {
                if (!pr_field(&wr, &wf, &ww, &wvl, &wv)) break;
                if (wf == 1 && ww == 2) {
                    struct p2p_reader er;
                    unsigned long ef, ew, evl;
                    const unsigned char *ev;
                    const unsigned char *block = NULL;
                    unsigned long blen = 0;
                    int want_type = 0, dont_have = 0;
                    unsigned char cid[128], prefix[8];
                    char cs[QRX_P2P_MAX_CID];
                    int clen = -1;
                    struct p2p_buf pb;
                    er.p = wv; er.len = wvl; er.pos = 0;
                    while (er.pos < er.len) {
                        if (!pr_field(&er, &ef, &ew, &evl, &ev)) break;
                        if (ef == 1 && ew == 2) {
                            block = ev;
                            blen = evl;
                        } else if (ef == 4 && ew == 0) {
                            want_type = (int)evl;
                        } else if (ef == 5 && ew == 0) {
                            dont_have = (int)evl;
                        }
                    }
                    if (!block || blen > sizeof(cid) || want_type != 0) continue;
                    memcpy(cid, block, blen);
                    clen = (int)blen;
                    if (!cid_format(cid, clen, cs, sizeof(cs))) continue;
                    if (p2p->be.block_has(cs)) {
                        unsigned char *data = NULL;
                        unsigned long dlen = 0;
                        if (p2p->be.block_get(cs, &data, &dlen) && data) {
                            int plen = (clen >= 4) ? 4 : 2;
                            if (clen >= 4) {
                                prefix[0] = cid[0];
                                prefix[1] = cid[1];
                                prefix[2] = cid[2];
                                prefix[3] = cid[3];
                            } else {
                                prefix[0] = 0x12;
                                prefix[1] = 0x20;
                            }
                            if (!pmsg) pmsg = AllocVec(QRX_P2P_MAX_MESSAGE, MEMF_ANY);
                            if (!top) top = AllocVec(QRX_P2P_MAX_MESSAGE, MEMF_ANY);
                            if (pmsg && top) {
                                pb.p = pmsg; pb.len = 0; pb.cap = QRX_P2P_MAX_MESSAGE;
                                if (pb_bytes(&pb, 1, prefix, (unsigned long)plen) &&
                                    pb_bytes(&pb, 2, data, dlen)) {
                                    struct p2p_buf tb;
                                    tb.p = top; tb.len = 0; tb.cap = QRX_P2P_MAX_MESSAGE;
                                    if (pb_bytes(&tb, 3, pmsg, pb.len))
                                        handled = stream_write_msg(c, top, tb.len) || handled;
                                }
                            }
                            }
                            FreeVec(data);
                        }
                    }
                }
            }
        i = 0;
    }
    FreeVec(pmsg);
    FreeVec(top);
    FreeVec(msg);
    return handled;
}

/* ------------------------------------------------------------------ */
/* Kademlia DHT                                                        */
/* ------------------------------------------------------------------ */

static int dht_add_provider(struct qrx_p2p *p2p, const char *key,
                            const char *peer, const char *host, int port)
{
    int i;
    for (i = 0; i < p2p->nproviders; ++i) {
        if (!strcmp(p2p->providers[i].key, key) &&
            !strcmp(p2p->providers[i].peer_id, peer)) {
            strncpy(p2p->providers[i].host, host, QRX_P2P_MAX_HOST - 1);
            p2p->providers[i].port = port;
            return 1;
        }
    }
    if (p2p->nproviders >= QRX_P2P_MAX_PROVIDERS) return 0;
    i = p2p->nproviders++;
    strncpy(p2p->providers[i].key, key, sizeof(p2p->providers[i].key) - 1);
    strncpy(p2p->providers[i].peer_id, peer, sizeof(p2p->providers[i].peer_id) - 1);
    strncpy(p2p->providers[i].host, host, sizeof(p2p->providers[i].host) - 1);
    p2p->providers[i].port = port;
    return 1;
}

static int dht_build_peer(struct qrx_p2p *p2p, struct p2p_buf *b,
                          int field, const char *id, const char *host, int port)
{
    unsigned char idbin[64], addr[64], peer[512];
    int idlen, alen;
    struct p2p_buf pb;
    idlen = b58_decode(id, idbin, sizeof(idbin));
    if (idlen <= 0) return 0;
    {
        struct in_addr a;
        a.s_addr = inet_addr((char *)host);
        if (a.s_addr == INADDR_NONE) return 0;
        addr[0] = 0x04; memcpy(addr + 1, &a, 4);
        addr[5] = 0x06; addr[6] = (unsigned char)(port >> 8);
        addr[7] = (unsigned char)port; alen = 8;
    }
    pb.p = peer; pb.len = 0; pb.cap = sizeof(peer);
    if (!pb_bytes(&pb, 1, idbin, idlen)) return 0;
    if (!pb_bytes(&pb, 2, addr, alen)) return 0;
    if (!pb_bytes(&pb, 3, "\x01", 1)) return 0;
    return pb_bytes(b, (unsigned)field, peer, pb.len);
}

static int dht_parse_peer(struct qrx_p2p *p2p, const unsigned char *b,
                          unsigned long len, const char *key, int provider)
{
    struct p2p_reader r;
    unsigned long f, w, vl;
    const unsigned char *v;
    unsigned char id[64];
    char peer[128], host[QRX_P2P_MAX_HOST];
    int idlen = 0, port = 0;
    r.p = b; r.len = len; r.pos = 0;
    peer[0] = host[0] = 0;
    while (r.pos < r.len) {
        if (!pr_field(&r, &f, &w, &vl, &v)) return 0;
        if (f == 1 && w == 2 && vl <= sizeof(id)) {
            memcpy(id, v, vl); idlen = (int)vl;
            if (!b58_encode(id, idlen, peer, sizeof(peer))) return 0;
        } else if (f == 2 && w == 2 && vl <= 256) {
            if (!host[0] && !decode_multiaddr(v, (int)vl, host, sizeof(host), &port))
                continue;
        }
    }
    if (!peer[0] || !host[0] || port <= 0) return 0;
    peer_add(p2p, peer, host, port);
    if (provider && key) dht_add_provider(p2p, key, peer, host, port);
    return 1;
}

static int dht_parse_peers(struct qrx_p2p *p2p, const unsigned char *b,
                           unsigned long len, const char *key, int provider)
{
    struct p2p_reader r;
    unsigned long f, w, vl;
    const unsigned char *v;
    int n = 0;
    r.p = b; r.len = len; r.pos = 0;
    while (r.pos < r.len) {
        if (!pr_field(&r, &f, &w, &vl, &v)) break;
        if ((f == 8 || f == 9) && w == 2)
            n += dht_parse_peer(p2p, v, vl, key, provider);
    }
    return n;
}

static int dht_request(struct qrx_p2p *p2p, struct p2p_peer *peer, int type,
                       const unsigned char *key, int keylen,
                       unsigned char *resp, unsigned long cap,
                       unsigned long *rlen)
{
    struct p2p_conn c;
    struct noise_state ns;
    char remote[128];
    static unsigned char msg[1024], top[4096];
    unsigned long len;
    struct p2p_buf b;
    if (!p2p_conn_connect(p2p, peer->host, peer->port, peer->id, &c, &ns,
                          remote, sizeof(remote))) return 0;
    if (!stream_open_out(&c, "/ipfs/kad/1.0.0")) { CloseSocket(c.fd); return 0; }
    b.p = msg; b.len = 0; b.cap = sizeof(msg);
    if (!pb_tag(&b, 1, 0) || !pb_varint(&b, (unsigned long)type) ||
        !pb_bytes(&b, 2, key, (unsigned long)keylen)) {
        CloseSocket(c.fd); return 0;
    }
    if (!stream_write_msg(&c, msg, b.len) ||
        !stream_read_msg(&c, top, sizeof(top), &len)) {
        CloseSocket(c.fd); return 0;
    }
    if (len > cap) len = cap;
    memcpy(resp, top, len);
    *rlen = len;
    CloseSocket(c.fd);
    return 1;
}

static int dht_announce_peer(struct qrx_p2p *p2p, struct p2p_peer *peer,
                             const char *keyhex, const unsigned char *key,
                             int keylen)
{
    struct p2p_conn c;
    struct noise_state ns;
    char remote[128];
    static unsigned char msg[2048], resp[512];
    unsigned long len;
    struct p2p_buf b;
    if (!p2p_conn_connect(p2p, peer->host, peer->port, peer->id, &c, &ns,
                          remote, sizeof(remote))) return 0;
    if (!stream_open_out(&c, "/ipfs/kad/1.0.0")) { CloseSocket(c.fd); return 0; }
    b.p = msg; b.len = 0; b.cap = sizeof(msg);
    if (!pb_tag(&b, 1, 0) || !pb_varint(&b, 2) ||
        !pb_bytes(&b, 2, key, (unsigned long)keylen) ||
        !dht_build_peer(p2p, &b, 9, p2p->peer_id, p2p->announce_host,
                        p2p->listen_port)) {
        CloseSocket(c.fd); return 0;
    }
    if (!stream_write_msg(&c, msg, b.len) ||
        !stream_read_msg(&c, resp, sizeof(resp), &len)) {
        CloseSocket(c.fd); return 0;
    }
    dht_add_provider(p2p, keyhex, p2p->peer_id, p2p->announce_host,
                     p2p->listen_port);
    CloseSocket(c.fd);
    return 1;
}

static int dht_get_providers(struct qrx_p2p *p2p, struct p2p_peer *peer,
                             const char *keyhex, const unsigned char *key,
                             int keylen)
{
    unsigned char resp[8192];
    unsigned long len;
    if (!dht_request(p2p, peer, 3, key, keylen, resp, sizeof(resp), &len)) {
        P2P_TRACE("dht: GET_PROVIDERS request failed");
        return 0;
    }
    P2P_TRACE("dht: GET_PROVIDERS response len=%lu", len);
    return dht_parse_peers(p2p, resp, len, keyhex, 1);
}

static int dht_serve(struct qrx_p2p *p2p, struct p2p_conn *c)
{
    static unsigned char msg[8192], resp[8192];
    unsigned long len, rlen = 0;
    struct p2p_reader r;
    unsigned long f, w, vl;
    const unsigned char *v;
    int type = -1;
    const unsigned char *key = NULL;
    unsigned long keylen = 0;
    unsigned char keyhex[160];
    struct p2p_buf b;
    int i;
    if (!stream_read_msg(c, msg, sizeof(msg), &len)) return 0;
    r.p = msg; r.len = len; r.pos = 0;
    while (r.pos < r.len) {
        if (!pr_field(&r, &f, &w, &vl, &v)) return 0;
        if (f == 1 && w == 0) type = (int)vl;
        else if (f == 2 && w == 2) { key = v; keylen = vl; }
        else if ((f == 9 || f == 8) && w == 2 && key) {
            char tmp[160];
            hex_encode(key, (int)keylen, tmp, sizeof(tmp));
            dht_parse_peer(p2p, v, vl, tmp, f == 9);
        }
    }
    if (!key) return 0;
    hex_encode(key, (int)keylen, keyhex, sizeof(keyhex));
    b.p = resp; b.len = 0; b.cap = sizeof(resp);
    if (!pb_tag(&b, 1, 0) || !pb_varint(&b, (unsigned long)(type < 0 ? 0 : type)) ||
        !pb_bytes(&b, 2, key, keylen)) return 0;
    if (type == 3) {
        for (i = 0; i < p2p->nproviders; ++i) {
            if (!strcmp(p2p->providers[i].key, keyhex))
                dht_build_peer(p2p, &b, 9, p2p->providers[i].peer_id,
                               p2p->providers[i].host, p2p->providers[i].port);
        }
    } else if (type == 4) {
        for (i = 0; i < p2p->npeers; ++i) {
            if (p2p->peers[i].host[0])
                dht_build_peer(p2p, &b, 8, p2p->peers[i].id,
                               p2p->peers[i].host, p2p->peers[i].port);
        }
    }
    rlen = b.len;
    return stream_write_msg(c, resp, rlen);
}

/* ------------------------------------------------------------------ */
/* bootstrap, status, public API                                      */
/* ------------------------------------------------------------------ */

static int p2p_fetch_any(struct qrx_p2p *p2p, const char *cidstr,
                         unsigned char **data, unsigned long *dlen)
{
    unsigned char cid[128];
    int clen = cid_parse(cidstr, cid, sizeof(cid));
    int i;
    char cs[QRX_P2P_MAX_CID];
    P2P_TRACE("fetch: parse %s clen=%d first=%02x", cidstr, clen,
              clen > 0 ? cid[0] : 0);
    if (clen <= 0) return 0;
    if (!cid_format(cid, clen, cs, sizeof(cs))) {
        P2P_TRACE("fetch: cid format failed");
        return 0;
    }
    if (p2p->be.block_get(cs, data, dlen)) return 1;
    P2P_TRACE("fetch: %s peers=%d", cidstr, p2p->npeers);
    for (i = 0; i < p2p->npeers; ++i) {
        P2P_TRACE("fetch: direct peer %d=%s", i, p2p->peers[i].id);
        if (bs_fetch_peer(p2p, &p2p->peers[i], cid, clen, data, dlen))
            return 1;
    }
    return 0;
}

struct qrx_p2p *qrx_p2p_new(const char *dir, int listen_port,
                            const struct qrx_p2p_backend *backend,
                            char *err, unsigned long errcap)
{
    struct qrx_p2p *p2p;
    char path[320];
    unsigned char seed[32];
    int n;
    if (err && errcap) err[0] = 0;
    if (!backend || !backend->block_get || !backend->block_put) {
        if (err) strncpy(err, "incomplete blockstore backend", errcap - 1);
        return NULL;
    }
    p2p = (struct qrx_p2p *)AllocVec(sizeof(*p2p), MEMF_ANY | MEMF_CLEAR);
    if (!p2p) {
        if (err) strncpy(err, "out of memory", errcap - 1);
        return NULL;
    }
    strncpy(p2p->dir, dir ? dir : "IPFS:.qrx-node", sizeof(p2p->dir) - 1);
    p2p_mkdir(p2p->dir);
    p2p->listen_port = listen_port > 0 ? listen_port : QRX_P2P_DEFAULT_PORT;
    p2p->listen_fd = -1;
    p2p->be = *backend;
    sprintf(path, "%s/peer.seed", p2p->dir);
    n = p2p_read_file(path, seed, sizeof(seed));
    if (n != (int)sizeof(seed)) {
        qrx_random_bytes(seed, sizeof(seed));
        if (!p2p_write_file(path, seed, sizeof(seed))) {
            if (err) strncpy(err, "cannot write peer.seed", errcap - 1);
            FreeVec(p2p);
            return NULL;
        }
    }
    memcpy(p2p->ed_seed, seed, 32);
    crypto_ed25519_key_pair(p2p->ed_priv, p2p->ed_pub, p2p->ed_seed);
    if (!derive_identity(p2p)) {
        if (err) strncpy(err, "cannot derive PeerID", errcap - 1);
        FreeVec(p2p);
        return NULL;
    }
    strncpy(p2p->announce_host,
            getenv("QRX_IPFS_ANNOUNCE") ? getenv("QRX_IPFS_ANNOUNCE") : "127.0.0.1",
            sizeof(p2p->announce_host) - 1);
    sprintf(path, "%s/peer-id", p2p->dir);
    p2p_write_file(path, p2p->peer_id, (int)strlen(p2p->peer_id));
    return p2p;
}

void qrx_p2p_free(struct qrx_p2p *p2p)
{
    if (!p2p) return;
    if (p2p->listen_fd >= 0) CloseSocket(p2p->listen_fd);
    FreeVec(p2p);
}

const char *qrx_p2p_peer_id(struct qrx_p2p *p2p)
{
    return p2p ? p2p->peer_id : "";
}

int qrx_p2p_listen_port(struct qrx_p2p *p2p)
{
    return p2p ? p2p->listen_port : 0;
}

int qrx_p2p_listen_fd(struct qrx_p2p *p2p)
{
    return p2p ? p2p->listen_fd : -1;
}

int qrx_p2p_listen(struct qrx_p2p *p2p)
{
    struct sockaddr_in sa;
    int one = 1;
    if (!p2p) return 0;
    if (p2p->listen_fd >= 0) return 1;
    p2p->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (p2p->listen_fd < 0) return 0;
    setsockopt(p2p->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)p2p->listen_port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(p2p->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(p2p->listen_fd, 8) < 0) {
        CloseSocket(p2p->listen_fd);
        p2p->listen_fd = -1;
        return 0;
    }
    return 1;
}

int qrx_p2p_add_peer(struct qrx_p2p *p2p, const char *ma)
{
    char host[QRX_P2P_MAX_HOST], peer[128];
    int port;
    if (!p2p || !parse_multiaddr(ma, host, sizeof(host), &port, peer, sizeof(peer)))
        return 0;
    return peer_add(p2p, peer, host, port);
}

int qrx_p2p_load_bootstrap(struct qrx_p2p *p2p, const char *path)
{
    char buf[4096], *line, *next;
    int n, count = 0;
    const char *env;
    if (!p2p) return 0;
    n = p2p_read_file(path ? path : "IPFS:.qrx-node/bootstrap", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        line = buf;
        while (line && *line) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (*line && *line != '#') count += qrx_p2p_add_peer(p2p, line);
            line = next;
        }
    }
    env = getenv("QRX_IPFS_BOOTSTRAP");
    if (env && *env) {
        char tmp[2048];
        strncpy(tmp, env, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;
        line = tmp;
        while (line && *line) {
            next = strchr(line, ',');
            if (!next) next = strchr(line, ';');
            if (next) *next++ = 0;
            if (*line) count += qrx_p2p_add_peer(p2p, line);
            line = next;
        }
    }
    return count;
}

int qrx_p2p_peer_count(struct qrx_p2p *p2p)
{
    int i, n = 0;
    if (!p2p) return 0;
    for (i = 0; i < p2p->npeers; ++i)
        if (p2p->peers[i].host[0]) ++n;
    return n;
}

int qrx_p2p_fetch(struct qrx_p2p *p2p, const char *cid,
                  unsigned char **data, unsigned long *len)
{
    unsigned char cidb[128], key[160];
    int clen, mhlen, i;
    char keyhex[160];
    if (!p2p || !data || !len) return 0;
    if (p2p_fetch_any(p2p, cid, data, len)) return 1;
    P2P_TRACE("fetch: direct failed, trying DHT");
    clen = cid_parse(cid, cidb, sizeof(cidb));
    if (clen <= 0) return 0;
    {
        const unsigned char *mh = cid_multihash(cidb, clen, &mhlen);
        if (!mh) return 0;
        memcpy(key, mh, (unsigned long)mhlen > sizeof(key) ? sizeof(key) : (unsigned long)mhlen);
        hex_encode(mh, mhlen, keyhex, sizeof(keyhex));
    }
    for (i = 0; i < p2p->npeers; ++i) {
        if (p2p->peers[i].host[0])
            dht_get_providers(p2p, &p2p->peers[i], keyhex, key, mhlen);
    }
    P2P_TRACE("fetch: providers after DHT=%d", p2p->nproviders);
    for (i = 0; i < p2p->nproviders; ++i) {
        char ma[QRX_P2P_MAX_ADDR];
        sprintf(ma, "/ip4/%s/tcp/%d/p2p/%s", p2p->providers[i].host,
                p2p->providers[i].port, p2p->providers[i].peer_id);
        qrx_p2p_add_peer(p2p, ma);
    }
    for (i = 0; i < p2p->npeers; ++i) {
        if (bs_fetch_peer(p2p, &p2p->peers[i], cidb, clen, data, len))
            return 1;
    }
    return 0;
}

int qrx_p2p_provide(struct qrx_p2p *p2p, const char *cid)
{
    unsigned char cidb[128], key[160];
    int clen, mhlen, i, n = 0;
    const unsigned char *mh;
    char keyhex[160];
    if (!p2p) return 0;
    clen = cid_parse(cid, cidb, sizeof(cidb));
    if (clen <= 0) return 0;
    mh = cid_multihash(cidb, clen, &mhlen);
    if (!mh) return 0;
    memcpy(key, mh, (unsigned long)mhlen > sizeof(key) ? sizeof(key) : (unsigned long)mhlen);
    hex_encode(mh, mhlen, keyhex, sizeof(keyhex));
    dht_add_provider(p2p, keyhex, p2p->peer_id, p2p->announce_host,
                     p2p->listen_port);
    for (i = 0; i < p2p->npeers; ++i) {
        if (p2p->peers[i].host[0])
            n += dht_announce_peer(p2p, &p2p->peers[i], keyhex, key, mhlen);
    }
    return n;
}

int qrx_p2p_tick(struct qrx_p2p *p2p)
{
    char pins[8 * QRX_P2P_MAX_CID];
    int n, i;
    if (!p2p || !p2p->be.pin_list) return 0;
    n = p2p->be.pin_list(pins, 4);
    for (i = 0; i < n; ++i)
        qrx_p2p_provide(p2p, pins + (i * QRX_P2P_MAX_CID));
    return n;
}

static int ms_accept_any(struct p2p_conn *c, char *proto, int cap)
{
    char buf[256];
    if (!ms_send(c, "/multistream/1.0.0") || !ms_recv(c, buf, sizeof(buf)))
        return 0;
    if (strcmp(buf, "/multistream/1.0.0")) return 0;
    if (!ms_recv(c, proto, cap)) return 0;
    if (!strcmp(proto, "/ipfs/bitswap/1.2.0") ||
        !strcmp(proto, "/ipfs/kad/1.0.0") ||
        !strcmp(proto, "/ipfs/id/1.0.0")) return ms_send(c, proto);
    return ms_send(c, "na") && 0;
}

static int identify_serve(struct qrx_p2p *p2p, struct p2p_conn *c)
{
    unsigned char pb[64], addr[64], msg[512];
    int pblen, alen;
    struct p2p_buf b;
    static const char *protos[] = {
        "/ipfs/bitswap/1.2.0", "/ipfs/kad/1.0.0"
    };
    int i;
    if (!identity_public_pb(p2p->ed_pub, pb, &pblen)) return 0;
    if (!own_multiaddr(p2p, addr, sizeof(addr), &alen)) return 0;
    b.p = msg; b.len = 0; b.cap = sizeof(msg);
    if (!pb_bytes(&b, 1, pb, pblen) || !pb_bytes(&b, 2, addr, alen)) return 0;
    for (i = 0; i < 2; ++i)
        if (!pb_bytes(&b, 3, protos[i], (unsigned long)strlen(protos[i]))) return 0;
    return stream_write_msg(c, msg, b.len);
}

int qrx_p2p_serve_fd(struct qrx_p2p *p2p, int fd)
{
    struct p2p_conn c;
    struct noise_state ns;
    char remote[128], proto[256];
    int i, ok = 0;
    if (!p2p) return 0;
    if (!p2p_conn_accept(p2p, fd, &c, &ns, remote, sizeof(remote))) {
        CloseSocket(fd);
        return 0;
    }
    peer_add(p2p, remote, NULL, 0);
    for (i = 0; i < 4; ++i) {
        c.stream = 0;
        if (!ym_accept(&c)) break;
        if (!ms_accept_any(&c, proto, sizeof(proto))) break;
        if (!strcmp(proto, "/ipfs/bitswap/1.2.0")) {
            bs_serve(p2p, &c);
            ok = 1;
            break;
        }
        if (!strcmp(proto, "/ipfs/kad/1.0.0")) {
            dht_serve(p2p, &c);
            ok = 1;
        } else if (!strcmp(proto, "/ipfs/id/1.0.0")) {
            identify_serve(p2p, &c);
            ok = 1;
        }
    }
    CloseSocket(fd);
    return ok;
}

int qrx_p2p_status_json(struct qrx_p2p *p2p, char *out, unsigned long cap)
{
    if (!p2p || !out) return 0;
    sprintf(out, "{\"PeerID\":\"%s\",\"ListenPort\":%d,\"Peers\":%d,\"Providers\":%d,\"Version\":\"%s\"}",
            p2p->peer_id, p2p->listen_port, qrx_p2p_peer_count(p2p),
            p2p->nproviders, QRX_P2P_VERSION);
    (void)cap;
    return 1;
}

/* End of qrx_p2p.c. */
