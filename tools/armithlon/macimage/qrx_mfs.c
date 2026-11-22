#include "qrx_mfs.h"

#include <string.h>

#define MFS_MDB_OFFSET 1024u
#define MFS_MDB_HEADER 64u
#define MFS_SECTOR 512u
#define MFS_SIG 0xD2D7u
#define MFS_ENTRY_FIXED 50u
#define MFS_BLOCKMAP_END 0x001u
#define MFS_BLOCKMAP_FREE 0x000u
#define MFS_BLOCKMAP_DIR 0xFFFu

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* MacRoman high bytes become '.', control bytes stay as they are: this is a
 * tool-friendly rendering, not a text conversion. */
static void copy_name(char *dst, const uint8_t *nam, uint8_t len)
{
    uint8_t i;

    if (len > 63u)
        len = 63u;
    for (i = 0; i < len; i++) {
        uint8_t c = nam[i];
        dst[i] = (c >= 0x20u && c < 0x7Fu) ? (char)c : '.';
    }
    dst[len] = '\0';
}

uint16_t qrx_mfs_blockmap_next(const qrx_mfs_t *mfs, uint16_t block)
{
    uint32_t index;
    uint32_t offset;
    const uint8_t *p;

    if (!mfs || !mfs->blockmap)
        return 0;
    if (block < 2u)
        return 0;
    index = (uint32_t)block - 2u;
    offset = (index * 3u) / 2u;
    if (offset + 2u > mfs->blockmap_len)
        return 0;
    p = mfs->blockmap + offset;
    if ((index & 1u) == 0u)
        return (uint16_t)(((uint16_t)p[0] << 4) | (uint16_t)(p[1] >> 4));
    return (uint16_t)(((uint16_t)(p[0] & 0x0Fu) << 8) | (uint16_t)p[1]);
}

static int32_t alloc_address(const qrx_mfs_t *mfs, uint16_t block,
                             uint32_t *address)
{
    uint32_t first;

    if (block < 2u)
        return QRX_MACIMG_ERR_RANGE;
    if ((uint32_t)block >= (uint32_t)mfs->alloc_blocks + 2u)
        return QRX_MACIMG_ERR_RANGE;
    first = (uint32_t)mfs->alloc_start * MFS_SECTOR;
    *address = first + ((uint32_t)block - 2u) * mfs->alloc_size;
    if ((uint64_t)*address + mfs->alloc_size > (uint64_t)mfs->size)
        return QRX_MACIMG_ERR_RANGE;
    return QRX_MACIMG_OK;
}

int32_t qrx_mfs_read_fork(const qrx_mfs_t *mfs, const qrx_mfs_file_t *file,
                          int resource, uint8_t *dst, uint32_t cap,
                          uint32_t *written)
{
    uint32_t remaining;
    uint16_t block;
    uint32_t done = 0;
    uint32_t guard = 0;

    if (!mfs || !file || !dst)
        return QRX_MACIMG_ERR_RANGE;
    remaining = resource ? file->rsrc_len : file->logical_len;
    if (remaining > cap)
        return QRX_MACIMG_ERR_RANGE;
    block = resource ? file->rsrc_block : file->start_block;
    if (remaining == 0u) {
        if (written)
            *written = 0u;
        return QRX_MACIMG_OK;
    }
    if (block == 0u)
        return QRX_MACIMG_ERR_RANGE;

    while (done < remaining) {
        uint32_t address;
        uint32_t chunk = remaining - done;
        int32_t status;

        status = alloc_address(mfs, block, &address);
        if (status != QRX_MACIMG_OK)
            return status;
        if (chunk > mfs->alloc_size)
            chunk = mfs->alloc_size;
        memcpy(dst + done, mfs->vol + address, chunk);
        done += chunk;
        if (done >= remaining)
            break;
        block = qrx_mfs_blockmap_next(mfs, block);
        if (block == MFS_BLOCKMAP_END || block == MFS_BLOCKMAP_FREE ||
            block == MFS_BLOCKMAP_DIR)
            return QRX_MACIMG_ERR_FORMAT;
        if (++guard > 4096u)
            return QRX_MACIMG_ERR_FORMAT;
    }
    if (written)
        *written = done;
    return QRX_MACIMG_OK;
}

