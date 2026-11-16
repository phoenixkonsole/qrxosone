#include "qrx_macho.h"
#include "qrx_ppc.h"

#include <stddef.h>
#include <string.h>

#define FAT_MAGIC 0xcafebabeu
#define MH_MAGIC 0xfeedfaceu
#define CPU_TYPE_POWERPC 18u
#define MH_EXECUTE 2u
#define LC_SEGMENT 1u
#define LC_UNIXTHREAD 5u
#define LC_LOAD_DYLIB 0xcu
#define LC_LOAD_DYLINKER 0xeu
#define LC_LOAD_WEAK_DYLIB 0x80000018u
#define LC_REEXPORT_DYLIB 0x8000001fu
#define LC_LOAD_UPWARD_DYLIB 0x80000023u
#define PPC_THREAD_STATE 1u
#define PPC_THREAD_STATE_COUNT 40u
#define VM_PROT_EXECUTE 4u

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
    qrx_macho_t img;
    const uint8_t *p;
    uint32_t ncmds, cmdbytes, cursor, i;
    int have_entry = 0;

    if (!out || !data)
        return QRX_MACHO_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    memset(&img, 0, sizeof(img));
    img.static_start_supported = 1;
    if (size < 4u)
        return QRX_MACHO_ERR_TRUNCATED;
    img.slice = data;
    img.slice_size = size;
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
                img.slice_offset = off;
                img.slice_size = len;
            }
        }
        if (!selected)
            return QRX_MACHO_ERR_ARCH;
        img.slice = data + img.slice_offset;
    }
    p = img.slice;
    size = img.slice_size;
    if (size < 28u)
        return QRX_MACHO_ERR_TRUNCATED;
    if (be32(p) != MH_MAGIC)
        return QRX_MACHO_ERR_FORMAT;
    if (be32(p + 4u) != CPU_TYPE_POWERPC)
        return QRX_MACHO_ERR_ARCH;
    if (be32(p + 12u) != MH_EXECUTE)
        return QRX_MACHO_ERR_FORMAT;
    img.cpu_subtype = be32(p + 8u);
    img.flags = be32(p + 24u);
    if (img.flags != 0u)
        img.static_start_supported = 0;
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
            if (len < 56u || img.segment_count == QRX_MACHO_MAX_SEGMENTS)
                return QRX_MACHO_ERR_SEGMENT;
            nsects = be32(c + 48u);
            /* Until sections/relocations are understood, execution is
             * restricted to sectionless synthetic segments. */
            if (nsects != 0u)
                img.static_start_supported = 0;
            if (nsects > (len - 56u) / 68u || len != 56u + nsects * 68u)
                return QRX_MACHO_ERR_SEGMENT;
            s = &img.segments[img.segment_count++];
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
                    img.entry = be32(c + pos); /* srr0 */
                    /* The prototype initializes a fresh CPU rather than
                     * restoring an arbitrary saved machine state. */
                    for (k = 1u; k < words; k++) {
                        if (be32(c + pos + 4u * k) != 0u)
                            img.static_start_supported = 0;
                    }
                    have_entry = 1;
                }
                if (flavor != PPC_THREAD_STATE)
                    img.static_start_supported = 0;
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
                img.has_dylinker = 1;
            else
                img.dylib_count++;
            img.static_start_supported = 0;
        } else {
            /* Inspection may skip unknown commands; execution must not. */
            img.static_start_supported = 0;
        }
        cursor += len;
    }
    if (cursor != 28u + cmdbytes)
        return QRX_MACHO_ERR_COMMAND;
    if (!have_entry || (img.entry & 3u))
        return QRX_MACHO_ERR_ENTRY;
    for (i = 0; i < img.segment_count; i++) {
        const qrx_macho_segment_t *s = &img.segments[i];
        if ((s->initprot & VM_PROT_EXECUTE) && img.entry >= s->vmaddr &&
            (uint64_t)img.entry + 4u <= (uint64_t)s->vmaddr + s->filesize)
            break;
    }
    if (i == img.segment_count)
        return QRX_MACHO_ERR_ENTRY;
    *out = img;
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

qrx_macho_run_status_t qrx_macho_run(const qrx_macho_t *img,
                                      uint8_t *memory, uint32_t size,
                                      uint32_t stack_base, uint32_t budget,
                                      qrx_macho_write_fn write_fn, void *ctx,
                                      qrx_macho_run_result_t *result)
{
    static const char program[] = "qrxmach";
    qrx_ppc_t ppc;
    macho_address_space_t space;
    uint32_t sp, name, left = budget, i;
    qrx_macho_status_t mapping;

    if (result)
        memset(result, 0, sizeof(*result));
    if (!img || !memory || !result || !write_fn || budget == 0u ||
        (stack_base & 15u) || (size & 15u) || stack_base >= size ||
        size - stack_base < 64u)
        return QRX_MACHO_RUN_ARGUMENT;
    if (!img->static_start_supported || img->has_dylinker || img->dylib_count)
        return QRX_MACHO_RUN_UNSUPPORTED;
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
