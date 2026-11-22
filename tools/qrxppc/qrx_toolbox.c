/*
 * QRXPPC - Memory Manager HLE (P2.2, first slice).
 *
 * The heap lives in guest memory, so blocks are readable by PPC code with
 * ordinary loads and stores.  Allocation is first-fit over freed blocks with a
 * bump tail; the host-side table is the only bookkeeping, there is no in-place
 * free list.  Handles are real master pointers: a 4-byte cell whose first word
 * is the data address, the data itself with its size word at data - 4.
 *
 * Deliberate limits (each an explicit error, never a hang):
 *   - no zones: one heap for the whole application,
 *   - pointer blocks shrink only; growth is QRX_MM_ERR_SIZE,
 *   - freed space is reused first-fit, but blocks are not compacted,
 *   - the number of tracked blocks is capped by QRX_MM_MAX_BLOCKS.
 */
#include "qrx_toolbox.h"
#include "qrx_font8x8.h"

#include <stdlib.h>
#include <string.h>

/* Guest memory is big-endian; words the guest reads must be written so. */
static uint32_t mm_get32(const qrx_mm_t *mm, uint32_t addr)
{
    const uint8_t *d = mm->ppc->mem.data;

    return (((uint32_t)d[addr] << 24) | ((uint32_t)d[addr + 1u] << 16) |
            ((uint32_t)d[addr + 2u] << 8) | (uint32_t)d[addr + 3u]);
}

static void mm_put32(qrx_mm_t *mm, uint32_t addr, uint32_t value)
{
    uint8_t *d = mm->ppc->mem.data;

    d[addr] = (uint8_t)(value >> 24);
    d[addr + 1u] = (uint8_t)(value >> 16);
    d[addr + 2u] = (uint8_t)(value >> 8);
    d[addr + 3u] = (uint8_t)value;
}

static void mm_clear(qrx_mm_t *mm, uint32_t addr, uint32_t n)
{
    memset(&mm->ppc->mem.data[addr], 0, n);
}

static void mm_copy(qrx_mm_t *mm, uint32_t dst, uint32_t src, uint32_t n)
{
    memcpy(&mm->ppc->mem.data[dst], &mm->ppc->mem.data[src], n);
}

static qrx_mm_status_t mm_fail(qrx_mm_t *mm, qrx_mm_status_t err)
{
    mm->last_err = (uint32_t)err;
    return err;
}

qrx_mm_status_t qrx_mm_init(qrx_mm_t *mm, qrx_ppc_t *ppc, uint32_t base,
                            uint32_t size)
{
    if (!mm || !ppc || !ppc->mem.data || base == 0u || size == 0u ||
        (base & (QRX_MM_ALIGN - 1u)) != 0u)
        return QRX_MM_ERR_ARG;
    if ((uint64_t)base + (uint64_t)size > (uint64_t)ppc->mem.size)
        return QRX_MM_ERR_ARG;
    memset(mm, 0, sizeof(*mm));
    mm->ppc = ppc;
    mm->base = base;
    mm->size = size;
    mm->bump = base;
    return QRX_MM_OK;
}

/* Physical size of a block with the given logical size, header included. */
static uint32_t mm_total(uint32_t bytes)
{
    return (bytes + 4u + (QRX_MM_ALIGN - 1u)) & ~(QRX_MM_ALIGN - 1u);
}

static int mm_find(const qrx_mm_t *mm, int kind, uint32_t data)
{
    unsigned i;

    for (i = 0u; i < mm->nblocks; i++) {
        if (mm->blocks[i].kind == kind && mm->blocks[i].base + 4u == data)
            return (int)i;
    }
    return -1;
}

static qrx_mm_status_t mm_alloc(qrx_mm_t *mm, uint32_t bytes, int kind,
                                uint32_t *out_data, int *out_index)
{
    qrx_mm_block_t *e;
    uint32_t total;
    unsigned i;

    if (bytes > mm->size)                /* smaller than the whole zone */
        return mm_fail(mm, QRX_MM_ERR_FULL);
    total = mm_total(bytes);
    e = 0;
    /* First fit over freed blocks keeps dispose/realloc churn small. */
    for (i = 0u; i < mm->nblocks; i++) {
        if (mm->blocks[i].kind == 0 && total <= mm->blocks[i].total) {
            e = &mm->blocks[i];
            break;
        }
    }
    if (!e) {
        if (mm->nblocks >= QRX_MM_MAX_BLOCKS)
            return mm_fail(mm, QRX_MM_ERR_FULL);
        if (total > mm->base + mm->size - mm->bump)
            return mm_fail(mm, QRX_MM_ERR_FULL);
        e = &mm->blocks[mm->nblocks++];
        e->base = mm->bump;
        e->total = mm->base + mm->size - mm->bump;
        mm->bump += total;
    }
    e->total = total;
    e->size = bytes;
    e->kind = kind;
    e->cell = 0u;
    mm_put32(mm, e->base, bytes);          /* the size word at data - 4 */
    *out_data = e->base + 4u;
    if (out_index)
        *out_index = (int)(e - mm->blocks);
    return QRX_MM_OK;
}

static void mm_release(qrx_mm_t *mm, int index)
{
    mm->blocks[index].kind = 0;            /* space stays in the free list */
}

qrx_mm_status_t qrx_mm_new_ptr(qrx_mm_t *mm, uint32_t bytes, int clear,
                               uint32_t *out)
{
    qrx_mm_status_t st;

    if (!mm || !out)
        return QRX_MM_ERR_ARG;
    *out = 0u;
    st = mm_alloc(mm, bytes, 1, out, 0);
    if (st == QRX_MM_OK && clear)
        mm_clear(mm, *out, bytes);
    return st;
}

qrx_mm_status_t qrx_mm_dispose_ptr(qrx_mm_t *mm, uint32_t ptr)
{
    int i;

    if (!mm || ptr == 0u)
        return QRX_MM_ERR_ARG;
    i = mm_find(mm, 1, ptr);
    if (i < 0)
        return mm_fail(mm, QRX_MM_ERR_INVALID);
    mm_release(mm, i);
    return QRX_MM_OK;
}

qrx_mm_status_t qrx_mm_new_handle(qrx_mm_t *mm, uint32_t bytes, int clear,
                                  uint32_t *out)
{
    uint32_t data;
    uint32_t cell;
    int data_index;
    qrx_mm_status_t st;

    if (!mm || !out)
        return QRX_MM_ERR_ARG;
    *out = 0u;
    st = mm_alloc(mm, bytes, 2, &data, &data_index);
    if (st != QRX_MM_OK)
        return st;
    st = mm_alloc(mm, 4u, 3, &cell, 0);    /* the master pointer cell */
    if (st != QRX_MM_OK) {
        mm_release(mm, data_index);         /* roll the data block back */
        return st;
    }
    mm_put32(mm, cell, data);               /* *handle == data address */
    mm->blocks[data_index].cell = cell;
    if (clear)
        mm_clear(mm, data, bytes);
    *out = cell;
    return QRX_MM_OK;
}

static qrx_mm_status_t mm_handle_data(const qrx_mm_t *mm, uint32_t handle,
                                      int *cell_index, uint32_t *data)
{
    int c;
    int d;

    c = mm_find(mm, 3, handle);
    if (c < 0)
        return QRX_MM_ERR_INVALID;
    d = mm_find(mm, 2, mm_get32(mm, handle));
    if (d < 0)
        return QRX_MM_ERR_INVALID;
    if (cell_index)
        *cell_index = c;
    if (data)
        *data = mm->blocks[d].base + 4u;
    return QRX_MM_OK;
}

qrx_mm_status_t qrx_mm_dispose_handle(qrx_mm_t *mm, uint32_t handle)
{
    uint32_t data;
    int c;
    int d;
    qrx_mm_status_t st;

    if (!mm || handle == 0u)
        return QRX_MM_ERR_ARG;
    st = mm_handle_data(mm, handle, &c, &data);
    if (st != QRX_MM_OK)
        return mm_fail(mm, st);
    d = mm_find(mm, 2, data);
    mm_release(mm, d);
    mm_release(mm, c);
    return QRX_MM_OK;
}

qrx_mm_status_t qrx_mm_set_handle_size(qrx_mm_t *mm, uint32_t handle,
                                       uint32_t bytes)
{
    uint32_t data;
    uint32_t fresh;
    int data_index;
    int fresh_index;
    qrx_mm_status_t st;

    if (!mm || handle == 0u)
        return QRX_MM_ERR_ARG;
    st = mm_handle_data(mm, handle, 0, &data);
    if (st != QRX_MM_OK)
        return mm_fail(mm, st);
    data_index = mm_find(mm, 2, data);
    if (bytes <= mm->blocks[data_index].size) {   /* shrink in place */
        mm->blocks[data_index].size = bytes;
        mm_put32(mm, data - 4u, bytes);
        return QRX_MM_OK;
    }
    /* Grow by creation: the old block stays valid until the new one exists,
     * so a failure leaves the handle exactly as it was. */
    st = mm_alloc(mm, bytes, 2, &fresh, &fresh_index);
    if (st != QRX_MM_OK)
        return st;
    mm_copy(mm, fresh, data, mm->blocks[data_index].size);
    mm_put32(mm, handle, fresh);                  /* master pointer moves */
    mm->blocks[fresh_index].cell = handle;
    mm_release(mm, data_index);
    return QRX_MM_OK;
}

qrx_mm_status_t qrx_mm_set_ptr_size(qrx_mm_t *mm, uint32_t ptr, uint32_t bytes)
{
    int i;

    if (!mm || ptr == 0u)
        return QRX_MM_ERR_ARG;
    i = mm_find(mm, 1, ptr);
    if (i < 0)
        return mm_fail(mm, QRX_MM_ERR_INVALID);
    if (bytes > mm->blocks[i].size)               /* pointers cannot move */
        return mm_fail(mm, QRX_MM_ERR_SIZE);
    mm->blocks[i].size = bytes;
    mm_put32(mm, ptr - 4u, bytes);
    return QRX_MM_OK;
}

qrx_mm_status_t qrx_mm_get_ptr_size(qrx_mm_t *mm, uint32_t ptr,
                                    uint32_t *size)
{
    int i;

    if (!mm || !size)
        return QRX_MM_ERR_ARG;
    i = mm_find(mm, 1, ptr);
    if (i < 0)
        return mm_fail(mm, QRX_MM_ERR_INVALID);
    *size = mm->blocks[i].size;
    return QRX_MM_OK;
}

qrx_mm_status_t qrx_mm_get_handle_size(qrx_mm_t *mm, uint32_t handle,
                                       uint32_t *size)
{
    uint32_t data;
    qrx_mm_status_t st;

    if (!mm || !size)
        return QRX_MM_ERR_ARG;
    st = mm_handle_data(mm, handle, 0, &data);
    if (st != QRX_MM_OK)
        return mm_fail(mm, st);
    *size = mm->blocks[mm_find(mm, 2, data)].size;
    return QRX_MM_OK;
}

uint32_t qrx_mm_free_bytes(qrx_mm_t *mm)
{
    uint32_t sum = 0u;
    unsigned i;

    if (!mm || !mm->ppc)
        return 0u;
    for (i = 0u; i < mm->nblocks; i++) {
        if (mm->blocks[i].kind == 0)
            sum += mm->blocks[i].total;
    }
    return sum + (mm->base + mm->size - mm->bump);
}

uint32_t qrx_mm_max_block(qrx_mm_t *mm)
{
    uint32_t best = 0u;
    unsigned i;

    if (!mm || !mm->ppc)
        return 0u;
    for (i = 0u; i < mm->nblocks; i++) {
        if (mm->blocks[i].kind == 0 && mm->blocks[i].total > best)
            best = mm->blocks[i].total;
    }
    if (mm->base + mm->size - mm->bump > best)
        best = mm->base + mm->size - mm->bump;
    return best;
}

uint32_t qrx_mm_dispatch(void *ctx, const qrx_mix_args_t *args,
                         qrx_mix_result_t *extra)
{
    qrx_mm_t *mm = (qrx_mm_t *)ctx;
    uint32_t selector;
    uint32_t a;
    uint32_t b;
    uint32_t size;

    (void)extra;
    if (!mm || !args)
        return QRX_MM_UNKNOWN;
    selector = args->gpr[0];
    a = args->gpr[1];
    b = args->gpr[2];
    switch (selector) {
    case QRX_TB_NEW_PTR: {
        uint32_t ptr = 0u;

        return qrx_mm_new_ptr(mm, a, 0, &ptr) == QRX_MM_OK ? ptr : 0u;
    }
    case QRX_TB_DISPOSE_PTR:
        return (uint32_t)qrx_mm_dispose_ptr(mm, a);
    case QRX_TB_SET_PTR_SIZE:
        return (uint32_t)qrx_mm_set_ptr_size(mm, a, b);
    case QRX_TB_GET_PTR_SIZE:
        return qrx_mm_get_ptr_size(mm, a, &size) == QRX_MM_OK
                   ? size : QRX_MM_UNKNOWN;
    case QRX_TB_NEW_HANDLE: {
        uint32_t handle = 0u;

        return qrx_mm_new_handle(mm, a, 0, &handle) == QRX_MM_OK ? handle : 0u;
    }
    case QRX_TB_DISPOSE_HANDLE:
        return (uint32_t)qrx_mm_dispose_handle(mm, a);
    case QRX_TB_SET_HANDLE_SIZE:
        return (uint32_t)qrx_mm_set_handle_size(mm, a, b);
    case QRX_TB_GET_HANDLE_SIZE:
        return qrx_mm_get_handle_size(mm, a, &size) == QRX_MM_OK
                   ? size : QRX_MM_UNKNOWN;
    case QRX_TB_FREE_MEM:
        return qrx_mm_free_bytes(mm);
    case QRX_TB_MAX_MEM:
        return qrx_mm_max_block(mm);
    default:
        return QRX_MM_UNKNOWN;
    }
}

/* ---- Resource Manager ------------------------------------------------ */
/* The table is searched in insertion order, like a resource map that was
 * loaded in file order; the first match wins, so duplicate (type, id)
 * pairs resolve to the earliest added resource (documented HLE choice). */
static qrx_rm_entry_t *rm_find(const qrx_rm_t *rm, uint32_t type, int16_t id)
{
    unsigned i;

    for (i = 0u; i < QRX_RM_MAX_ENTRIES; i++) {
        const qrx_rm_entry_t *e = &rm->entries[i];

        if (e->used && e->type == type && e->id == id)
            return (qrx_rm_entry_t *)e;
    }
    return 0;
}

static qrx_rm_entry_t *rm_find_handle(const qrx_rm_t *rm, uint32_t handle)
{
    unsigned i;

    for (i = 0u; i < QRX_RM_MAX_ENTRIES; i++) {
        const qrx_rm_entry_t *e = &rm->entries[i];

        if (e->used && e->handle == handle)
            return (qrx_rm_entry_t *)e;
    }
    return 0;
}

static int32_t rm_done(qrx_rm_t *rm, int32_t err)
{
    rm->last_err = err;
    return err;
}

void qrx_rm_init(qrx_rm_t *rm, qrx_mm_t *mm)
{
    if (rm)
        memset(rm, 0, sizeof(*rm));
    if (rm && mm)
        rm->mm = mm;
}

