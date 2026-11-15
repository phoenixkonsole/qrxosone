#include "qrx_testimg.h"

#include <string.h>

#define SECTOR 512u
#define MDB_OFFSET 1024u
#define MDB_HEADER 64u
#define ALLOC_SIZE 1024u
#define ALLOC_BLOCKS 391u
#define ALLOC_START 16u
#define DIR_START 4u
#define DIR_LEN 12u

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

uint8_t qrx_testimg_payload_byte(uint32_t index)
{
    return (uint8_t)((index * 7u + 0x31u) & 0xFFu);
}

uint8_t qrx_testimg_rsrc_byte(uint32_t index)
{
    return (uint8_t)(0xA5u ^ (uint8_t)index);
}

static uint32_t dc42_sum(const uint8_t *p, uint32_t len)
{
    uint32_t sum = 0;
    uint32_t i = 0;

    while (i + 1u < len) {
        sum += ((uint32_t)p[i] << 8) | p[i + 1u];
        sum = (sum >> 1) | (sum << 31);
        i += 2u;
    }
    if (i < len) {
        sum += (uint32_t)p[i] << 8;
        sum = (sum >> 1) | (sum << 31);
    }
    return sum;
}

/* 12-bit block map, two entries per three bytes, MSB first. */
static void map_set(uint8_t *vol, uint16_t block, uint16_t value)
{
    uint32_t index = (uint32_t)block - 2u;
    uint32_t off = MDB_OFFSET + MDB_HEADER + (index * 3u) / 2u;

    if ((index & 1u) == 0u) {
        vol[off] = (uint8_t)(value >> 4);
        vol[off + 1u] = (uint8_t)((vol[off + 1u] & 0x0Fu) | ((value & 0x0Fu) << 4));
    } else {
        vol[off] = (uint8_t)((vol[off] & 0xF0u) | ((value >> 8) & 0x0Fu));
        vol[off + 1u] = (uint8_t)value;
    }
}

static uint16_t alloc_chain(uint8_t *vol, uint32_t size, uint16_t *next_block)
{
    uint32_t blocks = (size + ALLOC_SIZE - 1u) / ALLOC_SIZE;
    uint16_t first = 0;
    uint32_t i;

    for (i = 0; i < blocks; i++) {
        uint16_t block = (*next_block)++;

        if (first == 0u)
            first = block;
        map_set(vol, block, (uint16_t)((i + 1u == blocks) ? 0x001u : *next_block));
    }
    return first;
}

static uint32_t block_address(uint16_t block)
{
    return ALLOC_START * SECTOR + ((uint32_t)block - 2u) * ALLOC_SIZE;
}

typedef struct {
    uint32_t sector;
    uint32_t off;
} dir_cursor;

static void add_entry(uint8_t *vol, dir_cursor *cursor, const char *name,
                      const char *type, const char *creator, uint16_t data_block,
                      uint32_t data_len, uint16_t rsrc_block, uint32_t rsrc_len,
                      uint32_t file_number)
{
    uint8_t name_len = (uint8_t)strlen(name);
    uint32_t entry_len = 50u + 1u + name_len;
    uint32_t base;
    uint8_t *entry;

    if (cursor->off + entry_len > SECTOR) {
        cursor->sector++;
        cursor->off = 0;
    }
    base = DIR_START * SECTOR + cursor->sector * SECTOR + cursor->off;
    entry = vol + base;
    entry[0] = 0x80;                       /* in use */
    entry[1] = 0x00;
    memcpy(entry + 2u, type, 4u);
    memcpy(entry + 6u, creator, 4u);
    wr16(entry + 10u, 0x0100u);            /* finder flag: no INIT */
    wr32(entry + 12u, 0u);
    wr16(entry + 16u, 0u);
    wr32(entry + 18u, file_number);
    wr16(entry + 22u, data_block);
    wr32(entry + 24u, data_len);
    wr32(entry + 28u, data_len == 0u ? 0u : ((data_len + ALLOC_SIZE - 1u) /
                                             ALLOC_SIZE) * ALLOC_SIZE);
    wr16(entry + 32u, rsrc_block);
    wr32(entry + 34u, rsrc_len);
    wr32(entry + 38u, rsrc_len == 0u ? 0u : ((rsrc_len + ALLOC_SIZE - 1u) /
                                             ALLOC_SIZE) * ALLOC_SIZE);
    wr32(entry + 42u, 0x12345678u);
    wr32(entry + 46u, 0x12345678u);
    entry[50] = name_len;
    memcpy(entry + 51u, name, name_len);
    cursor->off += (entry_len + 1u) & ~1u;
}

int qrx_testimg_mfs400(uint8_t *out, uint32_t cap, uint32_t *len)
{
    static const char volume_name[] = "Test Disk";
    dir_cursor cursor;
    uint16_t next_block = 2;
    uint16_t hello_block;
    uint16_t payload_block;
    uint16_t payload_rsrc_block;
    uint32_t used;
    uint32_t i;
    uint8_t *mdb;

    if (!out || cap < QRX_TESTIMG_400K)
        return 0;
    memset(out, 0, QRX_TESTIMG_400K);

    hello_block = alloc_chain(out, 5u, &next_block);
    payload_block = alloc_chain(out, 1300u, &next_block);
    payload_rsrc_block = alloc_chain(out, 64u, &next_block);

    memcpy(out + block_address(hello_block), "Hello", 5u);
    for (i = 0; i < 1300u; i++)
        out[block_address(payload_block) + i] = qrx_testimg_payload_byte(i);
    for (i = 0; i < 64u; i++)
        out[block_address(payload_rsrc_block) + i] = qrx_testimg_rsrc_byte(i);

    cursor.sector = 0;
    cursor.off = 0;
    add_entry(out, &cursor, "Hello", "TEXT", "ttxt", hello_block, 5u, 0u, 0u, 1u);
    add_entry(out, &cursor, "Payload", "BINA", "QRX+", payload_block, 1300u,
              payload_rsrc_block, 64u, 2u);
    add_entry(out, &cursor, "Empty", "TEXT", "ttxt", 0u, 0u, 0u, 0u, 3u);

    used = 4u;                              /* 1 + 2 + 1 allocation blocks */
    mdb = out + MDB_OFFSET;
    wr16(mdb, 0xD2D7u);
    wr32(mdb + 2u, 0u);
    wr32(mdb + 6u, 0u);
    wr16(mdb + 10u, 0u);
    wr16(mdb + 12u, 3u);
    wr16(mdb + 14u, DIR_START);
    wr16(mdb + 16u, DIR_LEN);
    wr16(mdb + 18u, ALLOC_BLOCKS);
    wr32(mdb + 20u, ALLOC_SIZE);
    wr32(mdb + 24u, 8192u);
    wr16(mdb + 28u, ALLOC_START);
    wr32(mdb + 30u, 4u);
    wr16(mdb + 34u, (uint16_t)(ALLOC_BLOCKS - used));
    mdb[36] = (uint8_t)(sizeof(volume_name) - 1u);
    memcpy(mdb + 37u, volume_name, sizeof(volume_name) - 1u);

    if (len)
        *len = QRX_TESTIMG_400K;
    return 1;
}

