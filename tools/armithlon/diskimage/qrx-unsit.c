/*
 * QRXUnSit - native StuffIt 5 extractor for QRX OS One.
 *
 * The container parser and the store (method 0) path are native AmigaDOS
 * code.  Method 13 (LZ + prefix/Huffman codes) is a compact C port of the
 * XADMaster algorithm, with the static tables in qrx-sit13-tables.h.
 * Nothing is mounted or unpacked on the host.
 */
#include <exec/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "qrx-sit13-tables.h"
#include "qrx-arsenic.h"

#define SIT5_ID 0xA5A5A5A5u
#define SIT5_FLAG_DIRECTORY 0x40

#define QRX_MAX_NODES 2048

typedef struct QrxNode {
    int16_t child[2];
    int16_t symbol;
} QrxNode;

typedef struct QrxCode {
    QrxNode node[QRX_MAX_NODES];
    int count;
} QrxCode;

typedef struct QrxBits {
    const uint8_t *data;
    uint32_t length;
    uint32_t bitpos;
} QrxBits;

static QrxCode qrx_first_code;
static QrxCode qrx_second_code;
static QrxCode qrx_offset_code;
static QrxCode qrx_meta_code;

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

static int copy_range(BPTR in, uint32_t off, uint32_t len, BPTR out,
                      uint8_t *buf, uint32_t bufsize)
{
    while (len) {
        uint32_t chunk = len > bufsize ? bufsize : len;
        if (!read_at(in, off, buf, chunk))
            return 0;
        if (Write(out, buf, (LONG)chunk) != (LONG)chunk)
            return 0;
        off += chunk;
        len -= chunk;
    }
    return 1;
}

static void mac_name(const uint8_t *src, uint32_t len, char *dst, size_t dstsize)
{
    size_t i;
    if (dstsize == 0)
        return;
    for (i = 0; i < len && i + 1 < dstsize; i++) {
        unsigned char c = src[i];
        if (c == ':' || c == '/' || c == '\\')
            dst[i] = '-';
        else if (c < 0x20 || c == 0x7f)
            dst[i] = '_';
        else if (c >= 0x80)
            dst[i] = '?';
        else
            dst[i] = (char)c;
    }
    dst[i] = 0;
}

static void join_path(char *out, size_t size, const char *dir, const char *name)
{
    if (dir && dir[0]) {
        if (name && name[0])
            snprintf(out, size, "%s/%s", dir, name);
        else
            snprintf(out, size, "%s", dir);
    } else {
        snprintf(out, size, "%s", name ? name : "");
    }
}

static void code_init(QrxCode *code)
{
    int i;
    code->count = 1;
    code->node[0].child[0] = -1;
    code->node[0].child[1] = -1;
    code->node[0].symbol = -1;
    for (i = 1; i < QRX_MAX_NODES; i++) {
        code->node[i].child[0] = -1;
        code->node[i].child[1] = -1;
        code->node[i].symbol = -1;
    }
}

static int code_new_node(QrxCode *code)
{
    int n;
    if (code->count >= QRX_MAX_NODES)
        return -1;
    n = code->count++;
    code->node[n].child[0] = -1;
    code->node[n].child[1] = -1;
    code->node[n].symbol = -1;
    return n;
}

/* Add a code whose bits are consumed least-significant-bit first. */
static int code_add(QrxCode *code, uint32_t bits, int len, int symbol)
{
    int i;
    int node = 0;
    if (len <= 0 || len > 32)
        return 0;
    for (i = 0; i < len; i++) {
        int bit = (bits >> i) & 1;
        int next = code->node[node].child[bit];
        if (next < 0) {
            next = code_new_node(code);
            if (next < 0)
                return 0;
            code->node[node].child[bit] = (int16_t)next;
        }
        node = next;
    }
    if (code->node[node].symbol >= 0)
        return 0;
    code->node[node].symbol = (int16_t)symbol;
    return 1;
}

