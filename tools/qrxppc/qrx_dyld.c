#include "qrx_dyld.h"

#include <string.h>

#define SECTION_TYPE 0xffu
#define S_NON_LAZY_SYMBOL_POINTERS 0x6u
#define S_LAZY_SYMBOL_POINTERS 0x7u
#define N_TYPE 0x0eu
#define N_UNDF 0x0u
#define N_SECT 0xeu
#define N_STAB 0xe0u
#define INDIRECT_SYMBOL_LOCAL 0x80000000u
#define INDIRECT_SYMBOL_ABS 0x40000000u
#define VM_PROT_WRITE 2u
#define TRAMPOLINE_BYTES 12u
#define GLOBALS_BYTES 16u
#define MAX_TRAMPOLINE_CACHE 64u

const struct qrx_dyld_import qrx_dyld_default_imports[] = {
    { "_write", QRX_DYLD_SEL_WRITE },
    { "_exit", QRX_DYLD_SEL_EXIT },
    { "___error", QRX_DYLD_SEL_ERRNO },
    { "_strlen", QRX_DYLD_SEL_STRLEN }
};

const uint32_t qrx_dyld_default_import_count =
    (uint32_t)(sizeof(qrx_dyld_default_imports) /
               sizeof(qrx_dyld_default_imports[0]));

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static const uint8_t *symbol_entry(const qrx_macho_t *img, uint32_t index)
{
    return img->slice + img->symtab.symoff + index * 12u;
}

static const char *symbol_name(const qrx_macho_t *img, uint32_t index)
{
    const uint8_t *e = symbol_entry(img, index);
    uint32_t strx = be32(e);
    const char *s;
    uint32_t left;

    if (strx >= img->symtab.strsize)
        return NULL;
    left = img->symtab.strsize - strx;
    s = (const char *)(img->slice + img->symtab.stroff + strx);
    if (!memchr(s, '\0', left))
        return NULL;
    return s;
}

/* A pointer slot must live in a writable, mapped segment: __DATA and
 * __IMPORT qualify, __TEXT and __LINKEDIT do not. */
static int slot_writable(const qrx_macho_t *img, uint32_t addr)
{
    uint32_t i;

    for (i = 0; i < img->segment_count; i++) {
        const qrx_macho_segment_t *s = &img->segments[i];
        const uint32_t off = addr - s->vmaddr;

        if (addr < s->vmaddr || off >= s->vmsize)
            continue;
        if (s->vmsize - off < 4u)
            continue;
        return (s->initprot & VM_PROT_WRITE) != 0u;
    }
    return 0;
}

static int ranges_overlap(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
    if (alen == 0u || blen == 0u)
        return 0;
    return a < (uint64_t)b + blen && b < (uint64_t)a + alen;
}

static qrx_dyld_status_t check_trampoline_area(const qrx_macho_t *img,
                                               uint32_t base, uint32_t size,
                                               uint32_t memory_size)
{
    uint32_t i;

    if (!size || base > memory_size || size > memory_size - base ||
        (base & 3u) || size < GLOBALS_BYTES)
        return QRX_DYLD_ERR_SPACE;
    for (i = 0; i < img->segment_count; i++) {
        const qrx_macho_segment_t *s = &img->segments[i];

        if (s->vmsize &&
            ranges_overlap(base, size, s->vmaddr, s->vmsize))
            return QRX_DYLD_ERR_SPACE;
    }
    return QRX_DYLD_OK;
}