static int dc42_wrap(const uint8_t *raw, uint32_t raw_len, uint8_t *out,
                     uint32_t cap, uint32_t *len, uint32_t sum_delta)
{
    static const char image_name[] = "Test Disk";
    uint32_t total = 84u + raw_len;
    uint32_t sum;

    if (!raw || !out || cap < total || raw_len == 0u)
        return 0;
    memset(out, 0, total);
    out[0] = (uint8_t)(sizeof(image_name) - 1u);
    memcpy(out + 1u, image_name, sizeof(image_name) - 1u);
    wr32(out + 64u, raw_len);
    wr32(out + 68u, 0u);                    /* no tags */
    sum = dc42_sum(raw, raw_len) + sum_delta;
    wr32(out + 72u, sum);
    wr32(out + 76u, 0u);
    out[80] = 0u;                           /* diskFormat: 400K */
    out[81] = 0x12u;                        /* formatByte: Macintosh 400K */
    wr16(out + 82u, 0x0100u);               /* the FTN's magic */
    memcpy(out + 84u, raw, raw_len);
    if (len)
        *len = total;
    return 1;
}

int qrx_testimg_dc42(const uint8_t *raw, uint32_t raw_len, uint8_t *out,
                     uint32_t cap, uint32_t *len)
{
    return dc42_wrap(raw, raw_len, out, cap, len, 0u);
}

int qrx_testimg_dc42_badsum(const uint8_t *raw, uint32_t raw_len, uint8_t *out,
                            uint32_t cap, uint32_t *len)
{
    return dc42_wrap(raw, raw_len, out, cap, len, 1u);
}

int qrx_testimg_bcem_rsrc(uint32_t blocks, uint8_t *out, uint32_t cap,
                          uint32_t *len)
{
    static const char image_name[] = "Test Disk";
    const uint32_t data_off = 256u;
    const uint32_t bcem_len = 128u;
    const uint32_t map_len = 50u;
    uint32_t map_off = data_off + 4u + bcem_len;
    uint32_t total = map_off + map_len;
    uint8_t *bcem;
    uint8_t *map;
    uint8_t *type_list;
    uint8_t *ref;

    if (!out || cap < total)
        return 0;
    memset(out, 0, total);

    wr32(out + 0u, data_off);
    wr32(out + 4u, map_off);
    wr32(out + 8u, 4u + bcem_len);
    wr32(out + 12u, map_len);

    wr32(out + data_off, bcem_len);
    bcem = out + data_off + 4u;
    wr16(bcem, 12u);                        /* bcem version 12 */
    bcem[4] = (uint8_t)(sizeof(image_name) - 1u);
    memcpy(bcem + 5u, image_name, sizeof(image_name) - 1u);
    wr32(bcem + 68u, blocks);

    map = out + map_off;
    memcpy(map, out, 16u);                  /* the map repeats the header */
    wr32(map + 16u, 0u);
    wr16(map + 20u, 0u);
    wr16(map + 22u, 0u);
    wr16(map + 24u, 28u);                   /* type list offset */
    wr16(map + 26u, 50u);                   /* name list offset: none */

    type_list = map + 28u;
    wr16(type_list, 0u);                    /* one type */
    wr32(type_list + 2u, 0x6263656DU);      /* 'bcem' */
    wr16(type_list + 6u, 0u);               /* one resource */
    wr16(type_list + 8u, 10u);              /* reference list offset */

    ref = type_list + 10u;
    wr16(ref, 128u);                        /* bcem resource id */
    wr16(ref + 2u, 0xFFFFu);                /* no name */
    ref[4] = 0u;
    ref[5] = ref[6] = ref[7] = 0u;          /* data offset within the fork */
    wr32(ref + 8u, 0u);

    if (len)
        *len = total;
    return 1;
}

static void apm_entry(uint8_t *out, uint32_t index, uint32_t start,
                      uint32_t count, const char *name, const char *type)
{
    uint8_t *entry = out + index * SECTOR;

    wr16(entry, 0x504Du);
    wr16(entry + 2u, 0u);
    wr32(entry + 4u, 2u);
    wr32(entry + 8u, start);
    wr32(entry + 12u, count);
    memcpy(entry + 16u, name, strlen(name));
    memcpy(entry + 48u, type, strlen(type));
    wr32(entry + 88u, 0x00000033u);         /* valid, allocated, readable */
}

int qrx_testimg_apm(const uint8_t *raw, uint32_t raw_len, uint8_t *out,
                    uint32_t cap, uint32_t *len)
{
    const uint32_t total = QRX_TESTIMG_APM_SIZE;
    const uint32_t blocks = total / SECTOR;
    const uint32_t volume_start = 64u;
    const uint32_t volume_blocks = (raw_len + SECTOR - 1u) / SECTOR;

    if (!raw || !out || cap < total || raw_len == 0u)
        return 0;
    if (volume_start + volume_blocks > blocks)
        return 0;
    memset(out, 0, total);
    wr16(out, 0x4552u);                     /* driver descriptor 'ER' */
    wr16(out + 2u, SECTOR);
    wr32(out + 4u, blocks);
    apm_entry(out, 1u, 1u, 2u, "Apple", "Apple_partition_map");
    apm_entry(out, 2u, volume_start, volume_blocks, "MacTest", "Apple_HFS");
    memcpy(out + volume_start * SECTOR, raw, raw_len);
    if (len)
        *len = total;
    return 1;
}

/* ------------------------------------------------------------------- HFS */

#define HFS_IMAGE_SIZE 819200u
#define HFS_ALLOC_START_SECTOR 20u
#define HFS_ALLOC_SIZE 512u
#define HFS_ALLOC_BLOCKS 1580u
#define HFS_NODE_SIZE 1024u

typedef struct {
    uint8_t *node;
    uint16_t size;
    uint16_t count;
    uint16_t pos;
    uint16_t offs[32];
} bt_builder;

static void hfs_bt_init(bt_builder *b, uint8_t *node, uint16_t size, uint8_t kind,
                        uint8_t height, uint32_t fwd, uint32_t back)
{
    memset(node, 0, size);
    wr32(node, fwd);
    wr32(node + 4u, back);
    node[8] = kind;
    node[9] = height;
    wr16(node + 10u, 0u);
    wr16(node + 12u, 0u);
    b->node = node;
    b->size = size;
    b->count = 0;
    b->pos = 14u;
}