static int code_build_canonical(QrxCode *code, const int *lengths,
                                int nsyms, int maxlen)
{
    uint32_t bits = 0;
    int length;
    int i;
    int left = 0;
    code_init(code);
    for (i = 0; i < nsyms; i++)
        if (lengths[i] > 0)
            left++;
    if (!left)
        return 0;
    for (length = 1; length <= maxlen; length++) {
        for (i = 0; i < nsyms; i++) {
            if (lengths[i] != length)
                continue;
            /* The LE bitstream presents a canonical code MSB first. */
            {
                uint32_t rev = 0;
                int j;
                for (j = 0; j < length; j++)
                    rev = (rev << 1) | ((bits >> j) & 1);
                if (!code_add(code, rev, length, i))
                    return 0;
            }
            bits++;
            if (--left == 0)
                return 1;
        }
        bits <<= 1;
    }
    return 0;
}

static int bits_get(QrxBits *in, int count)
{
    uint32_t value = 0;
    int i;
    if (count < 0 || count > 31)
        return -1;
    if (in->bitpos + (uint32_t)count > in->length * 8u)
        return -1;
    for (i = 0; i < count; i++) {
        uint32_t p = in->bitpos++;
        if ((in->data[p >> 3] >> (p & 7)) & 1)
            value |= 1u << i;
    }
    return (int)value;
}

static int code_symbol(QrxCode *code, QrxBits *in)
{
    int node = 0;
    int depth;
    for (depth = 0; depth < 32; depth++) {
        int bit = bits_get(in, 1);
        int next;
        if (bit < 0)
            return -1;
        next = code->node[node].child[bit];
        if (next < 0)
            return -1;
        node = next;
        if (code->node[node].symbol >= 0)
            return code->node[node].symbol;
    }
    return -1;
}

static int parse_lengths(QrxBits *in, QrxCode *meta, int *lengths, int nsyms)
{
    int i = 0;
    int length = 0;
    while (i < nsyms) {
        int value = code_symbol(meta, in);
        if (value < 0)
            return 0;
        switch (value) {
        case 31:
            length = -1;
            break;
        case 32:
            length++;
            break;
        case 33:
            length--;
            break;
        case 34:
            if (bits_get(in, 1) == 1 && i < nsyms)
                lengths[i++] = length;
            break;
        case 35:
            value = bits_get(in, 3);
            if (value < 0)
                return 0;
            value += 2;
            while (value-- && i < nsyms)
                lengths[i++] = length;
            break;
        case 36:
            value = bits_get(in, 6);
            if (value < 0)
                return 0;
            value += 10;
            while (value-- && i < nsyms)
                lengths[i++] = length;
            break;
        default:
            length = value + 1;
            break;
        }
        if (i < nsyms) {
            lengths[i] = length;
            i++;
        }
    }
    return 1;
}

static int parse_code(QrxBits *in, QrxCode *meta, QrxCode *out, int nsyms)
{
    int lengths[321];
    int i;
    for (i = 0; i < 321; i++)
        lengths[i] = 0;
    if (nsyms > 321 || !parse_lengths(in, meta, lengths, nsyms))
        return 0;
    return code_build_canonical(out, lengths, nsyms, 32);
}

static int build_meta_code(void)
{
    int i;
    code_init(&qrx_meta_code);
    for (i = 0; i < 37; i++) {
        if (!code_add(&qrx_meta_code, (uint32_t)qrx_metacodes[i],
                      qrx_metacodelengths[i], i))
            return 0;
    }
    return 1;
}

static int build_static_codes(int index)
{
    const int *first;
    const int *second;
    const int *offset;
    int offsetsize;
    switch (index) {
    case 1:
        first = qrx_firstcodelengths_1;
        second = qrx_secondcodelengths_1;
        offset = qrx_offsetcodelengths_1;
        offsetsize = 11;
        break;
    case 2:
        first = qrx_firstcodelengths_2;
        second = qrx_secondcodelengths_2;
        offset = qrx_offsetcodelengths_2;
        offsetsize = 13;
        break;
    case 3:
        first = qrx_firstcodelengths_3;
        second = qrx_secondcodelengths_3;
        offset = qrx_offsetcodelengths_3;
        offsetsize = 14;
        break;
    case 4:
        first = qrx_firstcodelengths_4;
        second = qrx_secondcodelengths_4;
        offset = qrx_offsetcodelengths_4;
        offsetsize = 11;
        break;
    case 5:
        first = qrx_firstcodelengths_5;
        second = qrx_secondcodelengths_5;
        offset = qrx_offsetcodelengths_5;
        offsetsize = 11;
        break;
    default:
        return 0;
    }
    return code_build_canonical(&qrx_first_code, first, 321, 32) &&
           code_build_canonical(&qrx_second_code, second, 321, 32) &&
           code_build_canonical(&qrx_offset_code, offset, offsetsize, 32);
}

