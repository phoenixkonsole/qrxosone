/*
 * QRXPPC - name -> A-line trap table for classic MacOS PPC imports (P2.3).
 *
 * A PEF fragment names its imports by library and symbol; the Code Fragment
 * Manager fixes the TOC slots to the resolved addresses.  This table maps the
 * InterfaceLib symbols of the StuffIt-Expander-5.5 application to the traps
 * qrx_tb_dispatch() serves (docs/QRXPPC_EXPANDER_IMPORTS.md is the full list).
 *
 * Kind: QRX_STUB_DIRECT means the symbol is the trap itself; the stub only
 * moves the arguments from r3.. into r4.. and raises the trap.
 * QRX_STUB_INDIRECT marks an InterfaceLib wrapper or inline call: the trap is
 * the right operation, but the argument shapes differ (ParamBlock vs
 * refNum/count/buffer, CurResFile vs explicit resource file), so these need a
 * dedicated wrapper before they may be used.
 */
#ifndef QRX_APPSTUB_H
#define QRX_APPSTUB_H

#include <stdint.h>

#define QRX_STUB_DIRECT 0u
#define QRX_STUB_INDIRECT 1u

typedef struct {
    const char *library;
    const char *name;
    uint16_t trap;
    uint8_t kind;
} qrx_appimport_t;

/* Symbol lookup by library and name; NULL when the table has no entry. */
const qrx_appimport_t *qrx_appimport_find(const char *library,
                                          const char *name);
unsigned qrx_appimport_count(void);

#endif /* QRX_APPSTUB_H */
