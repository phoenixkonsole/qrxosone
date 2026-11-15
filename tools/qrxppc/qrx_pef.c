/*
 * QRXPPC - PEF/Code Fragment Manager container reader (P2.1).
 *
 * The reader validates a Joy!peff container for the pwpc architecture and
 * describes its sections, loader header, imported libraries/symbols and
 * exported symbols.  qrx_pef_load() maps the unpacked sections into the flat
 * PPC address space.
 *
 * Deliberate limits (each reported as a distinct status, never as a crash):
 *   - packed sections are rejected (QRX_PEF_ERR_PACKED),
 *   - relocation instructions are parsed only structurally; references are
 *     not patched yet,
 *   - only pwpc containers are accepted (no CFM-68K),
 *   - no Toolbox, MacBinary or resource-fork support.
 */
#include "qrx_pef.h"

#include <string.h>

#define PEF_HEADER_SIZE 40u
#define PEF_SECTION_HEADER_SIZE 28u
#define PEF_LOADER_HEADER_SIZE 56u
#define PEF_LIBRARY_SIZE 24u
#define PEF_IMPORT_SYMBOL_SIZE 4u
#define PEF_EXPORT_SYMBOL_SIZE 10u
#define PEF_RELOC_HEADER_SIZE 12u
#define PEF_KIND_CODE 0u
#define PEF_KIND_DATA 1u
#define PEF_KIND_PACKED_DATA 2u
#define PEF_KIND_LOADER 4u
#define PEF_HASH_POWER_MAX 16u

/* Container size of a section: code and unpacked data store unpackedSize
 * bytes, a pattern-initialized (packed) data section and the loader section
 * store packedSize bytes. */
static uint32_t pef_section_container_size(const qrx_pef_section_t *s)
{
    if (s->kind == PEF_KIND_LOADER || s->kind == PEF_KIND_PACKED_DATA)
        return s->packed_size != 0u ? s->packed_size : s->unpacked_size;
    return s->unpacked_size;
}

#define PEF_TAG1 0x4A6F7921u     /* 'Joy!' */
#define PEF_TAG2 0x70656666u     /* 'peff' */
#define PEF_ARCH_PWPC 0x70777063u /* 'pwpc' */

static uint16_t pef_rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t pef_rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int32_t pef_rds32(const uint8_t *p)
{
    return (int32_t)pef_rd32(p);
}

static int pef_section_in_range(const qrx_pef_t *pef, int32_t index)
{
    return index >= 0 && index < (int32_t)pef->section_count;
}

const char *qrx_pef_string(const qrx_pef_t *pef, uint32_t name_offset)
{
    uint64_t start;

    if (!pef || !pef->loader)
        return 0;
    /* PEF stores symbol and library names as offsets into the loader string
     * table, not into the loader section. */
    start = (uint64_t)pef->loader_strings_offset + (uint64_t)name_offset;
    if (start >= (uint64_t)pef->loader_size)
        return 0;
    if (memchr(pef->loader + (uint32_t)start, '\0',
               pef->loader_size - (uint32_t)start) == 0)
        return 0;
    return (const char *)(pef->loader + (uint32_t)start);
}