static int decode13(const uint8_t *src, uint32_t srclen,
                    uint8_t *dst, uint32_t dstlen)
{
    QrxBits in;
    int value;
    int index;
    uint32_t pos = 0;
    QrxCode *current;

    in.data = src;
    in.length = srclen;
    in.bitpos = 0;
    value = bits_get(&in, 8);
    if (value < 0)
        return 0;

    index = value >> 4;
    if (index == 0) {
        if (!build_meta_code())
            return 0;
        if (!parse_code(&in, &qrx_meta_code, &qrx_first_code, 321))
            return 0;
        if (value & 0x08)
            qrx_second_code = qrx_first_code;
        else if (!parse_code(&in, &qrx_meta_code, &qrx_second_code, 321))
            return 0;
        if (!parse_code(&in, &qrx_meta_code, &qrx_offset_code,
                        (value & 7) + 10))
            return 0;
    } else if (!build_static_codes(index)) {
        return 0;
    }

    current = &qrx_first_code;
    while (pos < dstlen) {
        int symbol = code_symbol(current, &in);
        if (symbol < 0)
            return 0;
        if (symbol < 0x100) {
            dst[pos++] = (uint8_t)symbol;
            current = &qrx_first_code;
        } else {
            uint32_t length;
            int bitlength;
            uint32_t offset;
            current = &qrx_second_code;
            if (symbol < 0x13e) {
                length = (uint32_t)(symbol - 0x100 + 3);
            } else if (symbol == 0x13e) {
                value = bits_get(&in, 10);
                if (value < 0)
                    return 0;
                length = (uint32_t)value + 65;
            } else if (symbol == 0x13f) {
                value = bits_get(&in, 15);
                if (value < 0)
                    return 0;
                length = (uint32_t)value + 65;
            } else {
                break;
            }
            bitlength = code_symbol(&qrx_offset_code, &in);
            if (bitlength < 0)
                return 0;
            if (bitlength == 0) {
                offset = 1;
            } else if (bitlength == 1) {
                offset = 2;
            } else {
                value = bits_get(&in, bitlength - 1);
                if (value < 0)
                    return 0;
                offset = (1u << (bitlength - 1)) + (uint32_t)value + 1u;
            }
            if (!offset || offset > pos || length > dstlen - pos)
                return 0;
            while (length--) {
                dst[pos] = dst[pos - offset];
                pos++;
            }
        }
    }
    return pos == dstlen;
}

static int extract13(BPTR in, uint32_t off, uint32_t complen,
                     BPTR out, uint32_t datalen)
{
    uint8_t *compressed;
    uint8_t *plain;
    int ok;

    compressed = (uint8_t *)AllocMem(complen ? complen : 1, MEMF_ANY);
    plain = (uint8_t *)AllocMem(datalen ? datalen : 1, MEMF_ANY);
    if (!compressed || !plain) {
        if (compressed) FreeMem(compressed, complen ? complen : 1);
        if (plain) FreeMem(plain, datalen ? datalen : 1);
        PutStr((STRPTR)"QRXUnSit: out of memory for method 13\n");
        return 0;
    }
    ok = read_at(in, off, compressed, complen) &&
         decode13(compressed, complen, plain, datalen) &&
         Write(out, plain, (LONG)datalen) == (LONG)datalen;
    FreeMem(compressed, complen ? complen : 1);
    FreeMem(plain, datalen ? datalen : 1);
    return ok;
}

