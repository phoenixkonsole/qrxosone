/*
 * QRXPPC - 68k classic MacOS runtime (P2.4).
 *
 * The CPU is Musashi, the same 68020 core the ARMithlon engine carries, but
 * here it runs a Macintosh address space instead of an Amiga one: low memory,
 * the A5 world, the 'CODE' segments and a heap.  There is no Mac ROM, so every
 * Toolbox entry the application calls arrives as an A-line trap and is answered
 * (or refused) by the HLE table in qrx_m68k_dispatch().
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "qrx_m68k.h"
#include "qrx_macapp.h"
#include "m68k.h"

#define QRX_M68K_RES_MAX 64u
#define QRX_M68K_TRAP_GESTALT_UNDEF 0xFFFFEA51u   /* -5551 */
#define QRX_M68K_TRAP_PARAM_ERR 0xFFFFFECEu     /* -50 */
#define QRX_M68K_TRAP_ENV_VERS_TOO_BIG 0xFFFFEA81u /* -5503 */
#define QRX_M68K_ERR_FNF 0xFFFFFFD5u            /* -43 fnfErr */
#define QRX_M68K_ERR_RES_F_NOT_FOUND 0xFFFFFF3Fu /* -193 resFNotFound */
#define QRX_M68K_ERR_IO 0xFFFFFFDCu             /* -36 ioErr */
#define QRX_M68K_ERR_TOO_MANY_FILES 0xFFFFFFD6u /* -42 tooManyFiles */

/* ParamBlockRec offsets the File Manager traps read and write. */
#define QRX_M68K_PB_RESULT 16u
#define QRX_M68K_PB_NAME 18u
#define QRX_M68K_PB_REFNUM 24u
#define QRX_M68K_PB_BUFFER 32u
#define QRX_M68K_PB_REQCOUNT 36u
#define QRX_M68K_PB_ACTCOUNT 40u
#define QRX_M68K_PB_POSMODE 44u
#define QRX_M68K_PB_POSOFFSET 46u

typedef struct {
    int id;
    const uint8_t *data;
    uint32_t len;
} qrx_m68k_res_t;

static qrx_m68k_t *g_m;      /* machine the CPU callbacks work on */
static int g_stop;           /* 0 while running, else a qrx_m68k_status_t */

/* ------------------------------------------------------------------ memory */

static uint32_t rd16(const uint8_t *mem, uint32_t size, uint32_t a)
{
    if (a + 2u > size)
        return 0u;
    return ((uint32_t)mem[a] << 8) | (uint32_t)mem[a + 1u];
}

static uint32_t rd32(const uint8_t *mem, uint32_t size, uint32_t a)
{
    if (a + 4u > size)
        return 0u;
    return ((uint32_t)mem[a] << 24) | ((uint32_t)mem[a + 1u] << 16) |
           ((uint32_t)mem[a + 2u] << 8) | (uint32_t)mem[a + 3u];
}

static void wr16(uint8_t *mem, uint32_t size, uint32_t a, uint32_t v)
{
    if (a + 2u > size)
        return;
    mem[a] = (uint8_t)(v >> 8);
    mem[a + 1u] = (uint8_t)v;
}

static void wr32(uint8_t *mem, uint32_t size, uint32_t a, uint32_t v)
{
    if (a + 4u > size)
        return;
    mem[a] = (uint8_t)(v >> 24);
    mem[a + 1u] = (uint8_t)(v >> 16);
    mem[a + 2u] = (uint8_t)(v >> 8);
    mem[a + 3u] = (uint8_t)v;
}

/* Musashi calls these for every access; keep them trivial. */
unsigned int m68k_read_memory_8(unsigned int address)
{
    if (!g_m || address >= g_m->mem_size)
        return 0u;
    return g_m->mem[address];
}

unsigned int m68k_read_memory_16(unsigned int address)
{
    if (!g_m)
        return 0u;
    return rd16(g_m->mem, g_m->mem_size, address);
}

unsigned int m68k_read_memory_32(unsigned int address)
{
    if (!g_m)
        return 0u;
    return rd32(g_m->mem, g_m->mem_size, address);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    if (g_m && address < g_m->mem_size)
        g_m->mem[address] = (uint8_t)value;
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    if (g_m)
        wr16(g_m->mem, g_m->mem_size, address, value);
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    if (g_m)
        wr32(g_m->mem, g_m->mem_size, address, value);
}

/* ------------------------------------------------------------- resource fork */

/* Walk the 'CODE' resources of a resource fork.  The map layout is the classic
 * one: type list, one 8-byte entry per type, then 12-byte reference lists whose
 * data offset is relative to the start of the fork. */
static int collect_code(const uint8_t *rs, uint32_t len, qrx_m68k_res_t *out,
                        unsigned max)
{
    uint32_t data_off, map_off, data_len, type_list, base, i, j;
    unsigned count = 0u, ntypes;

    if (!rs || len < QRX_RSRC_HEADER + QRX_RSRC_MAP_HEADER)
        return -1;
    data_off = rd32(rs, len, 0u);
    map_off = rd32(rs, len, 4u);
    data_len = rd32(rs, len, 8u);
    if (data_off >= len || data_off + data_len > len ||
        map_off + QRX_RSRC_MAP_HEADER > len)
        return -1;
    type_list = map_off + rd16(rs, len, map_off + 24u);
    if (type_list + 2u > len)
        return -1;
    ntypes = rd16(rs, len, type_list) + 1u;
    base = type_list + 2u;
    for (i = 0u; i < ntypes; i++) {
        uint32_t type = rd32(rs, len, base + i * 8u);
        unsigned nres = (unsigned)(rd16(rs, len, base + i * 8u + 4u) + 1u);
        uint32_t refs = type_list + rd16(rs, len, base + i * 8u + 6u);
        if (type != 0x434F4445u)   /* 'CODE' */
            continue;
        for (j = 0u; j < nres; j++) {
            uint32_t entry = refs + j * QRX_RSRC_REF_SIZE;
            uint32_t roff, rlen;
            int id;
            if (entry + QRX_RSRC_REF_SIZE > len)
                return -1;
            id = (int)(int16_t)rd16(rs, len, entry);
            roff = rd32(rs, len, entry + 4u) & 0x00FFFFFFu;
            if (roff + 4u > data_len)
                return -1;
            rlen = rd32(rs, len, data_off + roff);
            if (roff + 4u + rlen > data_len)
                return -1;
            if (count < max) {
                out[count].id = id;
                out[count].data = rs + data_off + roff + 4u;
                out[count].len = rlen;
                count++;
            }
        }
    }
    return (int)count;
}

/* Installer builders compress the 'CODE' resources, so a segment does not
 * start with 68k code: VISE/InstallerMaker puts a 16-byte header in front
 * (A89F 000C, CRC, unpacked size, packed size), and Aladdin's ADC packer
 * starts the resource with 'ADCR'.  Running such a segment only produces a
 * bogus "missing trap" at its first header word. */
static const char *packed_code(const uint8_t *d, uint32_t len)
{
    /* The packed size in the header equals the resource length in every
     * VISE resource seen so far, so demand that instead of "fits". */
    if (len >= 16u && rd16(d, len, 0u) == 0xA89Fu &&
        rd16(d, len, 2u) == 0x000Cu && rd32(d, len, 12u) == len)
        return "VISE/InstallerMaker-packed 'CODE' resource";
    if (len >= 16u && rd32(d, len, 0u) == 0x41444352u)  /* 'ADCR' */
        return "Aladdin ADC-packed 'CODE' resource";
    return 0;
}

void qrx_m68k_set_clip(qrx_m68k_t *m, const qrx_m68k_clip_t *clip)
{
    if (!m)
        return;
    if (!clip) {
        memset(&m->clip, 0, sizeof m->clip);
        m->has_clip = 0;
        return;
    }
    m->clip = *clip;
    m->has_clip = clip->put && clip->get && clip->size ? 1 : 0;
}

void qrx_m68k_unload(qrx_m68k_t *m)
{
    if (!m)
        return;
    free(m->mem);
    free(m->rsrc);
    m->mem = 0;
    m->rsrc = 0;
    m->mem_size = 0u;
}

/* ------------------------------------------------------------------- loading */

/* Find a loaded 'CODE' segment by its resource ID. */
static int find_segment(const qrx_m68k_t *m, uint32_t id, uint32_t *base,
                        uint32_t *size)
{
    unsigned i;

    for (i = 0; i < m->code_count; i++) {
        if (m->seg[i].id == id) {
            if (base)
                *base = m->seg[i].base;
            if (size)
                *size = m->seg[i].size;
            return 1;
        }
    }
    return 0;
}

/* Put every unloaded jump table entry that names this segment into its loaded
 * form: the segment number in the first word, then `JMP abs.l` to the routine.
 * The routine sits at segment base + 4-byte near-model header + entry offset.
 * Returns 1 when patched, 0 when the segment is unknown, -1 when the table is
 * malformed (m->detail says why). */
static int patch_jump_table(qrx_m68k_t *m, uint32_t id)
{
    uint32_t base, size, off, i;

    if (!find_segment(m, id, &base, &size))
        return 0;
    for (i = 0; i + 8u <= m->jt_size; i += 8u) {
        uint32_t e = m->jt + i;

        if (rd16(m->mem, m->mem_size, e + 4u) != id ||
            rd16(m->mem, m->mem_size, e + 2u) != 0x3F3Cu ||
            rd16(m->mem, m->mem_size, e + 6u) !=
                (0xA000u | QRX_M68K_TRAP_LOAD_SEG))
            continue;
        off = rd16(m->mem, m->mem_size, e);
        if (off + 4u >= size) {
            snprintf(m->detail, sizeof m->detail,
                     "jump table entry %lu: offset 0x%lx lies outside CODE %lu",
                     (unsigned long)(i / 8u), (unsigned long)off,
                     (unsigned long)id);
            return -1;
        }
        wr16(m->mem, m->mem_size, e, id);
        m->mem[e + 2u] = 0x4Eu;          /* JMP abs.l */
        m->mem[e + 3u] = 0xF9u;
        wr32(m->mem, m->mem_size, e + 4u, base + 4u + off);
    }
    return 1;
}

/* Initialize the classic near-model startup from 'CODE' 0.  The four longwords
 * give the A5 world sizes and the jump table position; the table itself is
 * copied to A5 + jump table offset (CurJTOffset), and the application starts in
 * the first entry, which pushes its segment and traps to _LoadSeg. */
static qrx_m68k_status_t setup_code0(qrx_m68k_t *m, const uint8_t *d,
                                     uint32_t len)
{
    uint32_t above, below, jt_size, jt_off, a5, i;

    if (rd16(d, len, 0u) == 0xFFFFu) {
        snprintf(m->detail, sizeof m->detail,
                 "far-model 'CODE' 0 (0xFFFF marker) is not supported");
        return QRX_M68K_ERR_IMAGE;
    }
    above = rd32(d, len, 0u);
    below = rd32(d, len, 4u);
    jt_size = rd32(d, len, 8u);
    jt_off = rd32(d, len, 12u);
    if (jt_size < 8u || (jt_size & 7u) != 0u || jt_size > 0xFFFFu) {
        snprintf(m->detail, sizeof m->detail,
                 "'CODE' 0 jump table size %lu is not a positive multiple of 8",
                 (unsigned long)jt_size);
        return QRX_M68K_ERR_IMAGE;
    }
    if (len < 16u + jt_size) {
        snprintf(m->detail, sizeof m->detail,
                 "'CODE' 0 is %lu bytes, shorter than its %lu-byte jump table",
                 (unsigned long)len, (unsigned long)jt_size);
        return QRX_M68K_ERR_IMAGE;
    }
    if (jt_off < 16u || jt_off > 0xFFFFu ||
        jt_off + jt_size > QRX_M68K_A5_MAX - QRX_M68K_A5_BASE) {
        snprintf(m->detail, sizeof m->detail,
                 "'CODE' 0 jump table offset %lu does not fit the A5 world",
                 (unsigned long)jt_off);
        return QRX_M68K_ERR_IMAGE;
    }
    if (above < jt_off + jt_size) {
        snprintf(m->detail, sizeof m->detail,
                 "'CODE' 0 above-A5 size %lu does not hold the jump table",
                 (unsigned long)above);
        return QRX_M68K_ERR_IMAGE;
    }

    /* The globals live below A5; give the image its declared size instead of
     * assuming that a fixed A5 base leaves room for them. */
    a5 = (QRX_M68K_A5_BASE + below + 0xFFFu) & ~0xFFFu;
    if (a5 < QRX_M68K_A5_BASE || a5 + above > m->mem_size ||
        a5 + jt_off + jt_size > QRX_M68K_STACK_TOP) {
        snprintf(m->detail, sizeof m->detail,
                 "A5 world (0x%lx below, 0x%lx above A5) does not fit",
                 (unsigned long)below, (unsigned long)above);
        return QRX_M68K_ERR_MEM;
    }
    m->a5 = a5;
    m->jt = a5 + jt_off;
    m->jt_size = jt_size;
    m->has_jt = 1;
    memcpy(m->mem + m->jt, d + 16u, jt_size);
    wr32(m->mem, m->mem_size, QRX_M68K_LM_CURRENT_A5, a5);
    wr16(m->mem, m->mem_size, QRX_M68K_LM_CURJT_OFFSET, jt_off);

    /* Until a segment is loaded its entries execute MOVE.W #segment,-(SP) and
     * _LoadSeg; anything else is not a near-model table. */
    for (i = 0u; i < jt_size; i += 8u) {
        uint32_t e = m->jt + i;

        if (rd16(m->mem, m->mem_size, e + 2u) != 0x3F3Cu ||
            rd16(m->mem, m->mem_size, e + 4u) == 0u ||
            rd16(m->mem, m->mem_size, e + 6u) !=
                (0xA000u | QRX_M68K_TRAP_LOAD_SEG)) {
            snprintf(m->detail, sizeof m->detail,
                     "'CODE' 0 jump table entry %lu is not an unloaded "
                     "segment call", (unsigned long)(i / 8u));
            return QRX_M68K_ERR_IMAGE;
        }
    }

    /* The program entry point is the first entry.  It must name a loaded
     * segment and its offset must point past that segment's 4-byte header. */
    {
        uint32_t id = rd16(m->mem, m->mem_size, m->jt + 4u);
        uint32_t off = rd16(m->mem, m->mem_size, m->jt);
        uint32_t base, size;

        if (!find_segment(m, id, &base, &size)) {
            snprintf(m->detail, sizeof m->detail,
                     "'CODE' 0 entry point names missing CODE %lu",
                     (unsigned long)id);
            return QRX_M68K_ERR_IMAGE;
        }
        if (off + 4u >= size) {
            snprintf(m->detail, sizeof m->detail,
                     "'CODE' 0 entry point offset 0x%lx lies outside "
                     "CODE %lu", (unsigned long)off, (unsigned long)id);
            return QRX_M68K_ERR_IMAGE;
        }
    }
    return QRX_M68K_OK;
}

qrx_m68k_status_t qrx_m68k_load(qrx_m68k_t *m, const uint8_t *image,
                                uint32_t size)
{
    return qrx_m68k_load_limited(m, image, size, 0u);
}

