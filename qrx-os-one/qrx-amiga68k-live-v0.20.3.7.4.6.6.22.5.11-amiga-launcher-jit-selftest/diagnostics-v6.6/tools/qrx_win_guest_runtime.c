/* QRX OS One - QRX86 ReactOS guest runtime init/bridge stage
 * Release 0.20.3.7.4.6.6.22.4
 *
 * This component extends the 22.3 PE32 mapper into a real runtime hand-off:
 *   - two-pass recursive DLL mapping (cycle-safe)
 *   - PE32 HIGHLOW relocations
 *   - import resolution by name/ordinal + forwarded exports
 *   - selected ntdll imports rebound to deterministic QRX host-bridge thunks
 *   - TLS callback discovery
 *   - dependency-first DLL_PROCESS_ATTACH / DllMain call ordering
 *   - ntdll host-owned process bootstrap plan (TEB/PEB/heap/last-error)
 *   - serialized mapped address-space + init-call handoff for QRX86 execution
 *
 * It deliberately does not claim that x86 callbacks have run unless an executor
 * consumes the emitted handoff.  The purpose of this stage is to make the loader,
 * DllMain/TLS ordering and ntdll host bridge concrete and deterministic rather
 * than treating missing imports as ad-hoc WinRT stubs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

/* Deadwood AROS/m68k SDK compatibility: the selected native SDK can link
 * libc while its public stdio/stdlib headers omit ISO-C declarations. Keep
 * FILE opaque and declare only the ABI surface used by this translation unit. */
#if defined(__AROS__)
typedef void QRX_FILE;
extern QRX_FILE *fopen(const char *, const char *);
extern int fclose(QRX_FILE *);
extern int fseek(QRX_FILE *, long, int);
extern long ftell(QRX_FILE *);
extern size_t fread(void *, size_t, size_t, QRX_FILE *);
extern size_t fwrite(const void *, size_t, size_t, QRX_FILE *);
extern int printf(const char *, ...);
extern int puts(const char *);
extern int snprintf(char *, size_t, const char *, ...);
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern unsigned long strtoul(const char *, char **, int);
extern void exit(int);
#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif
#define FILE QRX_FILE
#endif

#define QRX_MAX_MODULES 160
#define QRX_MAX_PATH 512
#define QRX_MAX_NAME 128
#define QRX_MAX_DEPTH 96
#define QRX_MAX_TLS_CALLBACKS 64
#define QRX_MAX_INIT_CALLS 1024
#define QRX_MAX_BRIDGES 128
#define QRX_DEFAULT_SYSTEM32 "QRX:Windows/ReactOS/System32"
#define QRX_GUEST_FALLBACK_BASE 0x10000000u
#define QRX_GUEST_ALIGN 0x00010000u
#define QRX_BRIDGE_BASE 0x7ff00000u
#define QRX_BRIDGE_STRIDE 0x20u
#define QRX_TEB_BASE 0x7ffde000u
#define QRX_PEB_BASE 0x7ffdf000u
#define QRX_HEAP_HANDLE 0x7f010000u
#define QRX_PROCESS_ATTACH 1u

#define IMAGE_FILE_DLL 0x2000u
#define IMAGE_REL_BASED_ABSOLUTE 0u
#define IMAGE_REL_BASED_HIGHLOW 3u
#define IMAGE_ORDINAL_FLAG32 0x80000000u

#define QRX_INIT_HOST_NTDLL 1u
#define QRX_INIT_TLS       2u
#define QRX_INIT_DLLMAIN   3u
#define QRX_INIT_EXE_TLS   4u
#define QRX_INIT_EXE_ENTRY 5u

#define QRX_BRIDGE_STDCALL 1u
#define QRX_BRIDGE_CDECL   2u
#define QRX_BRIDGE_NORETURN 4u

typedef struct QRXPEMeta {
    uint16_t machine, sections, opt_size, characteristics;
    uint32_t entry_rva, image_base, size_image, size_headers;
    uint32_t export_rva, export_size, import_rva, import_size;
    uint32_t reloc_rva, reloc_size, tls_rva, tls_size;
    uint16_t subsystem;
    uint32_t sec_table_off;
} QRXPEMeta;

typedef struct QRXModule {
    char name[QRX_MAX_NAME];
    char path[QRX_MAX_PATH];
    uint8_t *file;
    size_t file_size;
    uint8_t *image;
    uint32_t image_size;
    uint32_t preferred_base, guest_base, entry_rva;
    uint32_t export_rva, export_size, import_rva, import_size;
    uint32_t reloc_rva, reloc_size, tls_rva, tls_size;
    uint16_t characteristics, subsystem;
    unsigned mapped;
    unsigned deps_scanned;
    unsigned imports_resolved;
    unsigned imports_seen;
    unsigned forwarders;
    unsigned relocations;
    unsigned unsupported_relocs;
    unsigned tls_count;
    uint32_t tls_callbacks[QRX_MAX_TLS_CALLBACKS];
} QRXModule;

typedef struct QRXBridgeDef {
    const char *name;
    uint16_t argc;
    uint16_t flags;
} QRXBridgeDef;

typedef struct QRXBridgeUse {
    uint32_t addr;
    uint16_t def_index;
    uint16_t reserved;
} QRXBridgeUse;

typedef struct QRXInitCall {
    uint32_t kind;
    uint32_t module_index;
    uint32_t target;
    uint32_t hinst;
    uint32_t reason;
    uint32_t reserved;
} QRXInitCall;

