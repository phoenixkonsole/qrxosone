#include "qrx_forkfile.h"

#include <string.h>

#define FORK_MAGIC_APPLESINGLE 0x00051600u
#define FORK_MAGIC_APPLEDOUBLE 0x00051607u
#define FORK_ENTRY_DATA 1u
#define FORK_ENTRY_RSRC 2u
#define FORK_ENTRY_NAME 3u
#define FORK_ENTRY_INFO 7u
#define FORK_ENTRY_DATES 8u
#define FORK_MACBINARY_HEADER 128u
#define FORK_BINHEX_MARKER 0x90u
#define FORK_BINHEX_HEADER 22u

static const char binhex_alphabet[] =
    "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* CRC-16/XMODEM, used by the MacBinary II header. */
static uint16_t crc16(const uint8_t *data, uint32_t len)
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

static void copy_name(qrx_forkfile_t *ff, const uint8_t *src, uint32_t len)
{
    uint32_t i;

    if (len > QRX_FORKFILE_NAME_MAX - 1u)
        len = QRX_FORKFILE_NAME_MAX - 1u;
    for (i = 0; i < len; i++) {
        uint8_t c = src[i];

        ff->name[i] = (c >= 0x20u && c < 0x7Fu) ? (char)c : '.';
    }
    ff->name[len] = '\0';
    ff->name_len = (uint8_t)len;
    ff->has_name = 1u;
}

const char *qrx_forkfile_format_name(qrx_forkfile_format format)
{
    switch (format) {
    case QRX_FORKFILE_APPLESINGLE: return "applesingle";
    case QRX_FORKFILE_APPLEDOUBLE: return "appledouble";
    case QRX_FORKFILE_MACBINARY: return "macbinary";
    case QRX_FORKFILE_BINHEX: return "binhex";
    default: return "none";
    }
}

/* ------------------------------------------------- AppleSingle / Double */

static int apple_entry(qrx_forkfile_t *ff, uint32_t id, const uint8_t *entry,
                       uint32_t size)
{
    switch (id) {
    case FORK_ENTRY_DATA:
        ff->data = entry;
        ff->data_len = size;
        return 0;
    case FORK_ENTRY_RSRC:
        ff->rsrc = entry;
        ff->rsrc_len = size;
        return 0;
    case FORK_ENTRY_NAME:
        if (size >= 1u && entry[0] <= size - 1u)
            copy_name(ff, entry + 1u, entry[0]);
        return 0;
    case FORK_ENTRY_INFO:
        if (size < 16u)
            return 0;
        ff->type_code = rd32(entry);
        ff->creator = rd32(entry + 4u);
        ff->fd_flags = rd16(entry + 8u);
        ff->fd_location = rd32(entry + 10u);
        ff->fd_folder = rd16(entry + 14u);
        return 0;
    case FORK_ENTRY_DATES:
        if (size < 16u)
            return 0;
        ff->created = rd32(entry);
        ff->modified = rd32(entry + 4u);
        return 0;
    default:
        return 0;                       /* comments, icons, ProDOS, ... */
    }
}

static int32_t open_apple(qrx_forkfile_t *ff, const uint8_t *blob, uint32_t len,
                          uint32_t magic)
{
    uint32_t entries;
    uint32_t i;
    const uint8_t *table;

    if (len < 26u)
        return QRX_MACIMG_ERR_SHORT;
    entries = rd16(blob + 24u);
    if ((uint64_t)26u + (uint64_t)entries * 12u > (uint64_t)len)
        return QRX_MACIMG_ERR_FORMAT;
    table = blob + 26u;
    for (i = 0; i < entries; i++) {
        const uint8_t *p = table + i * 12u;
        uint32_t id = rd32(p);
        uint32_t offset = rd32(p + 4u);
        uint32_t size = rd32(p + 8u);

        if (offset > len || size > len - offset)
            continue;                   /* a broken entry must not stop us */
        apple_entry(ff, id, blob + offset, size);
    }
    if (ff->data_len && !ff->data)
        return QRX_MACIMG_ERR_FORMAT;
    if (ff->rsrc_len && !ff->rsrc)
        return QRX_MACIMG_ERR_FORMAT;
    ff->format = (magic == FORK_MAGIC_APPLESINGLE) ? QRX_FORKFILE_APPLESINGLE
                                                   : QRX_FORKFILE_APPLEDOUBLE;
    return QRX_MACIMG_OK;
}