qrx_m68k_status_t qrx_m68k_load_limited(qrx_m68k_t *m, const uint8_t *image,
                                        uint32_t size, uint32_t max_mem)
{
    qrx_m68k_res_t res[QRX_M68K_RES_MAX];
    qrx_macbin_t mb;
    const uint8_t *rsrc;
    uint32_t rsrc_len, base;
    int found, i, macbin = 0;

    if (!m || !image || size < 32u)
        return QRX_M68K_ERR_ARG;
    memset(m, 0, sizeof(*m));
    rsrc = image;
    rsrc_len = size;
    /* A MacBinary header is only plausible when it names an application and
     * carries a resource fork; anything else is treated as a bare fork. */
    if (image[0] == 0u && image[1] > 0u && image[1] < 64u &&
        (rd32(image, size, 65u) == 0x4150504Cu ||      /* 'APPL' */
         rd32(image, size, 65u) == 0x434F4445u) &&      /* 'CODE' */
        rd32(image, size, 87u) > 0u &&
        qrx_macbin_parse(&mb, image, size) == QRX_MACAPP_OK) {
        rsrc = mb.rsrc;
        rsrc_len = mb.rsrc_len;
        macbin = 1;
    }
    found = collect_code(rsrc, rsrc_len, res, QRX_M68K_RES_MAX);
    if (found < 0)
        return QRX_M68K_ERR_IMAGE;
    if (found == 0)
        return QRX_M68K_ERR_NO_CODE;
    /* A packed installer cannot run here: report it instead of stopping at the
     * first word of the header, which would look like a missing Toolbox trap. */
    {
        const char *first_kind = NULL;
        int first_id = 0;
        unsigned packed_count = 0u;

        /* Resource maps need not be ordered by ID. Report the lowest packed
         * segment and the total, rather than the first map entry encountered. */
        for (i = 0; i < found; i++) {
            const char *kind;

            if (res[i].id <= 0)
                continue;
            kind = packed_code(res[i].data, res[i].len);
            if (!kind)
                continue;
            packed_count++;
            if (!first_kind || res[i].id < first_id) {
                first_kind = kind;
                first_id = res[i].id;
            }
        }
        if (first_kind) {
            snprintf(m->detail, sizeof m->detail,
                     "%s (lowest CODE %d, %u packed segment(s))",
                     first_kind, first_id, packed_count);
            return QRX_M68K_ERR_PACKED;
        }
    }

    /* The guest is a 32-bit machine, so the caller may ask for more than the
     * 16 MiB default; 16/8/4 MiB stay as compatibility steps below it. */
    {
        uint32_t sizes[4], last = 0u;
        unsigned count = 0u, si;
        if (max_mem > QRX_M68K_MEM_SIZE)
            sizes[count++] = max_mem;
        sizes[count++] = QRX_M68K_MEM_SIZE;
        sizes[count++] = 8u * 1024u * 1024u;
        sizes[count++] = 4u * 1024u * 1024u;
        for (si = 0; si < count; si++) {
            uint32_t need = QRX_M68K_CODE_BASE;
            uint32_t want = sizes[si];
            if (max_mem != 0u && want > max_mem)
                want = max_mem;
            if (want == last)
                continue;
            last = want;
            for (i = 0; i < found; i++)
                if (res[i].id > 0)
                    need = (need + res[i].len + 0xFFFu) & ~0xFFFu;
            /* The segments must stay below the heap, which starts in the
             * middle of the block. */
            if (need > want / QRX_M68K_HEAP_FRACTION)
                continue;
            m->mem = (uint8_t *)calloc(1u, want);
            if (m->mem) {
                m->mem_size = want;
                break;
            }
        }
        if (!m->mem)
            return QRX_M68K_ERR_MEM;
    }
    /* Everything the Machine reports has to stay inside the block that was
     * actually allocated: MemTop, BufPtr, the zone and the NewPtr limit. */
    m->heap = m->mem_size / QRX_M68K_HEAP_FRACTION;
    m->nheap_free = 0u;
    m->heap_end = m->mem_size > QRX_M68K_HEAP_RESERVE
                      ? m->mem_size - QRX_M68K_HEAP_RESERVE
                      : m->mem_size;

    /* The exit stub: ILLEGAL, caught by the trap callback as "returned". */
    m->mem[QRX_M68K_EXIT_STUB] = 0x4Au;
    m->mem[QRX_M68K_EXIT_STUB + 1u] = 0xFCu;

    /* Segments: CODE 1..n in ascending order, each page aligned. */
    base = QRX_M68K_CODE_BASE;
    for (i = 0; i < found; i++) {
        uint32_t len;
        if (res[i].id <= 0)
            continue;                    /* CODE 0 is the jump table */
        len = res[i].len;
        if (base + len > m->mem_size / QRX_M68K_HEAP_FRACTION)
            return QRX_M68K_ERR_MEM;
        memcpy(m->mem + base, res[i].data, len);
        if (m->code_count < QRX_M68K_SEG_MAX) {
            m->seg[m->code_count].id = (uint32_t)res[i].id;
            m->seg[m->code_count].base = base;
            m->seg[m->code_count].size = len;
            m->code_count++;
        }
        base = (base + len + 0xFFFu) & ~0xFFFu;
    }
    if (m->code_count == 0u)
        return QRX_M68K_ERR_NO_CODE;

    /* A5 world: 'CODE' 0 holds the near-model jump table.  Images that carry a
     * full header start through it; the two-byte synthetic fixtures used by the
     * host tests have no header and keep starting at the first segment. */
    m->a5 = QRX_M68K_A5_BASE;
    m->entry = m->seg[0].base;
    for (i = 0; i < found; i++) {
        qrx_m68k_status_t st;

        if (res[i].id != 0)
            continue;
        if (res[i].len >= 16u) {
            st = setup_code0(m, res[i].data, res[i].len);
            if (st != QRX_M68K_OK)
                return st;
            m->entry = m->jt + 2u;
        } else {
            if (res[i].len > QRX_M68K_A5_MAX - QRX_M68K_A5_BASE)
                return QRX_M68K_ERR_MEM;
            memcpy(m->mem + m->a5, res[i].data, res[i].len);
            wr32(m->mem, m->mem_size, QRX_M68K_LM_CURRENT_A5, m->a5);
        }
        break;
    }

    /* Low memory facts an application expects to find: MemTop, BufPtr, and a
     * zeroed heap/stack region (calloc did that). */
    /* The Resource Manager answers from the fork that was loaded: keep a copy,
     * because the caller frees the image after this call. */
    m->rsrc = (uint8_t *)malloc(rsrc_len);
    if (!m->rsrc)
        return QRX_M68K_ERR_MEM;
    memcpy(m->rsrc, rsrc, rsrc_len);
    m->rsrc_len = rsrc_len;
    if (macbin && mb.name_len != 0u) {
        unsigned n = mb.name_len < sizeof(m->app_name) - 1u
                         ? mb.name_len
                         : (unsigned)sizeof(m->app_name) - 1u;
        memcpy(m->app_name, mb.name, n);
        m->app_name[n] = '\0';
    }
    m->resfile = 0xFFFFFFFFu;              /* the application's own file */

    wr32(m->mem, m->mem_size, 0x0108u, m->mem_size);        /* MemTop */
    wr32(m->mem, m->mem_size, 0x010Cu, m->mem_size - 0x1000u); /* BufPtr */

    m->sp = QRX_M68K_STACK_TOP;
    return QRX_M68K_OK;
}

/* ------------------------------------------------------------- files (HLE) */

void qrx_m68k_set_fs(qrx_m68k_t *m, const qrx_m68k_fs_t *fs)
{
    if (!m || !fs)
        return;
    m->fs = *fs;
    m->has_fs = fs->open && fs->read && fs->write && fs->seek && fs->close;
}

void qrx_m68k_set_gfx(qrx_m68k_t *m, const qrx_m68k_gfx_t *gfx)
{
    if (!m)
        return;
    if (!gfx) {
        memset(&m->gfx, 0, sizeof m->gfx);
        m->has_gfx = 0;
        return;
    }
    m->gfx = *gfx;
    m->has_gfx = 1;
}

/* A guest Pascal path becomes a relative host path: ':' separates components.
 * Anything that would leave the share tree ("..") is refused. */
static int guest_path(qrx_m68k_t *m, uint32_t pstr, char *out, uint32_t cap)
{
    uint32_t len, i;

    if (pstr == 0u || pstr + 1u > m->mem_size)
        return 0;
    len = m->mem[pstr];
    if (pstr + 1u + len > m->mem_size || len + 1u > cap)
        return 0;
    for (i = 0u; i < len; i++) {
        char c = (char)m->mem[pstr + 1u + i];

        out[i] = c == ':' ? '/' : c;
    }
    out[len] = '\0';
    for (i = 0u; i + 1u < len; i++)
        if (out[i] == '.' && out[i + 1u] == '.' &&
            (i == 0u || out[i - 1u] == '/') &&
            (i + 2u == len || out[i + 2u] == '/'))
            return 0;
    return 1;
}

static int file_slot(qrx_m68k_t *m, uint32_t ref)
{
    if (ref == 0u || ref > QRX_M68K_FILE_MAX)
        return -1;
    return m->file[ref - 1u].used ? (int)(ref - 1u) : -1;
}

static int32_t pb_ok(qrx_m68k_t *m, uint32_t pb)
{
    if (pb == 0u || pb + 50u > m->mem_size)
        return QRX_M68K_TRAP_PARAM_ERR;
    return 0;
}

static int32_t hle_fs_open(qrx_m68k_t *m, uint32_t pb, int create)
{
    char path[QRX_M68K_PATH_MAX];
    unsigned slot;
    long fd;
    int32_t err = pb_ok(m, pb);

    if (err != 0)
        return err;
    if (!m->has_fs)
        return QRX_M68K_ERR_FNF;
    if (!guest_path(m, rd32(m->mem, m->mem_size, pb + QRX_M68K_PB_NAME), path,
                    (uint32_t)sizeof path))
        return QRX_M68K_ERR_FNF;
    for (slot = 0u; slot < QRX_M68K_FILE_MAX; slot++)
        if (!m->file[slot].used)
            break;
    if (slot == QRX_M68K_FILE_MAX)
        return QRX_M68K_ERR_TOO_MANY_FILES;
    fd = m->fs.open(m->fs.ctx, path, create);
    if (fd < 0)
        return QRX_M68K_ERR_FNF;
    m->file[slot].fd = fd;
    m->file[slot].used = 1;
    wr16(m->mem, m->mem_size, pb + QRX_M68K_PB_REFNUM, (uint16_t)(slot + 1u));
    return 0;
}

static int32_t hle_fs_close(qrx_m68k_t *m, uint32_t pb)
{
    int slot;
    int32_t err = pb_ok(m, pb);

    if (err != 0)
        return err;
    slot = file_slot(m, rd16(m->mem, m->mem_size, pb + QRX_M68K_PB_REFNUM));
    if (slot < 0 || !m->has_fs)
        return QRX_M68K_ERR_IO;
    m->fs.close(m->fs.ctx, m->file[slot].fd);
    m->file[slot].used = 0;
    m->file[slot].fd = -1;
    return 0;
}

static int32_t hle_fs_transfer(qrx_m68k_t *m, uint32_t pb, int write)
{
    int slot;
    uint32_t buf, count;
    long done;
    int32_t err = pb_ok(m, pb);

    if (err != 0)
        return err;
    slot = file_slot(m, rd16(m->mem, m->mem_size, pb + QRX_M68K_PB_REFNUM));
    if (slot < 0 || !m->has_fs)
        return QRX_M68K_ERR_IO;
    buf = rd32(m->mem, m->mem_size, pb + QRX_M68K_PB_BUFFER);
    count = rd32(m->mem, m->mem_size, pb + QRX_M68K_PB_REQCOUNT);
    if (count != 0u && (buf >= m->mem_size || count > m->mem_size - buf))
        return QRX_M68K_TRAP_PARAM_ERR;
    done = write ? m->fs.write(m->fs.ctx, m->file[slot].fd, m->mem + buf, count)
                 : m->fs.read(m->fs.ctx, m->file[slot].fd, m->mem + buf, count);
    if (done < 0)
        return QRX_M68K_ERR_IO;
    wr32(m->mem, m->mem_size, pb + QRX_M68K_PB_ACTCOUNT, (uint32_t)done);
    return 0;
}

/* GetFPos answers the position in ioReqCount, SetFPos takes ioPosOffset and
 * ioPosMode (0/3 = from mark, 1 = from start, 2 = from end). */
static int32_t hle_fs_get_fpos(qrx_m68k_t *m, uint32_t pb)
{
    int slot;
    long pos;
    int32_t err = pb_ok(m, pb);

    if (err != 0)
        return err;
    slot = file_slot(m, rd16(m->mem, m->mem_size, pb + QRX_M68K_PB_REFNUM));
    if (slot < 0 || !m->has_fs)
        return QRX_M68K_ERR_IO;
    pos = m->fs.seek(m->fs.ctx, m->file[slot].fd, 0, 1);
    if (pos < 0)
        return QRX_M68K_ERR_IO;
    wr32(m->mem, m->mem_size, pb + QRX_M68K_PB_REQCOUNT, (uint32_t)pos);
    wr32(m->mem, m->mem_size, pb + QRX_M68K_PB_ACTCOUNT, (uint32_t)pos);
    return 0;
}

static int32_t hle_fs_set_fpos(qrx_m68k_t *m, uint32_t pb)
{
    int slot;
    uint32_t mode;
    long pos;
    int32_t err = pb_ok(m, pb);

    if (err != 0)
        return err;
    slot = file_slot(m, rd16(m->mem, m->mem_size, pb + QRX_M68K_PB_REFNUM));
    if (slot < 0 || !m->has_fs)
        return QRX_M68K_ERR_IO;
    mode = rd16(m->mem, m->mem_size, pb + QRX_M68K_PB_POSMODE);
    pos = m->fs.seek(m->fs.ctx, m->file[slot].fd,
                     (int32_t)rd32(m->mem, m->mem_size, pb + QRX_M68K_PB_POSOFFSET),
                     mode == 1u ? 0 : mode == 2u ? 2 : 1);
    if (pos < 0)
        return QRX_M68K_ERR_IO;
    return 0;
}

/* GetEOF answers ioEOF at offset 32 and leaves the mark where it was. */
static int32_t hle_fs_get_eof(qrx_m68k_t *m, uint32_t pb)
{
    int slot;
    long pos, end;
    int32_t err = pb_ok(m, pb);

    if (err != 0)
        return err;
    slot = file_slot(m, rd16(m->mem, m->mem_size, pb + QRX_M68K_PB_REFNUM));
    if (slot < 0 || !m->has_fs)
        return QRX_M68K_ERR_IO;
    pos = m->fs.seek(m->fs.ctx, m->file[slot].fd, 0, 1);
    end = pos < 0 ? -1 : m->fs.seek(m->fs.ctx, m->file[slot].fd, 0, 2);
    if (pos < 0 || end < 0)
        return QRX_M68K_ERR_IO;
    m->fs.seek(m->fs.ctx, m->file[slot].fd, (int32_t)pos, 0);
    wr32(m->mem, m->mem_size, pb + 32u, (uint32_t)end);   /* ioEOF */
    return 0;
}

static int32_t hle_file_trap(qrx_m68k_t *m, uint32_t trap, uint32_t pb)
{
    switch (trap) {
    case QRX_M68K_TRAP_OPEN:
        return hle_fs_open(m, pb, 0);
    case QRX_M68K_TRAP_CREATE:
        return hle_fs_open(m, pb, 1);
    case QRX_M68K_TRAP_CLOSE:
        return hle_fs_close(m, pb);
    case QRX_M68K_TRAP_READ:
        return hle_fs_transfer(m, pb, 0);
    case QRX_M68K_TRAP_WRITE:
        return hle_fs_transfer(m, pb, 1);
    case QRX_M68K_TRAP_GET_FPOS:
        return hle_fs_get_fpos(m, pb);
    case QRX_M68K_TRAP_SET_FPOS:
        return hle_fs_set_fpos(m, pb);
    case QRX_M68K_TRAP_GET_EOF:
        return hle_fs_get_eof(m, pb);
    default:
        return QRX_M68K_TRAP_PARAM_ERR;
    }
}

/* ----------------------------------------------------------------------- HLE */

static uint32_t hle_gestalt(qrx_m68k_t *m, uint32_t selector, uint32_t out)
{
    uint32_t value = 0u;
    int known = 1;

    switch (selector) {
    case 0x74686473u:   /* 'thds': no Thread Manager */
        value = 0u;
        break;
    case 0x73797376u:   /* 'sysv': 8.1.0 */
        value = 0x00000810u;
        break;
    case 0x76657273u:   /* 'vers': Gestalt 1.0 */
        value = 0x00010000u;
        break;
    case 0x6F732020u:   /* 'os  ': no ROM trap table */
        value = 0u;
        break;
    case 0x61646472u:   /* 'addr': 32-bit addressing, exactly as MemTop says */
        value = m->mem_size > 0x01000000u ? 1u : 0u;
        break;
    default:
        known = 0;
        break;
    }
    if (!known)
        return QRX_M68K_TRAP_GESTALT_UNDEF;
    if (out != 0u) {
        if (out + 4u > m->mem_size)
            return QRX_M68K_TRAP_PARAM_ERR;
        wr32(m->mem, m->mem_size, out, value);
    }
    return 0u;
}

/* _SysEnvirons (0xA090): D0 is the requested version, A0 the SysEnvRec.  Version
 * 2 is answered; the 32-bit addressing state is not part of the record, the
 * application reads it through Gestalt 'addr' (hle_gestalt above). */
