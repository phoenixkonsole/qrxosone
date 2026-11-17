/* QRX OS One's users and their encrypted home volumes (qrx_vault.h).
 *
 * ~/Library/Application Support/QRX OS One/users (QRX_VAULT_DIR for tests), mode 0700:
 *   users.db      K <recovery public key>                      the system's ML-KEM-1024 key
 *                 U <name> <admin|user> <sectors> <salt> <t> <m KiB> <p> <wrap> <kem ct> <kem wrap>
 *                 A <name> <wrapped recovery private key>      one per admin
 *   <name>.qrxu   a 4096-byte header, then the sectors, AES-256-XTS with the sector number as tweak
 * All binary fields are hex. A wrap is nonce(12) | AES-256-GCM ciphertext | tag(16), with the user's
 * name and the field's purpose as associated data, so a wrap cannot be moved to another user.
 *
 * Built against OpenSSL 3.5 or later (ML-KEM, Argon2id). Without it (QRX_HAVE_OPENSSL undefined)
 * the service answers QRX_VAULT_EUNAVAILABLE and the system runs with no users, as before. */
#include "qrx_vault.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAIL(e) do { a[8] = UINT32_MAX; a[9] = (e); return; } while (0)

#ifndef QRX_HAVE_OPENSSL
void qrx_vault_call(uint32_t op, uint32_t *a, uint8_t *ram, uint64_t ram_size)
{
    (void)op; (void)ram; (void)ram_size;
    if (op == QRX_VAULT_STATUS) { a[8] = 0; a[10] = 0; a[11] = 0; a[12] = 0; a[9] = QRX_VAULT_EUNAVAILABLE; return; }
    FAIL(QRX_VAULT_EUNAVAILABLE);
}
int qrx_vault_admin_or_unset(void) { return 1; }
#else
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#define VK 64                   /* the XTS key: two AES-256 keys */
#define KEK 32
#define SALT 16
#define WRAP_VK (12 + VK + 16)
#define SECTOR 512u
#define HEADER 4096u
#define MAX_USERS 32
#define KEM_CT 1568             /* ML-KEM-1024 */
#define KEM_PUB 1568
#define KEM_PRIV_MAX 3200
#define ARGON_T 3u
#define ARGON_M 65536u          /* KiB */
#define ARGON_P 1u

