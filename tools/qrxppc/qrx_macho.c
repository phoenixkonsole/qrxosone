#include "qrx_macho.h"
#include "qrx_dyld.h"
#include "qrx_ppc.h"

#include <stddef.h>
#include <string.h>

#define FAT_MAGIC 0xcafebabeu
#define MH_MAGIC 0xfeedfaceu
#define CPU_TYPE_POWERPC 18u
#define MH_EXECUTE 2u
#define LC_SEGMENT 1u
#define LC_SYMTAB 2u
#define LC_UNIXTHREAD 5u
#define LC_DYSYMTAB 0xbu
#define LC_LOAD_DYLIB 0xcu
#define LC_ID_DYLIB 0xdu
#define LC_LOAD_DYLINKER 0xeu
#define LC_TWOLEVEL_HINTS 0x16u
#define LC_UUID 0x1bu
#define LC_CODE_SIGNATURE 0x1du
#define LC_SEGMENT_SPLIT_INFO 0x1eu
#define LC_FUNCTION_STARTS 0x26u
#define LC_DATA_IN_CODE 0x29u
#define LC_SOURCE_VERSION 0x2au
#define LC_LINKER_OPTION 0x2au
#define LC_DYLIB_CODE_SIGN_DRS 0x2bu
#define LC_LOAD_WEAK_DYLIB 0x80000018u
#define LC_REEXPORT_DYLIB 0x8000001fu
#define LC_LOAD_UPWARD_DYLIB 0x80000023u
#define PPC_THREAD_STATE 1u
#define PPC_THREAD_STATE_COUNT 40u
#define VM_PROT_EXECUTE 4u
#define SECTION_TYPE 0xffu
#define S_NON_LAZY_SYMBOL_POINTERS 0x6u
#define S_LAZY_SYMBOL_POINTERS 0x7u
#define N_TYPE 0x0eu
#define N_UNDF 0x0u
#define N_SECT 0xeu

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int range_ok(uint32_t size, uint32_t off, uint32_t len)
{
    return off <= size && len <= size - off;
}

static int reserved_segment(const qrx_macho_segment_t *s)
{
    return s->filesize == 0u && s->maxprot == 0u && s->initprot == 0u;
}