static void parse_entry(qrx_mfs_t *mfs, const uint8_t *entry)
{
    qrx_mfs_file_t *file;
    uint8_t name_len;

    if (mfs->nfiles >= (uint16_t)QRX_MFS_MAX_FILES)
        return;
    name_len = entry[MFS_ENTRY_FIXED];
    if (name_len == 0u)
        return;
    file = &mfs->files[mfs->nfiles];
    memset(file, 0, sizeof(*file));
    file->flags = entry[0];
    file->type_code = rd32(entry + 2u);
    file->creator = rd32(entry + 6u);
    file->finder_flags = rd16(entry + 10u);
    file->file_number = rd32(entry + 18u);
    file->start_block = rd16(entry + 22u);
    file->logical_len = rd32(entry + 24u);
    file->physical_len = rd32(entry + 28u);
    file->rsrc_block = rd16(entry + 32u);
    file->rsrc_len = rd32(entry + 34u);
    file->rsrc_physical_len = rd32(entry + 38u);
    file->created = rd32(entry + 42u);
    file->modified = rd32(entry + 46u);
    file->name_len = name_len;
    copy_name(file->name, entry + MFS_ENTRY_FIXED + 1u, name_len);
    mfs->nfiles++;
}

int32_t qrx_mfs_open(qrx_mfs_t *mfs, const uint8_t *volume, uint32_t len)
{
    const uint8_t *mdb;
    uint32_t dir_start;
    uint32_t dir_end;
    uint32_t blockmap_len;
    uint64_t alloc_end;
    uint32_t sector;

    if (!mfs)
        return QRX_MACIMG_ERR_SHORT;
    memset(mfs, 0, sizeof(*mfs));
    if (!volume || len < MFS_MDB_OFFSET + MFS_MDB_HEADER + 2u)
        return QRX_MACIMG_ERR_SHORT;
    mdb = volume + MFS_MDB_OFFSET;
    if (rd16(mdb) != MFS_SIG)
        return QRX_MACIMG_ERR_FORMAT;

    mfs->vol = volume;
    mfs->size = len;
    mfs->attributes = rd16(mdb + 10u);
    mfs->root_files = rd16(mdb + 12u);
    mfs->dir_start = rd16(mdb + 14u);
    mfs->dir_len = rd16(mdb + 16u);
    mfs->alloc_blocks = rd16(mdb + 18u);
    mfs->alloc_size = rd32(mdb + 20u);
    mfs->alloc_start = rd16(mdb + 28u);
    mfs->next_file_number = rd32(mdb + 30u);
    mfs->free_blocks = rd16(mdb + 34u);
    mfs->name_len = mdb[36];
    if (mfs->name_len > 27u)
        return QRX_MACIMG_ERR_FORMAT;
    memcpy(mfs->name, mdb + 37u, mfs->name_len);
    mfs->name[mfs->name_len] = '\0';

    if (mfs->dir_start < 3u || mfs->dir_len == 0u)
        return QRX_MACIMG_ERR_FORMAT;
    if (mfs->alloc_size < MFS_SECTOR || (mfs->alloc_size % MFS_SECTOR) != 0u)
        return QRX_MACIMG_ERR_FORMAT;
    if (mfs->alloc_blocks == 0u || mfs->alloc_start < 3u)
        return QRX_MACIMG_ERR_FORMAT;
    if ((uint32_t)mfs->dir_start + mfs->dir_len > mfs->alloc_start)
        return QRX_MACIMG_ERR_FORMAT;

    dir_start = (uint32_t)mfs->dir_start * MFS_SECTOR;
    dir_end = dir_start + (uint32_t)mfs->dir_len * MFS_SECTOR;
    if (dir_end > len)
        return QRX_MACIMG_ERR_RANGE;

    alloc_end = (uint64_t)mfs->alloc_start * MFS_SECTOR +
                (uint64_t)mfs->alloc_blocks * mfs->alloc_size;
    if (alloc_end > (uint64_t)len)
        return QRX_MACIMG_ERR_RANGE;

    blockmap_len = ((uint32_t)mfs->alloc_blocks * 3u + 1u) / 2u;
    if (MFS_MDB_OFFSET + MFS_MDB_HEADER + blockmap_len > len)
        return QRX_MACIMG_ERR_RANGE;
    mfs->blockmap = volume + MFS_MDB_OFFSET + MFS_MDB_HEADER;
    mfs->blockmap_len = blockmap_len;

    for (sector = 0; sector < mfs->dir_len; sector++) {
        const uint8_t *block = volume + dir_start + sector * MFS_SECTOR;
        uint32_t off = 0;

        while (off + MFS_ENTRY_FIXED + 1u <= MFS_SECTOR) {
            uint8_t flags = block[off];
            uint8_t name_len;
            uint32_t entry_len;

            if ((flags & 0x80u) == 0u)
                break;              /* empty space ends this sector */
            name_len = block[off + MFS_ENTRY_FIXED];
            entry_len = MFS_ENTRY_FIXED + 1u + name_len;
            if (name_len == 0u || off + entry_len > MFS_SECTOR)
                break;              /* truncated or crossing the sector */
            parse_entry(mfs, block + off);
            off += (entry_len + 1u) & ~1u;
        }
    }
    return QRX_MACIMG_OK;
}