struct user {
    char name[32];
    int admin;
    uint64_t sectors;
    uint8_t salt[SALT];
    uint32_t t, m, p;
    uint8_t wrap[WRAP_VK];
    uint8_t kem_ct[KEM_CT];
    uint8_t kem_wrap[WRAP_VK];
    uint8_t *priv_wrap;         /* admins: the recovery private key under their password */
    size_t priv_wrap_len;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct user users[MAX_USERS];
static int nusers = -1;         /* -1 not loaded */
static uint8_t kem_pub[KEM_PUB];
static int have_kem;
/* the session: one user at a time */
static int current = -1;
static uint8_t vkey[VK];
static EVP_CIPHER_CTX *xts_enc, *xts_dec;
static EVP_PKEY *recovery;      /* an admin's session only */
static int image_fd = -1;

/* ---- files */
static void dir_path(char *out, size_t n)
{
    const char *e = getenv("QRX_VAULT_DIR"), *h = getenv("HOME");
    if (e) snprintf(out, n, "%s", e);
    else snprintf(out, n, "%s/Library/Application Support/QRX OS One/users", h ? h : ".");
}
static void file_path(char *out, size_t n, const char *name)
{
    char d[PATH_MAX];
    dir_path(d, sizeof d);
    snprintf(out, n, "%s/%s", d, name);
}
static void make_dir(void)
{
    char d[PATH_MAX];
    dir_path(d, sizeof d);
    for (char *s = d + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(d, 0700); *s = '/'; }
    mkdir(d, 0700);
    chmod(d, 0700);
}

static void hex_out(FILE *f, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", b[i]);
}
static size_t hex_in(const char *s, uint8_t *b, size_t max)
{
    size_t n = 0;
    while (s[0] && s[1] && n < max) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) break;
        b[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void forget_users(void)
{
    for (int i = 0; i < MAX_USERS; i++) { free(users[i].priv_wrap); OPENSSL_cleanse(&users[i], sizeof users[i]); }
    nusers = 0;
    have_kem = 0;
}
static int find(const char *name)
{
    for (int i = 0; i < nusers; i++) if (!strcmp(users[i].name, name)) return i;
    return -1;
}
static void load(void)
{
    if (nusers >= 0) return;
    forget_users();
    char path[PATH_MAX];
    file_path(path, sizeof path, "users.db");
    FILE *f = fopen(path, "r");
    if (!f) return;
    static char line[16384];
    while (fgets(line, sizeof line, f)) {
        char *tok[12] = { 0 };
        int nt = 0;
        for (char *s = strtok(line, " \n"); s && nt < 12; s = strtok(NULL, " \n")) tok[nt++] = s;
        if (nt == 2 && !strcmp(tok[0], "K")) have_kem = hex_in(tok[1], kem_pub, KEM_PUB) == KEM_PUB;
        else if (nt == 11 && !strcmp(tok[0], "U") && nusers < MAX_USERS && strlen(tok[1]) < 32) {
            struct user *u = &users[nusers];
            snprintf(u->name, sizeof u->name, "%s", tok[1]);
            u->admin = !strcmp(tok[2], "admin");
            u->sectors = strtoull(tok[3], NULL, 10);
            u->t = (uint32_t)strtoul(tok[5], NULL, 10);
            u->m = (uint32_t)strtoul(tok[6], NULL, 10);
            u->p = (uint32_t)strtoul(tok[7], NULL, 10);
            if (hex_in(tok[4], u->salt, SALT) == SALT && hex_in(tok[8], u->wrap, WRAP_VK) == WRAP_VK &&
                hex_in(tok[9], u->kem_ct, KEM_CT) == KEM_CT && hex_in(tok[10], u->kem_wrap, WRAP_VK) == WRAP_VK)
                nusers++;
            else OPENSSL_cleanse(u, sizeof *u);
        } else if (nt == 3 && !strcmp(tok[0], "A")) {
            int i = find(tok[1]);
            size_t n = strlen(tok[2]) / 2;
            if (i >= 0 && n <= 12 + KEM_PRIV_MAX + 16 && !users[i].priv_wrap) {
                users[i].priv_wrap = malloc(n);
                users[i].priv_wrap_len = hex_in(tok[2], users[i].priv_wrap, n);
            }
        }
    }
    OPENSSL_cleanse(line, sizeof line);
    fclose(f);
}
static int save(void)
{
    char path[PATH_MAX], tmp[PATH_MAX];
    make_dir();
    file_path(path, sizeof path, "users.db");
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) return -1;
    if (have_kem) { fputs("K ", f); hex_out(f, kem_pub, KEM_PUB); fputc('\n', f); }
    for (int i = 0; i < nusers; i++) {
        struct user *u = &users[i];
        fprintf(f, "U %s %s %llu ", u->name, u->admin ? "admin" : "user", (unsigned long long)u->sectors);
        hex_out(f, u->salt, SALT);
        fprintf(f, " %u %u %u ", u->t, u->m, u->p);
        hex_out(f, u->wrap, WRAP_VK); fputc(' ', f);
        hex_out(f, u->kem_ct, KEM_CT); fputc(' ', f);
        hex_out(f, u->kem_wrap, WRAP_VK); fputc('\n', f);
    }
    for (int i = 0; i < nusers; i++)
        if (users[i].admin && users[i].priv_wrap) {
            fprintf(f, "A %s ", users[i].name);
            hex_out(f, users[i].priv_wrap, users[i].priv_wrap_len);
            fputc('\n', f);
        }
    int bad = fflush(f) || fsync(fileno(f));
    fclose(f);
    if (bad || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

/* ---- crypto */
static int kdf(const char *password, const uint8_t *salt, uint32_t t, uint32_t m, uint32_t p, uint8_t *out)
{
    EVP_KDF *k = EVP_KDF_fetch(NULL, "ARGON2ID", NULL);
    EVP_KDF_CTX *c = k ? EVP_KDF_CTX_new(k) : NULL;
    uint32_t threads = 1;
    OSSL_PARAM prm[] = {
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, (void *)password, strlen(password)),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void *)salt, SALT),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &t),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &m),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &p),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads),
        OSSL_PARAM_construct_end(),
    };
    int ok = c && EVP_KDF_derive(c, out, KEK, prm) == 1;
    EVP_KDF_CTX_free(c);
    EVP_KDF_free(k);
    return ok ? 0 : -1;
}
static void aad_of(char *aad, size_t n, const char *name, const char *purpose)
{
    snprintf(aad, n, "qrxvault1|%s|%s", name, purpose);
}
/* out: nonce | ciphertext | tag, len + 28 bytes */
static int wrap(const uint8_t *key, const char *name, const char *purpose, const uint8_t *in, size_t len, uint8_t *out)
{
    char aad[96];
    int n, ok;
    aad_of(aad, sizeof aad, name, purpose);
    if (RAND_bytes(out, 12) != 1) return -1;
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    ok = c && EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, key, out) == 1 &&
         EVP_EncryptUpdate(c, NULL, &n, (const uint8_t *)aad, (int)strlen(aad)) == 1 &&
         EVP_EncryptUpdate(c, out + 12, &n, in, (int)len) == 1 &&
         EVP_EncryptFinal_ex(c, out + 12 + n, &n) == 1 &&
         EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, out + 12 + len) == 1;
    EVP_CIPHER_CTX_free(c);
    return ok ? 0 : -1;
}
static int unwrap(const uint8_t *key, const char *name, const char *purpose, const uint8_t *in, size_t len, uint8_t *out)
{
    char aad[96];
    int n, ok;
    if (len < 28) return -1;
    len -= 28;
    aad_of(aad, sizeof aad, name, purpose);
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    ok = c && EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, key, in) == 1 &&
         EVP_DecryptUpdate(c, NULL, &n, (const uint8_t *)aad, (int)strlen(aad)) == 1 &&
         EVP_DecryptUpdate(c, out, &n, in + 12, (int)len) == 1 &&
         EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, (void *)(in + 12 + len)) == 1 &&
         EVP_DecryptFinal_ex(c, out + n, &n) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) OPENSSL_cleanse(out, len);
    return ok ? 0 : -1;
}
/* the recovery key's shared secret, turned into a wrapping key */
static void kem_kek(const uint8_t *ss, size_t n, uint8_t *out)
{
    static const char label[] = "qrxvault1 recovery";
    unsigned len = KEK;
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    EVP_DigestInit_ex(c, EVP_sha256(), NULL);
    EVP_DigestUpdate(c, label, sizeof label - 1);
    EVP_DigestUpdate(c, ss, n);
    EVP_DigestFinal_ex(c, out, &len);
    EVP_MD_CTX_free(c);
}
static EVP_PKEY *kem_public(void)
{
    return EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-1024", NULL, kem_pub, KEM_PUB);
}
/* the volume key for the recovery key: kem_ct and kem_wrap */
static int kem_seal(struct user *u, const uint8_t *vk)
{
    EVP_PKEY *pub = kem_public();
    EVP_PKEY_CTX *c = pub ? EVP_PKEY_CTX_new_from_pkey(NULL, pub, NULL) : NULL;
    uint8_t ss[64], k[KEK];
    size_t ctlen = KEM_CT, sslen = sizeof ss;
    int ok = c && EVP_PKEY_encapsulate_init(c, NULL) == 1 && EVP_PKEY_encapsulate(c, u->kem_ct, &ctlen, ss, &sslen) == 1 &&
             ctlen == KEM_CT;
    if (ok) {
        kem_kek(ss, sslen, k);
        ok = wrap(k, u->name, "recovery", vk, VK, u->kem_wrap) == 0;
    }
    OPENSSL_cleanse(ss, sizeof ss);
    OPENSSL_cleanse(k, sizeof k);
    EVP_PKEY_CTX_free(c);
    EVP_PKEY_free(pub);
    return ok ? 0 : -1;
}
static int kem_open(const struct user *u, uint8_t *vk)
{
    EVP_PKEY_CTX *c = recovery ? EVP_PKEY_CTX_new_from_pkey(NULL, recovery, NULL) : NULL;
    uint8_t ss[64], k[KEK];
    size_t sslen = sizeof ss;
    int ok = c && EVP_PKEY_decapsulate_init(c, NULL) == 1 && EVP_PKEY_decapsulate(c, ss, &sslen, u->kem_ct, KEM_CT) == 1;
    if (ok) {
        kem_kek(ss, sslen, k);
        ok = unwrap(k, u->name, "recovery", u->kem_wrap, WRAP_VK, vk) == 0;
    }
    OPENSSL_cleanse(ss, sizeof ss);
    OPENSSL_cleanse(k, sizeof k);
    EVP_PKEY_CTX_free(c);
    return ok ? 0 : -1;
}
/* an admin's copy of the recovery private key, under that admin's key */
static int seal_recovery(struct user *u, const uint8_t *kek)
{
    uint8_t priv[KEM_PRIV_MAX];
    size_t n = sizeof priv;
    if (!recovery || EVP_PKEY_get_raw_private_key(recovery, priv, &n) != 1) return -1;
    free(u->priv_wrap);
    u->priv_wrap = malloc(n + 28);
    u->priv_wrap_len = n + 28;
    int r = wrap(kek, u->name, "recovery-private", priv, n, u->priv_wrap);
    OPENSSL_cleanse(priv, sizeof priv);
    return r;
}
static EVP_PKEY *open_recovery(const struct user *u, const uint8_t *kek)
{
    uint8_t priv[KEM_PRIV_MAX];
    if (!u->priv_wrap || u->priv_wrap_len < 28 || u->priv_wrap_len - 28 > sizeof priv) return NULL;
    if (unwrap(kek, u->name, "recovery-private", u->priv_wrap, u->priv_wrap_len, priv)) return NULL;
    EVP_PKEY *k = EVP_PKEY_new_raw_private_key_ex(NULL, "ML-KEM-1024", NULL, priv, u->priv_wrap_len - 28);
    OPENSSL_cleanse(priv, sizeof priv);
    return k;
}
/* a fresh password for u: salt, Argon2id, the wrapped volume key; kek_out for the admin copy */
static int set_password(struct user *u, const char *password, const uint8_t *vk, uint8_t *kek_out)
{
    uint8_t kek[KEK];
    if (RAND_bytes(u->salt, SALT) != 1) return -1;
    u->t = ARGON_T; u->m = ARGON_M; u->p = ARGON_P;
    if (kdf(password, u->salt, u->t, u->m, u->p, kek)) return -1;
    int r = wrap(kek, u->name, "volume", vk, VK, u->wrap);
    if (kek_out) memcpy(kek_out, kek, KEK);
    OPENSSL_cleanse(kek, sizeof kek);
    return r;
}

