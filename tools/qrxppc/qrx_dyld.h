/* Restricted dyld substitute for PPC32 Mach-O images.
 *
 * This is not a dynamic linker.  It binds the pointer sections a classic
 * (non-lazy and lazy) linked PPC executable uses -- __nl_symbol_ptr and
 * __la_symbol_ptr -- by reading LC_SYMTAB/LC_DYSYMTAB and writing one guest
 * address per slot.  Undefined symbols are resolved against a caller-supplied
 * table and become small guest trampolines that enter the host dispatch
 * (r0 = 0x4000 | selector, `sc`).  Defined symbols inside the image are
 * written as their own virtual address, which is what the linker's stubs
 * expect for intra-image pointers.
 *
 * Absent on purpose: relocations, binding opcodes, weak symbols, two-level
 * namespace lookup, symbol interposition, Objective-C metadata, framework
 * loading and lazy binding at first call.  Everything is bound up front, so an
 * unresolved symbol fails the load instead of trapping later.
 */
#ifndef QRX_DYLD_H
#define QRX_DYLD_H

#include <stdint.h>

#include "qrx_macho.h"

/* Host call selectors passed to the trampolines.  They deliberately avoid the
 * BSD syscall numbers (1 exit, 4 write) the static path uses: the trampoline
 * loads QRX_DYLD_SEL_BASE | selector into r0. */
#define QRX_DYLD_SEL_BASE 0x4000u

enum {
    QRX_DYLD_SEL_WRITE = 1,
    QRX_DYLD_SEL_EXIT = 2,
    QRX_DYLD_SEL_ERRNO = 3,
    QRX_DYLD_SEL_STRLEN = 4
};

struct qrx_dyld_import {
    const char *name;  /* symbol name as spelled in the string table */
    uint32_t selector;
};

typedef struct {
    uint32_t slots;      /* pointer slots visited */
    uint32_t bound;      /* undefined symbols resolved from the table */
    uint32_t internal;   /* defined symbols written from the symbol table */
    uint32_t skipped;    /* local/absolute indirect entries */
    uint32_t unresolved; /* undefined symbols with no provider */
    char missing[48];    /* first unresolved name, truncated if necessary */
} qrx_dyld_result_t;

typedef enum {
    QRX_DYLD_OK = 0,
    QRX_DYLD_ERR_ARGUMENT,
    QRX_DYLD_ERR_IMAGE,   /* image is not dynamically bindable */
    QRX_DYLD_ERR_SYMBOLS, /* malformed symbol or indirect symbol table */
    QRX_DYLD_ERR_SPACE,   /* trampoline area invalid or too small */
    QRX_DYLD_ERR_UNRESOLVED
} qrx_dyld_status_t;

/* Bind img into memory.  Segments must already be mapped.  Trampolines and the
 * process errno word live in [trampoline_base, trampoline_base +
 * trampoline_size): the first 16 bytes hold globals, each trampoline is 12
 * bytes and is shared by all slots of the same import.  The area must lie in
 * guest memory and must not overlap a mapped segment or the stack.
 * Unresolved symbols make the call fail and are reported in *result. */
qrx_dyld_status_t qrx_dyld_bind(qrx_macho_t *image, uint8_t *memory,
                                uint32_t memory_size,
                                const struct qrx_dyld_import *imports,
                                uint32_t import_count,
                                uint32_t trampoline_base,
                                uint32_t trampoline_size,
                                qrx_dyld_result_t *result);

const char *qrx_dyld_status_string(qrx_dyld_status_t status);

/* Smallest host surface a classic PPC executable needs to print and exit. */
extern const struct qrx_dyld_import qrx_dyld_default_imports[];
extern const uint32_t qrx_dyld_default_import_count;

#endif