static void hfs_bt_add(bt_builder *b, const uint8_t *key, uint16_t key_len,
                       const uint8_t *rec, uint16_t rec_len)
{
    if (b->count >= 32u || (uint32_t)b->pos + key_len + rec_len > b->size)
        return;
    b->offs[b->count++] = b->pos;
    if (key_len)
        memcpy(b->node + b->pos, key, key_len);
    b->pos = (uint16_t)(b->pos + key_len);
    memcpy(b->node + b->pos, rec, rec_len);
    b->pos = (uint16_t)(b->pos + rec_len);
}

static void hfs_bt_finish(bt_builder *b)
{
    uint16_t i;

    wr16(b->node + 10u, b->count);
    wr16(b->node + b->size - 2u, b->pos);
    for (i = 0; i < b->count; i++)
        wr16(b->node + b->size - 2u * (uint16_t)(b->count - i + 1u), b->offs[i]);
}

static void hfs_bt_header_node(uint8_t *node, uint16_t size, uint32_t root,
                               uint32_t leaf_records, uint32_t first_leaf,
                               uint32_t last_leaf, uint16_t max_key,
                               uint32_t total_nodes, uint16_t free_nodes)
{
    bt_builder b;
    uint8_t rec[128];
    uint8_t map[32];

    memset(rec, 0, sizeof(rec));
    wr16(rec, 1u);                       /* treeDepth */
    wr32(rec + 2u, root);
    wr32(rec + 6u, leaf_records);
    wr32(rec + 10u, first_leaf);
    wr32(rec + 14u, last_leaf);
    wr16(rec + 18u, size);
    wr16(rec + 20u, max_key);
    wr32(rec + 22u, total_nodes);
    wr32(rec + 26u, free_nodes);
    rec[30] = 0u;                        /* HFS B-tree (not HFS+) */

    memset(map, 0, sizeof(map));
    hfs_bt_init(&b, node, size, 1u, 0u, 0u, 0u);
    memcpy(node + 14u, rec, 106u);
    b.offs[b.count++] = 14u;
    b.pos = 120u;
    memset(node + 120u, 0, 128u);        /* user data record */
    b.offs[b.count++] = 120u;
    b.pos = 248u;
    memcpy(node + 248u, map, sizeof(map));  /* node map record */
    b.offs[b.count++] = 248u;
    b.pos = 280u;
    hfs_bt_finish(&b);
}

static uint16_t hfs_cat_key(uint8_t *out, uint32_t parent, const char *name)
{
    uint16_t len = (uint16_t)strlen(name);
    uint16_t total = (uint16_t)(7u + len);

    out[0] = (uint8_t)(total - 1u);      /* key length excludes itself */
    out[1] = 0u;
    wr32(out + 2u, parent);
    out[6] = (uint8_t)len;
    memcpy(out + 7u, name, len);
    if (total & 1u) {
        out[total] = 0u;
        total++;
        out[0] = (uint8_t)(total - 1u);
    }
    return total;
}

static uint16_t hfs_ext_key(uint8_t *out, uint8_t fork_type, uint32_t file_id,
                            uint16_t start)
{
    out[0] = 9u;
    out[1] = 0u;
    out[2] = fork_type;
    out[3] = 0u;
    wr32(out + 4u, file_id);
    wr16(out + 8u, start);
    return 10u;
}

static void hfs_write_extents(uint8_t *out, const uint16_t *ext, int pairs)
{
    int i;

    for (i = 0; i < pairs; i++) {
        wr16(out + (uint32_t)i * 4u, ext[i * 2]);
        wr16(out + (uint32_t)i * 4u + 2u, ext[i * 2 + 1]);
    }
}

static uint16_t hfs_thread_rec(uint8_t *out, uint32_t parent, const char *name)
{
    uint16_t len = (uint16_t)strlen(name);
    uint16_t total = (uint16_t)(9u + len);

    memset(out, 0, total + 1u);
    wr16(out, 3u);
    wr16(out + 2u, 0u);
    wr32(out + 4u, parent);
    out[8] = (uint8_t)len;
    memcpy(out + 9u, name, len);
    if (total & 1u)
        total++;
    return total;
}

static uint16_t hfs_folder_rec(uint8_t *out, uint16_t valence, uint32_t cnid,
                               uint16_t icon)
{
    memset(out, 0, 70u);
    wr16(out, 1u);
    wr16(out + 4u, valence);
    wr32(out + 6u, cnid);
    wr16(out + 38u, icon);
    return 70u;
}

static uint16_t hfs_file_rec(uint8_t *out, uint32_t cnid, const char *type,
                             const char *creator, uint16_t flags, uint16_t icon,
                             uint32_t data_len, const uint16_t *data_ext,
                             uint32_t rsrc_len, const uint16_t *rsrc_ext)
{
    memset(out, 0, 98u);
    wr16(out, 2u);
    memcpy(out + 4u, type, 4u);
    memcpy(out + 8u, creator, 4u);
    wr16(out + 12u, flags);
    wr16(out + 18u, 0u);
    wr32(out + 20u, cnid);
    wr16(out + 24u, data_ext ? data_ext[0] : 0u);
    wr32(out + 26u, data_len);
    wr32(out + 30u, ((data_len + HFS_ALLOC_SIZE - 1u) / HFS_ALLOC_SIZE) * HFS_ALLOC_SIZE);
    wr16(out + 34u, rsrc_ext ? rsrc_ext[0] : 0u);
    wr32(out + 36u, rsrc_len);
    wr32(out + 40u, ((rsrc_len + HFS_ALLOC_SIZE - 1u) / HFS_ALLOC_SIZE) * HFS_ALLOC_SIZE);
    wr16(out + 56u, icon);
    if (data_ext)
        hfs_write_extents(out + 74u, data_ext, 3);
    if (rsrc_ext)
        hfs_write_extents(out + 86u, rsrc_ext, 3);
    return 98u;
}

static uint32_t hfs_block_addr(uint32_t block)
{
    return HFS_ALLOC_START_SECTOR * SECTOR + block * HFS_ALLOC_SIZE;
}

static void hfs_bitmap_set(uint8_t *bitmap, uint32_t block)
{
    bitmap[block / 8u] |= (uint8_t)(0x80u >> (block % 8u));
}