static void xts_tweak(uint64_t sector, uint8_t *iv)
{
    memset(iv, 0, 16);
    for (int i = 0; i < 8; i++) iv[i] = (uint8_t)(sector >> (8 * i));
}
static int xts(EVP_CIPHER_CTX *c, int enc, uint64_t sector, const uint8_t *in, uint8_t *out, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        uint8_t iv[16];
        int n;
        xts_tweak(sector + i, iv);
        if (EVP_CipherInit_ex(c, NULL, NULL, NULL, iv, enc) != 1 ||
            EVP_CipherUpdate(c, out + i * SECTOR, &n, in + i * SECTOR, SECTOR) != 1) return -1;
    }
    return 0;
}
static EVP_CIPHER_CTX *xts_ctx(const uint8_t *vk, int enc)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    if (c && EVP_CipherInit_ex(c, EVP_aes_256_xts(), NULL, vk, NULL, enc) == 1) return c;
    EVP_CIPHER_CTX_free(c);
    return NULL;
}
static int volume_key(uint8_t *vk)
{
    do { if (RAND_bytes(vk, VK) != 1) return -1; } while (!CRYPTO_memcmp(vk, vk + 32, 32));   /* XTS needs two keys */
    return 0;
}

/* ---- images */
static void image_path(char *out, size_t n, const char *name)
{
    char f[64];
    snprintf(f, sizeof f, "%s.qrxu", name);
    file_path(out, n, f);
}
/* a new image, every sector encrypted zeros: nothing shows which parts are in use */
static int make_image(const char *name, uint64_t sectors, const uint8_t *vk)
{
    char path[PATH_MAX];
    image_path(path, sizeof path, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    uint8_t head[HEADER] = "QRXVAULT";
    head[8] = 1;
    for (int i = 0; i < 8; i++) head[16 + i] = (uint8_t)(sectors >> (56 - 8 * i));
    snprintf((char *)head + 32, 32, "%s", name);
    int ok = write(fd, head, HEADER) == HEADER;
    enum { CHUNK = 2048 };
    static uint8_t zero[CHUNK * SECTOR], enc[CHUNK * SECTOR];
    EVP_CIPHER_CTX *c = xts_ctx(vk, 1);
    for (uint64_t s = 0; ok && s < sectors; s += CHUNK) {
        unsigned n = sectors - s < CHUNK ? (unsigned)(sectors - s) : CHUNK;
        ok = c && !xts(c, 1, s, zero, enc, n) && write(fd, enc, (size_t)n * SECTOR) == (ssize_t)n * SECTOR;
    }
    EVP_CIPHER_CTX_free(c);
    ok = ok && !fsync(fd);
    close(fd);
    if (!ok) unlink(path);
    return ok ? 0 : -1;
}

static void logout(void)
{
    OPENSSL_cleanse(vkey, sizeof vkey);
    EVP_CIPHER_CTX_free(xts_enc); xts_enc = NULL;
    EVP_CIPHER_CTX_free(xts_dec); xts_dec = NULL;
    EVP_PKEY_free(recovery); recovery = NULL;
    if (image_fd >= 0) close(image_fd);
    image_fd = -1;
    current = -1;
}

/* ---- the guest's text arguments */
static int text_arg(const uint32_t *a, const uint8_t *ram, uint64_t ram_size, char *buf, size_t n, char **lines, int want)
{
    if (a[2] >= n || (uint64_t)a[1] + a[2] > ram_size) return -1;
    memcpy(buf, ram + a[1], a[2]);
    buf[a[2]] = 0;
    int k = 0;
    char *s = buf;
    while (k < want) {
        lines[k++] = s;
        char *e = strchr(s, '\n');
        if (!e) break;
        *e = 0;
        s = e + 1;
    }
    for (int i = 0; i < k; i++) { size_t l = strlen(lines[i]); if (l && lines[i][l - 1] == '\r') lines[i][l - 1] = 0; }
    return k == want ? 0 : -1;
}
static int good_name(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > 31) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-'))
            return 0;
    return 1;
}

