#include "qrx_macimage.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void copy_cstr(char *dst, const uint8_t *src, uint32_t max_len)
{
    uint32_t i = 0;
    while (i < max_len && src[i] != 0 && i + 1u < 33u) {
        dst[i] = (char)src[i];
        i++;
    }
    dst[i] = '\0';
}

uint32_t qrx_macimg_dc42_checksum(const uint8_t *bytes, uint32_t len)
{
    uint32_t sum = 0;
    uint32_t i = 0;

    while (i + 1u < len) {
        sum += ((uint32_t)bytes[i] << 8) | (uint32_t)bytes[i + 1u];
        sum = (sum >> 1) | (sum << 31);
        i += 2u;
    }
    if (i < len) {
        sum += (uint32_t)bytes[i] << 8;
        sum = (sum >> 1) | (sum << 31);
    }
    return sum;
}

const char *qrx_macimg_container_name(qrx_macimg_container container)
{
    switch (container) {
    case QRX_MACIMG_RAW: return "raw";
    case QRX_MACIMG_DC42: return "dc42";
    case QRX_MACIMG_NDIF: return "ndif";
    case QRX_MACIMG_APM: return "apm";
    }
    return "unknown";
}

const char *qrx_macimg_fs_name(qrx_macimg_fs fs)
{
    switch (fs) {
    case QRX_MACIMG_FS_NONE: return "none";
    case QRX_MACIMG_FS_MFS: return "mfs";
    case QRX_MACIMG_FS_HFS: return "hfs";
    case QRX_MACIMG_FS_HFSPLUS: return "hfs+";
    }
    return "unknown";
}

const char *qrx_macimg_strerror(int32_t status)
{
    switch (status) {
    case QRX_MACIMG_OK: return "ok";
    case QRX_MACIMG_ERR_SHORT: return "short";
    case QRX_MACIMG_ERR_FORMAT: return "format";
    case QRX_MACIMG_ERR_CHECKSUM: return "checksum";
    case QRX_MACIMG_ERR_NDIF: return "ndif";
    case QRX_MACIMG_ERR_PART: return "partition";
    case QRX_MACIMG_ERR_RANGE: return "range";
    case QRX_MACIMG_ERR_NO_VOLUME: return "no-volume";
    }
    return "error";
}

/* ------------------------------------------------------------------ APM */

#define APM_BLOCK_SIZE 512u
#define APM_SIG_BLOCK0 0x4552u   /* 'ER' */
#define APM_SIG_ENTRY 0x504Du    /* 'PM' */

static int32_t apm_parse(qrx_macimg_t *img)
{
    const uint8_t *b = img->data;
    uint32_t block_size;
    uint32_t block_count;
    uint32_t index;

    if (img->size < 2u * APM_BLOCK_SIZE)
        return QRX_MACIMG_ERR_SHORT;
    block_size = rd16(b + 2u);
    block_count = rd32(b + 4u);
    if (block_size != APM_BLOCK_SIZE)
        return QRX_MACIMG_ERR_FORMAT;
    if (block_count == 0u || (uint64_t)block_count * APM_BLOCK_SIZE > (uint64_t)img->size)
        return QRX_MACIMG_ERR_RANGE;

    img->nparts = 0;
    for (index = 1u; index <= block_count; index++) {
        const uint8_t *entry;
        qrx_macimg_part_t *part;
        uint32_t start;
        uint32_t count;

        if (img->nparts >= (int)QRX_MACIMG_MAX_PARTS)
            break;
        if ((uint64_t)(index + 1u) * APM_BLOCK_SIZE > (uint64_t)img->size)
            return QRX_MACIMG_ERR_RANGE;
        entry = b + index * APM_BLOCK_SIZE;
        if (rd16(entry) != APM_SIG_ENTRY)
            break;
        start = rd32(entry + 8u);
        count = rd32(entry + 12u);
        if (count == 0u)
            return QRX_MACIMG_ERR_PART;
        if (start > block_count || count > block_count - start)
            return QRX_MACIMG_ERR_PART;
        part = &img->parts[img->nparts];
        memset(part, 0, sizeof(*part));
        part->used = 1u;
        part->start_block = start;
        part->block_count = count;
        copy_cstr(part->name, entry + 16u, 32u);
        copy_cstr(part->type, entry + 48u, 32u);
        img->nparts++;
    }
    if (img->nparts == 0)
        return QRX_MACIMG_ERR_PART;
    return QRX_MACIMG_OK;
}

/* ------------------------------------------------------------ resource fork */