int qrx_testimg_hfs800(uint8_t *out, uint32_t cap, uint32_t *len)
{
    static const char volume_name[] = "Test Disk";
    static const uint16_t data_hello[6] = { 12u, 1u, 0u, 0u, 0u, 0u };
    static const uint16_t data_payload[6] = { 13u, 3u, 0u, 0u, 0u, 0u };
    static const uint16_t rsrc_payload[6] = { 16u, 1u, 0u, 0u, 0u, 0u };
    static const uint16_t data_note[6] = { 17u, 2u, 0u, 0u, 0u, 0u };
    static const uint16_t data_frag[6] = { 19u, 1u, 21u, 1u, 23u, 1u };
    uint8_t *cat;
    uint8_t *xt;
    uint8_t *mdb;
    uint8_t key[40];
    uint8_t rec[128];
    bt_builder b;
    uint16_t klen;
    uint16_t rlen;
    uint32_t i;

    if (!out || cap < HFS_IMAGE_SIZE)
        return 0;
    memset(out, 0, HFS_IMAGE_SIZE);
    cat = out + (uint32_t)HFS_ALLOC_START_SECTOR * SECTOR;
    xt = out + (uint32_t)(HFS_ALLOC_START_SECTOR + 8u) * SECTOR;

    /* extents overflow tree: header node plus one leaf with Frag's 4th block */
    hfs_bt_header_node(xt, HFS_NODE_SIZE, 1u, 1u, 1u, 1u, 10u, 2u, 0u);
    hfs_bt_init(&b, xt + HFS_NODE_SIZE, HFS_NODE_SIZE, 0xFFu, 1u, 0u, 0u);
    {
        uint8_t er[12];

        memset(er, 0, sizeof(er));
        er[0] = 0u;
        er[1] = 25u;                     /* start block */
        er[2] = 0u;
        er[3] = 1u;                      /* one block */
        klen = hfs_ext_key(key, 0u, 5u, 25u);
        hfs_bt_add(&b, key, klen, er, 12u);
    }
    hfs_bt_finish(&b);

    /* catalog tree: header node, two leaves, one spare node */
    hfs_bt_header_node(cat, HFS_NODE_SIZE, 1u, 7u, 1u, 2u, 37u, 4u, 1u);

    hfs_bt_init(&b, cat + HFS_NODE_SIZE, HFS_NODE_SIZE, 0xFFu, 1u, 2u, 0u);
    klen = hfs_cat_key(key, 1u, "");
    rlen = hfs_thread_rec(rec, 1u, volume_name);
    hfs_bt_add(&b, key, klen, rec, rlen);
    klen = hfs_cat_key(key, 1u, "Docs");
    rlen = hfs_folder_rec(rec, 1u, 16u, 0u);
    hfs_bt_add(&b, key, klen, rec, rlen);
    klen = hfs_cat_key(key, 1u, "Frag");
    rlen = hfs_file_rec(rec, 5u, "BINA", "QRX+", 0u, 0u, 2048u, data_frag,
                        0u, NULL);
    hfs_bt_add(&b, key, klen, rec, rlen);
    klen = hfs_cat_key(key, 1u, "Hello");
    rlen = hfs_file_rec(rec, 2u, "TEXT", "ttxt", 0x0100u, 0u, 5u, data_hello,
                        0u, NULL);
    hfs_bt_add(&b, key, klen, rec, rlen);
    hfs_bt_finish(&b);

    hfs_bt_init(&b, cat + 2u * HFS_NODE_SIZE, HFS_NODE_SIZE, 0xFFu, 1u, 0u, 1u);
    klen = hfs_cat_key(key, 1u, "Payload");
    rlen = hfs_file_rec(rec, 3u, "BINA", "QRX+", 0u, 0u, 1300u, data_payload,
                        64u, rsrc_payload);
    hfs_bt_add(&b, key, klen, rec, rlen);
    klen = hfs_cat_key(key, 16u, "Docs");
    rlen = hfs_thread_rec(rec, 1u, "Docs");
    hfs_bt_add(&b, key, klen, rec, rlen);
    klen = hfs_cat_key(key, 16u, "Note");
    rlen = hfs_file_rec(rec, 17u, "TEXT", "ttxt", 0u, 0u, 700u, data_note,
                        0u, NULL);
    hfs_bt_add(&b, key, klen, rec, rlen);
    hfs_bt_finish(&b);

    /* file contents */
    memcpy(out + hfs_block_addr(12u), "Hello", 5u);
    for (i = 0; i < 1300u; i++)
        out[hfs_block_addr(13u) + i] = qrx_testimg_payload_byte(i);
    for (i = 0; i < 64u; i++)
        out[hfs_block_addr(16u) + i] = qrx_testimg_rsrc_byte(i);
    for (i = 0; i < 700u; i++)
        out[hfs_block_addr(17u) + i] = qrx_testimg_rsrc_byte(i + 8u);
    for (i = 0; i < 2048u; i++) {
        uint32_t block;
        if (i < 512u)
            block = 19u;
        else if (i < 1024u)
            block = 21u;
        else if (i < 1536u)
            block = 23u;
        else
            block = 25u;
        out[hfs_block_addr(block) + (i % 512u)] = qrx_testimg_payload_byte(i);
    }

    /* volume bitmap: catalog, extents tree, files and Frag's scattered blocks */
    for (i = 0; i <= 19u; i++)
        hfs_bitmap_set(out + 4u * SECTOR, i);
    hfs_bitmap_set(out + 4u * SECTOR, 21u);
    hfs_bitmap_set(out + 4u * SECTOR, 23u);
    hfs_bitmap_set(out + 4u * SECTOR, 25u);

    mdb = out + MDB_OFFSET;
    wr16(mdb, 0x4244u);
    wr32(mdb + 2u, 0u);
    wr32(mdb + 6u, 0u);
    wr16(mdb + 10u, 0u);
    wr16(mdb + 12u, 3u);
    wr16(mdb + 14u, 4u);                 /* volume bitmap start */
    wr16(mdb + 16u, 23u);                /* allocation pointer */
    wr16(mdb + 18u, HFS_ALLOC_BLOCKS);
    wr32(mdb + 20u, HFS_ALLOC_SIZE);
    wr32(mdb + 24u, 4096u);
    wr16(mdb + 28u, HFS_ALLOC_START_SECTOR);
    wr32(mdb + 30u, 18u);
    wr16(mdb + 34u, 1557u);
    mdb[36] = (uint8_t)(sizeof(volume_name) - 1u);
    memcpy(mdb + 37u, volume_name, sizeof(volume_name) - 1u);
    wr32(mdb + 74u, 4096u);
    wr32(mdb + 78u, 4096u);
    wr16(mdb + 82u, 1u);
    wr32(mdb + 84u, 4u);   /* files on the volume */
    wr32(mdb + 88u, 2u);
    wr32(mdb + 130u, 2048u);
    wr16(mdb + 134u, 8u);
    wr16(mdb + 136u, 4u);
    wr32(mdb + 146u, 4096u);
    wr16(mdb + 150u, 0u);
    wr16(mdb + 152u, 8u);

    if (len)
        *len = HFS_IMAGE_SIZE;
    return 1;
}

/* ------------------------------------------------------------------ HFS+ */

#define HFSP_IMAGE_SIZE 2097152u
#define HFSP_WRAP_OFFSET 4096u
#define HFSP_BLOCK_SIZE 1024u
#define HFSP_TOTAL_BLOCKS 2048u
#define HFSP_USED_BLOCKS 26u
#define HFSP_NODE_SIZE 1024u