static void op_create(uint32_t *a, uint8_t *ram, uint64_t ram_size)
{
    char buf[512], *l[4];
    if (text_arg(a, ram, ram_size, buf, sizeof buf, l, 4) || !good_name(l[0]) || strlen(l[1]) < 4) FAIL(QRX_VAULT_EARGS);
    int first = nusers == 0;
    if (!first && (current < 0 || !users[current].admin)) FAIL(QRX_VAULT_EADMIN);
    if (find(l[0]) >= 0) FAIL(QRX_VAULT_EEXISTS);
    if (nusers >= MAX_USERS) FAIL(QRX_VAULT_EARGS);
    unsigned long mb = strtoul(l[3], NULL, 10);
    if (mb < 16 || mb > 1048576) FAIL(QRX_VAULT_EARGS);
    struct user *u = &users[nusers];
    memset(u, 0, sizeof *u);
    snprintf(u->name, sizeof u->name, "%s", l[0]);
    u->admin = first || !strcmp(l[2], "admin");
    u->sectors = (uint64_t)mb * 2048;
    uint8_t vk[VK], kek[KEK];
    int bad = 0;
    if (first) {
        /* the system's recovery key; its private half goes to this admin only */
        EVP_PKEY *k = EVP_PKEY_Q_keygen(NULL, NULL, "ML-KEM-1024");
        size_t pn = KEM_PUB;
        if (!k || EVP_PKEY_get_raw_public_key(k, kem_pub, &pn) != 1 || pn != KEM_PUB) { EVP_PKEY_free(k); FAIL(QRX_VAULT_ECRYPTO); }
        have_kem = 1;
        EVP_PKEY_free(recovery);
        recovery = k;
    }
    if (!have_kem) FAIL(QRX_VAULT_ECRYPTO);
    bad = volume_key(vk) || set_password(u, l[1], vk, kek) || kem_seal(u, vk) || (u->admin && seal_recovery(u, kek));
    OPENSSL_cleanse(kek, sizeof kek);
    OPENSSL_cleanse(buf, sizeof buf);
    if (bad) { OPENSSL_cleanse(vk, sizeof vk); if (first) { EVP_PKEY_free(recovery); recovery = NULL; have_kem = 0; } FAIL(QRX_VAULT_ECRYPTO); }
    make_dir();
    if (make_image(u->name, u->sectors, vk)) {
        OPENSSL_cleanse(vk, sizeof vk);
        if (first) { EVP_PKEY_free(recovery); recovery = NULL; have_kem = 0; }
        FAIL(QRX_VAULT_EIO);
    }
    OPENSSL_cleanse(vk, sizeof vk);
    nusers++;
    if (save()) { char p[PATH_MAX]; nusers--; image_path(p, sizeof p, u->name); unlink(p); FAIL(QRX_VAULT_EIO); }
    if (first) { EVP_PKEY_free(recovery); recovery = NULL; }  /* back when the admin logs in */
    fprintf(stderr, "[VAULT] user %s (%s), %lu MB\n", u->name, u->admin ? "admin" : "user", mb);
    a[8] = 0;
}

