#ifndef QRX_MACIMAGE_H
#define QRX_MACIMAGE_H

/*
 * Read-only Mac disk image containers.
 *
 * Recognized: raw sector images, DiskCopy 4.2, uncompressed NDIF data forks
 * (with an optional .rsrc resource fork holding the bcem header) and Apple
 * Partition Map images.  A container never decodes a filesystem; it only
 * locates the volume bytes and reports what it found.
 *
 * Everything works on a caller-owned buffer.  No file I/O, no host calls.
 */

#include <stdint.h>

enum {
    QRX_MACIMG_OK = 0,
    QRX_MACIMG_ERR_SHORT = -1,      /* fewer bytes than the structure needs */
    QRX_MACIMG_ERR_FORMAT = -2,     /* not a recognized Mac image */
    QRX_MACIMG_ERR_CHECKSUM = -3,   /* DiskCopy 4.2 data checksum mismatch */
    QRX_MACIMG_ERR_NDIF = -4,       /* NDIF chunked/compressed: unsupported */
    QRX_MACIMG_ERR_PART = -5,       /* partition map is broken */
    QRX_MACIMG_ERR_RANGE = -6,      /* a length runs past the buffer */
    QRX_MACIMG_ERR_NO_VOLUME = -7   /* container OK, no MFS/HFS volume found */
};

typedef enum {
    QRX_MACIMG_RAW = 0,
    QRX_MACIMG_DC42,
    QRX_MACIMG_NDIF,
    QRX_MACIMG_APM
} qrx_macimg_container;

typedef enum {
    QRX_MACIMG_FS_NONE = 0,
    QRX_MACIMG_FS_MFS,
    QRX_MACIMG_FS_HFS,
    QRX_MACIMG_FS_HFSPLUS
} qrx_macimg_fs;

#define QRX_MACIMG_MAX_PARTS 8

typedef struct {
    uint8_t used;
    uint32_t start_block;    /* first block of the partition */
    uint32_t block_count;    /* blocks in the partition */
    char name[33];           /* pmPartName, NUL terminated */
    char type[33];           /* pmParType, NUL terminated */
} qrx_macimg_part_t;

typedef struct {
    qrx_macimg_container container;
    const uint8_t *data;     /* buffer handed to qrx_macimg_open */
    uint32_t size;           /* bytes handed to qrx_macimg_open */

    char name[64];           /* DiskCopy/NDIF image name, may be empty */
    uint8_t checksum_ok;     /* DC42 only; 0 when the header's sum differs */
    uint32_t ndif_blocks;    /* bcem disk size in 512-byte blocks, NDIF only */

    qrx_macimg_part_t parts[QRX_MACIMG_MAX_PARTS];
    int nparts;

    uint32_t volume_offset;  /* first byte of the chosen volume */
    uint32_t volume_size;    /* bytes available from that offset */
    qrx_macimg_fs fs;        /* signature found at volume_offset + 1024 */
    int volume_part;         /* 1-based partition index, 0 = whole image */
} qrx_macimg_t;

/*
 * bytes/len: the whole image file.  rsrc/rsrc_len: the resource fork when the
 * caller has one (NDIF `.rsrc` sidecar), otherwise NULL/0.  A DC42 checksum
 * mismatch fails with QRX_MACIMG_ERR_CHECKSUM; a chunked NDIF fails with
 * QRX_MACIMG_ERR_NDIF.  A recognized container without a known filesystem
 * returns QRX_MACIMG_OK with fs == QRX_MACIMG_FS_NONE.
 */
int32_t qrx_macimg_open(qrx_macimg_t *img, const uint8_t *bytes, uint32_t len,
                        const uint8_t *rsrc, uint32_t rsrc_len);

/* Choose a 1-based partition from the Apple Partition Map. */
int32_t qrx_macimg_select_partition(qrx_macimg_t *img, int index);

const char *qrx_macimg_container_name(qrx_macimg_container container);
const char *qrx_macimg_fs_name(qrx_macimg_fs fs);
const char *qrx_macimg_strerror(int32_t status);

/* DiskCopy 4.2 checksum: for every big-endian 16-bit word, add then rotate
 * the 32-bit accumulator right by one bit.  Exposed for tests and tools. */
uint32_t qrx_macimg_dc42_checksum(const uint8_t *bytes, uint32_t len);

#endif /* QRX_MACIMAGE_H */