int32_t qrx_rm_add_resource(qrx_rm_t *rm, uint32_t type, int16_t id,
                            const char *name, uint32_t handle)
{
    qrx_rm_entry_t *e;
    unsigned i;
    size_t k;

    if (!rm || !rm->mm || handle == 0u)
        return QRX_TB_PARAM_ERR;
    for (i = 0u; i < QRX_RM_MAX_ENTRIES; i++) {
        if (!rm->entries[i].used)
            break;
    }
    if (i >= QRX_RM_MAX_ENTRIES)
        return rm_done(rm, QRX_TB_PARAM_ERR);
    e = &rm->entries[i];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->id = id;
    e->handle = handle;
    e->used = 1u;
    e->loaded = 1u;
    if (name) {
        for (k = 0u; k < sizeof(e->name) - 1u && name[k] != '\0'; k++)
            e->name[k] = name[k];
        e->name[k] = '\0';
    }
    return rm_done(rm, QRX_TB_NO_ERR);
}

uint32_t qrx_rm_get_resource(qrx_rm_t *rm, uint32_t type, int16_t id)
{
    qrx_rm_entry_t *e;

    if (!rm)
        return 0u;
    e = rm_find(rm, type, id);
    if (!e) {
        rm->last_err = QRX_TB_RES_NOT_FOUND;
        return 0u;
    }
    rm->last_err = QRX_TB_NO_ERR;
    return e->handle;
}

uint32_t qrx_rm_get_named_resource(qrx_rm_t *rm, uint32_t type,
                                   const char *name)
{
    unsigned i;

    if (!rm || !name)
        return 0u;
    for (i = 0u; i < QRX_RM_MAX_ENTRIES; i++) {
        const qrx_rm_entry_t *e = &rm->entries[i];

        if (e->used && e->type == type && e->name[0] != '\0' &&
            strcmp(e->name, name) == 0) {
            rm->last_err = QRX_TB_NO_ERR;
            return e->handle;
        }
    }
    rm->last_err = QRX_TB_RES_NOT_FOUND;
    return 0u;
}

static int32_t rm_require(const qrx_rm_t *rm, uint32_t handle,
                          qrx_rm_entry_t **out)
{
    qrx_rm_entry_t *e;

    if (!rm || handle == 0u)
        return QRX_TB_PARAM_ERR;
    e = rm_find_handle(rm, handle);
    if (!e)
        return QRX_TB_RES_NOT_FOUND;
    if (out)
        *out = e;
    return QRX_TB_NO_ERR;
}

int32_t qrx_rm_load_resource(qrx_rm_t *rm, uint32_t handle)
{
    qrx_rm_entry_t *e;
    int32_t err;

    err = rm_require(rm, handle, &e);
    if (err != QRX_TB_NO_ERR)
        return rm_done(rm, err);
    e->loaded = 1u;
    return rm_done(rm, QRX_TB_NO_ERR);
}

int32_t qrx_rm_release_resource(qrx_rm_t *rm, uint32_t handle)
{
    qrx_rm_entry_t *e;
    int32_t err;

    err = rm_require(rm, handle, &e);
    if (err != QRX_TB_NO_ERR)
        return rm_done(rm, err);
    e->loaded = 0u;
    return rm_done(rm, QRX_TB_NO_ERR);
}

int32_t qrx_rm_detach_resource(qrx_rm_t *rm, uint32_t handle)
{
    qrx_rm_entry_t *e;
    int32_t err;

    err = rm_require(rm, handle, &e);
    if (err != QRX_TB_NO_ERR)
        return rm_done(rm, err);
    e->used = 0u;   /* the handle keeps its data; the map entry is gone */
    return rm_done(rm, QRX_TB_NO_ERR);
}

int32_t qrx_rm_remove_resource(qrx_rm_t *rm, uint32_t handle)
{
    qrx_rm_entry_t *e;
    int32_t err;

    err = rm_require(rm, handle, &e);
    if (err != QRX_TB_NO_ERR)
        return rm_done(rm, err);
    e->used = 0u;
    return rm_done(rm, QRX_TB_NO_ERR);
}

int32_t qrx_rm_size_resource(qrx_rm_t *rm, uint32_t handle)
{
    qrx_rm_entry_t *e;
    uint32_t size;
    int32_t err;

    err = rm_require(rm, handle, &e);
    if (err != QRX_TB_NO_ERR)
        return rm_done(rm, err);
    if (qrx_mm_get_handle_size(rm->mm, handle, &size) != QRX_MM_OK)
        return rm_done(rm, QRX_TB_RES_NOT_FOUND);
    return rm_done(rm, (int32_t)size);
}

int32_t qrx_rm_get_attrs(qrx_rm_t *rm, uint32_t handle)
{
    qrx_rm_entry_t *e;
    int32_t err;

    err = rm_require(rm, handle, &e);
    if (err != QRX_TB_NO_ERR)
        return rm_done(rm, err);
    return rm_done(rm, (int32_t)e->attrs);
}

int32_t qrx_rm_set_attrs(qrx_rm_t *rm, uint32_t handle, int32_t attrs)
{
    qrx_rm_entry_t *e;
    int32_t err;

    err = rm_require(rm, handle, &e);
    if (err != QRX_TB_NO_ERR)
        return rm_done(rm, err);
    e->attrs = (uint8_t)attrs;
    return rm_done(rm, QRX_TB_NO_ERR);
}

int32_t qrx_rm_res_error(const qrx_rm_t *rm)
{
    if (!rm)
        return QRX_TB_PARAM_ERR;
    return rm->last_err;
}

uint32_t qrx_rm_count_resources(const qrx_rm_t *rm, uint32_t type)
{
    uint32_t n = 0u;
    unsigned i;

    if (!rm)
        return 0u;
    for (i = 0u; i < QRX_RM_MAX_ENTRIES; i++) {
        if (rm->entries[i].used && rm->entries[i].type == type)
            n++;
    }
    return n;
}

uint32_t qrx_rm_get_ind_resource(const qrx_rm_t *rm, uint32_t type,
                                 uint32_t index)
{
    unsigned i;

    if (!rm || index == 0u)
        return 0u;
    for (i = 0u; i < QRX_RM_MAX_ENTRIES; i++) {
        if (rm->entries[i].used && rm->entries[i].type == type) {
            if (--index == 0u)
                return rm->entries[i].handle;
        }
    }
    return 0u;
}

uint32_t qrx_rm_count_types(const qrx_rm_t *rm)
{
    uint32_t n = 0u;
    unsigned i;
    unsigned j;

    if (!rm)
        return 0u;
    for (i = 0u; i < QRX_RM_MAX_ENTRIES; i++) {
        int seen = 0;

        if (!rm->entries[i].used)
            continue;
        for (j = 0u; j < i; j++) {
            if (rm->entries[j].used &&
                rm->entries[j].type == rm->entries[i].type) {
                seen = 1;
                break;
            }
        }
        if (!seen)
            n++;
    }
    return n;
}

uint32_t qrx_rm_get_ind_type(const qrx_rm_t *rm, uint32_t index)
{
    uint32_t seen = 0u;
    unsigned i;
    unsigned j;

    if (!rm || index == 0u)
        return 0u;
    for (i = 0u; i < QRX_RM_MAX_ENTRIES; i++) {
        int dupe = 0;

        if (!rm->entries[i].used)
            continue;
        for (j = 0u; j < i; j++) {
            if (rm->entries[j].used &&
                rm->entries[j].type == rm->entries[i].type) {
                dupe = 1;
                break;
            }
        }
        if (dupe)
            continue;
        if (++seen == index)
            return rm->entries[i].type;
    }
    return 0u;
}

int32_t qrx_rm_set_resource_size(qrx_rm_t *rm, uint32_t handle,
                                 uint32_t size)
{
    qrx_mm_status_t st;

    if (!rm || !rm->mm)
        return QRX_TB_PARAM_ERR;
    (void)rm_require(rm, handle, 0);
    st = qrx_mm_set_handle_size(rm->mm, handle, size);
    if (st == QRX_MM_OK)
        return rm_done(rm, QRX_TB_NO_ERR);
    return rm_done(rm, st == QRX_MM_ERR_FULL ? -8 : QRX_TB_PARAM_ERR);
}

/* ---- File Manager ------------------------------------------------------ */
/* Guest-memory helpers shared with the dispatch below. */
static int16_t fm_get16(const qrx_fm_t *fm, uint32_t addr)
{
    const uint8_t *d = fm->ppc->mem.data;

    return (int16_t)(((uint16_t)d[addr] << 8) | (uint16_t)d[addr + 1u]);
}

static uint32_t fm_get32(const qrx_fm_t *fm, uint32_t addr)
{
    const uint8_t *d = fm->ppc->mem.data;

    return (((uint32_t)d[addr] << 24) | ((uint32_t)d[addr + 1u] << 16) |
            ((uint32_t)d[addr + 2u] << 8) | (uint32_t)d[addr + 3u]);
}

static void fm_put16(qrx_fm_t *fm, uint32_t addr, int16_t value)
{
    uint8_t *d = fm->ppc->mem.data;

    d[addr] = (uint8_t)((uint16_t)value >> 8);
    d[addr + 1u] = (uint8_t)value;
}

static void fm_put32(qrx_fm_t *fm, uint32_t addr, uint32_t value)
{
    uint8_t *d = fm->ppc->mem.data;

    d[addr] = (uint8_t)(value >> 24);
    d[addr + 1u] = (uint8_t)(value >> 16);
    d[addr + 2u] = (uint8_t)(value >> 8);
    d[addr + 3u] = (uint8_t)value;
}

/* Pascal string from guest memory into a C buffer.  Returns 1 on success. */
static int fm_read_name(const qrx_fm_t *fm, uint32_t addr, char *out,
                        size_t cap)
{
    const uint8_t *d = fm->ppc->mem.data;
    unsigned len;
    unsigned i;

    if (addr == 0u || addr >= fm->ppc->mem.size)
        return 0;
    len = d[addr];
    if (len == 0u || len > QRX_FM_MAX_NAME ||
        (size_t)(addr + 1u + len) > (size_t)fm->ppc->mem.size)
        return 0;
    if (cap < (size_t)len + 1u)
        return 0;
    for (i = 0u; i < len; i++)
        out[i] = (char)d[addr + 1u + i];
    out[len] = '\0';
    return 1;
}

