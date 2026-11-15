/*
 * QRXPPC - classic MacOS application runner (P2.3).
 *
 * See qrx_apprun.h for the contract.  This file keeps the loading rules and
 * the trap trace; the Toolbox itself stays in qrx_toolbox.c.
 */
#include "qrx_apprun.h"
#include "qrx_appstub.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static uint32_t ar_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t ar_get32(const qrx_ppc_t *ppc, uint32_t addr)
{
    if (!ppc || !ppc->mem.data || addr > ppc->mem.size ||
        ppc->mem.size - addr < 4u)
        return 0u;
    return ar_be32(&ppc->mem.data[addr]);
}

static void ar_set_detail(qrx_apprun_t *run, const char *fmt, ...)
{
    va_list ap;

    if (!run)
        return;
    va_start(ap, fmt);
    vsnprintf(run->detail, sizeof(run->detail), fmt, ap);
    va_end(ap);
    run->detail[sizeof(run->detail) - 1u] = '\0';
}

/* MacBinary: version byte 0, a name length of 1..63 and both fork lengths
 * inside the image (each fork padded to 128 bytes). */
static int ar_looks_macbinary(const uint8_t *image, uint32_t size)
{
    uint32_t data_len, rsrc_len, need;

    if (!image || size < QRX_MACBIN_HEADER)
        return 0;
    if (image[0] != 0u || image[1] < 1u || image[1] > QRX_MACBIN_NAME_MAX)
        return 0;
    data_len = ar_be32(image + 83u);
    rsrc_len = ar_be32(image + 87u);
    if (data_len > QRX_MACBIN_FORK_MAX || rsrc_len > QRX_MACBIN_FORK_MAX)
        return 0;
    /* The data fork is padded to 128 bytes; the parser accepts a resource
     * fork that ends at the end of the image without its final padding. */
    need = QRX_MACBIN_HEADER + ((data_len + 127u) & ~127u) + rsrc_len;
    return need <= size;
}

/* A 'cfrg' resource in the resource fork points at the fragment. */
static int ar_has_cfrg(const uint8_t *rsrc, uint32_t rsrc_len)
{
    const uint8_t *res;
    uint32_t res_len;

    return rsrc && rsrc_len != 0u &&
           qrx_rsrc_find(rsrc, rsrc_len, QRX_CFRG_TYPE, 0, &res,
                         &res_len) == QRX_MACAPP_OK;
}

/* A 68k application carries its code as 'CODE' resources and has no 'cfrg'. */
static int ar_has_code(const uint8_t *rsrc, uint32_t rsrc_len)
{
    const uint8_t *res;
    uint32_t res_len;

    if (!rsrc || rsrc_len == 0u)
        return 0;
    return qrx_rsrc_find(rsrc, rsrc_len, 0x434F4445u /* 'CODE' */, 0, &res,
                         &res_len) == QRX_MACAPP_OK;
}

static uint32_t ar_import(void *ctx, const qrx_mix_args_t *args,
                          qrx_mix_result_t *extra);
static uint32_t ar_resolve(void *ctx, const char *library,
                           uint8_t symbol_class, const char *name);

static qrx_apprun_status_t ar_load_pef(qrx_apprun_t *run, const uint8_t *pef,
                                       uint32_t pef_len)
{
    qrx_pef_status_t st;

    if (pef_len < 4u || ar_be32(pef) != 0x4A6F7921u) { /* 'Joy!' */
        ar_set_detail(run, "data fork is not a PEF container");
        return QRX_APPRUN_ERR_IMAGE;
    }
    st = qrx_pef_parse(&run->pef, pef, pef_len);
    if (st != QRX_PEF_OK) {
        ar_set_detail(run, "PEF container rejected (status %d)", (int)st);
        return QRX_APPRUN_ERR_IMAGE;
    }
    st = qrx_pef_load(&run->pef, run->ppc, run->load_base);
    if (st != QRX_PEF_OK) {
        ar_set_detail(run, "PEF sections do not load (status %d)", (int)st);
        return st == QRX_PEF_ERR_MEMORY ? QRX_APPRUN_ERR_MEM
                                        : QRX_APPRUN_ERR_IMAGE;
    }
    if (run->pef.reloc_section_count != 0u) {
        st = qrx_pef_relocate(&run->pef, run->ppc, ar_resolve, run);
        if (st != QRX_PEF_OK) {
            ar_set_detail(run, "PEF relocation failed (status %d)", (int)st);
            return QRX_APPRUN_ERR_IMAGE;
        }
    }
    return QRX_APPRUN_OK;
}