qrx_pef_status_t qrx_pef_parse(qrx_pef_t *pef, const uint8_t *data,
                               uint32_t size)
{
    const uint8_t *loader;
    uint32_t loader_size;
    uint32_t strings_offset;
    uint32_t hash_offset;
    uint32_t hash_bytes;
    uint32_t symbol_base;
    uint32_t import_base;
    uint64_t export_end;
    uint32_t i;
    uint32_t j;
    unsigned loader_count = 0;

    if (!pef || !data)
        return QRX_PEF_ERR_SHORT;
    memset(pef, 0, sizeof(*pef));
    pef->loader_index = -1;
    pef->main_section = -1;
    pef->init_section = -1;
    pef->term_section = -1;
    pef->data = data;
    pef->size = size;

    if (size < PEF_HEADER_SIZE)
        return QRX_PEF_ERR_SHORT;
    if (pef_rd32(data) != PEF_TAG1 || pef_rd32(data + 4u) != PEF_TAG2)
        return QRX_PEF_ERR_MAGIC;
    if (pef_rd32(data + 8u) != PEF_ARCH_PWPC)
        return QRX_PEF_ERR_ARCH;
    pef->format_version = pef_rd32(data + 12u);
    if (pef->format_version != 1u)
        return QRX_PEF_ERR_VERSION;
    pef->section_count = pef_rd16(data + 32u);
    pef->inst_section_count = pef_rd16(data + 34u);
    if (pef->section_count == 0u ||
        pef->section_count > (uint16_t)QRX_PEF_MAX_SECTIONS)
        return QRX_PEF_ERR_SECTIONS;
    if (size < PEF_HEADER_SIZE +
               (uint32_t)pef->section_count * PEF_SECTION_HEADER_SIZE)
        return QRX_PEF_ERR_SHORT;

    for (i = 0; i < pef->section_count; i++) {
        const uint8_t *h = data + PEF_HEADER_SIZE +
                           i * PEF_SECTION_HEADER_SIZE;
        qrx_pef_section_t *s = &pef->sections[i];

        s->name_offset = pef_rds32(h);
        s->default_address = pef_rd32(h + 4u);
        s->total_size = pef_rd32(h + 8u);
        s->unpacked_size = pef_rd32(h + 12u);
        s->packed_size = pef_rd32(h + 16u);
        s->container_offset = pef_rd32(h + 20u);
        s->kind = h[24];
        s->share_kind = h[25];
        s->alignment = h[26];
        /* Pattern-initialized data is expanded by qrx_pef_load(); every other
         * kind keeps its contents verbatim, so packedSize must agree with
         * unpackedSize there (a mismatch is a damaged section header). */
        if (s->kind != PEF_KIND_PACKED_DATA && s->kind != PEF_KIND_LOADER &&
            s->packed_size != 0u && s->packed_size != s->unpacked_size)
            return QRX_PEF_ERR_PACKED;
        if (s->kind == PEF_KIND_PACKED_DATA &&
            s->unpacked_size == 0u)
            return QRX_PEF_ERR_PACKED;
        if (s->container_offset > size ||
            size - s->container_offset < pef_section_container_size(s))
            return QRX_PEF_ERR_OFFSET;
        if (s->kind == PEF_KIND_LOADER) {
            pef->loader_index = (int)i;
            loader_count++;
        }
    }
    if (loader_count != 1u)
        return QRX_PEF_ERR_LOADER;

    loader = data + pef->sections[pef->loader_index].container_offset;
    loader_size = pef_section_container_size(&pef->sections[pef->loader_index]);
    if (loader_size < PEF_LOADER_HEADER_SIZE)
        return QRX_PEF_ERR_LOADER;
    pef->loader = loader;
    pef->loader_size = loader_size;

    pef->main_section = pef_rds32(loader);
    pef->main_offset = pef_rd32(loader + 4u);
    pef->init_section = pef_rds32(loader + 8u);
    pef->init_offset = pef_rd32(loader + 12u);
    pef->term_section = pef_rds32(loader + 16u);
    pef->term_offset = pef_rd32(loader + 20u);
    pef->library_count = pef_rd32(loader + 24u);
    pef->import_count = pef_rd32(loader + 28u);
    pef->reloc_section_count = pef_rd32(loader + 32u);
    strings_offset = pef_rd32(loader + 40u);
    hash_offset = pef_rd32(loader + 44u);
    pef->export_hash_power = pef_rd32(loader + 48u);
    pef->export_count = pef_rd32(loader + 52u);

    if (pef->main_section < -1 ||
        pef->main_section >= (int32_t)pef->section_count ||
        pef->init_section < -1 ||
        pef->init_section >= (int32_t)pef->section_count ||
        pef->term_section < -1 ||
        pef->term_section >= (int32_t)pef->section_count)
        return QRX_PEF_ERR_LOADER;
    if (pef_section_in_range(pef, pef->main_section) &&
        pef->main_offset > pef->sections[pef->main_section].unpacked_size)
        return QRX_PEF_ERR_LOADER;
    if (pef_section_in_range(pef, pef->init_section) &&
        pef->init_offset > pef->sections[pef->init_section].unpacked_size)
        return QRX_PEF_ERR_LOADER;
    if (pef_section_in_range(pef, pef->term_section) &&
        pef->term_offset > pef->sections[pef->term_section].unpacked_size)
        return QRX_PEF_ERR_LOADER;
    if (strings_offset >= loader_size)
        return QRX_PEF_ERR_LOADER;
    pef->loader_strings_offset = strings_offset;

    if (pef->export_count > (uint32_t)QRX_PEF_MAX_EXPORTS ||
        pef->export_hash_power > PEF_HASH_POWER_MAX)
        return QRX_PEF_ERR_EXPORTS;

    /* Layout: hash slots, key table, then 10-byte exported symbol entries. */
    hash_bytes = 4u << pef->export_hash_power;
    symbol_base = hash_offset + hash_bytes +
                  pef->export_count * 4u;
    export_end = (uint64_t)hash_offset + hash_bytes +
                 (uint64_t)pef->export_count * 4u +
                 (uint64_t)pef->export_count * PEF_EXPORT_SYMBOL_SIZE;
    if (hash_offset > loader_size || export_end > loader_size)
        return QRX_PEF_ERR_EXPORTS;

    for (i = 0; i < pef->export_count; i++) {
        const uint8_t *e = loader + symbol_base + i * PEF_EXPORT_SYMBOL_SIZE;
        qrx_pef_export_t *x = &pef->exports[i];
        uint32_t class_and_name = pef_rd32(e);

        x->symbol_class = (uint8_t)(class_and_name >> 24);
        x->name_offset = class_and_name & 0x00FFFFFFu;
        x->value = pef_rd32(e + 4u);
        x->section_index = (int32_t)(int16_t)pef_rd16(e + 8u);
        if (!qrx_pef_string(pef, x->name_offset))
            return QRX_PEF_ERR_EXPORTS;
        if (x->section_index >= (int32_t)pef->section_count)
            return QRX_PEF_ERR_EXPORTS;
        if (pef_section_in_range(pef, x->section_index) &&
            x->value > pef->sections[x->section_index].unpacked_size)
            return QRX_PEF_ERR_EXPORTS;
    }

    if (pef->library_count > (uint32_t)QRX_PEF_MAX_LIBRARIES ||
        pef->import_count > (uint32_t)QRX_PEF_MAX_IMPORTS)
        return QRX_PEF_ERR_IMPORTS;
    import_base = PEF_LOADER_HEADER_SIZE +
                  pef->library_count * PEF_LIBRARY_SIZE;
    if ((uint64_t)import_base + (uint64_t)pef->import_count *
                                 PEF_IMPORT_SYMBOL_SIZE > loader_size)
        return QRX_PEF_ERR_IMPORTS;

    for (i = 0; i < pef->library_count; i++) {
        const uint8_t *l = loader + PEF_LOADER_HEADER_SIZE +
                           i * PEF_LIBRARY_SIZE;
        qrx_pef_library_t *lib = &pef->libraries[i];

        lib->name_offset = pef_rd32(l);
        lib->old_imp_version = pef_rd32(l + 4u);
        lib->current_version = pef_rd32(l + 8u);
        lib->symbol_count = pef_rd32(l + 12u);
        lib->first_symbol = pef_rd32(l + 16u);
        if (lib->first_symbol > pef->import_count ||
            pef->import_count - lib->first_symbol < lib->symbol_count)
            return QRX_PEF_ERR_IMPORTS;
        if (!qrx_pef_string(pef, lib->name_offset))
            return QRX_PEF_ERR_IMPORTS;
    }

    for (i = 0; i < pef->import_count; i++) {
        uint32_t class_and_name = pef_rd32(loader + import_base +
                                           i * PEF_IMPORT_SYMBOL_SIZE);

        pef->imports[i].symbol_class = (uint8_t)(class_and_name >> 24);
        pef->imports[i].name_offset = class_and_name & 0x00FFFFFFu;
        pef->imports[i].library_index = -1;
        if (!qrx_pef_string(pef, pef->imports[i].name_offset))
            return QRX_PEF_ERR_IMPORTS;
        for (j = 0; j < pef->library_count; j++) {
            const qrx_pef_library_t *lib = &pef->libraries[j];

            if (i >= lib->first_symbol &&
                i - lib->first_symbol < lib->symbol_count) {
                pef->imports[i].library_index = (int32_t)j;
                break;
            }
        }
    }

    pef->reloc_instr_offset = pef_rd32(loader + 36u);
    if (pef->reloc_section_count > (uint32_t)QRX_PEF_MAX_RELOC_SECTIONS)
        return QRX_PEF_ERR_RELOC;
    {
        uint32_t header_base = import_base +
                               pef->import_count * PEF_IMPORT_SYMBOL_SIZE;

        if ((uint64_t)header_base + (uint64_t)pef->reloc_section_count *
                                    PEF_RELOC_HEADER_SIZE > loader_size)
            return QRX_PEF_ERR_RELOC;
        for (i = 0; i < pef->reloc_section_count; i++) {
            const uint8_t *h = loader + header_base +
                               i * PEF_RELOC_HEADER_SIZE;
            qrx_pef_reloc_header_t *rh = &pef->reloc_headers[i];

            rh->section_index = pef_rd16(h);
            rh->reloc_count = pef_rd32(h + 4u);
            rh->first_reloc_offset = pef_rd32(h + 8u);
            if (rh->section_index >= pef->section_count)
                return QRX_PEF_ERR_RELOC;
            if ((uint64_t)pef->reloc_instr_offset +
                (uint64_t)rh->first_reloc_offset +
                (uint64_t)rh->reloc_count * 2u > loader_size)
                return QRX_PEF_ERR_RELOC;
        }
    }

    return QRX_PEF_OK;
}


