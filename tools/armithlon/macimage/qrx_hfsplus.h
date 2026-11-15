#ifndef QRX_HFSPLUS_H
#define QRX_HFSPLUS_H

/*
 * Read-only HFS+ / HFSX reader.
 *
 * Volume header at 1024, catalog and extents B-trees with 16-bit key lengths
 * and UTF-16BE node names, the extents overflow tree for forks with more than
 * eight extents, and the attributes B-tree as a fallback for resource forks
 * stored as com.apple.ResourceFork (fork-data records only).
 *
 * An HFS wrapper (Mac OS 8.1-9) is resolved transparently: when the volume
 * carries an HFS master directory block whose drEmbedSigWord names HFS+/HFSX,
 * the embedded volume is opened instead of the wrapper, so one image stays
 * one volume.  HFSX is detected by its signature and compares names
 * case-sensitively; HFS+ folds ASCII case.  A non-zero journalInfoBlock is
 * reported (journal != 0) but never replayed - this reader only reads.
 *
 * node[] is scratch for one B-tree node; qrx_hfsp_t is ~20 KiB and belongs on
 * the heap or in static storage, not on a 68k stack.
 */

#include "qrx_macimage.h"

#define QRX_HFSP_MAX_ENTRIES 64
#define QRX_HFSP_MAX_EXTENTS 64
#define QRX_HFSP_NODE_MAX 8192

typedef struct {
    uint32_t start;
    uint32_t count;
} qrx_hfsp_extent_t;

typedef struct {
    uint8_t is_dir;
    char name[64];          /* UTF-16 units below 0x100 as one byte, else '?' */
    uint8_t name_len;
    uint32_t parent_id;     /* CNID of the parent folder, 2 = root folder */
    uint32_t cnid;

    uint16_t flags;         /* catalog record flags (locked, invisible, ...) */
    uint32_t type_code;
    uint32_t creator;
    uint16_t fd_flags;      /* Finder flag word */
    uint16_t fd_folder;
    uint32_t icon_id;       /* HFS+ stores no icon ID; always 0 */
    uint32_t data_len;
    uint32_t rsrc_len;
    uint32_t created;
    uint32_t modified;

    qrx_hfsp_extent_t data_ext[8];
    qrx_hfsp_extent_t rsrc_ext[8];
    int data_count;
    int rsrc_count;
    uint8_t rsrc_via_attr;  /* resource fork came from com.apple.ResourceFork */
} qrx_hfsp_entry_t;

typedef struct {
    const uint8_t *vol;
    uint32_t size;

    uint32_t offset;        /* start of the HFS+ volume inside vol */
    uint32_t volume_size;
    uint8_t wrapped;        /* opened through an HFS wrapper */
    uint8_t case_sensitive; /* HFSX */
    uint8_t journaled;
    uint16_t signature;

    char name[64];          /* volume name from the root folder thread */
    uint8_t name_len;

    uint32_t attributes;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t file_count;
    uint32_t folder_count;

    uint32_t cat_size;
    uint32_t ext_size;
    uint32_t attr_size;
    uint16_t cat_node_size;
    uint16_t ext_node_size;
    uint16_t attr_node_size;

    qrx_hfsp_extent_t cat_list[QRX_HFSP_MAX_EXTENTS];
    int cat_count;
    qrx_hfsp_extent_t ext_list[QRX_HFSP_MAX_EXTENTS];
    int ext_count;
    qrx_hfsp_extent_t attr_list[QRX_HFSP_MAX_EXTENTS];
    int attr_count;

    uint16_t nentries;
    uint8_t truncated;
    qrx_hfsp_entry_t entries[QRX_HFSP_MAX_ENTRIES];

    uint8_t node[QRX_HFSP_NODE_MAX];
} qrx_hfsp_t;

/* Opens HFS+, HFSX or an HFS wrapper around either. Reads the whole catalog. */
int32_t qrx_hfsp_open(qrx_hfsp_t *fs, const uint8_t *volume, uint32_t len);

/* Direct children of `parent` (2 = the root folder). */
const qrx_hfsp_entry_t *qrx_hfsp_lookup(const qrx_hfsp_t *fs, uint32_t parent,
                                        const char *name);

/* "Folder:File" relative to the root; a leading ':' is ignored. */
const qrx_hfsp_entry_t *qrx_hfsp_lookup_path(const qrx_hfsp_t *fs,
                                             const char *path);

/* Copies one fork.  A cap smaller than the fork fails with ERR_RANGE. */
int32_t qrx_hfsp_read_fork(const qrx_hfsp_t *fs, const qrx_hfsp_entry_t *entry,
                           int resource, uint8_t *dst, uint32_t cap,
                           uint32_t *written);

#endif /* QRX_HFSPLUS_H */
