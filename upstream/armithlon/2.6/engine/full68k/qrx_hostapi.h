#ifndef QRX_HOSTAPI_H
#define QRX_HOSTAPI_H
/* Graphics APIs for x86 programs, implemented on the host: a call from x86 code
 * goes straight to one of these functions, without passing through the 68k. */
#include "qrx86core.h"
#include "qrx68100_full.h"
typedef uint32_t (*QRXHostApiFn)(x86cpu *c, const uint32_t *a);
typedef struct QRXHostApi {
    const char *name;
    int nargs;             /* stdcall: dwords popped by the callee; 0 for cdecl */
    QRXHostApiFn fn;
} QRXHostApi;
/* Accepts decorated names too (_grDrawTriangle@12). NULL when not implemented. */
const QRXHostApi *qrx_hostapi_find(const char *name);
/* scratch: 4 KiB of x86 memory the host may fill, for returned strings. */
void qrx_hostapi_context(QRXFullCPU *cpu, x86cpu *x86, uint32_t scratch);
extern const QRXHostApi qrx_api_gl[], qrx_api_glide[], qrx_api_d3d[], qrx_api_vk[], qrx_api_vk_procs[];
extern uint32_t qrx_hostapi_scratch;
/* A function needing x86 memory sets the request and returns; the door runs the
 * guest allocator and calls the function again. */
extern uint32_t qrx_hostapi_alloc_request;
void qrx_hostapi_alloc_result(uint32_t addr,uint32_t size);
/* Vtable slots of host COM objects, indexed by thunk number. */
extern const QRXHostApi *qrx_d3d_thunks[];
static inline float api_float(uint32_t v){float f;__builtin_memcpy(&f,&v,4);return f;}
static inline double api_double(const uint32_t *a){uint64_t u=(uint64_t)a[0]|((uint64_t)a[1]<<32);double d;__builtin_memcpy(&d,&u,8);return d;}
#endif