qrx_apprun_status_t qrx_apprun_init(qrx_apprun_t *run, qrx_ppc_t *ppc,
                                    uint32_t zone_base, uint32_t zone_size,
                                    uint32_t glue_addr, uint32_t sp_top,
                                    uint32_t sp_limit)
{
    if (!run || !ppc || !ppc->mem.data || zone_size == 0u ||
        glue_addr == 0u || sp_top <= sp_limit)
        return QRX_APPRUN_ERR_ARG;
    memset(run, 0, sizeof(*run));
    run->ppc = ppc;
    qrx_tb_init(&run->tb, ppc, zone_base, zone_size);
    if (qrx_mix_init(&run->mix, ppc, glue_addr, sp_top, sp_limit) !=
        QRX_MIX_OK)
        return QRX_APPRUN_ERR_MIX;
    if (qrx_mix_register(&run->mix, 0u, QRX_MIX_RAW, ar_import, run, 0) !=
        QRX_MIX_OK)
        return QRX_APPRUN_ERR_MIX;
    return QRX_APPRUN_OK;
}


/* ------------------------------------------------------------------ */
/* Import stubs: name -> trap                                          */
/* ------------------------------------------------------------------ */

#define AR_STUB_BYTES 64u

static void ar_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Import index in the loaded fragment; -1 when the name does not occur. */
static int ar_import_index(qrx_apprun_t *run, const char *library,
                           const char *name)
{
    uint32_t i;

    for (i = 0; i < run->pef.import_count && i < QRX_APPRUN_MAX_IMPORTS; i++) {
        const qrx_pef_import_t *im = &run->pef.imports[i];
        const char *candidate = qrx_pef_string(&run->pef, im->name_offset);

        if (!candidate || strcmp(candidate, name) != 0)
            continue;
        if (library && im->library_index >= 0 &&
            (uint32_t)im->library_index < run->pef.library_count) {
            const char *lib_name = qrx_pef_string(
                &run->pef,
                run->pef.libraries[im->library_index].name_offset);

            if (lib_name && strcmp(lib_name, library) != 0)
                continue;
        }
        return (int)i;
    }
    return -1;
}

/* Wrapper symbols whose argument list already matches the trap. */
static int ar_indirect_compatible(const char *name)
{
    return strcmp(name, "Get1Resource") == 0 ||
           strcmp(name, "Get1IndResource") == 0 ||
           strcmp(name, "Count1Resources") == 0 ||
           strcmp(name, "SetClip") == 0;
}

/* A stub is a two-word transition vector plus code: load the trap number into
 * r3, move the application's arguments from r3..r10 to r4..r11, select the
 * Toolbox import (r0 = 0) and raise the `sc`.  The caller's TOC stays in the
 * TVector, so r2 never changes. */