/* ------------------------------------------------------------------ */
/* Pattern-initialized data (PEF section kind 2)                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
} pef_pattern_t;

static int pef_pattern_vnum(pef_pattern_t *p, uint32_t *value)
{
    uint32_t v = 0;
    unsigned bytes = 0;

    for (;;) {
        uint8_t c;

        if (p->pos >= p->size || bytes++ >= 5u)
            return 0;
        c = p->data[p->pos++];
        v = (v << 7) | (uint32_t)(c & 0x7Fu);
        if ((c & 0x80u) == 0u) {
            *value = v;
            return 1;
        }
    }
}

/* Expands the five pattern opcodes documented in Mac OS Runtime
 * Architectures, "Section Contents": zero run, block copy, repeated block and
 * the two interleaved repeats (block/zero).  Every read and every write is
 * bounded by packedSize/unpackedSize, so a damaged description fails with
 * QRX_PEF_ERR_PACKED instead of overrunning guest memory. */
static qrx_pef_status_t pef_unpack_section(const uint8_t *src, uint32_t packed,
                                           uint8_t *dst, uint32_t unpacked)
{
    pef_pattern_t p;
    uint32_t out = 0;

    p.data = src;
    p.size = packed;
    p.pos = 0;
    while (p.pos < p.size) {
        uint8_t head = src[p.pos++];
        uint32_t op = (uint32_t)(head >> 5);
        uint32_t cnt = (uint32_t)(head & 0x1Fu);
        uint32_t rep, cust, k;

        if (cnt == 0u && !pef_pattern_vnum(&p, &cnt))
            return QRX_PEF_ERR_PACKED;
        if (cnt > unpacked - out)
            return QRX_PEF_ERR_PACKED;

        switch (op) {
        case 0u:                                    /* zero run */
            memset(dst + out, 0, cnt);
            out += cnt;
            break;
        case 1u:                                    /* block copy */
            if (cnt > p.size - p.pos)
                return QRX_PEF_ERR_PACKED;
            memcpy(dst + out, src + p.pos, cnt);
            p.pos += cnt;
            out += cnt;
            break;
        case 2u:                                    /* repeated block */
            if (!pef_pattern_vnum(&p, &rep) || cnt > p.size - p.pos)
                return QRX_PEF_ERR_PACKED;
            for (k = 0; k <= rep; k++) {
                if (cnt > unpacked - out)
                    return QRX_PEF_ERR_PACKED;
                memcpy(dst + out, src + p.pos, cnt);
                out += cnt;
            }
            p.pos += cnt;
            break;
        case 3u:                                    /* repeat with block */
        case 4u:                                    /* repeat with zero */
        {
            uint32_t common_at;

            /* The common block (cnt literal bytes, or cnt zeros for op 4) is
             * written once, then rep times a custom block followed by the same
             * common block; the custom blocks follow the common one in the
             * stream. */
            if (!pef_pattern_vnum(&p, &cust) ||
                !pef_pattern_vnum(&p, &rep))
                return QRX_PEF_ERR_PACKED;
            if (op == 3u && cnt > p.size - p.pos)
                return QRX_PEF_ERR_PACKED;
            common_at = p.pos;
            if (op == 3u)
                memcpy(dst + out, src + common_at, cnt);
            else
                memset(dst + out, 0, cnt);
            out += cnt;
            /* op 4 stores no common bytes: only the custom blocks follow. */
            p.pos = common_at + (op == 3u ? cnt : 0u);
            for (k = 0; k < rep; k++) {
                if (cust > p.size - p.pos ||
                    (uint64_t)cust + cnt > (uint64_t)(unpacked - out))
                    return QRX_PEF_ERR_PACKED;
                memcpy(dst + out, src + p.pos, cust);
                p.pos += cust;
                out += cust;
                if (op == 3u)
                    memcpy(dst + out, src + common_at, cnt);
                else
                    memset(dst + out, 0, cnt);
                out += cnt;
            }
            break;
        }
        default:
            return QRX_PEF_ERR_PACKED;
        }
    }
    if (out != unpacked)
        return QRX_PEF_ERR_PACKED;
    return QRX_PEF_OK;
}