static int extract15(BPTR in, uint32_t off, uint32_t complen,
                     BPTR out, uint32_t datalen)
{
    uint8_t *compressed;
    uint8_t *plain;
    int ok;

    compressed = (uint8_t *)AllocMem(complen ? complen : 1, MEMF_ANY);
    plain = (uint8_t *)AllocMem(datalen ? datalen : 1, MEMF_ANY);
    if (!compressed || !plain) {
        if (compressed) FreeMem(compressed, complen ? complen : 1);
        if (plain) FreeMem(plain, datalen ? datalen : 1);
        PutStr((STRPTR)"QRXUnSit: out of memory for method 15\n");
        return 0;
    }
    ok = read_at(in, off, compressed, complen) &&
         qrx_arsenic_decode(compressed, complen, plain, datalen) &&
         Write(out, plain, (LONG)datalen) == (LONG)datalen;
    FreeMem(compressed, complen ? complen : 1);
    FreeMem(plain, datalen ? datalen : 1);
    return ok;
}

static int write_fork(BPTR in, uint32_t off, uint32_t complen, uint32_t datalen,
                      uint8_t method, const char *path, uint8_t *copybuf)
{
    BPTR out = Open((STRPTR)path, MODE_NEWFILE);
    int ok = 0;
    if (!out) {
        Printf((STRPTR)"QRXUnSit: cannot create %s\n", (LONG)path);
        return 0;
    }
    if (method == 0)
        ok = copy_range(in, off, complen, out, copybuf, 32768);
    else if (method == 13)
        ok = extract13(in, off, complen, out, datalen);
    else if (method == 15)
        ok = extract15(in, off, complen, out, datalen);
    else
        Printf((STRPTR)"QRXUnSit: unsupported method %ld: %s\n",
               (LONG)method, (LONG)path);
    Close(out);
    return ok;
}


static int find_classic(BPTR in, uint32_t *archive_off)
{
    uint8_t scan[65536 + 16];
    LONG size = Seek(in, 0, OFFSET_END);
    uint32_t base = 0;
    if (size < 16)
        return 0;
    while (base + 16 <= (uint32_t)size) {
        uint32_t want = (uint32_t)size - base;
        uint32_t i;
        if (want > 65536)
            want = 65536;
        if (!read_at(in, base, scan, want))
            return 0;
        for (i = 0; i + 16 <= want; i++) {
            if (memcmp(scan + i, "SIT!", 4) == 0 &&
                memcmp(scan + i + 10, "rLau", 4) == 0) {
                *archive_off = base + i;
                return 1;
            }
        }
        if (want < 65536)
            break;
        base += want - 15;
    }
    return 0;
}

static void path_pop(char *path)
{
    char *p = strrchr(path, '/');
    if (p)
        *p = 0;
    else
        path[0] = 0;
}