typedef struct QRXLoader {
    QRXModule modules[QRX_MAX_MODULES];
    unsigned count;
    unsigned unresolved;
    unsigned missing_files;
    unsigned cycles;
    unsigned total_imports;
    unsigned total_resolved;
    unsigned total_forwarders;
    unsigned total_relocations;
    unsigned total_tls;
    unsigned bridge_bindings;
    uint32_t next_base;
    char system32[QRX_MAX_PATH];
    int verbose;
    int strict_bridge;
    QRXBridgeUse bridge_uses[QRX_MAX_BRIDGES];
    unsigned bridge_use_count;
    QRXInitCall init_calls[QRX_MAX_INIT_CALLS];
    unsigned init_count;
    unsigned init_mark[QRX_MAX_MODULES];
} QRXLoader;

/* The first bridge block intentionally covers the ntdll primitives that show up
 * very early in ReactOS process/DLL initialization.  Non-listed ntdll exports
 * still resolve to guest ntdll, so this is a host bridge, not a fake whole-DLL
 * replacement. */
static const QRXBridgeDef g_bridge_defs[] = {
    {"RtlZeroMemory",2,QRX_BRIDGE_STDCALL},
    {"RtlFillMemory",3,QRX_BRIDGE_STDCALL},
    {"RtlMoveMemory",3,QRX_BRIDGE_STDCALL},
    {"RtlCopyMemory",3,QRX_BRIDGE_STDCALL},
    {"RtlCompareMemory",3,QRX_BRIDGE_STDCALL},
    {"RtlInitUnicodeString",2,QRX_BRIDGE_STDCALL},
    {"RtlInitAnsiString",2,QRX_BRIDGE_STDCALL},
    {"RtlGetProcessHeap",0,QRX_BRIDGE_STDCALL},
    {"RtlAllocateHeap",3,QRX_BRIDGE_STDCALL},
    {"RtlReAllocateHeap",4,QRX_BRIDGE_STDCALL},
    {"RtlFreeHeap",3,QRX_BRIDGE_STDCALL},
    {"RtlSizeHeap",3,QRX_BRIDGE_STDCALL},
    {"RtlGetLastWin32Error",0,QRX_BRIDGE_STDCALL},
    {"RtlSetLastWin32Error",1,QRX_BRIDGE_STDCALL},
    {"RtlNtStatusToDosError",1,QRX_BRIDGE_STDCALL},
    {"RtlNtStatusToDosErrorNoTeb",1,QRX_BRIDGE_STDCALL},
    {"RtlUnicodeStringToAnsiString",3,QRX_BRIDGE_STDCALL},
    {"RtlAnsiStringToUnicodeString",3,QRX_BRIDGE_STDCALL},
    {"RtlFreeAnsiString",1,QRX_BRIDGE_STDCALL},
    {"RtlFreeUnicodeString",1,QRX_BRIDGE_STDCALL},
    {"RtlCreateUnicodeString",2,QRX_BRIDGE_STDCALL},
    {"RtlDosPathNameToNtPathName_U",4,QRX_BRIDGE_STDCALL},
    {"RtlAcquirePebLock",0,QRX_BRIDGE_STDCALL},
    {"RtlReleasePebLock",0,QRX_BRIDGE_STDCALL},
    {"RtlEnterCriticalSection",1,QRX_BRIDGE_STDCALL},
    {"RtlLeaveCriticalSection",1,QRX_BRIDGE_STDCALL},
    {"RtlInitializeCriticalSection",1,QRX_BRIDGE_STDCALL},
    {"RtlDeleteCriticalSection",1,QRX_BRIDGE_STDCALL},
    {"RtlInitializeCriticalSectionAndSpinCount",2,QRX_BRIDGE_STDCALL},
    {"RtlInterlockedCompareExchange64",3,QRX_BRIDGE_STDCALL},
    {"DbgPrint",0,QRX_BRIDGE_CDECL},
    {"DbgPrintEx",0,QRX_BRIDGE_CDECL},
    {"vDbgPrintEx",0,QRX_BRIDGE_CDECL},
    {"vDbgPrintExWithPrefix",0,QRX_BRIDGE_CDECL},
    {"RtlRaiseStatus",1,QRX_BRIDGE_STDCALL|QRX_BRIDGE_NORETURN},
    {"RtlExitUserProcess",1,QRX_BRIDGE_STDCALL|QRX_BRIDGE_NORETURN},
    {"NtTerminateProcess",2,QRX_BRIDGE_STDCALL|QRX_BRIDGE_NORETURN},
    {"NtClose",1,QRX_BRIDGE_STDCALL},
    {"NtDelayExecution",2,QRX_BRIDGE_STDCALL},
    {"NtQueryPerformanceCounter",2,QRX_BRIDGE_STDCALL},
    {"NtQuerySystemTime",1,QRX_BRIDGE_STDCALL},
    {"NtYieldExecution",0,QRX_BRIDGE_STDCALL},
    {"NtAllocateVirtualMemory",6,QRX_BRIDGE_STDCALL},
    {"NtFreeVirtualMemory",4,QRX_BRIDGE_STDCALL},
    {"NtProtectVirtualMemory",5,QRX_BRIDGE_STDCALL},
    {"NtQueryVirtualMemory",6,QRX_BRIDGE_STDCALL},
    {"NtQueryInformationProcess",5,QRX_BRIDGE_STDCALL},
    {"NtSetInformationThread",4,QRX_BRIDGE_STDCALL},
    {"NtQueryInformationThread",5,QRX_BRIDGE_STDCALL},
    {"NtOpenFile",6,QRX_BRIDGE_STDCALL},
    {"NtCreateFile",11,QRX_BRIDGE_STDCALL},
    {"NtReadFile",9,QRX_BRIDGE_STDCALL},
    {"NtWriteFile",9,QRX_BRIDGE_STDCALL},
    {"NtQueryInformationFile",5,QRX_BRIDGE_STDCALL},
    {"NtSetInformationFile",5,QRX_BRIDGE_STDCALL},
    {"NtQueryAttributesFile",2,QRX_BRIDGE_STDCALL},
    {"NtQueryFullAttributesFile",2,QRX_BRIDGE_STDCALL},
    {NULL,0,0}
};