qrx_macho_status_t qrx_macho_parse(qrx_macho_t *out, const uint8_t *data,
                                   uint32_t size)
{
    qrx_macho_t *img = out;
    const uint8_t *p;
    uint32_t ncmds, cmdbytes, cursor, i;
    int have_entry = 0;

    if (!out || !data)
        return QRX_MACHO_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    img->static_start_supported = 1;
    img->dynamic_start_supported = 1;
    if (size < 4u)
        return QRX_MACHO_ERR_TRUNCATED;
    img->slice = data;
    img->slice_size = size;
    if (be32(data) == FAT_MAGIC) {
        uint32_t count, selected = 0u, j;
        if (size < 8u)
            return QRX_MACHO_ERR_TRUNCATED;
        count = be32(data + 4u);
        if (count > (size - 8u) / 20u)
            return QRX_MACHO_ERR_TRUNCATED;
        for (j = 0; j < count; j++) {
            const uint8_t *a = data + 8u + j * 20u;
            uint32_t off = be32(a + 8u), len = be32(a + 12u);
            uint32_t align = be32(a + 16u);
            if (!range_ok(size, off, len) || align > 31u ||
                (align != 0u && (off & ((1u << align) - 1u)) != 0u) ||
                off < 8u + count * 20u)
                return QRX_MACHO_ERR_FORMAT;
            if (!selected && be32(a) == CPU_TYPE_POWERPC) {
                selected = 1u;
                img->slice_offset = off;
                img->slice_size = len;
            }
        }
        if (!selected)
            return QRX_MACHO_ERR_ARCH;
        img->slice = data + img->slice_offset;
    }
    p = img->slice;
    size = img->slice_size;
    if (size < 28u)
        return QRX_MACHO_ERR_TRUNCATED;
    if (be32(p) != MH_MAGIC)
        return QRX_MACHO_ERR_FORMAT;
    if (be32(p + 4u) != CPU_TYPE_POWERPC)
        return QRX_MACHO_ERR_ARCH;
    if (be32(p + 12u) != MH_EXECUTE)
        return QRX_MACHO_ERR_FORMAT;
    img->cpu_subtype = be32(p + 8u);
    img->flags = be32(p + 24u);
    if (img->flags != 0u)
        img->static_start_supported = 0;
    ncmds = be32(p + 16u);
    cmdbytes = be32(p + 20u);
    if (!range_ok(size, 28u, cmdbytes) || ncmds > cmdbytes / 8u)
        return QRX_MACHO_ERR_TRUNCATED;
    cursor = 28u;
    for (i = 0; i < ncmds; i++) {
        const uint8_t *c;
        uint32_t cmd, len, end = 28u + cmdbytes;
        if (!range_ok(end, cursor, 8u))
            return QRX_MACHO_ERR_COMMAND;
        c = p + cursor;
        cmd = be32(c);
        len = be32(c + 4u);
        if (len < 8u || (len & 3u) || !range_ok(end, cursor, len))
            return QRX_MACHO_ERR_COMMAND;
        if (cmd == LC_SEGMENT) {
            qrx_macho_segment_t *s;
            uint32_t nsects;
            if (len < 56u || img->segment_count == QRX_MACHO_MAX_SEGMENTS)
                return QRX_MACHO_ERR_SEGMENT;
            nsects = be32(c + 48u);
            /* A sectioned image is never a static-start candidate; the
             * dynamic path records the sections and binds their pointers. */
            if (nsects != 0u)
                img->static_start_supported = 0;
            if (nsects > (len - 56u) / 68u || len != 56u + nsects * 68u)
                return QRX_MACHO_ERR_SEGMENT;
            if (nsects > QRX_MACHO_MAX_SECTIONS - img->section_count)
                return QRX_MACHO_ERR_SEGMENT;
            s = &img->segments[img->segment_count++];
            memcpy(s->name, c + 8u, 16u);
            s->name[16] = '\0';
            s->vmaddr = be32(c + 24u);
            s->vmsize = be32(c + 28u);
            s->fileoff = be32(c + 32u);
            s->filesize = be32(c + 36u);
            s->maxprot = be32(c + 40u);
            s->initprot = be32(c + 44u);
            if (s->filesize > s->vmsize ||
                !range_ok(size, s->fileoff, s->filesize) ||
                (uint64_t)s->vmaddr + s->vmsize > 0x100000000ULL ||
                (s->initprot & ~s->maxprot) != 0u)
                return QRX_MACHO_ERR_SEGMENT;
            {
                uint32_t k;
                for (k = 0; k < nsects; k++) {
                    const uint8_t *sc = c + 56u + k * 68u;
                    qrx_macho_section_t *sec = &img->sections[img->section_count];
                    memcpy(sec->sectname, sc, 16u);
                    sec->sectname[16] = '\0';
                    memcpy(sec->segname, sc + 16u, 16u);
                    sec->segname[16] = '\0';
                    sec->addr = be32(sc + 32u);
                    sec->size = be32(sc + 36u);
                    sec->offset = be32(sc + 40u);
                    sec->flags = be32(sc + 56u);
                    sec->reserved1 = be32(sc + 60u);
                    sec->reserved2 = be32(sc + 64u);
                    sec->segment = img->segment_count - 1u;
                    if ((uint64_t)sec->addr + sec->size > 0x100000000ULL)
                        return QRX_MACHO_ERR_SEGMENT;
                    img->section_count++;
                }
            }
        } else if (cmd == LC_UNIXTHREAD) {
            uint32_t pos = 8u;
            while (pos < len) {
                uint32_t flavor, words;
                if (!range_ok(len, pos, 8u))
                    return QRX_MACHO_ERR_COMMAND;
                flavor = be32(c + pos);
                words = be32(c + pos + 4u);
                pos += 8u;
                if (words > (len - pos) / 4u)
                    return QRX_MACHO_ERR_COMMAND;
                if (flavor == PPC_THREAD_STATE) {
                    if (have_entry || words != PPC_THREAD_STATE_COUNT)
                        return QRX_MACHO_ERR_ENTRY;
                    uint32_t k;
                    img->entry = be32(c + pos); /* srr0 */
                    /* The prototype initializes a fresh CPU rather than
                     * restoring an arbitrary saved machine state. */
                    for (k = 1u; k < words; k++) {
                        if (be32(c + pos + 4u * k) != 0u)
                            img->static_start_supported = 0;
                    }
                    have_entry = 1;
                }
                if (flavor != PPC_THREAD_STATE)
                    img->static_start_supported = 0;
                pos += words * 4u;
            }
        } else if (cmd == LC_LOAD_DYLINKER || cmd == LC_LOAD_DYLIB ||
                   cmd == LC_LOAD_WEAK_DYLIB || cmd == LC_REEXPORT_DYLIB ||
                   cmd == LC_LOAD_UPWARD_DYLIB) {
            uint32_t off, minimum = cmd == LC_LOAD_DYLINKER ? 12u : 24u;
            if (len < minimum)
                return QRX_MACHO_ERR_COMMAND;
            off = be32(c + 8u);
            if (off < minimum || off >= len ||
                !memchr(c + off, '\0', len - off))
                return QRX_MACHO_ERR_COMMAND;
            if (cmd == LC_LOAD_DYLINKER)
                img->has_dylinker = 1;
            else
                img->dylib_count++;
            if (cmd != LC_LOAD_DYLINKER &&
                img->dylib_name_count < QRX_MACHO_MAX_DYLIBS) {
                qrx_macho_dylib_t *d = &img->dylibs[img->dylib_name_count++];
                uint32_t n = len - off;
                if (n > QRX_MACHO_DYLIB_NAME - 1u)
                    n = QRX_MACHO_DYLIB_NAME - 1u;
                memcpy(d->name, c + off, n);
                d->name[n] = '\0';
                d->command = cmd;
            }
            img->static_start_supported = 0;
        } else if (cmd == LC_SYMTAB) {
            uint32_t symoff, nsyms, stroff, strsize;
            if (len < 24u || img->symtab.present)
                return QRX_MACHO_ERR_COMMAND;
            symoff = be32(c + 8u);
            nsyms = be32(c + 12u);
            stroff = be32(c + 16u);
            strsize = be32(c + 20u);
            if (!range_ok(size, symoff, nsyms * 12u) ||
                !range_ok(size, stroff, strsize) ||
                nsyms > (size - symoff) / 12u)
                return QRX_MACHO_ERR_COMMAND;
            img->symtab.symoff = symoff;
            img->symtab.nsyms = nsyms;
            img->symtab.stroff = stroff;
            img->symtab.strsize = strsize;
            img->symtab.present = 1;
        } else if (cmd == LC_DYSYMTAB) {
            if (len < 80u)
                return QRX_MACHO_ERR_COMMAND;
            /* ilocalsym(8) nlocalsym(12) iextdefsym(16) nextdefsym(20)
             * iundefsym(24) nundefsym(28) tocoff(32) ntoc(36) modtaboff(40)
             * nmodtab(44) extrefsymoff(48) nextrefsyms(52) indirectsymoff(56)
             * nindirectsyms(60) extreloff(64) nextrel(68) locreloff(72)
             * nlocrel(76) */
            img->symtab.iundefsym = be32(c + 24u);
            img->symtab.nundefsym = be32(c + 28u);
            img->symtab.indirectsymoff = be32(c + 56u);
            img->symtab.nindirectsyms = be32(c + 60u);
            if ((img->symtab.nundefsym &&
                 img->symtab.iundefsym > img->symtab.nsyms) ||
                (img->symtab.nindirectsyms &&
                 !range_ok(size, img->symtab.indirectsymoff,
                           img->symtab.nindirectsyms * 4u)))
                return QRX_MACHO_ERR_COMMAND;
            img->symtab.indirect = 1;
        } else {
            /* Inspection may skip unknown commands; execution must not. */
            img->static_start_supported = 0;
            /* Commands that carry only metadata are fine for binding.  Any
             * other command (segments of a different kind, encryption,
             * fixups we do not process) disqualifies the dynamic path. */
            if (cmd != LC_UUID && cmd != LC_ID_DYLIB &&
                cmd != LC_TWOLEVEL_HINTS && cmd != LC_LINKER_OPTION &&
                cmd != LC_CODE_SIGNATURE && cmd != LC_SEGMENT_SPLIT_INFO &&
                cmd != LC_FUNCTION_STARTS && cmd != LC_DATA_IN_CODE &&
                cmd != LC_SOURCE_VERSION && cmd != LC_DYLIB_CODE_SIGN_DRS)
                img->dynamic_start_supported = 0;
        }
        cursor += len;
    }
    if (cursor != 28u + cmdbytes)
        return QRX_MACHO_ERR_COMMAND;
    if (!have_entry || (img->entry & 3u))
        return QRX_MACHO_ERR_ENTRY;
    for (i = 0; i < img->segment_count; i++) {
        const qrx_macho_segment_t *s = &img->segments[i];
        if ((s->initprot & VM_PROT_EXECUTE) && img->entry >= s->vmaddr &&
            (uint64_t)img->entry + 4u <= (uint64_t)s->vmaddr + s->filesize)
            break;
    }
    if (i == img->segment_count)
        return QRX_MACHO_ERR_ENTRY;
    /* The restricted binder needs a symbol table, an indirect symbol table
     * and at least one pointer section to write into. */
    if (!img->symtab.present || !img->symtab.indirect ||
        img->symtab.nsyms == 0u || img->section_count == 0u)
        img->dynamic_start_supported = 0;
    return QRX_MACHO_OK;
}