/* ------------------------------------------------------------- MacBinary */

static int32_t open_macbinary(qrx_forkfile_t *ff, const uint8_t *blob,
                              uint32_t len)
{
    uint32_t data_len;
    uint32_t rsrc_len;
    uint32_t rsrc_off;
    uint8_t name_len;

    if (len < FORK_MACBINARY_HEADER)
        return QRX_MACIMG_ERR_SHORT;
    if (blob[0] != 0u || blob[74] != 0u)
        return QRX_MACIMG_ERR_FORMAT;
    name_len = blob[1];
    if (name_len < 1u || name_len > 63u)
        return QRX_MACIMG_ERR_FORMAT;
    data_len = rd32(blob + 81u);
    rsrc_len = rd32(blob + 85u);
    /* MacBinary pads each fork to a multiple of 128 bytes. */
    rsrc_off = FORK_MACBINARY_HEADER + ((data_len + 127u) / 128u) * 128u;
    if (rsrc_off > len || rsrc_len > len - rsrc_off)
        return QRX_MACIMG_ERR_FORMAT;
    /* MacBinary II stores a CRC-16 over the first 125 header bytes. */
    if (blob[124] == 0x81u)
        ff->crc_state = (crc16(blob, 125u) == rd16(blob + 125u)) ? 1u : 2u;

    copy_name(ff, blob + 2u, name_len);
    ff->type_code = rd32(blob + 65u);
    ff->creator = rd32(blob + 69u);
    ff->fd_flags = rd16(blob + 73u);
    ff->fd_location = ((uint32_t)rd16(blob + 75u) << 16) | rd16(blob + 77u);
    ff->fd_folder = rd16(blob + 79u);
    ff->created = rd32(blob + 89u);
    ff->modified = rd32(blob + 93u);
    ff->data = blob + FORK_MACBINARY_HEADER;
    ff->data_len = data_len;
    ff->rsrc = blob + rsrc_off;
    ff->rsrc_len = rsrc_len;
    ff->format = QRX_FORKFILE_MACBINARY;
    return QRX_MACIMG_OK;
}

/* ---------------------------------------------------------------- BinHex */

static int binhex_value(uint8_t c)
{
    const char *p = strchr(binhex_alphabet, (char)c);

    return (p == NULL) ? -1 : (int)(p - binhex_alphabet);
}

/*
 * Decodes BinHex 4.0 into `out`: 6-bit characters to bytes, then the 0x90
 * run-length rule (0x90 0x00 is a literal 0x90, 0x90 n repeats the previous
 * byte n times).  The encoded body ends at the closing ':'.
 */
static int32_t binhex_decode(const uint8_t *text, uint32_t len, uint8_t *out,
                             uint32_t cap, uint32_t *out_len)
{
    uint32_t bits = 0;
    int nbits = 0;
    uint32_t n = 0;
    uint32_t i;
    uint8_t previous = 0;
    int expect_count = 0;

    for (i = 0; i < len; i++) {
        int value = binhex_value(text[i]);

        if (value < 0) {
            if (text[i] == ':')
                break;                  /* closing marker */
            continue;                   /* CR, LF, spaces between lines */
        }
        bits = (bits << 6) | (uint32_t)value;
        nbits += 6;
        while (nbits >= 8) {
            uint8_t byte;

            nbits -= 8;
            byte = (uint8_t)((bits >> nbits) & 0xFFu);
            if (expect_count) {
                if (byte == 0u) {
                    if (n >= cap)
                        return QRX_MACIMG_ERR_RANGE;
                    out[n++] = FORK_BINHEX_MARKER;
                } else {
                    uint32_t k;

                    if ((uint64_t)n + byte > (uint64_t)cap)
                        return QRX_MACIMG_ERR_RANGE;
                    for (k = 0; k < byte; k++)
                        out[n++] = previous;
                }
                expect_count = 0;
            } else if (byte == FORK_BINHEX_MARKER) {
                expect_count = 1;
            } else {
                if (n >= cap)
                    return QRX_MACIMG_ERR_RANGE;
                out[n++] = byte;
                previous = byte;
            }
        }
    }
    if (expect_count)
        return QRX_MACIMG_ERR_FORMAT;
    *out_len = n;
    return QRX_MACIMG_OK;
}

