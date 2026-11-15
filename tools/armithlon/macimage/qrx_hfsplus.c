#include "qrx_hfsplus.h"

#include <string.h>

#define HFSP_SIG_HFSP 0x482Bu
#define HFSP_SIG_HFSX 0x4858u
#define HFSP_HFS_SIG 0x4244u
#define HFSP_VH_OFFSET 1024u
#define HFSP_VH_SIZE 512u
#define HFSP_SECTOR 512u
#define HFSP_ROOT_FOLDER 2u
#define HFSP_FORK_DATA 0x00u
#define HFSP_FORK_RSRC 0xFFu
#define HFSP_ATTR_INLINE 0x10u
#define HFSP_ATTR_FORK 0x20u
#define HFSP_MIN_NODE_SIZE 512u
#define HFSP_FORK_EXTENTS 16u
#define HFSP_MAX_INLINE_EXTENTS 8

static const char resource_fork_name[] = "com.apple.ResourceFork";

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)rd32(p) << 32) | (uint64_t)rd32(p + 4u);
}

/* UTF-16 units below 0x100 become one byte (Latin-1), everything else '?'. */
static void hfsp_name(char *dst, const uint8_t *units, uint16_t count,
                      uint8_t *len)
{
    uint16_t i;
    uint8_t n = 0;

    for (i = 0; i < count && n < 63u; i++) {
        uint16_t c = rd16(units + (uint32_t)i * 2u);

        if (c < 0x80u)
            dst[n++] = (char)c;
        else if (c < 0x100u)
            dst[n++] = (char)(c & 0xFFu);
        else
            dst[n++] = '?';
    }
    dst[n] = '\0';
    if (len)
        *len = n;
}

static uint32_t hfsp_logical(const uint8_t *fork_data)
{
    uint64_t value = rd64(fork_data);

    return value > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)value;
}

/* Reads up to eight extent descriptors that start at `extents`. */
static int hfsp_extents_at(const uint8_t *extents, qrx_hfsp_extent_t *out,
                           int max)
{
    int i;
    int n = 0;

    for (i = 0; i < HFSP_MAX_INLINE_EXTENTS; i++) {
        uint32_t start = rd32(extents + (uint32_t)i * 8u);
        uint32_t blocks = rd32(extents + (uint32_t)i * 8u + 4u);

        if (blocks == 0u)
            break;
        if (n < max) {
            out[n].start = start;
            out[n].count = blocks;
            n++;
        }
    }
    return n;
}

static int hfsp_extents(const uint8_t *fork_data, qrx_hfsp_extent_t *out,
                        int max)
{
    return hfsp_extents_at(fork_data + HFSP_FORK_EXTENTS, out, max);
}

static uint32_t extent_bytes(const qrx_hfsp_t *fs, const qrx_hfsp_extent_t *list,
                             int count)
{
    uint64_t blocks = 0;
    int i;

    for (i = 0; i < count; i++)
        blocks += list[i].count;
    return blocks > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)blocks * fs->block_size;
}

static int32_t copy_extents(const qrx_hfsp_t *fs,
                            const qrx_hfsp_extent_t *list, int count,
                            uint32_t offset, uint8_t *dst, uint32_t len)
{
    uint32_t covered = 0;
    int i;

    for (i = 0; i < count && len > 0u; i++) {
        uint32_t extent_len = (uint32_t)list[i].count * fs->block_size;
        uint32_t skip;
        uint32_t take;
        uint64_t address;

        if (offset >= covered + extent_len) {
            covered += extent_len;
            continue;
        }
        skip = (offset > covered) ? offset - covered : 0u;
        take = len;
        if (take > extent_len - skip)
            take = extent_len - skip;
        address = (uint64_t)fs->offset + (uint64_t)list[i].start * fs->block_size +
                  skip;
        if (address + take > (uint64_t)fs->size)
            return QRX_MACIMG_ERR_RANGE;
        memcpy(dst, fs->vol + address, take);
        dst += take;
        len -= take;
        offset += take;
        covered += extent_len;
    }
    return len == 0u ? QRX_MACIMG_OK : QRX_MACIMG_ERR_RANGE;
}

