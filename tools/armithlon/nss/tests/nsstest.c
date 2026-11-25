/*
 * NSS on QRX OS One 64 (static: softoken and freebl linked in). Crypto through PKCS#11 like Gecko:
 * SHA-256, HMAC-SHA256, AES-128-GCM, ECDSA P-256 key generation, signature and verification,
 * random bytes; then optionally a TLS handshake to host:port given as arguments.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nspr.h"
#include "nss.h"
#include "pk11pub.h"
#include "secoid.h"
#include "keyhi.h"
#include "cryptohi.h"
#include "ssl.h"
#include "sslproto.h"
#include "cert.h"
#include "certdb.h"
#include "base64.h"
#include "prerror.h"

static int failures;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); if (!ok_) failures++; } while (0)

static void hex(const unsigned char *p, unsigned n, char *out) { for (unsigned i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]); }

static SECStatus accept_and_show(void *arg, PRFileDesc *fd, PRBool checksig, PRBool isServer)
{
    (void)arg; (void)checksig; (void)isServer;
    CERTCertificate *c = SSL_PeerCertificate(fd);
    if (c) {
        printf("server certificate: %s\n  issued by: %s\n", c->subjectName, c->issuerName);
        CERT_DestroyCertificate(c);
    }
    return SECSuccess;      /* no root store linked in yet: the chain is shown, not verified */
}

/* a PEM certificate file as a trusted root (in memory: NSS runs without a database here) */
static int trust_root(const char *path)
{
    PRFileDesc *f = PR_Open(path, PR_RDONLY, 0);
    if (!f) return 0;
    char pem[8192]; PRInt32 n = PR_Read(f, pem, sizeof pem - 1); PR_Close(f);
    if (n <= 0) return 0;
    pem[n] = 0;
    char *b = strstr(pem, "-----BEGIN CERTIFICATE-----"), *e = b ? strstr(b, "-----END CERTIFICATE-----") : NULL;
    if (!b || !e) return 0;
    b += strlen("-----BEGIN CERTIFICATE-----"); *e = 0;
    SECItem der = { siBuffer, NULL, 0 };
    if (ATOB_ConvertAsciiToItem(&der, b) != SECSuccess) return 0;
    CERTCertificate *c = CERT_NewTempCertificate(CERT_GetDefaultCertDB(), &der, NULL, PR_FALSE, PR_TRUE);
    SECITEM_FreeItem(&der, PR_FALSE);
    if (!c) return 0;
    CERTCertTrust t;
    CERT_DecodeTrustString(&t, "C,,");
    int ok = CERT_ChangeCertTrust(CERT_GetDefaultCertDB(), c, &t) == SECSuccess;
    printf("trusted root: %s\n", c->subjectName);
    return ok;
}