static void op_login(uint32_t *a, uint8_t *ram, uint64_t ram_size)
{
    char buf[256], *l[2];
    if (text_arg(a, ram, ram_size, buf, sizeof buf, l, 2)) FAIL(QRX_VAULT_EARGS);
    int i = find(l[0]);
    uint8_t kek[KEK], vk[VK];
    int bad = i < 0 || kdf(l[1], users[i].salt, users[i].t, users[i].m, users[i].p, kek) ||
              unwrap(kek, users[i].name, "volume", users[i].wrap, WRAP_VK, vk);
    OPENSSL_cleanse(buf, sizeof buf);
    if (bad) {
        OPENSSL_cleanse(kek, sizeof kek);
        sleep(1);                                  /* on top of Argon2id */
        FAIL(i < 0 ? QRX_VAULT_EPASSWORD : QRX_VAULT_EPASSWORD);   /* never tell which names exist */
    }
    logout();
    char path[PATH_MAX];
    image_path(path, sizeof path, users[i].name);
    image_fd = open(path, O_RDWR);
    xts_enc = xts_ctx(vk, 1);
    xts_dec = xts_ctx(vk, 0);
    if (users[i].admin) recovery = open_recovery(&users[i], kek);
    OPENSSL_cleanse(kek, sizeof kek);
    memcpy(vkey, vk, VK);
    OPENSSL_cleanse(vk, sizeof vk);
    if (image_fd < 0 || !xts_enc || !xts_dec) { logout(); FAIL(QRX_VAULT_EIO); }
    current = i;
    fprintf(stderr, "[VAULT] %s logged in%s\n", users[i].name, users[i].admin ? " (admin)" : "");
    a[8] = (uint32_t)users[i].sectors;
    a[10] = (uint32_t)i + 1;
    a[11] = (uint32_t)users[i].admin;
}

