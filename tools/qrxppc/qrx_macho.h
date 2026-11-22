/* PPC32 Mach-O inspection, mapping and restricted Darwin startup.
 *
 * Two startup levels exist.  The static one runs a sectionless executable
 * without any imports.  The dynamic one additionally binds undefined symbols
 * of __la_symbol_ptr/__nl_symbol_ptr sections to host-provided calls (see
 * qrx_dyld.h), which is the minimum a real Mac OS X executable needs.
 * Relocations, Objective-C, frameworks and a full VM remain absent.
 * The source image must remain alive and unmodified until mapping completes.
 */
#ifndef QRX_MACHO_H
#define QRX_MACHO_H

#include <stdint.h>

#define QRX_MACHO_MAX_SEGMENTS 128u
#define QRX_MACHO_MAX_SECTIONS 256u
#define QRX_MACHO_MAX_DYLIBS 32u
#define QRX_MACHO_DYLIB_NAME 64u

typedef enum {
    QRX_MACHO_OK = 0,
    QRX_MACHO_ERR_ARGUMENT,
    QRX_MACHO_ERR_FORMAT,
    QRX_MACHO_ERR_ARCH,
    QRX_MACHO_ERR_TRUNCATED,
    QRX_MACHO_ERR_COMMAND,
    QRX_MACHO_ERR_SEGMENT,
    QRX_MACHO_ERR_ENTRY,
    QRX_MACHO_ERR_MEMORY
} qrx_macho_status_t;

typedef struct {
    char name[17];
    uint32_t vmaddr, vmsize, fileoff, filesize, maxprot, initprot;
} qrx_macho_segment_t;

typedef struct {
    char sectname[17];
    char segname[17];
    uint32_t addr, size, offset, flags, reserved1, reserved2;
    uint32_t segment; /* index into qrx_macho_t.segments */
} qrx_macho_section_t;

typedef struct {
    char name[QRX_MACHO_DYLIB_NAME];
    uint32_t command;
} qrx_macho_dylib_t;

/* LC_SYMTAB and the part of LC_DYSYMTAB the binder needs. */
typedef struct {
    uint32_t symoff, nsyms, stroff, strsize;
    uint32_t indirectsymoff, nindirectsyms;
    uint32_t iundefsym, nundefsym;
    int present;   /* LC_SYMTAB seen */
    int indirect;  /* LC_DYSYMTAB seen with a valid indirect table */
} qrx_macho_symtab_t;

typedef struct {
    const uint8_t *slice;
    uint32_t slice_size;
    uint32_t slice_offset;
    uint32_t cpu_subtype;
    uint32_t flags;
    uint32_t entry;
    uint32_t segment_count;
    uint32_t section_count;
    uint32_t dylib_count;
    uint32_t dylib_name_count;
    int has_dylinker;
    int static_start_supported;  /* no sections, no imports, no dylibs at all */
    int dynamic_start_supported; /* linked image the restricted binder can bind */
    qrx_macho_symtab_t symtab;
    qrx_macho_segment_t segments[QRX_MACHO_MAX_SEGMENTS];
    qrx_macho_section_t sections[QRX_MACHO_MAX_SECTIONS];
    qrx_macho_dylib_t dylibs[QRX_MACHO_MAX_DYLIBS];
} qrx_macho_t;

/* Supports big-endian 32-bit PPC MH_EXECUTE, thin or FAT_MAGIC. Requires a
 * valid LC_UNIXTHREAD with PPC_THREAD_STATE and an executable file-backed PC.
 * Sections, LC_SYMTAB and LC_DYSYMTAB are recorded for the restricted binder;
 * other commands are structurally checked but not interpreted or executed.
 * Parses in place into *image, so the result is unspecified after a failure.
 * qrx_macho_t is large; callers on a small stack keep it static. */
qrx_macho_status_t qrx_macho_parse(qrx_macho_t *image, const uint8_t *data,
                                   uint32_t size);

/* Copy mapped segments at their preferred VM addresses and zero-fill their
 * tails. __PAGEZERO-like segments with no protection or file bytes are not
 * mapped. Validate all ranges and overlaps before making any changes. Input
 * and guest memory must not overlap. Mapping does not mean ready to execute. */
qrx_macho_status_t qrx_macho_map(const qrx_macho_t *image, uint8_t *memory,
                                 uint32_t memory_size);

/* Restricted process runner: argc=1, argv[0]="qrxmach", empty env/apple;
 * r1 points at argc, followed by argv, env and apple NULL terminators.
 * Only BSD write(4) to fd 1/2 and exit(1) are implemented. Unknown traps or
 * syscalls stop execution. The caller supplies separate guest memory, a
 * 16-byte-aligned stack region [stack_base, memory_size), an instruction
 * budget and an output callback. The callback returns 0 on complete success.
 * Segment initprot and stack permissions are enforced for interpreter
 * accesses and syscall buffers. This is not a security sandbox or a general
 * Darwin ABI implementation. */
typedef enum {
    QRX_MACHO_RUN_EXITED = 0,
    QRX_MACHO_RUN_ARGUMENT,
    QRX_MACHO_RUN_UNSUPPORTED,
    QRX_MACHO_RUN_MEMORY,
    QRX_MACHO_RUN_FAULT,
    QRX_MACHO_RUN_SYSCALL,
    QRX_MACHO_RUN_LIMIT
} qrx_macho_run_status_t;

typedef int (*qrx_macho_write_fn)(void *ctx, uint32_t fd,
                                  const uint8_t *bytes, uint32_t len);

typedef struct {
    uint32_t exit_code, instructions, syscall, pc, fault_addr;
    uint32_t imports_bound, imports_unresolved;
    char missing_import[48]; /* first unresolved symbol, empty if none */
} qrx_macho_run_result_t;

/* Binding configuration for a dynamically linked image.  The generated
 * trampolines and the process errno live in [trampoline_base,
 * trampoline_base + trampoline_size); 16 bytes are reserved for globals, so
 * at least 16 + 12 * imports bytes are required. */
typedef struct qrx_dyld_import qrx_dyld_import_t;

typedef struct {
    const qrx_dyld_import_t *imports;
    uint32_t import_count;
    uint32_t trampoline_base;
    uint32_t trampoline_size;
} qrx_macho_dynamic_t;

qrx_macho_run_status_t qrx_macho_run(const qrx_macho_t *image,
                                      uint8_t *memory, uint32_t memory_size,
                                      uint32_t stack_base, uint32_t budget,
                                      qrx_macho_write_fn write_fn, void *ctx,
                                      qrx_macho_run_result_t *result);

/* As qrx_macho_run, but binds a dynamically linked image first and dispatches
 * host-library calls (r0 = 0x4000 | selector) in addition to write(4)/exit(1).
 * The image must report dynamic_start_supported. */
qrx_macho_run_status_t qrx_macho_run_dynamic(
    const qrx_macho_t *image, uint8_t *memory, uint32_t memory_size,
    uint32_t stack_base, uint32_t budget, qrx_macho_write_fn write_fn,
    void *ctx, const qrx_macho_dynamic_t *dynamic,
    qrx_macho_run_result_t *result);
const char *qrx_macho_run_status_string(qrx_macho_run_status_t status);
const char *qrx_macho_status_string(qrx_macho_status_t status);

#endif