/* Sections are laid out one after another, each starting on a 4 KiB
 * boundary: a real fragment has a 200 KiB code section, so a fixed stride per
 * section index would let the code overlap the data section. */
static uint64_t pef_section_layout(const qrx_pef_t *pef, uint32_t base,
                                   unsigned index)
{
    uint64_t cursor = base;
    unsigned i;

    for (i = 0; i < index; i++) {
        uint64_t size = pef->sections[i].total_size;

        if (size < pef->sections[i].unpacked_size)
            size = pef->sections[i].unpacked_size;
        cursor = (cursor + size + 0xFFFu) & ~(uint64_t)0xFFFu;
    }
    return cursor;
}

qrx_pef_status_t qrx_pef_load(qrx_pef_t *pef, qrx_ppc_t *ppc, uint32_t base)
{
    uint32_t i;

    if (!pef || !ppc || !ppc->mem.data)
        return QRX_PEF_ERR_MEMORY;

    /* Validate every placement before copying anything, so a rejected
     * container leaves guest memory untouched. */
    for (i = 0; i < pef->section_count; i++) {
        const qrx_pef_section_t *s = &pef->sections[i];
        uint64_t addr = pef_section_layout(pef, base, i);

        if (addr > (uint64_t)ppc->mem.size ||
            (uint64_t)ppc->mem.size - addr < (uint64_t)s->unpacked_size)
            return QRX_PEF_ERR_MEMORY;
    }
    for (i = 0; i < pef->section_count; i++) {
        qrx_pef_section_t *s = &pef->sections[i];
        uint32_t addr = (uint32_t)pef_section_layout(pef, base, i);
        qrx_pef_status_t st = QRX_PEF_OK;

        if (s->unpacked_size != 0u)
            memset(ppc->mem.data + addr, 0, s->unpacked_size);
        if (s->kind == PEF_KIND_PACKED_DATA) {
            st = pef_unpack_section(pef->data + s->container_offset,
                                    s->packed_size, ppc->mem.data + addr,
                                    s->unpacked_size);
            if (st != QRX_PEF_OK)
                return st;
        } else if (s->unpacked_size != 0u) {
            memcpy(ppc->mem.data + addr, pef->data + s->container_offset,
                   s->unpacked_size);
        }
        s->address = addr;
    }
    return QRX_PEF_OK;
}