static uint32_t ar_stub_build(qrx_apprun_t *run, unsigned index,
                              uint32_t trap)
{
    uint32_t addr = run->stub_area + index * AR_STUB_BYTES;
    uint8_t *p = run->ppc->mem.data + addr;
    int r;

    memset(p, 0, AR_STUB_BYTES);
    ar_put32(p, addr + 8u);            /* TVector: entry */
    ar_put32(p + 4u, run->toc);        /* TVector: TOC of the fragment */
    p += 8u;
    for (r = 10; r >= 3; r--) {        /* or r(r+1),r(r),r(r) */
        ar_put32(p, 0x7C000378u | ((uint32_t)(r + 1) << 21) |
                    ((uint32_t)r << 16) | ((uint32_t)r << 11));
        p += 4u;
    }
    ar_put32(p, 0x38000000u);          /* li r0,0 (import 0) */
    p += 4u;
    ar_put32(p, 0x3C600000u);          /* lis r3,0 */
    p += 4u;
    ar_put32(p, 0x60630000u | (trap & 0xFFFFu)); /* ori r3,r3,trap */
    p += 4u;
    ar_put32(p, 0x44000002u);          /* sc */
    p += 4u;
    ar_put32(p, 0x4E800020u);          /* blr */
    return addr;
}

/* Reserve the stub pool right behind the last section. */
static uint32_t ar_stub_area(qrx_apprun_t *run)
{
    uint64_t end = run->load_base;
    uint32_t i;

    if (run->stub_area)
        return run->stub_area;
    for (i = 0; i < run->pef.section_count; i++) {
        const qrx_pef_section_t *s = &run->pef.sections[i];
        uint64_t size = s->total_size;

        if (size < s->unpacked_size)
            size = s->unpacked_size;
        if ((uint64_t)s->address + size > end)
            end = (uint64_t)s->address + size;
    }
    end = (end + 15u) & ~(uint64_t)15u;
    if (end + (uint64_t)run->pef.import_count * AR_STUB_BYTES >
        (uint64_t)run->ppc->mem.size)
        return 0u;
    run->stub_area = (uint32_t)end;
    return run->stub_area;
}

static uint32_t ar_resolve(void *ctx, const char *library,
                           uint8_t symbol_class, const char *name)
{
    qrx_apprun_t *run = (qrx_apprun_t *)ctx;
    const qrx_appimport_t *entry;
    uint32_t trap;
    int index;

    (void)symbol_class;
    if (!run || !run->ppc || !name)
        return 0u;
    index = ar_import_index(run, library, name);
    if (index < 0)
        return 0u;
    if (run->stub_addr[index])
        return run->stub_addr[index];
    if (!ar_stub_area(run))
        return 0u;

    entry = qrx_appimport_find(library, name);
    if (entry && (entry->kind == QRX_STUB_DIRECT ||
                  ar_indirect_compatible(name))) {
        trap = entry->trap;
        run->nimport_stub++;
    } else {
        /* No HLE yet (or an indirect symbol whose argument shape needs a
         * wrapper): the stub still exists, so the load succeeds, but its trap
         * number names the import and stops the run at the first call. */
        trap = QRX_APPRUN_PSEUDO_TRAP + (uint32_t)index;
        if (entry)
            run->nimport_wrapper++;
        else
            run->nimport_missing++;
    }
    run->stub_addr[index] = ar_stub_build(run, (unsigned)index, trap);
    return run->stub_addr[index];
}

const char *qrx_apprun_missing_import(const qrx_apprun_t *run,
                                      const char **library)
{
    uint32_t index;
    const qrx_pef_import_t *im;
    const char *name;

    if (!run || run->unimplemented_trap < QRX_APPRUN_PSEUDO_TRAP)
        return 0;
    index = run->unimplemented_trap - QRX_APPRUN_PSEUDO_TRAP;
    if (index >= run->pef.import_count)
        return 0;
    im = &run->pef.imports[index];
    name = qrx_pef_string(&run->pef, im->name_offset);
    if (library) {
        *library = 0;
        if (im->library_index >= 0 &&
            (uint32_t)im->library_index < run->pef.library_count)
            *library = qrx_pef_string(
                &run->pef,
                run->pef.libraries[im->library_index].name_offset);
    }
    return name;
}