typedef struct {
    uint32_t first_leaf;
    uint32_t last_leaf;
    uint32_t leaf_records;
    uint16_t node_size;
    uint8_t depth;
} bt_header_t;

static int32_t read_node(qrx_hfsp_t *fs, const qrx_hfsp_extent_t *list, int count,
                         uint32_t node_size, uint32_t fork_size,
                         uint32_t index)
{
    uint64_t offset;

    if (node_size < HFSP_MIN_NODE_SIZE || node_size > QRX_HFSP_NODE_MAX)
        return QRX_MACIMG_ERR_FORMAT;
    offset = (uint64_t)index * node_size;
    if (offset + node_size > fork_size)
        return QRX_MACIMG_ERR_FORMAT;
    return copy_extents(fs, list, count, (uint32_t)offset, fs->node, node_size);
}

static int32_t bt_header(qrx_hfsp_t *fs, const qrx_hfsp_extent_t *list,
                         int count, uint32_t fork_size, bt_header_t *out)
{
    const uint8_t *rec;
    uint16_t node_size;
    int32_t status;

    status = copy_extents(fs, list, count, 0u, fs->node, 512u);
    if (status != QRX_MACIMG_OK)
        return status;
    if (fs->node[8] != 1)               /* header node */
        return QRX_MACIMG_ERR_FORMAT;
    rec = fs->node + 14u;
    node_size = rd16(rec + 18u);
    if (node_size < HFSP_MIN_NODE_SIZE || node_size > QRX_HFSP_NODE_MAX)
        return QRX_MACIMG_ERR_FORMAT;
    if (node_size > fork_size)
        return QRX_MACIMG_ERR_FORMAT;
    out->depth = (uint8_t)rd16(rec);
    out->first_leaf = rd32(rec + 10u);
    out->last_leaf = rd32(rec + 14u);
    out->leaf_records = rd32(rec + 6u);
    out->node_size = node_size;
    return QRX_MACIMG_OK;
}

/* ------------------------------------------------------- extents overflow */

static int32_t collect_overflow(qrx_hfsp_t *fs, uint32_t file_id, int resource,
                                qrx_hfsp_extent_t *list, int *count, int max)
{
    bt_header_t header;
    uint32_t node_index;
    int guard = 0;
    int32_t status;

    if (fs->ext_count <= 0)
        return QRX_MACIMG_ERR_FORMAT;
    status = bt_header(fs, fs->ext_list, fs->ext_count, fs->ext_size, &header);
    if (status != QRX_MACIMG_OK)
        return status;
    node_index = header.first_leaf;
    while (node_index != 0u && guard++ < 4096) {
        uint16_t records;
        uint16_t i;

        status = read_node(fs, fs->ext_list, fs->ext_count, header.node_size,
                           fs->ext_size, node_index);
        if (status != QRX_MACIMG_OK)
            return status;
        records = rd16(fs->node + 10u);
        for (i = 0; i < records; i++) {
            uint16_t start = rd16(fs->node + header.node_size -
                                  2u * (uint16_t)(records - i + 1u));
            uint16_t end = rd16(fs->node + header.node_size -
                                2u * (uint16_t)(records - i));
            const uint8_t *key;
            const uint8_t *rec;
            int n;

            if (end > header.node_size || start >= end || start < 14u)
                continue;
            key = fs->node + start;
            if ((uint32_t)key[0] + 2u + 64u > (uint32_t)(end - start))
                continue;
            if (key[2] != (resource ? HFSP_FORK_RSRC : HFSP_FORK_DATA))
                continue;
            if (rd32(key + 4u) != file_id)
                continue;
            rec = key + 2u + (uint32_t)key[0] + (uint32_t)key[1];
            n = hfsp_extents_at(rec, list + *count, max - *count);
            *count += n;
            if (*count >= max)
                return QRX_MACIMG_OK;
        }
        node_index = rd32(fs->node);
    }
    return QRX_MACIMG_OK;
}

/* ------------------------------------------------------------- attributes */

