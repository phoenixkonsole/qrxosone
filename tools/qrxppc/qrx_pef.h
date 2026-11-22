/*
 * QRXPPC - PEF/Code Fragment Manager container reader (P2.1).
 *
 * Reads the container structures documented in "MacOS Runtime Architectures":
 * container header, section headers, loader header, imported libraries and
 * symbols, and the export tables.  Sections are mapped into the flat PPC
 * address space; relocations, packed sections and Toolbox calls are not
 * implemented yet and are reported as explicit errors.
 */
#ifndef QRX_PEF_H
#define QRX_PEF_H

#include "qrx_ppc.h"

#define QRX_PEF_MAX_SECTIONS 16
#define QRX_PEF_MAX_EXPORTS 256
#define QRX_PEF_MAX_IMPORTS 1024
#define QRX_PEF_MAX_LIBRARIES 16
#define QRX_PEF_MAX_RELOC_SECTIONS 16
#define QRX_PEF_SECTION_STRIDE 0x1000u

typedef enum {
    QRX_PEF_OK = 0,
    QRX_PEF_ERR_SHORT,      /* container or table truncated */
    QRX_PEF_ERR_MAGIC,      /* not a Joy!peff container */
    QRX_PEF_ERR_VERSION,    /* unsupported formatVersion */
    QRX_PEF_ERR_ARCH,       /* not a pwpc container */
    QRX_PEF_ERR_SECTIONS,   /* section count or header table invalid */
    QRX_PEF_ERR_OFFSET,     /* section data outside the container */
    QRX_PEF_ERR_PACKED,     /* packed section (not supported yet) */
    QRX_PEF_ERR_LOADER,     /* loader section missing or malformed */
    QRX_PEF_ERR_EXPORTS,    /* export table invalid or too large */
    QRX_PEF_ERR_IMPORTS,    /* import table invalid or too large */
    QRX_PEF_ERR_MEMORY,     /* guest memory too small for a section */
    QRX_PEF_ERR_RELOC,      /* relocation stream or header invalid */
    QRX_PEF_ERR_UNRESOLVED, /* import resolver returned no address */
    QRX_PEF_ERR_NOT_LOADED  /* sections were not mapped yet */
} qrx_pef_status_t;

typedef struct {
    int32_t name_offset;
    uint32_t default_address;
    uint32_t total_size;
    uint32_t unpacked_size;
    uint32_t packed_size;
    uint32_t container_offset;
    uint32_t address;          /* runtime address set by qrx_pef_load() */
    uint8_t kind;
    uint8_t share_kind;
    uint8_t alignment;
} qrx_pef_section_t;

typedef struct {
    uint8_t symbol_class;
    uint32_t name_offset;      /* into the loader string table */
    uint32_t value;            /* section-relative offset or absolute */
    int32_t section_index;     /* negative values are special (TOC/absolute) */
} qrx_pef_export_t;

typedef struct {
    int32_t library_index;     /* -1 if no library claims the symbol */
    uint8_t symbol_class;
    uint32_t name_offset;
} qrx_pef_import_t;

typedef struct {
    uint16_t section_index;
    uint32_t reloc_count;        /* number of 16-bit relocation blocks */
    uint32_t first_reloc_offset; /* relative to the relocation area */
} qrx_pef_reloc_header_t;

typedef struct {
    uint32_t name_offset;
    uint32_t old_imp_version;
    uint32_t current_version;
    uint32_t symbol_count;
    uint32_t first_symbol;
} qrx_pef_library_t;

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t format_version;
    uint16_t section_count;
    uint16_t inst_section_count;
    qrx_pef_section_t sections[QRX_PEF_MAX_SECTIONS];
    int loader_index;          /* -1 if absent */
    const uint8_t *loader;
    uint32_t loader_size;
    uint32_t loader_strings_offset;
    int32_t main_section;
    uint32_t main_offset;
    int32_t init_section;
    uint32_t init_offset;
    int32_t term_section;
    uint32_t term_offset;
    uint32_t library_count;
    qrx_pef_library_t libraries[QRX_PEF_MAX_LIBRARIES];
    uint32_t import_count;
    qrx_pef_import_t imports[QRX_PEF_MAX_IMPORTS];
    uint32_t export_count;
    uint32_t export_hash_power;
    uint32_t reloc_section_count;
    uint32_t reloc_instr_offset;
    qrx_pef_reloc_header_t reloc_headers[QRX_PEF_MAX_RELOC_SECTIONS];
    qrx_pef_export_t exports[QRX_PEF_MAX_EXPORTS];
} qrx_pef_t;

/* Validate a complete container and fill the description. */
qrx_pef_status_t qrx_pef_parse(qrx_pef_t *pef, const uint8_t *data,
                               uint32_t size);

/* Copy every unpacked section to base + index * QRX_PEF_SECTION_STRIDE. */
qrx_pef_status_t qrx_pef_load(qrx_pef_t *pef, qrx_ppc_t *ppc, uint32_t base);

/* Loader string table lookup; NULL when the offset or its terminator is out
 * of range. */
const char *qrx_pef_string(const qrx_pef_t *pef, uint32_t name_offset);

/* Export lookup by class (-1 accepts any class); index or -1. */
int qrx_pef_find_export(const qrx_pef_t *pef, int symbol_class,
                        const char *name);

/* Runtime address of a section (0 before qrx_pef_load() or when invalid). */
uint32_t qrx_pef_section_address(const qrx_pef_t *pef, int index);

/* Runtime address of the main transition vector (0 if the fragment has no
 * main symbol).  The entry point itself is the first word at that address. */
uint32_t qrx_pef_main_tvector(const qrx_pef_t *pef);

/* Runtime addresses of the fragment's init and term transition vectors, 0
 * when the container has none.  CFM runs init after loading and term before
 * unloading; both use the same two-word layout as the main vector. */
uint32_t qrx_pef_init_tvector(const qrx_pef_t *pef);
uint32_t qrx_pef_term_tvector(const qrx_pef_t *pef);

/* Resolve one imported symbol.  library is the importing library name, or ""
 * when the container names none.  Returning 0 marks the import unresolved. */
typedef uint32_t (*qrx_pef_resolver_t)(void *ctx, const char *library,
                                       uint8_t symbol_class, const char *name);

/* Apply every section's relocation instructions.  All imports are resolved
 * before the first write, so an unresolved symbol leaves memory untouched. */
qrx_pef_status_t qrx_pef_relocate(qrx_pef_t *pef, qrx_ppc_t *ppc,
                                  qrx_pef_resolver_t resolver, void *ctx);

#endif /* QRX_PEF_H */