int qrx_pef_find_export(const qrx_pef_t *pef, int symbol_class,
                        const char *name)
{
    uint32_t i;

    if (!pef || !name)
        return -1;
    for (i = 0; i < pef->export_count; i++) {
        const qrx_pef_export_t *e = &pef->exports[i];
        const char *candidate;

        if (symbol_class >= 0 && e->symbol_class != (uint8_t)symbol_class)
            continue;
        candidate = qrx_pef_string(pef, e->name_offset);
        if (candidate && strcmp(candidate, name) == 0)
            return (int)i;
    }
    return -1;
}

uint32_t qrx_pef_section_address(const qrx_pef_t *pef, int index)
{
    if (!pef || index < 0 || index >= (int)pef->section_count)
        return 0;
    return pef->sections[index].address;
}

static uint32_t pef_side_tvector(const qrx_pef_t *pef, int section,
                                 uint32_t offset)
{
    if (!pef || !pef_section_in_range(pef, section))
        return 0;
    if (pef->sections[section].address == 0u)
        return 0;
    if (offset > pef->sections[section].unpacked_size ||
        pef->sections[section].unpacked_size - offset < 8u)
        return 0;
    return pef->sections[section].address + offset;
}

uint32_t qrx_pef_main_tvector(const qrx_pef_t *pef)
{
    return pef ? pef_side_tvector(pef, pef->main_section, pef->main_offset)
               : 0u;
}