static int attr_name_matches(const uint8_t *units, uint16_t count)
{
    size_t i;
    size_t len = strlen(resource_fork_name);

    if (count != len)
        return 0;
    for (i = 0; i < len; i++) {
        if (rd16(units + i * 2u) != (uint16_t)(uint8_t)resource_fork_name[i])
            return 0;
    }
    return 1;
}

/*
 * Looks up com.apple.ResourceFork for `file_id`.  Returns 1 when a fork-data
 * attribute was found and copies its extents and logical size, 0 otherwise.
 */
static int resource_attribute(qrx_hfsp_t *fs, uint32_t file_id,
                              qrx_hfsp_extent_t *out, int *count,
                              uint32_t *length)
{
    bt_header_t header;
    uint32_t node_index;
    int guard = 0;

    if (fs->attr_count <= 0 || fs->attr_size == 0u)
        return 0;
    if (bt_header(fs, fs->attr_list, fs->attr_count, fs->attr_size, &header) !=
        QRX_MACIMG_OK)
        return 0;
    node_index = header.first_leaf;
    while (node_index != 0u && guard++ < 4096) {
        uint16_t records;
        uint16_t i;

        if (read_node(fs, fs->attr_list, fs->attr_count, header.node_size,
                      fs->attr_size, node_index) != QRX_MACIMG_OK)
            return 0;
        records = rd16(fs->node + 10u);
        for (i = 0; i < records; i++) {
            uint16_t start = rd16(fs->node + header.node_size -
                                  2u * (uint16_t)(records - i + 1u));
            uint16_t end = rd16(fs->node + header.node_size -
                                2u * (uint16_t)(records - i));
            const uint8_t *key;
            const uint8_t *rec;
            uint16_t key_len;
            uint16_t units;

            if (end > header.node_size || start >= end || start < 14u)
                continue;
            key = fs->node + start;
            if ((uint32_t)(end - start) < 14u)
                continue;
            key_len = (uint16_t)(rd16(key) + 2u);
            if (key_len + 8u > (uint32_t)(end - start))
                continue;
            if (rd32(key + 4u) != file_id)
                continue;
            units = rd16(key + 12u);
            if (!attr_name_matches(key + 14u, units))
                continue;
            rec = key + key_len;
            if (rd32(rec) != HFSP_ATTR_FORK)
                continue;               /* inline attributes are not forks */
            *length = rd32(rec + 8u);
            *count = hfsp_extents(rec, out, 8);
            return *count > 0 ? 1 : 0;
        }
        node_index = rd32(fs->node);
    }
    return 0;
}

/* ------------------------------------------------------------------ names */

