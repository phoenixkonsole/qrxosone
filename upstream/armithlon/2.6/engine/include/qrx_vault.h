#ifndef QRX_VAULT_H
#define QRX_VAULT_H
/* QRX OS One's users and their encrypted home volumes (qrx_vault.c).
 *
 * Each user has an image (<name>.qrxu) encrypted with AES-256-XTS under a random 512-bit volume key.
 * The volume key is kept twice in users.db: wrapped with AES-256-GCM under a key from the user's
 * password (Argon2id, 64 MiB), and wrapped under an ML-KEM-1024 shared secret for the system's
 * recovery key, whose private half only admins can unwrap. The first user is always the admin.
 * The keys never reach guest memory; the guest reads and writes plain sectors through this service.
 *
 * Host call service QRX_VAULT_SERVICE, the network's block layout: q[3] op, a[0..12] at q+16,
 * a[8] the result (UINT32_MAX on failure), a[9] the reason (QRX_VAULT_E*). Texts are lines in guest
 * memory at a[1], a[2] bytes long. */
#include <stdint.h>

#define QRX_VAULT_SERVICE 9u

enum {
    QRX_VAULT_STATUS = 1,   /* a[8] users, a[10] the logged-in user's number + 1, a[11] 1 admin, a[12] sectors */
    QRX_VAULT_LIST = 2,     /* a[1], a[2] out: "name admin|user\n" per user; a[8] bytes */
    QRX_VAULT_CREATE = 3,   /* "name\npassword\nadmin|user\nsize in MB": the first user is the admin, later ones need an admin */
    QRX_VAULT_LOGIN = 4,    /* "name\npassword": a[8] sectors, a[10] number + 1, a[11] 1 admin */
    QRX_VAULT_LOGOUT = 5,
    QRX_VAULT_READ = 6,     /* a[0] (a[3] high) the first 512-byte sector, a[1] buffer, a[2] sectors */
    QRX_VAULT_WRITE = 7,
    QRX_VAULT_PASSWD = 8,   /* "old\nnew" for the logged-in user */
    QRX_VAULT_RESET = 9,    /* admin: "name\nnew password", through the recovery key */
    QRX_VAULT_DELETE = 10,  /* admin: "name"; never the admin who is logged in */
};
enum {
    QRX_VAULT_EARGS = 1, QRX_VAULT_EEXISTS = 2, QRX_VAULT_ENOUSER = 3, QRX_VAULT_EPASSWORD = 4,
    QRX_VAULT_EADMIN = 5, QRX_VAULT_ENOLOGIN = 6, QRX_VAULT_EIO = 7, QRX_VAULT_ECRYPTO = 8,
    QRX_VAULT_EUNAVAILABLE = 9,
};

/* One host call: a[] as above, guest memory ram[0..ram_size) */
void qrx_vault_call(uint32_t op, uint32_t *a, uint8_t *ram, uint64_t ram_size);
/* Settings that change the whole system (the network): nobody set up yet, or an admin logged in */
int qrx_vault_admin_or_unset(void);
#endif