static uint32_t hle_sys_environs(qrx_m68k_t *m, uint32_t want, uint32_t rec)
{
    if (want > 2u)
        return QRX_M68K_TRAP_ENV_VERS_TOO_BIG;
    if (want != 0u) {
        if (rec == 0u || (rec & 1u) != 0u || rec + 14u > m->mem_size)
            return QRX_M68K_TRAP_PARAM_ERR;
        wr16(m->mem, m->mem_size, rec + 0u, (uint16_t)want);   /* environsVersion */
        wr16(m->mem, m->mem_size, rec + 2u, QRX_M68K_MACHINE_TYPE);
        wr16(m->mem, m->mem_size, rec + 4u, 0x0810u);          /* System 8.1.0 */
        wr16(m->mem, m->mem_size, rec + 6u, 2u);               /* processor: 68020 */
        m->mem[rec + 8u] = 1u;                                 /* Musashi runs the FPU */
        m->mem[rec + 9u] = 1u;                                 /* Color QuickDraw */
        wr16(m->mem, m->mem_size, rec + 10u, 0u);              /* no AppleTalk driver */
        wr16(m->mem, m->mem_size, rec + 12u, 0u);              /* SysVRefNum */
    }
    return 2u;
}

static uint32_t hle_new_ptr(qrx_m68k_t *m, uint32_t size)
{
    uint32_t addr;
    unsigned i;

    if (size == 0u)
        size = 1u;
    size = (size + 3u) & ~3u;
    /* First fit over the blocks a Dispose... trap gave back. */
    for (i = 0u; i < m->nheap_free; i++) {
        if (m->heap_free[i].size < size)
            continue;
        addr = m->heap_free[i].addr;
        if (m->heap_free[i].size == size) {
            m->nheap_free--;
            m->heap_free[i] = m->heap_free[m->nheap_free];
        } else {
            m->heap_free[i].addr += size;
            m->heap_free[i].size -= size;
        }
        memset(m->mem + addr, 0, size);
        return addr;
    }
    addr = (m->heap + 3u) & ~3u;
    if (addr + size > m->heap_end)
        return 0u;
    m->heap = addr + size;
    memset(m->mem + addr, 0, size);
    return addr;
}

/* Hand a block from NewPtr back.  Neighbours merge, and a block that touches
 * the top of the heap pulls the bump pointer down, so the common
 * "allocate, use, dispose" cycle needs no new memory. */
static void hle_free_ptr(qrx_m68k_t *m, uint32_t addr, uint32_t size)
{
    unsigned i;
    int again;

    if (addr == 0u || size == 0u)
        return;
    size = (size + 3u) & ~3u;
    if (addr < m->heap_end && size > m->heap_end - addr)
        return;
    for (i = 0u; i < m->nheap_free; i++)
        if (m->heap_free[i].addr + m->heap_free[i].size == addr) {
            addr = m->heap_free[i].addr;
            size += m->heap_free[i].size;
            m->nheap_free--;
            m->heap_free[i] = m->heap_free[m->nheap_free];
            break;
        }
    for (i = 0u; i < m->nheap_free; i++)
        if (m->heap_free[i].addr == addr + size) {
            size += m->heap_free[i].size;
            m->nheap_free--;
            m->heap_free[i] = m->heap_free[m->nheap_free];
            break;
        }
    while (addr + size == m->heap) {
        m->heap = addr;
        again = 0;
        for (i = 0u; i < m->nheap_free; i++)
            if (m->heap_free[i].addr + m->heap_free[i].size == m->heap) {
                m->heap = m->heap_free[i].addr;
                m->nheap_free--;
                m->heap_free[i] = m->heap_free[m->nheap_free];
                again = 1;
                break;
            }
        if (!again)
            return;
    }
    if (m->nheap_free < QRX_M68K_HEAP_FREE_MAX) {
        m->heap_free[m->nheap_free].addr = addr;
        m->heap_free[m->nheap_free].size = size;
        m->nheap_free++;
    }
}

/* A handle is a master pointer in the heap that names the block; DisposeHandle
 * clears the slot, NewHandle reuses it.  Nothing moves here, so HLock/HUnlock
 * have nothing to do. */
static int handle_slot(qrx_m68k_t *m, uint32_t h)
{
    unsigned i;

    if (h == 0u)
        return -1;
    for (i = 0; i < m->nhandle; i++)
        if (m->handle[i].mp == h)
            return (int)i;
    return -1;
}

static uint32_t hle_new_handle(qrx_m68k_t *m, uint32_t size)
{
    unsigned slot = QRX_M68K_HANDLE_MAX, i;
    uint32_t block, mp;

    for (i = 0; i < m->nhandle; i++)
        if (m->handle[i].mp == 0u) {
            slot = i;
            break;
        }
    if (slot == QRX_M68K_HANDLE_MAX) {
        if (m->nhandle >= QRX_M68K_HANDLE_MAX)
            return 0u;
        slot = m->nhandle++;
    }
    if (size == 0u)
        size = 1u;
    block = hle_new_ptr(m, size);
    mp = hle_new_ptr(m, 4u);
    if (block == 0u || mp == 0u) {
        m->handle[slot].mp = 0u;
        return 0u;
    }
    wr32(m->mem, m->mem_size, mp, block);
    m->handle[slot].mp = mp;
    m->handle[slot].size = size;
    return mp;
}

static void hle_dispose_handle(qrx_m68k_t *m, uint32_t h)
{
    int i = handle_slot(m, h);

    if (i < 0)
        return;
    wr32(m->mem, m->mem_size, h, 0u);   /* the bump heap cannot reuse the block */
    m->handle[i].mp = 0u;
}

static uint32_t hle_set_handle_size(qrx_m68k_t *m, uint32_t h, uint32_t size)
{
    int i = handle_slot(m, h);
    uint32_t block, copy;

    if (i < 0)
        return 0u;
    if (size == 0u)
        size = 1u;
    block = hle_new_ptr(m, size);
    if (block == 0u)
        return 0u;
    copy = size < m->handle[i].size ? size : m->handle[i].size;
    memcpy(m->mem + block, m->mem + rd32(m->mem, m->mem_size, h), copy);
    wr32(m->mem, m->mem_size, h, block);
    m->handle[i].size = size;
    return h;
}

/* The loaded application is the only resource file: OpenResFile accepts a path
 * whose last component is the application name (any path when the image carried
 * no name) and answers with refNum -1. */
static uint32_t hle_open_res_file(qrx_m68k_t *m, uint32_t pstr)
{
    uint32_t len, i, start = 0u, n;

    if (pstr == 0u || pstr + 1u > m->mem_size)
        return 0xFFFFFFFFu;
    len = m->mem[pstr];
    if (pstr + 1u + len > m->mem_size)
        return QRX_M68K_ERR_FNF;
    if (len == 0u || m->app_name[0] == '\0') {
        m->resfile = 0xFFFFFFFFu;
        return 0xFFFFFFFFu;
    }
    for (i = 0u; i < len; i++)
        if (m->mem[pstr + 1u + i] == ':' || m->mem[pstr + 1u + i] == '/')
            start = i + 1u;
    n = len - start;
    if (n != strlen(m->app_name))
        return QRX_M68K_ERR_FNF;
    for (i = 0u; i < n; i++) {
        char a = (char)m->mem[pstr + 1u + start + i];
        char b = m->app_name[i];

        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return QRX_M68K_ERR_FNF;
    }
    m->resfile = 0xFFFFFFFFu;
    return 0xFFFFFFFFu;
}

/* GetResource hands out one handle per resource and keeps it, as MacOS does. */
static uint32_t hle_get_resource(qrx_m68k_t *m, uint32_t type, int32_t id)
{
    const uint8_t *data = 0;
    uint32_t len = 0u, h, i;

    if (m->rsrc == 0)
        return 0u;
    for (i = 0; i < m->nres; i++)
        if (m->rescache[i].type == type && m->rescache[i].id == id)
            return m->rescache[i].h;
    if (qrx_rsrc_find(m->rsrc, m->rsrc_len, type, (int)id, &data, &len) !=
        QRX_MACAPP_OK)
        return 0u;
    h = hle_new_handle(m, len);
    if (h == 0u)
        return 0u;
    memcpy(m->mem + rd32(m->mem, m->mem_size, h), data, len);
    if (m->nres < QRX_M68K_RES_CACHE) {
        m->rescache[m->nres].type = type;
        m->rescache[m->nres].id = id;
        m->rescache[m->nres].h = h;
        m->nres++;
    }
    return h;
}

/* -------------------------------------------------- QuickDraw and dialogs */

static const uint8_t qd_black[8] = {0xFFu, 0xFFu, 0xFFu, 0xFFu,
                                    0xFFu, 0xFFu, 0xFFu, 0xFFu};

static int mem_span(const qrx_m68k_t *m, uint32_t a, uint32_t len)
{
    return a != 0u && len <= m->mem_size && a <= m->mem_size - len;
}

static int qd_read_rect(const qrx_m68k_t *m, uint32_t a, int32_t *top,
                        int32_t *left, int32_t *bottom, int32_t *right)
{
    if (!mem_span(m, a, 8u))
        return 0;
    *top = (int16_t)rd16(m->mem, m->mem_size, a);
    *left = (int16_t)rd16(m->mem, m->mem_size, a + 2u);
    *bottom = (int16_t)rd16(m->mem, m->mem_size, a + 4u);
    *right = (int16_t)rd16(m->mem, m->mem_size, a + 6u);
    return 1;
}

static void qd_write_rect(qrx_m68k_t *m, uint32_t a, int32_t top, int32_t left,
                          int32_t bottom, int32_t right)
{
    wr16(m->mem, m->mem_size, a, (uint16_t)top);
    wr16(m->mem, m->mem_size, a + 2u, (uint16_t)left);
    wr16(m->mem, m->mem_size, a + 4u, (uint16_t)bottom);
    wr16(m->mem, m->mem_size, a + 6u, (uint16_t)right);
}

/* A rectangle operation reaches the drawing hook; an unreadable rectangle or a
 * missing hook answers quietly (QuickDraw has no error code here). */
static void qd_draw(qrx_m68k_t *m, int op, uint32_t rect_addr,
                    const uint8_t *pat)
{
    int32_t top, left, bottom, right;

    if (!qd_read_rect(m, rect_addr, &top, &left, &bottom, &right))
        return;
    if (m->has_gfx && m->gfx.rect)
        m->gfx.rect(m->gfx.ctx, top, left, bottom, right, op, pat);
}

static void qd_draw_line(qrx_m68k_t *m, int32_t from_y, int32_t from_x,
                         int32_t to_y, int32_t to_x)
{
    m->pen_y = to_y;
    m->pen_x = to_x;
    if (m->has_gfx && m->gfx.line)
        m->gfx.line(m->gfx.ctx, from_y, from_x, to_y, to_x, m->pen_mode,
                    m->pen_size_w, m->pen_size_h);
}

static void qd_draw_text(qrx_m68k_t *m, const char *s, uint32_t len)
{
    if (len != 0u && m->has_gfx && m->gfx.text)
        m->gfx.text(m->gfx.ctx, m->pen_y, m->pen_x, s, len, m->text_font,
                    m->text_size, m->text_face);
}

/* An event record is what, message, when, where (v,h) and modifiers. */
static void ev_write(qrx_m68k_t *m, uint32_t rec, uint16_t what,
                     uint32_t message, int32_t vy, int32_t vx, uint16_t mods)
{
    if (!mem_span(m, rec, QRX_M68K_EVENT_RECORD))
        return;
    memset(m->mem + rec, 0, QRX_M68K_EVENT_RECORD);
    wr16(m->mem, m->mem_size, rec, what);
    wr32(m->mem, m->mem_size, rec + 2u, message);
    wr32(m->mem, m->mem_size, rec + 6u, m->ticks);
    wr16(m->mem, m->mem_size, rec + 10u, (uint16_t)vy);
    wr16(m->mem, m->mem_size, rec + 12u, (uint16_t)vx);
    wr16(m->mem, m->mem_size, rec + 14u, mods);
}

/* The queue is filled from two sides: by the application's own PostEvent and
 * by the guest's input pump, which turns clicks and keystrokes in a real
 * AROS window into MacOS events.  When it is full the oldest event goes, so a
 * pump that keeps running cannot grow the machine block. */
int qrx_m68k_post_event(qrx_m68k_t *m, uint16_t what, uint32_t message,
                        int32_t vy, int32_t vx, uint16_t mods)
{
    unsigned i;

    if (!m || what == 0u)
        return 0;
    if (m->nevent >= QRX_M68K_EVENT_QUEUE) {
        for (i = 1u; i < m->nevent; i++)
            m->evq[i - 1u] = m->evq[i];
        m->nevent = QRX_M68K_EVENT_QUEUE - 1u;
    }
    m->evq[m->nevent].what = what;
    m->evq[m->nevent].message = message;
    m->evq[m->nevent].vy = vy;
    m->evq[m->nevent].vx = vx;
    m->evq[m->nevent].mods = mods;
    m->nevent++;
    if (what == QRX_M68K_EVENT_MENU) {
        m->menu_pick = message;      /* menuID << 16 | item, as MenuSelect */
        m->menu_pick_valid = 1;
    }
    m->mouse_y = vy;
    m->mouse_x = vx;
    if (what == 1u)                   /* mouseDown */
        m->mouse_down = 1;
    else if (what == 2u)              /* mouseUp */
        m->mouse_down = 0;
    return 1;
}

/* Let the guest collect what its input source has queued before an event trap
 * answers. */
static void ev_poll(qrx_m68k_t *m)
{
    if (m->has_gfx && m->gfx.poll)
        m->gfx.poll(m->gfx.ctx);
}

static int ev_take(qrx_m68k_t *m, uint32_t mask, uint32_t rec, int remove)
{
    unsigned i;

    if (!mem_span(m, rec, QRX_M68K_EVENT_RECORD))
        return 0;
    for (i = 0u; i < m->nevent; i++) {
        if (mask != 0u && (mask & (1u << (m->evq[i].what & 0x0Fu))) == 0u)
            continue;
        ev_write(m, rec, m->evq[i].what, m->evq[i].message, m->evq[i].vy,
                 m->evq[i].vx, m->evq[i].mods);
        if (remove) {
            for (; i + 1u < m->nevent; i++)
                m->evq[i] = m->evq[i + 1u];
            m->nevent--;
        }
        return 1;
    }
    ev_write(m, rec, 0u, 0u, m->mouse_y, m->mouse_x, 0u);
    return 0;
}

/* Control Manager: a control is bounds, title, value and hilite state in a
 * record of this runtime.  Drawing goes through the QuickDraw hooks, so the
 * guest puts the control into whatever port it has set. */
#define QRX_M68K_IN_BUTTON 10

static int ctrl_slot(qrx_m68k_t *m, uint32_t handle)
{
    unsigned i;

    if (handle == 0u)
        return -1;
    for (i = 0u; i < QRX_M68K_CONTROL_MAX; i++)
        if (m->ctrl[i].used && m->ctrl[i].handle == handle)
            return (int)i;
    return -1;
}

static void ctrl_draw(qrx_m68k_t *m, int slot)
{
    uint32_t len;

    if (slot < 0 || !m->ctrl[slot].used || !m->ctrl[slot].visible)
        return;
    if (!m->has_gfx)
        return;
    if (m->gfx.rect)
        m->gfx.rect(m->gfx.ctx, m->ctrl[slot].top, m->ctrl[slot].left,
                    m->ctrl[slot].bottom, m->ctrl[slot].right,
                    m->ctrl[slot].hilite != 0 ? QRX_M68K_RECT_PAINT
                                              : QRX_M68K_RECT_FRAME,
                    qd_black);
    len = (uint32_t)strlen(m->ctrl[slot].title);
    if (len != 0u && m->gfx.text)
        m->gfx.text(m->gfx.ctx, m->ctrl[slot].top, m->ctrl[slot].left + 2,
                    m->ctrl[slot].title, len, m->text_font, m->text_size,
                    m->text_face);
}

static int ctrl_hit(qrx_m68k_t *m, int slot, int32_t vy, int32_t vx)
{
    if (slot < 0 || !m->ctrl[slot].used || !m->ctrl[slot].visible)
        return 0;
    return vy >= m->ctrl[slot].top && vy < m->ctrl[slot].bottom &&
           vx >= m->ctrl[slot].left && vx < m->ctrl[slot].right;
}

