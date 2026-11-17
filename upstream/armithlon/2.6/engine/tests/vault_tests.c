/* qrx_vault: the first user is the admin, wrong passwords fail, sectors round-trip encrypted,
 * admins reset passwords through the ML-KEM recovery key, users cannot administer. */
#include "qrx_vault.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint8_t ram[1 << 20];
static uint32_t a[13];
static uint32_t call(uint32_t op, const char *text)
{
    memset(a, 0, sizeof a);
    if (text) { strcpy((char *)ram + 0x1000, text); a[1] = 0x1000; a[2] = (uint32_t)strlen(text); }
    qrx_vault_call(op, a, ram, sizeof ram);
    return a[8];
}
static uint32_t io(uint32_t op, uint32_t sector, uint32_t buf, uint32_t n)
{
    memset(a, 0, sizeof a);
    a[0] = sector; a[1] = buf; a[2] = n;
    qrx_vault_call(op, a, ram, sizeof ram);
    return a[8];
}

int main(int argc, char **argv)
{
    const char *prog = argv[0];
    if (argc > 1) {                     /* the reload check, in a fresh process */
        for (int i = 0; i < 4096; i++) ram[0x2000 + i] = (uint8_t)("QRX OS One home "[i % 16]);
        if (call(QRX_VAULT_STATUS, NULL) != 2 || call(QRX_VAULT_LOGIN, "alice\nsecret-1") == UINT32_MAX || a[11] != 1) return 1;
        if (io(QRX_VAULT_READ, 100, 0x4000, 8) != 8 || memcmp(ram + 0x2000, ram + 0x4000, 4096)) return 2;
        return call(QRX_VAULT_RESET, "bob\nbob-3") == 0 ? 0 : 3;     /* the recovery key came back from disk too */
    }
    char dir[] = "/tmp/qrxvaultXXXXXX";
    assert(mkdtemp(dir));
    setenv("QRX_VAULT_DIR", dir, 1);
    assert(call(QRX_VAULT_STATUS, NULL) == 0 && qrx_vault_admin_or_unset());
    /* the first user is the admin, whatever the request says */
    assert(call(QRX_VAULT_CREATE, "alice\nsecret-1\nuser\n16") == 0);
    assert(!qrx_vault_admin_or_unset());
    /* nobody logged in: no more users */
    assert(call(QRX_VAULT_CREATE, "bob\nbobpass\nuser\n16") == UINT32_MAX && a[9] == QRX_VAULT_EADMIN);
    assert(call(QRX_VAULT_LOGIN, "alice\nwrong") == UINT32_MAX && a[9] == QRX_VAULT_EPASSWORD);
    assert(call(QRX_VAULT_LOGIN, "alice\nsecret-1") == 16 * 2048 && a[11] == 1);
    assert(qrx_vault_admin_or_unset());
    assert(call(QRX_VAULT_CREATE, "bob\nbobpass\nuser\n16") == 0);
    assert(call(QRX_VAULT_CREATE, "bob\nx2345\nuser\n16") == UINT32_MAX && a[9] == QRX_VAULT_EEXISTS);
    /* sectors: what is written comes back, and the file holds neither the text nor zeros */
    for (int i = 0; i < 4096; i++) ram[0x2000 + i] = (uint8_t)("QRX OS One home "[i % 16]);
    assert(io(QRX_VAULT_WRITE, 100, 0x2000, 8) == 8);
    memset(ram + 0x4000, 0, 4096);
    assert(io(QRX_VAULT_READ, 100, 0x4000, 8) == 8 && !memcmp(ram + 0x2000, ram + 0x4000, 4096));
    assert(io(QRX_VAULT_READ, 16 * 2048 - 1, 0x4000, 2) == UINT32_MAX);
    char path[300];
    snprintf(path, sizeof path, "%s/alice.qrxu", dir);
    FILE *f = fopen(path, "rb");
    static uint8_t img[4096 + 16 * 2048 * 512];
    size_t n = fread(img, 1, sizeof img, f);
    fclose(f);
    assert(n == sizeof img);
    assert(!memmem(img, n, "QRX OS One home", 15));
    static const uint8_t zeros[64];
    assert(!memmem(img + 4096, n - 4096, zeros, 64));
    assert(call(QRX_VAULT_LIST, NULL) == 0);
    a[1] = 0x6000; a[2] = 200; qrx_vault_call(QRX_VAULT_LIST, a, ram, sizeof ram);
    assert(!memcmp(ram + 0x6000, "alice admin\nbob user\n", a[8]));
    /* bob forgot his password: the admin resets it through the recovery key */
    assert(call(QRX_VAULT_RESET, "bob\nnewbob") == 0);
    assert(call(QRX_VAULT_LOGOUT, NULL) == 0);
    assert(io(QRX_VAULT_READ, 0, 0x4000, 1) == UINT32_MAX && a[9] == QRX_VAULT_ENOLOGIN);
    assert(call(QRX_VAULT_LOGIN, "bob\nbobpass") == UINT32_MAX);
    assert(call(QRX_VAULT_LOGIN, "bob\nnewbob") == 16 * 2048 && a[11] == 0);
    assert(!qrx_vault_admin_or_unset());
    assert(call(QRX_VAULT_CREATE, "eve\nevepass\nadmin\n16") == UINT32_MAX && a[9] == QRX_VAULT_EADMIN);
    assert(call(QRX_VAULT_DELETE, "alice") == UINT32_MAX && a[9] == QRX_VAULT_EADMIN);
    assert(call(QRX_VAULT_PASSWD, "newbob\nbob-3") == 0);
    /* alice's data are still hers; a new process reads users.db from disk */
    char self[400];
    snprintf(self, sizeof self, "%s reload", prog);
    assert(system(self) == 0);
    assert(call(QRX_VAULT_LOGIN, "alice\nsecret-1") != UINT32_MAX);
    /* a second admin gets its own copy of the recovery key and can reset too */
    assert(call(QRX_VAULT_CREATE, "carol\ncarolpw\nadmin\n16") == 0);
    assert(call(QRX_VAULT_LOGIN, "carol\ncarolpw") != UINT32_MAX && a[11] == 1);
    assert(call(QRX_VAULT_RESET, "bob\nbob-4") == 0);
    assert(call(QRX_VAULT_DELETE, "alice") == UINT32_MAX);
    assert(call(QRX_VAULT_DELETE, "bob") == 0);
    assert(call(QRX_VAULT_LOGIN, "bob\nbob-4") == UINT32_MAX);
    printf("vault: admin, users, XTS images, Argon2id, ML-KEM recovery all ok\n");
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    return system(cmd);
}