static int tls_test(const char *host, int port, int verify)
{
    PRNetAddr addr;
    PRAddrInfo *ai = PR_GetAddrInfoByName(host, PR_AF_UNSPEC, PR_AI_ADDRCONFIG);
    if (!ai || !PR_EnumerateAddrInfo(NULL, ai, (PRUint16)port, &addr)) { printf("cannot resolve %s\n", host); return 0; }
    PR_FreeAddrInfo(ai);
    char as[64]; PR_NetAddrToString(&addr, as, sizeof as);
    printf("connecting to %s (%s) port %d\n", host, as, port);
    PRFileDesc *tcp = PR_OpenTCPSocket(addr.raw.family);
    if (!tcp || PR_Connect(tcp, &addr, PR_SecondsToInterval(30)) != PR_SUCCESS) { printf("connect failed: %d\n", PR_GetError()); return 0; }
    PRFileDesc *s = SSL_ImportFD(NULL, tcp);
    if (!s) { printf("SSL_ImportFD: error %d\n", PR_GetError()); return 0; }
    SSLVersionRange vr = { SSL_LIBRARY_VERSION_TLS_1_2, SSL_LIBRARY_VERSION_TLS_1_3 };
    SSL_VersionRangeSet(s, &vr);
    SSL_OptionSet(s, SSL_HANDSHAKE_AS_CLIENT, PR_TRUE);
    /* every test connection a full handshake: a resumed session skips the certificate check */
    SSL_OptionSet(s, SSL_NO_CACHE, PR_TRUE);
    SSL_SetURL(s, host);
    if (!verify) SSL_AuthCertificateHook(s, accept_and_show, NULL);   /* else NSS's own check: chain and name */
    SSL_ResetHandshake(s, PR_FALSE);   /* the TCP connection is already up: start as client */
    if (SSL_ForceHandshake(s) != SECSuccess) { printf("handshake failed: %d (%s)\n", PR_GetError(), PR_ErrorToName(PR_GetError())); PR_Close(s); return 0; }
    if (verify) { CERTCertificate *pc = SSL_PeerCertificate(s); if (pc) { printf("verified: %s\n", pc->subjectName); CERT_DestroyCertificate(pc); } }
    SSLChannelInfo ci; SSLCipherSuiteInfo csi;
    memset(&ci, 0, sizeof ci); memset(&csi, 0, sizeof csi);
    if (SSL_GetChannelInfo(s, &ci, sizeof ci) != SECSuccess) printf("channel info: error %d\n", PR_GetError());
    SSL_GetCipherSuiteInfo(ci.cipherSuite, &csi, sizeof csi);
    printf("TLS %d.%d, %s\n", (ci.protocolVersion >> 8) - 2, (ci.protocolVersion & 0xff) - 1, csi.cipherSuiteName);
    char req[256];
    snprintf(req, sizeof req, "HEAD / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    PRInt32 w = PR_Write(s, req, (PRInt32)strlen(req));
    if (w < 0) printf("write: error %d\n", PR_GetError());
    char resp[512] = "";
    PRInt32 n = PR_Read(s, resp, sizeof resp - 1);
    if (n < 0) printf("read: error %d\n", PR_GetError());
    if (n > 0) { resp[n] = 0; char *e = strstr(resp, "\r\n"); if (e) *e = 0; printf("response: %s\n", resp); }
    PR_Close(s);
    return n > 0 && !strncmp(resp, "HTTP/", 5);
}

int main(int argc, char **argv)
{
    PR_Init(PR_USER_THREAD, PR_PRIORITY_NORMAL, 0);
    CHECK("NSS_NoDB_Init", NSS_NoDB_Init(NULL) == SECSuccess);
    printf("NSS %s\n", NSS_GetVersion());

    unsigned char d[32]; char h[80];
    PK11_HashBuf(SEC_OID_SHA256, d, (const unsigned char *)"abc", 3);
    hex(d, 32, h);
    CHECK("SHA-256(abc)", !strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    PK11SlotInfo *slot = PK11_GetInternalSlot();
    unsigned char keyb[16] = { 0 };
    for (int i = 0; i < 16; i++) keyb[i] = (unsigned char)i;
    SECItem ki = { siBuffer, keyb, 16 };
    PK11SymKey *hk = PK11_ImportSymKey(slot, CKM_SHA256_HMAC, PK11_OriginUnwrap, CKA_SIGN, &ki, NULL);
    PK11Context *hc = hk ? PK11_CreateContextBySymKey(CKM_SHA256_HMAC, CKA_SIGN, hk, &(SECItem){ siBuffer, NULL, 0 }) : NULL;
    unsigned mac_len = 0; unsigned char mac[32];
    int hmac_ok = hc && PK11_DigestBegin(hc) == SECSuccess && PK11_DigestOp(hc, (const unsigned char *)"QRX", 3) == SECSuccess &&
                  PK11_DigestFinal(hc, mac, &mac_len, sizeof mac) == SECSuccess && mac_len == 32;
    if (hc) PK11_DestroyContext(hc, PR_TRUE);
    hex(mac, 32, h);
    printf("HMAC-SHA256(key 00..0f, \"QRX\") = %s\n", h);
    /* expected value from Python's hmac module on the host */
    CHECK("HMAC-SHA256", hmac_ok && !strcmp(h, "02431dc7482422cf0fe987ca0fe9f16e556121febeded74602e199128872b563"));

    PK11SymKey *ak = PK11_ImportSymKey(slot, CKM_AES_GCM, PK11_OriginUnwrap, CKA_ENCRYPT, &ki, NULL);
    unsigned char iv[12] = { 1,2,3,4,5,6,7,8,9,10,11,12 };
    CK_GCM_PARAMS gp = { iv, 12, 96, NULL, 0, 128 };
    SECItem par = { siBuffer, (unsigned char *)&gp, sizeof gp };
    const char *plain = "Hallo NSS auf QRX OS One 64";
    unsigned char ct[64], pt[64]; unsigned ctl = 0, ptl = 0;
    int gcm_ok = ak && PK11_Encrypt(ak, CKM_AES_GCM, &par, ct, &ctl, sizeof ct, (const unsigned char *)plain, (unsigned)strlen(plain)) == SECSuccess &&
                 PK11_Decrypt(ak, CKM_AES_GCM, &par, pt, &ptl, sizeof pt, ct, ctl) == SECSuccess &&
                 ptl == strlen(plain) && !memcmp(pt, plain, ptl);
    ct[0] ^= 1;
    int tamper = ak && PK11_Decrypt(ak, CKM_AES_GCM, &par, pt, &ptl, sizeof pt, ct, ctl) != SECSuccess;
    CHECK("AES-128-GCM encrypt/decrypt, tamper detected", gcm_ok && tamper);

    SECOidData *curve = SECOID_FindOIDByTag(SEC_OID_ANSIX962_EC_PRIME256V1);
    SECItem ecp = { siBuffer, NULL, 0 };
    SECITEM_AllocItem(NULL, &ecp, 2 + curve->oid.len);
    ecp.data[0] = SEC_ASN1_OBJECT_ID; ecp.data[1] = (unsigned char)curve->oid.len;
    memcpy(ecp.data + 2, curve->oid.data, curve->oid.len);
    SECKEYPublicKey *pub = NULL;
    PRIntervalTime t0 = PR_IntervalNow();
    SECKEYPrivateKey *priv = PK11_GenerateKeyPair(slot, CKM_EC_KEY_PAIR_GEN, &ecp, &pub, PR_FALSE, PR_FALSE, NULL);
    unsigned char dig[32];
    PK11_HashBuf(SEC_OID_SHA256, dig, (const unsigned char *)plain, (unsigned)strlen(plain));
    SECItem digi = { siBuffer, dig, 32 }, sig = { siBuffer, NULL, 0 };
    int ec_ok = priv && pub && SECITEM_AllocItem(NULL, &sig, (unsigned)PK11_SignatureLen(priv)) &&
                PK11_Sign(priv, &sig, &digi) == SECSuccess && PK11_Verify(pub, &sig, &digi, NULL) == SECSuccess;
    dig[0] ^= 1;
    int ec_bad = pub && PK11_Verify(pub, &sig, &digi, NULL) != SECSuccess;
    printf("ECDSA P-256 keygen+sign+verify: %u ms\n", PR_IntervalToMilliseconds(PR_IntervalNow() - t0));
    CHECK("ECDSA P-256", ec_ok && ec_bad);

    unsigned char r1[16], r2[16];
    CHECK("random", PK11_GenerateRandom(r1, 16) == SECSuccess && PK11_GenerateRandom(r2, 16) == SECSuccess && memcmp(r1, r2, 16));

    if (argc > 2) {
        NSS_SetDomesticPolicy();
        CHECK("TLS handshake and HTTP response (certificate shown)", tls_test(argv[1], atoi(argv[2]), 0));
        if (argc > 3) {
            /* the real check: without the root the handshake must fail, with it it must pass */
            CHECK("TLS refuses an unknown root", !tls_test(argv[1], atoi(argv[2]), 1));
            CHECK("trusted root loaded", trust_root(argv[3]));
            CHECK("TLS with certificate verification", tls_test(argv[1], atoi(argv[2]), 1));
        }
    }
    printf(failures ? "NSS %d FAILED\n" : "NSS ALL PASS\n", failures);
    return failures != 0;
}