static int ctrl_new(qrx_m68k_t *m, uint32_t owner, int32_t top, int32_t left,
                    int32_t bottom, int32_t right, int visible, int16_t value,
                    int16_t min, int16_t max, const char *title, uint32_t len,
                    int16_t item)
{
    unsigned i;
    int slot = -1;

    for (i = 0u; i < QRX_M68K_CONTROL_MAX; i++)
        if (!m->ctrl[i].used) {
            slot = (int)i;
            break;
        }
    if (slot < 0)
        return -1;
    memset(&m->ctrl[slot], 0, sizeof m->ctrl[0]);
    m->ctrl[slot].handle = hle_new_ptr(m, 8u);
    if (m->ctrl[slot].handle == 0u)
        return -1;
    m->ctrl[slot].used = 1;
    m->ctrl[slot].owner = owner;
    m->ctrl[slot].visible = visible;
    m->ctrl[slot].value = value;
    m->ctrl[slot].min = min;
    m->ctrl[slot].max = max;
    m->ctrl[slot].item = item;
    m->ctrl[slot].top = top;
    m->ctrl[slot].left = left;
    m->ctrl[slot].bottom = bottom;
    m->ctrl[slot].right = right;
    if (len > QRX_M68K_MENU_TEXT - 1u)
        len = QRX_M68K_MENU_TEXT - 1u;
    if (title != NULL && len != 0u)
        memcpy(m->ctrl[slot].title, title, len);
    m->ctrl[slot].title[len] = '\0';
    return slot;
}

/* The DITL item number under a point, 0 when no control of that dialog is
 * there.  A dialog's controls are the item itself: item 4 is a button, 5 a
 * checkbox, 6 a radio button and 7 a control. */
static int dlg_item_at(qrx_m68k_t *m, uint32_t rec, int32_t vy, int32_t vx)
{
    unsigned i;

    for (i = 0u; i < QRX_M68K_CONTROL_MAX; i++) {
        if (!m->ctrl[i].used || m->ctrl[i].item == 0)
            continue;
        if (rec != 0u && m->ctrl[i].owner != rec)
            continue;
        if (ctrl_hit(m, (int)i, vy, vx))
            return m->ctrl[i].item;
    }
    return 0;
}

/* Take a queued mouseDown that lands in one of the dialog's controls and
 * answer its item number; 0 leaves the queue alone. */
static int dlg_take_click(qrx_m68k_t *m, uint32_t rec)
{
    unsigned i, e;

    for (e = 0u; e < m->nevent; e++) {
        int item;

        if (m->evq[e].what != 1u)      /* mouseDown */
            continue;
        item = dlg_item_at(m, rec, m->evq[e].vy, m->evq[e].vx);
        if (item == 0)
            continue;
        for (i = e; i + 1u < m->nevent; i++)
            m->evq[i] = m->evq[i + 1u];
        m->nevent--;
        return item;
    }
    return 0;
}

/* 'DITL' entries that respond to the mouse become control records owned by the
 * dialog record, so FindControl, TrackControl and the drawing traps see them
 * like a control the program made itself. */
static void dlg_build_controls(qrx_m68k_t *m, uint32_t rec, unsigned count,
                               uint32_t table)
{
    uint32_t entries = (uint32_t)count * QRX_M68K_DLG_ENTRY;
    unsigned i;

    for (i = 0u; i < count; i++) {
        uint32_t entry = table + i * QRX_M68K_DLG_ENTRY;
        uint32_t len = m->mem[entry + 9u];
        uint32_t off = rd16(m->mem, m->mem_size, entry + 10u);
        uint32_t text = table + entries + off;
        uint8_t type = m->mem[entry + 8u];
        int32_t top, left, bottom, right;

        if (type < 4u || type > 7u)
            continue;
        if (!qd_read_rect(m, entry, &top, &left, &bottom, &right))
            continue;
        if (len != 0u && !mem_span(m, text, len))
            len = 0u;
        (void)ctrl_new(m, rec, top, left, bottom, right, 1, 0, 0, 1,
                       len != 0u ? (const char *)(m->mem + text) : NULL, len,
                       (int16_t)(i + 1u));
    }
}

/* Give a dialog back: its controls, its item table and the record itself.  The
 * table's size is not stored in the record, so it is recovered from the item
 * lengths (each item's text plus its terminating byte). */
static void dlg_free(qrx_m68k_t *m, uint32_t rec)
{
    uint32_t table, entries, total = 0u;
    unsigned i, count;

    if (rec == 0u || !mem_span(m, rec, QRX_M68K_DLG_RECORD))
        return;
    for (i = 0u; i < QRX_M68K_CONTROL_MAX; i++)
        if (m->ctrl[i].used && m->ctrl[i].owner == rec) {
            hle_free_ptr(m, m->ctrl[i].handle, 8u);
            m->ctrl[i].used = 0;
        }
    count = rd16(m->mem, m->mem_size, rec + QRX_M68K_DLG_ITEMS);
    table = rd32(m->mem, m->mem_size, rec + QRX_M68K_DLG_TABLE);
    entries = (uint32_t)count * QRX_M68K_DLG_ENTRY;
    if (table != 0u && mem_span(m, table, entries)) {
        for (i = 0u; i < count; i++)
            total += (uint32_t)
                         m->mem[table + i * QRX_M68K_DLG_ENTRY + 9u] +
                     1u;
        hle_free_ptr(m, table, entries + total);
    }
    hle_free_ptr(m, rec, QRX_M68K_DLG_RECORD);
    for (i = 0u; i < m->ndialog; i++)
        if (m->dialog_rec[i].rec == rec) {
            for (; i + 1u < m->ndialog; i++)
                m->dialog_rec[i] = m->dialog_rec[i + 1u];
            m->ndialog--;
            break;
        }
}

/* Dialog Manager: 'DLOG' names the 'DITL'.  The record is a stub of 64 bytes
 * (QRX_M68K_DLG_*) whose item table holds the parsed rectangles and texts, so
 * DrawDialog can draw and ModalDialog can name an item. */
static uint32_t hle_get_new_dialog(qrx_m68k_t *m, int16_t id)
{
    const uint8_t *dlog = 0, *ditl = 0;
    uint32_t dlog_len = 0u, ditl_len = 0u, items_id;
    uint32_t rec, table, pos, off, total_text = 0u, entries;
    int32_t top = 0x7FFF, left = 0x7FFF, bottom = -0x8000, right = -0x8000;
    unsigned count = 0u, i;

    if (m->rsrc == 0)
        return 0u;
    if (qrx_rsrc_find(m->rsrc, m->rsrc_len, 0x444C4F47u, (int)id, &dlog,
                      &dlog_len) != QRX_MACAPP_OK)
        return 0u;
    if (dlog_len < 14u)
        return 0u;
    items_id = rd16(dlog, dlog_len, 10u);   /* itemsID after procID/visible/refCon */
    if (qrx_rsrc_find(m->rsrc, m->rsrc_len, 0x4449544Cu, (int)items_id, &ditl,
                      &ditl_len) != QRX_MACAPP_OK)
        return 0u;
    if (ditl_len < 2u)
        return 0u;

    pos = 2u;                       /* first pass: count, bounds, text size */
    while (count < QRX_M68K_DIALOG_ITEM_MAX && pos + 14u <= ditl_len) {
        uint32_t len = ditl[pos + 13u];
        int32_t t, l, b, r;

        if (pos + 14u + len > ditl_len)
            break;
        t = (int16_t)rd16(ditl, ditl_len, pos + 4u);
        l = (int16_t)rd16(ditl, ditl_len, pos + 6u);
        b = (int16_t)rd16(ditl, ditl_len, pos + 8u);
        r = (int16_t)rd16(ditl, ditl_len, pos + 10u);
        if (count == 0u) {
            top = t;
            left = l;
            bottom = b;
            right = r;
        } else {
            if (t < top)
                top = t;
            if (l < left)
                left = l;
            if (b > bottom)
                bottom = b;
            if (r > right)
                right = r;
        }
        total_text += len + 1u;
        count++;
        pos += 14u + len;
    }
    if (count == 0u)
        return 0u;

    entries = count * QRX_M68K_DLG_ENTRY;
    rec = hle_new_ptr(m, QRX_M68K_DLG_RECORD);
    table = hle_new_ptr(m, entries + total_text);
    if (rec == 0u || table == 0u)
        return 0u;
    wr16(m->mem, m->mem_size, rec + QRX_M68K_DLG_KIND, 2u);   /* dialogKind */
    m->mem[rec + QRX_M68K_DLG_VISIBLE] = 1u;
    wr16(m->mem, m->mem_size, rec + QRX_M68K_DLG_ITEMS, (uint16_t)count);
    wr16(m->mem, m->mem_size, rec + QRX_M68K_DLG_HIT, 0u);
    qd_write_rect(m, rec + QRX_M68K_DLG_BOUNDS, top, left, bottom, right);
    wr32(m->mem, m->mem_size, rec + QRX_M68K_DLG_TABLE, table);

    pos = 2u;
    off = 0u;                       /* second pass: item table and texts */
    for (i = 0u; i < count; i++) {
        uint32_t len = ditl[pos + 13u];
        uint32_t entry = table + i * QRX_M68K_DLG_ENTRY;
        uint32_t text = table + entries + off;

        qd_write_rect(m, entry, (int16_t)rd16(ditl, ditl_len, pos + 4u),
                      (int16_t)rd16(ditl, ditl_len, pos + 6u),
                      (int16_t)rd16(ditl, ditl_len, pos + 8u),
                      (int16_t)rd16(ditl, ditl_len, pos + 10u));
        m->mem[entry + 8u] = ditl[pos + 12u];       /* item type */
        m->mem[entry + 9u] = (uint8_t)len;
        wr16(m->mem, m->mem_size, entry + 10u, (uint16_t)off);
        if (len != 0u)
            memcpy(m->mem + text, ditl + pos + 14u, len);
        m->mem[text + len] = 0u;
        off += len + 1u;
        pos += 14u + len;
    }
    dlg_build_controls(m, rec, count, table);
    if (m->ndialog < QRX_M68K_DIALOG_MAX) {
        m->dialog_rec[m->ndialog].rec = rec;
        m->dialog_rec[m->ndialog].count = (uint16_t)count;
        m->ndialog++;
    }
    return rec;
}

/* DrawDialog: the dialog frame, a frame around the buttons and the text of
 * every item that has one. */
static void hle_draw_dialog(qrx_m68k_t *m, uint32_t rec)
{
    uint32_t table, entries;
    int32_t top, left, bottom, right;
    unsigned count = 0u, i, known = 0u;

    for (i = 0u; i < m->ndialog; i++)
        if (m->dialog_rec[i].rec == rec) {
            count = m->dialog_rec[i].count;
            known = 1u;
        }
    if (!known)
        return;
    if (!qd_read_rect(m, rec + QRX_M68K_DLG_BOUNDS, &top, &left, &bottom,
                      &right))
        return;
    if (m->has_gfx && m->gfx.rect)
        m->gfx.rect(m->gfx.ctx, top, left, bottom, right, QRX_M68K_RECT_FRAME,
                    qd_black);
    table = rd32(m->mem, m->mem_size, rec + QRX_M68K_DLG_TABLE);
    entries = (uint32_t)count * QRX_M68K_DLG_ENTRY;
    for (i = 0u; i < count; i++) {
        uint32_t entry = table + i * QRX_M68K_DLG_ENTRY;
        uint32_t len = m->mem[entry + 9u];
        uint32_t off = rd16(m->mem, m->mem_size, entry + 10u);
        uint8_t type = m->mem[entry + 8u];

        if (!qd_read_rect(m, entry, &top, &left, &bottom, &right))
            continue;
        if (type == 4u || type == 5u || type == 6u || type == 7u) {
            if (m->has_gfx && m->gfx.rect)
                m->gfx.rect(m->gfx.ctx, top, left, bottom, right,
                            QRX_M68K_RECT_FRAME, qd_black);
        }
        if (len != 0u && m->has_gfx && m->gfx.text)
            m->gfx.text(m->gfx.ctx, top, left + 2,
                        (const char *)(m->mem + table + entries + off), len,
                        m->text_font, m->text_size, m->text_face);
    }
}

/* ------------------------------------------------------- windows and menus */

static int win_slot(qrx_m68k_t *m, uint32_t rec)
{
    unsigned i;

    if (rec == 0u)
        return -1;
    for (i = 0u; i < m->nwindow; i++)
        if (m->win[i].used && m->win[i].rec == rec)
            return (int)i;
    return -1;
}

/* Publish the fields the guest record mirrors; the rest stays host-side. */
static void win_write(qrx_m68k_t *m, int slot)
{
    uint32_t a = m->win[slot].rec;
    uint32_t next = 0u, len;
    unsigned i;

    for (i = (unsigned)slot + 1u; i < m->nwindow; i++)
        if (m->win[i].used) {
            next = m->win[i].rec;
            break;
        }
    len = (uint32_t)strlen(m->win[slot].title);
    if (len > QRX_M68K_MENU_TEXT - 1u)
        len = QRX_M68K_MENU_TEXT - 1u;
    wr16(m->mem, m->mem_size, a + QRX_M68K_WIN_KIND, 0u);
    m->mem[a + QRX_M68K_WIN_VISIBLE] = (uint8_t)(m->win[slot].visible ? 1u : 0u);
    m->mem[a + QRX_M68K_WIN_HILITED] = (uint8_t)(m->win[slot].hilited ? 1u : 0u);
    wr32(m->mem, m->mem_size, a + QRX_M68K_WIN_REFCON, m->win[slot].wref);
    qd_write_rect(m, a + QRX_M68K_WIN_FRAME, m->win[slot].top,
                  m->win[slot].left, m->win[slot].bottom, m->win[slot].right);
    wr32(m->mem, m->mem_size, a + QRX_M68K_WIN_NEXT, next);
    m->mem[a + QRX_M68K_WIN_TITLE] = (uint8_t)len;
    memcpy(m->mem + a + QRX_M68K_WIN_TITLE + 1u, m->win[slot].title, len);
}

static void win_notify(qrx_m68k_t *m, int op, int slot)
{
    if (slot < 0 || !m->has_gfx || !m->gfx.window)
        return;
    m->gfx.window(m->gfx.ctx, op, m->win[slot].top, m->win[slot].left,
                  m->win[slot].bottom, m->win[slot].right, m->win[slot].title,
                  (uint32_t)strlen(m->win[slot].title), m->win[slot].rec);
}

/* InvalRect carries its own rectangle. */
static void win_notify_rect(qrx_m68k_t *m, int op, int slot, int32_t top,
                            int32_t left, int32_t bottom, int32_t right)
{
    if (slot < 0 || !m->has_gfx || !m->gfx.window)
        return;
    m->gfx.window(m->gfx.ctx, op, top, left, bottom, right, m->win[slot].title,
                  (uint32_t)strlen(m->win[slot].title), m->win[slot].rec);
}

static uint32_t hle_new_window(qrx_m68k_t *m, uint32_t bounds, uint32_t title,
                               uint32_t visible, uint32_t wref)
{
    int32_t top, left, bottom, right;
    unsigned slot = QRX_M68K_WINDOW_MAX, i;
    uint32_t rec, len;

    if (!qd_read_rect(m, bounds, &top, &left, &bottom, &right))
        return 0u;
    for (i = 0u; i < m->nwindow; i++)
        if (!m->win[i].used) {
            slot = i;
            break;
        }
    if (slot == QRX_M68K_WINDOW_MAX) {
        if (m->nwindow >= QRX_M68K_WINDOW_MAX)
            return 0u;
        slot = m->nwindow++;
    }
    rec = hle_new_ptr(m, QRX_M68K_WINDOW_RECORD);
    if (rec == 0u)
        return 0u;
    memset(m->mem + rec, 0, QRX_M68K_WINDOW_RECORD);
    memset(&m->win[slot], 0, sizeof m->win[slot]);
    m->win[slot].rec = rec;
    m->win[slot].used = 1;
    m->win[slot].wref = wref;
    m->win[slot].top = top;
    m->win[slot].left = left;
    m->win[slot].bottom = bottom;
    m->win[slot].right = right;
    if (title != 0u && mem_span(m, title, 1u)) {
        len = m->mem[title];
        if (len > QRX_M68K_MENU_TEXT - 1u)
            len = QRX_M68K_MENU_TEXT - 1u;
        if (mem_span(m, title + 1u, len)) {
            memcpy(m->win[slot].title, m->mem + title + 1u, len);
            m->win[slot].title[len] = '\0';
        }
    }
    win_write(m, (int)slot);
    win_notify(m, QRX_M68K_WIN_CREATE, (int)slot);

    if (m->front != 0u) {                 /* the new window comes to the front */
        int old = win_slot(m, m->front);

        if (old >= 0) {
            m->win[old].hilited = 0;
            win_write(m, old);
            win_notify(m, QRX_M68K_WIN_HILITE, old);
        }
    }
    m->front = rec;
    m->win[slot].hilited = 1;
    if (visible) {
        m->win[slot].visible = 1;
        win_notify(m, QRX_M68K_WIN_SHOW, (int)slot);
    }
    win_write(m, (int)slot);
    return rec;
}

