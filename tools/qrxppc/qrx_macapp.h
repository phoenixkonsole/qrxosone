/*
 * QRXPPC - MacBinary II wrapper and resource fork reader (P2.1).
 *
 * A classic MacOS application is delivered as a MacBinary file: a 128-byte
 * header, the data fork (for a PPC application the PEF container), the
 * resource fork whose 'cfrg' resource names the container inside the data
 * fork, and zero padding to a 128-byte boundary after each fork.
 *
 * This module validates that wrapper, looks the 'cfrg' resource up and hands
 * the fragment to qrx_pef.c.  It stops at the fragment: there is no Toolbox,
 * no code-fragment initialisation beyond qrx_pef_load() and no 68k container.
 */
#ifndef QRX_MACAPP_H
#define QRX_MACAPP_H

#include "qrx_pef.h"

#define QRX_MACBIN_HEADER 128u      /* fixed header size */
#define QRX_MACBIN_NAME_MAX 63u     /* length byte range is 1..63 */
#define QRX_MACBIN_FORK_MAX 0x10000000u /* refuse forks beyond 256 MiB */
#define QRX_MACBIN_VERSION_II 129u  /* byte 122: first MacBinary II version */
#define QRX_RSRC_HEADER 16u         /* data/map offsets and lengths */
#define QRX_RSRC_MAP_HEADER 28u     /* header copy, next, ref, flags, lists */
#define QRX_RSRC_REF_SIZE 12u       /* one reference list entry */
#define QRX_CFRG_TYPE 0x63667267u   /* 'cfrg' */
#define QRX_CFRG_SIZE 12u           /* reserved word, offset, length */

typedef enum {
    QRX_MACAPP_OK = 0,
    QRX_MACAPP_ERR_ARG,     /* NULL pointer or impossible size */
    QRX_MACAPP_ERR_HEADER,  /* version byte, name length or fork length */
    QRX_MACAPP_ERR_CRC,     /* MacBinary II header CRC mismatch */
    QRX_MACAPP_ERR_SHORT,   /* image or resource data truncated */
    QRX_MACAPP_ERR_RSRC,    /* resource header, map or type list invalid */
    QRX_MACAPP_ERR_NO_CFRG, /* no 'cfrg' resource with the requested id */
    QRX_MACAPP_ERR_CFRG,    /* 'cfrg' payload malformed or outside the fork */
    QRX_MACAPP_ERR_PEF      /* the fragment itself was rejected */
} qrx_macapp_status_t;

typedef struct {
    const uint8_t *data;        /* data fork inside the image, not copied */
    uint32_t data_len;
    const uint8_t *rsrc;        /* resource fork inside the image */
    uint32_t rsrc_len;
    uint32_t file_type;         /* Finder type, e.g. 'APPL' */
    uint32_t file_creator;
    unsigned name_len;
    char name[QRX_MACBIN_NAME_MAX + 1u];
    int has_crc;                /* MacBinary II header (CRC checked) */
} qrx_macbin_t;

typedef struct {
    uint32_t offset;            /* into the data fork */
    uint32_t length;
} qrx_cfrg_t;

/* Validate the 128-byte header and locate both forks.  The image must stay
 * valid as long as the returned pointers are used. */
qrx_macapp_status_t qrx_macbin_parse(qrx_macbin_t *mb, const uint8_t *image,
                                     uint32_t size);

/* Find one resource by type and id in a resource fork; the result points at
 * the resource data without its 4-byte length prefix. */
qrx_macapp_status_t qrx_rsrc_find(const uint8_t *rsrc, uint32_t size,
                                  uint32_t type, int id, const uint8_t **out,
                                  uint32_t *out_len);

/* Decode a 'cfrg' resource into the fragment's position in the data fork.
 * fork_size is the data fork length, because the payload only names the
 * position; the fragment must lie inside that fork. */
qrx_macapp_status_t qrx_cfrg_parse(qrx_cfrg_t *cfrg, const uint8_t *data,
                                   uint32_t size, uint32_t fork_size);

/* Parse an application file, load and relocate the fragment it carries.
 * pef->data points into image, so the image has to outlive the fragment. */
qrx_macapp_status_t qrx_app_load(qrx_pef_t *pef, qrx_ppc_t *ppc, uint32_t base,
                                 const uint8_t *image, uint32_t size,
                                 qrx_pef_resolver_t resolver, void *ctx);

#endif /* QRX_MACAPP_H */