static void hfsp_wr64(uint8_t *out, uint64_t value)
{
    wr32(out, (uint32_t)(value >> 32));
    wr32(out + 4u, (uint32_t)value);
}

static uint16_t hfsp_units(const char *in, uint8_t *out)
{
    const unsigned char *p = (const unsigned char *)in;
    uint16_t n = 0;

    while (*p) {
        wr16(out + (uint32_t)n * 2u, *p);
        n++;
        p++;
    }
    return n;
}

static void hfsp_bt_header_node(uint8_t *node, uint16_t size, uint32_t root,
                                uint32_t leaf_records, uint32_t first_leaf,
                                uint32_t last_leaf, uint16_t max_key,
                                uint32_t total_nodes, uint16_t free_nodes,
                                uint8_t compare_type)
{
    bt_builder b;
    uint8_t rec[106];
    uint8_t map[32];

    memset(rec, 0, sizeof(rec));
    wr16(rec, 1u);
    wr32(rec + 2u, root);
    wr32(rec + 6u, leaf_records);
    wr32(rec + 10u, first_leaf);
    wr32(rec + 14u, last_leaf);
    wr16(rec + 18u, size);
    wr16(rec + 20u, max_key);
    wr32(rec + 22u, total_nodes);
    wr32(rec + 26u, free_nodes);
    rec[32] = 0u;                        /* btreeType: HFS+ */
    rec[33] = compare_type;              /* 0 = binary, 0xBC = case-sensitive */

    memset(map, 0, sizeof(map));
    hfs_bt_init(&b, node, size, 1u, 0u, 0u, 0u);
    memcpy(node + 14u, rec, sizeof(rec));
    b.offs[b.count++] = 14u;
    b.pos = 126u;
    memset(node + 126u, 0, 128u);        /* user data record */
    b.offs[b.count++] = 126u;
    b.pos = 254u;
    memcpy(node + 254u, map, sizeof(map));  /* node map record */
    b.offs[b.count++] = 254u;
    b.pos = 286u;
    hfs_bt_finish(&b);
}

static uint16_t hfsp_cat_key(uint8_t *out, uint32_t parent, const uint8_t *units,
                             uint16_t count)
{
    uint16_t total = (uint16_t)(8u + 2u * count);

    wr16(out, (uint16_t)(6u + 2u * count));
    wr32(out + 2u, parent);
    wr16(out + 6u, count);
    if (count)
        memcpy(out + 8u, units, (size_t)count * 2u);
    return total;
}

static uint16_t hfsp_ext_key(uint8_t *out, uint8_t fork_type, uint32_t file_id,
                             uint32_t start)
{
    wr16(out, 10u);
    out[2] = fork_type;
    out[3] = 0u;
    wr32(out + 4u, file_id);
    wr32(out + 8u, start);
    return 12u;
}

static uint16_t hfsp_attr_key(uint8_t *out, uint32_t file_id,
                              const uint8_t *units, uint16_t count)
{
    wr16(out, (uint16_t)(12u + 2u * count));
    wr16(out + 2u, 0u);
    wr32(out + 4u, file_id);
    wr32(out + 8u, 0u);
    wr16(out + 12u, count);
    if (count)
        memcpy(out + 14u, units, (size_t)count * 2u);
    return (uint16_t)(14u + 2u * count);
}

static uint16_t hfsp_thread_rec(uint8_t *out, uint32_t parent,
                                const uint8_t *units, uint16_t count)
{
    wr16(out, 3u);
    wr16(out + 2u, 0u);
    wr32(out + 4u, parent);
    wr16(out + 8u, count);
    if (count)
        memcpy(out + 10u, units, (size_t)count * 2u);
    return (uint16_t)(10u + 2u * count);
}

static uint16_t hfsp_folder_rec(uint8_t *out, uint32_t cnid, uint32_t valence)
{
    memset(out, 0, 88u);
    wr16(out, 1u);
    wr32(out + 4u, valence);
    wr32(out + 8u, cnid);
    return 88u;
}

static void hfsp_fork_data(uint8_t *out, uint64_t logical,
                           const uint32_t *ext, int pairs)
{
    uint32_t blocks = 0;
    int i;

    memset(out, 0, 80u);
    hfsp_wr64(out, logical);
    for (i = 0; i < pairs; i++) {
        wr32(out + 16u + (uint32_t)i * 8u, ext[i * 2]);
        wr32(out + 16u + (uint32_t)i * 8u + 4u, ext[i * 2 + 1]);
        blocks += ext[i * 2 + 1];
    }
    wr32(out + 12u, blocks);
}

static uint16_t hfsp_file_rec(uint8_t *out, uint32_t cnid, const char *type,
                              const char *creator, uint16_t fd_flags,
                              uint64_t data_len, const uint32_t *data_ext,
                              int data_pairs, uint64_t rsrc_len,
                              const uint32_t *rsrc_ext, int rsrc_pairs)
{
    memset(out, 0, 248u);
    wr16(out, 2u);
    wr32(out + 8u, cnid);
    memcpy(out + 48u, type, 4u);
    memcpy(out + 52u, creator, 4u);
    wr16(out + 56u, fd_flags);
    hfsp_fork_data(out + 88u, data_len, data_ext, data_pairs);
    hfsp_fork_data(out + 168u, rsrc_len, rsrc_ext, rsrc_pairs);
    return 248u;
}

static void hfsp_vh(uint8_t *out, int hfsx)
{
    uint8_t *vh = out + MDB_OFFSET;
    const uint32_t alloc_bitmap[2] = { 2u, 1u };
    const uint32_t extents_fork[2] = { 3u, 2u };
    const uint32_t catalog_fork[2] = { 5u, 4u };
    const uint32_t attributes_fork[2] = { 33u, 2u };

    memset(vh, 0, 512u);
    wr16(vh, hfsx ? 0x4858u : 0x482Bu);
    wr16(vh + 2u, hfsx ? 5u : 4u);
    wr32(vh + 4u, 0u);
    wr32(vh + 8u, 0x31302E30u);          /* "10.0" as last-mounted version */
    wr32(vh + 12u, 0u);                  /* journalInfoBlock: not journaled */
    wr32(vh + 16u, 0xC0000000u);
    wr32(vh + 20u, 0xC0000000u);
    wr32(vh + 24u, 0u);
    wr32(vh + 28u, 0xC0000000u);
    wr32(vh + 32u, 5u);                  /* fileCount */
    wr32(vh + 36u, 2u);                  /* folderCount */
    wr32(vh + 40u, HFSP_BLOCK_SIZE);
    wr32(vh + 44u, HFSP_TOTAL_BLOCKS);
    wr32(vh + 48u, HFSP_TOTAL_BLOCKS - HFSP_USED_BLOCKS);
    wr32(vh + 52u, 34u);                 /* nextAllocation */
    wr32(vh + 64u, 18u);                 /* nextCatalogID */
    hfsp_fork_data(vh + 112u, 256u, alloc_bitmap, 1);
    hfsp_fork_data(vh + 192u, 2048u, extents_fork, 1);
    hfsp_fork_data(vh + 272u, 4096u, catalog_fork, 1);
    hfsp_fork_data(vh + 352u, 2048u, attributes_fork, 1);
}

