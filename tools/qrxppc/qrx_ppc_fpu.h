/* QRXPPC P1.3 floating-point helpers and FPSCR definitions. */
#ifndef QRX_PPC_FPU_H
#define QRX_PPC_FPU_H

#include "qrx_ppc.h"

/* FPSCR bit positions as numeric 32-bit masks. */
#define QRX_FPSCR_FX       0x80000000u
#define QRX_FPSCR_FEX      0x40000000u
#define QRX_FPSCR_VX       0x20000000u
#define QRX_FPSCR_OX       0x10000000u
#define QRX_FPSCR_UX       0x08000000u
#define QRX_FPSCR_ZX       0x04000000u
#define QRX_FPSCR_XX       0x02000000u
#define QRX_FPSCR_VXSNAN   0x01000000u
#define QRX_FPSCR_VXISI    0x00800000u
#define QRX_FPSCR_VXIDI    0x00400000u
#define QRX_FPSCR_VXZDZ    0x00200000u
#define QRX_FPSCR_VXIMZ    0x00100000u
#define QRX_FPSCR_VXVC     0x00080000u
#define QRX_FPSCR_FR       0x00040000u
#define QRX_FPSCR_FI       0x00020000u
#define QRX_FPSCR_FPRF     0x0001F000u
#define QRX_FPSCR_RN_MASK  0x00000003u

uint64_t qrx_fp_add(qrx_ppc_t *p, uint64_t a, uint64_t b, int subtract);
uint64_t qrx_fp_mul(qrx_ppc_t *p, uint64_t a, uint64_t b);
uint64_t qrx_fp_div(qrx_ppc_t *p, uint64_t a, uint64_t b);
uint64_t qrx_fp_frsp(qrx_ppc_t *p, uint64_t a);
uint32_t qrx_fp_fctiw(qrx_ppc_t *p, uint64_t a, int truncate);
void qrx_fp_compare(qrx_ppc_t *p, unsigned field,
                    uint64_t a, uint64_t b, int ordered);
void qrx_fp_class(qrx_ppc_t *p, uint64_t bits);
void qrx_fp_mtfsf(qrx_ppc_t *p, uint64_t src, unsigned fm);

#endif /* QRX_PPC_FPU_H */