static int32_t open_binhex(qrx_forkfile_t *ff, const uint8_t *blob,
                           uint32_t len, uint8_t *work, uint32_t work_cap)
{
    uint32_t start;
    uint32_t decoded = 0;
    uint8_t name_len;
    uint32_t data_len;
    uint32_t rsrc_len;
    uint32_t rsrc_off;
    int32_t status;

    if (len < 2u)
        return QRX_MACIMG_ERR_SHORT;
    if (!work || work_cap == 0u)
        return QRX_MACIMG_ERR_RANGE;    /* BinHex always needs scratch */
    for (start = 0; start < len; start++) {
        if (blob[start] == ':')
            break;
    }
    if (start >= len)
        return QRX_MACIMG_ERR_FORMAT;
    status = binhex_decode(blob + start + 1u, len - start - 1u, work, work_cap,
                           &decoded);
    if (status != QRX_MACIMG_OK)
        return status;
    if (decoded < FORK_BINHEX_HEADER + 1u)
        return QRX_MACIMG_ERR_FORMAT;
    name_len = work[0];
    if (name_len < 1u || name_len > 63u)
        return QRX_MACIMG_ERR_FORMAT;
    if ((uint32_t)name_len + FORK_BINHEX_HEADER > decoded)
        return QRX_MACIMG_ERR_FORMAT;
    data_len = rd32(work + 12u + name_len);
    rsrc_len = rd32(work + 16u + name_len);
    rsrc_off = (uint32_t)name_len + FORK_BINHEX_HEADER + data_len;
    if (rsrc_off > decoded || rsrc_len > decoded - rsrc_off)
        return QRX_MACIMG_ERR_FORMAT;

    copy_name(ff, work + 1u, name_len);
    ff->type_code = rd32(work + 2u + name_len);
    ff->creator = rd32(work + 6u + name_len);
    ff->fd_flags = rd16(work + 10u + name_len);
    ff->data = work + (uint32_t)name_len + FORK_BINHEX_HEADER;
    ff->data_len = data_len;
    ff->rsrc = work + rsrc_off;
    ff->rsrc_len = rsrc_len;
    ff->format = QRX_FORKFILE_BINHEX;
    return QRX_MACIMG_OK;
}

/* ------------------------------------------------------------------ open */

int32_t qrx_forkfile_open(qrx_forkfile_t *ff, const uint8_t *blob, uint32_t len,
                          uint8_t *work, uint32_t work_cap)
{
    uint32_t magic;

    if (!ff)
        return QRX_MACIMG_ERR_SHORT;
    memset(ff, 0, sizeof(*ff));
    if (!blob || len < 8u)
        return QRX_MACIMG_ERR_SHORT;
    magic = rd32(blob);
    if (magic == FORK_MAGIC_APPLESINGLE || magic == FORK_MAGIC_APPLEDOUBLE)
        return open_apple(ff, blob, len, magic);
    if (blob[0] == 0u && blob[1] >= 1u && blob[1] <= 63u)
        return open_macbinary(ff, blob, len);
    if (blob[0] == '(' || blob[0] == ':' || blob[0] == '\r' || blob[0] == '\n')
        return open_binhex(ff, blob, len, work, work_cap);
    return QRX_MACIMG_ERR_FORMAT;
}

int32_t qrx_forkfile_read(const qrx_forkfile_t *ff, int resource,
                          uint32_t offset, uint8_t *dst, uint32_t len)
{
    const uint8_t *source;
    uint32_t length;

    if (!ff || !dst)
        return QRX_MACIMG_ERR_RANGE;
    if (resource != 0 && resource != 1)
        return QRX_MACIMG_ERR_RANGE;
    if (resource) {
        source = ff->rsrc;
        length = ff->rsrc_len;
    } else {
        source = ff->data;
        length = ff->data_len;
    }
    if (length == 0u && len == 0u)
        return QRX_MACIMG_OK;
    if (!source || offset > length || len > length - offset)
        return QRX_MACIMG_ERR_RANGE;
    if (len)
        memcpy(dst, source + offset, len);
    return QRX_MACIMG_OK;
}