int qrx_testimg_hfsp(uint8_t *out, uint32_t cap, uint32_t *len, int hfsx)
{
    static const uint32_t data_hello[2] = { 9u, 1u };
    static const uint32_t data_payload[2] = { 10u, 2u };
    static const uint32_t rsrc_payload[2] = { 12u, 1u };
    static const uint32_t data_note[2] = { 13u, 1u };
    static const uint32_t data_frag[16] = {
        15u, 1u, 17u, 1u, 19u, 1u, 21u, 1u, 23u, 1u, 25u, 1u, 27u, 1u, 29u, 1u
    };
    static const char volume_name[] = "Test Disk";
    uint8_t *cat = out + 5u * HFSP_BLOCK_SIZE;
    uint8_t *xt = out + 3u * HFSP_BLOCK_SIZE;
    uint8_t *at = out + 33u * HFSP_BLOCK_SIZE;
    uint8_t units[64];
    uint8_t key[128];
    uint8_t rec[256];
    bt_builder b;
    uint16_t klen;
    uint16_t rlen;
    uint16_t count;
    uint32_t i;
    uint16_t record;

    if (!out || cap < HFSP_IMAGE_SIZE)
        return 0;
    memset(out, 0, HFSP_IMAGE_SIZE);

    /* extents overflow tree: header node plus Frag's ninth block */
    hfsp_bt_header_node(xt, HFSP_NODE_SIZE, 1u, 1u, 1u, 1u, 10u, 2u, 0u, 0u);
    hfs_bt_init(&b, xt + HFSP_NODE_SIZE, HFSP_NODE_SIZE, 0xFFu, 1u, 0u, 0u);
    {
        uint8_t er[64];

        memset(er, 0, sizeof(er));
        wr32(er, 31u);
        wr32(er + 4u, 1u);
        klen = hfsp_ext_key(key, 0u, 5u, 31u);
        hfs_bt_add(&b, key, klen, er, 64u);
    }
    hfs_bt_finish(&b);

    /* attributes tree: Café's resource fork as com.apple.ResourceFork */
    hfsp_bt_header_node(at, HFSP_NODE_SIZE, 1u, 1u, 1u, 1u, 56u, 2u, 0u, 0u);
    hfs_bt_init(&b, at + HFSP_NODE_SIZE, HFSP_NODE_SIZE, 0xFFu, 1u, 0u, 0u);
    {
        uint8_t ar[80];
        uint16_t attr_units = hfsp_units("com.apple.ResourceFork", units);

        memset(ar, 0, sizeof(ar));
        wr32(ar, 0x20u);                 /* fork-data attribute */
        wr32(ar + 8u, 40u);              /* logical size */
        wr32(ar + 16u, 14u);
        wr32(ar + 20u, 1u);
        klen = hfsp_attr_key(key, 6u, units, attr_units);
        hfs_bt_add(&b, key, klen, ar, 80u);
    }
    hfs_bt_finish(&b);

    /* catalog tree: header node, two leaves, one spare node */
    record = hfsx ? 9u : 8u;
    hfsp_bt_header_node(cat, HFSP_NODE_SIZE, 1u, record, 1u, 2u, 40u, 4u, 1u,
                        hfsx ? 0xBCu : 0u);

    hfs_bt_init(&b, cat + HFSP_NODE_SIZE, HFSP_NODE_SIZE, 0xFFu, 1u, 2u, 0u);
    count = hfsp_units("Caf\xE9", units);
    klen = hfsp_cat_key(key, 2u, units, count);
    rlen = hfsp_file_rec(rec, 6u, "TEXT", "ttxt", 0u, 0u, NULL, 0, 0u, NULL, 0);
    hfs_bt_add(&b, key, klen, rec, rlen);
    count = hfsp_units("Docs", units);
    klen = hfsp_cat_key(key, 2u, units, count);
    rlen = hfsp_folder_rec(rec, 16u, 1u);
    hfs_bt_add(&b, key, klen, rec, rlen);
    count = hfsp_units("Frag", units);
    klen = hfsp_cat_key(key, 2u, units, count);
    rlen = hfsp_file_rec(rec, 5u, "BINA", "QRX+", 0u, 9000u, data_frag, 8, 0u,
                         NULL, 0);
    hfs_bt_add(&b, key, klen, rec, rlen);
    count = hfsp_units("Hello", units);
    klen = hfsp_cat_key(key, 2u, units, count);
    rlen = hfsp_file_rec(rec, 2u, "TEXT", "ttxt", 0x0100u, 5u, data_hello, 1,
                         0u, NULL, 0);
    hfs_bt_add(&b, key, klen, rec, rlen);
    hfs_bt_finish(&b);

    hfs_bt_init(&b, cat + 2u * HFSP_NODE_SIZE, HFSP_NODE_SIZE, 0xFFu, 1u, 0u, 1u);
    count = hfsp_units("Payload", units);
    klen = hfsp_cat_key(key, 2u, units, count);
    rlen = hfsp_file_rec(rec, 3u, "BINA", "QRX+", 0u, 1300u, data_payload, 1,
                         64u, rsrc_payload, 1);
    hfs_bt_add(&b, key, klen, rec, rlen);
    count = hfsp_units(volume_name, units);
    klen = hfsp_cat_key(key, 2u, units, count);
    rlen = hfsp_thread_rec(rec, 2u, units, count);
    hfs_bt_add(&b, key, klen, rec, rlen);
    count = hfsp_units("Docs", units);
    klen = hfsp_cat_key(key, 16u, units, count);
    rlen = hfsp_thread_rec(rec, 2u, units, count);
    hfs_bt_add(&b, key, klen, rec, rlen);
    count = hfsp_units("Note", units);
    klen = hfsp_cat_key(key, 16u, units, count);
    rlen = hfsp_file_rec(rec, 17u, "TEXT", "ttxt", 0u, 700u, data_note, 1, 0u,
                         NULL, 0);
    hfs_bt_add(&b, key, klen, rec, rlen);
    if (hfsx) {
        count = hfsp_units("hello", units);
        klen = hfsp_cat_key(key, 2u, units, count);
        rlen = hfsp_file_rec(rec, 7u, "TEXT", "ttxt", 0u, 0u, NULL, 0, 0u, NULL,
                             0);
        hfs_bt_add(&b, key, klen, rec, rlen);
    }
    hfs_bt_finish(&b);

    /* file contents */
    memcpy(out + 9u * HFSP_BLOCK_SIZE, "Hello", 5u);
    for (i = 0; i < 1300u; i++)
        out[10u * HFSP_BLOCK_SIZE + i] = qrx_testimg_payload_byte(i);
    for (i = 0; i < 64u; i++)
        out[12u * HFSP_BLOCK_SIZE + i] = qrx_testimg_rsrc_byte(i);
    for (i = 0; i < 700u; i++)
        out[13u * HFSP_BLOCK_SIZE + i] = qrx_testimg_rsrc_byte(i + 8u);
    for (i = 0; i < 40u; i++)
        out[14u * HFSP_BLOCK_SIZE + i] = qrx_testimg_rsrc_byte(i + 64u);
    for (i = 0; i < 9000u; i++) {
        uint32_t block = (i < 8192u) ? 15u + 2u * (i / HFSP_BLOCK_SIZE) : 31u;

        out[(uint64_t)block * HFSP_BLOCK_SIZE + (i % HFSP_BLOCK_SIZE)] =
            qrx_testimg_payload_byte(i);
    }

    /* allocation bitmap */
    for (i = 0; i <= 14u; i++)
        hfs_bitmap_set(out + 2u * HFSP_BLOCK_SIZE, i);
    for (i = 15u; i <= 31u; i += 2u)
        hfs_bitmap_set(out + 2u * HFSP_BLOCK_SIZE, i);
    hfs_bitmap_set(out + 2u * HFSP_BLOCK_SIZE, 33u);
    hfs_bitmap_set(out + 2u * HFSP_BLOCK_SIZE, 34u);

    hfsp_vh(out, hfsx);

    if (len)
        *len = HFSP_IMAGE_SIZE;
    return 1;
}