static void op_io(uint32_t op, uint32_t *a, uint8_t *ram, uint64_t ram_size)
{
    if (current < 0) FAIL(QRX_VAULT_ENOLOGIN);
    uint64_t sector = (uint64_t)a[3] << 32 | a[0], count = a[2];
    if (count > 2048 || sector > users[current].sectors || count > users[current].sectors - sector) FAIL(QRX_VAULT_EARGS);
    if ((uint64_t)a[1] + count * SECTOR > ram_size) FAIL(QRX_VAULT_EARGS);
    static uint8_t tmp[2048 * SECTOR];
    off_t at = (off_t)(HEADER + sector * SECTOR);
    size_t bytes = (size_t)count * SECTOR;
    if (op == QRX_VAULT_READ) {
        if (pread(image_fd, tmp, bytes, at) != (ssize_t)bytes || xts(xts_dec, 0, sector, tmp, ram + a[1], (unsigned)count))
            FAIL(QRX_VAULT_EIO);
    } else {
        if (xts(xts_enc, 1, sector, ram + a[1], tmp, (unsigned)count) || pwrite(image_fd, tmp, bytes, at) != (ssize_t)bytes)
            FAIL(QRX_VAULT_EIO);
    }
    a[8] = (uint32_t)count;
}

static void op_passwd(uint32_t *a, uint8_t *ram, uint64_t ram_size)
{
    char buf[256], *l[2];
    if (current < 0) FAIL(QRX_VAULT_ENOLOGIN);
    if (text_arg(a, ram, ram_size, buf, sizeof buf, l, 2) || strlen(l[1]) < 4) FAIL(QRX_VAULT_EARGS);
    struct user *u = &users[current], old = *u;
    uint8_t kek[KEK], vk[VK];
    if (kdf(l[0], u->salt, u->t, u->m, u->p, kek) || unwrap(kek, u->name, "volume", u->wrap, WRAP_VK, vk)) {
        OPENSSL_cleanse(buf, sizeof buf); OPENSSL_cleanse(kek, sizeof kek); sleep(1); FAIL(QRX_VAULT_EPASSWORD);
    }
    int bad = set_password(u, l[1], vk, kek) || (u->admin && seal_recovery(u, kek));
    OPENSSL_cleanse(buf, sizeof buf); OPENSSL_cleanse(kek, sizeof kek); OPENSSL_cleanse(vk, sizeof vk);
    if (bad || save()) { free(u->priv_wrap == old.priv_wrap ? NULL : u->priv_wrap); *u = old; FAIL(QRX_VAULT_ECRYPTO); }
    if (old.priv_wrap != u->priv_wrap) free(old.priv_wrap);
    a[8] = 0;
}