qrx_macho_status_t qrx_macho_map(const qrx_macho_t *img, uint8_t *memory,
                                 uint32_t memory_size)
{
    uint32_t i, j;
    uintptr_t src, dst;
    if (!img || !img->slice || !memory ||
        img->segment_count > QRX_MACHO_MAX_SEGMENTS)
        return QRX_MACHO_ERR_ARGUMENT;
    src = (uintptr_t)img->slice;
    dst = (uintptr_t)memory;
    if ((src <= dst && dst - src < img->slice_size) ||
        (dst <= src && src - dst < memory_size))
        return QRX_MACHO_ERR_ARGUMENT;
    for (i = 0; i < img->segment_count; i++) {
        const qrx_macho_segment_t *s = &img->segments[i];
        if (s->filesize > s->vmsize ||
            !range_ok(img->slice_size, s->fileoff, s->filesize) ||
            (uint64_t)s->vmaddr + s->vmsize > 0x100000000ULL)
            return QRX_MACHO_ERR_SEGMENT;
        if (s->vmsize == 0u)
            continue;
        /* Reservations such as __PAGEZERO also own their address range. */
        for (j = 0; j < i; j++) {
            const qrx_macho_segment_t *t = &img->segments[j];
            if (t->vmsize != 0u &&
                (uint64_t)s->vmaddr < (uint64_t)t->vmaddr + t->vmsize &&
                (uint64_t)t->vmaddr < (uint64_t)s->vmaddr + s->vmsize)
                return QRX_MACHO_ERR_SEGMENT;
        }
        if (!reserved_segment(s) &&
            !range_ok(memory_size, s->vmaddr, s->vmsize))
            return QRX_MACHO_ERR_MEMORY;
    }
    for (i = 0; i < img->segment_count; i++) {
        const qrx_macho_segment_t *s = &img->segments[i];
        if (reserved_segment(s) || s->vmsize == 0u)
            continue;
        memcpy(memory + s->vmaddr, img->slice + s->fileoff, s->filesize);
        memset(memory + s->vmaddr + s->filesize, 0, s->vmsize - s->filesize);
    }
    return QRX_MACHO_OK;
}