static int name_equal(const qrx_hfsp_t *fs, const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a;
        char cb = *b;

        if (!fs->case_sensitive) {
            if (ca >= 'A' && ca <= 'Z')
                ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z')
                cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ------------------------------------------------------- catalog parsing */

static void parse_record(qrx_hfsp_t *fs, const uint8_t *key,
                         const uint8_t *rec, uint16_t rec_len)
{
    uint16_t type = rd16(rec);
    uint32_t parent = rd32(key + 2u);
    uint16_t units = rd16(key + 6u);
    qrx_hfsp_entry_t *entry;

    if (type == 3u || type == 4u) {     /* thread record */
        if (parent == HFSP_ROOT_FOLDER && !fs->name_len) {
            uint32_t record_parent = rd32(rec + 4u);
            uint16_t record_units = rd16(rec + 8u);

            if (record_parent == HFSP_ROOT_FOLDER && rec_len >= 10u)
                hfsp_name(fs->name, rec + 10u, record_units, &fs->name_len);
        }
        return;
    }
    if (type != 1u && type != 2u)
        return;
    if (fs->nentries >= (uint16_t)QRX_HFSP_MAX_ENTRIES) {
        fs->truncated = 1u;
        return;
    }
    entry = &fs->entries[fs->nentries];
    memset(entry, 0, sizeof(*entry));
    entry->parent_id = parent;
    entry->cnid = rd32(rec + 8u);
    entry->flags = rd16(rec + 2u);
    entry->created = rd32(rec + 12u);
    entry->modified = rd32(rec + 16u);
    hfsp_name(entry->name, key + 8u, units, &entry->name_len);

    if (type == 1u) {
        if (rec_len < 88u)
            return;
        entry->is_dir = 1u;
    } else {
        const uint8_t *data;
        const uint8_t *rsrc;

        if (rec_len < 248u)
            return;
        data = rec + 88u;
        rsrc = rec + 168u;
        entry->type_code = rd32(rec + 48u);
        entry->creator = rd32(rec + 52u);
        entry->fd_flags = rd16(rec + 56u);
        entry->fd_folder = rd16(rec + 62u);
        entry->data_len = hfsp_logical(data);
        entry->data_count = hfsp_extents(data, entry->data_ext, 8);
        entry->rsrc_len = hfsp_logical(rsrc);
        entry->rsrc_count = hfsp_extents(rsrc, entry->rsrc_ext, 8);
    }
    fs->nentries++;
}

static int32_t catalog_scan(qrx_hfsp_t *fs)
{
    bt_header_t header;
    uint32_t node_index;
    int guard = 0;
    int32_t status;

    status = bt_header(fs, fs->cat_list, fs->cat_count, fs->cat_size, &header);
    if (status != QRX_MACIMG_OK)
        return status;
    fs->cat_node_size = header.node_size;
    if (header.first_leaf == 0u)
        return 0;
    node_index = header.first_leaf;
    while (node_index != 0u && guard++ < 4096) {
        uint16_t records;
        uint16_t i;

        status = read_node(fs, fs->cat_list, fs->cat_count, header.node_size,
                           fs->cat_size, node_index);
        if (status != QRX_MACIMG_OK)
            return status;
        if ((uint8_t)fs->node[8] != 0xFFu)
            return QRX_MACIMG_ERR_FORMAT;
        records = rd16(fs->node + 10u);
        for (i = 0; i < records; i++) {
            uint16_t start = rd16(fs->node + header.node_size -
                                  2u * (uint16_t)(records - i + 1u));
            uint16_t end = rd16(fs->node + header.node_size -
                                2u * (uint16_t)(records - i));
            const uint8_t *key;
            uint16_t key_len;
            uint16_t rec_len;

            if (end > header.node_size || start >= end || start < 14u)
                continue;
            key = fs->node + start;
            key_len = (uint16_t)(rd16(key) + 2u);
            if ((uint32_t)start + key_len + 2u > end)
                continue;
            rec_len = (uint16_t)(end - start - key_len);
            parse_record(fs, key, key + key_len, rec_len);
        }
        node_index = rd32(fs->node);
    }
    return QRX_MACIMG_OK;
}

/*
 * Files without a resource fork in their catalog record may still carry one
 * as com.apple.ResourceFork in the attributes tree.  This runs after the
 * catalog scan because both tree readers reuse fs->node as scratch.
 */
static void fill_attribute_forks(qrx_hfsp_t *fs)
{
    uint16_t i;

    if (fs->attr_size == 0u || fs->attr_count <= 0)
        return;
    for (i = 0; i < fs->nentries; i++) {
        qrx_hfsp_entry_t *entry = &fs->entries[i];
        uint32_t length = 0;
        int count = 0;

        if (entry->is_dir || entry->rsrc_len != 0u || entry->rsrc_count != 0)
            continue;
        if (!resource_attribute(fs, entry->cnid, entry->rsrc_ext, &count,
                                &length))
            continue;
        entry->rsrc_len = length;
        entry->rsrc_count = count;
        entry->rsrc_via_attr = 1u;
    }
}

/* ------------------------------------------------------------------- open */

static int32_t build_catalog_extents(qrx_hfsp_t *fs)
{
    if (fs->cat_count == 0)
        return QRX_MACIMG_ERR_FORMAT;
    if (extent_bytes(fs, fs->cat_list, fs->cat_count) < fs->cat_size) {
        int32_t status = collect_overflow(fs, 4u, 0, fs->cat_list,
                                          &fs->cat_count,
                                          QRX_HFSP_MAX_EXTENTS);
        if (status != QRX_MACIMG_OK)
            return status;
    }
    return 0;
}

int32_t qrx_hfsp_open(qrx_hfsp_t *fs, const uint8_t *volume, uint32_t len)
{
    const uint8_t *vh;
    uint32_t offset = 0;
    uint32_t volume_len;
    uint16_t sig;
    int32_t status;

    if (!fs)
        return QRX_MACIMG_ERR_SHORT;
    memset(fs, 0, sizeof(*fs));
    if (!volume || len < HFSP_VH_OFFSET + HFSP_VH_SIZE)
        return QRX_MACIMG_ERR_SHORT;

    /* An HFS wrapper keeps the real volume at drEmbedExtent. */
    sig = rd16(volume + HFSP_VH_OFFSET);
    if (sig == HFSP_HFS_SIG) {
        uint16_t embed;
        uint32_t alloc_size;
        uint32_t alloc_start;
        uint32_t start;
        uint32_t blocks;
        uint64_t embed_offset;
        uint64_t embed_len;

        if (len < HFSP_VH_OFFSET + 130u)
            return QRX_MACIMG_ERR_SHORT;
        embed = rd16(volume + HFSP_VH_OFFSET + 124u);
        if (embed != HFSP_SIG_HFSP && embed != HFSP_SIG_HFSX)
            return QRX_MACIMG_ERR_FORMAT;
        alloc_size = rd32(volume + HFSP_VH_OFFSET + 20u);
        alloc_start = rd16(volume + HFSP_VH_OFFSET + 28u);
        start = rd16(volume + HFSP_VH_OFFSET + 126u);
        blocks = rd16(volume + HFSP_VH_OFFSET + 128u);
        if (alloc_size == 0u || blocks == 0u)
            return QRX_MACIMG_ERR_FORMAT;
        embed_offset = (uint64_t)alloc_start * HFSP_SECTOR +
                       (uint64_t)start * alloc_size;
        embed_len = (uint64_t)blocks * alloc_size;
        if (embed_offset + HFSP_VH_OFFSET + HFSP_VH_SIZE > (uint64_t)len)
            return QRX_MACIMG_ERR_RANGE;
        if (embed_len > (uint64_t)len - embed_offset)
            embed_len = (uint64_t)len - embed_offset;
        offset = (uint32_t)embed_offset;
        volume_len = (uint32_t)embed_len;
        fs->wrapped = 1u;
        sig = rd16(volume + offset + HFSP_VH_OFFSET);
    } else {
        volume_len = len;
    }
    if (sig != HFSP_SIG_HFSP && sig != HFSP_SIG_HFSX)
        return QRX_MACIMG_ERR_FORMAT;
    if (offset + HFSP_VH_OFFSET + HFSP_VH_SIZE > len)
        return QRX_MACIMG_ERR_RANGE;

    fs->vol = volume;
    fs->size = len;
    fs->offset = offset;
    fs->volume_size = volume_len;
    fs->signature = sig;
    fs->case_sensitive = (sig == HFSP_SIG_HFSX) ? 1u : 0u;
    vh = volume + offset + HFSP_VH_OFFSET;
    fs->attributes = rd32(vh + 4u);
    fs->journaled = rd32(vh + 12u) != 0u ? 1u : 0u;
    fs->file_count = rd32(vh + 32u);
    fs->folder_count = rd32(vh + 36u);
    fs->block_size = rd32(vh + 40u);
    fs->total_blocks = rd32(vh + 44u);
    fs->free_blocks = rd32(vh + 48u);

    if (fs->block_size < HFSP_SECTOR || (fs->block_size % HFSP_SECTOR) != 0u)
        return QRX_MACIMG_ERR_FORMAT;
    if (fs->total_blocks == 0u)
        return QRX_MACIMG_ERR_FORMAT;
    /* Block 0 starts at the beginning of the volume, so the header at 1024
     * lies inside the first block when blocks are at least 1 KiB. */
    if ((uint64_t)fs->total_blocks * fs->block_size > (uint64_t)volume_len)
        return QRX_MACIMG_ERR_RANGE;

    fs->cat_size = hfsp_logical(vh + 272u);
    fs->ext_size = hfsp_logical(vh + 192u);
    fs->attr_size = hfsp_logical(vh + 352u);
    fs->cat_count = hfsp_extents(vh + 272u, fs->cat_list, QRX_HFSP_MAX_EXTENTS);
    fs->ext_count = hfsp_extents(vh + 192u, fs->ext_list, QRX_HFSP_MAX_EXTENTS);
    fs->attr_count = hfsp_extents(vh + 352u, fs->attr_list,
                                  QRX_HFSP_MAX_EXTENTS);
    if (fs->cat_size == 0u || fs->cat_count == 0)
        return QRX_MACIMG_ERR_FORMAT;
    if (fs->ext_size == 0u || fs->ext_count == 0)
        return QRX_MACIMG_ERR_FORMAT;

    {
        bt_header_t header;

        status = bt_header(fs, fs->ext_list, fs->ext_count, fs->ext_size,
                           &header);
        if (status != QRX_MACIMG_OK)
            return status;
        fs->ext_node_size = header.node_size;
    }
    status = build_catalog_extents(fs);
    if (status != QRX_MACIMG_OK)
        return status;
    status = catalog_scan(fs);
    if (status != QRX_MACIMG_OK)
        return status;
    fill_attribute_forks(fs);
    return QRX_MACIMG_OK;
}

/* ---------------------------------------------------------------- lookup */

const qrx_hfsp_entry_t *qrx_hfsp_lookup(const qrx_hfsp_t *fs, uint32_t parent,
                                        const char *name)
{
    uint16_t i;

    if (!fs || !name)
        return NULL;
    for (i = 0; i < fs->nentries; i++) {
        const qrx_hfsp_entry_t *entry = &fs->entries[i];

        if (entry->parent_id == parent && name_equal(fs, entry->name, name))
            return entry;
    }
    return NULL;
}

const qrx_hfsp_entry_t *qrx_hfsp_lookup_path(const qrx_hfsp_t *fs,
                                             const char *path)
{
    uint32_t parent = HFSP_ROOT_FOLDER;
    const qrx_hfsp_entry_t *entry = NULL;
    char component[64];
    const char *p = path;

    if (!fs || !path)
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
        entry = qrx_hfsp_lookup(fs, parent, component);
        if (entry == NULL)
            return NULL;
        if (*p && !entry->is_dir)
            return NULL;
        parent = entry->cnid;
    }
    return entry;
}

/* ------------------------------------------------------------------ forks */

int32_t qrx_hfsp_read_fork(const qrx_hfsp_t *hfs, const qrx_hfsp_entry_t *entry,
                           int resource, uint8_t *dst, uint32_t cap,
                           uint32_t *written)
{
    qrx_hfsp_t *fs = (qrx_hfsp_t *)hfs;     /* node[] is scratch space */
    qrx_hfsp_extent_t list[QRX_HFSP_MAX_EXTENTS];
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
    count = 0;
    if (resource) {
        count = entry->rsrc_count;
        if (count > 8)
            count = 8;
        memcpy(list, entry->rsrc_ext, (size_t)count * sizeof(list[0]));
    } else {
        count = entry->data_count;
        if (count > 8)
            count = 8;
        memcpy(list, entry->data_ext, (size_t)count * sizeof(list[0]));
    }
    if (extent_bytes(fs, list, count) < need) {
        status = collect_overflow(fs, entry->cnid, resource, list, &count,
                                  QRX_HFSP_MAX_EXTENTS);
        if (status != QRX_MACIMG_OK)
            return status;
    }
    if (extent_bytes(fs, list, count) < need)
        return QRX_MACIMG_ERR_FORMAT;
    status = copy_extents(fs, list, count, 0u, dst, need);
    if (status != QRX_MACIMG_OK)
        return status;
    if (written)
        *written = need;
    return QRX_MACIMG_OK;
}
