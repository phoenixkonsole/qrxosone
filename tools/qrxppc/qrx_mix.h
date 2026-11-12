/*
 * QRXPPC P1.5 - mixed-mode bridge between 68k and PPC code.
 *
 * The bridge implements the subset of the PowerOpen / MacOS 9 calling
 * convention that a Mixed Mode glue stub needs.  The 68k side calls
 * qrx_mix_call(); the PPC code calls back into 68k code through registered
 * imports.  See qrx_mix.c for the ABI contract and its limits.
 *
 * Arguments are placed with a qrx_mix_sig_t: one character per parameter in
 * declaration order, `i`/`p` for a 32-bit integer or pointer, `l` for a
 * 64-bit integer and `d` for an IEEE double; the return type is `v`, `i`,
 * `l` or `f`.  Integer arguments fill r3..r10, floating-point arguments fill
 * f1..f8 (separate cursors), and every argument that does not fit is written
 * to the parameter area at 24(r1), 4-byte aligned (`l`/`d` 8-byte aligned).
 *
 * A call can also enter through a Mixed Mode routine descriptor held in guest
 * memory (qrx_mix_rd_resolve / qrx_mix_rd_call); the descriptor supplies the
 * entry point and, for register-based records, also the placement itself:
 * qrx_mix_rd_procinfo_sig() decodes the record's ProcInfo and
 * qrx_mix_rd_invoke_procinfo() calls through it without a caller-supplied
 * signature.  A signature stays necessary for what ProcInfo cannot express.
 * The same
 * descriptors address imports: PPC code puts a registered descriptor address
 * in r0 instead of an import index, the bridge resolves its 68k record and
 * reports that entry point in qrx_mix_args_t.proc.  An import registered
 * without a signature takes its placement from the descriptor's ProcInfo on
 * every call.  The other direction uses
 * the same descriptors: 68k code that holds a UniversalProcPtr calls
 * qrx_mix_rd_invoke(), which selects the descriptor's PPC record, so a 68k
 * callback can re-enter PPC code.
 */
#ifndef QRX_MIX_H
#define QRX_MIX_H

#include "qrx_ppc.h"

#define QRX_MIX_ARGS 8u          /* r3..r10 / f1..f8 */
#define QRX_MIX_IMPORTS 16u      /* import indices carried in r0 at `sc` */
#define QRX_MIX_PROCS 8u         /* imports addressed by routine descriptor */
#define QRX_MIX_LINKAGE 24u      /* PowerOpen linkage area at 0(r1) */
#define QRX_MIX_FRAME 64u        /* stack space between nested glue frames */
#define QRX_MIX_DEPTH_MAX 16u    /* recursion guard for nested calls */
#define QRX_MIX_SIG_MAX 16u      /* parameters per signature */
#define QRX_MIX_STACK_WORDS 32u  /* words the bridge will read or write */

/*
 * Routine descriptors (Universal Interfaces / MixedMode.h, 2-byte packed).
 *
 *   0  UInt16 goMixedModeTrap          must be QRX_MIX_RD_TRAP
 *   2  SInt8  version                  must be QRX_MIX_RD_VERSION
 *   3  UInt8  routineDescriptorFlags   must be 0 (no dispatched routines)
 *   4  UInt32 reserved1                ignored
 *   8  UInt8  reserved2                ignored
 *   9  UInt8  selectorInfo             ignored
 *   10 UInt16 routineCount             index of the last record (0 = one)
 *   12 RoutineRecord records[], QRX_MIX_RD_RECORD bytes each:
 *         0  UInt32 procInfo           see qrx_mix_rd_procinfo_sig
 *         4  SInt8  reserved1          ignored
 *         5  SInt8  ISA                QRX_MIX_RD_ISA_68K/_PPC
 *         6  UInt16 routineFlags       see QRX_MIX_RD_FLAG_*
 *         8  UInt32 procDescriptor     address or descriptor-relative offset
 *         12 UInt32 reserved2          ignored
 *         16 UInt32 selector           ignored
 */