int qrx_testimg_hfsp_wrapper(const uint8_t *hfsp, uint32_t hfsp_len, uint8_t *out,
                             uint32_t cap, uint32_t *len)
{
    const uint32_t total = HFSP_WRAP_OFFSET + HFSP_IMAGE_SIZE;
    uint8_t *mdb;

    if (!hfsp || !out || cap < total || hfsp_len > HFSP_IMAGE_SIZE)
        return 0;
    memset(out, 0, total);
    memcpy(out + HFSP_WRAP_OFFSET, hfsp, hfsp_len);

    /* HFS wrapper: allocation blocks of 512 bytes starting at sector 8. */
    mdb = out + MDB_OFFSET;
    wr16(mdb, 0x4244u);
    wr16(mdb + 12u, 1u);
    wr16(mdb + 14u, 4u);
    wr16(mdb + 18u, 4096u);              /* drNmAlBlks for 2 MiB of 512 B */
    wr32(mdb + 20u, 512u);               /* drAlBlkSiz */
    wr16(mdb + 28u, 8u);                 /* drAlBlSt */
    wr32(mdb + 30u, 16u);
    mdb[36] = 9u;
    memcpy(mdb + 37u, "Test Disk", 9u);
    wr16(mdb + 124u, 0x482Bu);           /* drEmbedSigWord: HFS+ inside */
    wr16(mdb + 126u, 0u);                /* drEmbedExtent.startBlock */
    wr16(mdb + 128u, 4096u);             /* drEmbedExtent.blockCount */

    if (len)
        *len = total;
    return 1;
}

/* ------------------------------------------------------- fork containers */

#define FORK_TEST_CAP 8192u
#define FORK_BINHEX_HEADER 22u

static const char fork_binhex_alphabet[] =
    "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";

static uint16_t fork_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0;
    uint32_t i;
    int bit;

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }
    return crc;
}

/* Bytes of the BinHex test file's data fork: runs, a literal 0x90, payload. */
uint8_t qrx_testimg_binhex_byte(uint32_t index)
{
    if (index < 10u)
        return 'A';
    if (index < 15u)
        return 0x90u;
    return qrx_testimg_payload_byte(index - 15u);
}

static void fork_put_name(uint8_t *out, const char *name, uint8_t *len)
{
    uint8_t n = (uint8_t)strlen(name);

    out[0] = n;
    memcpy(out + 1u, name, n);
    *len = n;
}

int qrx_testimg_applesingle(uint8_t *out, uint32_t cap, uint32_t *len)
{
    static const uint32_t ids[5] = { 1u, 2u, 3u, 7u, 8u };
    const uint32_t data_len = 5u;
    const uint32_t rsrc_len = 64u;
    const uint32_t header = 26u + 5u * 12u;
    uint32_t offsets[5];
    uint8_t name_len;
    uint32_t i;

    if (!out || cap < header + data_len + rsrc_len + 6u + 32u)
        return 0;
    memset(out, 0, header + data_len + rsrc_len + 6u + 32u);
    wr32(out, 0x00051600u);
    wr32(out + 4u, 0x00020000u);
    wr16(out + 24u, 5u);
    offsets[0] = header;
    offsets[1] = offsets[0] + data_len;
    offsets[2] = offsets[1] + rsrc_len;
    offsets[3] = offsets[2] + 6u;
    offsets[4] = offsets[3] + 16u;
    wr32(out + 26u, ids[0]);
    wr32(out + 30u, offsets[0]);
    wr32(out + 34u, data_len);
    wr32(out + 38u, ids[1]);
    wr32(out + 42u, offsets[1]);
    wr32(out + 46u, rsrc_len);
    wr32(out + 50u, ids[2]);
    wr32(out + 54u, offsets[2]);
    wr32(out + 58u, 6u);
    wr32(out + 62u, ids[3]);
    wr32(out + 66u, offsets[3]);
    wr32(out + 70u, 16u);
    wr32(out + 74u, ids[4]);
    wr32(out + 78u, offsets[4]);
    wr32(out + 82u, 16u);

    memcpy(out + offsets[0], "Hello", data_len);
    for (i = 0; i < rsrc_len; i++)
        out[offsets[1] + i] = qrx_testimg_rsrc_byte(i);
    fork_put_name(out + offsets[2], "Hello", &name_len);
    memcpy(out + offsets[3], "TEXT", 4u);
    memcpy(out + offsets[3] + 4u, "ttxt", 4u);
    wr16(out + offsets[3] + 8u, 0x0100u);
    wr32(out + offsets[3] + 10u, 0x00400080u);
    wr16(out + offsets[3] + 14u, 0u);
    wr32(out + offsets[4], 0xC0000000u);
    wr32(out + offsets[4] + 4u, 0xC0000100u);

    if (len)
        *len = offsets[4] + 16u;
    return 1;
}