static void hle_select_window(qrx_m68k_t *m, uint32_t rec)
{
    int slot = win_slot(m, rec), old;

    if (slot < 0)
        return;
    if (m->front != rec) {
        old = win_slot(m, m->front);
        if (old >= 0) {
            m->win[old].hilited = 0;
            win_write(m, old);
            win_notify(m, QRX_M68K_WIN_HILITE, old);
        }
        m->front = rec;
        m->win[slot].hilited = 1;
        win_write(m, slot);
    }
    win_notify(m, QRX_M68K_WIN_SELECT, slot);
}

static void hle_dispose_window(qrx_m68k_t *m, uint32_t rec)
{
    int slot = win_slot(m, rec);
    unsigned i;

    if (slot < 0)
        return;
    win_notify(m, QRX_M68K_WIN_DISPOSE, slot);
    /* Controls that name this window go with it, so a record address that is
     * handed out again cannot adopt them. */
    for (i = 0u; i < QRX_M68K_CONTROL_MAX; i++)
        if (m->ctrl[i].used && m->ctrl[i].owner == rec) {
            hle_free_ptr(m, m->ctrl[i].handle, 8u);
            m->ctrl[i].used = 0;
        }
    if (m->front == rec)
        m->front = 0u;
    hle_free_ptr(m, rec, QRX_M68K_WINDOW_RECORD);
    memset(&m->win[slot], 0, sizeof m->win[slot]);
}

static int menu_slot(qrx_m68k_t *m, uint32_t handle)
{
    unsigned i;

    if (handle == 0u)
        return -1;
    for (i = 0u; i < m->nmenu; i++)
        if (m->menu[i].used && m->menu[i].handle == handle)
            return (int)i;
    return -1;
}

/* The handle's block carries the MenuInfo fields a menu needs. */
static void menu_write(qrx_m68k_t *m, int slot)
{
    uint32_t block = rd32(m->mem, m->mem_size, m->menu[slot].handle);
    uint32_t bits = 0u;
    unsigned i;

    if (block == 0u || block + 16u > m->mem_size)
        return;
    wr16(m->mem, m->mem_size, block, (uint16_t)m->menu[slot].id);
    wr16(m->mem, m->mem_size, block + 2u, 0u);            /* menuWidth */
    wr16(m->mem, m->mem_size, block + 4u,
         (uint16_t)(m->menu[slot].count * 12u));          /* menuHeight */
    for (i = 0u; i < m->menu[slot].count && i < 16u; i++)
        if (m->menu[slot].enabled[i])
            bits |= 1u << i;
    wr16(m->mem, m->mem_size, block + 10u, (uint16_t)bits);
}

static int menu_slot_by_id(qrx_m68k_t *m, uint32_t id)
{
    unsigned i;

    for (i = 0u; i < m->nmbar; i++) {
        int slot = menu_slot(m, m->mbar[i]);

        if (slot >= 0 && (uint32_t)m->menu[slot].id == id)
            return slot;
    }
    return -1;
}

static void menu_notify(qrx_m68k_t *m, int op, int slot, int32_t item,
                        int32_t value)
{
    if (slot < 0 || !m->has_gfx || !m->gfx.menu)
        return;
    m->gfx.menu(m->gfx.ctx, op, m->menu[slot].title,
                (uint32_t)strlen(m->menu[slot].title), item, 0, 0u, value,
                m->menu[slot].handle);
}

/* Enable, Disable and CheckItem touch nothing but flags of an item that is
 * already in the menu, so the hook says so instead of asking for a rebuild. */
static void menu_notify_state(qrx_m68k_t *m, int slot, unsigned item)
{
    if (slot < 0 || !m->has_gfx || !m->gfx.menu || item >= m->menu[slot].count)
        return;
    m->gfx.menu(m->gfx.ctx, QRX_M68K_MENU_ITEM_STATE, m->menu[slot].title,
                (uint32_t)strlen(m->menu[slot].title), (int32_t)item,
                m->menu[slot].item[item],
                (uint32_t)strlen(m->menu[slot].item[item]),
                (m->menu[slot].enabled[item] ? 1 : 0) |
                    (m->menu[slot].checked[item] ? 2 : 0),
                m->menu[slot].handle);
}

static void menu_notify_item(qrx_m68k_t *m, int slot, int op, unsigned item,
                             int32_t value)
{
    if (slot < 0 || !m->has_gfx || !m->gfx.menu)
        return;
    m->gfx.menu(m->gfx.ctx, op, m->menu[slot].title,
                (uint32_t)strlen(m->menu[slot].title), (int32_t)item,
                m->menu[slot].item[item], (uint32_t)strlen(m->menu[slot].item[item]),
                value, m->menu[slot].handle);
}

static uint32_t hle_new_menu(qrx_m68k_t *m, int16_t id, uint32_t title)
{
    unsigned slot = QRX_M68K_MENU_MAX, i;
    uint32_t h, len;

    for (i = 0u; i < m->nmenu; i++)
        if (!m->menu[i].used) {
            slot = i;
            break;
        }
    if (slot == QRX_M68K_MENU_MAX) {
        if (m->nmenu >= QRX_M68K_MENU_MAX)
            return 0u;
        slot = m->nmenu++;
    }
    h = hle_new_handle(m, 16u);
    if (h == 0u)
        return 0u;
    memset(&m->menu[slot], 0, sizeof m->menu[slot]);
    m->menu[slot].handle = h;
    m->menu[slot].used = 1;
    m->menu[slot].id = id;
    if (title != 0u && mem_span(m, title, 1u)) {
        len = m->mem[title];
        if (len > QRX_M68K_MENU_TEXT - 1u)
            len = QRX_M68K_MENU_TEXT - 1u;
        if (mem_span(m, title + 1u, len)) {
            memcpy(m->menu[slot].title, m->mem + title + 1u, len);
            m->menu[slot].title[len] = '\0';
        }
    }
    menu_write(m, (int)slot);
    menu_notify(m, QRX_M68K_MENU_NEW, (int)slot, 0, (int32_t)id);
    return h;
}

/* Put a menu into the bar at position before (0 = after the last one). */
static void hle_insert_menu(qrx_m68k_t *m, uint32_t handle, uint32_t before_id)
{
    unsigned pos = m->nmbar, i;

    if (m->nmbar >= QRX_M68K_MENU_BAR_MAX)
        return;
    if (before_id != 0u)
        for (i = 0u; i < m->nmbar; i++) {
            int slot = menu_slot(m, m->mbar[i]);

            if (slot >= 0 && (uint32_t)m->menu[slot].id == before_id) {
                pos = i;
                break;
            }
        }
    for (i = m->nmbar; i > pos; i--)
        m->mbar[i] = m->mbar[i - 1u];
    m->mbar[pos] = handle;
    m->nmbar++;
}

/* The menu bar is a filled strip with one title per menu, then every item. */
static void hle_draw_menu_bar(qrx_m68k_t *m)
{
    int32_t x = 4;
    unsigned i;

    /* The bar itself is the guest's: it turns these calls into an Intuition
     * menu strip, so nothing is drawn into a window here. */
    for (i = 0u; i < m->nmbar; i++) {
        int slot = menu_slot(m, m->mbar[i]);
        unsigned j;

        if (slot < 0)
            continue;
        if (m->has_gfx && m->gfx.menu)
            m->gfx.menu(m->gfx.ctx, QRX_M68K_MENU_BAR, m->menu[slot].title,
                        (uint32_t)strlen(m->menu[slot].title), (int32_t)i, 0, 0u,
                        x, m->menu[slot].handle);
        x += 12 + (int32_t)strlen(m->menu[slot].title) * 8;
        for (j = 0u; j < m->menu[slot].count; j++)
            menu_notify_item(m, slot, QRX_M68K_MENU_ITEM, j,
                             m->menu[slot].enabled[j] ? 1 : 0);
    }
}

/* Answer one QuickDraw, Event Manager or Dialog Manager trap.  Returns 0 for a
 * trap this slice does not know, so dispatch can report it as missing. */
