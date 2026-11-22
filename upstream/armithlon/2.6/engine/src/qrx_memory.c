#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "qrx_memory.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

static uint64_t read_cgroup_limit(const char *path) {
    char line[128];
    FILE *f=fopen(path,"r");
    if(!f)return 0;
    int ok=fgets(line,sizeof(line),f)!=NULL;
    fclose(f);
    if(!ok || line[0]=='m')return 0; /* 'max' = unrestricted */
    errno=0;
    char *end=NULL;
    unsigned long long n=strtoull(line,&end,10);
    if(errno || end==line || (*end && *end!='\n'))return 0;
    if(n>=UINT64_C(0x7ffffffffffff000))return 0; /* cgroup v1 unlimited sentinel */
    return (uint64_t)n;
}
uint64_t qrx_host_memory_bytes(void) {
#ifdef __APPLE__
    uint64_t bytes=0; size_t n=sizeof(bytes);
    if(sysctlbyname("hw.memsize",&bytes,&n,NULL,0)==0 && n==sizeof(bytes))return bytes;
#endif
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    long pages=sysconf(_SC_PHYS_PAGES),size=sysconf(_SC_PAGESIZE);
    if(pages>0 && size>0)return (uint64_t)(unsigned long)pages*(uint64_t)(unsigned long)size;
#endif
    return 0;
}
uint64_t qrx_host_memory_available_bytes(void) {
#if defined(_SC_AVPHYS_PAGES) && defined(_SC_PAGESIZE)
    long pages=sysconf(_SC_AVPHYS_PAGES),size=sysconf(_SC_PAGESIZE);
    if(pages>0 && size>0)return (uint64_t)(unsigned long)pages*(uint64_t)(unsigned long)size;
#endif
    return 0;
}
uint64_t qrx_effective_host_memory_bytes(void) {
    uint64_t physical=qrx_host_memory_bytes();
#if defined(__linux__)
    uint64_t lim=read_cgroup_limit("/sys/fs/cgroup/memory.max");
    if(!lim)lim=read_cgroup_limit("/sys/fs/cgroup/memory/memory.limit_in_bytes");
    if(lim && (!physical || lim<physical))physical=lim;
#endif
    return physical;
}
bool qrx_ram_validate(uint64_t n){return n>=QRX_RAM_MIN_BYTES && n<=QRX_RAM_MAX_BYTES && n%4096==0 && n<=SIZE_MAX;}
size_t qrx_ram_auto_bytes(void) {
    uint64_t physical=qrx_effective_host_memory_bytes();
    if(!physical)physical=UINT64_C(512)*1024*1024; /* documented conservative fallback */
    uint64_t target=(physical/4u)&~(UINT64_C(1024)*1024-1u);
    if(target<QRX_RAM_MIN_BYTES)target=QRX_RAM_MIN_BYTES;
    if(target>QRX_RAM_MAX_BYTES)target=QRX_RAM_MAX_BYTES;
    return (size_t)target;
}
bool qrx_ram_allocate(QRXHostMemory *mem, size_t bytes) {
    if(!mem || bytes<1024 || bytes>QRX_RAM_MAX_BYTES || bytes>SIZE_MAX)return false;
    memset(mem,0,sizeof(*mem));
#if defined(MAP_ANONYMOUS) || defined(MAP_ANON)
    int flags=MAP_PRIVATE;
#if defined(MAP_ANONYMOUS)
    flags|=MAP_ANONYMOUS;
#else
    flags|=MAP_ANON;
#endif
#ifdef MAP_NORESERVE
    flags|=MAP_NORESERVE;
#endif
    void *p=mmap(NULL,bytes,PROT_READ|PROT_WRITE,flags,-1,0);
    if(p!=MAP_FAILED) {mem->addr=p;mem->bytes=bytes;mem->mapped=true;return true;}
#endif
    /* Fallback for platforms without anonymous mmap. Some memory is eagerly
     * allocated; a failed allocation is surfaced, never silently reduced. */
    mem->addr=calloc(1,bytes);
    if(!mem->addr)return false;
    mem->bytes=bytes;
    return true;
}
void qrx_ram_release(QRXHostMemory *m) {
    if(!m)return;
    if(m->addr){if(m->mapped)munmap(m->addr,m->bytes);else free(m->addr);}
    memset(m,0,sizeof(*m));
}