static uint16_t rd16(const uint8_t *p){ return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
static uint32_t rd32(const uint8_t *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static void wr32(uint8_t *p,uint32_t v){ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static uint32_t align_up(uint32_t v,uint32_t a){ return (v+a-1u)&~(a-1u); }
static int qrx_tolower(int c){ return tolower((unsigned char)c); }
static int qrx_stricmp(const char*a,const char*b){ while(*a&&*b){int x=qrx_tolower(*a),y=qrx_tolower(*b);if(x!=y)return x-y;++a;++b;}return qrx_tolower(*a)-qrx_tolower(*b); }
static void qrx_lower(char*s){ while(*s){*s=(char)qrx_tolower(*s);++s;} }
static const char*qrx_basename(const char*p){const char*b=p,*q;for(q=p;*q;++q)if(*q=='/'||*q=='\\'||*q==':')b=q+1;return b;}
static int qrx_has_dot(const char*s){return strchr(s,'.')!=NULL;}
static int range_ok(size_t size,uint32_t off,uint32_t len){return (uint64_t)off+(uint64_t)len<=(uint64_t)size;}

static uint8_t*read_whole(const char*path,size_t*out_size){FILE*f=fopen(path,"rb");long n;uint8_t*b;if(!f)return NULL;if(fseek(f,0,SEEK_END)!=0){fclose(f);return NULL;}n=ftell(f);if(n<=0){fclose(f);return NULL;}if(fseek(f,0,SEEK_SET)!=0){fclose(f);return NULL;}b=(uint8_t*)malloc((size_t)n);if(!b){fclose(f);return NULL;}if(fread(b,1,(size_t)n,f)!=(size_t)n){free(b);fclose(f);return NULL;}fclose(f);*out_size=(size_t)n;return b;}

static int pe_meta(const uint8_t*b,size_t n,QRXPEMeta*m,char*why,size_t cap){uint32_t pe,opt;uint16_t magic;memset(m,0,sizeof(*m));if(n<0x40||b[0]!='M'||b[1]!='Z'){snprintf(why,cap,"not MZ");return 0;}pe=rd32(b+0x3c);if(!range_ok(n,pe,24)||memcmp(b+pe,"PE\0\0",4)!=0){snprintf(why,cap,"not PE");return 0;}m->machine=rd16(b+pe+4);m->sections=rd16(b+pe+6);m->opt_size=rd16(b+pe+20);m->characteristics=rd16(b+pe+22);opt=pe+24;if(!range_ok(n,opt,m->opt_size)||m->opt_size<96){snprintf(why,cap,"bad optional header");return 0;}magic=rd16(b+opt);if(magic!=0x10b){snprintf(why,cap,"not PE32");return 0;}if(m->machine!=0x014c){snprintf(why,cap,"not i386");return 0;}m->entry_rva=rd32(b+opt+16);m->image_base=rd32(b+opt+28);m->size_image=rd32(b+opt+56);m->size_headers=rd32(b+opt+60);m->subsystem=rd16(b+opt+68);if(m->opt_size>=104){m->export_rva=rd32(b+opt+96);m->export_size=rd32(b+opt+100);}if(m->opt_size>=112){m->import_rva=rd32(b+opt+104);m->import_size=rd32(b+opt+108);}if(m->opt_size>=144){m->reloc_rva=rd32(b+opt+136);m->reloc_size=rd32(b+opt+140);}if(m->opt_size>=176){m->tls_rva=rd32(b+opt+168);m->tls_size=rd32(b+opt+172);}m->sec_table_off=opt+m->opt_size;if(!m->sections||m->sections>96||!range_ok(n,m->sec_table_off,(uint32_t)m->sections*40u)){snprintf(why,cap,"bad section table");return 0;}if(!m->size_image||m->size_image>0x20000000u){snprintf(why,cap,"invalid SizeOfImage");return 0;}return 1;}

static int guest_range_used(const QRXLoader*l,uint32_t base,uint32_t size){uint64_t a0=base,a1=(uint64_t)base+size;unsigned i;for(i=0;i<l->count;i++){const QRXModule*m=&l->modules[i];uint64_t b0=m->guest_base,b1=(uint64_t)m->guest_base+m->image_size;if(m->guest_base&&a0<b1&&b0<a1)return 1;}return 0;}
static uint32_t choose_guest_base(QRXLoader*l,uint32_t preferred,uint32_t size){uint32_t b;size=align_up(size,QRX_GUEST_ALIGN);if(preferred>=0x00010000u&&!guest_range_used(l,preferred,size))return preferred;b=align_up(l->next_base,QRX_GUEST_ALIGN);while(guest_range_used(l,b,size)){if(b>0x70000000u-size)return 0;b=align_up(b+size,QRX_GUEST_ALIGN);}l->next_base=b+size;return b;}

static int map_image(QRXModule*m,const QRXPEMeta*meta,char*why,size_t cap){unsigned i;m->image=(uint8_t*)calloc(1,meta->size_image);if(!m->image){snprintf(why,cap,"out of memory");return 0;}m->image_size=meta->size_image;{uint32_t nh=meta->size_headers;if(nh>m->file_size)nh=(uint32_t)m->file_size;if(nh>m->image_size)nh=m->image_size;memcpy(m->image,m->file,nh);}for(i=0;i<meta->sections;i++){uint32_t o=meta->sec_table_off+i*40u,va=rd32(m->file+o+12),rawsize=rd32(m->file+o+16),raw=rd32(m->file+o+20),take=rawsize;if(!rawsize)continue;if(va>=m->image_size||raw>=m->file_size)continue;if((uint64_t)va+take>m->image_size)take=m->image_size-va;if((uint64_t)raw+take>m->file_size)take=(uint32_t)(m->file_size-raw);memcpy(m->image+va,m->file+raw,take);}return 1;}

static int apply_relocs(QRXModule*m,char*why,size_t cap){uint32_t pos,end;int64_t delta=(int64_t)(uint64_t)m->guest_base-(int64_t)(uint64_t)m->preferred_base;if(!delta)return 1;if(!m->reloc_rva||!m->reloc_size){snprintf(why,cap,"relocation required but no relocation directory");return 0;}if((uint64_t)m->reloc_rva+m->reloc_size>m->image_size){snprintf(why,cap,"relocation directory out of image");return 0;}pos=m->reloc_rva;end=m->reloc_rva+m->reloc_size;while(pos+8<=end){uint32_t page=rd32(m->image+pos),block=rd32(m->image+pos+4),q;if(block<8||pos+block>end){snprintf(why,cap,"bad relocation block");return 0;}for(q=pos+8;q+2<=pos+block;q+=2){uint16_t e=rd16(m->image+q);unsigned type=e>>12;uint32_t rva=page+(e&0xfffu);if(type==IMAGE_REL_BASED_ABSOLUTE)continue;if(type==IMAGE_REL_BASED_HIGHLOW){uint32_t v;if(rva+4>m->image_size){snprintf(why,cap,"HIGHLOW out of image");return 0;}v=rd32(m->image+rva);wr32(m->image+rva,(uint32_t)((int64_t)(uint64_t)v+delta));m->relocations++;}else m->unsupported_relocs++;}pos+=block;}if(m->unsupported_relocs){snprintf(why,cap,"unsupported relocation types=%u",m->unsupported_relocs);return 0;}return 1;}

static QRXModule*find_module(QRXLoader*l,const char*name){unsigned i;for(i=0;i<l->count;i++)if(qrx_stricmp(l->modules[i].name,name)==0)return &l->modules[i];return NULL;}
static int module_index(QRXLoader*l,QRXModule*m){if(!m)return -1;return (int)(m-l->modules);}
static int cstr_image(const QRXModule*m,uint32_t rva,char*out,size_t cap){size_t i=0;if(rva>=m->image_size||cap<2)return 0;while(rva+i<m->image_size&&i+1<cap){char c=(char)m->image[rva+i];out[i++]=c;if(!c)return 1;}out[cap-1]=0;return 0;}

static QRXModule*map_dependency(QRXLoader*l,const char*name,unsigned depth);

static QRXModule*map_path(QRXLoader*l,const char*path,const char*logical,unsigned depth){QRXModule*m;QRXPEMeta meta;char why[192];uint32_t base;if(depth>QRX_MAX_DEPTH){printf("dependency depth exceeded\n");return NULL;}m=find_module(l,logical);if(m){l->cycles++;return m;}if(l->count>=QRX_MAX_MODULES){printf("module limit exceeded\n");return NULL;}m=&l->modules[l->count++];memset(m,0,sizeof(*m));snprintf(m->name,sizeof(m->name),"%s",logical);qrx_lower(m->name);snprintf(m->path,sizeof(m->path),"%s",path);m->file=read_whole(path,&m->file_size);if(!m->file){printf("MISSING %s (%s)\n",m->name,path);l->missing_files++;return m;}if(!pe_meta(m->file,m->file_size,&meta,why,sizeof(why))){printf("PE ERROR %s: %s\n",m->name,why);return m;}m->preferred_base=meta.image_base;m->entry_rva=meta.entry_rva;m->export_rva=meta.export_rva;m->export_size=meta.export_size;m->import_rva=meta.import_rva;m->import_size=meta.import_size;m->reloc_rva=meta.reloc_rva;m->reloc_size=meta.reloc_size;m->tls_rva=meta.tls_rva;m->tls_size=meta.tls_size;m->characteristics=meta.characteristics;m->subsystem=meta.subsystem;base=choose_guest_base(l,meta.image_base,meta.size_image);if(!base){printf("PE ERROR %s: no guest VA\n",m->name);return m;}m->guest_base=base;if(!map_image(m,&meta,why,sizeof(why))){printf("PE ERROR %s: %s\n",m->name,why);return m;}if(!apply_relocs(m,why,sizeof(why))){printf("PE ERROR %s: %s\n",m->name,why);return m;}m->mapped=1;l->total_relocations+=m->relocations;if(l->verbose)printf("MAP %-24s base=%08lx pref=%08lx size=%lu entry=%08lx%s%s\n",m->name,(unsigned long)m->guest_base,(unsigned long)m->preferred_base,(unsigned long)m->image_size,(unsigned long)(m->guest_base+m->entry_rva),(m->characteristics&IMAGE_FILE_DLL)?" DLL":" EXE",m->tls_rva?" TLS":"");return m;}

static QRXModule*map_dependency(QRXLoader*l,const char*name,unsigned depth){char n[QRX_MAX_NAME],path[QRX_MAX_PATH];QRXModule*m;snprintf(n,sizeof(n),"%s",name);qrx_lower(n);m=find_module(l,n);if(m)return m;if(strlen(l->system32)+strlen(n)+2u>sizeof(path))return NULL;strcpy(path,l->system32);strcat(path,"/");strcat(path,n);return map_path(l,path,n,depth);}

/* Phase 1: recursively map all DLLs without requiring dependencies to be fully
 * resolved first.  ReactOS has legitimate DLL cycles; the 22.3 one-pass loader
 * could reject these before the graph was complete. */
static int scan_dependencies(QRXLoader*l,QRXModule*m,unsigned depth){uint32_t pos,end;if(!m||!m->mapped)return 0;if(m->deps_scanned)return 1;if(depth>QRX_MAX_DEPTH)return 0;m->deps_scanned=1;if(!m->import_rva||!m->import_size)return 1;if((uint64_t)m->import_rva+m->import_size>m->image_size)return 0;pos=m->import_rva;end=m->import_rva+m->import_size;while(pos+20<=end){uint32_t oft=rd32(m->image+pos),name_rva=rd32(m->image+pos+12),ft=rd32(m->image+pos+16);char dll[QRX_MAX_NAME];QRXModule*dep;if(!(oft||name_rva||ft))break;if(!cstr_image(m,name_rva,dll,sizeof(dll)))return 0;qrx_lower(dll);dep=map_dependency(l,dll,depth+1);if(!dep||!dep->mapped)return 0;if(!scan_dependencies(l,dep,depth+1))return 0;pos+=20;}return 1;}

static int bridge_def_index(const char*name){int i;for(i=0;g_bridge_defs[i].name;i++)if(strcmp(g_bridge_defs[i].name,name)==0)return i;return -1;}
static uint32_t bridge_addr_for(QRXLoader*l,int def_index){unsigned i;uint32_t addr;if(def_index<0)return 0;addr=QRX_BRIDGE_BASE+(uint32_t)def_index*QRX_BRIDGE_STRIDE;for(i=0;i<l->bridge_use_count;i++)if(l->bridge_uses[i].def_index==(uint16_t)def_index)return l->bridge_uses[i].addr;if(l->bridge_use_count<QRX_MAX_BRIDGES){l->bridge_uses[l->bridge_use_count].addr=addr;l->bridge_uses[l->bridge_use_count].def_index=(uint16_t)def_index;l->bridge_use_count++;}return addr;}

static int resolve_forwarder(QRXLoader*l,const char*forward,unsigned depth,uint32_t*addr);
static uint32_t resolve_export(QRXLoader*l,QRXModule*m,const char*name,uint16_t ordinal,int byordinal,unsigned depth,int*ok){uint8_t*e;uint32_t base,nfunc,nname,funcs,names,ords,index=0xffffffffu,frva;unsigned i;*ok=0;if(depth>QRX_MAX_DEPTH||!m||!m->mapped||!m->export_rva||m->export_rva+40>m->image_size)return 0;e=m->image+m->export_rva;base=rd32(e+16);nfunc=rd32(e+20);nname=rd32(e+24);funcs=rd32(e+28);names=rd32(e+32);ords=rd32(e+36);if(!nfunc||funcs+4u*nfunc>m->image_size)return 0;if(byordinal){if((uint32_t)ordinal<base||(uint32_t)ordinal>=base+nfunc)return 0;index=(uint32_t)ordinal-base;}else{if(!name||names+4u*nname>m->image_size||ords+2u*nname>m->image_size)return 0;for(i=0;i<nname;i++){uint32_t nrva=rd32(m->image+names+4u*i);char nb[QRX_MAX_NAME];if(!cstr_image(m,nrva,nb,sizeof(nb)))continue;if(strcmp(nb,name)==0){index=rd16(m->image+ords+2u*i);break;}}if(index==0xffffffffu||index>=nfunc)return 0;}frva=rd32(m->image+funcs+4u*index);if(!frva)return 0;if(m->export_size&&frva>=m->export_rva&&frva<m->export_rva+m->export_size){char fw[QRX_MAX_PATH];uint32_t a=0;if(!cstr_image(m,frva,fw,sizeof(fw)))return 0;if(!resolve_forwarder(l,fw,depth+1,&a))return 0;m->forwarders++;l->total_forwarders++;*ok=1;return a;}if(frva>=m->image_size)return 0;*ok=1;return m->guest_base+frva;}

static int resolve_forwarder(QRXLoader*l,const char*forward,unsigned depth,uint32_t*addr){char mod[QRX_MAX_NAME],sym[QRX_MAX_NAME],tmp[QRX_MAX_NAME];const char*dot=strrchr(forward,'.');QRXModule*m;int ok=0;if(!dot||dot==forward||!dot[1])return 0;if((size_t)(dot-forward)>=sizeof(mod))return 0;memcpy(mod,forward,(size_t)(dot-forward));mod[dot-forward]=0;snprintf(sym,sizeof(sym),"%s",dot+1);snprintf(tmp,sizeof(tmp),"%s",mod);if(!qrx_has_dot(tmp))strncat(tmp,".dll",sizeof(tmp)-strlen(tmp)-1);qrx_lower(tmp);m=find_module(l,tmp);if(!m||!m->mapped)return 0;if(sym[0]=='#'){unsigned long o=strtoul(sym+1,NULL,10);*addr=resolve_export(l,m,NULL,(uint16_t)o,1,depth+1,&ok);}else *addr=resolve_export(l,m,sym,0,0,depth+1,&ok);return ok;}

/* Phase 2: resolve every import after the complete module graph is mapped. */
static int resolve_imports(QRXLoader*l,QRXModule*m){uint32_t pos,end;if(!m||!m->mapped)return 0;if(m->imports_resolved)return 1;if(!m->import_rva||!m->import_size){m->imports_resolved=1;return 1;}if((uint64_t)m->import_rva+m->import_size>m->image_size)return 0;pos=m->import_rva;end=m->import_rva+m->import_size;while(pos+20<=end){uint32_t oft=rd32(m->image+pos),name_rva=rd32(m->image+pos+12),ft=rd32(m->image+pos+16),ilt,iat;char dll[QRX_MAX_NAME];QRXModule*dep;if(!(oft||name_rva||ft))break;if(!cstr_image(m,name_rva,dll,sizeof(dll)))return 0;qrx_lower(dll);dep=find_module(l,dll);if(!dep||!dep->mapped){printf("UNRESOLVED DLL %s for %s\n",dll,m->name);l->unresolved++;return 0;}ilt=oft?oft:ft;iat=ft;while(1){uint32_t v,addr=0;int ok=0;if(ilt+4>m->image_size||iat+4>m->image_size)return 0;v=rd32(m->image+ilt);if(!v)break;m->imports_seen++;l->total_imports++;if(v&IMAGE_ORDINAL_FLAG32){addr=resolve_export(l,dep,NULL,(uint16_t)(v&0xffffu),1,0,&ok);if(!ok&&l->verbose)printf("UNRESOLVED %s!#%u\n",dll,(unsigned)(v&0xffffu));}else{char fn[QRX_MAX_NAME];uint32_t hn=v;if(hn+2>m->image_size||!cstr_image(m,hn+2,fn,sizeof(fn)))return 0;if(strcmp(dll,"ntdll.dll")==0){int bi=bridge_def_index(fn);if(bi>=0){addr=bridge_addr_for(l,bi);ok=1;l->bridge_bindings++;if(l->verbose)printf("BRIDGE %-24s -> %08lx (%s imports)\n",fn,(unsigned long)addr,m->name);}else if(l->strict_bridge){if(l->verbose)printf("UNBRIDGED ntdll!%s requested by %s\n",fn,m->name);ok=0;}else addr=resolve_export(l,dep,fn,0,0,0,&ok);}else addr=resolve_export(l,dep,fn,0,0,0,&ok);if(!ok&&l->verbose)printf("UNRESOLVED %s!%s\n",dll,fn);}if(!ok){l->unresolved++;return 0;}wr32(m->image+iat,addr);l->total_resolved++;ilt+=4;iat+=4;}pos+=20;}m->imports_resolved=1;return 1;}

static int parse_tls(QRXLoader*l,QRXModule*m){uint32_t cbva,cbrva,i;if(!m||!m->mapped||!m->tls_rva||m->tls_rva+24>m->image_size)return 1;cbva=rd32(m->image+m->tls_rva+12);if(!cbva)return 1;if(cbva<m->guest_base||cbva>=m->guest_base+m->image_size){if(l->verbose)printf("TLS WARN %s callbacks VA=%08lx outside module\n",m->name,(unsigned long)cbva);return 1;}cbrva=cbva-m->guest_base;for(i=0;i<QRX_MAX_TLS_CALLBACKS;i++){uint32_t f;if(cbrva+4>m->image_size)break;f=rd32(m->image+cbrva);cbrva+=4;if(!f)break;m->tls_callbacks[m->tls_count++]=f;l->total_tls++;}if(l->verbose&&m->tls_count)printf("TLS %-24s callbacks=%u\n",m->name,m->tls_count);return 1;}

static int add_init(QRXLoader*l,uint32_t kind,unsigned mi,uint32_t target,uint32_t hinst,uint32_t reason){QRXInitCall*c;if(l->init_count>=QRX_MAX_INIT_CALLS)return 0;c=&l->init_calls[l->init_count++];c->kind=kind;c->module_index=mi;c->target=target;c->hinst=hinst;c->reason=reason;c->reserved=0;return 1;}

static void visit_init(QRXLoader*l,unsigned mi){QRXModule*m;uint32_t pos,end;if(mi>=l->count)return;if(l->init_mark[mi]==2)return;if(l->init_mark[mi]==1){l->cycles++;return;}l->init_mark[mi]=1;m=&l->modules[mi];if(m->import_rva&&m->import_size&&(uint64_t)m->import_rva+m->import_size<=m->image_size){pos=m->import_rva;end=m->import_rva+m->import_size;while(pos+20<=end){uint32_t oft=rd32(m->image+pos),name_rva=rd32(m->image+pos+12),ft=rd32(m->image+pos+16);char dll[QRX_MAX_NAME];QRXModule*d;int di;if(!(oft||name_rva||ft))break;if(cstr_image(m,name_rva,dll,sizeof(dll))){qrx_lower(dll);d=find_module(l,dll);di=module_index(l,d);if(di>=0&&(unsigned)di!=mi)visit_init(l,(unsigned)di);}pos+=20;}}
    /* ntdll process bootstrap is host-owned.  We still map guest ntdll for
     * exports and non-bridged code paths, but do not execute its DllMain here. */
    if(strcmp(m->name,"ntdll.dll")==0){add_init(l,QRX_INIT_HOST_NTDLL,mi,0,m->guest_base,QRX_PROCESS_ATTACH);}else if(m->characteristics&IMAGE_FILE_DLL){unsigned t;for(t=0;t<m->tls_count;t++)add_init(l,QRX_INIT_TLS,mi,m->tls_callbacks[t],m->guest_base,QRX_PROCESS_ATTACH);if(m->entry_rva)add_init(l,QRX_INIT_DLLMAIN,mi,m->guest_base+m->entry_rva,m->guest_base,QRX_PROCESS_ATTACH);}l->init_mark[mi]=2;}

static void build_init_plan(QRXLoader*l,QRXModule*root){unsigned i,ri=(unsigned)module_index(l,root);memset(l->init_mark,0,sizeof(l->init_mark));visit_init(l,ri);for(i=0;i<root->tls_count;i++)add_init(l,QRX_INIT_EXE_TLS,ri,root->tls_callbacks[i],root->guest_base,QRX_PROCESS_ATTACH);if(root->entry_rva)add_init(l,QRX_INIT_EXE_ENTRY,ri,root->guest_base+root->entry_rva,root->guest_base,0);}

static const char*init_kind_name(uint32_t k){switch(k){case QRX_INIT_HOST_NTDLL:return"HOST_NTDLL_BOOTSTRAP";case QRX_INIT_TLS:return"DLL_TLS_PROCESS_ATTACH";case QRX_INIT_DLLMAIN:return"DLLMAIN_PROCESS_ATTACH";case QRX_INIT_EXE_TLS:return"EXE_TLS_PROCESS_ATTACH";case QRX_INIT_EXE_ENTRY:return"EXE_ENTRY";default:return"UNKNOWN";}}

static void print_init_plan(QRXLoader*l){unsigned i;puts("QRXWinGuestRuntime: initialization plan");printf("  host TEB=%08lx PEB=%08lx process-heap=%08lx\n",(unsigned long)QRX_TEB_BASE,(unsigned long)QRX_PEB_BASE,(unsigned long)QRX_HEAP_HANDLE);for(i=0;i<l->init_count;i++){QRXInitCall*c=&l->init_calls[i];const char*n=(c->module_index<l->count)?l->modules[c->module_index].name:"?";printf("  %03u %-26s %-22s target=%08lx hinst=%08lx reason=%lu\n",i,init_kind_name(c->kind),n,(unsigned long)c->target,(unsigned long)c->hinst,(unsigned long)c->reason);}}

static void print_bridges(QRXLoader*l){unsigned i;puts("QRXWinGuestRuntime: ntdll host bridge bindings");for(i=0;i<l->bridge_use_count;i++){QRXBridgeUse*u=&l->bridge_uses[i];const QRXBridgeDef*d=&g_bridge_defs[u->def_index];printf("  %08lx  %-36s argc=%u abi=%s%s\n",(unsigned long)u->addr,d->name,(unsigned)d->argc,(d->flags&QRX_BRIDGE_CDECL)?"cdecl":"stdcall",(d->flags&QRX_BRIDGE_NORETURN)?" noreturn":"");}}

/* Handoff format is intentionally simple and endian-neutral-on-disk (all scalar
 * fields are little-endian because the consumer is the i386 QRX86 runtime).
 * The AROS/m68k producer writes bytes explicitly rather than host-endian structs. */
static int put_u32(FILE*f,uint32_t v){uint8_t b[4];wr32(b,v);return fwrite(b,1,4,f)==4;}
static int put_name(FILE*f,const char*s,size_t n){char b[128];size_t m=n;if(m>sizeof(b))m=sizeof(b);memset(b,0,m);if(s)strncpy(b,s,m-1);return fwrite(b,1,m,f)==m;}
static int emit_handoff(QRXLoader*l,QRXModule*root,const char*path){FILE*f;unsigned i;uint32_t rooti=(uint32_t)module_index(l,root);f=fopen(path,"wb");if(!f){printf("cannot create handoff: %s\n",path);return 0;}if(fwrite("QRXGRT1\0",1,8,f)!=8)goto fail;if(!put_u32(f,1)||!put_u32(f,l->count)||!put_u32(f,l->bridge_use_count)||!put_u32(f,l->init_count)||!put_u32(f,rooti)||!put_u32(f,QRX_TEB_BASE)||!put_u32(f,QRX_PEB_BASE)||!put_u32(f,QRX_HEAP_HANDLE))goto fail;for(i=0;i<l->count;i++){QRXModule*m=&l->modules[i];if(!put_u32(f,m->guest_base)||!put_u32(f,m->image_size)||!put_u32(f,m->entry_rva)||!put_u32(f,m->characteristics)||!put_u32(f,m->tls_count)||!put_name(f,m->name,64))goto fail;}for(i=0;i<l->bridge_use_count;i++){QRXBridgeUse*u=&l->bridge_uses[i];const QRXBridgeDef*d=&g_bridge_defs[u->def_index];if(!put_u32(f,u->addr)||!put_u32(f,u->def_index)||!put_u32(f,d->argc)||!put_u32(f,d->flags)||!put_name(f,d->name,64))goto fail;}for(i=0;i<l->init_count;i++){QRXInitCall*c=&l->init_calls[i];if(!put_u32(f,c->kind)||!put_u32(f,c->module_index)||!put_u32(f,c->target)||!put_u32(f,c->hinst)||!put_u32(f,c->reason)||!put_u32(f,c->reserved))goto fail;}for(i=0;i<l->count;i++){QRXModule*m=&l->modules[i];if(fwrite(m->image,1,m->image_size,f)!=m->image_size)goto fail;}if(fclose(f)!=0)return 0;printf("QRXWinGuestRuntime: handoff=%s modules=%u init-calls=%u bridges=%u\n",path,l->count,l->init_count,l->bridge_use_count);return 1;fail:fclose(f);printf("failed writing handoff: %s\n",path);return 0;}

static void free_loader(QRXLoader*l){unsigned i;for(i=0;i<l->count;i++){free(l->modules[i].file);free(l->modules[i].image);l->modules[i].file=NULL;l->modules[i].image=NULL;}}

static void usage(void){puts("QRXWinGuestRuntime - QRX86 ReactOS DllMain/TLS + ntdll host-bridge stage");puts("Usage: QRX:C/QRXWinGuestRuntime [options] EXE");puts("  --system32 PATH       ReactOS System32 directory");puts("  --quiet               less mapping output");puts("  --strict-bridge       reject ntdll imports not implemented by host bridge");puts("  --bridge-dump         print active ntdll bridge bindings");puts("  --init-plan           print dependency-first TLS/DllMain plan");puts("  --emit-handoff PATH   serialize mapped images + bridge + init plan for QRX86");puts("Exit 0 means mapping/link/init-plan construction passed. It does not mean guest callbacks executed.");}

/* 22.5.11: QRXLoader is large (tens to hundreds of KB); never place it on the
 * AmigaDOS process stack (4-64 KB, no MMU guard on m68k). Keep it in BSS. */
static QRXLoader l;
int main(int argc,char**argv){const char*target=NULL,*handoff=NULL,*base;char logical[QRX_MAX_NAME];QRXModule*root;int i,show_bridge=0,show_init=0,rc=1;unsigned n;memset(&l,0,sizeof(l));l.next_base=QRX_GUEST_FALLBACK_BASE;l.verbose=1;snprintf(l.system32,sizeof(l.system32),"%s",QRX_DEFAULT_SYSTEM32);for(i=1;i<argc;i++){if(!strcmp(argv[i],"--quiet"))l.verbose=0;else if(!strcmp(argv[i],"--strict-bridge"))l.strict_bridge=1;else if(!strcmp(argv[i],"--bridge-dump"))show_bridge=1;else if(!strcmp(argv[i],"--init-plan"))show_init=1;else if(!strcmp(argv[i],"--emit-handoff")&&i+1<argc)handoff=argv[++i];else if(!strcmp(argv[i],"--system32")&&i+1<argc)snprintf(l.system32,sizeof(l.system32),"%s",argv[++i]);else if(!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")){usage();return 0;}else if(argv[i][0]=='-'&&argv[i][1]){printf("Unknown option: %s\n",argv[i]);usage();return 2;}else target=argv[i];}if(!target){usage();return 2;}base=qrx_basename(target);snprintf(logical,sizeof(logical),"%s",base);qrx_lower(logical);printf("QRXWinGuestRuntime: target=%s system32=%s\n",target,l.system32);root=map_path(&l,target,logical,0);if(!root||!root->mapped)goto done;if(!scan_dependencies(&l,root,0)){printf("QRXWinGuestRuntime: dependency mapping failed\n");goto done;}for(n=0;n<l.count;n++)if(!resolve_imports(&l,&l.modules[n])){printf("QRXWinGuestRuntime: import resolution failed in %s\n",l.modules[n].name);goto done;}for(n=0;n<l.count;n++)if(!parse_tls(&l,&l.modules[n]))goto done;build_init_plan(&l,root);printf("QRXWinGuestRuntime: modules=%u imports=%u resolved=%u bridges=%u tls-callbacks=%u init-calls=%u relocations=%u forwarders=%u cycles=%u missing=%u unresolved=%u\n",l.count,l.total_imports,l.total_resolved,l.bridge_bindings,l.total_tls,l.init_count,l.total_relocations,l.total_forwarders,l.cycles,l.missing_files,l.unresolved);if(show_bridge)print_bridges(&l);if(show_init)print_init_plan(&l);if(l.unresolved||l.missing_files)goto done;if(handoff&&!emit_handoff(&l,root,handoff))goto done;puts("QRXWinGuestRuntime: INIT/LINK PASS - DllMain/TLS plan and ntdll host bridge are ready for QRX86 executor handoff");rc=0;done:if(rc)puts("QRXWinGuestRuntime: INIT/LINK FAIL");free_loader(&l);return rc;}