uint32_t qrx_pef_init_tvector(const qrx_pef_t *pef)
{
    return pef ? pef_side_tvector(pef, pef->init_section, pef->init_offset)
               : 0u;
}

uint32_t qrx_pef_term_tvector(const qrx_pef_t *pef)
{
    return pef ? pef_side_tvector(pef, pef->term_section, pef->term_offset)
               : 0u;
}


/* ------------------------------------------------------------------ */
/* Relocation engine (PEF relocation instruction set)                  */
/* ------------------------------------------------------------------ */

#define PEF_RELOC_MAX_INSNS 64

typedef struct {
    const qrx_pef_t *pef;
    uint8_t *mem;
    uint32_t mem_size;
    uint32_t base[QRX_PEF_MAX_SECTIONS];
    uint32_t size[QRX_PEF_MAX_SECTIONS];
    uint32_t section_c;
    uint32_t section_d;
    uint32_t current_section;
    uint32_t address;
    const uint32_t *imports;
    uint32_t import_count;
    uint32_t import_index;
    int bad;
    unsigned hist_count;
    struct {
        uint32_t offset;
        uint32_t blocks;
    } hist[PEF_RELOC_MAX_INSNS];
} pef_reloc_t;

static int pef_reloc_add_word(pef_reloc_t *r, uint32_t value)
{
    uint32_t base = r->base[r->current_section];
    uint32_t size = r->size[r->current_section];
    uint32_t word;

    if (r->address < base || r->address - base > size ||
        size - (r->address - base) < 4u) {
        r->bad = 1;
        return 0;
    }
    word = pef_rd32(r->mem + r->address) + value;
    r->mem[r->address + 0u] = (uint8_t)(word >> 24);
    r->mem[r->address + 1u] = (uint8_t)(word >> 16);
    r->mem[r->address + 2u] = (uint8_t)(word >> 8);
    r->mem[r->address + 3u] = (uint8_t)word;
    return 1;
}

static int pef_reloc_set_position(pef_reloc_t *r, uint32_t offset)
{
    uint32_t base = r->base[r->current_section];
    uint32_t size = r->size[r->current_section];

    if (offset > size || size - offset < 4u) {
        r->bad = 1;
        return 0;
    }
    r->address = base + offset;
    return 1;
}

static uint32_t pef_reloc_import(pef_reloc_t *r, uint32_t index)
{
    if (index >= r->import_count) {
        r->bad = 1;
        return 0;
    }
    return r->imports[index];
}

static void pef_reloc_exec(pef_reloc_t *r, uint32_t off, uint32_t end,
                           unsigned record, unsigned depth);

static void pef_reloc_repeat(pef_reloc_t *r, uint32_t start, uint32_t chunks,
                             uint32_t times, unsigned record, unsigned depth)
{
    uint32_t acc = 0;
    uint32_t rep;
    int k;

    if (!record || depth >= 3u || times == 0u || times > 4096u) {
        r->bad = 1;
        return;
    }
    k = (int)r->hist_count;
    while (k > 0 && acc < chunks) {
        k--;
        acc += r->hist[(unsigned)k % PEF_RELOC_MAX_INSNS].blocks;
    }
    if (acc != chunks || r->hist_count - (unsigned)k > PEF_RELOC_MAX_INSNS) {
        r->bad = 1;
        return;
    }
    for (rep = 0; rep < times && !r->bad; rep++)
        pef_reloc_exec(r, r->hist[(unsigned)k % PEF_RELOC_MAX_INSNS].offset,
                       start, 0, depth + 1u);
}