/* Point every lazy and non-lazy symbol pointer at its provider. */
static qrx_dyld_status_t bind_section(qrx_macho_t *img, const qrx_macho_section_t *sec,
                                      uint8_t *memory,
                                      const struct qrx_dyld_import *imports,
                                      uint32_t import_count,
                                      uint32_t *cursor, uint32_t limit,
                                      uint32_t *cache_import, uint32_t *cache_addr,
                                      uint32_t *cached, qrx_dyld_result_t *result)
{
    uint32_t count, k;

    if (sec->size & 3u)
        return QRX_DYLD_ERR_SYMBOLS;
    count = sec->size / 4u;
    if (count && (sec->reserved1 > img->symtab.nindirectsyms ||
                  count > img->symtab.nindirectsyms - sec->reserved1))
        return QRX_DYLD_ERR_SYMBOLS;
    for (k = 0; k < count; k++) {
        const uint8_t *indp = img->slice + img->symtab.indirectsymoff +
                              (sec->reserved1 + k) * 4u;
        uint32_t indirect = be32(indp);
        uint32_t slot = sec->addr + k * 4u;
        const uint8_t *sym;
        uint32_t type, value;

        if (slot < sec->addr)
            return QRX_DYLD_ERR_SYMBOLS;
        result->slots++;
        if (indirect & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) {
            result->skipped++;
            continue;
        }
        if (indirect >= img->symtab.nsyms)
            return QRX_DYLD_ERR_SYMBOLS;
        sym = symbol_entry(img, indirect);
        type = (uint32_t)sym[4] & N_TYPE;
        value = be32(sym + 8u);
        if (!slot_writable(img, slot))
            return QRX_DYLD_ERR_IMAGE;
        if (type == N_SECT) {
            /* Defined in the image: the linker's stubs expect its address. */
            put32(memory + slot, value);
            result->internal++;
            continue;
        }
        if (type != N_UNDF) {
            result->skipped++;
            continue;
        }
        {
            const char *name = symbol_name(img, indirect);
            uint32_t i, found = 0u, addr = 0u;

            if (!name)
                return QRX_DYLD_ERR_SYMBOLS;
            for (i = 0; i < *cached; i++) {
                if (imports[cache_import[i]].name &&
                    strcmp(imports[cache_import[i]].name, name) == 0) {
                    addr = cache_addr[i];
                    found = 1u;
                    break;
                }
            }
            for (i = 0; i < import_count && !found; i++) {
                if (!imports[i].name || strcmp(imports[i].name, name) != 0)
                    continue;
                if (imports[i].selector > 0x7fffu)
                    return QRX_DYLD_ERR_ARGUMENT;
                if (*cursor > limit || limit - *cursor < TRAMPOLINE_BYTES)
                    return QRX_DYLD_ERR_SPACE;
                /* li r0, 0x4000 | selector: the dispatcher never confuses this
                 * with the BSD syscall numbers of the static path. */
                put32(memory + *cursor,
                      0x38000000u | (QRX_DYLD_SEL_BASE | imports[i].selector));
                put32(memory + *cursor + 4u, 0x44000002u); /* sc */
                put32(memory + *cursor + 8u, 0x4e800020u); /* blr */
                addr = *cursor;
                *cursor += TRAMPOLINE_BYTES;
                if (*cached < MAX_TRAMPOLINE_CACHE) {
                    cache_import[*cached] = i;
                    cache_addr[*cached] = addr;
                    (*cached)++;
                }
                found = 1u;
            }
            if (!found) {
                if (result->unresolved == 0u) {
                    size_t n = strlen(name);
                    if (n > sizeof(result->missing) - 1u)
                        n = sizeof(result->missing) - 1u;
                    memcpy(result->missing, name, n);
                    result->missing[n] = '\0';
                }
                result->unresolved++;
                continue;
            }
            put32(memory + slot, addr);
            result->bound++;
        }
    }
    return QRX_DYLD_OK;
}

qrx_dyld_status_t qrx_dyld_bind(qrx_macho_t *img, uint8_t *memory,
                                uint32_t memory_size,
                                const struct qrx_dyld_import *imports,
                                uint32_t import_count,
                                uint32_t trampoline_base,
                                uint32_t trampoline_size,
                                qrx_dyld_result_t *result)
{
    qrx_dyld_result_t local;
    uint32_t cache_import[MAX_TRAMPOLINE_CACHE];
    uint32_t cache_addr[MAX_TRAMPOLINE_CACHE];
    uint32_t cached = 0u, cursor, limit, i;
    qrx_dyld_status_t st;

    if (!img || !memory || !result)
        return QRX_DYLD_ERR_ARGUMENT;
    memset(&local, 0, sizeof(local));
    memset(result, 0, sizeof(*result));
    if (!img->dynamic_start_supported)
        return QRX_DYLD_ERR_IMAGE;
    if (!img->symtab.present || !img->symtab.indirect ||
        !img->section_count || !img->symtab.nsyms)
        return QRX_DYLD_ERR_IMAGE;
    if (import_count && !imports)
        return QRX_DYLD_ERR_ARGUMENT;
    st = check_trampoline_area(img, trampoline_base, trampoline_size,
                               memory_size);
    if (st != QRX_DYLD_OK)
        return st;
    cursor = trampoline_base + GLOBALS_BYTES;
    limit = trampoline_base + trampoline_size;
    for (i = 0; i < img->section_count; i++) {
        const qrx_macho_section_t *sec = &img->sections[i];
        uint32_t type = sec->flags & SECTION_TYPE;

        if (type != S_LAZY_SYMBOL_POINTERS &&
            type != S_NON_LAZY_SYMBOL_POINTERS)
            continue;
        st = bind_section(img, sec, memory, imports, import_count, &cursor,
                          limit, cache_import, cache_addr, &cached, &local);
        *result = local;
        if (st != QRX_DYLD_OK)
            return st;
    }
    *result = local;
    if (local.unresolved)
        return QRX_DYLD_ERR_UNRESOLVED;
    if (local.slots == 0u)
        return QRX_DYLD_ERR_IMAGE;
    return QRX_DYLD_OK;
}

const char *qrx_dyld_status_string(qrx_dyld_status_t status)
{
    switch (status) {
    case QRX_DYLD_OK: return "ok";
    case QRX_DYLD_ERR_ARGUMENT: return "invalid argument";
    case QRX_DYLD_ERR_IMAGE: return "image not dynamically bindable";
    case QRX_DYLD_ERR_SYMBOLS: return "malformed symbol or indirect table";
    case QRX_DYLD_ERR_SPACE: return "trampoline area invalid or too small";
    case QRX_DYLD_ERR_UNRESOLVED: return "unresolved imported symbol";
    }
    return "unknown status";
}
