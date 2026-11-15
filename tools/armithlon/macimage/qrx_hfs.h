#ifndef QRX_HFS_H
#define QRX_HFS_H

/*
 * Read-only Hierarchical File System (HFS, 1985) reader.
 *
 * Parses the master directory block, then the catalog and extents B-trees.
 * Files carry their Finder information (type, creator, flags, icon id) and
 * both forks; the extents overflow tree is consulted when a fork needs more
 * than the three extents a catalog record holds.  Nothing is written.
 *
 * Node buffers are shared scratch space: qrx_hfs_t is ~12 KiB and belongs on
 * the heap, not on a 68k stack.
 */

#include "qrx_macimage.h"

#define QRX_HFS_MAX_ENTRIES 64
#define QRX_HFS_MAX_EXTENTS 12
#define QRX_HFS_NODE_MAX 4096

typedef struct {
    uint16_t start;
    uint16_t count;
} qrx_hfs_extent_t;

typedef struct {
    uint8_t is_dir;
    char name[64];          /* MacRoman bytes, high bytes replaced by '.' */
    uint8_t name_len;
    uint32_t parent_id;     /* CNID of the parent folder, 1 = root parent */
    uint32_t cnid;          /* file or folder ID */

    /* files */
    uint32_t type_code;
    uint32_t creator;
    uint16_t fd_flags;      /* Finder flag word */
    uint32_t fd_location;   /* Finder icon position, v<<16 | h */
    uint16_t fd_folder;
    uint16_t icon_id;       /* FXInfo frIconID */
    uint32_t data_len;
    uint32_t rsrc_len;
    uint32_t created;
    uint32_t modified;
    uint8_t data_ext[12];   /* the three extents from the catalog record */
    uint8_t rsrc_ext[12];
} qrx_hfs_entry_t;

typedef struct {
    const uint8_t *vol;
    uint32_t size;

    char name[28];
    uint8_t name_len;
    uint16_t attributes;
    uint16_t alloc_blocks;
    uint32_t alloc_size;
    uint16_t alloc_start;
    uint16_t free_blocks;
    uint32_t file_count;
    uint32_t dir_count;

    uint32_t cat_size;
    uint32_t ext_size;
    uint16_t cat_node_size;
    uint16_t ext_node_size;

    uint8_t xt_ext[12];                    /* the MDB's extents tree extents */
    uint8_t ct_ext[12];
    qrx_hfs_extent_t xt_list[QRX_HFS_MAX_EXTENTS];
    int xt_count;
    qrx_hfs_extent_t cat_list[QRX_HFS_MAX_EXTENTS];
    int cat_count;

    uint16_t nentries;
    uint8_t truncated;      /* catalog held more entries than fit */
    qrx_hfs_entry_t entries[QRX_HFS_MAX_ENTRIES];

    uint8_t node[QRX_HFS_NODE_MAX];   /* scratch for one B-tree node */
} qrx_hfs_t;

/* Validates the MDB and the catalog tree, then reads the whole catalog. */
int32_t qrx_hfs_open(qrx_hfs_t *hfs, const uint8_t *volume, uint32_t len);

/* Direct children of `parent` (1 = the root folder's parent). */
const qrx_hfs_entry_t *qrx_hfs_lookup(const qrx_hfs_t *hfs, uint32_t parent,
                                      const char *name);

/* "Folder:File" relative to the root; a leading ':' is ignored. */
const qrx_hfs_entry_t *qrx_hfs_lookup_path(const qrx_hfs_t *hfs,
                                           const char *path);

/* Copies one fork.  A cap smaller than the fork fails with ERR_RANGE. */
int32_t qrx_hfs_read_fork(const qrx_hfs_t *hfs, const qrx_hfs_entry_t *entry,
                          int resource, uint8_t *dst, uint32_t cap,
                          uint32_t *written);

#endif /* QRX_HFS_H */
