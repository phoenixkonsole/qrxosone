#ifndef QRX_FORKFILE_H
#define QRX_FORKFILE_H

/*
 * Read-only readers for the fork containers that carry Macintosh files
 * outside a Mac volume:
 *
 *   AppleSingle  (magic 0x00051600)  both forks plus Finder info in one file
 *   AppleDouble  (magic 0x00051607)  the same header, usually resource fork
 *                                   and metadata for a real data file
 *   MacBinary II (128-byte header)   name, type/creator, both forks
 *   BinHex 4.0   (text)              hex-encoded, 0x90 run-length compressed
 *
 * Nothing is written and no Apple material is needed: the synthetic test
 * containers come from qrx_testimg.c.
 *
 * Only BinHex needs scratch memory, because its forks are compressed; every
 * other format points straight into the caller's blob.  Pass `work` with
 * `work_cap` big enough for the decoded BinHex stream (header + both forks).
 */

#include "qrx_macimage.h"

#define QRX_FORKFILE_NAME_MAX 64

typedef enum {
    QRX_FORKFILE_NONE = 0,
    QRX_FORKFILE_APPLESINGLE,
    QRX_FORKFILE_APPLEDOUBLE,
    QRX_FORKFILE_MACBINARY,
    QRX_FORKFILE_BINHEX
} qrx_forkfile_format;

typedef struct {
    qrx_forkfile_format format;
    uint8_t has_name;
    uint8_t crc_state;      /* 0 = not present, 1 = matches, 2 = mismatch */
    char name[QRX_FORKFILE_NAME_MAX];
    uint8_t name_len;
    uint32_t type_code;
    uint32_t creator;
    uint16_t fd_flags;
    uint32_t fd_location;   /* Finder icon position, v << 16 | h */
    uint16_t fd_folder;
    uint32_t created;       /* seconds since 1904 */
    uint32_t modified;
    uint32_t data_len;
    uint32_t rsrc_len;
    const uint8_t *data;    /* NULL when the container has no data fork */
    const uint8_t *rsrc;
} qrx_forkfile_t;

/* Detects the container and points the forks at their bytes. */
int32_t qrx_forkfile_open(qrx_forkfile_t *ff, const uint8_t *blob, uint32_t len,
                          uint8_t *work, uint32_t work_cap);

/* Copies fork bytes; offset + len must stay inside the fork. */
int32_t qrx_forkfile_read(const qrx_forkfile_t *ff, int resource,
                          uint32_t offset, uint8_t *dst, uint32_t len);

const char *qrx_forkfile_format_name(qrx_forkfile_format format);

#endif /* QRX_FORKFILE_H */