#define QRX_MIX_RD_TRAP 0xAAFEu             /* goMixedModeTrap: A-line */
#define QRX_MIX_RD_VERSION 7                /* kRoutineDescriptorVersion */
#define QRX_MIX_RD_HEADER 12u               /* header before the records */
#define QRX_MIX_RD_RECORD 20u               /* packed RoutineRecord */
#define QRX_MIX_RD_MAX_RECORDS 8u           /* routineCount is 0..7 */
#define QRX_MIX_RD_ISA_68K 0                /* kM68kISA */
#define QRX_MIX_RD_ISA_PPC 1                /* kPowerPCISA */
#define QRX_MIX_RD_FLAG_RELATIVE 0x0001u    /* kProcDescriptorIsRelative */
#define QRX_MIX_RD_FLAG_NEEDS_PREP 0x0002u  /* kFragmentNeedsPreparing */
#define QRX_MIX_RD_FLAG_NATIVE 0x0004u      /* kUseNativeISA */
#define QRX_MIX_RD_FLAG_SELECTOR 0x0008u    /* kPassSelector */
#define QRX_MIX_RD_FLAG_INDEXABLE 0x0010u   /* kSelectorIsIndexable */

/*
 * ProcInfoType (MixedMode.h) as far as register-based PPC records use it.
 * Bits 0..3 hold the calling convention, bits 4..5 the result size code, bits
 * 6..10 the result register and each following 5-bit field one parameter in
 * order: bits 0..1 its size code, bits 2..4 a register hint.  The result
 * register and the hints are not used - PowerOpen assigns r3..r10 and f1..f8
 * by position - so only the size codes describe the placement.
 */
#define QRX_MIX_PI_CONVENTION_MASK 0x0000000Fu
#define QRX_MIX_PI_REGISTER_BASED 2u        /* kRegisterBased */
#define QRX_MIX_PI_RESULT_PHASE 4u          /* kResultSizePhase */
#define QRX_MIX_PI_RESULT_MASK 0x00000030u
#define QRX_MIX_PI_PARAM_PHASE 11u          /* kRegisterParameterPhase */
#define QRX_MIX_PI_PARAM_WIDTH 5u           /* kRegisterParameterWidth */
#define QRX_MIX_PI_PARAM_SIZE_MASK 0x3u
#define QRX_MIX_PI_PARAM_MAX 4u             /* fields at bits 11..30 */

typedef enum {
    QRX_MIX_OK = 0,
    QRX_MIX_ERR_ARG,      /* invalid argument, stack bounds or alignment */
    QRX_MIX_ERR_SP,       /* callee did not restore the stack pointer */
    QRX_MIX_ERR_IMPORT,   /* unregistered import index at `sc` */
    QRX_MIX_ERR_TRAP,     /* trap loop that never returns to the bridge */
    QRX_MIX_ERR_RUN,      /* halt, fault or illegal instruction in PPC code */
    QRX_MIX_ERR_LIMIT,    /* instruction budget exhausted */
    QRX_MIX_ERR_SIG,      /* malformed signature or impossible placement */
    QRX_MIX_ERR_RD        /* malformed or unsupported routine descriptor */
} qrx_mix_status_t;

typedef struct {
    char params[QRX_MIX_SIG_MAX];   /* 'i', 'p', 'l', 'd' in argument order */
    unsigned nparams;
    char ret;                       /* 'v', 'i', 'l' or 'f' */
} qrx_mix_sig_t;

typedef struct {
    uint32_t rtoc;                  /* r2, PowerOpen RTOC */
    uint32_t gpr[QRX_MIX_ARGS];     /* integer-class arguments in order */
    unsigned gpr_count;
    double fpr[QRX_MIX_ARGS];       /* double arguments in order */
    unsigned fpr_count;
    uint32_t stack[QRX_MIX_STACK_WORDS]; /* parameter area words, big-endian */
    unsigned stack_count;
    uint32_t sp;                    /* imports: guest address of 24(r1) */
    uint32_t proc;                  /* descriptor imports: 68k entry point */
} qrx_mix_args_t;

typedef struct {
    uint32_t gpr[2];                /* r3, r4 */
    double fpr;                     /* f1 */
} qrx_mix_result_t;

/*
 * 68k callback.  With a signature the return value becomes r3 (`i`), the
 * high half of r3:r4 (`l`) or is ignored (`v`, `f`); extra->gpr[1] supplies
 * the low half of a 64-bit result and extra->fpr supplies f1.  Without a
 * signature the bridge forwards r3..r10 and f1..f8 verbatim and takes the
 * return value as r3, extra->gpr[1] as r4 and extra->fpr as f1.
 */
