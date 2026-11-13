/*
 * QRXPPC - MacBinary II wrapper and resource fork reader (P2.1).
 *
 * Deliberate limits, each reported as its own status instead of a crash:
 *   - MacBinary III signatures are accepted like II (the CRC still applies),
 *   - the resource map is read, but names and attributes are ignored,
 *   - 'cfrg' resources must use the documented 12-byte form,
 *   - packed PEF sections stay rejected by qrx_pef.c.
 */
#include "qrx_macapp.h"

#include <string.h>

static uint32_t macapp_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t macapp_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* CRC-16-CCITT as MacBinary II uses it: polynomial 0x1021, initial value 0,
 * over the first 124 header bytes. */
static uint16_t macapp_crc16(const uint8_t *p, uint32_t n)
{
    uint16_t crc = 0u;
    uint32_t i;
    unsigned bit;

    for (i = 0u; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (bit = 0u; bit < 8u; bit++) {
            if (crc & 0x8000u)
                crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
            else
                crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

qrx_macapp_status_t qrx_macbin_parse(qrx_macbin_t *mb, const uint8_t *image,
                                     uint32_t size)
{
    uint32_t data_len;
    uint32_t rsrc_len;
    uint32_t data_off;
    uint32_t rsrc_off;
    uint32_t i;

    if (!mb || !image)
        return QRX_MACAPP_ERR_ARG;
    memset(mb, 0, sizeof(*mb));
    if (size < QRX_MACBIN_HEADER)
        return QRX_MACAPP_ERR_SHORT;
    if (image[0] != 0u)
        return QRX_MACAPP_ERR_HEADER;      /* version byte stays zero */
    if (image[1] < 1u || image[1] > QRX_MACBIN_NAME_MAX)
        return QRX_MACAPP_ERR_HEADER;
    data_len = macapp_be32(image + 83u);
    rsrc_len = macapp_be32(image + 87u);
    if (data_len > QRX_MACBIN_FORK_MAX || rsrc_len > QRX_MACBIN_FORK_MAX)
        return QRX_MACAPP_ERR_HEADER;
    data_off = QRX_MACBIN_HEADER;
    rsrc_off = data_off + ((data_len + 127u) & ~127u);
    if (data_len > size - data_off)
        return QRX_MACAPP_ERR_SHORT;
    if (rsrc_off > size || rsrc_len > size - rsrc_off)
        return QRX_MACAPP_ERR_SHORT;
    if (image[122] >= QRX_MACBIN_VERSION_II) {
        if (macapp_crc16(image, 124u) != macapp_be16(image + 124u))
            return QRX_MACAPP_ERR_CRC;
        mb->has_crc = 1;
    }
    mb->data = image + data_off;
    mb->data_len = data_len;
    mb->rsrc = image + rsrc_off;
    mb->rsrc_len = rsrc_len;
    mb->file_type = macapp_be32(image + 65u);
    mb->file_creator = macapp_be32(image + 69u);
    mb->name_len = image[1];
    for (i = 0u; i < mb->name_len; i++)
        mb->name[i] = (char)image[2u + i];
    mb->name[mb->name_len] = 0;
    return QRX_MACAPP_OK;
}

qrx_macapp_status_t qrx_rsrc_find(const uint8_t *rsrc, uint32_t size,
                                  uint32_t type, int id, const uint8_t **out,
                                  uint32_t *out_len)
{
    const uint8_t *map;
    uint32_t data_off;
    uint32_t map_off;
    uint32_t data_len;
    uint32_t map_len;
    uint32_t type_list;
    uint32_t types;
    uint32_t i;
    uint32_t j;

    if (!rsrc || !out || !out_len)
        return QRX_MACAPP_ERR_ARG;
    if (size < QRX_RSRC_HEADER)
        return QRX_MACAPP_ERR_SHORT;
    data_off = macapp_be32(rsrc + 0u);
    map_off = macapp_be32(rsrc + 4u);
    data_len = macapp_be32(rsrc + 8u);
    map_len = macapp_be32(rsrc + 12u);
    if (data_off > size || map_off > size)
        return QRX_MACAPP_ERR_RSRC;
    if (data_len > size - data_off || map_len > size - map_off)
        return QRX_MACAPP_ERR_SHORT;
    if (map_len < QRX_RSRC_MAP_HEADER)
        return QRX_MACAPP_ERR_RSRC;
    map = rsrc + map_off;
    type_list = macapp_be16(map + 24u);
    if (type_list + 2u > map_len)
        return QRX_MACAPP_ERR_RSRC;
    types = (uint32_t)macapp_be16(map + type_list) + 1u;
    if (type_list + 2u + types * 8u > map_len)
        return QRX_MACAPP_ERR_RSRC;
    for (i = 0u; i < types; i++) {
        const uint8_t *entry = map + type_list + 2u + i * 8u;
        uint32_t refs;
        uint32_t ref_list;

        if (macapp_be32(entry) != type)
            continue;
        refs = (uint32_t)macapp_be16(entry + 4u) + 1u;
        ref_list = type_list + (uint32_t)macapp_be16(entry + 6u);
        if (ref_list + refs * QRX_RSRC_REF_SIZE > map_len)
            return QRX_MACAPP_ERR_RSRC;
        for (j = 0u; j < refs; j++) {
            const uint8_t *ref = map + ref_list + j * QRX_RSRC_REF_SIZE;
            uint32_t res_off;
            uint32_t res_len;

            if ((int)macapp_be16(ref) != id)
                continue;
            res_off = ((uint32_t)ref[5] << 16) | ((uint32_t)ref[6] << 8) |
                      (uint32_t)ref[7];
            if (res_off > data_len || data_len - res_off < 4u)
                return QRX_MACAPP_ERR_RSRC;
            res_len = macapp_be32(rsrc + data_off + res_off);
            if (res_len > data_len - res_off - 4u)
                return QRX_MACAPP_ERR_SHORT;
            *out = rsrc + data_off + res_off + 4u;
            *out_len = res_len;
            return QRX_MACAPP_OK;
        }
    }
    return QRX_MACAPP_ERR_NO_CFRG;
}

qrx_macapp_status_t qrx_cfrg_parse(qrx_cfrg_t *cfrg, const uint8_t *data,
                                   uint32_t size, uint32_t fork_size)
{
    if (!cfrg || !data)
        return QRX_MACAPP_ERR_ARG;
    memset(cfrg, 0, sizeof(*cfrg));
    if (size < QRX_CFRG_SIZE)
        return QRX_MACAPP_ERR_CFRG;
    if (macapp_be32(data) != 0u)
        return QRX_MACAPP_ERR_CFRG;        /* reserved word stays zero */
    cfrg->offset = macapp_be32(data + 4u);
    cfrg->length = macapp_be32(data + 8u);
    if (cfrg->length == 0u || cfrg->offset >= fork_size)
        return QRX_MACAPP_ERR_CFRG;
    if (cfrg->length > fork_size - cfrg->offset)
        return QRX_MACAPP_ERR_CFRG;
    return QRX_MACAPP_OK;
}

qrx_macapp_status_t qrx_app_load(qrx_pef_t *pef, qrx_ppc_t *ppc, uint32_t base,
                                 const uint8_t *image, uint32_t size,
                                 qrx_pef_resolver_t resolver, void *ctx)
{
    qrx_macbin_t mb;
    qrx_cfrg_t cfrg;
    const uint8_t *res;
    uint32_t res_len;
    qrx_macapp_status_t st;

    if (!pef || !ppc)
        return QRX_MACAPP_ERR_ARG;
    st = qrx_macbin_parse(&mb, image, size);
    if (st != QRX_MACAPP_OK)
        return st;
    if (mb.rsrc_len == 0u)
        return QRX_MACAPP_ERR_NO_CFRG;
    st = qrx_rsrc_find(mb.rsrc, mb.rsrc_len, QRX_CFRG_TYPE, 0, &res, &res_len);
    if (st != QRX_MACAPP_OK)
        return st;
    st = qrx_cfrg_parse(&cfrg, res, res_len, mb.data_len);
    if (st != QRX_MACAPP_OK)
        return st;
    if (qrx_pef_parse(pef, mb.data + cfrg.offset, cfrg.length) != QRX_PEF_OK)
        return QRX_MACAPP_ERR_PEF;
    if (qrx_pef_load(pef, ppc, base) != QRX_PEF_OK)
        return QRX_MACAPP_ERR_PEF;
    if (qrx_pef_relocate(pef, ppc, resolver, ctx) != QRX_PEF_OK)
        return QRX_MACAPP_ERR_PEF;
    return QRX_MACAPP_OK;
}