/* Decode a two-word transition vector.  Relocation adds the section delta to
 * both words, so they are absolute addresses afterwards; a fragment without
 * relocations keeps default-address values there, and those are shifted by the
 * code section's delta. */
static void ar_read_tvector(qrx_apprun_t *run, uint32_t tvector,
                            uint32_t *entry, uint32_t *toc)
{
    const qrx_pef_section_t *code = &run->pef.sections[0];
    uint32_t raw_entry = ar_get32(run->ppc, tvector);
    uint32_t i;
    int inside = 0;

    for (i = 0; i < run->pef.section_count; i++) {
        const qrx_pef_section_t *sec = &run->pef.sections[i];

        if (sec->address != 0u && raw_entry >= sec->address &&
            raw_entry - sec->address < sec->unpacked_size) {
            inside = 1;
            break;
        }
    }
    *toc = ar_get32(run->ppc, tvector + 4u);
    *entry = raw_entry;
    if (!inside && raw_entry >= code->default_address &&
        raw_entry - code->default_address < code->unpacked_size)
        *entry = raw_entry + (code->address - code->default_address);
}

qrx_apprun_status_t qrx_apprun_load(qrx_apprun_t *run, const uint8_t *image,
                                    uint32_t size, uint32_t load_base)
{
    qrx_macapp_status_t mst;

    if (!run || !image || size == 0u)
        return QRX_APPRUN_ERR_ARG;
    run->load_base = load_base;
    run->tvector = 0u;
    run->entry = 0u;
    run->toc = 0u;
    run->stub_area = 0u;
    run->nimport_stub = 0u;
    run->nimport_wrapper = 0u;
    run->nimport_missing = 0u;
    memset(run->stub_addr, 0, sizeof(run->stub_addr));

    if (ar_looks_macbinary(image, size)) {
        qrx_macbin_t mb;

        mst = qrx_macbin_parse(&mb, image, size);
        if (mst != QRX_MACAPP_OK) {
            ar_set_detail(run, "MacBinary header rejected (status %d)",
                          (int)mst);
            return QRX_APPRUN_ERR_IMAGE;
        }
        if (mb.data_len >= 4u && ar_be32(mb.data) == 0x4A6F7921u) {
            /* The fragment is the data fork itself.  A 'cfrg' list resource
             * may still name other fragments; the first 'pwpc' container in
             * the data fork is what CFM would run. */
            qrx_apprun_status_t rc = ar_load_pef(run, mb.data, mb.data_len);

            if (rc != QRX_APPRUN_OK)
                return rc;
        } else if (ar_has_cfrg(mb.rsrc, mb.rsrc_len)) {
            mst = qrx_app_load(&run->pef, run->ppc, run->load_base, image,
                               size, ar_resolve, run);
            if (mst != QRX_MACAPP_OK) {
                ar_set_detail(run, "application fragment rejected (status %d)",
                              (int)mst);
                return mst == QRX_MACAPP_ERR_PEF ? QRX_APPRUN_ERR_IMAGE
                                                 : QRX_APPRUN_ERR_IMAGE;
            }
        } else {
            /* No fragment descriptor: either a bare PEF in the data fork or
             * a 68k application whose code lives in 'CODE' resources. */
            if (mb.data_len >= 4u && ar_be32(mb.data) == 0x4A6F7921u) {
                qrx_apprun_status_t rc = ar_load_pef(run, mb.data,
                                                     mb.data_len);
                if (rc != QRX_APPRUN_OK)
                    return rc;
            } else if (ar_has_code(mb.rsrc, mb.rsrc_len)) {
                ar_set_detail(run,
                              "68k application: 'CODE' resources, no 'cfrg'");
                return QRX_APPRUN_ERR_68K;
            } else if (mb.data_len >= 4u &&
                       (memcmp(mb.data, "ST50", 4u) == 0 ||
                        memcmp(mb.data, "SVCT", 4u) == 0)) {
                ar_set_detail(run,
                              "installer payload (%.4s), not an application",
                              (const char *)mb.data);
                return QRX_APPRUN_ERR_IMAGE;
            } else {
                ar_set_detail(run, "no 'cfrg' resource and no PEF data fork");
                return QRX_APPRUN_ERR_IMAGE;
            }
        }
    } else if (ar_be32(image) == 0x4A6F7921u) {
        qrx_apprun_status_t rc = ar_load_pef(run, image, size);
        if (rc != QRX_APPRUN_OK)
            return rc;
    } else {
        ar_set_detail(run, "not MacBinary and not a PEF container");
        return QRX_APPRUN_ERR_IMAGE;
    }

    run->tvector = qrx_pef_main_tvector(&run->pef);
    if (run->tvector == 0u) {
        ar_set_detail(run, "fragment has no main transition vector");
        return QRX_APPRUN_ERR_LOAD;
    }
    ar_read_tvector(run, run->tvector, &run->entry, &run->toc);
    if (run->entry == 0u) {
        ar_set_detail(run, "main transition vector has no entry point");
        return QRX_APPRUN_ERR_LOAD;
    }
    {
        /* CFM calls the fragment's init routine once after loading; without
         * it a Metrowerks app runs its C++ static initialisation nowhere and
         * main returns immediately. */
        uint32_t init_tv = qrx_pef_init_tvector(&run->pef);

        run->init_entry = 0u;
        run->init_toc = 0u;
        if (init_tv != 0u && init_tv != run->tvector)
            ar_read_tvector(run, init_tv, &run->init_entry, &run->init_toc);
    }
    ar_set_detail(run,
                  "loaded at 0x%08X, entry 0x%08X, TOC 0x%08X, "
                  "%u/%u imports with a trap",
                  (unsigned)run->load_base, (unsigned)run->entry,
                  (unsigned)run->toc, run->nimport_stub,
                  (unsigned)run->pef.import_count);
    return QRX_APPRUN_OK;
}