/*
 * The NDIF chunk map (and the image size) live in a 'bcem' resource of the
 * resource fork.  Only the documented bcem prologue is read: version, name
 * and the disk size in 512-byte blocks.  The chunk map itself is not decoded,
 * so a data fork that is shorter than that size is a chunked/compressed image
 * and is refused instead of being misread as raw sectors.
 */
static int rsrc_find(const uint8_t *r, uint32_t len, uint32_t type, int16_t id,
                     const uint8_t **found, uint32_t *found_len)
{
    uint32_t data_off;
    uint32_t map_off;
    uint32_t map_len;
    uint16_t type_list_off;
    const uint8_t *map;
    const uint8_t *type_list;
    int types;
    int i;

    if (len < 28u)
        return 0;
    data_off = rd32(r);
    map_off = rd32(r + 4u);
    map_len = rd32(r + 12u);
    if (data_off > len || map_off > len || map_len < 28u)
        return 0;
    if ((uint64_t)map_off + map_len > (uint64_t)len)
        return 0;
    map = r + map_off;
    type_list_off = rd16(map + 24u);
    if ((uint64_t)map_off + type_list_off + 2u > (uint64_t)len)
        return 0;
    type_list = map + type_list_off;
    types = (int)rd16(type_list) + 1;
    for (i = 0; i < types; i++) {
        const uint8_t *type_entry = type_list + 2 + (uint32_t)i * 8u;
        int resources;
        uint16_t ref_off;
        int j;

        if ((uint64_t)(type_entry - r) + 8u > (uint64_t)len)
            return 0;
        if (rd32(type_entry) != type)
            continue;
        resources = (int)rd16(type_entry + 4u) + 1;
        ref_off = rd16(type_entry + 6u);
        for (j = 0; j < resources; j++) {
            const uint8_t *ref = type_list + ref_off + (uint32_t)j * 12u;
            uint32_t off;
            uint32_t size;

            if ((uint64_t)(ref - r) + 12u > (uint64_t)len)
                return 0;
            if ((int16_t)rd16(ref) != id)
                continue;
            off = ((uint32_t)ref[5] << 16) | ((uint32_t)ref[6] << 8) | ref[7];
            if ((uint64_t)data_off + off + 4u > (uint64_t)len)
                return 0;
            size = rd32(r + data_off + off);
            if ((uint64_t)data_off + off + 4u + size > (uint64_t)len)
                return 0;
            *found = r + data_off + off + 4u;
            *found_len = size;
            return 1;
        }
    }
    return 0;
}

#define BCEM_SIZE_OFFSET 68u

static int32_t ndif_parse(qrx_macimg_t *img, const uint8_t *rsrc,
                          uint32_t rsrc_len)
{
    const uint8_t *bcem = NULL;
    uint32_t bcem_len = 0;
    uint32_t blocks;
    uint16_t version;
    uint8_t name_len;

    if (!rsrc_find(rsrc, rsrc_len, 0x6263656Du /* 'bcem' */, 128, &bcem,
                   &bcem_len))
        return QRX_MACIMG_ERR_FORMAT;
    if (bcem_len < BCEM_SIZE_OFFSET + 4u)
        return QRX_MACIMG_ERR_SHORT;
    version = rd16(bcem);
    if (version < 10u || version > 12u)
        return QRX_MACIMG_ERR_FORMAT;
    name_len = bcem[4];
    if (name_len > 63u)
        name_len = 63u;
    memcpy(img->name, bcem + 5u, name_len);
    img->name[name_len] = '\0';
    blocks = rd32(bcem + BCEM_SIZE_OFFSET);
    if (blocks == 0u)
        return QRX_MACIMG_ERR_FORMAT;
    img->ndif_blocks = blocks;
    if ((uint64_t)blocks * 512u != (uint64_t)img->size)
        return QRX_MACIMG_ERR_NDIF;    /* chunked or compressed data fork */
    return QRX_MACIMG_OK;
}

/* ------------------------------------------------------------------ open */

static qrx_macimg_fs volume_kind(const uint8_t *vol, uint32_t len)
{
    if (len < 1024u + 2u)
        return QRX_MACIMG_FS_NONE;
    if (rd16(vol + 1024u) == 0xD2D7u)
        return QRX_MACIMG_FS_MFS;
    if (rd16(vol + 1024u) == 0x482Bu || rd16(vol + 1024u) == 0x4858u)
        return QRX_MACIMG_FS_HFSPLUS;
    if (rd16(vol + 1024u) == 0x4244u) {
        /* Mac OS 8.1-9 kept HFS+ inside an HFS wrapper: one volume, not two. */
        if (len >= 1024u + 126u &&
            (rd16(vol + 1024u + 124u) == 0x482Bu ||
             rd16(vol + 1024u + 124u) == 0x4858u))
            return QRX_MACIMG_FS_HFSPLUS;
        return QRX_MACIMG_FS_HFS;
    }
    return QRX_MACIMG_FS_NONE;
}

