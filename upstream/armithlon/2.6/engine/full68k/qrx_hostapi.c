#include "qrx_hostapi.h"
#include <string.h>
uint32_t qrx_hostapi_scratch;
void qrx_hostapi_context(QRXFullCPU *cpu, x86cpu *x86, uint32_t scratch){
    (void)cpu;(void)x86;
    if(scratch)qrx_hostapi_scratch=scratch;
}
/* _name@N and name both find name. */
const QRXHostApi *qrx_hostapi_find(const char *name){
    char plain[64];
    size_t n=strlen(name);
    if(n>=sizeof(plain))return NULL;
    if(name[0]=='_'){name++;n--;}
    memcpy(plain,name,n);plain[n]=0;
    char *at=strchr(plain,'@');
    if(at)*at=0;
    static const QRXHostApi *const tables[]={qrx_api_gl,qrx_api_glide,qrx_api_d3d,qrx_api_vk,qrx_api_vk_procs,NULL};
    for(unsigned t=0;tables[t];t++)
        for(const QRXHostApi *f=tables[t];f->name;f++)
            if(!strcmp(f->name,plain))return f;
    return NULL;
}