static void ar_log(qrx_apprun_t *run, const qrx_mix_args_t *args,
                   uint32_t result, int implemented)
{
    qrx_apprun_trap_t *slot;

    slot = &run->trace[run->ntrace % QRX_APPRUN_TRACE];
    slot->trap = args->gpr[0];
    slot->selector = run->ppc ? run->ppc->cpu.gpr[0] : 0u;
    slot->arg0 = args->gpr[1];
    slot->arg1 = args->gpr[2];
    slot->result = result;
    slot->implemented = implemented ? 1u : 0u;
    run->ntrace++;
    run->ntrace_total++;
    if (implemented)
        run->nimplemented++;
}

/* Import 0: the application's `sc` seam.  r3 carries the A-line trap. */
static uint32_t ar_import(void *ctx, const qrx_mix_args_t *args,
                          qrx_mix_result_t *extra)
{
    qrx_apprun_t *run = (qrx_apprun_t *)ctx;
    uint32_t result;
    int implemented;

    result = qrx_tb_dispatch(&run->tb, args, extra);
    implemented = result != QRX_MM_UNKNOWN;
    ar_log(run, args, result, implemented);
    if (!implemented) {
        run->unimplemented_trap = args->gpr[0];
        qrx_mix_request_stop(&run->mix);
    }
    return result;
}