static int extract_classic(BPTR in, uint32_t archive_off, const char *dest)
{
    uint8_t hdr[22];
    uint8_t entry[112];
    uint8_t *buf;
    uint32_t total, pos, end;
    uint32_t i;
    char curdir[512] = "";
    int rc = 0;

    if (!read_at(in, archive_off, hdr, 22))
        return 20;
    total = be32(hdr + 6);
    if (total < 22 || archive_off + total < archive_off)
        return 20;
    end = archive_off + total;
    pos = archive_off + 22;

    buf = (uint8_t *)AllocMem(32768, MEMF_ANY);
    if (!buf)
        return 20;

    for (i = 0; i < 1000000 && pos + 112 <= end; i++) {
        uint32_t reslen, rescomp, datalen, datacomp;
        uint8_t rm, dm;
        uint32_t namelen;
        char name[256], outpath[640], rsrcpath[640];
        uint32_t dataoff;

        if (!read_at(in, pos, entry, 112)) {
            PutStr((STRPTR)"QRXUnSit: truncated classic entry\\n");
            rc = 20; break;
        }
        rm = entry[0];
        dm = entry[1];
        namelen = entry[2];
        if (namelen > 31)
            namelen = 31;
        mac_name(entry + 3, namelen, name, sizeof(name));
        reslen = be32(entry + 84);
        datalen = be32(entry + 88);
        rescomp = be32(entry + 92);
        datacomp = be32(entry + 96);

        if ((dm & 0x6f) == 0x20 || (rm & 0x6f) == 0x20) {
            if (name[0]) {
                char dirpath[640];
                join_path(outpath, sizeof(outpath), dest, curdir);
                join_path(dirpath, sizeof(dirpath), outpath, name);
                CreateDir((STRPTR)dirpath);
                if (curdir[0]) {
                    size_t len = strlen(curdir);
                    snprintf(curdir + len, sizeof(curdir) - len, "/%s", name);
                } else {
                    snprintf(curdir, sizeof(curdir), "%s", name);
                }
            }
            pos += 112;
            continue;
        }
        if ((dm & 0x6f) == 0x21 || (rm & 0x6f) == 0x21) {
            path_pop(curdir);
            pos += 112;
            continue;
        }
        if (!name[0]) {
            PutStr((STRPTR)"QRXUnSit: empty classic name\\n");
            rc = 20; break;
        }
        if (dm & 0x80 || rm & 0x80) {
            Printf((STRPTR)"QRXUnSit: encrypted classic entry: %s\\n",
                   (LONG)name);
            rc = 20; break;
        }
        dataoff = pos + 112;
        if (reslen) {
            join_path(outpath, sizeof(outpath), dest, curdir);
            join_path(rsrcpath, sizeof(rsrcpath), outpath, name);
            strncat(rsrcpath, ".rsrc", sizeof(rsrcpath) - strlen(rsrcpath) - 1);
            if (!write_fork(in, dataoff, rescomp ? rescomp : reslen, reslen,
                            rm & 0x0f, rsrcpath, buf)) {
                Printf((STRPTR)"QRXUnSit: classic resource skipped: %s\\n",
                       (LONG)name);
                DeleteFile((STRPTR)rsrcpath);
            }
            dataoff += rescomp ? rescomp : reslen;
        }
        join_path(outpath, sizeof(outpath), dest, curdir);
        join_path(rsrcpath, sizeof(rsrcpath), outpath, name);
        if (!write_fork(in, dataoff, datacomp ? datacomp : datalen, datalen,
                        dm & 0x0f, rsrcpath, buf)) {
            Printf((STRPTR)"QRXUnSit: classic data failed: %s\\n", (LONG)name);
            rc = 20; break;
        }
        Printf((STRPTR)"QRXUnSit: %s (%ld bytes, classic method %ld)\\n",
               (LONG)name, (LONG)datalen, (LONG)(dm & 0x0f));
        pos = dataoff + (datacomp ? datacomp : datalen);
    }

    FreeMem(buf, 32768);
    return rc;
}