typedef uint32_t (*qrx_mix_import_fn)(void *ctx, const qrx_mix_args_t *args,
                                      qrx_mix_result_t *extra);

typedef struct {
    qrx_ppc_t *ppc;
    uint32_t glue_addr;             /* holds the return-to-68k `sc` word */
    uint32_t sp_top;                /* upper stack bound, 16-byte aligned */
    uint32_t sp_limit;              /* lower stack bound */
    unsigned depth;                 /* nested call depth */
    qrx_mix_import_fn imports[QRX_MIX_IMPORTS];
    void *import_ctx[QRX_MIX_IMPORTS];
    qrx_mix_sig_t sigs[QRX_MIX_IMPORTS];
    unsigned sig_used[QRX_MIX_IMPORTS];
    uint32_t import_desc[QRX_MIX_IMPORTS];
    unsigned import_desc_used[QRX_MIX_IMPORTS]; /* placement from ProcInfo */
    uint32_t proc_desc[QRX_MIX_PROCS];
    qrx_mix_import_fn proc_fn[QRX_MIX_PROCS];
    void *proc_ctx[QRX_MIX_PROCS];
    qrx_mix_sig_t proc_sigs[QRX_MIX_PROCS];
    unsigned proc_sig_used[QRX_MIX_PROCS];
    unsigned proc_sig_derived[QRX_MIX_PROCS]; /* placement from ProcInfo */
    unsigned proc_used[QRX_MIX_PROCS];
} qrx_mix_t;

/* Parse "idip" plus a return code; rejects unknown types and too many
 * parameters.  The result can be passed to qrx_mix_register()/qrx_mix_call()
 * directly and is copied by both. */
qrx_mix_status_t qrx_mix_sig_parse(qrx_mix_sig_t *sig, const char *params,
                                   char ret);
qrx_mix_status_t qrx_mix_init(qrx_mix_t *mix, qrx_ppc_t *ppc,
                              uint32_t glue_addr, uint32_t sp_top,
                              uint32_t sp_limit);
/* Register an import reached by index.  With a signature the signature
 * decides the placement and `descriptor` is ignored, so pass 0.  Without a
 * signature but with a descriptor, the placement is derived from that
 * descriptor's ProcInfo exactly as qrx_mix_register_proc() does (the PPC
 * record's when it has one, otherwise the 68k record's) and re-derived on every
 * call; an undecodable ProcInfo fails the registration with QRX_MIX_ERR_RD, and
 * a descriptor below QRX_MIX_IMPORTS would be mistaken for an index and is
 * rejected.  Without either the import receives the raw register state (legacy
 * behaviour).  PPC code always puts the index in r0 before `sc`, so args->proc
 * stays 0. */
qrx_mix_status_t qrx_mix_register(qrx_mix_t *mix, unsigned index,
                                  uint32_t descriptor,
                                  qrx_mix_import_fn fn, void *ctx,
                                  const qrx_mix_sig_t *sig);
/* Register a 68k routine identified by its routine descriptor.  PPC code then
 * puts the descriptor address (not an import index) in r0 before `sc`; the
 * bridge resolves the descriptor's 68k record, calls fn and passes that record's
 * entry point as args->proc.  The descriptor must be valid and lie at or above
 * QRX_MIX_IMPORTS; descriptor imports are limited to QRX_MIX_PROCS.
 * sig may be NULL: the placement is then derived from the record's ProcInfo
 * (the PPC record's when the descriptor has one, otherwise the 68k record's)
 * and re-derived on every call.  A ProcInfo that cannot describe the placement
 * makes the registration fail with QRX_MIX_ERR_RD.  An explicit signature wins
 * over ProcInfo. */
qrx_mix_status_t qrx_mix_register_proc(qrx_mix_t *mix, uint32_t descriptor,
                                       qrx_mix_import_fn fn, void *ctx,
                                       const qrx_mix_sig_t *sig);
/* With a signature, args->gpr/fpr/stack must hold exactly the arguments the
 * signature places in registers and in the parameter area; with a NULL
 * signature the register counts are loaded verbatim (legacy behaviour). */
qrx_mix_status_t qrx_mix_call(qrx_mix_t *mix, uint32_t entry,
                              const qrx_mix_sig_t *sig,
                              const qrx_mix_args_t *args,
                              qrx_mix_result_t *result, uint32_t budget);