static void op_reset(uint32_t *a, uint8_t *ram, uint64_t ram_size)
{
    char buf[256], *l[2];
    if (current < 0 || !users[current].admin) FAIL(QRX_VAULT_EADMIN);
    if (!recovery) FAIL(QRX_VAULT_ECRYPTO);
    if (text_arg(a, ram, ram_size, buf, sizeof buf, l, 2) || strlen(l[1]) < 4) FAIL(QRX_VAULT_EARGS);
    int i = find(l[0]);
    if (i < 0) { OPENSSL_cleanse(buf, sizeof buf); FAIL(QRX_VAULT_ENOUSER); }
    struct user *u = &users[i], old = *u;
    uint8_t vk[VK], kek[KEK];
    int bad = kem_open(u, vk) || set_password(u, l[1], vk, kek) || (u->admin && seal_recovery(u, kek));
    OPENSSL_cleanse(buf, sizeof buf); OPENSSL_cleanse(kek, sizeof kek); OPENSSL_cleanse(vk, sizeof vk);
    if (bad || save()) { if (u->priv_wrap != old.priv_wrap) free(u->priv_wrap); *u = old; FAIL(QRX_VAULT_ECRYPTO); }
    if (old.priv_wrap != u->priv_wrap) free(old.priv_wrap);
    fprintf(stderr, "[VAULT] %s's password reset by %s\n", u->name, users[current].name);
    a[8] = 0;
}