int main(int argc, char **argv)
{
    BPTR in;
    uint8_t hdr[512];
    uint8_t entry[4096];
    uint8_t meta[64];
    uint8_t *buf;
    const char *dest;
    uint32_t total, first, pos;
    uint32_t i;
    int rc = 0;

    if (argc < 2 || argc > 3) {
        PutStr((STRPTR)"Usage: QRXUnSit <archive.sit> [destination-dir]\n");
        return 20;
    }
    dest = argc == 3 ? argv[2] : "";

    in = Open((STRPTR)argv[1], MODE_OLDFILE);
    if (!in) {
        Printf((STRPTR)"QRXUnSit: cannot open %s\n", (LONG)argv[1]);
        return 20;
    }
    if (!read_at(in, 0, hdr, 100)) {
        PutStr((STRPTR)"QRXUnSit: cannot read SIT5 header\n");
        Close(in);
        return 20;
    }
    if (memcmp(hdr, "StuffIt (c)1997-", 16) != 0 || hdr[82] != 5) {
        uint32_t classic_off;
        if (find_classic(in, &classic_off)) {
            int classic_rc;
            if (dest[0])
                CreateDir((STRPTR)dest);
            classic_rc = extract_classic(in, classic_off, dest);
            Close(in);
            return classic_rc;
        }
        PutStr((STRPTR)"QRXUnSit: not a StuffIt 5/classic SIT archive\n");
        Close(in);
        return 20;
    }
    total = be16(hdr + 92);
    first = be32(hdr + 94);
    if (total == 0)
        total = 1;
    pos = first;

    if (dest[0])
        CreateDir((STRPTR)dest);

    buf = (uint8_t *)AllocMem(32768, MEMF_ANY);
    if (!buf) {
        PutStr((STRPTR)"QRXUnSit: out of memory\n");
        Close(in);
        return 20;
    }

    for (i = 0; i < total && i < 100000; i++) {
        uint32_t hs, datalen, complen, namelen, cur, base;
        uint32_t reslen = 0, rescomp = 0;
        uint32_t dataoff;
        uint8_t flags, method = 0, resmethod = 0, ver;
        uint16_t something = 0;
        char name[256], outpath[512], rsrcpath[512];

        if (!read_at(in, pos, entry, 8)) {
            PutStr((STRPTR)"QRXUnSit: truncated entry header\n");
            rc = 20; break;
        }
        if (be32(entry) != SIT5_ID || pos + 8 > 0xffffffffu) {
            PutStr((STRPTR)"QRXUnSit: invalid entry id\n");
            rc = 20; break;
        }
        hs = be16(entry + 6);
        if (hs < 48 || hs > sizeof(entry) || !read_at(in, pos, entry, hs)) {
            PutStr((STRPTR)"QRXUnSit: invalid entry header size\n");
            rc = 20; break;
        }
        flags = entry[9];
        ver = entry[4];
        namelen = be16(entry + 30);
        if (namelen > 255 || 48 + namelen > hs) {
            PutStr((STRPTR)"QRXUnSit: invalid name\n");
            rc = 20; break;
        }
        mac_name(entry + 48, namelen, name, sizeof(name));

        /* The per-entry Finder/resource metadata lives after the variable
           name/comment header.  It is not included in hs. */
        cur = 48 + namelen;
        if (cur < hs) {
            uint16_t commentsize = be16(entry + cur);
            cur += 4 + commentsize;
        }
        if (!read_at(in, pos + cur, meta, sizeof(meta))) {
            PutStr((STRPTR)"QRXUnSit: truncated entry metadata\n");
            rc = 20; break;
        }
        something = be16(meta);
        base = 14 + ((ver == 1) ? 22 : 18);
        if (something & 0x01) {
            if (base + 16 > sizeof(meta)) {
                PutStr((STRPTR)"QRXUnSit: invalid resource metadata\n");
                rc = 20; break;
            }
            reslen = be32(meta + base);
            rescomp = be32(meta + base + 4);
            resmethod = meta[base + 12];
        }

        if (flags & SIT5_FLAG_DIRECTORY) {
            uint16_t children = be16(entry + 46);
            if (name[0]) {
                join_path(outpath, sizeof(outpath), dest, name);
                CreateDir((STRPTR)outpath);
            }
            pos += cur + base;
            total += children;
            continue;
        }

        method = entry[46];
        datalen = be32(entry + 34);
        complen = be32(entry + 38);
        if (datalen == 0xffffffffu) {
            total++;
            pos += hs;
            continue;
        }
        if (complen == 0)
            complen = datalen;
        if (method != 0 && method != 13 && method != 15) {
            Printf((STRPTR)"QRXUnSit: %s uses unsupported method %ld\n",
                   (LONG)name, (LONG)method);
            rc = 20;
            break;
        }
        if (!name[0]) {
            PutStr((STRPTR)"QRXUnSit: empty archive name\n");
            rc = 20; break;
        }

        dataoff = pos + cur + base + (reslen ? 14 : 0);
        if (reslen) {
            join_path(rsrcpath, sizeof(rsrcpath), dest, name);
            strncat(rsrcpath, ".rsrc", sizeof(rsrcpath) - strlen(rsrcpath) - 1);
            if (!write_fork(in, dataoff, rescomp ? rescomp : reslen, reslen,
                            resmethod, rsrcpath, buf)) {
                /* A failed resource fork must not prevent the data fork
                   from being extracted. */
                Printf((STRPTR)"QRXUnSit: resource fork skipped: %s\n",
                       (LONG)name);
                DeleteFile((STRPTR)rsrcpath);
            }
            dataoff += rescomp ? rescomp : reslen;
        }
        join_path(outpath, sizeof(outpath), dest, name);
        if (!write_fork(in, dataoff, complen, datalen, method, outpath, buf)) {
            Printf((STRPTR)"QRXUnSit: data extraction failed: %s\n", (LONG)name);
            rc = 20; break;
        }
        Printf((STRPTR)"QRXUnSit: %s (%ld bytes, method %ld)\n",
               (LONG)name, (LONG)datalen, (LONG)method);
        pos = dataoff + complen;
    }

    FreeMem(buf, 32768);
    Close(in);
    return rc;
}
