/*
 * QRXDSK - join a StuffIt archive split over two HFS floppy images.
 *
 * The two 1.44 MB HFS volumes contain DropStuff 5.5.sit.1 and
 * DropStuff 5.5.sit.2.  The first segment has a 426-byte split header,
 * every following segment has a 100-byte split header.  The HFS catalog
 * is read directly; no filesystem mount and no host tool are involved.
 */
#include <exec/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define SEG1_SKIP 426u
#define SEG2_SKIP 100u
#define MAX_CATALOG (8u * 1024u * 1024u)
#define MAX_SEGMENT (8u * 1024u * 1024u)

typedef struct HfsFile {
    uint8_t *data;
    uint32_t size;
} HfsFile;

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int read_at(BPTR f, uint32_t off, void *buf, uint32_t len)
{
    if (Seek(f, (LONG)off, OFFSET_BEGINNING) < 0)
        return 0;
    return Read(f, buf, (LONG)len) == (LONG)len;
}

static int write_all(BPTR f, const uint8_t *p, uint32_t len)
{
    while (len) {
        LONG chunk = len > 32768u ? 32768L : (LONG)len;
        if (Write(f, p, chunk) != chunk)
            return 0;
        p += chunk;
        len -= (uint32_t)chunk;
    }
    return 1;
}

static int ends_with(const uint8_t *s, uint32_t n, const char *suffix)
{
    uint32_t slen = (uint32_t)strlen(suffix);
    return n >= slen && memcmp(s + n - slen, suffix, slen) == 0;
}

/*
 * Find a regular file in the HFS catalog B-tree and read its data fork.
 * Only the three inline extents of the catalog file are used; that is
 * sufficient for the 1.44 MB test volumes.
 */
static int hfs_extract(BPTR dsk, const char *suffix, HfsFile *out)
{
    uint8_t mdb[512];
    uint8_t *cat = NULL;
    uint32_t alloc_size, alloc_start, cat_size, done;
    uint16_t ext_start[3], ext_num[3];
    uint32_t node_size, leaf;
    uint32_t guard;
    int i;

    out->data = NULL;
    out->size = 0;

    if (!read_at(dsk, 1024, mdb, sizeof(mdb)))
        return 0;
    if (mdb[0] != 'B' || mdb[1] != 'D')
        return 0;

    alloc_size = be32(mdb + 20);
    alloc_start = be16(mdb + 28);
    cat_size = be32(mdb + 146);
    if (alloc_size < 512 || (alloc_size & (alloc_size - 1)) ||
        alloc_size > 65536 || cat_size < 512 || cat_size > MAX_CATALOG)
        return 0;

    for (i = 0; i < 3; i++) {
        ext_start[i] = be16(mdb + 150 + i * 4);
        ext_num[i] = be16(mdb + 152 + i * 4);
    }

    cat = (uint8_t *)AllocMem(cat_size, MEMF_ANY);
    if (!cat)
        return 0;

    done = 0;
    for (i = 0; i < 3 && done < cat_size; i++) {
        uint32_t bytes = (uint32_t)ext_num[i] * alloc_size;
        uint32_t chunk = bytes;
        uint64_t off;

        if (chunk > cat_size - done)
            chunk = cat_size - done;
        if (chunk == 0)
            continue;
        off = ((uint64_t)ext_start[i] * (alloc_size / 512) +
               alloc_start) * 512;
        if (off > 0xffffffffu ||
            !read_at(dsk, (uint32_t)off, cat + done, chunk))
            goto fail;
        done += chunk;
    }
    if (done != cat_size || cat[8] != 1)
        goto fail;

    node_size = be16(cat + 32);
    leaf = be32(cat + 24);
    if (node_size < 512 || node_size > 32768 || cat_size < node_size)
        goto fail;

    guard = cat_size / node_size + 1;
    while (leaf && guard--) {
        uint32_t node_off = leaf * node_size;
        uint8_t *node;
        uint16_t nrec, r;

        if (node_off > cat_size || node_size > cat_size - node_off)
            goto fail;
        node = cat + node_off;
        if (node[8] != 0xff)
            goto fail;
        nrec = be16(node + 10);
        if (nrec > (node_size - 14) / 2)
            goto fail;

        for (r = 0; r < nrec; r++) {
            uint16_t start = be16(node + node_size - 2 * (r + 1));
            uint16_t end = be16(node + node_size - 2 * (r + 2));
            uint8_t keylen, namelen, type;
            uint16_t dataoff, datalen;
            uint32_t size, got;
            uint8_t *data;
            int e;

            if (start < 14 || end > node_size || start >= end)
                continue;
            keylen = node[start];
            if (keylen < 7 || (uint32_t)start + 1 + keylen > end)
                continue;
            namelen = node[start + 6];
            if (namelen == 0 || (uint32_t)start + 7 + namelen > end)
                continue;
            if (!ends_with(node + start + 7, namelen, suffix))
                continue;

            dataoff = (uint16_t)(start + 1 + keylen);
            if (dataoff >= end)
                continue;
            type = node[dataoff];
            if (type != 2)
                continue;
            datalen = (uint16_t)(end - dataoff);
            if (datalen < 78)
                continue;
            size = be32(node + dataoff + 26);
            if (size == 0 || size > MAX_SEGMENT)
                continue;

            data = (uint8_t *)AllocMem(size, MEMF_ANY);
            if (!data)
                goto fail;
            got = 0;
            for (e = 0; e < 3 && got < size; e++) {
                uint16_t es = be16(node + dataoff + 74 + e * 4);
                uint16_t en = be16(node + dataoff + 76 + e * 4);
                uint32_t bytes = (uint32_t)en * alloc_size;
                uint64_t off;

                if (bytes > size - got)
                    bytes = size - got;
                if (bytes == 0)
                    continue;
                off = ((uint64_t)es * (alloc_size / 512) +
                       alloc_start) * 512;
                if (off > 0xffffffffu ||
                    !read_at(dsk, (uint32_t)off, data + got, bytes)) {
                    FreeMem(data, size);
                    goto fail;
                }
                got += bytes;
            }
            if (got != size) {
                FreeMem(data, size);
                continue;
            }
            out->data = data;
            out->size = size;
            FreeMem(cat, cat_size);
            return 1;
        }
        leaf = be32(node);
    }

fail:
    FreeMem(cat, cat_size);
    return 0;
}

