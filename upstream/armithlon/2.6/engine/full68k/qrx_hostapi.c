#include "qrx_hostapi.h"
#include <string.h>
uint32_t qrx_hostapi_scratch;
void qrx_hostapi_context(QRXFullCPU *cpu, x86cpu *x86, uint32_t scratch){
    (void)cpu;(void)x86;
    if(scratch)qrx_hostapi_scratch=scratch;
}
/* _name@N and name both find name. */
/* kernel32 functions so small and so often called (the C runtime of Visual C++ 2015 and later
 * calls them around every errno access) that a trip to the 68k side for each costs more than
 * the work: TLS slots and the last error in the calling thread's TEB (FS base), as Windows and
 * qrx-win32 keep them (TLS at +0x1000, 64 slots; last error at +0x34). */
static uint32_t k32_TlsGetValue(x86cpu *c,const uint32_t *a){return a[0]<64u?x86_rd32(c,c->sbase[X86_FS]+0x1000u+a[0]*4u):0u;}
static uint32_t k32_TlsSetValue(x86cpu *c,const uint32_t *a){if(a[0]<64u)x86_wr32(c,c->sbase[X86_FS]+0x1000u+a[0]*4u,a[1]);return 1;}
static uint32_t k32_GetLastError(x86cpu *c,const uint32_t *a){(void)a;return x86_rd32(c,c->sbase[X86_FS]+0x34u);}
static uint32_t k32_SetLastError(x86cpu *c,const uint32_t *a){x86_wr32(c,c->sbase[X86_FS]+0x34u,a[0]);return 0;}
const QRXHostApi qrx_api_k32[]={
    {"TlsGetValue",1,k32_TlsGetValue},{"TlsSetValue",2,k32_TlsSetValue},
    {"GetLastError",0,k32_GetLastError},{"SetLastError",1,k32_SetLastError},
    {NULL,0,NULL}
};
const QRXHostApi *qrx_hostapi_find(const char *name){
    char plain[64];
    size_t n=strlen(name);
    if(n>=sizeof(plain))return NULL;
    if(name[0]=='_'){name++;n--;}
    memcpy(plain,name,n);plain[n]=0;
    char *at=strchr(plain,'@');
    if(at)*at=0;
    static const QRXHostApi *const tables[]={qrx_api_k32,qrx_api_gl,qrx_api_glide,qrx_api_d3d,qrx_api_vk,qrx_api_vk_procs,NULL};
    for(unsigned t=0;tables[t];t++)
        for(const QRXHostApi *f=tables[t];f->name;f++)
            if(!strcmp(f->name,plain))return f;
    return NULL;
}
