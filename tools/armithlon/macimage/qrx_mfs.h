#ifndef QRX_MFS_H
#define QRX_MFS_H

/*
 * Read-only Macintosh File System (MFS) reader.
 *
 * MFS is the flat 400K/800K filesystem of the original Macintosh: a 64-byte
 * MDB header at byte 1024, a 12-bit allocation block map behind it, a
 * contiguous directory area and one flat list of files.  Nothing is written.
 */

#include "qrx_macimage.h"

#define QRX_MFS_MAX_FILES 64

typedef struct {
    char name[64];            /* MacRoman bytes, high bytes replaced by '.' */
    uint8_t name_len;
    uint8_t flags;            /* bit 7 in use, bit 0 locked */
    uint32_t type_code;
    uint32_t creator;
    uint16_t finder_flags;
    uint32_t file_number;
    uint16_t start_block;     /* data fork, 0 = empty */
    uint16_t rsrc_block;      /* resource fork, 0 = empty */
    uint32_t logical_len;     /* data fork bytes */
    uint32_t physical_len;
    uint32_t rsrc_len;
    uint32_t rsrc_physical_len;
    uint32_t created;         /* seconds since 1904-01-01, local time */
    uint32_t modified;
} qrx_mfs_file_t;

typedef struct {
    const uint8_t *vol;
    uint32_t size;

    char name[28];
    uint8_t name_len;
    uint16_t attributes;
    uint16_t root_files;      /* drNmFls */
    uint16_t dir_start;       /* drDirSt, in 512-byte sectors */
    uint16_t dir_len;         /* drDirLen, in sectors */
    uint16_t alloc_blocks;    /* drNmAlBlks, blocks are numbered from 2 */
    uint32_t alloc_size;      /* drAlBlkSiz, bytes */
    uint16_t alloc_start;     /* drAlBlSt, in sectors */
    uint16_t free_blocks;     /* drFreeBks */
    uint32_t next_file_number;

    const uint8_t *blockmap;  /* 12-bit entries, entry 0 belongs to block 2 */
    uint32_t blockmap_len;

    uint16_t nfiles;
    qrx_mfs_file_t files[QRX_MFS_MAX_FILES];
} qrx_mfs_t;

/* Validates the MDB, the directory area and the allocation area. */
int32_t qrx_mfs_open(qrx_mfs_t *mfs, const uint8_t *volume, uint32_t len);

/*
 * Copy one fork into dst.  `resource` selects the resource fork.  The buffer
 * must hold the whole fork; a smaller cap fails with
 * QRX_MACIMG_ERR_RANGE and leaves dst untouched.
 */
int32_t qrx_mfs_read_fork(const qrx_mfs_t *mfs, const qrx_mfs_file_t *file,
                          int resource, uint8_t *dst, uint32_t cap,
                          uint32_t *written);

/* The next allocation block in a chain, 0 when the volume image is broken. */
uint16_t qrx_mfs_blockmap_next(const qrx_mfs_t *mfs, uint16_t block);

#endif /* QRX_MFS_H */
