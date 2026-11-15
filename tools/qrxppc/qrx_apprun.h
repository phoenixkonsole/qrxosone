/*
 * QRXPPC - classic MacOS application runner (P2.3).
 *
 * Loads a MacBinary application (or a bare PEF container), brings the Toolbox
 * HLE up around it and runs its main fragment through the Mixed Mode bridge.
 * Every Toolbox call the application makes is logged; the first trap the HLE
 * does not implement ends the run with QRX_APPRUN_UNIMPLEMENTED instead of
 * letting the application continue with a made-up result.
 *
 * The seam is the HLE import: the fragment executes `sc` with the import index
 * in r0 (0 selects the Toolbox dispatcher) and the A-line trap number in r3.
 * A real MacOS fragment reaches InterfaceLib through a routine descriptor and
 * Mixed Mode instead; that path is reported as an unregistered `sc` selector,
 * with the selector and r3 in the detail text.  There is no 68k container
 * support (P2.1-open): an application without a 'cfrg' PEF fragment is refused
 * with QRX_APPRUN_ERR_68K when its resource fork carries 'CODE' resources.
 */
#ifndef QRX_APPRUN_H
#define QRX_APPRUN_H

#include "qrx_toolbox.h"
#include "qrx_macapp.h"
#include "qrx_mix.h"

#define QRX_APPRUN_TRACE 64u      /* most recent traps kept for inspection */
#define QRX_APPRUN_MAX_IMPORTS 512u /* same ceiling as qrx_pef.h */
#define QRX_APPRUN_PSEUDO_TRAP 0x7E00u /* pseudo trap of an import stub */
#define QRX_APPRUN_DETAIL 160u    /* explanation for a refusal or stop */

typedef enum {
    QRX_APPRUN_OK = 0,
    QRX_APPRUN_ERR_ARG,        /* NULL pointer or impossible layout */
    QRX_APPRUN_ERR_MEM,        /* PEF sections do not fit guest memory */
    QRX_APPRUN_ERR_MIX,        /* bridge initialisation failed */
    QRX_APPRUN_ERR_IMAGE,      /* neither MacBinary, PEF nor a usable app */
    QRX_APPRUN_ERR_68K,        /* 68k application: 'CODE' resources, no cfrg */
    QRX_APPRUN_ERR_LOAD,       /* fragment parsed but has no main TVector */
    QRX_APPRUN_UNIMPLEMENTED,  /* stopped at the first unimplemented trap */
    QRX_APPRUN_STOPPED,        /* stopped by the environment, not by a trap */
    QRX_APPRUN_EXITED,         /* main returned to the bridge */
    QRX_APPRUN_FAULT,          /* halt, fault or illegal instruction */
    QRX_APPRUN_LIMIT,          /* instruction budget exhausted */
    QRX_APPRUN_TRAP_LOOP,      /* trap loop that never returns */
    QRX_APPRUN_ERR_IMPORT,     /* `sc` selector is not a registered import */
    QRX_APPRUN_ERR_RUN         /* other bridge or dispatch failure */
} qrx_apprun_status_t;

typedef struct {
    uint32_t trap;        /* A-line trap number from r3 */
    uint32_t selector;    /* r0 at the `sc` (import index) */
    uint32_t arg0;        /* r4 at the call */
    uint32_t arg1;        /* r5 at the call */
    uint32_t result;      /* value the dispatcher returned */
    uint8_t implemented;  /* 0 when the dispatcher answered QRX_MM_UNKNOWN */
} qrx_apprun_trap_t;

typedef struct {
    qrx_ppc_t *ppc;
    qrx_tb_t tb;
    qrx_mix_t mix;
    qrx_pef_t pef;
    uint32_t load_base;
    uint32_t tvector;
    uint32_t entry;
    uint32_t toc;
    uint32_t init_entry;      /* CFM init routine, 0 when the fragment has none */
    uint32_t init_toc;
    unsigned ntrace;          /* entries currently in trace[] */
    unsigned ntrace_total;    /* every trap seen, including overwritten ones */
    unsigned nimplemented;    /* traps the HLE served */
    uint32_t unimplemented_trap; /* first trap the HLE could not serve */
    qrx_apprun_trap_t trace[QRX_APPRUN_TRACE];
    uint32_t stub_area;       /* first stub of the import pool, 0 = unused */
    uint32_t stub_addr[QRX_APPRUN_MAX_IMPORTS]; /* TVector per import */
    unsigned nimport_stub;    /* imports with a real trap stub */
    unsigned nimport_wrapper; /* indirect imports still needing a wrapper */
    unsigned nimport_missing; /* imports without any HLE entry */
    char detail[QRX_APPRUN_DETAIL];
} qrx_apprun_t;

/* Bring up Memory Manager zone, Toolbox HLE and the bridge.  glue_addr holds
 * the `sc` the bridge returns to; sp_top/sp_limit bound the application
 * stack.  All addresses are guest addresses inside ppc->mem. */
qrx_apprun_status_t qrx_apprun_init(qrx_apprun_t *run, qrx_ppc_t *ppc,
                                    uint32_t zone_base, uint32_t zone_size,
                                    uint32_t glue_addr, uint32_t sp_top,
                                    uint32_t sp_limit);

/* Load a MacBinary application (data and resource fork in one image) or a
 * bare PEF container whose data fork starts with 'Joy!'.  Sections are placed
 * at load_base; the image must stay valid while the run is alive. */
qrx_apprun_status_t qrx_apprun_load(qrx_apprun_t *run, const uint8_t *image,
                                    uint32_t size, uint32_t load_base);

/* Run the loaded fragment's main TVector.  Kernel r2 is taken from the
 * TVector, so a real fragment sees its own TOC.  budget bounds the executed
 * instructions. */
qrx_apprun_status_t qrx_apprun_start(qrx_apprun_t *run, uint32_t budget);

const char *qrx_apprun_strerror(qrx_apprun_status_t status);

/* Name and library of the import whose stub raised the unimplemented trap, or
 * NULL when the stop came from a real trap.  Valid until the next load. */
const char *qrx_apprun_missing_import(const qrx_apprun_t *run,
                                      const char **library);

#endif /* QRX_APPRUN_H */