static void pef_reloc_exec(pef_reloc_t *r, uint32_t off, uint32_t end,
                           unsigned record, unsigned depth)
{
    while (off + 2u <= end && !r->bad) {
        const uint8_t *p = r->pef->loader;
        uint32_t start = off;
        uint32_t w = pef_rd16(p + off);
        uint32_t blocks = 1;
        uint32_t i;

        if ((w & 0xC000u) == 0x0000u) {
            uint32_t skip = (w >> 6) & 0xFFu;
            uint32_t count = w & 0x3Fu;

            r->address += skip * 4u;
            for (i = 0; i < count && !r->bad; i++) {
                pef_reloc_add_word(r, r->section_d);
                r->address += 4u;
            }
            off += 2u;
        } else if ((w & 0xE000u) == 0x4000u) {
            uint32_t sub = (w >> 9) & 0xFu;
            uint32_t n = (w & 0x1FFu) + 1u;

            off += 2u;
            for (i = 0; i < n && !r->bad; i++) {
                switch (sub) {
                case 0u:
                    pef_reloc_add_word(r, r->section_c);
                    r->address += 4u;
                    break;
                case 1u:
                    pef_reloc_add_word(r, r->section_d);
                    r->address += 4u;
                    break;
                case 2u:
                    pef_reloc_add_word(r, r->section_c);
                    r->address += 4u;
                    pef_reloc_add_word(r, r->section_d);
                    r->address += 8u;   /* word 1 plus the glue word */
                    break;
                case 3u:
                    pef_reloc_add_word(r, r->section_c);
                    r->address += 4u;
                    pef_reloc_add_word(r, r->section_d);
                    r->address += 4u;
                    break;
                case 4u:
                    pef_reloc_add_word(r, r->section_d);
                    r->address += 8u;
                    break;
                case 5u: {
                    uint32_t address = pef_reloc_import(r, r->import_index);

                    if (!r->bad) {
                        pef_reloc_add_word(r, address);
                        r->import_index++;
                        r->address += 4u;
                    }
                    break;
                }
                default:
                    r->bad = 1;
                    break;
                }
            }
        } else if ((w & 0xE000u) == 0x6000u) {
            uint32_t sub = (w >> 9) & 0xFu;
            uint32_t index = w & 0x1FFu;

            off += 2u;
            switch (sub) {
            case 0u: {
                uint32_t address = pef_reloc_import(r, index);

                if (!r->bad) {
                    pef_reloc_add_word(r, address);
                    r->address += 4u;
                    r->import_index = index + 1u;
                }
                break;
            }
            case 1u:
            case 2u:
            case 3u:
                if (index >= r->pef->section_count) {
                    r->bad = 1;
                } else if (sub == 1u) {
                    r->section_c = r->base[index] -
                                   r->pef->sections[index].default_address;
                } else if (sub == 2u) {
                    r->section_d = r->base[index] -
                                   r->pef->sections[index].default_address;
                } else {
                    pef_reloc_add_word(r, r->base[index]);
                    r->address += 4u;
                }
                break;
            default:
                r->bad = 1;
                break;
            }
        } else if ((w & 0xF000u) == 0x8000u) {
            r->address += (w & 0x0FFFu) + 1u;
            off += 2u;
        } else if ((w & 0xF000u) == 0x9000u) {
            uint32_t chunks = ((w >> 8) & 0xFu) + 1u;
            uint32_t times = (w & 0xFFu) + 1u;

            off += 2u;
            pef_reloc_repeat(r, start, chunks, times, record, depth);
        } else if ((w & 0xFC00u) == 0xA000u) {
            uint32_t offset;

            if (off + 4u > end) {
                r->bad = 1;
                break;
            }
            offset = ((w & 0x3FFu) << 16) | pef_rd16(p + off + 2u);
            blocks = 2;
            off += 4u;
            pef_reloc_set_position(r, offset);
        } else if ((w & 0xFC00u) == 0xA400u) {
            uint32_t index;
            uint32_t address;

            if (off + 4u > end) {
                r->bad = 1;
                break;
            }
            index = ((w & 0x3FFu) << 16) | pef_rd16(p + off + 2u);
            blocks = 2;
            off += 4u;
            address = pef_reloc_import(r, index);
            if (!r->bad) {
                pef_reloc_add_word(r, address);
                r->address += 4u;
                r->import_index = index + 1u;
            }
        } else if ((w & 0xFC00u) == 0xB000u) {
            uint32_t chunks;
            uint32_t times;

            if (off + 4u > end) {
                r->bad = 1;
                break;
            }
            chunks = ((w >> 6) & 0xFu) + 1u;
            times = ((w & 0x3Fu) << 16) | pef_rd16(p + off + 2u);
            blocks = 2;
            off += 4u;
            pef_reloc_repeat(r, start, chunks, times, record, depth);
        } else if ((w & 0xFC00u) == 0xB400u) {
            uint32_t sub = (w >> 6) & 0xFu;
            uint32_t index;

            if (off + 4u > end) {
                r->bad = 1;
                break;
            }
            index = ((w & 0x3Fu) << 16) | pef_rd16(p + off + 2u);
            blocks = 2;
            off += 4u;
            if (index >= r->pef->section_count || sub > 2u) {
                r->bad = 1;
            } else if (sub == 0u) {
                pef_reloc_add_word(r, r->base[index]);
                r->address += 4u;
            } else if (sub == 1u) {
                r->section_c = r->base[index] -
                               r->pef->sections[index].default_address;
            } else {
                r->section_d = r->base[index] -
                               r->pef->sections[index].default_address;
            }
        } else {
            r->bad = 1;
        }

        if (!r->bad && record) {
            r->hist[r->hist_count % PEF_RELOC_MAX_INSNS].offset = start;
            r->hist[r->hist_count % PEF_RELOC_MAX_INSNS].blocks = blocks;
            r->hist_count++;
        }
    }
}

