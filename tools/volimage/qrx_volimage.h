/*
 * qrx_volimage.h - encrypted QRX volume images ("DMG for QRX").
 *
 * A .qrxvol file is a single encrypted container that holds a drawer
 * tree.  It is protected with a username and a password:
 *
 *   key material = PBKDF2-HMAC-SHA256(username NUL password, salt, N)
 *   payload      = AES-256-CTR archive of files and drawers
 *   integrity    = HMAC-SHA256 over header, username and ciphertext
 *
 * The image can be copied anywhere, including into IPFS, without
 * exposing its contents.  Mounting is not required: QRXVol creates and
 * unpacks images, QRX Drive can hand the result straight to IPFS.
 */
#ifndef QRX_VOLIMAGE_H
#define QRX_VOLIMAGE_H

#define QRX_VOL_MAGIC       "QRXVOL1"
#define QRX_VOL_VERSION     1
#define QRX_VOL_DEFAULT_ITER 12000
#define QRX_VOL_EXT         ".qrxvol"
#define QRX_VOL_USER_MAX    63
#define QRX_VOL_PASS_MAX    127

struct qrx_volinfo {
    char user[QRX_VOL_USER_MAX + 1];
    unsigned long iterations;
    unsigned long payload_bytes;
    unsigned long files;
    unsigned long bytes;
};

/* Create image from source drawer/file.  Returns 1 on success. */
int qrx_volimage_create(const char *image, const char *source,
                        const char *user, const char *password,
                        char *err, unsigned long errcap);

/* Verify only the HMAC and header. */
int qrx_volimage_verify(const char *image, const char *user,
                        const char *password, char *err, unsigned long errcap);

/* Unpack into dest (created when missing). */
int qrx_volimage_extract(const char *image, const char *dest,
                         const char *user, const char *password,
                         char *err, unsigned long errcap);

/* Read the clear header: username, iterations and payload size. */
int qrx_volimage_info(const char *image, struct qrx_volinfo *info,
                      char *err, unsigned long errcap);

#endif