static int hle_toolbox(qrx_m68k_t *m, uint32_t trap, uint32_t *result)
{
    uint32_t d0 = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t d1 = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t d2 = m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t d3 = m68k_get_reg(NULL, M68K_REG_D3);
    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t a1 = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t a2 = m68k_get_reg(NULL, M68K_REG_A2);
    int32_t top, left, bottom, right, vy, vx;
    uint32_t out = 0u;

    switch (trap) {
    case QRX_M68K_TRAP_INIT_CURSOR:   /* A850 */
    case QRX_M68K_TRAP_HIDE_CURSOR:   /* A852 */
    case QRX_M68K_TRAP_SHOW_CURSOR:   /* A853 */
    case QRX_M68K_TRAP_SET_CLIP:      /* A879 */
    case QRX_M68K_TRAP_BACK_PAT:      /* A87C */
    case QRX_M68K_TRAP_PORT_SIZE:     /* A876 */
    case QRX_M68K_TRAP_MOVE_PORT_TO:  /* A877 */
    case QRX_M68K_TRAP_SYSTEM_TASK:   /* A9B4 */
    case QRX_M68K_TRAP_SYS_BEEP:      /* A9C8 */
    case QRX_M68K_TRAP_INIT_DIALOGS:  /* A97B */
        break;                        /* accepted, nothing to keep */
    case QRX_M68K_TRAP_INIT_GRAF:     /* A86E: A0 = &thePort */
        if (!mem_span(m, a0, 4u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        wr32(m->mem, m->mem_size, a0, m->port);
        break;
    case QRX_M68K_TRAP_INIT_PORT:     /* A86D */
    case QRX_M68K_TRAP_OPEN_PORT:     /* A86F */
    case QRX_M68K_TRAP_SET_PORT:      /* A873 */
        if (a0 == 0u) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        m->port = a0;
        m->pen_y = 0;
        m->pen_x = 0;
        if (m->has_gfx && m->gfx.target)
            m->gfx.target(m->gfx.ctx, a0);
        break;
    case QRX_M68K_TRAP_GET_PORT:      /* A874: A0 = GrafPtr cell */
        if (!mem_span(m, a0, 4u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        wr32(m->mem, m->mem_size, a0, m->port);
        break;
    case QRX_M68K_TRAP_CLOSE_PORT:    /* A87D */
        if (a0 == m->port)
            m->port = 0u;
        break;
    case QRX_M68K_TRAP_SET_PT:        /* A880: h in D0, v in D1 */
        if (!mem_span(m, a0, 4u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        wr16(m->mem, m->mem_size, a0, (uint16_t)d1);
        wr16(m->mem, m->mem_size, a0 + 2u, (uint16_t)d0);
        break;
    case QRX_M68K_TRAP_MOVE_TO:       /* A893: A0 = Point* */
        if (!mem_span(m, a0, 4u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        m->pen_y = (int16_t)rd16(m->mem, m->mem_size, a0);
        m->pen_x = (int16_t)rd16(m->mem, m->mem_size, a0 + 2u);
        break;
    case QRX_M68K_TRAP_LINE_TO:       /* A891 */
        if (!mem_span(m, a0, 4u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        qd_draw_line(m, m->pen_y, m->pen_x,
                     (int16_t)rd16(m->mem, m->mem_size, a0),
                     (int16_t)rd16(m->mem, m->mem_size, a0 + 2u));
        break;
    case QRX_M68K_TRAP_LINE:          /* A892: from, to */
        if (!mem_span(m, a0, 4u) || !mem_span(m, a1, 4u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        qd_draw_line(m, (int16_t)rd16(m->mem, m->mem_size, a0),
                     (int16_t)rd16(m->mem, m->mem_size, a0 + 2u),
                     (int16_t)rd16(m->mem, m->mem_size, a1),
                     (int16_t)rd16(m->mem, m->mem_size, a1 + 2u));
        break;
    case QRX_M68K_TRAP_HIDE_PEN:      /* A896 */
        m->pen_vis = 0;
        break;
    case QRX_M68K_TRAP_SHOW_PEN:      /* A897 */
        m->pen_vis = 1;
        break;
    case QRX_M68K_TRAP_GET_PEN:       /* A89A: A0 = Point* */
        if (!mem_span(m, a0, 4u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        wr16(m->mem, m->mem_size, a0, (uint16_t)m->pen_y);
        wr16(m->mem, m->mem_size, a0 + 2u, (uint16_t)m->pen_x);
        break;
    case QRX_M68K_TRAP_PEN_SIZE:      /* A89B: width in D0, height in D1 */
        m->pen_size_w = (uint16_t)(d0 & 0xFFFFu) ? (uint16_t)(d0 & 0xFFFFu) : 1u;
        m->pen_size_h = (uint16_t)(d1 & 0xFFFFu) ? (uint16_t)(d1 & 0xFFFFu) : 1u;
        break;
    case QRX_M68K_TRAP_PEN_MODE:      /* A89C */
        m->pen_mode = (uint16_t)(d0 & 0x000Fu);
        break;
    case QRX_M68K_TRAP_PEN_PAT:       /* A89D */
        if (!mem_span(m, a0, 8u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        memcpy(m->pen_pat, m->mem + a0, 8u);
        break;
    case QRX_M68K_TRAP_PEN_NORMAL:    /* A89E */
        m->pen_mode = 0u;
        m->pen_size_w = 1u;
        m->pen_size_h = 1u;
        memcpy(m->pen_pat, qd_black, 8u);
        break;
    case QRX_M68K_TRAP_FRAME_RECT:    /* A8A1 */
        qd_draw(m, QRX_M68K_RECT_FRAME, a0, m->pen_pat);
        break;
    case QRX_M68K_TRAP_PAINT_RECT:    /* A8A2 */
        qd_draw(m, QRX_M68K_RECT_PAINT, a0, m->pen_pat);
        break;
    case QRX_M68K_TRAP_ERASE_RECT:    /* A8A3 */
        qd_draw(m, QRX_M68K_RECT_ERASE, a0, m->pen_pat);
        break;
    case QRX_M68K_TRAP_INVERT_RECT:   /* A8A4 */
        qd_draw(m, QRX_M68K_RECT_INVERT, a0, m->pen_pat);
        break;
    case QRX_M68K_TRAP_FILL_RECT:     /* A8A5: pattern in A1 */
        if (!mem_span(m, a1, 8u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        qd_draw(m, QRX_M68K_RECT_FILL, a0, m->mem + a1);
        break;
    case QRX_M68K_TRAP_EQUAL_RECT:    /* A8A6 */
        if (!qd_read_rect(m, a0, &top, &left, &bottom, &right) ||
            !qd_read_rect(m, a1, &vy, &vx, &vy, &vx)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        out = (vy == top && vx == left) ? 1u : 0u;
        break;
    case QRX_M68K_TRAP_OFFSET_RECT:   /* A8A8: dh in D0, dv in D1 */
        if (!qd_read_rect(m, a0, &top, &left, &bottom, &right)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        qd_write_rect(m, a0, top + (int16_t)d1, left + (int16_t)d0,
                      bottom + (int16_t)d1, right + (int16_t)d0);
        break;
    case QRX_M68K_TRAP_INSET_RECT:    /* A8A9 */
        if (!qd_read_rect(m, a0, &top, &left, &bottom, &right)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        qd_write_rect(m, a0, top + (int16_t)d1, left + (int16_t)d0,
                      bottom - (int16_t)d1, right - (int16_t)d0);
        break;
    case QRX_M68K_TRAP_PT_IN_RECT:    /* A8AD: point, rectangle */
        if (!mem_span(m, a0, 4u) ||
            !qd_read_rect(m, a1, &top, &left, &bottom, &right)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        vy = (int16_t)rd16(m->mem, m->mem_size, a0);
        vx = (int16_t)rd16(m->mem, m->mem_size, a0 + 2u);
        out = (vx >= left && vx < right && vy >= top && vy < bottom) ? 1u : 0u;
        break;
    case QRX_M68K_TRAP_DRAW_CHAR: {   /* A883: character in D0 */
        char c = (char)(d0 & 0xFFu);
        qd_draw_text(m, &c, 1u);
        break;
    }
    case QRX_M68K_TRAP_DRAW_STRING:   /* A884: A0 = Pascal string */
        if (!mem_span(m, a0, 1u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        {
            uint32_t len = m->mem[a0];

            if (!mem_span(m, a0 + 1u, len)) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            qd_draw_text(m, (const char *)(m->mem + a0 + 1u), len);
        }
        break;
    case QRX_M68K_TRAP_DRAW_TEXT: {   /* A885: buffer, first D0, count D1 */
        int32_t first = (int16_t)(d0 & 0xFFFFu);
        int32_t count = (int16_t)(d1 & 0xFFFFu);

        if (first < 0 || count < 0 ||
            !mem_span(m, a0, (uint32_t)first + (uint32_t)count)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        qd_draw_text(m, (const char *)(m->mem + a0 + (uint32_t)first),
                     (uint32_t)count);
        break;
    }
    case QRX_M68K_TRAP_TEXT_FONT:     /* A887 */
        m->text_font = (uint16_t)d0;
        break;
    case QRX_M68K_TRAP_TEXT_FACE:     /* A888 */
        m->text_face = (uint16_t)d0;
        break;
    case QRX_M68K_TRAP_TEXT_MODE:     /* A889 */
        m->text_mode = (uint16_t)(d0 & 0x000Fu);
        break;
    case QRX_M68K_TRAP_TEXT_SIZE: {   /* A88A */
        int32_t size = (int16_t)(d0 & 0xFFFFu);

        if (size <= 0 || size > 96) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        m->text_size = (uint16_t)size;
        break;
    }
    case QRX_M68K_TRAP_TICK_COUNT:    /* A975 */
        ev_poll(m);
        m->ticks++;
        out = m->ticks;
        break;
    case QRX_M68K_TRAP_GET_MOUSE:     /* A972: A0 = Point* */
        ev_poll(m);
        if (!mem_span(m, a0, 4u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        wr16(m->mem, m->mem_size, a0, (uint16_t)m->mouse_y);
        wr16(m->mem, m->mem_size, a0 + 2u, (uint16_t)m->mouse_x);
        break;
    case QRX_M68K_TRAP_BUTTON:        /* A974 */
    case QRX_M68K_TRAP_STILL_DOWN:    /* A973 */
    case QRX_M68K_TRAP_WAIT_MOUSE_UP: /* A977 */
        ev_poll(m);
        out = m->mouse_down ? 1u : 0u;
        break;
    case QRX_M68K_TRAP_GET_NEXT_EVENT:  /* mask D0, record A0 */
    case QRX_M68K_TRAP_EVENT_AVAIL:     /* A971 */
        ev_poll(m);
        out = (uint32_t)ev_take(m, d0, a0,
                                trap == QRX_M68K_TRAP_GET_NEXT_EVENT);
        break;
    case QRX_M68K_TRAP_WAIT_NEXT_EVENT: /* mask D0, record A0, sleep D1 */
        ev_poll(m);
        out = (uint32_t)ev_take(m, d0, a0, 1);
        if (out == 0u)
            m->ticks++;                 /* a sleepless sleep still ticks */
        break;
    case QRX_M68K_TRAP_GET_KEYS:      /* A976: A0 = 16-byte key map */
        if (!mem_span(m, a0, 16u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        memset(m->mem + a0, 0, 16u);
        break;
    case QRX_M68K_TRAP_POST_EVENT: {  /* what in D0, event record in A0 */
        uint16_t what = (uint16_t)(d0 & 0xFFFFu);
        int32_t vy = m->mouse_y, vx = m->mouse_x;

        /* The where the caller put into the event record is the click point. */
        if (mem_span(m, a0, QRX_M68K_EVENT_RECORD)) {
            vy = (int16_t)rd16(m->mem, m->mem_size, a0 + 10u);
            vx = (int16_t)rd16(m->mem, m->mem_size, a0 + 12u);
        }
        (void)qrx_m68k_post_event(m, what, a0, vy, vx, 0u);
        break;
    }
    case QRX_M68K_TRAP_FLUSH_EVENTS:  /* A032 */
        m->nevent = 0u;
        m->mouse_down = 0;
        break;
    case QRX_M68K_TRAP_INFO_SCRAP:    /* A9F9: no scrap record, no flavors */
        out = 0u;
        break;
    case QRX_M68K_TRAP_UNLOAD_SCRAP:  /* A9FA: the clip has no disk side */
    case QRX_M68K_TRAP_LOAD_SCRAP:    /* A9FB */
        out = 0u;
        break;
    case QRX_M68K_TRAP_ZERO_SCRAP:    /* A9FC: clear the clip before a copy */
        if (m->has_clip)
            m->clip.put(m->clip.ctx, (const uint8_t *)"", 0u);
        out = 0u;
        break;
    case QRX_M68K_TRAP_PUT_SCRAP: {   /* A9FE: length D0, type D1, data A0 */
        if (!m->has_clip) {
            out = QRX_M68K_SCRAP_NO_SCRAP;
            break;
        }
        if (d1 != 0x54455854u)        /* only 'TEXT' has a host counterpart */
            break;
        if (d0 != 0u && !mem_span(m, a0, d0)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        m->clip.put(m->clip.ctx, m->mem + a0, d0);
        break;
    }
    case QRX_M68K_TRAP_GET_SCRAP: {   /* A9FD: hDest A0, type D0, offset A1 */
        int32_t got;
        uint32_t len, block;

        if (mem_span(m, a1, 4u))
            wr32(m->mem, m->mem_size, a1, 0u);
        if (!m->has_clip) {
            out = QRX_M68K_SCRAP_NO_SCRAP;
            break;
        }
        if (d0 != 0x54455854u) {      /* noTypeErr: the clip holds text */
            out = QRX_M68K_SCRAP_NO_TYPE;
            break;
        }
        len = (uint32_t)m->clip.size(m->clip.ctx);
        if (len == 0u) {
            out = 0u;
            break;
        }
        if (a0 == 0u) {               /* a nil destination only asks the size */
            out = len;
            break;
        }
        if (hle_set_handle_size(m, a0, len) == 0u) {
            out = QRX_M68K_SCRAP_NO_MEM;
            break;
        }
        block = rd32(m->mem, m->mem_size, a0);
        got = m->clip.get(m->clip.ctx, m->mem + block, len);
        out = got > 0 ? (uint32_t)got : 0u;
        break;
    }
    case QRX_M68K_TRAP_GET_NEW_DIALOG: /* A97C: id D0, storage A0, behind A1 */
        out = hle_get_new_dialog(m, (int16_t)(d0 & 0xFFFFu));
        break;
    case QRX_M68K_TRAP_DRAW_DIALOG:   /* A981 */
        hle_draw_dialog(m, a0);
        break;
    case QRX_M68K_TRAP_CLOSE_DIALOG:   /* A982 */
    case QRX_M68K_TRAP_DISPOSE_DIALOG: /* A983: controls, table and record go */
        dlg_free(m, a0);
        break;
    case QRX_M68K_TRAP_MODAL_DIALOG: {  /* A991: filter A0, VAR itemHit A1 */
        /* The filterProc names no dialog, so any dialog control under the
         * click answers; item 1 stays the default without a click. */
        int item = dlg_take_click(m, 0u);

        if (mem_span(m, a1, 2u))
            wr16(m->mem, m->mem_size, a1, (uint16_t)(item != 0 ? item : 1));
        break;
    }
    case QRX_M68K_TRAP_IS_DIALOG_EVENT: { /* A97F: event A0, dialog A1 */
        uint16_t what;
        int32_t vy, vx;

        out = 0u;
        if (!mem_span(m, a0, QRX_M68K_EVENT_RECORD))
            break;
        what = rd16(m->mem, m->mem_size, a0);
        vy = (int16_t)rd16(m->mem, m->mem_size, a0 + 10u);
        vx = (int16_t)rd16(m->mem, m->mem_size, a0 + 12u);
        if (what == 1u) {               /* mouseDown in a control */
            out = dlg_item_at(m, a1, vy, vx) != 0 ? 1u : 0u;
        } else if (what == 3u) {        /* keyDown: the default button */
            unsigned i;

            for (i = 0u; i < QRX_M68K_CONTROL_MAX; i++)
                if (m->ctrl[i].used && m->ctrl[i].item != 0 &&
                    (a1 == 0u || m->ctrl[i].owner == a1)) {
                    out = 1u;
                    break;
                }
        }
        break;
    }
    case QRX_M68K_TRAP_DIALOG_SELECT: { /* A980: event A0, dialog A1, VAR A2 */
        uint16_t what = 0u;
        int32_t vy = 0, vx = 0;
        int item = 0;

        if (mem_span(m, a0, QRX_M68K_EVENT_RECORD)) {
            what = rd16(m->mem, m->mem_size, a0);
            vy = (int16_t)rd16(m->mem, m->mem_size, a0 + 10u);
            vx = (int16_t)rd16(m->mem, m->mem_size, a0 + 12u);
        }
        if (what == 1u)
            item = dlg_item_at(m, a1, vy, vx);
        if (mem_span(m, a2, 2u))
            wr16(m->mem, m->mem_size, a2, (uint16_t)item);
        out = item != 0 ? 1u : 0u;
        break;
    }
    case QRX_M68K_TRAP_ALERT:         /* A985 */
    case QRX_M68K_TRAP_STOP_ALERT:    /* A986 */
    case QRX_M68K_TRAP_NOTE_ALERT:    /* A987 */
    case QRX_M68K_TRAP_CAUTION_ALERT: /* A988: the default button */
        out = 1u;
        break;
    /* ---------------- Window Manager */
    case QRX_M68K_TRAP_INIT_WINDOWS:      /* A912 */
    case QRX_M68K_TRAP_INIT_MENUS:        /* A930 */
        break;
    case QRX_M68K_TRAP_GET_WMGR_PORT: {   /* A910: A0 = GrafPort cell */
        if (!mem_span(m, a0, 4u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        if (m->wmgr_port == 0u)
            m->wmgr_port = hle_new_ptr(m, 32u);
        wr32(m->mem, m->mem_size, a0, m->wmgr_port);
        break;
    }
    case QRX_M68K_TRAP_NEW_WINDOW:        /* A913: storage, bounds, title,
                                           * behind, visible, procID, goAway,
                                           * refCon */
        out = hle_new_window(m, a1, a2, d0 & 0xFFFFu, d3);
        break;
    case QRX_M68K_TRAP_GET_WREF_CON:      /* A917 */
        {
            int slot = win_slot(m, a0);

            out = slot < 0 ? 0u : m->win[slot].wref;
        }
        break;
    case QRX_M68K_TRAP_SET_WREF_CON:      /* A918 */
        {
            int slot = win_slot(m, a0);

            if (slot >= 0) {
                m->win[slot].wref = d0;
                win_write(m, slot);
            }
        }
        break;
    case QRX_M68K_TRAP_GET_WTITLE:        /* A919: window, Str255 out */
        {
            int slot = win_slot(m, a0);
            uint32_t len;

            if (slot < 0 || !mem_span(m, a1, 1u)) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            len = (uint32_t)strlen(m->win[slot].title);
            if (!mem_span(m, a1 + 1u, len)) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            m->mem[a1] = (uint8_t)len;
            memcpy(m->mem + a1 + 1u, m->win[slot].title, len);
        }
        break;
    case QRX_M68K_TRAP_SET_WTITLE:        /* A91A: window, Str255 */
        {
            int slot = win_slot(m, a0);
            uint32_t len;

            if (slot < 0 || !mem_span(m, a1, 1u)) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            len = m->mem[a1];
            if (len > QRX_M68K_MENU_TEXT - 1u)
                len = QRX_M68K_MENU_TEXT - 1u;
            if (!mem_span(m, a1 + 1u, len)) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            memcpy(m->win[slot].title, m->mem + a1 + 1u, len);
            m->win[slot].title[len] = '\0';
            win_write(m, slot);
            win_notify(m, QRX_M68K_WIN_TITLE_CHANGED, slot);
        }
        break;
    case QRX_M68K_TRAP_SHOW_WINDOW:       /* A915 */
    case QRX_M68K_TRAP_HIDE_WINDOW:       /* A916 */
        {
            int slot = win_slot(m, a0);

            if (slot < 0) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            m->win[slot].visible = trap == QRX_M68K_TRAP_SHOW_WINDOW;
            win_write(m, slot);
            win_notify(m, m->win[slot].visible ? QRX_M68K_WIN_SHOW
                                               : QRX_M68K_WIN_HIDE, slot);
        }
        break;
    case QRX_M68K_TRAP_MOVE_WINDOW:       /* A91B: window, h, v */
        {
            int slot = win_slot(m, a0);
            int32_t dh = (int16_t)(d0 & 0xFFFFu);
            int32_t dv = (int16_t)(d1 & 0xFFFFu);

            if (slot < 0) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            m->win[slot].top += dv;
            m->win[slot].bottom += dv;
            m->win[slot].left += dh;
            m->win[slot].right += dh;
            win_write(m, slot);
            win_notify(m, QRX_M68K_WIN_MOVE, slot);
        }
        break;
    case QRX_M68K_TRAP_SIZE_WINDOW:       /* A91D: window, width, height */
        {
            int slot = win_slot(m, a0);

            if (slot < 0) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            m->win[slot].right = m->win[slot].left + (int16_t)(d0 & 0xFFFFu);
            m->win[slot].bottom = m->win[slot].top + (int16_t)(d1 & 0xFFFFu);
            win_write(m, slot);
            win_notify(m, QRX_M68K_WIN_MOVE, slot);
        }
        break;
    case QRX_M68K_TRAP_HILITE_WINDOW:     /* A91C: window, hilite */
        {
            int slot = win_slot(m, a0);

            if (slot < 0) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            m->win[slot].hilited = (d0 & 0xFFFFu) != 0u;
            win_write(m, slot);
            win_notify(m, QRX_M68K_WIN_HILITE, slot);
        }
        break;
    case QRX_M68K_TRAP_SELECT_WINDOW:     /* A91F */
    case QRX_M68K_TRAP_BRING_TO_FRONT:    /* A920 */
    case QRX_M68K_TRAP_SEND_BEHIND:       /* A921 */
        if (win_slot(m, a0) < 0) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        hle_select_window(m, a0);
        break;
    case QRX_M68K_TRAP_BEGIN_UPDATE:      /* A922 */
    case QRX_M68K_TRAP_END_UPDATE:        /* A923 */
        {
            int slot = win_slot(m, a0);

            if (slot < 0) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            win_notify(m, trap == QRX_M68K_TRAP_BEGIN_UPDATE
                              ? QRX_M68K_WIN_UPDATE_BEGIN
                              : QRX_M68K_WIN_UPDATE_END, slot);
        }
        break;
    case QRX_M68K_TRAP_INVAL_RECT:        /* A928: A0 = Rect* */
        if (!qd_read_rect(m, a0, &top, &left, &bottom, &right)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        win_notify_rect(m, QRX_M68K_WIN_INVAL, win_slot(m, m->front), top, left,
                        bottom, right);
        break;
    case QRX_M68K_TRAP_FRONT_WINDOW:      /* A924 */
        out = m->front;
        break;
    case QRX_M68K_TRAP_CLOSE_WINDOW:      /* A92D */
    case QRX_M68K_TRAP_DISPOSE_WINDOW:    /* A914 */
        hle_dispose_window(m, a0);
        break;
    case QRX_M68K_TRAP_FIND_WINDOW:       /* A92C: point, VAR window */
        if (mem_span(m, a1, 4u))
            wr32(m->mem, m->mem_size, a1, 0u);
        out = 0u;                         /* inDesk */
        break;
    /* ---------------- Menu Manager */
    case QRX_M68K_TRAP_NEW_MENU:          /* A931: id in D0, title in A0 */
        out = hle_new_menu(m, (int16_t)(d0 & 0xFFFFu), a0);
        break;
    case QRX_M68K_TRAP_APPEND_MENU:       /* A933: menu, Str255 item */
        {
            int slot = menu_slot(m, a0);
            uint32_t len;

            if (slot < 0 || !mem_span(m, a1, 1u) ||
                m->menu[slot].count >= QRX_M68K_MENU_ITEM_MAX) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            len = m->mem[a1];
            if (len > QRX_M68K_MENU_TEXT - 1u)
                len = QRX_M68K_MENU_TEXT - 1u;
            if (!mem_span(m, a1 + 1u, len)) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            memcpy(m->menu[slot].item[m->menu[slot].count], m->mem + a1 + 1u,
                   len);
            m->menu[slot].item[m->menu[slot].count][len] = '\0';
            m->menu[slot].enabled[m->menu[slot].count] = 1;
            m->menu[slot].count++;
            menu_write(m, slot);
            menu_notify_item(m, slot, QRX_M68K_MENU_ITEM,
                             m->menu[slot].count - 1u, 1);
        }
        break;
    case QRX_M68K_TRAP_INSERT_MENU:       /* A935: menu, beforeID */
        hle_insert_menu(m, a0, d0 & 0xFFFFu);
        break;
    case QRX_M68K_TRAP_CLEAR_MENU_BAR:    /* A934 */
        m->nmbar = 0u;
        break;
    case QRX_M68K_TRAP_DELETE_MENU:       /* A936: id in D0 */
        {
            unsigned i;

            for (i = 0u; i < m->nmbar; i++) {
                int slot = menu_slot(m, m->mbar[i]);

                if (slot >= 0 && (uint32_t)m->menu[slot].id == (d0 & 0xFFFFu)) {
                    unsigned j;

                    for (j = i; j + 1u < m->nmbar; j++)
                        m->mbar[j] = m->mbar[j + 1u];
                    m->nmbar--;
                    break;
                }
            }
        }
        break;
    case QRX_M68K_TRAP_DISPOSE_MENU:      /* A932 */
        {
            int slot = menu_slot(m, a0);

            if (slot >= 0) {
                menu_notify(m, QRX_M68K_MENU_DISPOSE, slot, 0, 0);
                m->menu[slot].used = 0;
                m->menu[slot].handle = 0u;
            }
        }
        break;
    case QRX_M68K_TRAP_DRAW_MENU_BAR:     /* A937 */
        hle_draw_menu_bar(m);
        break;
    case QRX_M68K_TRAP_ENABLE_ITEM:       /* A939 */
    case QRX_M68K_TRAP_DISABLE_ITEM:      /* A93A */
        {
            int slot = menu_slot(m, a0);
            unsigned item = d0 & 0xFFFFu;

            if (slot < 0) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            if (item == 0u) {
                unsigned j;

                for (j = 0u; j < m->menu[slot].count; j++)
                    m->menu[slot].enabled[j] =
                        trap == QRX_M68K_TRAP_ENABLE_ITEM;
                for (j = 0u; j < m->menu[slot].count; j++)
                    menu_notify_state(m, slot, j);
            } else if (item <= m->menu[slot].count) {
                m->menu[slot].enabled[item - 1u] =
                    trap == QRX_M68K_TRAP_ENABLE_ITEM;
                menu_notify_state(m, slot, item - 1u);
            }
            menu_write(m, slot);
        }
        break;
    case QRX_M68K_TRAP_CHECK_ITEM:        /* A945: menu, item, check */
        {
            int slot = menu_slot(m, a0);
            unsigned item = d0 & 0xFFFFu;

            if (slot < 0 || item == 0u || item > m->menu[slot].count) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            m->menu[slot].checked[item - 1u] = (d1 & 0xFFFFu) != 0u;
            menu_notify_state(m, slot, item - 1u);
        }
        break;
    case QRX_M68K_TRAP_GET_MENU_ITEM_TEXT: /* A946: menu, item, Str255 out */
        {
            int slot = menu_slot(m, a0);
            unsigned item = d0 & 0xFFFFu;
            uint32_t len;

            if (slot < 0 || item == 0u || item > m->menu[slot].count ||
                !mem_span(m, a1, 1u)) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            len = (uint32_t)strlen(m->menu[slot].item[item - 1u]);
            if (!mem_span(m, a1 + 1u, len)) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            m->mem[a1] = (uint8_t)len;
            memcpy(m->mem + a1 + 1u, m->menu[slot].item[item - 1u], len);
        }
        break;
    case QRX_M68K_TRAP_SET_MENU_ITEM_TEXT: /* A947: menu, item, Str255 */
        {
            int slot = menu_slot(m, a0);
            unsigned item = d0 & 0xFFFFu;
            uint32_t len;

            if (slot < 0 || item == 0u || item > m->menu[slot].count ||
                !mem_span(m, a1, 1u)) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            len = m->mem[a1];
            if (len > QRX_M68K_MENU_TEXT - 1u)
                len = QRX_M68K_MENU_TEXT - 1u;
            if (!mem_span(m, a1 + 1u, len)) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            memcpy(m->menu[slot].item[item - 1u], m->mem + a1 + 1u, len);
            m->menu[slot].item[item - 1u][len] = '\0';
            menu_write(m, slot);
            menu_notify_item(m, slot, QRX_M68K_MENU_ITEM_TEXT, item - 1u, 1);
        }
        break;
    case QRX_M68K_TRAP_COUNT_MITEMS:      /* A950 */
        {
            int slot = menu_slot(m, a0);

            out = slot < 0 ? 0u : (uint32_t)m->menu[slot].count;
        }
        break;
    case QRX_M68K_TRAP_DELETE_MENU_ITEM:  /* A952: menu, item */
        {
            int slot = menu_slot(m, a0);
            unsigned item = d0 & 0xFFFFu, j;

            if (slot < 0 || item == 0u || item > m->menu[slot].count) {
                out = QRX_M68K_TRAP_PARAM_ERR;
                break;
            }
            for (j = item - 1u; j + 1u < m->menu[slot].count; j++) {
                strcpy(m->menu[slot].item[j], m->menu[slot].item[j + 1u]);
                m->menu[slot].enabled[j] = m->menu[slot].enabled[j + 1u];
                m->menu[slot].checked[j] = m->menu[slot].checked[j + 1u];
                m->menu[slot].cmd[j] = m->menu[slot].cmd[j + 1u];
                m->menu[slot].style[j] = m->menu[slot].style[j + 1u];
                m->menu[slot].icon[j] = m->menu[slot].icon[j + 1u];
                m->menu[slot].mark[j] = m->menu[slot].mark[j + 1u];
            }
            m->menu[slot].count--;
            menu_write(m, slot);
            menu_notify_item(m, slot, QRX_M68K_MENU_DELETE_ITEM, item - 1u, 0);
        }
        break;
    case QRX_M68K_TRAP_HILITE_MENU: { /* A938: menu ID in D0, 0 clears */
        uint32_t id = d0 & 0xFFFFu;
        int slot = menu_slot_by_id(m, id != 0u ? id : (uint32_t)m->menu_hilite);

        if (id != 0u && slot < 0)
            break;                       /* that ID is not in the bar */
        m->menu_hilite = id != 0u ? (int16_t)id : 0;
        if (slot < 0)
            slot = 0;                    /* clearing, no title left to name */
        menu_notify(m, QRX_M68K_MENU_HILITE, slot, -1, (int32_t)id);
        out = 0u;
        break;
    }
    case QRX_M68K_TRAP_MENU_SELECT: { /* A93D: startPt on the stack */
        /* The point only says where the tracking starts; what the guest
         * already picked is what MacOS would have returned. */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);

        if (mem_span(m, sp, 4u)) {
            m->menu_point_y = (int32_t)(int16_t)rd16(m->mem, m->mem_size, sp);
            m->menu_point_x =
                (int32_t)(int16_t)rd16(m->mem, m->mem_size, sp + 2u);
            m68k_set_reg(M68K_REG_A7, sp + 4u);
        }
        out = m->menu_pick_valid ? m->menu_pick : 0u;
        m->menu_pick_valid = 0;
        if (out != 0u) {
            /* MacOS leaves the title of the chosen menu highlighted until
             * HiliteMenu(0); mirror that in the model. */
            int slot = menu_slot_by_id(m, out >> 16);

            if (slot >= 0) {
                m->menu_hilite = m->menu[slot].id;
                menu_notify(m, QRX_M68K_MENU_HILITE, slot, -1,
                            (int32_t)m->menu[slot].id);
            }
        }
        break;
    }
    case QRX_M68K_TRAP_SET_ITM_ICON:   /* A940: menu A0, item D0, icon D1 */
    case QRX_M68K_TRAP_SET_ITM_STYLE:  /* A942: menu A0, item D0, style D1 */
    case QRX_M68K_TRAP_SET_ITEM_MARK: { /* A944: menu A0, item D0, mark D1 */
        int slot = menu_slot(m, a0);
        unsigned item = d0 & 0xFFFFu;
        int32_t value = (int32_t)(int16_t)(d1 & 0xFFFFu);
        int op;

        if (slot < 0 || item == 0u || item > m->menu[slot].count) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        if (trap == QRX_M68K_TRAP_SET_ITM_ICON) {
            op = QRX_M68K_MENU_ITEM_ICON;
            m->menu[slot].icon[item - 1u] = (int16_t)value;
        } else if (trap == QRX_M68K_TRAP_SET_ITM_STYLE) {
            op = QRX_M68K_MENU_ITEM_STYLE;
            m->menu[slot].style[item - 1u] = (int16_t)value;
        } else {
            op = QRX_M68K_MENU_ITEM_MARK;
            m->menu[slot].mark[item - 1u] = (char)(value & 0xFF);
        }
        menu_notify_item(m, slot, op, item - 1u, value);
        break;
    }
    case QRX_M68K_TRAP_GET_ITEM_CMD: { /* A84E: menu A0, item D0, byte* A1 */
        int slot = menu_slot(m, a0);
        unsigned item = d0 & 0xFFFFu;

        if (slot < 0 || item == 0u || item > m->menu[slot].count ||
            !mem_span(m, a1, 1u)) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        m->mem[a1] = (uint8_t)m->menu[slot].cmd[item - 1u];
        break;
    }
    case QRX_M68K_TRAP_SET_ITEM_CMD: { /* A84F: menu A0, item D0, char D1 */
        int slot = menu_slot(m, a0);
        unsigned item = d0 & 0xFFFFu;
        char ch = (char)(d1 & 0xFFu);

        if (slot < 0 || item == 0u || item > m->menu[slot].count) {
            out = QRX_M68K_TRAP_PARAM_ERR;
            break;
        }
        m->menu[slot].cmd[item - 1u] = ch;   /* stored as given, like MacOS */
        menu_notify_item(m, slot, QRX_M68K_MENU_ITEM_CMD, item - 1u,
                         (int32_t)ch);
        break;
    }
    case QRX_M68K_TRAP_MENU_KEY: {    /* A93E: character in D0 */
        /* Like MacOS: upper case only, first enabled item in bar order wins. */
        char ch = (char)(d0 & 0xFFFFu);
        unsigned i, j;

        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 'a' + 'A');

        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 'a' + 'A');
        out = 0u;
        for (i = 0u; i < m->nmbar && ch != '\0'; i++) {
            int slot = menu_slot(m, m->mbar[i]);

            if (slot < 0)
                continue;
            for (j = 0u; j < m->menu[slot].count; j++) {
                char key = m->menu[slot].cmd[j];

                if (key >= 'a' && key <= 'z')
                    key = (char)(key - 'a' + 'A');
                if (!m->menu[slot].enabled[j] || key != ch)
                    continue;
                out = ((uint32_t)(uint16_t)m->menu[slot].id << 16) | (j + 1u);
                m->menu_hilite = m->menu[slot].id;
                menu_notify(m, QRX_M68K_MENU_HILITE, slot, -1,
                            (int32_t)m->menu[slot].id);
                j = m->menu[slot].count;   /* stop, out is set */
                i = m->nmbar;
                break;
            }
        }
        break;
    }
    case QRX_M68K_TRAP_NEW_CONTROL: { /* A954: owner A0, rect A1, title A2 */
        char title[QRX_M68K_MENU_TEXT];
        uint32_t len = 0u;
        int slot;

        title[0] = '\0';
        if (mem_span(m, a2, 1u)) {
            len = m->mem[a2];
            if (len > QRX_M68K_MENU_TEXT - 1u)
                len = QRX_M68K_MENU_TEXT - 1u;
            if (mem_span(m, a2 + 1u, len))
                memcpy(title, m->mem + a2 + 1u, len);
        }
        slot = qd_read_rect(m, a1, &top, &left, &bottom, &right)
                   ? ctrl_new(m, a0, top, left, bottom, right, d0 != 0u,
                              (int16_t)d1, (int16_t)d2, (int16_t)d3, title, len,
                              0)
                   : -1;
        out = slot < 0 ? 0u : m->ctrl[slot].handle;
        break;
    }
    case QRX_M68K_TRAP_DISPOSE_CONTROL: { /* A955 */
        int slot = ctrl_slot(m, a0);

        if (slot >= 0) {
            hle_free_ptr(m, m->ctrl[slot].handle, 8u);
            m->ctrl[slot].used = 0;
        }
        break;
    }
    case QRX_M68K_TRAP_KILL_CONTROLS: {   /* A956: owner in A0 */
        unsigned i;

        for (i = 0u; i < QRX_M68K_CONTROL_MAX; i++)
            if (m->ctrl[i].used && m->ctrl[i].owner == a0) {
                hle_free_ptr(m, m->ctrl[i].handle, 8u);
                m->ctrl[i].used = 0;
            }
        break;
    }
    case QRX_M68K_TRAP_SHOW_CONTROL:     /* A957 */
    case QRX_M68K_TRAP_HIDE_CONTROL: {   /* A958 */
        int slot = ctrl_slot(m, a0);

        if (slot >= 0)
            m->ctrl[slot].visible = trap == QRX_M68K_TRAP_SHOW_CONTROL;
        break;
    }
    case QRX_M68K_TRAP_MOVE_CONTROL: {   /* A959: control A0, h D0, v D1 */
        int slot = ctrl_slot(m, a0);
        int32_t dh, dv;

        if (slot < 0)
            break;
        dh = (int32_t)(int16_t)(d0 & 0xFFFFu);
        dv = (int32_t)(int16_t)(d1 & 0xFFFFu);
        m->ctrl[slot].left += dh;
        m->ctrl[slot].right += dh;
        m->ctrl[slot].top += dv;
        m->ctrl[slot].bottom += dv;
        break;
    }
    case QRX_M68K_TRAP_SIZE_CONTROL: {   /* A95C: control A0, w D0, h D1 */
        int slot = ctrl_slot(m, a0);

        if (slot < 0)
            break;
        m->ctrl[slot].right =
            m->ctrl[slot].left + (int32_t)(int16_t)(d0 & 0xFFFFu);
        m->ctrl[slot].bottom =
            m->ctrl[slot].top + (int32_t)(int16_t)(d1 & 0xFFFFu);
        break;
    }
    case QRX_M68K_TRAP_HILITE_CONTROL: { /* A95D: control A0, state D0 */
        int slot = ctrl_slot(m, a0);

        if (slot >= 0)
            m->ctrl[slot].hilite = (int16_t)(d0 & 0xFFFFu);
        break;
    }
    case QRX_M68K_TRAP_GET_CONTROL_VALUE:   /* A960 */
    case QRX_M68K_TRAP_GET_CONTROL_MIN:     /* A961 */
    case QRX_M68K_TRAP_GET_CONTROL_MAX: {   /* A962 */
        int slot = ctrl_slot(m, a0);

        if (slot >= 0) {
            if (trap == QRX_M68K_TRAP_GET_CONTROL_VALUE)
                out = (uint16_t)m->ctrl[slot].value;
            else if (trap == QRX_M68K_TRAP_GET_CONTROL_MIN)
                out = (uint16_t)m->ctrl[slot].min;
            else
                out = (uint16_t)m->ctrl[slot].max;
        }
        break;
    }
    case QRX_M68K_TRAP_SET_CONTROL_VALUE: { /* A963: control A0, value D0 */
        int slot = ctrl_slot(m, a0);

        if (slot >= 0)
            m->ctrl[slot].value = (int16_t)(d0 & 0xFFFFu);
        break;
    }
    case QRX_M68K_TRAP_GET_CONTROL_TITLE: { /* A95E: control A0, buffer A1 */
        int slot = ctrl_slot(m, a0);
        uint32_t len;

        if (slot < 0 || !mem_span(m, a1, 1u))
            break;
        len = (uint32_t)strlen(m->ctrl[slot].title);
        if (!mem_span(m, a1 + 1u, len))
            break;
        m->mem[a1] = (uint8_t)len;
        memcpy(m->mem + a1 + 1u, m->ctrl[slot].title, len);
        break;
    }
    case QRX_M68K_TRAP_SET_CONTROL_TITLE: { /* A95F: control A0, Str255 A1 */
        int slot = ctrl_slot(m, a0);
        uint32_t len;

        if (slot < 0 || !mem_span(m, a1, 1u))
            break;
        len = m->mem[a1];
        if (len > QRX_M68K_MENU_TEXT - 1u)
            len = QRX_M68K_MENU_TEXT - 1u;
        if (!mem_span(m, a1 + 1u, len))
            break;
        memcpy(m->ctrl[slot].title, m->mem + a1 + 1u, len);
        m->ctrl[slot].title[len] = '\0';
        break;
    }
    case QRX_M68K_TRAP_DRAW_CONTROLS: {  /* A969: owner in A0 */
        unsigned i;

        for (i = 0u; i < QRX_M68K_CONTROL_MAX; i++)
            if (m->ctrl[i].used && m->ctrl[i].owner == a0)
                ctrl_draw(m, (int)i);
        break;
    }
    case QRX_M68K_TRAP_DRAW_1_CONTROL:   /* A96D: control in A0 */
        ctrl_draw(m, ctrl_slot(m, a0));
        break;
    case QRX_M68K_TRAP_FIND_CONTROL: {   /* A96C: point A0, owner A1, out A2 */
        unsigned i;
        int32_t py, px;

        out = 0u;
        if (!mem_span(m, a0, 4u))
            break;
        py = (int32_t)(int16_t)rd16(m->mem, m->mem_size, a0);
        px = (int32_t)(int16_t)rd16(m->mem, m->mem_size, a0 + 2u);
        for (i = 0u; i < QRX_M68K_CONTROL_MAX; i++) {
            if (!m->ctrl[i].used || m->ctrl[i].owner != a1)
                continue;
            if (!ctrl_hit(m, (int)i, py, px))
                continue;
            if (mem_span(m, a2, 4u))
                wr32(m->mem, m->mem_size, a2, m->ctrl[i].handle);
            out = QRX_M68K_IN_BUTTON;
            break;
        }
        break;
    }
    case QRX_M68K_TRAP_TRACK_CONTROL: {  /* A968: control A0, point A1 */
        int slot = ctrl_slot(m, a0);
        int32_t py, px;

        out = 0u;
        if (slot < 0 || !mem_span(m, a1, 4u))
            break;
        py = (int32_t)(int16_t)rd16(m->mem, m->mem_size, a1);
        px = (int32_t)(int16_t)rd16(m->mem, m->mem_size, a1 + 2u);
        if (ctrl_hit(m, slot, py, px)) {
            m->ctrl[slot].hilite = 1;
            out = QRX_M68K_IN_BUTTON;
        } else {
            m->ctrl[slot].hilite = 0;
        }
        break;
    }
    case QRX_M68K_TRAP_PARAM_TEXT: {  /* A98B: four Pascal strings */
        uint32_t addr[QRX_M68K_PARAM_TEXT];
        unsigned i;

        addr[0] = a0;
        addr[1] = a1;
        addr[2] = a2;
        addr[3] = (uint32_t)m68k_get_reg(NULL, M68K_REG_A3);
        for (i = 0u; i < QRX_M68K_PARAM_TEXT; i++) {
            uint32_t len;

            m->param[i][0] = '\0';
            if (!mem_span(m, addr[i], 1u))
                continue;
            len = m->mem[addr[i]];
            if (len > QRX_M68K_TEXT_MAX - 1u)
                len = QRX_M68K_TEXT_MAX - 1u;
            if (!mem_span(m, addr[i] + 1u, len))
                continue;
            memcpy(m->param[i], m->mem + addr[i] + 1u, len);
            m->param[i][len] = '\0';
        }
        break;
    }
    default:
        return 0;
    }
    m68k_set_reg(M68K_REG_D0, out);
    *result = out;
    return 1;
}

static void trace_trap(qrx_m68k_t *m, uint32_t trap, uint32_t pc, uint32_t d0,
                       uint32_t a0, uint32_t result, int implemented)
{
    qrx_m68k_trap_t *t;

    if (m->ntrace < QRX_M68K_TRACE_MAX) {
        t = &m->trace[m->ntrace++];
    } else {
        memmove(&m->trace[0], &m->trace[1],
                sizeof(m->trace[0]) * (QRX_M68K_TRACE_MAX - 1u));
        t = &m->trace[QRX_M68K_TRACE_MAX - 1u];
    }
    t->trap = trap;
    t->pc = pc;
    t->d0 = d0;
    t->a0 = a0;
    t->result = result;
    t->implemented = (uint8_t)(implemented ? 1 : 0);
    if (implemented)
        m->nimplemented++;
}

/* Answer one A-line trap.  Returns 1 when the trap was handled (execution
 * continues), 0 when it is not implemented (the run stops). */
static int dispatch(qrx_m68k_t *m, uint32_t trap, uint32_t pc)
{
    uint32_t d0 = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t result = d0;

    switch (trap) {
    case QRX_M68K_TRAP_LOAD_SEG: {   /* classic near-model segment call */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
        uint32_t seg = rd16(m->mem, m->mem_size, sp);
        uint32_t entry = pc >= 6u ? pc - 6u : 0u;
        uint32_t off, base = 0u, target;
        int patched;

        /* The caller reached us through the last six bytes of a jump table
         * entry, so the entry starts six bytes before the trap word. */
        if (!m->has_jt || entry < m->jt || entry >= m->jt + m->jt_size ||
            ((entry - m->jt) & 7u) != 0u ||
            rd16(m->mem, m->mem_size, entry + 2u) != 0x3F3Cu ||
            rd16(m->mem, m->mem_size, entry + 4u) != seg ||
            rd16(m->mem, m->mem_size, entry + 6u) != (0xA000u | trap)) {
            trace_trap(m, trap, pc, d0, a0, 0u, 0);
            m->stop_trap = trap;
            strcpy(m->detail, m->has_jt
                   ? "segment loader: _LoadSeg was not called through its "
                     "jump table entry"
                   : "segment loader: no near-model 'CODE' 0 jump table");
            g_stop = QRX_M68K_ERR_RUN;
            return 0;
        }
        /* Read the routine offset before patching: the patch replaces the
         * entry's first word with the segment number. */
        off = rd16(m->mem, m->mem_size, entry);
        patched = patch_jump_table(m, seg);
        if (patched <= 0) {
            if (patched == 0)
                snprintf(m->detail, sizeof m->detail,
                         "segment loader: unknown CODE %lu",
                         (unsigned long)seg);
            trace_trap(m, trap, pc, d0, a0, 0u, 0);
            m->stop_trap = trap;
            g_stop = QRX_M68K_ERR_RUN;
            return 0;
        }
        find_segment(m, seg, &base, 0);
        target = base + 4u + off;
        /* MOVE.W #segment,-(SP) pushed one word; the routine returns to the
         * caller's return address. */
        m68k_set_reg(M68K_REG_A7, sp + 2u);
        m68k_set_reg(M68K_REG_PC, entry + 2u);
        trace_trap(m, trap, pc, d0, a0, target, 1);
        return 1;
    }
    case QRX_M68K_TRAP_GESTALT:      /* 0xA1AD */
        result = hle_gestalt(m, d0, a0);
        m68k_set_reg(M68K_REG_D0, result);
        break;
    case QRX_M68K_TRAP_GET_ZONE:     /* 0xA11A */
        a0 = m->heap;
        m68k_set_reg(M68K_REG_A0, a0);
        result = 0u;
        break;
    case QRX_M68K_TRAP_SET_ZONE:     /* 0xA11B */
        result = 0u;
        m68k_set_reg(M68K_REG_D0, 0u);
        break;
    case QRX_M68K_TRAP_NEW_PTR:      /* 0xA11E */
        a0 = hle_new_ptr(m, d0);
        m68k_set_reg(M68K_REG_A0, a0);
        result = a0;
        break;
    case QRX_M68K_TRAP_DISPOSE_PTR:  /* 0xA11F: the bump heap never frees */
        result = 0u;
        break;
    case QRX_M68K_TRAP_SYS_ENVIRONS: /* 0xA090 */
        result = hle_sys_environs(m, d0, a0);
        m68k_set_reg(M68K_REG_D0, result);
        break;
    case QRX_M68K_TRAP_NEW_HANDLE: {  /* 0xA122: size is a long on the stack */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
        uint32_t size = rd32(m->mem, m->mem_size, sp);

        a0 = hle_new_handle(m, size);
        m68k_set_reg(M68K_REG_A7, sp + 4u);
        m68k_set_reg(M68K_REG_A0, a0);
        result = a0;
        break;
    }
    case QRX_M68K_TRAP_DISPOSE_HANDLE: { /* 0xA023 */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);

        hle_dispose_handle(m, rd32(m->mem, m->mem_size, sp));
        m68k_set_reg(M68K_REG_A7, sp + 4u);
        result = 0u;
        break;
    }
    case QRX_M68K_TRAP_GET_HANDLE_SIZE: { /* 0xA025 */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
        int slot = handle_slot(m, rd32(m->mem, m->mem_size, sp));

        m68k_set_reg(M68K_REG_A7, sp + 4u);
        result = slot < 0 ? 0u : m->handle[slot].size;
        m68k_set_reg(M68K_REG_D0, result);
        break;
    }
    case QRX_M68K_TRAP_SET_HANDLE_SIZE: { /* 0xA024: handle, then size */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
        uint32_t h = rd32(m->mem, m->mem_size, sp);
        uint32_t size = rd32(m->mem, m->mem_size, sp + 4u);

        a0 = hle_set_handle_size(m, h, size);
        m68k_set_reg(M68K_REG_A7, sp + 8u);
        m68k_set_reg(M68K_REG_A0, a0);
        result = a0;
        break;
    }
    case QRX_M68K_TRAP_HLOCK:      /* 0xA029: nothing here can move */
    case QRX_M68K_TRAP_HUNLOCK: {  /* 0xA02A */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);

        a0 = rd32(m->mem, m->mem_size, sp);
        m68k_set_reg(M68K_REG_A7, sp + 4u);
        m68k_set_reg(M68K_REG_A0, a0);
        result = a0;
        break;
    }
    case QRX_M68K_TRAP_OPEN_RES_FILE:  /* 0xA997: A0 = Pascal path */
        result = hle_open_res_file(m, a0);
        m68k_set_reg(M68K_REG_D0, result);
        break;
    case QRX_M68K_TRAP_CUR_RES_FILE:   /* 0xA994 */
        result = m->resfile;
        m68k_set_reg(M68K_REG_D0, result);
        break;
    case QRX_M68K_TRAP_USE_RES_FILE:   /* 0xA998: refNum in D0 */
        if (d0 == 0xFFFFFFFFu) {
            m->resfile = d0;
            result = 0u;
        } else {
            result = QRX_M68K_ERR_RES_F_NOT_FOUND;
        }
        m68k_set_reg(M68K_REG_D0, result);
        break;
    case QRX_M68K_TRAP_GET_RESOURCE: { /* 0xA9A0: type in D0, id on the stack */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
        int32_t id = (int32_t)(int16_t)rd16(m->mem, m->mem_size, sp);

        a0 = hle_get_resource(m, d0, id);
        m68k_set_reg(M68K_REG_A7, sp + 2u);
        m68k_set_reg(M68K_REG_A0, a0);
        result = a0;
        break;
    }
    case QRX_M68K_TRAP_RELEASE_RESOURCE: { /* 0xA9A3: handle on the stack */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);

        m68k_set_reg(M68K_REG_A7, sp + 4u);   /* the handle stays cached */
        result = 0u;
        break;
    }
    case QRX_M68K_TRAP_OPEN:      /* 0xA000 */
    case QRX_M68K_TRAP_CLOSE:     /* 0xA001 */
    case QRX_M68K_TRAP_READ:      /* 0xA002 */
    case QRX_M68K_TRAP_WRITE:     /* 0xA003 */
    case QRX_M68K_TRAP_CREATE:    /* 0xA008 */
    case QRX_M68K_TRAP_GET_FPOS:  /* 0xA010 */
    case QRX_M68K_TRAP_SET_FPOS:  /* 0xA011 */
    case QRX_M68K_TRAP_GET_EOF: { /* 0xA016: param block on the stack */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
        uint32_t pb = rd32(m->mem, m->mem_size, sp);
        int32_t err = hle_file_trap(m, trap, pb);

        m68k_set_reg(M68K_REG_A7, sp + 4u);
        if (pb != 0u && pb + 2u <= m->mem_size)
            wr16(m->mem, m->mem_size, pb + QRX_M68K_PB_RESULT, (uint16_t)err);
        result = (uint32_t)err;
        break;
    }
    case QRX_M68K_TRAP_EXIT_TO_SHELL: /* 0xA9F4 */
        trace_trap(m, trap, pc, d0, a0, 0u, 1);
        strcpy(m->detail, "ExitToShell");
        g_stop = QRX_M68K_EXITED;
        return 0;
    default:
        if (!hle_toolbox(m, trap, &result)) {
            trace_trap(m, trap, pc, d0, a0, 0u, 0);
            m->stop_trap = trap;
            strcpy(m->detail, "missing trap");
            g_stop = QRX_M68K_UNIMPLEMENTED;
            return 0;
        }
        break;
    }
    trace_trap(m, trap, pc, d0, a0, result, 1);
    m68k_set_reg(M68K_REG_PC, pc + 2u);
    return 1;
}

/* Musashi reports every A-line and every ILLEGAL here. */
static int trap_callback(int opcode)
{
    qrx_m68k_t *m = g_m;
    uint32_t word = (uint32_t)opcode & 0xFFFFu;
    uint32_t pc, trap;

    if (!m)
        return 0;
    if (word == 0x4AFCu) {           /* the exit stub: main returned */
        g_stop = QRX_M68K_EXITED;
        if (m->detail[0] == '\0')
            strcpy(m->detail, "main returned");
        m68k_end_timeslice();
        return 1;
    }
    if ((word & 0xF000u) != 0xA000u)
        return 0;
    pc = m68k_get_reg(NULL, M68K_REG_PC);
    if (!(rd16(m->mem, m->mem_size, pc) == word && (pc & 1u) == 0u)) {
        if (rd16(m->mem, m->mem_size, pc - 2u) == word)
            pc -= 2u;
    }
    trap = word & 0x0FFFu;
    if (!dispatch(m, trap, pc)) {
        m68k_end_timeslice();
        return 1;
    }
    return 1;
}

/* ------------------------------------------------------------------- running */

qrx_m68k_status_t qrx_m68k_start(qrx_m68k_t *m, uint32_t budget)
{
    uint32_t left;

    if (!m || !m->mem)
        return QRX_M68K_ERR_ARG;
    g_m = m;
    g_stop = 0;
    m->ntrace = 0u;
    m->nimplemented = 0u;
    m->stop_trap = 0u;
    m->detail[0] = '\0';

    if (m->pen_size_w == 0u)
        m->pen_size_w = 1u;
    if (m->pen_size_h == 0u)
        m->pen_size_h = 1u;
    if (m->text_size == 0u)
        m->text_size = 12u;
    if (m->ticks == 0u)
        m->ticks = 1u;

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68020);
    m68k_set_illg_instr_callback(trap_callback);
    m68k_set_reg(M68K_REG_PC, m->entry);
    m68k_set_reg(M68K_REG_A7, m->sp);
    m68k_set_reg(M68K_REG_A5, m->a5);
    m68k_set_reg(M68K_REG_A6, m->a5);
    m68k_set_reg(M68K_REG_D0, 0u);
    /* The application's main returns into the ILLEGAL stub. */
    m->sp -= 4u;
    wr32(m->mem, m->mem_size, m->sp, QRX_M68K_EXIT_STUB);
    m68k_set_reg(M68K_REG_A7, m->sp);

    left = budget;
    while (g_stop == 0 && left > 0u) {
        uint32_t slice = left > 1000000u ? 1000000u : left;
        uint32_t used = (uint32_t)m68k_execute((int)slice);
        if (used == 0u)
            break;
        left = used >= slice ? left - slice : left - used;
    }
    if (g_stop == 0) {
        if (m68k_get_reg(NULL, M68K_REG_PC) == QRX_M68K_EXIT_STUB) {
            g_stop = QRX_M68K_EXITED;
            strcpy(m->detail, "main returned");
        } else {
            g_stop = QRX_M68K_LIMIT;
            strcpy(m->detail, "instruction budget exhausted");
        }
    }
    if (g_stop == QRX_M68K_UNIMPLEMENTED && m->detail[0] == '\0')
        strcpy(m->detail, "missing trap");
    g_m = 0;
    return (qrx_m68k_status_t)g_stop;
}

const char *qrx_m68k_status_name(qrx_m68k_status_t st)
{
    switch (st) {
    case QRX_M68K_OK: return "ok";
    case QRX_M68K_ERR_ARG: return "bad argument";
    case QRX_M68K_ERR_IMAGE: return "not a MacBinary image or resource fork";
    case QRX_M68K_ERR_NO_CODE: return "68k application: no 'CODE' resources";
    case QRX_M68K_ERR_MEM: return "segments do not fit guest memory";
    case QRX_M68K_ERR_PACKED: return "'CODE' resources are packed";
    case QRX_M68K_UNIMPLEMENTED: return "stopped at an unimplemented trap";
    case QRX_M68K_EXITED: return "exited";
    case QRX_M68K_LIMIT: return "instruction budget exhausted";
    case QRX_M68K_FAULT: return "CPU fault";
    case QRX_M68K_ERR_RUN: return "run failure";
    default: return "unknown";
    }
}