static void use_volume(qrx_macimg_t *img, int part_index)
{
    img->volume_part = part_index;
    if (part_index > 0 &&
        part_index <= img->nparts) {
        const qrx_macimg_part_t *part = &img->parts[part_index - 1];
        img->volume_offset = part->start_block * APM_BLOCK_SIZE;
        img->volume_size = part->block_count * APM_BLOCK_SIZE;
    } else {
        img->volume_offset = 0u;
        img->volume_size = img->size;
    }
    if (img->volume_offset > img->size)
        img->volume_size = 0u;
    else if (img->volume_size > img->size - img->volume_offset)
        img->volume_size = img->size - img->volume_offset;
    img->fs = volume_kind(img->data + img->volume_offset, img->volume_size);
}

static int dc42_looks_valid(const uint8_t *b, uint32_t len, uint32_t *data_size,
                            uint32_t *tag_size)
{
    uint32_t ds;
    uint32_t ts;
    uint8_t disk_format;

    if (len < 84u)
        return 0;
    ds = rd32(b + 64u);
    ts = rd32(b + 68u);
    disk_format = b[80];
    if (rd16(b + 82u) != 0x0100u)
        return 0;                      /* DiskCopy's own magic, see FTN */
    if (disk_format > 3u)
        return 0;
    if (ds < 512u || (ds % 512u) != 0u)
        return 0;
    if ((uint64_t)84u + ds + ts > (uint64_t)len)
        return 0;
    if ((ts % 512u) != 0u)
        return 0;
    *data_size = ds;
    *tag_size = ts;
    return 1;
}

int32_t qrx_macimg_open(qrx_macimg_t *img, const uint8_t *bytes, uint32_t len,
                        const uint8_t *rsrc, uint32_t rsrc_len)
{
    uint32_t data_size = 0;
    uint32_t tag_size = 0;

    if (!img)
        return QRX_MACIMG_ERR_SHORT;
    memset(img, 0, sizeof(*img));
    if (!bytes || len < 512u)
        return QRX_MACIMG_ERR_SHORT;
    img->data = bytes;
    img->size = len;

    if (dc42_looks_valid(bytes, len, &data_size, &tag_size)) {
        uint32_t stored = rd32(bytes + 72u);
        uint32_t computed = qrx_macimg_dc42_checksum(bytes + 84u, data_size);

        img->container = QRX_MACIMG_DC42;
        img->checksum_ok = (uint8_t)(stored == computed);
        {
            uint8_t name_len = bytes[0];
            if (name_len > 63u)
                name_len = 63u;
            memcpy(img->name, bytes + 1u, name_len);
            img->name[name_len] = '\0';
        }
        (void)tag_size;
        if (!img->checksum_ok)
            return QRX_MACIMG_ERR_CHECKSUM;
        img->data = bytes + 84u;
        img->size = data_size;
        use_volume(img, 0);
        return QRX_MACIMG_OK;
    }

    if (rsrc && rsrc_len > 0u) {
        int32_t status = ndif_parse(img, rsrc, rsrc_len);
        if (status != QRX_MACIMG_ERR_FORMAT) {
            img->container = QRX_MACIMG_NDIF;
            if (status != QRX_MACIMG_OK)
                return status;
            use_volume(img, 0);
            return QRX_MACIMG_OK;
        }
    }

    if (rd16(bytes) == APM_SIG_BLOCK0) {
        int32_t status = apm_parse(img);

        img->container = QRX_MACIMG_APM;
        if (status != QRX_MACIMG_OK)
            return status;
        use_volume(img, 0);
        {
            int i;
            for (i = 1; i <= img->nparts; i++) {
                use_volume(img, i);
                if (img->fs != QRX_MACIMG_FS_NONE)
                    break;
            }
        }
        return QRX_MACIMG_OK;
    }

    img->container = QRX_MACIMG_RAW;
    use_volume(img, 0);
    return QRX_MACIMG_OK;
}

int32_t qrx_macimg_select_partition(qrx_macimg_t *img, int index)
{
    if (!img || index < 1 || index > img->nparts)
        return QRX_MACIMG_ERR_PART;
    use_volume(img, index);
    return QRX_MACIMG_OK;
}