int main(int argc, char **argv)
{
    BPTR d1 = 0, d2 = 0, out = 0;
    HfsFile seg1, seg2;
    int rc = 20;

    if (argc != 4) {
        PutStr((STRPTR)"Usage: QRXDSK <disk1.dsk> <disk2.dsk> <output.sit>\n");
        return 20;
    }

    seg1.data = NULL;
    seg2.data = NULL;

    d1 = Open((STRPTR)argv[1], MODE_OLDFILE);
    d2 = Open((STRPTR)argv[2], MODE_OLDFILE);
    if (!d1 || !d2) {
        Printf((STRPTR)"QRXDSK: cannot open input image\n");
        goto done;
    }

    if (!hfs_extract(d1, ".sit.1", &seg1)) {
        Printf((STRPTR)"QRXDSK: .sit.1 not found in %s\n", (LONG)argv[1]);
        goto done;
    }
    if (!hfs_extract(d2, ".sit.2", &seg2)) {
        Printf((STRPTR)"QRXDSK: .sit.2 not found in %s\n", (LONG)argv[2]);
        goto done;
    }

    if (seg1.size <= SEG1_SKIP || seg2.size <= SEG2_SKIP ||
        seg1.data[0] != 0x84 || seg1.data[1] != 0x55 ||
        seg2.data[0] != 0x84 || seg2.data[1] != 0x55 ||
        be16(seg1.data + 2) != 1 || be16(seg2.data + 2) != 2 ||
        memcmp(seg1.data + SEG1_SKIP, "SIT!", 4) != 0) {
        Printf((STRPTR)"QRXDSK: invalid split segment header\n");
        goto done;
    }

    out = Open((STRPTR)argv[3], MODE_NEWFILE);
    if (!out) {
        Printf((STRPTR)"QRXDSK: cannot create %s\n", (LONG)argv[3]);
        goto done;
    }

    if (!write_all(out, seg1.data + SEG1_SKIP, seg1.size - SEG1_SKIP) ||
        !write_all(out, seg2.data + SEG2_SKIP, seg2.size - SEG2_SKIP)) {
        Close(out);
        DeleteFile((STRPTR)argv[3]);
        Printf((STRPTR)"QRXDSK: write failed\n");
        goto done;
    }
    Close(out);

    Printf((STRPTR)"QRXDSK: joined %ld + %ld bytes -> %s\n",
           (LONG)(seg1.size - SEG1_SKIP),
           (LONG)(seg2.size - SEG2_SKIP),
           (LONG)argv[3]);
    rc = 0;

done:
    if (seg1.data)
        FreeMem(seg1.data, seg1.size);
    if (seg2.data)
        FreeMem(seg2.data, seg2.size);
    if (d1)
        Close(d1);
    if (d2)
        Close(d2);
    return rc;
}