int qrx_testimg_appledouble(uint8_t *out, uint32_t cap, uint32_t *len)
{
    static const uint32_t ids[4] = { 2u, 3u, 7u, 8u };
    const uint32_t rsrc_len = 64u;
    const uint32_t header = 26u + 4u * 12u;
    uint32_t offsets[4];
    uint8_t name_len;
    uint32_t i;

    if (!out || cap < header + rsrc_len + 8u + 32u)
        return 0;
    memset(out, 0, header + rsrc_len + 8u + 32u);
    wr32(out, 0x00051607u);
    wr32(out + 4u, 0x00020000u);
    wr16(out + 24u, 4u);
    offsets[0] = header;
    offsets[1] = offsets[0] + rsrc_len;
    offsets[2] = offsets[1] + 8u;
    offsets[3] = offsets[2] + 16u;
    wr32(out + 26u, ids[0]);
    wr32(out + 30u, offsets[0]);
    wr32(out + 34u, rsrc_len);
    wr32(out + 38u, ids[1]);
    wr32(out + 42u, offsets[1]);
    wr32(out + 46u, 8u);
    wr32(out + 50u, ids[2]);
    wr32(out + 54u, offsets[2]);
    wr32(out + 58u, 16u);
    wr32(out + 62u, ids[3]);
    wr32(out + 66u, offsets[3]);
    wr32(out + 70u, 16u);

    for (i = 0; i < rsrc_len; i++)
        out[offsets[0] + i] = qrx_testimg_rsrc_byte(i);
    fork_put_name(out + offsets[1], "Payload", &name_len);
    memcpy(out + offsets[2], "BINA", 4u);
    memcpy(out + offsets[2] + 4u, "QRX+", 4u);
    wr16(out + offsets[2] + 8u, 0x0080u);
    wr32(out + offsets[2] + 10u, 0x00200040u);
    wr16(out + offsets[2] + 14u, 0u);
    wr32(out + offsets[3], 0xC0000200u);
    wr32(out + offsets[3] + 4u, 0xC0000300u);

    if (len)
        *len = offsets[3] + 16u;
    return 1;
}

int qrx_testimg_macbinary(uint8_t *out, uint32_t cap, uint32_t *len)
{
    const uint32_t data_len = 260u;
    const uint32_t rsrc_len = 64u;
    const uint32_t data_off = 128u;
    const uint32_t rsrc_off = data_off + ((data_len + 127u) / 128u) * 128u;
    const uint32_t total = rsrc_off + rsrc_len;
    uint16_t crc;
    uint32_t i;

    if (!out || cap < total)
        return 0;
    memset(out, 0, total);
    out[0] = 0u;
    fork_put_name(out + 1u, "Payload", &out[1]);
    memcpy(out + 65u, "BINA", 4u);
    memcpy(out + 69u, "QRX+", 4u);
    out[73] = 0x01u;                     /* Finder flags 0x0100 */
    wr16(out + 75u, 0x0040u);
    wr16(out + 77u, 0x0080u);
    wr16(out + 79u, 0u);
    wr32(out + 81u, data_len);
    wr32(out + 85u, rsrc_len);
    wr32(out + 89u, 0xC0000000u);
    wr32(out + 93u, 0xC0000100u);
    out[124] = 0x81u;                    /* MacBinary II marker */
    crc = fork_crc16(out, 125u);
    wr16(out + 125u, crc);
    out[127] = 0u;

    for (i = 0; i < data_len; i++)
        out[data_off + i] = qrx_testimg_payload_byte(i);
    for (i = 0; i < rsrc_len; i++)
        out[rsrc_off + i] = qrx_testimg_rsrc_byte(i);

    if (len)
        *len = total;
    return 1;
}

/* 0x90 0x00 is a literal 0x90; runs of four or more become byte, 0x90, n. */
static uint32_t fork_binhex_rle(const uint8_t *in, uint32_t len, uint8_t *out,
                                uint32_t cap)
{
    uint32_t i = 0;
    uint32_t n = 0;

    while (i < len) {
        uint8_t byte = in[i];
        uint32_t run = 1;

        if (byte == 0x90u) {
            if (n + 2u > cap)
                return 0;
            out[n++] = 0x90u;
            out[n++] = 0x00u;
            i++;
            continue;
        }
        while (i + run < len && in[i + run] == byte && run < 256u)
            run++;
        if (run >= 4u) {
            if (n + 3u > cap)
                return 0;
            out[n++] = byte;
            out[n++] = 0x90u;
            out[n++] = (uint8_t)(run - 1u);
            i += run;
        } else {
            if (n + 1u > cap)
                return 0;
            out[n++] = byte;
            i++;
        }
    }
    return n;
}

int qrx_testimg_binhex(uint8_t *out, uint32_t cap, uint32_t *len)
{
    static const char banner[] =
        "(This file must be converted with BinHex 4.0)\r\n:\r\n";
    uint8_t decoded[FORK_TEST_CAP];
    uint8_t packed[FORK_TEST_CAP];
    const uint32_t data_len = 260u;
    const uint32_t rsrc_len = 64u;
    uint32_t header_len;
    uint32_t packed_len;
    uint32_t n;
    uint32_t i;
    uint32_t acc = 0;
    int bits = 0;
    uint8_t name_len;
    uint32_t pos;

    if (!out || cap < 512u)
        return 0;
    memset(decoded, 0, sizeof(decoded));
    fork_put_name(decoded, "Runs", &name_len);
    header_len = (uint32_t)name_len + FORK_BINHEX_HEADER;
    pos = 1u + name_len;
    decoded[pos] = 0u;                   /* version */
    memcpy(decoded + pos + 1u, "BINA", 4u);
    memcpy(decoded + pos + 5u, "QRX+", 4u);
    wr16(decoded + pos + 9u, 0u);
    wr32(decoded + pos + 11u, data_len);
    wr32(decoded + pos + 15u, rsrc_len);
    wr16(decoded + pos + 19u, 0u);       /* header CRC, not checked */
    for (i = 0; i < data_len; i++)
        decoded[header_len + i] = qrx_testimg_binhex_byte(i);
    for (i = 0; i < rsrc_len; i++)
        decoded[header_len + data_len + i] = qrx_testimg_rsrc_byte(i);
    decoded[header_len + data_len + rsrc_len] = 0u;
    decoded[header_len + data_len + rsrc_len + 1u] = 0u;
    n = header_len + data_len + rsrc_len + 2u;

    packed_len = fork_binhex_rle(decoded, n, packed, sizeof(packed));
    if (packed_len == 0u)
        return 0;

    memcpy(out, banner, sizeof(banner) - 1u);
    pos = (uint32_t)sizeof(banner) - 1u;
    for (i = 0; i < packed_len + 1u; i++) {
        acc = (acc << 8) | (i < packed_len ? packed[i] : 0u);
        bits += 8;
        while (bits >= 6) {
            char c = fork_binhex_alphabet[(acc >> (bits - 6)) & 0x3Fu];

            out[pos++] = (uint8_t)c;
            bits -= 6;
            if ((pos % 64u) == 0u)
                out[pos++] = '\r';
        }
    }
    out[pos++] = ':';
    out[pos++] = '\r';
    out[pos++] = '\n';
    memcpy(out + pos, "synthetic\r\n", 11u);
    pos += 11u;

    if (len)
        *len = pos;
    return 1;
}