static char fm_fold(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

static int fm_name_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (fm_fold(*a) != fm_fold(*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int fm_find(const qrx_fm_t *fm, const char *name)
{
    unsigned i;

    for (i = 0u; i < QRX_FM_MAX_FILES; i++) {
        if (fm->files[i].used && fm_name_eq(fm->files[i].name, name))
            return (int)i;
    }
    return -1;
}

static int fm_file_open(const qrx_fm_t *fm, unsigned index)
{
    unsigned i;

    for (i = 0u; i < QRX_FM_MAX_OPENS; i++) {
        if (fm->opens[i].used && fm->opens[i].file == index)
            return 1;
    }
    return 0;
}

static int fm_find_open(const qrx_fm_t *fm, int16_t refnum)
{
    unsigned i;

    if (refnum < 1 || refnum > (int16_t)QRX_FM_MAX_OPENS)
        return -1;
    i = (unsigned)(refnum - 1);
    if (!fm->opens[i].used)
        return -1;
    return (int)i;
}

static int fm_reserve(qrx_fm_file_t *f, uint32_t size)
{
    uint8_t *fresh;
    uint32_t cap;

    if (size <= f->cap)
        return 1;
    cap = f->cap != 0u ? f->cap : 64u;
    while (cap < size)
        cap *= 2u;
    fresh = (uint8_t *)malloc(cap);
    if (!fresh)
        return 0;
    memset(fresh, 0, cap);
    if (f->data) {
        memcpy(fresh, f->data, f->size);
        free(f->data);
    }
    f->data = fresh;
    f->cap = cap;
    return 1;
}

void qrx_fm_init(qrx_fm_t *fm, qrx_ppc_t *ppc)
{
    if (fm)
        memset(fm, 0, sizeof(*fm));
    if (fm && ppc)
        fm->ppc = ppc;
}

int32_t qrx_fm_create_at(qrx_fm_t *fm, const char *name,
                         const uint8_t *fndr)
{
    qrx_fm_file_t *f;
    unsigned i;

    if (!fm || !name || name[0] == '\0')
        return QRX_TB_PARAM_ERR;
    if (fm_find(fm, name) >= 0)
        return QRX_TB_DUP_FN;
    f = 0;
    for (i = 0u; i < QRX_FM_MAX_FILES; i++) {
        if (!fm->files[i].used) {
            f = &fm->files[i];
            break;
        }
    }
    if (!f)
        return QRX_TB_DSK_FUL_ERR;
    memset(f, 0, sizeof(*f));
    for (i = 0u; i < QRX_FM_MAX_NAME && name[i] != '\0'; i++)
        f->name[i] = name[i];
    f->name[i] = '\0';
    if (fndr)
        memcpy(f->fndr, fndr, 16u);
    f->used = 1u;
    return QRX_TB_NO_ERR;
}

int32_t qrx_fm_open(qrx_fm_t *fm, const char *name, int16_t *refnum)
{
    qrx_fm_open_t *o;
    unsigned i;
    int index;

    if (!fm || !name || !refnum)
        return QRX_TB_PARAM_ERR;
    index = fm_find(fm, name);
    if (index < 0)
        return QRX_TB_FNF_ERR;
    o = 0;
    for (i = 0u; i < QRX_FM_MAX_OPENS; i++) {
        if (!fm->opens[i].used) {
            o = &fm->opens[i];
            break;
        }
    }
    if (!o)
        return QRX_TB_DSK_FUL_ERR;
    memset(o, 0, sizeof(*o));
    o->used = 1u;
    o->file = (uint8_t)index;
    o->mark = 0u;
    *refnum = (int16_t)((o - fm->opens) + 1);
    return QRX_TB_NO_ERR;
}

int32_t qrx_fm_close(qrx_fm_t *fm, int16_t refnum)
{
    int slot;

    if (!fm)
        return QRX_TB_PARAM_ERR;
    slot = fm_find_open(fm, refnum);
    if (slot < 0)
        return QRX_TB_RF_NUM_ERR;
    fm->opens[slot].used = 0u;
    return QRX_TB_NO_ERR;
}

int32_t qrx_fm_delete(qrx_fm_t *fm, const char *name)
{
    qrx_fm_file_t *f;
    int index;

    if (!fm || !name)
        return QRX_TB_PARAM_ERR;
    index = fm_find(fm, name);
    if (index < 0)
        return QRX_TB_FNF_ERR;
    if (fm_file_open(fm, (unsigned)index))
        return QRX_TB_FN_OPN_ERR;
    f = &fm->files[index];
    if (f->data)
        free(f->data);
    memset(f, 0, sizeof(*f));
    return QRX_TB_NO_ERR;
}

int32_t qrx_fm_get_file_info(const qrx_fm_t *fm, const char *name,
                             uint8_t *fndr)
{
    int index;

    if (!fm || !name || !fndr)
        return QRX_TB_PARAM_ERR;
    index = fm_find(fm, name);
    if (index < 0)
        return QRX_TB_FNF_ERR;
    memcpy(fndr, fm->files[index].fndr, 16u);
    return QRX_TB_NO_ERR;
}

int32_t qrx_fm_set_file_info(qrx_fm_t *fm, const char *name,
                             const uint8_t *fndr)
{
    int index;

    if (!fm || !name || !fndr)
        return QRX_TB_PARAM_ERR;
    index = fm_find(fm, name);
    if (index < 0)
        return QRX_TB_FNF_ERR;
    memcpy(fm->files[index].fndr, fndr, 16u);
    return QRX_TB_NO_ERR;
}

/* Volume: only the default volume and our vrefnum 1 exist. */
static int32_t fm_check_vref(const qrx_fm_t *fm, int16_t vref)
{
    (void)fm;
    if (vref == 0 || vref == 1)
        return QRX_TB_NO_ERR;
    return QRX_TB_VOL_OFF_LIN_ERR;
}

/* File mark for the current op: fsAtMark (0) keeps the mark, fsFromStart
 * (1) takes ioPosOffset; anything else is paramErr. */
static int32_t fm_set_mark(qrx_fm_open_t *o, int16_t pos_mode,
                           uint32_t pos_offset)
{
    if (pos_mode == 0)
        return QRX_TB_NO_ERR;
    if (pos_mode == 1) {
        o->mark = pos_offset;
        return QRX_TB_NO_ERR;
    }
    return QRX_TB_PARAM_ERR;
}

static int32_t fm_dispatch_io(qrx_fm_t *fm, uint32_t param, uint32_t sel)
{
    char name[QRX_FM_MAX_NAME + 1u];
    int16_t vref;
    int16_t refnum;
    int32_t err = QRX_TB_NO_ERR;

    if (param == 0u)
        return QRX_TB_PARAM_ERR;
    vref = fm_get16(fm, param + 8u);
    err = fm_check_vref(fm, vref);
    if (err != QRX_TB_NO_ERR)
        return err;
    if (sel == QRX_TB_FM_OPEN) {
        if (!fm_read_name(fm, fm_get32(fm, param + 4u), name, sizeof(name)))
            return QRX_TB_PARAM_ERR;
        refnum = 0;
        err = qrx_fm_open(fm, name, &refnum);
        if (err == QRX_TB_NO_ERR)
            fm_put16(fm, param + 10u, refnum);
        return err;
    }
    if (sel == QRX_TB_FM_CLOSE) {
        return qrx_fm_close(fm, fm_get16(fm, param + 10u));
    }
    refnum = fm_get16(fm, param + 10u);
    {
        int slot = fm_find_open(fm, refnum);
        qrx_fm_open_t *o;
        qrx_fm_file_t *f;

        if (slot < 0)
            return QRX_TB_RF_NUM_ERR;
        o = &fm->opens[slot];
        f = &fm->files[o->file];
        if (sel == QRX_TB_FM_GET_EOF) {
            fm_put32(fm, param + 20u, f->size);
            return QRX_TB_NO_ERR;
        }
        if (sel == QRX_TB_FM_SET_EOF) {
            uint32_t size = fm_get32(fm, param + 20u);

            if (size > QRX_FM_MAX_FILE)
                return QRX_TB_DSK_FUL_ERR;
            if (size > f->size) {
                if (!fm_reserve(f, size))
                    return QRX_TB_DSK_FUL_ERR;
            }
            f->size = size;
            if (o->mark > size)
                o->mark = size;
            return QRX_TB_NO_ERR;
        }
        err = fm_set_mark(o, fm_get16(fm, param + 24u),
                          fm_get32(fm, param + 26u));
        if (err != QRX_TB_NO_ERR)
            return err;
        if (sel == QRX_TB_FM_READ) {
            uint32_t want = fm_get32(fm, param + 16u);
            uint32_t have = f->size > o->mark ? f->size - o->mark : 0u;
            uint32_t n = want < have ? want : have;
            uint32_t buffer = fm_get32(fm, param + 12u);

            if (n != 0u) {
                if (buffer == 0u ||
                    (uint64_t)buffer + (uint64_t)n > fm->ppc->mem.size)
                    return QRX_TB_PARAM_ERR;
                memcpy(&fm->ppc->mem.data[buffer], &f->data[o->mark], n);
                o->mark += n;
            }
            fm_put32(fm, param + 20u, n);
            return QRX_TB_NO_ERR;
        }
        if (sel == QRX_TB_FM_WRITE) {
            uint32_t want = fm_get32(fm, param + 16u);
            uint32_t buffer = fm_get32(fm, param + 12u);

            if (buffer == 0u ||
                (uint64_t)buffer + (uint64_t)want > fm->ppc->mem.size)
                return QRX_TB_PARAM_ERR;
            if (want > QRX_FM_MAX_FILE ||
                o->mark + want > QRX_FM_MAX_FILE)
                return QRX_TB_DSK_FUL_ERR;
            if (!fm_reserve(f, o->mark + want))
                return QRX_TB_DSK_FUL_ERR;
            if (want != 0u) {
                memcpy(&f->data[o->mark], &fm->ppc->mem.data[buffer], want);
                o->mark += want;
                if (f->size < o->mark)
                    f->size = o->mark;
            }
            fm_put32(fm, param + 20u, want);
            return QRX_TB_NO_ERR;
        }
        return QRX_TB_PARAM_ERR;
    }
}

static int32_t fm_dispatch_name(qrx_fm_t *fm, uint32_t param, uint32_t sel)
{
    char name[QRX_FM_MAX_NAME + 1u];
    int16_t vref;
    int32_t err;

    if (param == 0u)
        return QRX_TB_PARAM_ERR;
    if (!fm_read_name(fm, fm_get32(fm, param + 2u), name, sizeof(name)))
        return QRX_TB_PARAM_ERR;
    vref = fm_get16(fm, param + 6u);
    err = fm_check_vref(fm, vref);
    if (err != QRX_TB_NO_ERR)
        return err;
    if (sel == QRX_TB_FM_CREATE)
        return qrx_fm_create_at(fm, name, &fm->ppc->mem.data[param + 8u]);
    if (sel == QRX_TB_FM_DELETE)
        return qrx_fm_delete(fm, name);
    if (sel == QRX_TB_FM_GET_FILE_INFO)
        return qrx_fm_get_file_info(fm, name, &fm->ppc->mem.data[param + 8u]);
    if (sel == QRX_TB_FM_SET_FILE_INFO)
        return qrx_fm_set_file_info(fm, name, &fm->ppc->mem.data[param + 8u]);
    return QRX_TB_PARAM_ERR;
}

uint32_t qrx_fm_dispatch(qrx_fm_t *fm, const qrx_mix_args_t *args)
{
    uint32_t sel;
    uint32_t param;
    int32_t err = QRX_TB_PARAM_ERR;

    if (!fm || !args)
        return (uint32_t)QRX_MM_UNKNOWN;
    sel = args->gpr[0];
    param = args->gpr[1];
    if (sel == QRX_TB_FM_OPEN || sel == QRX_TB_FM_CLOSE ||
        sel == QRX_TB_FM_READ || sel == QRX_TB_FM_WRITE ||
        sel == QRX_TB_FM_GET_EOF || sel == QRX_TB_FM_SET_EOF)
        err = fm_dispatch_io(fm, param, sel);
    else if (sel == QRX_TB_FM_CREATE || sel == QRX_TB_FM_DELETE ||
             sel == QRX_TB_FM_GET_FILE_INFO || sel == QRX_TB_FM_SET_FILE_INFO)
        err = fm_dispatch_name(fm, param, sel);
    if (param != 0u && (sel == QRX_TB_FM_OPEN || sel == QRX_TB_FM_CLOSE ||
                        sel == QRX_TB_FM_READ || sel == QRX_TB_FM_WRITE ||
                        sel == QRX_TB_FM_GET_EOF || sel == QRX_TB_FM_SET_EOF))
        fm_put16(fm, param + 2u, (int16_t)err);
    else if (param != 0u)
        fm_put16(fm, param + 0u, (int16_t)err);
    return (uint32_t)err;
}

uint32_t qrx_rm_dispatch(qrx_rm_t *rm, const qrx_mix_args_t *args)
{
    uint32_t sel;
    uint32_t a;
    uint32_t b;
    char name[32];
    uint32_t name_addr;

    if (!rm || !args)
        return (uint32_t)QRX_MM_UNKNOWN;
    sel = args->gpr[0];
    a = args->gpr[1];
    b = args->gpr[2];
    switch (sel) {
    case QRX_TB_RM_GET:
        return qrx_rm_get_resource(rm, a, (int16_t)b);
    case QRX_TB_RM_GET_NAMED: {
        const qrx_ppc_t *ppc = rm->mm->ppc;
        unsigned len;
        unsigned i;

        name_addr = b;
        if (name_addr == 0u || name_addr >= ppc->mem.size ||
            ppc->mem.data[name_addr] == 0u ||
            ppc->mem.data[name_addr] > 31u) {
            rm->last_err = QRX_TB_RES_NOT_FOUND;
            return 0u;
        }
        len = ppc->mem.data[name_addr];
        for (i = 0u; i < len && i < sizeof(name) - 1u; i++)
            name[i] = (char)ppc->mem.data[name_addr + 1u + i];
        name[i] = '\0';
        return qrx_rm_get_named_resource(rm, a, name);
    }
    case QRX_TB_RM_LOAD:
        return (uint32_t)qrx_rm_load_resource(rm, a);
    case QRX_TB_RM_RELEASE:
        return (uint32_t)qrx_rm_release_resource(rm, a);
    case QRX_TB_RM_DETACH:
        return (uint32_t)qrx_rm_detach_resource(rm, a);
    case QRX_TB_RM_REMOVE:
        return (uint32_t)qrx_rm_remove_resource(rm, a);
    case QRX_TB_RM_SIZE:
        return (uint32_t)qrx_rm_size_resource(rm, a);
    case QRX_TB_RM_GET_ATTRS:
        return (uint32_t)qrx_rm_get_attrs(rm, a);
    case QRX_TB_RM_SET_ATTRS:
        return (uint32_t)qrx_rm_set_attrs(rm, a, (int32_t)b);
    case QRX_TB_RM_ADD: {
        /* r4=type, r5=id, r6=name (Pascal, 0=none), r7=handle */
        uint32_t name_addr = args->gpr[3];
        const qrx_ppc_t *ppc = rm->mm->ppc;
        unsigned len;
        unsigned i;

        name[0] = '\0';
        if (name_addr != 0u) {
            if (name_addr >= ppc->mem.size ||
                ppc->mem.data[name_addr] == 0u ||
                ppc->mem.data[name_addr] > 31u ||
                name_addr + 1u + ppc->mem.data[name_addr] > ppc->mem.size)
                return (uint32_t)rm_done(rm, QRX_TB_PARAM_ERR);
            len = ppc->mem.data[name_addr];
            for (i = 0u; i < len && i < sizeof(name) - 1u; i++)
                name[i] = (char)ppc->mem.data[name_addr + 1u + i];
            name[i] = '\0';
        }
        return (uint32_t)qrx_rm_add_resource(
            rm, a, (int16_t)b, name[0] != '\0' ? name : 0,
            args->gpr[4]);
    }
    case QRX_TB_RM_COUNT:
        return qrx_rm_count_resources(rm, a);
    case QRX_TB_RM_GET_IND:
        return qrx_rm_get_ind_resource(rm, a, b);
    case QRX_TB_RM_COUNT_TYPES:
        return qrx_rm_count_types(rm);
    case QRX_TB_RM_GET_IND_TYPE:
        return qrx_rm_get_ind_type(rm, a);
    case QRX_TB_RM_ERROR:
        return (uint32_t)qrx_rm_res_error(rm);
    case QRX_TB_RM_DISPATCH:
        /* Only selector $7003 (SetResourceSize) is in this slice. */
        if (a != QRX_TB_RM_SET_SIZE_SELEN) {
            rm->last_err = QRX_TB_PARAM_ERR;
            return (uint32_t)QRX_TB_PARAM_ERR;
        }
        return (uint32_t)qrx_rm_set_resource_size(rm, b, args->gpr[3]);
    default:
        return (uint32_t)QRX_MM_UNKNOWN;
    }
}


/* ---- QuickDraw -------------------------------------------------------- */
/* Checked guest-memory helpers for the bitmap/rect/point shapes. */
static int qd_range(const qrx_ppc_t *ppc, uint32_t addr, uint32_t len)
{
    return addr <= ppc->mem.size && len <= ppc->mem.size - addr;
}

static int16_t qd_get16(const qrx_ppc_t *ppc, uint32_t addr)
{
    return (int16_t)(((uint16_t)ppc->mem.data[addr] << 8) |
                     (uint16_t)ppc->mem.data[addr + 1u]);
}

static uint32_t qd_get32(const qrx_ppc_t *ppc, uint32_t addr)
{
    return (((uint32_t)ppc->mem.data[addr] << 24) |
            ((uint32_t)ppc->mem.data[addr + 1u] << 16) |
            ((uint32_t)ppc->mem.data[addr + 2u] << 8) |
            (uint32_t)ppc->mem.data[addr + 3u]);
}

static void qd_put16(qrx_ppc_t *ppc, uint32_t addr, int16_t v)
{
    ppc->mem.data[addr] = (uint8_t)((uint16_t)v >> 8);
    ppc->mem.data[addr + 1u] = (uint8_t)v;
}

static void qd_put32(qrx_ppc_t *ppc, uint32_t addr, uint32_t v)
{
    ppc->mem.data[addr] = (uint8_t)(v >> 24);
    ppc->mem.data[addr + 1u] = (uint8_t)(v >> 16);
    ppc->mem.data[addr + 2u] = (uint8_t)(v >> 8);
    ppc->mem.data[addr + 3u] = (uint8_t)v;
}

typedef struct {
    uint32_t base;
    int16_t row_bytes;
    int16_t top, left, bottom, right;
} qd_bitmap_t;

/* Read and validate a BitMap (basePtr/rowBytes/bounds) from guest memory. */
static int qd_read_bitmap(const qrx_ppc_t *ppc, uint32_t addr, qd_bitmap_t *bm)
{
    int16_t width, height;

    if (!qd_range(ppc, addr, 14u))
        return 0;
    bm->base = qd_get32(ppc, addr);
    bm->row_bytes = qd_get16(ppc, addr + 4u);
    bm->top = qd_get16(ppc, addr + 6u);
    bm->left = qd_get16(ppc, addr + 8u);
    bm->bottom = qd_get16(ppc, addr + 10u);
    bm->right = qd_get16(ppc, addr + 12u);
    width = (int16_t)(bm->right - bm->left);
    height = (int16_t)(bm->bottom - bm->top);
    if (bm->row_bytes <= 0 || width <= 0 || height <= 0)
        return 0;
    if (!qd_range(ppc, bm->base, (uint32_t)bm->row_bytes * (uint32_t)height))
        return 0;
    return 1;
}

static int qd_in_bounds(const qd_bitmap_t *bm, int16_t h, int16_t v)
{
    return h >= bm->left && h < bm->right && v >= bm->top && v < bm->bottom;
}

static int qd_get_pixel(const qrx_ppc_t *ppc, const qd_bitmap_t *bm,
                        int16_t h, int16_t v)
{
    uint32_t addr = bm->base +
        (uint32_t)((int32_t)(v - bm->top) * (int32_t)bm->row_bytes) +
        (uint32_t)(h - bm->left) / 8u;

    if (!qd_in_bounds(bm, h, v) || !qd_range(ppc, addr, 1u))
        return 0;
    return (ppc->mem.data[addr] >> (7u - (uint32_t)(h - bm->left) % 8u)) & 1u;
}

static void qd_set_pixel(qrx_ppc_t *ppc, const qd_bitmap_t *bm,
                         int16_t h, int16_t v, int bit)
{
    uint32_t addr = bm->base +
        (uint32_t)((int32_t)(v - bm->top) * (int32_t)bm->row_bytes) +
        (uint32_t)(h - bm->left) / 8u;
    unsigned shift = 7u - (uint32_t)(h - bm->left) % 8u;

    if (!qd_in_bounds(bm, h, v) || !qd_range(ppc, addr, 1u))
        return;
    if (bit)
        ppc->mem.data[addr] = (uint8_t)(ppc->mem.data[addr] | (1u << shift));
    else
        ppc->mem.data[addr] = (uint8_t)(ppc->mem.data[addr] & ~(1u << shift));
}

static int qd_pat_bit(const uint8_t *pat, int16_t h, int16_t v)
{
    unsigned row = (unsigned)(v & 7);
    unsigned col = (unsigned)(h & 7);

    return (pat[row] >> (7u - col)) & 1u;
}

static const uint8_t QD_PAT_BLACK[8] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
static const uint8_t QD_PAT_WHITE[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

void qrx_qd_init(qrx_qd_t *qd, qrx_ppc_t *ppc)
{
    if (!qd)
        return;
    memset(qd, 0, sizeof(*qd));
    qd->ppc = ppc;
    qd->pen_w = 1;
    qd->pen_hgt = 1;
    qd->pen_mode = 0;
    qd->pen_vis = 1;
    memcpy(qd->pen_pat, QD_PAT_BLACK, sizeof(qd->pen_pat));
    memcpy(qd->back_pat, QD_PAT_WHITE, sizeof(qd->back_pat));
}

void qrx_qd_set_screen(qrx_qd_t *qd, uint32_t bitmap_addr)
{
    qd_bitmap_t bm;

    qd->port = bitmap_addr;
    if (qd_read_bitmap(qd->ppc, bitmap_addr, &bm)) {
        qd->pen_h = bm.left;
        qd->pen_v = bm.top;
    }
}

uint32_t qrx_qd_get_port(const qrx_qd_t *qd)
{
    return qd ? qd->port : 0u;
}

int32_t qrx_qd_move_to(qrx_qd_t *qd, int16_t h, int16_t v)
{
    qd_bitmap_t bm;

    if (!qd || !qd_read_bitmap(qd->ppc, qd->port, &bm))
        return QRX_TB_PARAM_ERR;
    qd->pen_h = h;
    qd->pen_v = v;
    return QRX_TB_NO_ERR;
}

int32_t qrx_qd_fill_rect(qrx_qd_t *qd, int16_t top, int16_t left,
                         int16_t bottom, int16_t right,
                         const uint8_t *pat)
{
    qd_bitmap_t bm;
    int16_t y, x;

    if (!qd || !pat || !qd_read_bitmap(qd->ppc, qd->port, &bm))
        return QRX_TB_PARAM_ERR;
    if (top < bm.top) top = bm.top;
    if (left < bm.left) left = bm.left;
    if (bottom > bm.bottom) bottom = bm.bottom;
    if (right > bm.right) right = bm.right;
    for (y = top; y < bottom; y++)
        for (x = left; x < right; x++)
            qd_set_pixel(qd->ppc, &bm, x, y, qd_pat_bit(pat, x, y));
    return QRX_TB_NO_ERR;
}

int32_t qrx_qd_erase_rect(qrx_qd_t *qd, int16_t top, int16_t left,
                          int16_t bottom, int16_t right)
{
    return qrx_qd_fill_rect(qd, top, left, bottom, right, qd->back_pat);
}

int32_t qrx_qd_invert_rect(qrx_qd_t *qd, int16_t top, int16_t left,
                           int16_t bottom, int16_t right)
{
    qd_bitmap_t bm;
    int16_t y, x;

    if (!qd || !qd_read_bitmap(qd->ppc, qd->port, &bm))
        return QRX_TB_PARAM_ERR;
    if (top < bm.top) top = bm.top;
    if (left < bm.left) left = bm.left;
    if (bottom > bm.bottom) bottom = bm.bottom;
    if (right > bm.right) right = bm.right;
    for (y = top; y < bottom; y++)
        for (x = left; x < right; x++)
            qd_set_pixel(qd->ppc, &bm, x, y,
                         qd_get_pixel(qd->ppc, &bm, x, y) ^ 1);
    return QRX_TB_NO_ERR;
}

int32_t qrx_qd_frame_rect(qrx_qd_t *qd, int16_t top, int16_t left,
                          int16_t bottom, int16_t right)
{
    int16_t w = qd ? qd->pen_w : 1;
    int16_t h = qd ? qd->pen_hgt : 1;

    if (!qd)
        return QRX_TB_PARAM_ERR;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (qrx_qd_fill_rect(qd, top, left, top + h, right, qd->pen_pat) != 0)
        return QRX_TB_PARAM_ERR;
    qrx_qd_fill_rect(qd, bottom - h, left, bottom, right, qd->pen_pat);
    qrx_qd_fill_rect(qd, top, left, bottom, left + w, qd->pen_pat);
    qrx_qd_fill_rect(qd, top, right - w, bottom, right, qd->pen_pat);
    return QRX_TB_NO_ERR;
}

int32_t qrx_qd_line_to(qrx_qd_t *qd, int16_t h, int16_t v)
{
    qd_bitmap_t bm;
    int16_t x0, y0, dx, dy, err, sx, sy;
    int16_t w, hgt;

    if (!qd || !qd_read_bitmap(qd->ppc, qd->port, &bm))
        return QRX_TB_PARAM_ERR;
    x0 = qd->pen_h;
    y0 = qd->pen_v;
    dx = (int16_t)((h > x0) ? (h - x0) : (x0 - h));
    dy = (int16_t)((v > y0) ? (v - y0) : (y0 - v));
    sx = (x0 < h) ? 1 : -1;
    sy = (y0 < v) ? 1 : -1;
    err = (int16_t)(dx - dy);
    w = qd->pen_w < 1 ? 1 : qd->pen_w;
    hgt = qd->pen_hgt < 1 ? 1 : qd->pen_hgt;
    for (;;) {
        if (qd->pen_vis)
            qrx_qd_fill_rect(qd, y0, x0, (int16_t)(y0 + hgt),
                             (int16_t)(x0 + w), qd->pen_pat);
        if (x0 == h && y0 == v)
            break;
        {
            int16_t e2 = (int16_t)(2 * err);
            if (e2 > -dy) { err = (int16_t)(err - dy); x0 = (int16_t)(x0 + sx); }
            if (e2 < dx) { err = (int16_t)(err + dx); y0 = (int16_t)(y0 + sy); }
        }
    }
    qd->pen_h = h;
    qd->pen_v = v;
    return QRX_TB_NO_ERR;
}

/* Read a Rect from guest memory; returns 0 when it is not readable. */
static int qd_read_rect(const qrx_ppc_t *ppc, uint32_t addr,
                        int16_t *top, int16_t *left, int16_t *bottom,
                        int16_t *right)
{
    if (!qd_range(ppc, addr, 8u))
        return 0;
    *top = qd_get16(ppc, addr);
    *left = qd_get16(ppc, addr + 2u);
    *bottom = qd_get16(ppc, addr + 4u);
    *right = qd_get16(ppc, addr + 6u);
    return 1;
}

static int qd_write_rect(qrx_ppc_t *ppc, uint32_t addr, int16_t top,
                         int16_t left, int16_t bottom, int16_t right)
{
    if (!qd_range(ppc, addr, 8u))
        return 0;
    qd_put16(ppc, addr, top);
    qd_put16(ppc, addr + 2u, left);
    qd_put16(ppc, addr + 4u, bottom);
    qd_put16(ppc, addr + 6u, right);
    return 1;
}

/* Copy an 8-byte pattern from guest memory; returns 0 when unreadable. */
static int qd_read_pat(const qrx_ppc_t *ppc, uint32_t addr, uint8_t *out)
{
    if (!qd_range(ppc, addr, 8u))
        return 0;
    memcpy(out, &ppc->mem.data[addr], 8u);
    return 1;
}

uint32_t qrx_qd_dispatch(qrx_qd_t *qd, const qrx_mix_args_t *args)
{
    uint32_t sel;
    int16_t top, left, bottom, right;
    uint8_t pat[8];

    if (!qd || !args)
        return QRX_MM_UNKNOWN;
    sel = args->gpr[0];
    switch (sel) {
    case QRX_TB_QD_INIT_GRAF:
        /* r4 = @thePort: publish the current port there. */
        if (!qd_range(qd->ppc, args->gpr[1], 4u))
            return (uint32_t)QRX_TB_PARAM_ERR;
        qd_put32(qd->ppc, args->gpr[1], qd->port);
        return 0u;
    case QRX_TB_QD_INIT_PORT:
    case QRX_TB_QD_OPEN_PORT:
        /* r4 = BitMap/Port address: make it the current port when valid. */
        {
            qd_bitmap_t bm;
            if (!qd_read_bitmap(qd->ppc, args->gpr[1], &bm))
                return (uint32_t)QRX_TB_PARAM_ERR;
            qd->port = args->gpr[1];
            qd->pen_h = bm.left;
            qd->pen_v = bm.top;
        }
        return 0u;
    case QRX_TB_QD_SET_PORT:
        {
            qd_bitmap_t bm;
            if (!qd_read_bitmap(qd->ppc, args->gpr[1], &bm))
                return (uint32_t)QRX_TB_PARAM_ERR;
            qd->port = args->gpr[1];
        }
        return 0u;
    case QRX_TB_QD_GET_PORT:
        return qd->port;
    case QRX_TB_QD_MOVE_TO:
        /* r4 = packed h:v, or r4/r5 as separate halves.  Pascal would pass
         * one Point; our seam packs (h << 16) | (v & 0xFFFF) in r4. */
        return (uint32_t)qrx_qd_move_to(qd, (int16_t)(args->gpr[1] >> 16),
                                        (int16_t)args->gpr[1]);
    case QRX_TB_QD_LINE_TO:
        return (uint32_t)qrx_qd_line_to(qd, (int16_t)(args->gpr[1] >> 16),
                                        (int16_t)args->gpr[1]);
    case QRX_TB_QD_LINE:
        return (uint32_t)qrx_qd_line_to(
            qd, (int16_t)(qd->pen_h + (int16_t)(args->gpr[1] >> 16)),
            (int16_t)(qd->pen_v + (int16_t)args->gpr[1]));
    case QRX_TB_QD_HIDE_PEN:
        qd->pen_vis = 0;
        return 0u;
    case QRX_TB_QD_SHOW_PEN:
        qd->pen_vis = 1;
        return 0u;
    case QRX_TB_QD_GET_PEN:
        /* r4 = Point* to receive (h,v). */
        if (!qd_range(qd->ppc, args->gpr[1], 4u))
            return (uint32_t)QRX_TB_PARAM_ERR;
        qd_put16(qd->ppc, args->gpr[1], qd->pen_h);
        qd_put16(qd->ppc, args->gpr[1] + 2u, qd->pen_v);
        return 0u;
    case QRX_TB_QD_SET_PT:
        /* r4 = Point*, r5 = packed h:v. */
        if (!qd_range(qd->ppc, args->gpr[1], 4u))
            return (uint32_t)QRX_TB_PARAM_ERR;
        qd_put16(qd->ppc, args->gpr[1], (int16_t)(args->gpr[2] >> 16));
        qd_put16(qd->ppc, args->gpr[1] + 2u, (int16_t)args->gpr[2]);
        return 0u;
    case QRX_TB_QD_PEN_SIZE:
        /* r4 = packed w:h. */
        qd->pen_w = (int16_t)(args->gpr[1] >> 16);
        qd->pen_hgt = (int16_t)args->gpr[1];
        if (qd->pen_w < 1) qd->pen_w = 1;
        if (qd->pen_hgt < 1) qd->pen_hgt = 1;
        return 0u;
    case QRX_TB_QD_PEN_MODE:
        if ((int16_t)(args->gpr[1] & 0xFFFFu) > 1)
            return (uint32_t)QRX_TB_PARAM_ERR;
        qd->pen_mode = (int16_t)(args->gpr[1] & 0xFFFFu);
        return 0u;
    case QRX_TB_QD_PEN_PAT:
        if (!qd_read_pat(qd->ppc, args->gpr[1], pat))
            return (uint32_t)QRX_TB_PARAM_ERR;
        memcpy(qd->pen_pat, pat, 8u);
        return 0u;
    case QRX_TB_QD_PEN_NORMAL:
        qd->pen_w = 1;
        qd->pen_hgt = 1;
        qd->pen_mode = 0;
        memcpy(qd->pen_pat, QD_PAT_BLACK, 8u);
        return 0u;
    case QRX_TB_QD_BACK_PAT:
        if (!qd_read_pat(qd->ppc, args->gpr[1], pat))
            return (uint32_t)QRX_TB_PARAM_ERR;
        memcpy(qd->back_pat, pat, 8u);
        return 0u;
    case QRX_TB_QD_FRAME_RECT:
    case QRX_TB_QD_PAINT_RECT:
    case QRX_TB_QD_ERASE_RECT:
    case QRX_TB_QD_INVERT_RECT:
    case QRX_TB_QD_FILL_RECT:
        if (!qd_read_rect(qd->ppc, args->gpr[1], &top, &left, &bottom, &right))
            return (uint32_t)QRX_TB_PARAM_ERR;
        if (sel == QRX_TB_QD_FRAME_RECT)
            return (uint32_t)qrx_qd_frame_rect(qd, top, left, bottom, right);
        if (sel == QRX_TB_QD_ERASE_RECT)
            return (uint32_t)qrx_qd_erase_rect(qd, top, left, bottom, right);
        if (sel == QRX_TB_QD_INVERT_RECT)
            return (uint32_t)qrx_qd_invert_rect(qd, top, left, bottom, right);
        if (sel == QRX_TB_QD_FILL_RECT) {
            if (!qd_read_pat(qd->ppc, args->gpr[2], pat))
                return (uint32_t)QRX_TB_PARAM_ERR;
            return (uint32_t)qrx_qd_fill_rect(qd, top, left, bottom, right, pat);
        }
        return (uint32_t)qrx_qd_fill_rect(qd, top, left, bottom, right,
                                           qd->pen_pat);
    case QRX_TB_QD_EQUAL_RECT:
        if (!qd_read_rect(qd->ppc, args->gpr[1], &top, &left, &bottom, &right))
            return (uint32_t)QRX_TB_PARAM_ERR;
        {
            int16_t t2, l2, b2, r2;
            if (!qd_read_rect(qd->ppc, args->gpr[2], &t2, &l2, &b2, &r2))
                return (uint32_t)QRX_TB_PARAM_ERR;
            return (t2 == top && l2 == left && b2 == bottom && r2 == right)
                ? 1u : 0u;
        }
    case QRX_TB_QD_SET_RECT:
        /* r4 = Rect*, r5 = top, r6 = left, r7 = bottom, r8 = right. */
        return qd_write_rect(qd->ppc, args->gpr[1],
                             (int16_t)args->gpr[2], (int16_t)args->gpr[3],
                             (int16_t)args->gpr[4], (int16_t)args->gpr[5])
            ? 0u : (uint32_t)QRX_TB_PARAM_ERR;
    case QRX_TB_QD_OFFSET_RECT:
        if (!qd_read_rect(qd->ppc, args->gpr[1], &top, &left, &bottom, &right))
            return (uint32_t)QRX_TB_PARAM_ERR;
        return qd_write_rect(qd->ppc, args->gpr[1],
                             (int16_t)(top + (int16_t)(args->gpr[2] >> 16)),
                             (int16_t)(left + (int16_t)args->gpr[2]),
                             (int16_t)(bottom + (int16_t)(args->gpr[2] >> 16)),
                             (int16_t)(right + (int16_t)args->gpr[2]))
            ? 0u : (uint32_t)QRX_TB_PARAM_ERR;
    case QRX_TB_QD_INSET_RECT:
        if (!qd_read_rect(qd->ppc, args->gpr[1], &top, &left, &bottom, &right))
            return (uint32_t)QRX_TB_PARAM_ERR;
        return qd_write_rect(qd->ppc, args->gpr[1],
                             (int16_t)(top + (int16_t)(args->gpr[2] >> 16)),
                             (int16_t)(left + (int16_t)args->gpr[2]),
                             (int16_t)(bottom - (int16_t)(args->gpr[2] >> 16)),
                             (int16_t)(right - (int16_t)args->gpr[2]))
            ? 0u : (uint32_t)QRX_TB_PARAM_ERR;
    case QRX_TB_QD_PT_IN_RECT:
        /* r4 = Point*, r5 = Rect*. */
        if (!qd_range(qd->ppc, args->gpr[1], 4u) ||
            !qd_read_rect(qd->ppc, args->gpr[2], &top, &left, &bottom, &right))
            return (uint32_t)QRX_TB_PARAM_ERR;
        {
            int16_t h = qd_get16(qd->ppc, args->gpr[1]);
            int16_t v = qd_get16(qd->ppc, args->gpr[1] + 2u);
            return (h >= left && h < right && v >= top && v < bottom) ? 1u : 0u;
        }
    default:
        return QRX_MM_UNKNOWN;
    }
}

/* ---- Window Manager ----------------------------------------------------- */
static int wm_slot_of(const qrx_wm_t *wm, uint32_t record)
{
    int i;

    for (i = 0; i < (int)QRX_WM_MAX_WINDOWS; i++) {
        if (wm->wins[i].used && wm->wins[i].record == record)
            return i;
    }
    return -1;
}

static void wm_write_record(qrx_wm_t *wm, int slot)
{
    qrx_wm_window_t *w = &wm->wins[slot];
    qrx_ppc_t *ppc = wm->ppc;
    int16_t row_bytes;
    int16_t width = (int16_t)(w->right - w->left);
    int16_t height = (int16_t)(w->bottom - w->top);

    if (!qd_range(ppc, w->record, QRX_WM_RECORD_SIZE))
        return;
    qd_put16(ppc, w->record, (int16_t)QRX_WM_KIND);       /* windowKind */
    qd_put16(ppc, w->record + 2u, (int16_t)(w->visible ? 1 : 0));
    qd_put32(ppc, w->record + 4u, w->refcon);             /* refCon */
    qd_put16(ppc, w->record + 24u, w->top);               /* frame */
    qd_put16(ppc, w->record + 26u, w->left);
    qd_put16(ppc, w->record + 28u, w->bottom);
    qd_put16(ppc, w->record + 30u, w->right);
    qd_put32(ppc, w->record + 56u,                        /* nextWindow */
             w->next >= 0 ? wm->wins[w->next].record : 0u);
    qd_put16(ppc, w->record + 64u, (int16_t)strlen(w->title));
    row_bytes = (int16_t)(((width + 15) / 16) * 2);
    if (row_bytes < 2)
        row_bytes = 2;
    qd_put32(ppc, w->record + 82u, w->bitmap);            /* portBits */
    qd_put16(ppc, w->record + 86u, row_bytes);
    qd_put16(ppc, w->record + 88u, 0);                    /* port bounds */
    qd_put16(ppc, w->record + 90u, 0);
    qd_put16(ppc, w->record + 92u, height);
    qd_put16(ppc, w->record + 94u, width);
}

static void wm_refresh_links(qrx_wm_t *wm)
{
    int i;

    for (i = 0; i < (int)QRX_WM_MAX_WINDOWS; i++) {
        if (wm->wins[i].used)
            wm_write_record(wm, i);
    }
}

/* Redraw the desktop: clear the screen, then composite visible windows
 * back-to-front (walk the front-to-back list and draw it in reverse). */
static void wm_composite(qrx_wm_t *wm)
{
    qd_bitmap_t screen;
    int order[QRX_WM_MAX_WINDOWS];
    int n = 0, i, j;

    if (!qd_read_bitmap(wm->ppc, wm->screen, &screen))
        return;
    for (j = 0; j < (int)(screen.bottom - screen.top); j++)
        for (i = 0; i < (int)(screen.right - screen.left); i++)
            qd_set_pixel(wm->ppc, &screen,
                         (int16_t)(screen.left + i),
                         (int16_t)(screen.top + j), 0);
    for (i = wm->front; i >= 0; i = wm->wins[i].next)
        order[n++] = i;
    for (j = n - 1; j >= 0; j--) {
        qrx_wm_window_t *w = &wm->wins[order[j]];
        qd_bitmap_t content;
        int16_t cy, cx;

        if (!w->used || !w->visible || w->bitmap == 0u)
            continue;
        content.base = w->bitmap;
        content.row_bytes = (int16_t)(((w->right - w->left + 15) / 16) * 2);
        if (content.row_bytes < 2)
            content.row_bytes = 2;
        content.top = 0;
        content.left = 0;
        content.bottom = (int16_t)(w->bottom - w->top);
        content.right = (int16_t)(w->right - w->left);
        for (cy = 0; cy < content.bottom; cy++) {
            for (cx = 0; cx < content.right; cx++) {
                int16_t sx = (int16_t)(w->left + cx);
                int16_t sy = (int16_t)(w->top + cy);

                if (qd_in_bounds(&screen, sx, sy) &&
                    qd_get_pixel(wm->ppc, &content, cx, cy))
                    qd_set_pixel(wm->ppc, &screen, sx, sy, 1);
            }
        }
    }
}

void qrx_wm_init(qrx_wm_t *wm, qrx_ppc_t *ppc, qrx_mm_t *mm,
                 uint32_t screen_bitmap)
{
    if (!wm)
        return;
    memset(wm, 0, sizeof(*wm));
    wm->ppc = ppc;
    wm->mm = mm;
    wm->screen = screen_bitmap;
    wm->front = -1;
}

uint32_t qrx_wm_front_window(const qrx_wm_t *wm)
{
    int i;

    for (i = wm->front; i >= 0; i = wm->wins[i].next) {
        if (wm->wins[i].used && wm->wins[i].visible)
            return wm->wins[i].record;
    }
    return 0u;
}

static void wm_unlink(qrx_wm_t *wm, int slot)
{
    int prev = -1, i;

    for (i = wm->front; i >= 0 && i != slot; i = wm->wins[i].next)
        prev = i;
    if (prev < 0)
        wm->front = wm->wins[slot].next;
    else
        wm->wins[prev].next = wm->wins[slot].next;
    wm->wins[slot].next = -1;
}

static void wm_link_front(qrx_wm_t *wm, int slot)
{
    wm->wins[slot].next = wm->front;
    wm->front = slot;
}

static void wm_link_behind(qrx_wm_t *wm, int slot, int behind)
{
    int i;

    if (behind < 0) {
        wm_link_front(wm, slot);
        return;
    }
    for (i = wm->front; i >= 0 && i != behind; i = wm->wins[i].next)
        ;
    if (i < 0) {
        wm_link_front(wm, slot);
        return;
    }
    wm->wins[slot].next = wm->wins[behind].next;
    wm->wins[behind].next = slot;
}

int32_t qrx_wm_show_window(qrx_wm_t *wm, uint32_t win)
{
    int slot = wm_slot_of(wm, win);

    if (slot < 0)
        return QRX_TB_W_WRONG_KIND;
    if (wm->wins[slot].bitmap == 0u)
        return QRX_TB_W_NO_CONTENT;
    wm->wins[slot].visible = 1;
    wm_write_record(wm, slot);
    wm_composite(wm);
    return QRX_TB_NO_ERR;
}

int32_t qrx_wm_hide_window(qrx_wm_t *wm, uint32_t win)
{
    int slot = wm_slot_of(wm, win);

    if (slot < 0)
        return QRX_TB_W_WRONG_KIND;
    wm->wins[slot].visible = 0;
    wm_write_record(wm, slot);
    wm_composite(wm);
    return QRX_TB_NO_ERR;
}

int32_t qrx_wm_close_window(qrx_wm_t *wm, uint32_t win)
{
    int slot = wm_slot_of(wm, win);

    if (slot < 0)
        return QRX_TB_W_WRONG_KIND;
    wm_unlink(wm, slot);
    if (wm->wins[slot].bitmap != 0u)
        qrx_mm_dispose_ptr(wm->mm, wm->wins[slot].bitmap);
    if (wm->wins[slot].own_record)
        qrx_mm_dispose_ptr(wm->mm, wm->wins[slot].record);
    memset(&wm->wins[slot], 0, sizeof(wm->wins[slot]));
    wm_refresh_links(wm);
    wm_composite(wm);
    return QRX_TB_NO_ERR;
}

uint32_t qrx_wm_dispatch(qrx_wm_t *wm, const qrx_mix_args_t *args)
{
    uint32_t sel;
    int slot;

    if (!wm || !args)
        return QRX_MM_UNKNOWN;
    sel = args->gpr[0];
    switch (sel) {
    case QRX_TB_WM_INIT_WINDOWS:
        return 0u;
    case QRX_TB_WM_GET_WMGR_PORT:
        return wm->screen;
    case QRX_TB_WM_FRONT_WINDOW:
        return qrx_wm_front_window(wm);
    case QRX_TB_WM_NEW_WINDOW: {
        /* r4 = wStorage (0 = allocate), r5 = bounds Rect*,
         * r6 = title Pascal*, r7 = visible, r8 = theProc, r9 = behind. */
        int16_t top, left, bottom, right;
        int16_t width, height;
        uint32_t record = args->gpr[1];
        uint32_t bytes;
        int own = record == 0u;
        int i;

        if (!qd_read_rect(wm->ppc, args->gpr[2], &top, &left, &bottom, &right))
            return (uint32_t)QRX_TB_PARAM_ERR;
        width = (int16_t)(right - left);
        height = (int16_t)(bottom - top);
        if (width <= 0 || height <= 0)
            return (uint32_t)QRX_TB_PARAM_ERR;
        for (i = 0; i < (int)QRX_WM_MAX_WINDOWS; i++) {
            if (!wm->wins[i].used)
                break;
        }
        if (i == (int)QRX_WM_MAX_WINDOWS)
            return (uint32_t)QRX_TB_PARAM_ERR;
        bytes = (uint32_t)(((width + 15) / 16) * 2) * (uint32_t)height;
        if (qrx_mm_new_ptr(wm->mm, bytes, 1, &wm->wins[i].bitmap) != QRX_MM_OK)
            return (uint32_t)QRX_TB_PARAM_ERR;
        wm->wins[i].bitmap_bytes = bytes;
        if (own) {
            if (qrx_mm_new_ptr(wm->mm, QRX_WM_RECORD_SIZE, 1, &record) !=
                QRX_MM_OK) {
                qrx_mm_dispose_ptr(wm->mm, wm->wins[i].bitmap);
                memset(&wm->wins[i], 0, sizeof(wm->wins[i]));
                return (uint32_t)QRX_TB_PARAM_ERR;
            }
        }
        if (!qd_range(wm->ppc, record, QRX_WM_RECORD_SIZE)) {
            qrx_mm_dispose_ptr(wm->mm, wm->wins[i].bitmap);
            if (own)
                qrx_mm_dispose_ptr(wm->mm, record);
            return (uint32_t)QRX_TB_PARAM_ERR;
        }
        {
            uint32_t win_bitmap = wm->wins[i].bitmap;
            memset(&wm->wins[i], 0, sizeof(wm->wins[i]));
            wm->wins[i].bitmap = win_bitmap;
            wm->wins[i].bitmap_bytes = bytes;
        }
        wm->wins[i].used = 1;
        wm->wins[i].own_record = (uint8_t)(own ? 1 : 0);
        wm->wins[i].visible = (uint8_t)(args->gpr[4] != 0u);
        wm->wins[i].top = top;
        wm->wins[i].left = left;
        wm->wins[i].bottom = bottom;
        wm->wins[i].right = right;
        wm->wins[i].record = record;
        wm->wins[i].bitmap_bytes = bytes;
        wm->wins[i].proc = (uint16_t)(args->gpr[6] & 0xFFFFu);
        wm->wins[i].next = -1;
        /* Pascal title (may be empty) into the host slot. */
        {
            const uint8_t *d = wm->ppc->mem.data;
            uint32_t tp = args->gpr[3];
            if (qd_range(wm->ppc, tp, 1u)) {
                unsigned len = d[tp];
                if (len > 63u)
                    len = 63u;
                if (len > 0u && qd_range(wm->ppc, tp + 1u, len))
                    memcpy(wm->wins[i].title, d + tp + 1u, len);
                wm->wins[i].title[len] = '\0';
            }
        }
        wm_link_behind(wm, i, wm_slot_of(wm, args->gpr[7]));
        wm_write_record(wm, i);
        wm_refresh_links(wm);
        if (wm->wins[i].visible)
            wm_composite(wm);
        return record;
    }
    case QRX_TB_WM_DISPOSE_WINDOW:
    case QRX_TB_WM_CLOSE_WINDOW:
        return (uint32_t)qrx_wm_close_window(wm, args->gpr[1]);
    case QRX_TB_WM_SHOW_WINDOW:
        return (uint32_t)qrx_wm_show_window(wm, args->gpr[1]);
    case QRX_TB_WM_HIDE_WINDOW:
        return (uint32_t)qrx_wm_hide_window(wm, args->gpr[1]);
    case QRX_TB_WM_SELECT_WINDOW:
    case QRX_TB_WM_BRING_TO_FRONT:
        slot = wm_slot_of(wm, args->gpr[1]);
        if (slot < 0)
            return (uint32_t)QRX_TB_W_WRONG_KIND;
        wm_unlink(wm, slot);
        wm_link_front(wm, slot);
        wm_refresh_links(wm);
        wm_composite(wm);
        return 0u;
    case QRX_TB_WM_SEND_BEHIND:
        slot = wm_slot_of(wm, args->gpr[1]);
        if (slot < 0)
            return (uint32_t)QRX_TB_W_WRONG_KIND;
        wm_unlink(wm, slot);
        {
            int behind = wm_slot_of(wm, args->gpr[2]);
            if (behind < 0 && args->gpr[2] != 0u)
                return (uint32_t)QRX_TB_W_WRONG_KIND;
            if (behind < 0) {
                int tail = -1, j;
                for (j = wm->front; j >= 0; j = wm->wins[j].next)
                    tail = j;
                if (tail < 0)
                    wm->front = slot;
                else
                    wm->wins[tail].next = slot;
                wm->wins[slot].next = -1;
            } else {
                wm_link_behind(wm, slot, behind);
            }
        }
        wm_refresh_links(wm);
        wm_composite(wm);
        return 0u;
    case QRX_TB_WM_MOVE_WINDOW:
        slot = wm_slot_of(wm, args->gpr[1]);
        if (slot < 0)
            return (uint32_t)QRX_TB_W_WRONG_KIND;
        {
            int16_t h = (int16_t)(args->gpr[2] >> 16);
            int16_t v = (int16_t)args->gpr[2];
            int16_t w = (int16_t)(wm->wins[slot].right - wm->wins[slot].left);
            int16_t hh = (int16_t)(wm->wins[slot].bottom - wm->wins[slot].top);
            wm->wins[slot].left = h;
            wm->wins[slot].top = v;
            wm->wins[slot].right = (int16_t)(h + w);
            wm->wins[slot].bottom = (int16_t)(v + hh);
        }
        wm_write_record(wm, slot);
        wm_composite(wm);
        return 0u;
    case QRX_TB_WM_SIZE_WINDOW:
        slot = wm_slot_of(wm, args->gpr[1]);
        if (slot < 0)
            return (uint32_t)QRX_TB_W_WRONG_KIND;
        {
            int16_t w = (int16_t)(args->gpr[2] >> 16);
            int16_t h = (int16_t)args->gpr[2];
            if (w <= 0 || h <= 0)
                return (uint32_t)QRX_TB_PARAM_ERR;
            wm->wins[slot].right = (int16_t)(wm->wins[slot].left + w);
            wm->wins[slot].bottom = (int16_t)(wm->wins[slot].top + h);
        }
        wm_write_record(wm, slot);
        wm_composite(wm);
        return 0u;
    case QRX_TB_WM_SET_TITLE:
        slot = wm_slot_of(wm, args->gpr[1]);
        if (slot < 0)
            return (uint32_t)QRX_TB_W_WRONG_KIND;
        {
            const uint8_t *d = wm->ppc->mem.data;
            uint32_t tp = args->gpr[2];
            unsigned len = 0u;
            if (!qd_range(wm->ppc, tp, 1u))
                return (uint32_t)QRX_TB_PARAM_ERR;
            len = d[tp];
            if (len > 63u)
                len = 63u;
            if (len > 0u && !qd_range(wm->ppc, tp + 1u, len))
                return (uint32_t)QRX_TB_PARAM_ERR;
            if (len > 0u)
                memcpy(wm->wins[slot].title, d + tp + 1u, len);
            wm->wins[slot].title[len] = '\0';
            wm_write_record(wm, slot);
        }
        return 0u;
    case QRX_TB_WM_GET_TITLE:
        slot = wm_slot_of(wm, args->gpr[1]);
        if (slot < 0)
            return (uint32_t)QRX_TB_W_WRONG_KIND;
        {
            uint32_t tp = args->gpr[2];
            unsigned len = (unsigned)strlen(wm->wins[slot].title);
            if (!qd_range(wm->ppc, tp, 1u + len))
                return (uint32_t)QRX_TB_PARAM_ERR;
            wm->ppc->mem.data[tp] = (uint8_t)len;
            if (len > 0u)
                memcpy(&wm->ppc->mem.data[tp + 1u], wm->wins[slot].title, len);
        }
        return 0u;
    case QRX_TB_WM_GET_REFCON:
        slot = wm_slot_of(wm, args->gpr[1]);
        if (slot < 0)
            return (uint32_t)QRX_TB_W_WRONG_KIND;
        return wm->wins[slot].refcon;
    case QRX_TB_WM_SET_REFCON:
        slot = wm_slot_of(wm, args->gpr[1]);
        if (slot < 0)
            return (uint32_t)QRX_TB_W_WRONG_KIND;
        wm->wins[slot].refcon = args->gpr[2];
        wm_write_record(wm, slot);
        return 0u;
    case QRX_TB_WM_BEGIN_UPDATE:
    case QRX_TB_WM_INVAL_RECT:
        slot = wm_slot_of(wm, args->gpr[1]);
        if (slot < 0)
            return (uint32_t)QRX_TB_W_WRONG_KIND;
        return 0u;
    case QRX_TB_WM_END_UPDATE:
        slot = wm_slot_of(wm, args->gpr[1]);
        if (slot < 0)
            return (uint32_t)QRX_TB_W_WRONG_KIND;
        wm_composite(wm);
        return 0u;
    case QRX_TB_WM_FIND_WINDOW:
        /* r4 = Point*, r5 = WindowPtr* (receives the hit window or 0). */
        if (!qd_range(wm->ppc, args->gpr[1], 4u) ||
            !qd_range(wm->ppc, args->gpr[2], 4u))
            return (uint32_t)QRX_TB_PARAM_ERR;
        {
            int16_t h = qd_get16(wm->ppc, args->gpr[1]);
            int16_t v = qd_get16(wm->ppc, args->gpr[1] + 2u);
            int i;
            uint32_t hit = 0u;
            for (i = wm->front; i >= 0; i = wm->wins[i].next) {
                if (wm->wins[i].used && wm->wins[i].visible &&
                    h >= wm->wins[i].left && h < wm->wins[i].right &&
                    v >= wm->wins[i].top && v < wm->wins[i].bottom) {
                    hit = wm->wins[i].record;
                    break;
                }
            }
            qd_put32(wm->ppc, args->gpr[2], hit);
        }
        return 0u;
    default:
        return QRX_MM_UNKNOWN;
    }
}

/* ---- Menu Manager -------------------------------------------------------- */
static int menu_slot_of(const qrx_menu_mgr_t *mmgr, uint32_t handle)
{
    int i;

    for (i = 0; i < (int)QRX_MENU_MAX_MENUS; i++) {
        if (mmgr->menus[i].used && mmgr->menus[i].handle == handle)
            return i;
    }
    return -1;
}

static uint32_t menu_record_addr(const qrx_menu_mgr_t *mmgr, int slot)
{
    return qd_get32(mmgr->ppc, mmgr->menus[slot].handle);
}

static void menu_write_record(qrx_menu_mgr_t *mmgr, int slot)
{
    qrx_menu_t *m = &mmgr->menus[slot];
    uint32_t base = menu_record_addr(mmgr, slot);
    uint32_t flags = 0u;
    int i;

    if (base == 0u || !qd_range(mmgr->ppc, base, 18u))
        return;
    qd_put16(mmgr->ppc, base, m->id);
    qd_put16(mmgr->ppc, base + 2u, 0);   /* menuWidth */
    qd_put16(mmgr->ppc, base + 4u, 0);   /* menuHeight */
    for (i = 0; i < (int)m->nitems && i < 32; i++) {
        if (m->enabled[i])
            flags |= 1u << i;
    }
    qd_put32(mmgr->ppc, base + 10u, flags);  /* enableFlags */
}

static void menu_refresh_data(qrx_menu_mgr_t *mmgr, int slot)
{
    qrx_menu_t *m = &mmgr->menus[slot];
    uint32_t base = menu_record_addr(mmgr, slot);
    uint32_t data_handle;
    if (base == 0u || !qd_range(mmgr->ppc, base, 18u))
        return;
    data_handle = qd_get32(mmgr->ppc, base + 14u);
    uint32_t need = 0u;
    int i;

    for (i = 0; i < (int)m->nitems; i++)
        need += 1u + (uint32_t)strlen(m->text[i]);
    if (data_handle != 0u) {
        uint32_t size = 0u;
        if (qrx_mm_get_handle_size(mmgr->mm, data_handle, &size) != QRX_MM_OK)
            data_handle = 0u;
        else if (size < need) {
            qrx_mm_dispose_handle(mmgr->mm, data_handle);
            data_handle = 0u;
        }
    }
    if (data_handle == 0u) {
        if (qrx_mm_new_handle(mmgr->mm, need > 0u ? need : 1u, 1,
                              &data_handle) != QRX_MM_OK)
            return;
        qd_put32(mmgr->ppc, base + 14u, data_handle);
    }
    {
        uint32_t cell = qd_get32(mmgr->ppc, data_handle);
        uint32_t at = cell;
        for (i = 0; i < (int)m->nitems; i++) {
            size_t len = strlen(m->text[i]);
            if (!qd_range(mmgr->ppc, at, 1u + (uint32_t)len))
                break;
            mmgr->ppc->mem.data[at] = (uint8_t)len;
            if (len > 0u)
                memcpy(&mmgr->ppc->mem.data[at + 1u], m->text[i], len);
            at += 1u + (uint32_t)len;
        }
    }
}

void qrx_menu_init(qrx_menu_mgr_t *mmgr, qrx_ppc_t *ppc, qrx_mm_t *mm)
{
    if (!mmgr)
        return;
    memset(mmgr, 0, sizeof(*mmgr));
    mmgr->ppc = ppc;
    mmgr->mm = mm;
}

uint32_t qrx_menu_new(qrx_menu_mgr_t *mmgr, int16_t id, const char *title)
{
    int i;
    uint32_t handle = 0u;

    (void)title;
    if (!mmgr || !mmgr->mm)
        return 0u;
    for (i = 0; i < (int)QRX_MENU_MAX_MENUS; i++) {
        if (!mmgr->menus[i].used)
            break;
    }
    if (i == (int)QRX_MENU_MAX_MENUS)
        return 0u;
    if (qrx_mm_new_handle(mmgr->mm, 18u, 1, &handle) != QRX_MM_OK)
        return 0u;
    memset(&mmgr->menus[i], 0, sizeof(mmgr->menus[i]));
    mmgr->menus[i].used = 1;
    mmgr->menus[i].handle = handle;
    mmgr->menus[i].id = id;
    menu_write_record(mmgr, i);
    qd_put32(mmgr->ppc, menu_record_addr(mmgr, i) + 14u, 0u); /* menuData */
    return handle;
}

int32_t qrx_menu_append(qrx_menu_mgr_t *mmgr, uint32_t menu, const char *text)
{
    int slot = menu_slot_of(mmgr, menu);
    qrx_menu_t *m;
    size_t len;

    if (slot < 0 || !text)
        return QRX_TB_PARAM_ERR;
    m = &mmgr->menus[slot];
    if (m->nitems >= QRX_MENU_MAX_ITEMS)
        return QRX_TB_PARAM_ERR;
    len = strlen(text);
    if (len > QRX_MENU_MAX_TEXT)
        len = QRX_MENU_MAX_TEXT;
    memcpy(m->text[m->nitems], text, len);
    m->text[m->nitems][len] = '\0';
    m->enabled[m->nitems] = 1;
    m->checked[m->nitems] = 0;
    m->nitems++;
    menu_write_record(mmgr, slot);
    menu_refresh_data(mmgr, slot);
    return QRX_TB_NO_ERR;
}

uint32_t qrx_menu_count_items(const qrx_menu_mgr_t *mmgr, uint32_t menu)
{
    int slot = menu_slot_of(mmgr, menu);

    return slot < 0 ? 0u : mmgr->menus[slot].nitems;
}

uint32_t qrx_menu_dispatch(qrx_menu_mgr_t *mmgr, const qrx_mix_args_t *args)
{
    uint32_t sel;
    int slot;
    int16_t item;

    if (!mmgr || !args)
        return QRX_MM_UNKNOWN;
    sel = args->gpr[0];
    switch (sel) {
    case QRX_TB_MENU_INIT:
    case QRX_TB_MENU_DRAW_BAR:
        return 0u;
    case QRX_TB_MENU_NEW: {
        /* r4 = menuID (int16), r5 = title Pascal*. */
        char title[256];
        const uint8_t *d = mmgr->ppc->mem.data;
        uint32_t tp = args->gpr[2];

        title[0] = '\0';
        if (qd_range(mmgr->ppc, tp, 1u)) {
            unsigned len = d[tp];
            if (len > 255u)
                len = 255u;
            if (len > 0u && qd_range(mmgr->ppc, tp + 1u, len))
                memcpy(title, d + tp + 1u, len);
            title[len] = '\0';
        }
        return qrx_menu_new(mmgr, (int16_t)(args->gpr[1] & 0xFFFFu), title);
    }
    case QRX_TB_MENU_DISPOSE:
        slot = menu_slot_of(mmgr, args->gpr[1]);
        if (slot < 0)
            return (uint32_t)QRX_TB_PARAM_ERR;
        {
            uint32_t data_handle = qd_get32(
                mmgr->ppc, menu_record_addr(mmgr, slot) + 14u);
            unsigned i, j;
            if (data_handle != 0u)
                qrx_mm_dispose_handle(mmgr->mm, data_handle);
            qrx_mm_dispose_handle(mmgr->mm, mmgr->menus[slot].handle);
            for (i = 0u, j = 0u; i < mmgr->nbar; i++) {
                if (mmgr->bar[i] != mmgr->menus[slot].handle)
                    mmgr->bar[j++] = mmgr->bar[i];
            }
            mmgr->nbar = (uint16_t)j;
            memset(&mmgr->menus[slot], 0, sizeof(mmgr->menus[slot]));
        }
        return 0u;
    case QRX_TB_MENU_APPEND:
        slot = menu_slot_of(mmgr, args->gpr[1]);
        if (slot < 0)
            return (uint32_t)QRX_TB_PARAM_ERR;
        {
            const uint8_t *d = mmgr->ppc->mem.data;
            uint32_t tp = args->gpr[2];
            char text[256];
            unsigned len;
            if (!qd_range(mmgr->ppc, tp, 1u))
                return (uint32_t)QRX_TB_PARAM_ERR;
            len = d[tp];
            if (len > 255u)
                return (uint32_t)QRX_TB_PARAM_ERR;
            if (len > 0u && !qd_range(mmgr->ppc, tp + 1u, len))
                return (uint32_t)QRX_TB_PARAM_ERR;
            if (len > 0u)
                memcpy(text, d + tp + 1u, len);
            text[len] = '\0';
            return (uint32_t)qrx_menu_append(mmgr, args->gpr[1], text);
        }
    case QRX_TB_MENU_COUNT_ITEMS:
        return qrx_menu_count_items(mmgr, args->gpr[1]);
    case QRX_TB_MENU_GET_ITEM_TEXT:
        slot = menu_slot_of(mmgr, args->gpr[1]);
        item = (int16_t)(args->gpr[2] & 0xFFFFu);
        if (slot < 0 || item < 1 ||
            item > (int16_t)mmgr->menus[slot].nitems)
            return (uint32_t)QRX_TB_PARAM_ERR;
        {
            uint32_t tp = args->gpr[3];
            size_t len = strlen(mmgr->menus[slot].text[item - 1]);
            if (!qd_range(mmgr->ppc, tp, 1u + (uint32_t)len))
                return (uint32_t)QRX_TB_PARAM_ERR;
            mmgr->ppc->mem.data[tp] = (uint8_t)len;
            if (len > 0u)
                memcpy(&mmgr->ppc->mem.data[tp + 1u],
                       mmgr->menus[slot].text[item - 1], len);
        }
        return 0u;
    case QRX_TB_MENU_SET_ITEM_TEXT:
        slot = menu_slot_of(mmgr, args->gpr[1]);
        item = (int16_t)(args->gpr[2] & 0xFFFFu);
        if (slot < 0 || item < 1 ||
            item > (int16_t)mmgr->menus[slot].nitems)
            return (uint32_t)QRX_TB_PARAM_ERR;
        {
            const uint8_t *d = mmgr->ppc->mem.data;
            uint32_t tp = args->gpr[3];
            unsigned len;
            if (!qd_range(mmgr->ppc, tp, 1u))
                return (uint32_t)QRX_TB_PARAM_ERR;
            len = d[tp];
            if (len > QRX_MENU_MAX_TEXT)
                return (uint32_t)QRX_TB_PARAM_ERR;
            if (len > 0u && !qd_range(mmgr->ppc, tp + 1u, len))
                return (uint32_t)QRX_TB_PARAM_ERR;
            if (len > 0u)
                memcpy(mmgr->menus[slot].text[item - 1], d + tp + 1u, len);
            mmgr->menus[slot].text[item - 1][len] = '\0';
            menu_write_record(mmgr, slot);
            menu_refresh_data(mmgr, slot);
        }
        return 0u;
    case QRX_TB_MENU_ENABLE_ITEM:
    case QRX_TB_MENU_DISABLE_ITEM:
        slot = menu_slot_of(mmgr, args->gpr[1]);
        item = (int16_t)(args->gpr[2] & 0xFFFFu);
        if (slot < 0 || item < 1 ||
            item > (int16_t)mmgr->menus[slot].nitems)
            return (uint32_t)QRX_TB_PARAM_ERR;
        mmgr->menus[slot].enabled[item - 1] =
            (uint8_t)(sel == QRX_TB_MENU_ENABLE_ITEM ? 1 : 0);
        menu_write_record(mmgr, slot);
        return 0u;
    case QRX_TB_MENU_CHECK_ITEM:
        slot = menu_slot_of(mmgr, args->gpr[1]);
        item = (int16_t)(args->gpr[2] & 0xFFFFu);
        if (slot < 0 || item < 1 ||
            item > (int16_t)mmgr->menus[slot].nitems)
            return (uint32_t)QRX_TB_PARAM_ERR;
        mmgr->menus[slot].checked[item - 1] = (uint8_t)(args->gpr[3] != 0u);
        return 0u;
    case QRX_TB_MENU_DELETE_ITEM:
        slot = menu_slot_of(mmgr, args->gpr[1]);
        item = (int16_t)(args->gpr[2] & 0xFFFFu);
        if (slot < 0 || item < 1 ||
            item > (int16_t)mmgr->menus[slot].nitems)
            return (uint32_t)QRX_TB_PARAM_ERR;
        {
            qrx_menu_t *m = &mmgr->menus[slot];
            int i;
            for (i = item - 1; i + 1 < (int)m->nitems; i++) {
                memcpy(m->text[i], m->text[i + 1], sizeof(m->text[i]));
                m->enabled[i] = m->enabled[i + 1];
                m->checked[i] = m->checked[i + 1];
            }
            m->nitems--;
            menu_write_record(mmgr, slot);
            menu_refresh_data(mmgr, slot);
        }
        return 0u;
    case QRX_TB_MENU_CLEAR_BAR:
        mmgr->nbar = 0u;
        return 0u;
    case QRX_TB_MENU_INSERT:
        /* r4 = menu, r5 = before (0 = at the end). */
        if (menu_slot_of(mmgr, args->gpr[1]) < 0)
            return (uint32_t)QRX_TB_PARAM_ERR;
        {
            unsigned i, j, at = mmgr->nbar;
            for (i = 0u; i < mmgr->nbar; i++) {
                if (mmgr->bar[i] == args->gpr[1]) {
                    for (j = i; j + 1u < mmgr->nbar; j++)
                        mmgr->bar[j] = mmgr->bar[j + 1u];
                    mmgr->nbar--;
                    break;
                }
            }
            if (args->gpr[2] != 0u) {
                for (i = 0u; i < mmgr->nbar; i++) {
                    if (mmgr->bar[i] == args->gpr[2]) {
                        at = i;
                        break;
                    }
                }
            }
            if (mmgr->nbar >= QRX_MENU_MAX_MENUS)
                return (uint32_t)QRX_TB_PARAM_ERR;
            for (j = mmgr->nbar; j > at; j--)
                mmgr->bar[j] = mmgr->bar[j - 1u];
            mmgr->bar[at] = args->gpr[1];
            mmgr->nbar++;
        }
        return 0u;
    case QRX_TB_MENU_DELETE:
        /* r4 = menu: remove it from the bar. */
        {
            unsigned i, j;
            int found = 0;
            for (i = 0u, j = 0u; i < mmgr->nbar; i++) {
                if (mmgr->bar[i] == args->gpr[1]) {
                    found = 1;
                    continue;
                }
                mmgr->bar[j++] = mmgr->bar[i];
            }
            if (!found)
                return (uint32_t)QRX_TB_PARAM_ERR;
            mmgr->nbar = (uint16_t)j;
        }
        return 0u;
    case QRX_TB_MENU_GET_BAR:
        return mmgr->nbar;    /* entry count; handles via the C API */
    case QRX_TB_MENU_SET_BAR:
        /* r4 = bar handle whose data is an array of MenuHandles. */
        {
            uint32_t cell = args->gpr[1];
            uint32_t size = 0u;
            uint32_t data;
            if (qrx_mm_get_handle_size(mmgr->mm, cell, &size) != QRX_MM_OK)
                return (uint32_t)QRX_TB_PARAM_ERR;
            if (size / 4u > QRX_MENU_MAX_MENUS)
                return (uint32_t)QRX_TB_PARAM_ERR;
            data = qd_get32(mmgr->ppc, cell);
            mmgr->nbar = 0u;
            {
                uint32_t i;
                for (i = 0u; i < size / 4u; i++) {
                    uint32_t h = qd_get32(mmgr->ppc, data + 4u * i);
                    if (menu_slot_of(mmgr, h) < 0)
                        return (uint32_t)QRX_TB_PARAM_ERR;
                    mmgr->bar[mmgr->nbar++] = h;
                }
            }
        }
        return 0u;
    default:
        return QRX_MM_UNKNOWN;
    }
}

/* ---- Dialog Manager -------------------------------------------------------- */
void qrx_dm_init(qrx_dm_t *dm, qrx_ppc_t *ppc)
{
    if (!dm)
        return;
    memset(dm, 0, sizeof(*dm));
    dm->ppc = ppc;
    dm->last_item = 1;
}

uint32_t qrx_dm_dispatch(qrx_dm_t *dm, const qrx_mix_args_t *args)
{
    uint32_t sel;

    if (!dm || !args)
        return QRX_MM_UNKNOWN;
    sel = args->gpr[0];
    switch (sel) {
    case QRX_TB_DM_INIT:
        return 0u;
    case QRX_TB_DM_ALERT:
    case QRX_TB_DM_STOP_ALERT:
    case QRX_TB_DM_NOTE_ALERT:
    case QRX_TB_DM_CAUTION_ALERT:
        /* r4 = alert resource id; no event loop, dismiss with item 1. */
        dm->last_alert_id = (uint16_t)(args->gpr[1] & 0xFFFFu);
        dm->last_item = 1;
        return 1u;
    case QRX_TB_DM_MODAL_DIALOG:
        /* r4 = dialog, r5 = filter proc; dismiss at once with item 1. */
        dm->last_item = 1;
        return 1u;
    default:
        return QRX_MM_UNKNOWN;
    }
}

/* ---- Text/Font Manager ------------------------------------------------- */
static int text_scale(const qrx_font_t *ft)
{
    int scale;

    if (!ft || ft->size <= 0)
        return 1;
    scale = ft->size / 9;
    if (scale < 1)
        scale = 1;
    if (scale > 4)
        scale = 4;
    return scale;
}

void qrx_font_init(qrx_font_t *ft, qrx_ppc_t *ppc, qrx_qd_t *qd)
{
    if (!ft)
        return;
    memset(ft, 0, sizeof(*ft));
    ft->ppc = ppc;
    ft->qd = qd;
    ft->font = QRX_TEXT_SYSTEM_FONT;
    ft->size = QRX_TEXT_SYSTEM_SIZE;
    ft->face = 0;
    ft->mode = 1;            /* txMode srcOr, as QuickDraw initialises it */
    ft->space_extra = 0;
}

int16_t qrx_text_advance(const qrx_font_t *ft)
{
    return (int16_t)(8 * text_scale(ft));
}

int16_t qrx_text_ascent(const qrx_font_t *ft)
{
    return (int16_t)(7 * text_scale(ft));
}

int16_t qrx_text_descent(const qrx_font_t *ft)
{
    return (int16_t)(1 * text_scale(ft));
}

int16_t qrx_text_char_advance(const qrx_font_t *ft)
{
    int32_t adv = qrx_text_advance(ft);

    if (ft)
        adv += ft->space_extra >> 16;
    if (adv < 0)
        adv = 0;
    if (adv > 32767)
        adv = 32767;
    return (int16_t)adv;
}

const uint8_t *qrx_text_glyph(uint32_t ch)
{
    if (ch < QRX_FONT_FIRST || ch > QRX_FONT_LAST)
        return NULL;
    return qrx_glyph8x8[ch - QRX_FONT_FIRST];
}

/* Draw one 8x8 glyph with its top row seven scale steps above the baseline. */
static void text_draw_glyph(qrx_font_t *ft, uint32_t ch, int16_t h,
                            int16_t baseline)
{
    const uint8_t *glyph = qrx_text_glyph(ch);
    qd_bitmap_t bm;
    int scale, row, col, dy, dx;

    if (!glyph || !ft || !ft->qd)
        return;
    if (!qd_read_bitmap(ft->ppc, ft->qd->port, &bm))
        return;
    scale = text_scale(ft);
    for (row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (col = 0; col < 8; col++) {
            int on = (bits >> col) & 1;
            int32_t px = (int32_t)h + col * scale;
            int32_t py = (int32_t)baseline - 7 * scale + row * scale;

            if (ft->mode != 0 && !on)
                continue;
            for (dy = 0; dy < scale; dy++) {
                for (dx = 0; dx < scale; dx++) {
                    int16_t x = (int16_t)(px + dx);
                    int16_t y = (int16_t)(py + dy);
                    if (!qd_in_bounds(&bm, x, y))
                        continue;
                    switch (ft->mode) {
                    case 0:
                        qd_set_pixel(ft->ppc, &bm, x, y, on);
                        break;
                    case 1:
                        qd_set_pixel(ft->ppc, &bm, x, y, 1);
                        break;
                    case 2:
                        qd_set_pixel(ft->ppc, &bm, x, y,
                                     qd_get_pixel(ft->ppc, &bm, x, y) ^ 1);
                        break;
                    default:
                        qd_set_pixel(ft->ppc, &bm, x, y, 0);
                        break;
                    }
                }
            }
        }
    }
}

void qrx_text_draw(qrx_font_t *ft, const uint8_t *bytes, uint32_t count)
{
    uint32_t i;

    if (!ft || !ft->qd || !bytes)
        return;
    for (i = 0u; i < count; i++) {
        int16_t h = ft->qd->pen_h;
        text_draw_glyph(ft, bytes[i], h, ft->qd->pen_v);
        ft->qd->pen_h = (int16_t)(h + qrx_text_char_advance(ft));
    }
}

static int32_t text_width(const qrx_font_t *ft, uint32_t count)
{
    int32_t width;

    if (!ft || count == 0u)
        return 0;
    width = (int32_t)count * qrx_text_char_advance(ft);
    if (width > 32767)
        width = 32767;
    return width;
}

/* Read a Pascal string into a C buffer; 0 when unreadable. */
static int text_read_pstring(const qrx_ppc_t *ppc, uint32_t addr, char *out,
                             size_t cap)
{
    unsigned len, i;

    if (!qd_range(ppc, addr, 1u))
        return 0;
    len = ppc->mem.data[addr];
    if (!qd_range(ppc, addr + 1u, len))
        return 0;
    if (cap < (size_t)len + 1u)
        return 0;
    for (i = 0u; i < len; i++)
        out[i] = (char)ppc->mem.data[addr + 1u + i];
    out[len] = '\0';
    return 1;
}

static int text_name_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

uint32_t qrx_text_dispatch(qrx_font_t *ft, const qrx_mix_args_t *args)
{
    uint32_t sel;
    char name[256];

    if (!ft || !args)
        return QRX_MM_UNKNOWN;
    sel = args->gpr[0];
    switch (sel) {
    case QRX_TB_TEXT_INIT_FONTS:
        qrx_font_init(ft, ft->ppc, ft->qd);
        return 0u;
    case QRX_TB_TEXT_FONT:
        ft->font = (int16_t)args->gpr[1];
        return 0u;
    case QRX_TB_TEXT_FACE:
        ft->face = (int16_t)args->gpr[1];
        return 0u;
    case QRX_TB_TEXT_MODE:
        if ((int16_t)args->gpr[1] < 0 || (int16_t)args->gpr[1] > 3)
            return (uint32_t)QRX_TB_PARAM_ERR;
        ft->mode = (int16_t)args->gpr[1];
        return 0u;
    case QRX_TB_TEXT_SIZE:
        if ((int16_t)args->gpr[1] <= 0 ||
            (int16_t)args->gpr[1] > QRX_TEXT_MAX_SIZE)
            return (uint32_t)QRX_TB_PARAM_ERR;
        ft->size = (int16_t)args->gpr[1];
        return 0u;
    case QRX_TB_TEXT_SPACE_EXTRA:
        ft->space_extra = (int32_t)args->gpr[1];
        return 0u;
    case QRX_TB_TEXT_CHAR_WIDTH:
        return (uint32_t)(int16_t)qrx_text_char_advance(ft);
    case QRX_TB_TEXT_DRAW_CHAR:
        {
            uint8_t ch = (uint8_t)args->gpr[1];
            qrx_text_draw(ft, &ch, 1u);
        }
        return 0u;
    case QRX_TB_TEXT_DRAW_STRING:
        {
            uint32_t addr = args->gpr[1];
            unsigned len;
            if (!qd_range(ft->ppc, addr, 1u))
                return (uint32_t)QRX_TB_PARAM_ERR;
            len = ft->ppc->mem.data[addr];
            if (len == 0u)
                return 0u;
            if (!qd_range(ft->ppc, addr + 1u, len))
                return (uint32_t)QRX_TB_PARAM_ERR;
            qrx_text_draw(ft, &ft->ppc->mem.data[addr + 1u], len);
        }
        return 0u;
    case QRX_TB_TEXT_DRAW_TEXT:
        {
            uint32_t addr = args->gpr[1];
            uint32_t first = args->gpr[2];
            uint32_t count = args->gpr[3];
            if (count == 0u)
                return 0u;
            if (!qd_range(ft->ppc, addr + first, count))
                return (uint32_t)QRX_TB_PARAM_ERR;
            qrx_text_draw(ft, &ft->ppc->mem.data[addr + first], count);
        }
        return 0u;
    case QRX_TB_TEXT_STRING_WIDTH:
        {
            uint32_t addr = args->gpr[1];
            unsigned len;
            if (!qd_range(ft->ppc, addr, 1u))
                return (uint32_t)QRX_TB_PARAM_ERR;
            len = ft->ppc->mem.data[addr];
            if (len != 0u && !qd_range(ft->ppc, addr + 1u, len))
                return (uint32_t)QRX_TB_PARAM_ERR;
            return (uint32_t)text_width(ft, len);
        }
    case QRX_TB_TEXT_WIDTH:
        {
            uint32_t addr = args->gpr[1];
            uint32_t first = args->gpr[2];
            uint32_t count = args->gpr[3];
            if (count == 0u)
                return 0u;
            if (!qd_range(ft->ppc, addr + first, count))
                return (uint32_t)QRX_TB_PARAM_ERR;
            return (uint32_t)text_width(ft, count);
        }
    case QRX_TB_TEXT_FONT_INFO:
        {
            uint32_t addr = args->gpr[1];
            if (!qd_range(ft->ppc, addr, 8u))
                return (uint32_t)QRX_TB_PARAM_ERR;
            qd_put16(ft->ppc, addr, qrx_text_ascent(ft));
            qd_put16(ft->ppc, addr + 2u, qrx_text_descent(ft));
            qd_put16(ft->ppc, addr + 4u, qrx_text_advance(ft));
            qd_put16(ft->ppc, addr + 6u, 0);
        }
        return 0u;
    case QRX_TB_TEXT_GET_FONT_NAME:
        {
            uint32_t addr = args->gpr[2];
            const char *n;
            unsigned len, i;
            if ((int16_t)args->gpr[1] != QRX_TEXT_SYSTEM_FONT)
                return (uint32_t)QRX_TB_PARAM_ERR;
            n = "Chicago";
            len = (unsigned)strlen(n);
            if (!qd_range(ft->ppc, addr, len + 1u))
                return (uint32_t)QRX_TB_PARAM_ERR;
            ft->ppc->mem.data[addr] = (uint8_t)len;
            for (i = 0u; i < len; i++)
                ft->ppc->mem.data[addr + 1u + i] = (uint8_t)n[i];
        }
        return 0u;
    case QRX_TB_TEXT_GET_FNUM:
        {
            uint32_t addr = args->gpr[2];
            int16_t family = QRX_TEXT_SYSTEM_FONT;
            if (!text_read_pstring(ft->ppc, args->gpr[1], name, sizeof(name)))
                return (uint32_t)QRX_TB_PARAM_ERR;
            if (!text_name_eq(name, "chicago") && !text_name_eq(name, "system"))
                family = QRX_TEXT_SYSTEM_FONT; /* only one family exists */
            if (!qd_range(ft->ppc, addr, 2u))
                return (uint32_t)QRX_TB_PARAM_ERR;
            qd_put16(ft->ppc, addr, family);
        }
        return 0u;
    default:
        return QRX_MM_UNKNOWN;
    }
}

/* ---- Event Manager ----------------------------------------------------- */
static uint32_t ev_now(qrx_ev_t *ev)
{
    if (!ev)
        return 0u;
    if (ev->tick_fn)
        return ev->tick_fn(ev->tick_ctx);
    return ev->ticks;
}

void qrx_ev_init(qrx_ev_t *ev, qrx_ppc_t *ppc)
{
    if (!ev)
        return;
    memset(ev, 0, sizeof(*ev));
    ev->ppc = ppc;
}

void qrx_ev_set_tick_source(qrx_ev_t *ev, uint32_t (*fn)(void *), void *ctx)
{
    if (!ev)
        return;
    ev->tick_fn = fn;
    ev->tick_ctx = ctx;
}

void qrx_ev_set_ticks(qrx_ev_t *ev, uint32_t ticks)
{
    if (ev)
        ev->ticks = ticks;
}

void qrx_ev_advance_ticks(qrx_ev_t *ev, uint32_t delta)
{
    if (ev)
        ev->ticks += delta;
}

void qrx_ev_set_mouse(qrx_ev_t *ev, int16_t v, int16_t h, int down)
{
    if (!ev)
        return;
    ev->mouse_v = v;
    ev->mouse_h = h;
    ev->mouse_down = down ? 1 : 0;
}

void qrx_ev_set_key(qrx_ev_t *ev, unsigned code, int down)
{
    if (!ev || code > 127u)
        return;
    if (down)
        ev->keymap[code / 8u] |= (uint8_t)(0x80u >> (code % 8u));
    else
        ev->keymap[code / 8u] &= (uint8_t)~(0x80u >> (code % 8u));
}

void qrx_ev_set_modifiers(qrx_ev_t *ev, uint16_t modifiers)
{
    if (ev)
        ev->modifiers = modifiers;
}

int qrx_ev_post(qrx_ev_t *ev, int16_t what, int32_t message)
{
    qrx_event_t *slot;

    if (!ev || what == QRX_EV_NULL || ev->count >= QRX_EV_QUEUE)
        return 0;
    slot = &ev->queue[(ev->head + ev->count) % QRX_EV_QUEUE];
    slot->what = what;
    slot->message = message;
    slot->when = (int32_t)ev_now(ev);
    slot->where_v = ev->mouse_v;
    slot->where_h = ev->mouse_h;
    slot->modifiers = ev->modifiers;
    ev->count++;
    return 1;
}

unsigned qrx_ev_count(const qrx_ev_t *ev)
{
    return ev ? ev->count : 0u;
}

static int ev_match(uint16_t mask, int16_t what)
{
    if (mask == 0)
        return 1;            /* documented: 0 means everyEventMask */
    if (what <= 0 || what > 15)
        return 0;
    return (mask & (uint16_t)(1u << what)) != 0;
}

static void ev_write_record(qrx_ev_t *ev, uint32_t addr,
                            const qrx_event_t *record)
{
    qd_put16(ev->ppc, addr, record->what);
    qd_put32(ev->ppc, addr + 2u, (uint32_t)record->message);
    qd_put32(ev->ppc, addr + 6u, (uint32_t)record->when);
    qd_put16(ev->ppc, addr + 10u, record->where_v);
    qd_put16(ev->ppc, addr + 12u, record->where_h);
    qd_put16(ev->ppc, addr + 14u, (int16_t)record->modifiers);
}

/* Return the oldest matching event, optionally removing it, and write the
 * record (or a nullEvent) to guest memory. */
static uint32_t ev_take(qrx_ev_t *ev, uint16_t mask, uint32_t addr, int remove)
{
    unsigned i;

    for (i = 0u; i < ev->count; i++) {
        unsigned index = (ev->head + i) % QRX_EV_QUEUE;
        qrx_event_t *slot = &ev->queue[index];

        if (!ev_match(mask, slot->what))
            continue;
        ev_write_record(ev, addr, slot);
        if (remove) {
            unsigned j;
            for (j = i; j + 1u < ev->count; j++) {
                unsigned from = (ev->head + j) % QRX_EV_QUEUE;
                unsigned to = (ev->head + j + 1u) % QRX_EV_QUEUE;
                ev->queue[from] = ev->queue[to];
            }
            ev->count--;
        }
        return 1u;
    }
    {
        qrx_event_t null_event;
        null_event.what = QRX_EV_NULL;
        null_event.message = 0;
        null_event.when = (int32_t)ev_now(ev);
        null_event.where_v = ev->mouse_v;
        null_event.where_h = ev->mouse_h;
        null_event.modifiers = ev->modifiers;
        ev_write_record(ev, addr, &null_event);
    }
    return 0u;
}

/* ---- Operating System Utilities: Gestalt ------------------------------ */
/* Gestalt(selector, response): the runner reports the environment a classic
 * PowerPC application expects on MacOS 8, without the Thread Manager.  An
 * unknown selector is gestaltUndefSelectorErr, so the application can take
 * its fallback path instead of the run stopping at a missing trap. */
#define TB_GESTALT_UNDEF_SELECTOR (-5551)

static uint32_t tb_get_zone(const qrx_tb_t *tb)
{
    /* The zone is the header the Memory Manager owns; NewHandle/NewPtr ignore
     * the caller's zone, so a fragment that just passes it back is served. */
    return tb->mm.base;
}

static uint32_t tb_gestalt(const qrx_tb_t *tb, const qrx_mix_args_t *args)
{
    uint32_t selector = args->gpr[1];
    uint32_t response = args->gpr[2];
    uint32_t value;

    if (!qd_range(tb->mm.ppc, response, 4u))
        return (uint32_t)QRX_TB_PARAM_ERR;
    switch (selector) {
    case 0x74686473u:   /* 'thds': Thread Manager attribute bits. */
        value = 0u;
        break;
    case 0x73797376u:   /* 'sysv': system 8.1.0 (hi word major). */
        value = 0x00000810u;
        break;
    case 0x76657273u:   /* 'vers': Gestalt version 1.0. */
        value = 0x00010000u;
        break;
    case 0x6f732020u:   /* 'os  ': OS trap table, absent in this HLE. */
        value = 0u;
        break;
    default:
        return (uint32_t)TB_GESTALT_UNDEF_SELECTOR;
    }
    qd_put32(tb->mm.ppc, response, value);
    return (uint32_t)QRX_TB_NO_ERR;
}

uint32_t qrx_ev_dispatch(qrx_ev_t *ev, const qrx_mix_args_t *args)
{
    uint32_t sel;

    if (!ev || !args)
        return QRX_MM_UNKNOWN;
    sel = args->gpr[0];
    switch (sel) {
    case QRX_TB_EV_TICK_COUNT:
        return ev_now(ev);
    case QRX_TB_EV_BUTTON:
    case QRX_TB_EV_STILL_DOWN:
    case QRX_TB_EV_WAIT_MOUSE_UP:
        return ev->mouse_down ? 1u : 0u;
    case QRX_TB_EV_SYSTEM_TASK:
        return 0u;
    case QRX_TB_EV_GET_MOUSE:
        /* r4 = Point*: v at +0, h at +2. */
        if (!qd_range(ev->ppc, args->gpr[1], 4u))
            return (uint32_t)QRX_TB_PARAM_ERR;
        qd_put16(ev->ppc, args->gpr[1], ev->mouse_v);
        qd_put16(ev->ppc, args->gpr[1] + 2u, ev->mouse_h);
        return 0u;
    case QRX_TB_EV_GET_KEYS:
        if (!qd_range(ev->ppc, args->gpr[1], 16u))
            return (uint32_t)QRX_TB_PARAM_ERR;
        memcpy(&ev->ppc->mem.data[args->gpr[1]], ev->keymap, 16u);
        return 0u;
    case QRX_TB_EV_POST:
        /* r4 = event code, r5 = message. */
        return (uint32_t)qrx_ev_post(ev, (int16_t)args->gpr[1],
                                     (int32_t)args->gpr[2]);
    case QRX_TB_EV_FLUSH:
        {
            /* r4/r5 = fromMask/toMask: remove every event whose type bit
             * falls inside the inclusive mask range. */
            uint32_t from = args->gpr[1] & 0xFFFFu;
            uint32_t to = args->gpr[2] & 0xFFFFu;
            unsigned i = 0u;

            while (i < ev->count) {
                unsigned index = (ev->head + i) % QRX_EV_QUEUE;
                int16_t what = ev->queue[index].what;
                uint32_t bit = (what > 0 && what <= 15) ? (1u << what) : 0u;

                if (bit != 0 && bit >= from && bit <= to) {
                    unsigned j;
                    for (j = i; j + 1u < ev->count; j++) {
                        unsigned a = (ev->head + j) % QRX_EV_QUEUE;
                        unsigned b = (ev->head + j + 1u) % QRX_EV_QUEUE;
                        ev->queue[a] = ev->queue[b];
                    }
                    ev->count--;
                } else {
                    i++;
                }
            }
        }
        return 0u;
    case QRX_TB_EV_GET_NEXT:
    case QRX_TB_EV_WAIT_NEXT:
    case QRX_TB_EV_AVAIL:
        {
            uint32_t addr = args->gpr[2];
            int remove = (sel != QRX_TB_EV_AVAIL);
            if (!qd_range(ev->ppc, addr, QRX_EV_RECORD_SIZE))
                return (uint32_t)QRX_TB_PARAM_ERR;
            return ev_take(ev, (uint16_t)args->gpr[1], addr, remove);
        }
    default:
        return QRX_MM_UNKNOWN;
    }
}

void qrx_tb_init(qrx_tb_t *tb, qrx_ppc_t *ppc, uint32_t zone_base,
                 uint32_t zone_size)
{
    if (!tb)
        return;
    memset(tb, 0, sizeof(*tb));
    qrx_mm_init(&tb->mm, ppc, zone_base, zone_size);
    qrx_rm_init(&tb->rm, &tb->mm);
    qrx_fm_init(&tb->fm, ppc);
    qrx_qd_init(&tb->qd, ppc);
    qrx_wm_init(&tb->wm, ppc, &tb->mm, 0u);
    qrx_menu_init(&tb->menu, ppc, &tb->mm);
    qrx_dm_init(&tb->dm, ppc);
    qrx_font_init(&tb->font, ppc, &tb->qd);
    qrx_ev_init(&tb->ev, ppc);
}

void qrx_tb_set_screen(qrx_tb_t *tb, uint32_t bitmap_addr)
{
    if (!tb)
        return;
    qrx_qd_set_screen(&tb->qd, bitmap_addr);
    tb->wm.screen = bitmap_addr;
}

uint32_t qrx_tb_dispatch(void *ctx, const qrx_mix_args_t *args,
                         qrx_mix_result_t *extra)
{
    qrx_tb_t *tb = (qrx_tb_t *)ctx;
    uint32_t sel;

    (void)extra;
    if (!tb || !args)
        return QRX_MM_UNKNOWN;
    sel = args->gpr[0];
    /* Memory Manager traps first: FreeMem/NewPtr live between the File
     * Manager traps, so route on the exact trap list. */
    if (sel == QRX_TB_FREE_MEM || sel == QRX_TB_DISPOSE_PTR ||
        sel == QRX_TB_SET_PTR_SIZE || sel == QRX_TB_GET_PTR_SIZE ||
        sel == QRX_TB_DISPOSE_HANDLE || sel == QRX_TB_SET_HANDLE_SIZE ||
        sel == QRX_TB_GET_HANDLE_SIZE || sel == QRX_TB_MAX_MEM ||
        sel == QRX_TB_NEW_PTR || sel == QRX_TB_NEW_HANDLE)
        return qrx_mm_dispatch(&tb->mm, args, extra);
    if ((sel >= QRX_TB_FM_OPEN && sel <= QRX_TB_FM_SET_EOF) ||
        sel == QRX_TB_FM_CREATE || sel == QRX_TB_FM_DELETE ||
        sel == QRX_TB_FM_GET_FILE_INFO || sel == QRX_TB_FM_SET_FILE_INFO)
        return qrx_fm_dispatch(&tb->fm, args);
    if ((sel >= QRX_TB_RM_DETACH && sel <= QRX_TB_RM_ERROR) ||
        sel == QRX_TB_RM_DISPATCH)
        return qrx_rm_dispatch(&tb->rm, args);
    if ((sel >= QRX_TB_TEXT_DRAW_CHAR && sel <= QRX_TB_TEXT_SPACE_EXTRA) ||
        sel == QRX_TB_TEXT_INIT_FONTS || sel == QRX_TB_TEXT_GET_FONT_NAME ||
        sel == QRX_TB_TEXT_GET_FNUM)
        return qrx_text_dispatch(&tb->font, args);
    if (sel == QRX_TB_EV_WAIT_NEXT || sel == QRX_TB_EV_POST ||
        sel == QRX_TB_EV_FLUSH || sel == QRX_TB_EV_SYSTEM_TASK ||
        (sel >= QRX_TB_EV_GET_NEXT && sel <= QRX_TB_EV_WAIT_MOUSE_UP))
        return qrx_ev_dispatch(&tb->ev, args);
    if ((sel >= QRX_TB_QD_INIT_PORT && sel <= QRX_TB_QD_PT_IN_RECT) ||
        sel == QRX_TB_QD_SET_PT || sel == QRX_TB_QD_BACK_PAT)
        return qrx_qd_dispatch(&tb->qd, args);
    if ((sel >= QRX_TB_WM_GET_WMGR_PORT && sel <= QRX_TB_WM_CLOSE_WINDOW))
        return qrx_wm_dispatch(&tb->wm, args);
    if ((sel >= QRX_TB_MENU_INIT && sel <= QRX_TB_MENU_DELETE_ITEM))
        return qrx_menu_dispatch(&tb->menu, args);
    if (sel == QRX_TB_GESTALT)
        return tb_gestalt(tb, args);
    if (sel == QRX_TB_GET_ZONE)
        return tb_get_zone(tb);
    if (sel == QRX_TB_SET_ZONE)
        return 0u;
    if (sel == QRX_TB_DM_INIT || (sel >= QRX_TB_DM_ALERT &&
                                 sel <= QRX_TB_DM_MODAL_DIALOG))
        return qrx_dm_dispatch(&tb->dm, args);
    return QRX_MM_UNKNOWN;
}