typedef struct {
    const qrx_macho_t *image;
    uint32_t stack_base;
    uint32_t memory_size;
    uint32_t tramp_base; /* generated import trampolines: read + execute */
    uint32_t tramp_end;
} macho_address_space_t;

/* Require the entire access to fit in one region with the requested right.
 * In particular, zero-fill gaps and reserved __PAGEZERO are not guest memory. */
static int macho_access(void *ctx, uint32_t addr, uint32_t len, unsigned access)
{
    const macho_address_space_t *space = (const macho_address_space_t *)ctx;
    uint32_t i;
    if (!space || !range_ok(space->memory_size, addr, len))
        return 0;
    if (len == 0u)
        return 1;
    if (addr >= space->stack_base &&
        (access & ~(QRX_PPC_ACCESS_READ | QRX_PPC_ACCESS_WRITE)) == 0u)
        return 1;
    if (space->tramp_end > space->tramp_base && addr >= space->tramp_base &&
        addr < space->tramp_end &&
        (access & ~(QRX_PPC_ACCESS_READ | QRX_PPC_ACCESS_EXEC)) == 0u)
        return 1;
    for (i = 0; i < space->image->segment_count; i++) {
        const qrx_macho_segment_t *s = &space->image->segments[i];
        if (addr >= s->vmaddr && addr - s->vmaddr < s->vmsize &&
            len <= s->vmsize - (addr - s->vmaddr) &&
            (s->initprot & access) == access)
            return 1;
    }
    return 0;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static qrx_macho_run_status_t run_image(const qrx_macho_t *img,
                                        uint8_t *memory, uint32_t size,
                                        uint32_t stack_base, uint32_t budget,
                                        qrx_macho_write_fn write_fn, void *ctx,
                                        const qrx_macho_dynamic_t *dyn,
                                        qrx_macho_run_result_t *result)
{
    static const char program[] = "qrxmach";
    qrx_ppc_t ppc;
    macho_address_space_t space;
    uint32_t sp, name, left = budget, i;
    uint32_t errno_addr = 0u;
    qrx_macho_status_t mapping;

    if (result)
        memset(result, 0, sizeof(*result));
    if (!img || !memory || !result || !write_fn || budget == 0u ||
        (stack_base & 15u) || (size & 15u) || stack_base >= size ||
        size - stack_base < 64u)
        return QRX_MACHO_RUN_ARGUMENT;
    if (dyn) {
        if (!img->dynamic_start_supported)
            return QRX_MACHO_RUN_UNSUPPORTED;
    } else if (!img->static_start_supported || img->has_dylinker ||
               img->dylib_count) {
        return QRX_MACHO_RUN_UNSUPPORTED;
    }
    if (img->segment_count > QRX_MACHO_MAX_SEGMENTS || img->entry == 0u)
        return QRX_MACHO_RUN_ARGUMENT;
    for (i = 0; i < img->segment_count; i++) {
        const qrx_macho_segment_t *s = &img->segments[i];
        if (s->vmsize && (uint64_t)s->vmaddr < size &&
            (uint64_t)s->vmaddr + s->vmsize > stack_base)
            return QRX_MACHO_RUN_MEMORY;
    }
    mapping = qrx_macho_map(img, memory, size);
    if (mapping != QRX_MACHO_OK)
        return mapping == QRX_MACHO_ERR_MEMORY ? QRX_MACHO_RUN_MEMORY
                                                : QRX_MACHO_RUN_ARGUMENT;
    memset(memory + stack_base, 0, size - stack_base);
    if (dyn) {
        qrx_dyld_result_t bound;
        qrx_dyld_status_t bs;
        uint32_t end;

        if (dyn->trampoline_size == 0u || dyn->trampoline_base >= stack_base ||
            dyn->trampoline_size > stack_base - dyn->trampoline_base)
            return QRX_MACHO_RUN_MEMORY;
        end = dyn->trampoline_base + dyn->trampoline_size;
        if (end > size)
            return QRX_MACHO_RUN_MEMORY;
        memset(memory + dyn->trampoline_base, 0, dyn->trampoline_size);
        bs = qrx_dyld_bind((qrx_macho_t *)img, memory, size, dyn->imports,
                           dyn->import_count, dyn->trampoline_base,
                           dyn->trampoline_size, &bound);
        result->imports_bound = bound.bound;
        result->imports_unresolved = bound.unresolved;
        if (bound.unresolved) {
            size_t n = strlen(bound.missing);
            if (n > sizeof(result->missing_import) - 1u)
                n = sizeof(result->missing_import) - 1u;
            memcpy(result->missing_import, bound.missing, n);
            result->missing_import[n] = '\0';
        }
        if (bs == QRX_DYLD_ERR_UNRESOLVED)
            return QRX_MACHO_RUN_SYSCALL;
        if (bs != QRX_DYLD_OK)
            return bs == QRX_DYLD_ERR_SPACE || bs == QRX_DYLD_ERR_ARGUMENT
                       ? QRX_MACHO_RUN_MEMORY
                       : QRX_MACHO_RUN_UNSUPPORTED;
        errno_addr = dyn->trampoline_base;
    }
    name = size - (uint32_t)sizeof(program);
    memcpy(memory + name, program, sizeof(program));
    sp = (name - 20u) & ~15u;
    put32(memory + sp, 1u);       /* argc */
    put32(memory + sp + 4u, name); /* argv[0] */
    put32(memory + sp + 8u, 0u); /* argv terminator */
    put32(memory + sp + 12u, 0u); /* env terminator */
    put32(memory + sp + 16u, 0u); /* apple terminator */
    qrx_ppc_init(&ppc, memory, size);
    space.image = img;
    space.stack_base = stack_base;
    space.memory_size = size;
    space.tramp_base = dyn ? dyn->trampoline_base : 0u;
    space.tramp_end = dyn ? dyn->trampoline_base + dyn->trampoline_size : 0u;
    ppc.access = macho_access;
    ppc.access_ctx = &space;
    ppc.cpu.pc = img->entry;
    ppc.cpu.gpr[1] = sp;
    while (left) {
        qrx_ppc_status_t st = qrx_ppc_run(&ppc, left);
        result->instructions += ppc.insns;
        left -= ppc.insns;
        result->pc = ppc.cpu.pc;
        if (st == QRX_PPC_OK)
            return QRX_MACHO_RUN_LIMIT;
        if (st != QRX_PPC_TRAP ||
            !macho_access(&space, ppc.cpu.srr0, 4u, QRX_PPC_ACCESS_EXEC) ||
            be32(memory + ppc.cpu.srr0) != 0x44000002u) {
            result->fault_addr = ppc.fault_addr;
            return QRX_MACHO_RUN_FAULT;
        }
        result->pc = ppc.cpu.srr0;
        result->syscall = ppc.cpu.gpr[0];
        if (errno_addr && (result->syscall & 0xfffff000u) == 0x4000u) {
            uint32_t selector = result->syscall & 0xfffu;
            uint32_t ret = 0u, error = 0u;

            switch (selector) {
            case QRX_DYLD_SEL_WRITE: {
                uint32_t fd = ppc.cpu.gpr[3];
                uint32_t addr = ppc.cpu.gpr[4];
                uint32_t count = ppc.cpu.gpr[5];

                if (fd != 1u && fd != 2u) {
                    error = 9u; /* EBADF */
                } else if (!macho_access(&space, addr, count,
                                         QRX_PPC_ACCESS_READ)) {
                    error = 14u; /* EFAULT */
                } else if (count &&
                           write_fn(ctx, fd, memory + addr, count) != 0) {
                    error = 5u; /* EIO */
                } else {
                    ret = count;
                }
                break;
            }
            case QRX_DYLD_SEL_EXIT:
                result->exit_code = ppc.cpu.gpr[3] & 0xffu;
                return QRX_MACHO_RUN_EXITED;
            case QRX_DYLD_SEL_ERRNO:
                ret = errno_addr;
                break;
            case QRX_DYLD_SEL_STRLEN: {
                uint32_t addr = ppc.cpu.gpr[3], n = 0u;

                while (n < 4096u) {
                    uint32_t c;
                    if (!macho_access(&space, addr + n, 1u,
                                      QRX_PPC_ACCESS_READ)) {
                        error = 14u; /* EFAULT */
                        n = 0u;
                        break;
                    }
                    c = memory[addr + n];
                    if (c == 0u)
                        break;
                    n++;
                }
                if (error)
                    n = 0u;
                else if (n == 4096u && memory[addr + n - 1u] != 0u)
                    error = 14u;
                ret = error ? 0u : n;
                break;
            }
            default:
                return QRX_MACHO_RUN_SYSCALL;
            }
            if (error) {
                put32(memory + errno_addr, error);
                ret = 0xffffffffu;
                ppc.cpu.cr |= 0x10000000u; /* CR0.SO */
            } else {
                ppc.cpu.cr &= ~0x10000000u;
            }
            ppc.cpu.gpr[3] = ret;
            ppc.cpu.pc = ppc.cpu.srr0 + 4u;
            continue;
        }
        if (result->syscall == 1u) {
            result->exit_code = ppc.cpu.gpr[3] & 0xffu;
            return QRX_MACHO_RUN_EXITED;
        }
        if (result->syscall != 4u)
            return QRX_MACHO_RUN_SYSCALL;
        {
            uint32_t fd = ppc.cpu.gpr[3], addr = ppc.cpu.gpr[4];
            uint32_t count = ppc.cpu.gpr[5], error = 0u;
            if (fd != 1u && fd != 2u)
                error = 9u; /* EBADF */
            else if (!macho_access(&space, addr, count, QRX_PPC_ACCESS_READ))
                error = 14u; /* EFAULT */
            else if (count && write_fn(ctx, fd, memory + addr, count) != 0)
                error = 5u; /* EIO */
            ppc.cpu.gpr[3] = error ? error : count;
            if (error)
                ppc.cpu.cr |= 0x10000000u; /* CR0.SO */
            else
                ppc.cpu.cr &= ~0x10000000u;
        }
        ppc.cpu.pc = ppc.cpu.srr0 + 4u;
    }
    return QRX_MACHO_RUN_LIMIT;
}

qrx_macho_run_status_t qrx_macho_run(const qrx_macho_t *img,
                                      uint8_t *memory, uint32_t size,
                                      uint32_t stack_base, uint32_t budget,
                                      qrx_macho_write_fn write_fn, void *ctx,
                                      qrx_macho_run_result_t *result)
{
    return run_image(img, memory, size, stack_base, budget, write_fn, ctx,
                     NULL, result);
}

qrx_macho_run_status_t qrx_macho_run_dynamic(
    const qrx_macho_t *img, uint8_t *memory, uint32_t size,
    uint32_t stack_base, uint32_t budget, qrx_macho_write_fn write_fn,
    void *ctx, const qrx_macho_dynamic_t *dynamic,
    qrx_macho_run_result_t *result)
{
    return run_image(img, memory, size, stack_base, budget, write_fn, ctx,
                     dynamic, result);
}

const char *qrx_macho_run_status_string(qrx_macho_run_status_t status)
{
    switch (status) {
    case QRX_MACHO_RUN_EXITED: return "exited";
    case QRX_MACHO_RUN_ARGUMENT: return "invalid image or arguments";
    case QRX_MACHO_RUN_UNSUPPORTED: return "unsupported dynamic or non-synthetic image";
    case QRX_MACHO_RUN_MEMORY: return "invalid guest memory or stack";
    case QRX_MACHO_RUN_FAULT: return "PPC fault, halt or non-syscall trap";
    case QRX_MACHO_RUN_SYSCALL: return "unsupported syscall";
    case QRX_MACHO_RUN_LIMIT: return "instruction budget exhausted";
    }
    return "unknown status";
}

const char *qrx_macho_status_string(qrx_macho_status_t status)
{
    switch (status) {
    case QRX_MACHO_OK: return "ok";
    case QRX_MACHO_ERR_ARGUMENT: return "invalid argument";
    case QRX_MACHO_ERR_FORMAT: return "unsupported or malformed format";
    case QRX_MACHO_ERR_ARCH: return "no PPC32 architecture";
    case QRX_MACHO_ERR_TRUNCATED: return "truncated image";
    case QRX_MACHO_ERR_COMMAND: return "invalid load command";
    case QRX_MACHO_ERR_SEGMENT: return "invalid or overlapping segment";
    case QRX_MACHO_ERR_ENTRY: return "invalid PPC entry point";
    case QRX_MACHO_ERR_MEMORY: return "guest memory too small";
    }
    return "unknown status";
}