typedef struct {
    uint32_t descriptor;    /* guest address of the RoutineDescriptor */
    uint32_t record;        /* guest address of the selected RoutineRecord */
    uint32_t entry;         /* resolved entry point */
    uint32_t proc_info;     /* raw ProcInfoType, not interpreted */
    uint16_t routine_flags;
    unsigned record_count;  /* records present (routineCount + 1) */
    int isa;                /* ISA of the selected record */
} qrx_mix_rd_t;

/* Validate a routine descriptor in guest memory and select the first record
 * whose ISA matches `isa`.  Rejected: a trap word other than 0xAAFE, a version
 * other than 7, nonzero routineDescriptorFlags (dispatched descriptors), a
 * record count beyond QRX_MIX_RD_MAX_RECORDS, a truncated table, no record
 * for the requested ISA, kFragmentNeedsPreparing, selector flags, unknown flag
 * bits, and entry points that are zero, unaligned or outside memory. */
qrx_mix_status_t qrx_mix_rd_resolve(qrx_mix_t *mix, uint32_t descriptor,
                                    int isa, qrx_mix_rd_t *out);
/* Resolve a PPC descriptor and call its entry point; other ISAs are rejected
 * because this core executes PowerPC code only.  sig may be NULL: like
 * qrx_mix_rd_invoke(), the record's ProcInfo then supplies the placement, and
 * an undecodable ProcInfo is QRX_MIX_ERR_RD rather than a raw-register call. */
qrx_mix_status_t qrx_mix_rd_call(qrx_mix_t *mix, uint32_t descriptor, int isa,
                                 const qrx_mix_sig_t *sig,
                                 const qrx_mix_args_t *args,
                                 qrx_mix_result_t *result, uint32_t budget);
/* Entry point for 68k code that holds a UniversalProcPtr.  68k code knows the
 * descriptor address but not the ISA of its records, so the bridge selects the
 * PPC record itself and runs it with the given signature and arguments; a
 * descriptor without a PPC record is QRX_MIX_ERR_RD.  Intended for the 68k side
 * of the bridge, i.e. from inside an import callback: the nested frame hangs
 * below the caller's r1, the CPU state is saved and restored around the call,
 * and failures leave the outer PPC code untouched, so the callback can carry
 * on.  Nested calls count against QRX_MIX_DEPTH_MAX, and every call gets its
 * own instruction budget.  sig may be NULL: the PPC record's ProcInfo then
 * supplies the placement exactly like qrx_mix_rd_invoke_procinfo(), and an
 * undecodable ProcInfo is QRX_MIX_ERR_RD instead of a raw-register call. */
qrx_mix_status_t qrx_mix_rd_invoke(qrx_mix_t *mix, uint32_t descriptor,
                                   const qrx_mix_sig_t *sig,
                                   const qrx_mix_args_t *args,
                                   qrx_mix_result_t *result, uint32_t budget);
/* Derive argument placement from a resolved record's ProcInfo.  Supported is
 * kRegisterBased with one to four parameters of 1, 2 or 4 bytes and a result
 * of 0, 1, 2 or 4 bytes: each parameter becomes an `i` (the next integer
 * register), a 0-byte result `v` and anything else `i`.  The size code cannot
 * tell a double from a 32-bit integer, cannot express 64-bit integers and
 * stops after four parameters, so such routines still need a signature of
 * their own.  Rejected with QRX_MIX_ERR_RD: every other calling convention
 * (stack-based, dispatched, kSpecialCase), a parameter field that carries a
 * register hint but size code 0, and a fifth parameter, whose bits would lie
 * beyond bit 31.  The signature is written only on success. */
qrx_mix_status_t qrx_mix_rd_procinfo_sig(const qrx_mix_rd_t *rd,
                                         qrx_mix_sig_t *sig);
/* Resolve the descriptor's PPC record, derive the signature from its ProcInfo
 * and call it; identical to qrx_mix_rd_invoke() with a NULL signature, and
 * nesting, stack frame and state restore are the same. */
qrx_mix_status_t qrx_mix_rd_invoke_procinfo(qrx_mix_t *mix,
                                            uint32_t descriptor,
                                            const qrx_mix_args_t *args,
                                            qrx_mix_result_t *result,
                                            uint32_t budget);

#endif /* QRX_MIX_H */