static void op_delete(uint32_t *a, uint8_t *ram, uint64_t ram_size)
{
    char buf[64], *l[1];
    if (current < 0 || !users[current].admin) FAIL(QRX_VAULT_EADMIN);
    if (text_arg(a, ram, ram_size, buf, sizeof buf, l, 1)) FAIL(QRX_VAULT_EARGS);
    int i = find(l[0]);
    if (i < 0) FAIL(QRX_VAULT_ENOUSER);
    if (i == current || i == 0) FAIL(QRX_VAULT_EADMIN);   /* the first user stays the admin */
    char path[PATH_MAX];
    image_path(path, sizeof path, users[i].name);
    struct user gone = users[i];
    memmove(&users[i], &users[i + 1], (size_t)(nusers - i - 1) * sizeof users[0]);
    nusers--;
    memset(&users[nusers], 0, sizeof users[0]);
    if (current > i) current--;
    if (save()) { FAIL(QRX_VAULT_EIO); }
    free(gone.priv_wrap);
    unlink(path);
    fprintf(stderr, "[VAULT] user %s deleted\n", gone.name);
    a[8] = 0;
}

void qrx_vault_call(uint32_t op, uint32_t *a, uint8_t *ram, uint64_t ram_size)
{
    pthread_mutex_lock(&lock);
    load();
    a[8] = 0; a[9] = 0;
    switch (op) {
    case QRX_VAULT_STATUS:
        a[8] = (uint32_t)nusers;
        a[10] = current < 0 ? 0 : (uint32_t)current + 1;
        a[11] = current >= 0 && users[current].admin;
        a[12] = current < 0 ? 0 : (uint32_t)users[current].sectors;
        break;
    case QRX_VAULT_LIST: {
        char out[MAX_USERS * 48];
        size_t n = 0;
        for (int i = 0; i < nusers; i++) n += (size_t)snprintf(out + n, sizeof out - n, "%s %s\n", users[i].name, users[i].admin ? "admin" : "user");
        if ((uint64_t)a[1] + a[2] > ram_size) { a[8] = UINT32_MAX; a[9] = QRX_VAULT_EARGS; break; }
        if (n > a[2]) n = a[2];
        memcpy(ram + a[1], out, n);
        a[8] = (uint32_t)n;
        break;
    }
    case QRX_VAULT_CREATE: op_create(a, ram, ram_size); break;
    case QRX_VAULT_LOGIN: op_login(a, ram, ram_size); break;
    case QRX_VAULT_LOGOUT: logout(); break;
    case QRX_VAULT_READ: case QRX_VAULT_WRITE: op_io(op, a, ram, ram_size); break;
    case QRX_VAULT_PASSWD: op_passwd(a, ram, ram_size); break;
    case QRX_VAULT_RESET: op_reset(a, ram, ram_size); break;
    case QRX_VAULT_DELETE: op_delete(a, ram, ram_size); break;
    default: a[8] = UINT32_MAX; a[9] = QRX_VAULT_EARGS;
    }
    if (getenv("QRX_VAULT_TRACE")) fprintf(stderr, "[VAULT] op %u -> %u (%u)\n", op, a[8], a[9]);
    pthread_mutex_unlock(&lock);
}

int qrx_vault_admin_or_unset(void)
{
    pthread_mutex_lock(&lock);
    load();
    int r = nusers == 0 || (current >= 0 && users[current].admin);
    pthread_mutex_unlock(&lock);
    return r;
}
#endif