static qrx_apprun_status_t ar_call(qrx_apprun_t *run, uint32_t entry,
                                   uint32_t toc, uint32_t budget,
                                   const char *what)
{
    qrx_mix_args_t args;
    qrx_mix_result_t out;
    qrx_mix_status_t mst;

    memset(&args, 0, sizeof(args));
    memset(&out, 0, sizeof(out));
    args.rtoc = toc;
    mst = qrx_mix_call(&run->mix, entry, QRX_MIX_RAW_SIG, &args, &out, budget);
    switch (mst) {
    case QRX_MIX_OK:
        ar_set_detail(run, "%s returned after %u trap(s), %u implemented",
                      what, run->ntrace_total, run->nimplemented);
        return QRX_APPRUN_EXITED;
    case QRX_MIX_STOP: {
        const char *library = 0;
        const char *missing = qrx_apprun_missing_import(run, &library);

        if (missing) {
            ar_set_detail(run,
                          "missing import %s::%s (no HLE entry and no stub)",
                          library ? library : "?", missing);
            return QRX_APPRUN_UNIMPLEMENTED;
        }
        return run->unimplemented_trap != 0u ? QRX_APPRUN_UNIMPLEMENTED
                                             : QRX_APPRUN_STOPPED;
    }
    case QRX_MIX_ERR_IMPORT:
        ar_set_detail(run,
                      "sc selector 0x%08X is not a registered import "
                      "(r3=0x%08X); the fragment needs Mixed Mode",
                      (unsigned)run->ppc->cpu.gpr[0],
                      (unsigned)run->ppc->cpu.gpr[3]);
        return QRX_APPRUN_ERR_IMPORT;
    case QRX_MIX_ERR_RUN:
        ar_set_detail(run, "PPC fault at 0x%08X",
                      (unsigned)run->ppc->fault_addr);
        return QRX_APPRUN_FAULT;
    case QRX_MIX_ERR_LIMIT:
        ar_set_detail(run, "instruction budget exhausted");
        return QRX_APPRUN_LIMIT;
    case QRX_MIX_ERR_TRAP:
        ar_set_detail(run, "trap loop without returning to the bridge");
        return QRX_APPRUN_TRAP_LOOP;
    case QRX_MIX_ERR_SP:
        ar_set_detail(run, "fragment did not restore the stack pointer");
        return QRX_APPRUN_ERR_RUN;
    default:
        ar_set_detail(run, "bridge stopped with status %d", (int)mst);
        return QRX_APPRUN_ERR_MIX;
    }
}

qrx_apprun_status_t qrx_apprun_start(qrx_apprun_t *run, uint32_t budget)
{
    qrx_apprun_status_t st;

    if (!run || run->entry == 0u)
        return QRX_APPRUN_ERR_ARG;
    if (run->init_entry == 0u)
        return ar_call(run, run->entry, run->toc, budget, "main");
    st = ar_call(run, run->init_entry, run->init_toc, budget, "init routine");
    if (st != QRX_APPRUN_EXITED)
        return st;
    return ar_call(run, run->entry, run->toc, budget, "main");
}

const char *qrx_apprun_strerror(qrx_apprun_status_t status)
{
    switch (status) {
    case QRX_APPRUN_OK:              return "ok";
    case QRX_APPRUN_ERR_ARG:         return "bad argument or layout";
    case QRX_APPRUN_ERR_MEM:         return "fragment does not fit guest memory";
    case QRX_APPRUN_ERR_MIX:         return "bridge initialisation failed";
    case QRX_APPRUN_ERR_IMAGE:       return "not a usable Mac application";
    case QRX_APPRUN_ERR_68K:         return "68k application (no PEF fragment)";
    case QRX_APPRUN_ERR_LOAD:        return "fragment has no main entry";
    case QRX_APPRUN_UNIMPLEMENTED:   return "stopped at an unimplemented trap";
    case QRX_APPRUN_STOPPED:         return "stopped by the environment";
    case QRX_APPRUN_EXITED:          return "main returned";
    case QRX_APPRUN_FAULT:           return "PPC fault";
    case QRX_APPRUN_LIMIT:           return "instruction budget exhausted";
    case QRX_APPRUN_TRAP_LOOP:       return "trap loop";
    case QRX_APPRUN_ERR_IMPORT:      return "unregistered sc selector";
    case QRX_APPRUN_ERR_RUN:         return "bridge or dispatch failure";
    default:                         return "unknown application status";
    }
}