qrx_pef_status_t qrx_pef_relocate(qrx_pef_t *pef, qrx_ppc_t *ppc,
                                  qrx_pef_resolver_t resolver, void *ctx)
{
    uint32_t resolved[QRX_PEF_MAX_IMPORTS];
    uint32_t i;
    uint32_t j;

    if (!pef || !ppc || !ppc->mem.data)
        return QRX_PEF_ERR_MEMORY;
    for (i = 0; i < pef->section_count; i++) {
        if (pef->sections[i].unpacked_size != 0u &&
            pef->sections[i].address == 0u)
            return QRX_PEF_ERR_NOT_LOADED;
    }
    if (pef->import_count != 0u && !resolver)
        return QRX_PEF_ERR_UNRESOLVED;
    for (i = 0; i < pef->import_count; i++) {
        const qrx_pef_import_t *imp = &pef->imports[i];
        const char *name = qrx_pef_string(pef, imp->name_offset);
        const char *library = "";
        uint32_t address;

        if (imp->library_index >= 0) {
            const qrx_pef_library_t *lib = &pef->libraries[imp->library_index];

            library = qrx_pef_string(pef, lib->name_offset);
        }
        address = resolver(ctx, library ? library : "", imp->symbol_class,
                           name ? name : "");
        if (address == 0u)
            return QRX_PEF_ERR_UNRESOLVED;
        resolved[i] = address;
    }
    for (i = 0; i < pef->reloc_section_count; i++) {
        const qrx_pef_reloc_header_t *rh = &pef->reloc_headers[i];
        pef_reloc_t r;

        memset(&r, 0, sizeof(r));
        r.pef = pef;
        r.mem = ppc->mem.data;
        r.mem_size = ppc->mem.size;
        r.imports = resolved;
        r.import_count = pef->import_count;
        r.current_section = rh->section_index;
        for (j = 0; j < pef->section_count; j++) {
            r.base[j] = pef->sections[j].address;
            r.size[j] = pef->sections[j].unpacked_size;
        }
        if (pef->section_count > 0u)
            r.section_c = r.base[0] - pef->sections[0].default_address;
        if (pef->section_count > 1u)
            r.section_d = r.base[1] - pef->sections[1].default_address;
        r.address = r.base[rh->section_index];
        pef_reloc_exec(&r, pef->reloc_instr_offset + rh->first_reloc_offset,
                       pef->reloc_instr_offset + rh->first_reloc_offset +
                       rh->reloc_count * 2u, 1u, 0u);
        if (r.bad)
            return QRX_PEF_ERR_RELOC;
    }
    return QRX_PEF_OK;
}
