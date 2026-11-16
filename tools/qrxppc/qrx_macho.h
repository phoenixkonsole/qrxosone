/* PPC32 Mach-O inspection and fixed-address mapping prototype.
 *
 * A separate, deliberately restricted static Darwin startup is provided;
 * dyld, relocations, imports, a full VM and framework APIs are absent.
 * The source image must remain alive and unmodified until mapping completes.
 */
#ifndef QRX_MACHO_H
#define QRX_MACHO_H

#include <stdint.h>

#define QRX_MACHO_MAX_SEGMENTS 128u

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
    const uint8_t *slice;
    uint32_t slice_size;
    uint32_t slice_offset;
    uint32_t cpu_subtype;
    uint32_t flags;
    uint32_t entry;
    uint32_t segment_count;
    uint32_t dylib_count;
    int has_dylinker;
    int static_start_supported; /* restricted load commands and initial state */
    qrx_macho_segment_t segments[QRX_MACHO_MAX_SEGMENTS];
} qrx_macho_t;

/* Supports big-endian 32-bit PPC MH_EXECUTE, thin or FAT_MAGIC. Requires a
 * valid LC_UNIXTHREAD with PPC_THREAD_STATE and an executable file-backed PC.
 * Other commands are structurally checked but not interpreted or executed. */
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
} qrx_macho_run_result_t;

qrx_macho_run_status_t qrx_macho_run(const qrx_macho_t *image,
                                      uint8_t *memory, uint32_t memory_size,
                                      uint32_t stack_base, uint32_t budget,
                                      qrx_macho_write_fn write_fn, void *ctx,
                                      qrx_macho_run_result_t *result);
const char *qrx_macho_run_status_string(qrx_macho_run_status_t status);
const char *qrx_macho_status_string(qrx_macho_status_t status);

#endif
