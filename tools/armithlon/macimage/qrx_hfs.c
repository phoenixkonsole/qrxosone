#include "qrx_hfs.h"

#include <string.h>

#define HFS_SIG 0x4244u
#define HFS_MDB_OFFSET 1024u
#define HFS_SECTOR 512u
#define HFS_FORK_DATA 0x00u
#define HFS_FORK_RSRC 0xFFu
#define HFS_CATALOG_ID 4u
#define HFS_ROOT_PARENT 1u
#define HFS_MIN_NODE_SIZE 512u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void copy_name(char *dst, const uint8_t *src, uint8_t len)
{
    uint8_t i;

    if (len > 63u)
        len = 63u;
    for (i = 0; i < len; i++) {
        uint8_t c = src[i];
        dst[i] = (c >= 0x20u && c < 0x7Fu) ? (char)c : '.';
    }
    dst[len] = '\0';
}

static int name_equals(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ------------------------------------------------------------------ extents */

static int extent_list(const uint8_t *ext12, qrx_hfs_extent_t *out, int max)
{
    int i;
    int n = 0;

    for (i = 0; i < 3; i++) {
        uint16_t start = rd16(ext12 + (uint32_t)i * 4u);
        uint16_t count = rd16(ext12 + (uint32_t)i * 4u + 2u);

        if (count == 0u)
            break;
        if (n < max) {
            out[n].start = start;
            out[n].count = count;
            n++;
        }
    }
    return n;
}

static uint32_t extents_bytes(const qrx_hfs_t *h, const qrx_hfs_extent_t *list,
                              int n)
{
    uint32_t blocks = 0;
    int i;

    for (i = 0; i < n; i++)
        blocks += list[i].count;
    return blocks * h->alloc_size;
}

static int32_t copy_extents(const qrx_hfs_t *h, const qrx_hfs_extent_t *list,
                            int n, uint32_t offset, uint8_t *dst, uint32_t len)
{
    uint32_t covered = 0;
    int i;

    for (i = 0; i < n && len > 0u; i++) {
        uint32_t extent_bytes = (uint32_t)list[i].count * h->alloc_size;
        uint32_t skip;
        uint32_t avail;
        uint32_t take;
        uint64_t address;

        if (offset >= covered + extent_bytes) {
            covered += extent_bytes;
            continue;
        }
        skip = (offset > covered) ? offset - covered : 0u;
        avail = extent_bytes - skip;
        take = (len < avail) ? len : avail;
        address = (uint64_t)h->alloc_start * HFS_SECTOR +
                  (uint64_t)list[i].start * h->alloc_size + skip;
        if (address + take > (uint64_t)h->size)
            return QRX_MACIMG_ERR_RANGE;
        memcpy(dst, h->vol + address, take);
        dst += take;
        len -= take;
        offset += take;
        covered += extent_bytes;
    }
    return len == 0u ? QRX_MACIMG_OK : QRX_MACIMG_ERR_RANGE;
}

typedef struct {
    uint32_t root;
    uint32_t first_leaf;
    uint32_t last_leaf;
    uint32_t leaf_records;
    uint16_t node_size;
    uint16_t depth;
} bt_header_t;

static int32_t read_node(qrx_hfs_t *h, const qrx_hfs_extent_t *list, int n,
                         uint32_t node_size, uint32_t file_size,
                         uint32_t index)
{
    uint32_t offset;

    if (node_size < HFS_MIN_NODE_SIZE || node_size > QRX_HFS_NODE_MAX)
        return QRX_MACIMG_ERR_FORMAT;
    offset = index * node_size;
    if (offset > file_size || node_size > file_size - offset)
        return QRX_MACIMG_ERR_FORMAT;
    return copy_extents(h, list, n, offset, h->node, node_size);
}

static int32_t bt_header(qrx_hfs_t *h, const qrx_hfs_extent_t *list, int n,
                         uint32_t file_size, bt_header_t *out)
{
    const uint8_t *rec;
    uint16_t node_size;
    int32_t status;

    /* The header node starts at offset 0; the first 512 bytes carry the node
     * descriptor and the 106-byte header record, enough to learn nodeSize. */
    status = copy_extents(h, list, n, 0u, h->node, HFS_MIN_NODE_SIZE);
    if (status != QRX_MACIMG_OK)
        return status;
    if (h->node[8] != 1)                /* kind: header node */
        return QRX_MACIMG_ERR_FORMAT;
    rec = h->node + 14u;
    node_size = rd16(rec + 18u);
    if (node_size < HFS_MIN_NODE_SIZE || node_size > QRX_HFS_NODE_MAX)
        return QRX_MACIMG_ERR_FORMAT;
    if (node_size > file_size)
        return QRX_MACIMG_ERR_FORMAT;
    out->depth = rd16(rec);
    out->root = rd32(rec + 2u);
    out->leaf_records = rd32(rec + 6u);
    out->first_leaf = rd32(rec + 10u);
    out->last_leaf = rd32(rec + 14u);
    out->node_size = node_size;
    return QRX_MACIMG_OK;
}

static int32_t collect_overflow(qrx_hfs_t *h, uint32_t file_id, int resource,
                                qrx_hfs_extent_t *list, int *count, int max)
{
    bt_header_t header;
    uint32_t node_index;
    int guard = 0;
    int32_t status;

    if (h->xt_count <= 0)
        return QRX_MACIMG_ERR_FORMAT;
    status = bt_header(h, h->xt_list, h->xt_count, h->ext_size, &header);
    if (status != QRX_MACIMG_OK)
        return status;
    node_index = header.first_leaf;
    while (node_index != 0u && guard++ < 4096) {
        uint16_t records;
        uint16_t i;

        status = read_node(h, h->xt_list, h->xt_count, header.node_size,
                           h->ext_size, node_index);
        if (status != QRX_MACIMG_OK)
            return status;
        records = rd16(h->node + 10u);
        for (i = 0; i < records; i++) {
            uint16_t nrec = records;
            uint16_t start = rd16(h->node + header.node_size -
                                  2u * (uint16_t)(nrec - i + 1));
            uint16_t end = rd16(h->node + header.node_size -
                                2u * (uint16_t)(nrec - i));
            const uint8_t *key;
            const uint8_t *rec;
            uint8_t fork_type;
            uint32_t fid;
            int n;

            if (end > header.node_size || start >= end || start < 14u)
                continue;
            key = h->node + start;
            if ((uint32_t)key[0] + 1u + 12u > (uint32_t)(end - start))
                continue;
            fork_type = key[2];
            fid = rd32(key + 4u);
            if (fid != file_id)
                continue;
            if (fork_type != (resource ? HFS_FORK_RSRC : HFS_FORK_DATA))
                continue;
            rec = key + ((key[0] + 1u + 1u) & ~1u);
            n = extent_list(rec, list + *count, max - *count);
            *count += n;
            if (*count >= max)
                return QRX_MACIMG_OK;
        }
        node_index = rd32(h->node);
    }
    return QRX_MACIMG_OK;
}

/* ------------------------------------------------------- catalog parsing */

static void parse_record(qrx_hfs_t *h, uint16_t index, uint16_t node_size,
                         uint16_t records)
{
    uint16_t start;
    uint16_t end;
    const uint8_t *key;
    const uint8_t *rec;
    uint32_t key_len;
    uint32_t parent;
    uint8_t name_len;
    uint16_t type;
    qrx_hfs_entry_t *entry;

    if (index >= records)
        return;
    start = rd16(h->node + node_size - 2u * (uint16_t)(records - index + 1));
    end = rd16(h->node + node_size - 2u * (uint16_t)(records - index));
    if (end > node_size || start >= end || start < 14u)
        return;
    if ((uint32_t)(end - start) < 8u)
        return;
    key = h->node + start;
    key_len = (uint32_t)key[0] + 1u;
    if (key_len & 1u)
        key_len++;                      /* keys keep records word aligned */
    if (key_len + 7u > (uint32_t)(end - start))
        return;
    parent = rd32(key + 2u);
    name_len = key[6];
    if (name_len > 31u)
        return;
    rec = key + key_len;
    type = rd16(rec);

    if (type != 1u && type != 2u)
        return;                         /* folder/file threads carry no entry */
    if (h->nentries >= (uint16_t)QRX_HFS_MAX_ENTRIES) {
        h->truncated = 1u;
        return;
    }
    entry = &h->entries[h->nentries];
    memset(entry, 0, sizeof(*entry));
    entry->parent_id = parent;
    entry->name_len = name_len;
    copy_name(entry->name, key + 7u, name_len);

    if (type == 1u) {
        if ((uint32_t)(end - start) - key_len < 70u)
            return;
        entry->is_dir = 1u;
        entry->cnid = rd32(rec + 6u);
        entry->icon_id = rd16(rec + 38u);
        entry->created = rd32(rec + 10u);
        entry->modified = rd32(rec + 14u);
    } else {
        if ((uint32_t)(end - start) - key_len < 98u)
            return;
        entry->cnid = rd32(rec + 20u);
        entry->type_code = rd32(rec + 4u);
        entry->creator = rd32(rec + 8u);
        entry->fd_flags = rd16(rec + 12u);
        entry->fd_location = rd32(rec + 14u);
        entry->fd_folder = rd16(rec + 18u);
        entry->data_len = rd32(rec + 26u);
        entry->rsrc_len = rd32(rec + 36u);
        entry->created = rd32(rec + 44u);
        entry->modified = rd32(rec + 48u);
        entry->icon_id = rd16(rec + 56u);
        memcpy(entry->data_ext, rec + 74u, 12u);
        memcpy(entry->rsrc_ext, rec + 86u, 12u);
    }
    h->nentries++;
}

static int32_t catalog_scan(qrx_hfs_t *h)
{
    bt_header_t header;
    uint32_t node_index;
    int guard = 0;
    int32_t status;

    status = bt_header(h, h->cat_list, h->cat_count, h->cat_size, &header);
    if (status != QRX_MACIMG_OK)
        return status;
    h->cat_node_size = header.node_size;
    if (header.first_leaf == 0u)
        return 0;                       /* empty catalog tree */
    node_index = header.first_leaf;
    while (node_index != 0u && guard++ < 4096) {
        uint16_t records;
        uint16_t i;

        status = read_node(h, h->cat_list, h->cat_count, header.node_size,
                           h->cat_size, node_index);
        if (status != QRX_MACIMG_OK)
            return status;
        if ((uint8_t)h->node[8] != 0xFFu)   /* leaf nodes are kind -1 */
            return QRX_MACIMG_ERR_FORMAT;
        records = rd16(h->node + 10u);
        for (i = 0; i < records; i++)
            parse_record(h, i, header.node_size, records);
        node_index = rd32(h->node);
    }
    return QRX_MACIMG_OK;
}

/* ------------------------------------------------------------------- open */

static int32_t build_catalog_extents(qrx_hfs_t *h)
{
    uint32_t covered;

    h->cat_count = extent_list(h->ct_ext, h->cat_list, QRX_HFS_MAX_EXTENTS);
    if (h->cat_count == 0)
        return QRX_MACIMG_ERR_FORMAT;
    covered = extents_bytes(h, h->cat_list, h->cat_count);
    if (covered < h->cat_size) {
        int32_t status = collect_overflow(h, HFS_CATALOG_ID, 0, h->cat_list,
                                          &h->cat_count, QRX_HFS_MAX_EXTENTS);
        if (status != QRX_MACIMG_OK)
            return status;
        if (extents_bytes(h, h->cat_list, h->cat_count) < h->cat_size)
            return QRX_MACIMG_ERR_FORMAT;
    }
    return QRX_MACIMG_OK;
}

int32_t qrx_hfs_open(qrx_hfs_t *hfs, const uint8_t *volume, uint32_t len)
{
    const uint8_t *mdb;
    uint64_t alloc_end;
    int32_t status;

    if (!hfs)
        return QRX_MACIMG_ERR_SHORT;
    memset(hfs, 0, sizeof(*hfs));
    if (!volume || len < HFS_MDB_OFFSET + 162u)
        return QRX_MACIMG_ERR_SHORT;
    mdb = volume + HFS_MDB_OFFSET;
    if (rd16(mdb) != HFS_SIG)
        return QRX_MACIMG_ERR_FORMAT;

    hfs->vol = volume;
    hfs->size = len;
    hfs->attributes = rd16(mdb + 10u);
    hfs->alloc_blocks = rd16(mdb + 18u);
    hfs->alloc_size = rd32(mdb + 20u);
    hfs->alloc_start = rd16(mdb + 28u);
    hfs->free_blocks = rd16(mdb + 34u);
    hfs->file_count = rd32(mdb + 84u);
    hfs->dir_count = rd32(mdb + 88u);
    hfs->ext_size = rd32(mdb + 130u);
    memcpy(hfs->xt_ext, mdb + 134u, 12u);
    hfs->cat_size = rd32(mdb + 146u);
    memcpy(hfs->ct_ext, mdb + 150u, 12u);

    hfs->name_len = mdb[36];
    if (hfs->name_len > 27u)
        return QRX_MACIMG_ERR_FORMAT;
    memcpy(hfs->name, mdb + 37u, hfs->name_len);
    hfs->name[hfs->name_len] = '\0';

    if (hfs->alloc_blocks == 0u || hfs->alloc_start < 3u)
        return QRX_MACIMG_ERR_FORMAT;
    if (hfs->alloc_size < HFS_SECTOR || (hfs->alloc_size % HFS_SECTOR) != 0u)
        return QRX_MACIMG_ERR_FORMAT;
    alloc_end = (uint64_t)hfs->alloc_start * HFS_SECTOR +
                (uint64_t)hfs->alloc_blocks * hfs->alloc_size;
    if (alloc_end > (uint64_t)len)
        return QRX_MACIMG_ERR_RANGE;
    if (hfs->cat_size == 0u || hfs->ext_size == 0u)
        return QRX_MACIMG_ERR_FORMAT;

    hfs->xt_count = extent_list(hfs->xt_ext, hfs->xt_list, QRX_HFS_MAX_EXTENTS);
    if (hfs->xt_count == 0)
        return QRX_MACIMG_ERR_FORMAT;
    {
        bt_header_t header;
        status = bt_header(hfs, hfs->xt_list, hfs->xt_count, hfs->ext_size,
                           &header);
        if (status != QRX_MACIMG_OK)
            return status;
        hfs->ext_node_size = header.node_size;
    }

    status = build_catalog_extents(hfs);
    if (status != QRX_MACIMG_OK)
        return status;
    return catalog_scan(hfs);
}

/* ---------------------------------------------------------------- lookup */

const qrx_hfs_entry_t *qrx_hfs_lookup(const qrx_hfs_t *hfs, uint32_t parent,
                                      const char *name)
{
    uint16_t i;

    if (!hfs || !name)
        return NULL;
    for (i = 0; i < hfs->nentries; i++) {
        const qrx_hfs_entry_t *entry = &hfs->entries[i];
        if (entry->parent_id == parent && name_equals(entry->name, name))
            return entry;
    }
    return NULL;
}

const qrx_hfs_entry_t *qrx_hfs_lookup_path(const qrx_hfs_t *hfs,
                                           const char *path)
{
    uint32_t parent = HFS_ROOT_PARENT;
    const qrx_hfs_entry_t *entry = NULL;
    char component[64];
    const char *p = path;

    if (!hfs || !path)
        return NULL;
    if (*p == ':')
        p++;
    while (*p) {
        size_t n = 0;

        while (*p && *p != ':' && n + 1u < sizeof(component))
            component[n++] = *p++;
        component[n] = '\0';
        if (*p == ':')
            p++;
        entry = qrx_hfs_lookup(hfs, parent, component);
        if (entry == NULL)
            return NULL;
        if (*p && !entry->is_dir)
            return NULL;
        parent = entry->cnid;
    }
    return entry;
}

/* ------------------------------------------------------------------ forks */

int32_t qrx_hfs_read_fork(const qrx_hfs_t *hfs, const qrx_hfs_entry_t *entry,
                          int resource, uint8_t *dst, uint32_t cap,
                          uint32_t *written)
{
    qrx_hfs_t *h = (qrx_hfs_t *)hfs;    /* node[] is scratch space */
    qrx_hfs_extent_t list[QRX_HFS_MAX_EXTENTS];
    int count;
    uint32_t need;
    int32_t status;

    if (!hfs || !entry || !dst)
        return QRX_MACIMG_ERR_RANGE;
    need = resource ? entry->rsrc_len : entry->data_len;
    if (need > cap)
        return QRX_MACIMG_ERR_RANGE;
    if (need == 0u) {
        if (written)
            *written = 0u;
        return QRX_MACIMG_OK;
    }
    count = extent_list(resource ? entry->rsrc_ext : entry->data_ext, list,
                        QRX_HFS_MAX_EXTENTS);
    if (extents_bytes(h, list, count) < need) {
        status = collect_overflow(h, entry->cnid, resource, list, &count,
                                  QRX_HFS_MAX_EXTENTS);
        if (status != QRX_MACIMG_OK)
            return status;
    }
    if (extents_bytes(h, list, count) < need)
        return QRX_MACIMG_ERR_FORMAT;
    status = copy_extents(h, list, count, 0u, dst, need);
    if (status != QRX_MACIMG_OK)
        return status;
    if (written)
        *written = need;
    return QRX_MACIMG_OK;
}
