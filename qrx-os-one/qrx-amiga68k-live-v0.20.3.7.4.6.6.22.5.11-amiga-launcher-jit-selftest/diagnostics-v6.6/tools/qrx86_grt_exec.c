/* QRX OS One - QRX86 GRT executor + tiered JIT foundation
 * Release 0.20.3.7.4.6.6.22.5
 *
 * Consumes the QRXGRT1 handoff produced by QRXWinGuestRuntime and executes
 * dependency-ordered ReactOS TLS/DllMain/EXE entry calls in an i386 VM.
 *
 * Design goals:
 *  - deterministic guest virtual-memory map from QRXGRT1
 *  - FS/TEB + minimal PEB bootstrap
 *  - ntdll host bridge dispatch at synthetic bridge addresses
 *  - interpreter as correctness/reference fallback
 *  - cached basic blocks
 *  - on m68k: executable native threaded blocks (JSR helper sequence) with
 *    cache flush, so the dispatch path is genuinely generated native code;
 *    later direct instruction emission can replace individual helpers without
 *    changing the loader/dispatcher ABI.
 *  - first unsupported opcode is reported with module/EIP/raw bytes
 *
 * This is a JIT/executor foundation, not a claim of complete IA-32 or Win32
 * compatibility. Unsupported instructions fail closed with a precise trace.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <ctype.h>

/* Deadwood AROS/m68k SDK compatibility; see QRXWinGuestRuntime. FILE stays
 * opaque and diagnostics use stdout so no SDK-specific stderr object ABI is
 * required. */
/* 22.5.11: only when the selected SDK stdio.h did not declare FILE/fopen
 * itself - a complete Deadwood SDK (posixc/stdc stdio) conflicts with this shim. */
#if defined(__AROS__) && !defined(_STDC_STDIO_H_) && !defined(_POSIXC_STDIO_H_) && !defined(_STDIO_H) && !defined(_STDIO_H_)
typedef void QRX_FILE;
extern QRX_FILE *fopen(const char *, const char *);
extern int fclose(QRX_FILE *);
extern size_t fread(void *, size_t, size_t, QRX_FILE *);
extern int printf(const char *, ...);
extern int puts(const char *);
extern int snprintf(char *, size_t, const char *, ...);
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern unsigned long strtoul(const char *, char **, int);
extern void exit(int);
#define FILE QRX_FILE
#endif
#if defined(__mc68000__) || defined(__m68k__)
#include <proto/exec.h>
#endif

#define QRX_GRT_MAGIC "QRXGRT1\0"
#define QRX_MAX_MODULES 160
#define QRX_MAX_BRIDGES 128
#define QRX_MAX_INIT 1024
#define QRX_MAX_REGIONS 256
#define QRX_MAX_BLOCKS 4096
#define QRX_MAX_BLOCK_INSNS 96
#define QRX_NAME 64
#define QRX_STACK_BASE 0x6ff00000u
#define QRX_STACK_SIZE (1024u*1024u)
#define QRX_TEB_SIZE 4096u
#define QRX_PEB_SIZE 4096u
#define QRX_HEAP_BASE 0x50000000u
#define QRX_HEAP_LIMIT 0x5f000000u
#define QRX_SENTINEL_RETURN 0x7feff000u
#define QRX_MAX_STEPS 5000000u
#define QRX_BLOCK_HOT_THRESHOLD 2u
/* 22.5.11: long REP string operations run in resumable chunks; every iteration is charged to the watchdog. */
#define QRX_REP_CHUNK 65536u

#define FL_CF 0x00000001u
#define FL_PF 0x00000004u
#define FL_AF 0x00000010u
#define FL_ZF 0x00000040u
#define FL_SF 0x00000080u
#define FL_DF 0x00000400u
#define FL_OF 0x00000800u

#define INIT_HOST_NTDLL 1u
#define INIT_TLS 2u
#define INIT_DLLMAIN 3u
#define INIT_EXE_TLS 4u
#define INIT_EXE_ENTRY 5u
#define BR_CDECL 2u
#define BR_NORETURN 4u

#define STATUS_SUCCESS 0x00000000u
#define STATUS_NOT_IMPLEMENTED 0xc0000002u
#define STATUS_INVALID_PARAMETER 0xc000000du
#define STATUS_NO_MEMORY 0xc0000017u

typedef struct QRXModuleRec {
    uint32_t base,size,entry_rva,characteristics,tls_count;
    char name[QRX_NAME];
    uint8_t *image;
} QRXModuleRec;

typedef struct QRXBridgeRec {
    uint32_t addr,def_index,argc,flags;
    char name[QRX_NAME];
} QRXBridgeRec;

typedef struct QRXInitRec {
    uint32_t kind,module_index,target,hinst,reason,reserved;
} QRXInitRec;

typedef struct QRXRegion {
    uint32_t base,size;
    uint8_t *data;
    int owned;
    int writable;
    char tag[QRX_NAME];
} QRXRegion;

typedef struct QRXCPU {
    uint32_t r[8]; /* EAX ECX EDX EBX ESP EBP ESI EDI */
    uint32_t eip,eflags;
    uint32_t fs_base;
    uint32_t halted;
    uint32_t fault;
    uint32_t unsupported_eip;
    uint8_t unsupported_bytes[12];
    uint32_t unsupported_len;
} QRXCPU;

typedef struct QRXVM QRXVM;

typedef struct QRXBlock {
    uint32_t eip;
    uint32_t insn_eip[QRX_MAX_BLOCK_INSNS];
    uint16_t count;
    uint16_t terminal;
    uint32_t hits;
    void *native_code;
    uint32_t native_size;
} QRXBlock;

struct QRXVM {
    QRXModuleRec modules[QRX_MAX_MODULES]; unsigned module_count;
    QRXBridgeRec bridges[QRX_MAX_BRIDGES]; unsigned bridge_count;
    QRXInitRec init[QRX_MAX_INIT]; unsigned init_count;
    uint32_t root_index,teb_base,peb_base,heap_handle;
    QRXRegion regions[QRX_MAX_REGIONS]; unsigned region_count;
    uint32_t heap_next;
    QRXBlock blocks[QRX_MAX_BLOCKS]; unsigned block_count;
    unsigned trace,verbose,jit_enabled,jit_threshold; uint64_t max_steps,trace_lines,step_deadline;
    uint64_t steps,blocks_executed,jit_blocks_executed,interp_steps,bridge_calls;
    uint32_t last_error;
};

#if defined(__mc68000__) || defined(__m68k__)
static QRXVM *g_jit_vm;
static QRXCPU *g_jit_cpu;
static QRXBlock *g_jit_block;
static unsigned g_jit_index;
#endif

static uint16_t rd16le(const uint8_t*p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static uint32_t rd32le(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void wr16le(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void wr32le(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static int get_u32(FILE*f,uint32_t*v){uint8_t b[4];if(fread(b,1,4,f)!=4)return 0;*v=rd32le(b);return 1;}
static int get_name(FILE*f,char*s,size_t n){if(n<1)return 0;if(fread(s,1,n,f)!=n)return 0;s[n-1]=0;return 1;}
static uint32_t align_up(uint32_t v,uint32_t a){return (v+a-1u)&~(a-1u);}

static int region_add(QRXVM*v,uint32_t base,uint32_t size,uint8_t*data,int owned,int writable,const char*tag){QRXRegion*r;if(!size||v->region_count>=QRX_MAX_REGIONS)return 0;r=&v->regions[v->region_count++];r->base=base;r->size=size;r->data=data;r->owned=owned;r->writable=writable;snprintf(r->tag,sizeof(r->tag),"%s",tag?tag:"region");return 1;}
static QRXRegion*region_find(QRXVM*v,uint32_t a,uint32_t n){unsigned i;uint64_t end=(uint64_t)a+n;for(i=0;i<v->region_count;i++){QRXRegion*r=&v->regions[i];if(a>=r->base&&end<=(uint64_t)r->base+r->size)return r;}return NULL;}
static uint8_t*vm_ptr(QRXVM*v,uint32_t a,uint32_t n,int write){QRXRegion*r=region_find(v,a,n);if(!r|| (write&&!r->writable))return NULL;return r->data+(a-r->base);}
static int vm_r8(QRXVM*v,uint32_t a,uint8_t*x){uint8_t*p=vm_ptr(v,a,1,0);if(!p)return 0;*x=*p;return 1;}
static int vm_r16(QRXVM*v,uint32_t a,uint16_t*x){uint8_t*p=vm_ptr(v,a,2,0);if(!p)return 0;*x=rd16le(p);return 1;}
static int vm_r32(QRXVM*v,uint32_t a,uint32_t*x){uint8_t*p=vm_ptr(v,a,4,0);if(!p)return 0;*x=rd32le(p);return 1;}
static int vm_w8(QRXVM*v,uint32_t a,uint8_t x){uint8_t*p=vm_ptr(v,a,1,1);if(!p)return 0;*p=x;return 1;}
static int vm_w16(QRXVM*v,uint32_t a,uint16_t x){uint8_t*p=vm_ptr(v,a,2,1);if(!p)return 0;wr16le(p,x);return 1;}
static int vm_w32(QRXVM*v,uint32_t a,uint32_t x){uint8_t*p=vm_ptr(v,a,4,1);if(!p)return 0;wr32le(p,x);return 1;}

static const char*module_for(QRXVM*v,uint32_t a,uint32_t*rva){unsigned i;for(i=0;i<v->module_count;i++){QRXModuleRec*m=&v->modules[i];if(a>=m->base&&a<m->base+m->size){if(rva)*rva=a-m->base;return m->name;}}if(rva)*rva=0;return "<host/unknown>";}
static QRXBridgeRec*bridge_for(QRXVM*v,uint32_t a){unsigned i;for(i=0;i<v->bridge_count;i++)if(v->bridges[i].addr==a)return &v->bridges[i];return NULL;}

static int load_handoff(const char*path,QRXVM*v){FILE*f;char magic[8];uint32_t ver,i;memset(v,0,sizeof(*v));v->heap_next=QRX_HEAP_BASE;v->jit_enabled=0;v->jit_threshold=QRX_BLOCK_HOT_THRESHOLD;f=fopen(path,"rb");if(!f){printf("QRX86GRT: cannot open %s\n",path);return 0;}if(fread(magic,1,8,f)!=8||memcmp(magic,QRX_GRT_MAGIC,8)){printf("QRX86GRT: bad handoff magic\n");fclose(f);return 0;}if(!get_u32(f,&ver)||ver!=1||!get_u32(f,&v->module_count)||!get_u32(f,&v->bridge_count)||!get_u32(f,&v->init_count)||!get_u32(f,&v->root_index)||!get_u32(f,&v->teb_base)||!get_u32(f,&v->peb_base)||!get_u32(f,&v->heap_handle)){printf("QRX86GRT: malformed header\n");fclose(f);return 0;}if(v->module_count>QRX_MAX_MODULES||v->bridge_count>QRX_MAX_BRIDGES||v->init_count>QRX_MAX_INIT||v->root_index>=v->module_count){printf("QRX86GRT: handoff counts out of range\n");fclose(f);return 0;}for(i=0;i<v->module_count;i++){QRXModuleRec*m=&v->modules[i];if(!get_u32(f,&m->base)||!get_u32(f,&m->size)||!get_u32(f,&m->entry_rva)||!get_u32(f,&m->characteristics)||!get_u32(f,&m->tls_count)||!get_name(f,m->name,64)){fclose(f);return 0;}}
for(i=0;i<v->bridge_count;i++){QRXBridgeRec*b=&v->bridges[i];if(!get_u32(f,&b->addr)||!get_u32(f,&b->def_index)||!get_u32(f,&b->argc)||!get_u32(f,&b->flags)||!get_name(f,b->name,64)){fclose(f);return 0;}}
for(i=0;i<v->init_count;i++){QRXInitRec*r=&v->init[i];if(!get_u32(f,&r->kind)||!get_u32(f,&r->module_index)||!get_u32(f,&r->target)||!get_u32(f,&r->hinst)||!get_u32(f,&r->reason)||!get_u32(f,&r->reserved)){fclose(f);return 0;}}
for(i=0;i<v->module_count;i++){QRXModuleRec*m=&v->modules[i];m->image=(uint8_t*)malloc(m->size?m->size:1);if(!m->image||fread(m->image,1,m->size,f)!=m->size){fclose(f);return 0;}if(!region_add(v,m->base,m->size,m->image,0,1,m->name)){fclose(f);return 0;}}
fclose(f);return 1;}

static void free_vm(QRXVM*v){unsigned i;for(i=0;i<v->module_count;i++)free(v->modules[i].image);for(i=0;i<v->region_count;i++)if(v->regions[i].owned)free(v->regions[i].data);for(i=0;i<v->block_count;i++)free(v->blocks[i].native_code);}

static int bootstrap_vm(QRXVM*v){uint8_t*teb=(uint8_t*)calloc(1,QRX_TEB_SIZE),*peb=(uint8_t*)calloc(1,QRX_PEB_SIZE),*stack=(uint8_t*)calloc(1,QRX_STACK_SIZE);QRXModuleRec*root=&v->modules[v->root_index];if(!teb||!peb||!stack)return 0;if(!region_add(v,v->teb_base,QRX_TEB_SIZE,teb,1,1,"TEB")||!region_add(v,v->peb_base,QRX_PEB_SIZE,peb,1,1,"PEB")||!region_add(v,QRX_STACK_BASE,QRX_STACK_SIZE,stack,1,1,"STACK"))return 0;
/* x86 NT TEB essentials */
vm_w32(v,v->teb_base+0x00,0xffffffffu); /* ExceptionList */
vm_w32(v,v->teb_base+0x04,QRX_STACK_BASE+QRX_STACK_SIZE-16); /* StackBase */
vm_w32(v,v->teb_base+0x08,QRX_STACK_BASE); /* StackLimit */
vm_w32(v,v->teb_base+0x18,v->teb_base); /* NtTib.Self */
vm_w32(v,v->teb_base+0x30,v->peb_base); /* PEB */
vm_w32(v,v->teb_base+0x34,0); /* LastErrorValue */
/* minimal PEB */
vm_w8(v,v->peb_base+2,0); /* BeingDebugged */
vm_w32(v,v->peb_base+0x08,root->base); /* ImageBaseAddress */
vm_w32(v,v->peb_base+0x18,v->heap_handle); /* ProcessHeap */
return 1;}

static int push32(QRXVM*v,QRXCPU*c,uint32_t x){c->r[4]-=4;return vm_w32(v,c->r[4],x);}
static int pop32(QRXVM*v,QRXCPU*c,uint32_t*x){if(!vm_r32(v,c->r[4],x))return 0;c->r[4]+=4;return 1;}
static uint32_t parity_even8(uint32_t x){x^=x>>4;x&=0xf;return (0x9669u>>x)&1u;}
static uint32_t mask_for(int bits){return bits==8?0xffu:bits==16?0xffffu:0xffffffffu;}
static uint32_t sign_for(int bits){return bits==8?0x80u:bits==16?0x8000u:0x80000000u;}
static void flags_szp(QRXCPU*c,uint32_t r,int bits){uint32_t m=mask_for(bits),s=sign_for(bits);r&=m;c->eflags&=~(FL_SF|FL_ZF|FL_PF);if(!r)c->eflags|=FL_ZF;if(r&s)c->eflags|=FL_SF;if(parity_even8(r&0xff))c->eflags|=FL_PF;}
static uint32_t alu_add(QRXCPU*c,uint32_t a,uint32_t b,int bits,uint32_t carry){uint64_t m=mask_for(bits),x=(uint64_t)(a&m)+(uint64_t)(b&m)+carry;uint32_t r=(uint32_t)x&(uint32_t)m,s=sign_for(bits);c->eflags&=~(FL_CF|FL_OF|FL_AF);if(x>m)c->eflags|=FL_CF;if((~(a^b)&(a^r)&s)!=0)c->eflags|=FL_OF;if(((a^b^r)&0x10)!=0)c->eflags|=FL_AF;flags_szp(c,r,bits);return r;}
static uint32_t alu_sub(QRXCPU*c,uint32_t a,uint32_t b,int bits,uint32_t borrow){uint64_t m=mask_for(bits),bb=(uint64_t)(b&m)+borrow;uint32_t r=(uint32_t)((a&m)-bb)&(uint32_t)m,s=sign_for(bits);c->eflags&=~(FL_CF|FL_OF|FL_AF);if((uint64_t)(a&m)<bb)c->eflags|=FL_CF;if(((a^b)&(a^r)&s)!=0)c->eflags|=FL_OF;if(((a^b^r)&0x10)!=0)c->eflags|=FL_AF;flags_szp(c,r,bits);return r;}
static uint32_t logic_flags(QRXCPU*c,uint32_t r,int bits){c->eflags&=~(FL_CF|FL_OF|FL_AF);flags_szp(c,r,bits);return r&mask_for(bits);}
static int cond_true(QRXCPU*c,unsigned cc){int cf=!!(c->eflags&FL_CF),zf=!!(c->eflags&FL_ZF),sf=!!(c->eflags&FL_SF),of=!!(c->eflags&FL_OF),pf=!!(c->eflags&FL_PF);switch(cc&15){case 0:return of;case 1:return !of;case 2:return cf;case 3:return !cf;case 4:return zf;case 5:return !zf;case 6:return cf||zf;case 7:return !cf&&!zf;case 8:return sf;case 9:return !sf;case 10:return pf;case 11:return !pf;case 12:return sf!=of;case 13:return sf==of;case 14:return zf||(sf!=of);case 15:return !zf&&(sf==of);}return 0;}

static uint32_t reg_read(QRXCPU*c,unsigned r,int bits){uint32_t v;if(bits==8){if(r<4)v=c->r[r]&0xffu;else v=(c->r[r-4]>>8)&0xffu;return v;}v=c->r[r&7];return bits==16?v&0xffffu:v;}
static void reg_write(QRXCPU*c,unsigned r,int bits,uint32_t v){if(bits==8){if(r<4)c->r[r]=(c->r[r]&~0xffu)|(v&0xffu);else{unsigned q=r-4;c->r[q]=(c->r[q]&~0xff00u)|((v&0xffu)<<8);}return;}if(bits==16)c->r[r&7]=(c->r[r&7]&0xffff0000u)|(v&0xffffu);else c->r[r&7]=v;}

typedef struct EA {int is_reg;unsigned reg;uint32_t addr;unsigned bytes;} EA;
static int fetch8(QRXVM*v,uint32_t a,uint8_t*x){return vm_r8(v,a,x);}
static int fetch16(QRXVM*v,uint32_t a,uint16_t*x){return vm_r16(v,a,x);}
static int fetch32(QRXVM*v,uint32_t a,uint32_t*x){return vm_r32(v,a,x);}

static int decode_ea32(QRXVM*v,QRXCPU*c,uint32_t*pc,uint8_t modrm,uint32_t segbase,EA*e){unsigned mod=modrm>>6,rm=modrm&7;uint32_t base=0,index=0,disp=0;uint8_t sib,b;uint32_t d;if(mod==3){e->is_reg=1;e->reg=rm;e->addr=0;e->bytes=0;return 1;}e->is_reg=0;if(rm==4){if(!fetch8(v,(*pc)++, &sib))return 0;{unsigned ss=sib>>6,ix=(sib>>3)&7,ba=sib&7;if(ix!=4)index=c->r[ix]<<ss;if(ba==5&&mod==0){if(!fetch32(v,*pc,&d))return 0;*pc+=4;base=d;}else base=c->r[ba];}}else if(rm==5&&mod==0){if(!fetch32(v,*pc,&d))return 0;*pc+=4;base=d;}else base=c->r[rm];if(mod==1){if(!fetch8(v,(*pc)++,&b))return 0;disp=(uint32_t)(int32_t)(int8_t)b;}else if(mod==2){if(!fetch32(v,*pc,&d))return 0;*pc+=4;disp=d;}e->addr=segbase+base+index+disp;e->bytes=0;return 1;}
static int ea_read(QRXVM*v,QRXCPU*c,EA*e,int bits,uint32_t*x){if(e->is_reg){*x=reg_read(c,e->reg,bits);return 1;}if(bits==8){uint8_t q;if(!vm_r8(v,e->addr,&q))return 0;*x=q;return 1;}if(bits==16){uint16_t q;if(!vm_r16(v,e->addr,&q))return 0;*x=q;return 1;}return vm_r32(v,e->addr,x);}
static int ea_write(QRXVM*v,QRXCPU*c,EA*e,int bits,uint32_t x){if(e->is_reg){reg_write(c,e->reg,bits,x);return 1;}return bits==8?vm_w8(v,e->addr,(uint8_t)x):bits==16?vm_w16(v,e->addr,(uint16_t)x):vm_w32(v,e->addr,x);}

static void unsupported(QRXVM*v,QRXCPU*c,uint32_t eip){unsigned i;c->fault=1;c->unsupported_eip=eip;c->unsupported_len=0;for(i=0;i<sizeof(c->unsupported_bytes);i++){uint8_t b;if(!vm_r8(v,eip+i,&b))break;c->unsupported_bytes[c->unsupported_len++]=b;}}
static void memfault(QRXCPU*c){c->fault=2;}

static int exec_string(QRXVM*v,QRXCPU*c,uint8_t op,int bits,unsigned rep,int*resume){uint32_t count=rep?c->r[1]:1u,step=(uint32_t)(bits/8),i;int dir=(c->eflags&FL_DF)?-1:1,early=0;uint64_t room=v->steps<v->step_deadline?v->step_deadline-v->steps:0u;*resume=0;if(count>QRX_REP_CHUNK)count=QRX_REP_CHUNK;if((uint64_t)count>room+1u)count=(uint32_t)(room+1u);for(i=0;i<count;i++){uint32_t x=0,y=0;switch(op){case 0xa4:case 0xa5:if(bits==8){uint8_t q;if(!vm_r8(v,c->r[6],&q)||!vm_w8(v,c->r[7],q))return 0;}else if(bits==16){uint16_t q;if(!vm_r16(v,c->r[6],&q)||!vm_w16(v,c->r[7],q))return 0;}else{if(!vm_r32(v,c->r[6],&x)||!vm_w32(v,c->r[7],x))return 0;}c->r[6]+=dir*(int32_t)step;c->r[7]+=dir*(int32_t)step;break;case 0xaa:case 0xab:x=reg_read(c,0,bits);if(bits==8){if(!vm_w8(v,c->r[7],(uint8_t)x))return 0;}else if(bits==16){if(!vm_w16(v,c->r[7],(uint16_t)x))return 0;}else if(!vm_w32(v,c->r[7],x))return 0;c->r[7]+=dir*(int32_t)step;break;case 0xac:case 0xad:if(bits==8){uint8_t q;if(!vm_r8(v,c->r[6],&q))return 0;x=q;}else if(bits==16){uint16_t q;if(!vm_r16(v,c->r[6],&q))return 0;x=q;}else if(!vm_r32(v,c->r[6],&x))return 0;reg_write(c,0,bits,x);c->r[6]+=dir*(int32_t)step;break;case 0xae:case 0xaf:if(bits==8){uint8_t q;if(!vm_r8(v,c->r[7],&q))return 0;y=q;}else if(bits==16){uint16_t q;if(!vm_r16(v,c->r[7],&q))return 0;y=q;}else if(!vm_r32(v,c->r[7],&y))return 0;x=reg_read(c,0,bits);alu_sub(c,x,y,bits,0);c->r[7]+=dir*(int32_t)step;if(rep==1 && !(c->eflags&FL_ZF)){i++;early=1;goto done;}if(rep==2 && (c->eflags&FL_ZF)){i++;early=1;goto done;}break;default:return 0;}}
done:if(rep)c->r[1]-=i;if(i>1u)v->steps+=i-1u;if(rep&&!early&&c->r[1]!=0u)*resume=1;return 1;}

/* Return: 1 executed, 0 fault/unsupported. Sets *terminal for control transfer. */
static int step_one(QRXVM*v,QRXCPU*c,int*terminal){uint32_t start=c->eip,pc=start,segbase=0,imm=0,a=0,b=0,r=0;uint8_t op=0,m=0,q=0;int bits=32,rep=0,prefixes=0;EA ea;unsigned reg,sub;*terminal=0;
for(;;){if(!fetch8(v,pc++,&op)){memfault(c);return 0;}if(op==0x66){bits=16;prefixes++;continue;}if(op==0x64){segbase=c->fs_base;prefixes++;continue;}if(op==0xf3){rep=1;prefixes++;continue;}if(op==0xf2){rep=2;prefixes++;continue;}if(op==0xf0){prefixes++;continue;}if(op==0x2e||op==0x3e||op==0x26||op==0x36||op==0x65){prefixes++;continue;}break;}if(prefixes>8){unsupported(v,c,start);return 0;}
#define FAILMEM() do{memfault(c);return 0;}while(0)
#define FETCH8(dst) do{uint8_t _x;if(!fetch8(v,pc++,&_x))FAILMEM();(dst)=_x;}while(0)
#define FETCH16(dst) do{uint16_t _x;if(!fetch16(v,pc,&_x))FAILMEM();pc+=2;(dst)=_x;}while(0)
#define FETCH32(dst) do{uint32_t _x;if(!fetch32(v,pc,&_x))FAILMEM();pc+=4;(dst)=_x;}while(0)
#define MODRM() do{FETCH8(m);if(!decode_ea32(v,c,&pc,m,segbase,&ea))FAILMEM();reg=(m>>3)&7;}while(0)
#define READ_EA(dst,sz) do{if(!ea_read(v,c,&ea,(sz),&(dst)))FAILMEM();}while(0)
#define WRITE_EA(src,sz) do{if(!ea_write(v,c,&ea,(sz),(src)))FAILMEM();}while(0)

if(op>=0x50&&op<=0x57){if(!push32(v,c,c->r[op-0x50]))FAILMEM();c->eip=pc;return 1;}
if(op>=0x58&&op<=0x5f){if(!pop32(v,c,&r))FAILMEM();c->r[op-0x58]=r;c->eip=pc;return 1;}
if(op>=0x40&&op<=0x47){unsigned rr=op-0x40;uint32_t oldcf=c->eflags&FL_CF;c->r[rr]=alu_add(c,c->r[rr],1,bits,0);c->eflags=(c->eflags&~FL_CF)|oldcf;c->eip=pc;return 1;}
if(op>=0x48&&op<=0x4f){unsigned rr=op-0x48;uint32_t oldcf=c->eflags&FL_CF;c->r[rr]=alu_sub(c,c->r[rr],1,bits,0);c->eflags=(c->eflags&~FL_CF)|oldcf;c->eip=pc;return 1;}
if(op>=0xb0&&op<=0xb7){uint8_t x;FETCH8(x);reg_write(c,op-0xb0,8,x);c->eip=pc;return 1;}
if(op>=0xb8&&op<=0xbf){if(bits==16){uint16_t x;FETCH16(x);reg_write(c,op-0xb8,16,x);}else{FETCH32(imm);c->r[op-0xb8]=imm;}c->eip=pc;return 1;}
if(op>=0x70&&op<=0x7f){int8_t d;FETCH8(q);d=(int8_t)q;c->eip=cond_true(c,op&15)?pc+(int32_t)d:pc;*terminal=1;return 1;}
if(op>=0x91&&op<=0x97){unsigned rr=op-0x90;r=c->r[0];c->r[0]=c->r[rr];c->r[rr]=r;c->eip=pc;return 1;}

switch(op){
case 0x90:c->eip=pc;return 1;
case 0x68:FETCH32(imm);if(!push32(v,c,imm))FAILMEM();c->eip=pc;return 1;
case 0x6a:FETCH8(q);if(!push32(v,c,(uint32_t)(int32_t)(int8_t)q))FAILMEM();c->eip=pc;return 1;
case 0x69:case 0x6b:MODRM();READ_EA(a,bits);if(op==0x6b){FETCH8(q);imm=(uint32_t)(int32_t)(int8_t)q;}else if(bits==16){uint16_t x;FETCH16(x);imm=(uint32_t)(int32_t)(int16_t)x;}else FETCH32(imm);{int64_t z=(int64_t)(int32_t)a*(int64_t)(int32_t)imm;r=(uint32_t)z;reg_write(c,reg,bits,r);c->eflags&=~(FL_CF|FL_OF);if(z!=(int64_t)(int32_t)r)c->eflags|=FL_CF|FL_OF;}c->eip=pc;return 1;
case 0x98:if(bits==16)reg_write(c,0,16,(uint16_t)(int16_t)(int8_t)reg_read(c,0,8));else c->r[0]=(uint32_t)(int32_t)(int16_t)reg_read(c,0,16);c->eip=pc;return 1;
case 0x9c:if(!push32(v,c,c->eflags))FAILMEM();c->eip=pc;return 1;
case 0x9d:if(!pop32(v,c,&c->eflags))FAILMEM();c->eip=pc;return 1;
case 0x9e:{uint32_t ah=reg_read(c,4,8);c->eflags=(c->eflags&~(FL_SF|FL_ZF|FL_AF|FL_PF|FL_CF))|(ah&(FL_SF|FL_ZF|FL_AF|FL_PF|FL_CF));c->eip=pc;return 1;}
case 0x9f:{uint32_t ah=(c->eflags&(FL_SF|FL_ZF|FL_AF|FL_PF|FL_CF))|0x02u;reg_write(c,4,8,ah);c->eip=pc;return 1;}
case 0x60:{uint32_t oes=c->r[4];if(!push32(v,c,c->r[0])||!push32(v,c,c->r[1])||!push32(v,c,c->r[2])||!push32(v,c,c->r[3])||!push32(v,c,oes)||!push32(v,c,c->r[5])||!push32(v,c,c->r[6])||!push32(v,c,c->r[7]))FAILMEM();c->eip=pc;return 1;}
case 0x61:if(!pop32(v,c,&c->r[7])||!pop32(v,c,&c->r[6])||!pop32(v,c,&c->r[5]))FAILMEM();c->r[4]+=4;if(!pop32(v,c,&c->r[3])||!pop32(v,c,&c->r[2])||!pop32(v,c,&c->r[1])||!pop32(v,c,&c->r[0]))FAILMEM();c->eip=pc;return 1;
case 0x62:case 0x63:case 0x64:case 0x65:case 0x66:case 0x67:unsupported(v,c,start);return 0;
case 0x89:MODRM();WRITE_EA(reg_read(c,reg,bits),bits);c->eip=pc;return 1;
case 0x8b:MODRM();READ_EA(a,bits);reg_write(c,reg,bits,a);c->eip=pc;return 1;
case 0x88:bits=8;MODRM();WRITE_EA(reg_read(c,reg,8),8);c->eip=pc;return 1;
case 0x8a:bits=8;MODRM();READ_EA(a,8);reg_write(c,reg,8,a);c->eip=pc;return 1;
case 0x8d:MODRM();if(ea.is_reg){unsupported(v,c,start);return 0;}reg_write(c,reg,bits,ea.addr-segbase);c->eip=pc;return 1;
case 0x8f:MODRM();if(reg!=0){unsupported(v,c,start);return 0;}if(!pop32(v,c,&r))FAILMEM();WRITE_EA(r,32);c->eip=pc;return 1;
case 0xc6:bits=8;MODRM();if(reg!=0){unsupported(v,c,start);return 0;}FETCH8(q);WRITE_EA(q,8);c->eip=pc;return 1;
case 0xc7:MODRM();if(reg!=0){unsupported(v,c,start);return 0;}if(bits==16){uint16_t x;FETCH16(x);imm=x;}else FETCH32(imm);WRITE_EA(imm,bits);c->eip=pc;return 1;
case 0x01:case 0x09:case 0x11:case 0x19:case 0x21:case 0x29:case 0x31:case 0x39:MODRM();READ_EA(a,bits);b=reg_read(c,reg,bits);sub=(op>>3)&7;switch(sub){case 0:r=alu_add(c,a,b,bits,0);WRITE_EA(r,bits);break;case 1:r=logic_flags(c,a|b,bits);WRITE_EA(r,bits);break;case 2:r=alu_add(c,a,b,bits,!!(c->eflags&FL_CF));WRITE_EA(r,bits);break;case 3:r=alu_sub(c,a,b,bits,!!(c->eflags&FL_CF));WRITE_EA(r,bits);break;case 4:r=logic_flags(c,a&b,bits);WRITE_EA(r,bits);break;case 5:r=alu_sub(c,a,b,bits,0);WRITE_EA(r,bits);break;case 6:r=logic_flags(c,a^b,bits);WRITE_EA(r,bits);break;case 7:(void)alu_sub(c,a,b,bits,0);break;}c->eip=pc;return 1;
case 0x03:case 0x0b:case 0x13:case 0x1b:case 0x23:case 0x2b:case 0x33:case 0x3b:MODRM();READ_EA(b,bits);a=reg_read(c,reg,bits);sub=(op>>3)&7;switch(sub){case 0:r=alu_add(c,a,b,bits,0);reg_write(c,reg,bits,r);break;case 1:r=logic_flags(c,a|b,bits);reg_write(c,reg,bits,r);break;case 2:r=alu_add(c,a,b,bits,!!(c->eflags&FL_CF));reg_write(c,reg,bits,r);break;case 3:r=alu_sub(c,a,b,bits,!!(c->eflags&FL_CF));reg_write(c,reg,bits,r);break;case 4:r=logic_flags(c,a&b,bits);reg_write(c,reg,bits,r);break;case 5:r=alu_sub(c,a,b,bits,0);reg_write(c,reg,bits,r);break;case 6:r=logic_flags(c,a^b,bits);reg_write(c,reg,bits,r);break;case 7:(void)alu_sub(c,a,b,bits,0);break;}c->eip=pc;return 1;
case 0x05:FETCH32(imm);c->r[0]=alu_add(c,c->r[0],imm,32,0);c->eip=pc;return 1;
case 0x0d:FETCH32(imm);c->r[0]=logic_flags(c,c->r[0]|imm,32);c->eip=pc;return 1;
case 0x25:FETCH32(imm);c->r[0]=logic_flags(c,c->r[0]&imm,32);c->eip=pc;return 1;
case 0x2d:FETCH32(imm);c->r[0]=alu_sub(c,c->r[0],imm,32,0);c->eip=pc;return 1;
case 0x35:FETCH32(imm);c->r[0]=logic_flags(c,c->r[0]^imm,32);c->eip=pc;return 1;
case 0x3d:FETCH32(imm);(void)alu_sub(c,c->r[0],imm,32,0);c->eip=pc;return 1;
case 0x84:bits=8;MODRM();READ_EA(a,8);b=reg_read(c,reg,8);logic_flags(c,a&b,8);c->eip=pc;return 1;
case 0x85:MODRM();READ_EA(a,bits);b=reg_read(c,reg,bits);logic_flags(c,a&b,bits);c->eip=pc;return 1;
case 0x80:case 0x81:case 0x83:bits=(op==0x80)?8:bits;MODRM();READ_EA(a,bits);if(op==0x80){FETCH8(q);imm=q;}else if(op==0x83){FETCH8(q);imm=(uint32_t)(int32_t)(int8_t)q;}else if(bits==16){uint16_t x;FETCH16(x);imm=x;}else FETCH32(imm);sub=reg;switch(sub){case 0:r=alu_add(c,a,imm,bits,0);WRITE_EA(r,bits);break;case 1:r=logic_flags(c,a|imm,bits);WRITE_EA(r,bits);break;case 2:r=alu_add(c,a,imm,bits,!!(c->eflags&FL_CF));WRITE_EA(r,bits);break;case 3:r=alu_sub(c,a,imm,bits,!!(c->eflags&FL_CF));WRITE_EA(r,bits);break;case 4:r=logic_flags(c,a&imm,bits);WRITE_EA(r,bits);break;case 5:r=alu_sub(c,a,imm,bits,0);WRITE_EA(r,bits);break;case 6:r=logic_flags(c,a^imm,bits);WRITE_EA(r,bits);break;case 7:(void)alu_sub(c,a,imm,bits,0);break;}c->eip=pc;return 1;
case 0xa0:FETCH32(imm);{uint8_t x;if(!vm_r8(v,segbase+imm,&x))FAILMEM();reg_write(c,0,8,x);}c->eip=pc;return 1;
case 0xa1:FETCH32(imm);if(bits==16){uint16_t x;if(!vm_r16(v,segbase+imm,&x))FAILMEM();reg_write(c,0,16,x);}else{uint32_t x;if(!vm_r32(v,segbase+imm,&x))FAILMEM();c->r[0]=x;}c->eip=pc;return 1;
case 0xa2:FETCH32(imm);if(!vm_w8(v,segbase+imm,(uint8_t)reg_read(c,0,8)))FAILMEM();c->eip=pc;return 1;
case 0xa3:FETCH32(imm);if(bits==16){if(!vm_w16(v,segbase+imm,(uint16_t)reg_read(c,0,16)))FAILMEM();}else if(!vm_w32(v,segbase+imm,c->r[0]))FAILMEM();c->eip=pc;return 1;
case 0xa8:FETCH8(q);logic_flags(c,reg_read(c,0,8)&q,8);c->eip=pc;return 1;
case 0xa9:if(bits==16){uint16_t x;FETCH16(x);imm=x;}else FETCH32(imm);logic_flags(c,reg_read(c,0,bits)&imm,bits);c->eip=pc;return 1;
case 0xa4:case 0xaa:case 0xac:case 0xae:{int rs;if(!exec_string(v,c,op,8,rep,&rs))FAILMEM();if(!rs)c->eip=pc;return 1;}
case 0xa5:case 0xab:case 0xad:case 0xaf:{int rs;if(!exec_string(v,c,op,bits,rep,&rs))FAILMEM();if(!rs)c->eip=pc;return 1;}
case 0xc3:if(!pop32(v,c,&c->eip))FAILMEM();*terminal=1;return 1;
case 0xc2:{uint16_t n;FETCH16(n);if(!pop32(v,c,&c->eip))FAILMEM();c->r[4]+=n;*terminal=1;return 1;}
case 0xc9:c->r[4]=c->r[5];if(!pop32(v,c,&c->r[5]))FAILMEM();c->eip=pc;return 1;
case 0xe8:FETCH32(imm);if(!push32(v,c,pc))FAILMEM();c->eip=pc+(int32_t)imm;*terminal=1;return 1;
case 0xe9:FETCH32(imm);c->eip=pc+(int32_t)imm;*terminal=1;return 1;
case 0xeb:FETCH8(q);c->eip=pc+(int32_t)(int8_t)q;*terminal=1;return 1;
case 0xf5:c->eflags^=FL_CF;c->eip=pc;return 1;
case 0xf8:c->eflags&=~FL_CF;c->eip=pc;return 1;
case 0xf9:c->eflags|=FL_CF;c->eip=pc;return 1;
case 0xfc:c->eflags&=~FL_DF;c->eip=pc;return 1;
case 0xfd:c->eflags|=FL_DF;c->eip=pc;return 1;
case 0x99:c->r[2]=(c->r[0]&0x80000000u)?0xffffffffu:0;c->eip=pc;return 1;
case 0x87:MODRM();READ_EA(a,bits);b=reg_read(c,reg,bits);WRITE_EA(b,bits);reg_write(c,reg,bits,a);c->eip=pc;return 1;
case 0xf7:MODRM();READ_EA(a,bits);sub=reg;if(sub==0){if(bits==16){uint16_t x;FETCH16(x);imm=x;}else FETCH32(imm);logic_flags(c,a&imm,bits);}else if(sub==2)WRITE_EA((~a)&mask_for(bits),bits);else if(sub==3){r=alu_sub(c,0,a,bits,0);WRITE_EA(r,bits);}else if(sub==4){uint64_t z=(uint64_t)c->r[0]*(uint64_t)a;c->r[0]=(uint32_t)z;c->r[2]=(uint32_t)(z>>32);c->eflags&=~(FL_CF|FL_OF);if(c->r[2])c->eflags|=FL_CF|FL_OF;}else if(sub==5){int64_t z=(int64_t)(int32_t)c->r[0]*(int64_t)(int32_t)a;c->r[0]=(uint32_t)z;c->r[2]=(uint32_t)((uint64_t)z>>32);c->eflags&=~(FL_CF|FL_OF);if(z!=(int64_t)(int32_t)c->r[0])c->eflags|=FL_CF|FL_OF;}else if(sub==6){uint64_t num=((uint64_t)c->r[2]<<32)|c->r[0];if(!a){c->fault=3;return 0;}c->r[0]=(uint32_t)(num/a);c->r[2]=(uint32_t)(num%a);}else if(sub==7){int64_t num=((int64_t)(int32_t)c->r[2]<<32)|c->r[0];int32_t d=(int32_t)a;if(!d){c->fault=3;return 0;}c->r[0]=(uint32_t)(int32_t)(num/d);c->r[2]=(uint32_t)(int32_t)(num%d);}else{unsupported(v,c,start);return 0;}c->eip=pc;return 1;
case 0xff:MODRM();READ_EA(a,32);sub=reg;if(sub==0){uint32_t old=c->eflags&FL_CF;r=alu_add(c,a,1,32,0);c->eflags=(c->eflags&~FL_CF)|old;WRITE_EA(r,32);c->eip=pc;}else if(sub==1){uint32_t old=c->eflags&FL_CF;r=alu_sub(c,a,1,32,0);c->eflags=(c->eflags&~FL_CF)|old;WRITE_EA(r,32);c->eip=pc;}else if(sub==2){if(!push32(v,c,pc))FAILMEM();c->eip=a;*terminal=1;}else if(sub==4){c->eip=a;*terminal=1;}else if(sub==6){if(!push32(v,c,a))FAILMEM();c->eip=pc;}else{unsupported(v,c,start);return 0;}return 1;
case 0xd1:case 0xd3:case 0xc1:MODRM();READ_EA(a,bits);sub=reg;if(op==0xd1)imm=1;else if(op==0xd3)imm=c->r[1]&0x1f;else{FETCH8(q);imm=q&0x1f;}if(!imm){c->eip=pc;return 1;}if(sub==4){r=(a<<imm)&mask_for(bits);c->eflags&=~FL_CF;c->eflags|=((a>>(bits-imm))&1)?FL_CF:0;flags_szp(c,r,bits);WRITE_EA(r,bits);}else if(sub==5){r=(a&mask_for(bits))>>imm;c->eflags&=~FL_CF;c->eflags|=((a>>(imm-1))&1)?FL_CF:0;flags_szp(c,r,bits);WRITE_EA(r,bits);}else if(sub==7){if(bits==32)r=(uint32_t)((int32_t)a>>imm);else if(bits==16)r=(uint16_t)((int16_t)a>>imm);else r=(uint8_t)((int8_t)a>>imm);c->eflags&=~FL_CF;c->eflags|=((a>>(imm-1))&1)?FL_CF:0;flags_szp(c,r,bits);WRITE_EA(r,bits);}else{unsupported(v,c,start);return 0;}c->eip=pc;return 1;
case 0x0f:{uint8_t op2;FETCH8(op2);if(op2>=0x40&&op2<=0x4f){MODRM();READ_EA(a,bits);if(cond_true(c,op2&15))reg_write(c,reg,bits,a);c->eip=pc;return 1;}if(op2>=0x80&&op2<=0x8f){FETCH32(imm);c->eip=cond_true(c,op2&15)?pc+(int32_t)imm:pc;*terminal=1;return 1;}if(op2>=0x90&&op2<=0x9f){bits=8;MODRM();WRITE_EA(cond_true(c,op2&15)?1:0,8);c->eip=pc;return 1;}if(op2>=0xc8&&op2<=0xcf){reg=op2-0xc8;a=c->r[reg];c->r[reg]=((a&0xff)<<24)|((a&0xff00)<<8)|((a>>8)&0xff00)|((a>>24)&0xff);c->eip=pc;return 1;}switch(op2){case 0x1f:MODRM();c->eip=pc;return 1;case 0xa3:case 0xab:case 0xb3:case 0xbb:MODRM();READ_EA(a,bits);{uint32_t bit=reg_read(c,reg,32)&(bits-1),mask=1u<<bit;c->eflags=(c->eflags&~FL_CF)|((a&mask)?FL_CF:0);if(op2==0xab)WRITE_EA(a|mask,bits);else if(op2==0xb3)WRITE_EA(a&~mask,bits);else if(op2==0xbb)WRITE_EA(a^mask,bits);}c->eip=pc;return 1;case 0xba:MODRM();FETCH8(q);READ_EA(a,bits);{uint32_t bit=q&(bits-1),mask=1u<<bit;c->eflags=(c->eflags&~FL_CF)|((a&mask)?FL_CF:0);if(reg==5)WRITE_EA(a|mask,bits);else if(reg==6)WRITE_EA(a&~mask,bits);else if(reg==7)WRITE_EA(a^mask,bits);else if(reg!=4){unsupported(v,c,start);return 0;}}c->eip=pc;return 1;case 0xb6:MODRM();READ_EA(a,8);c->r[reg]=a;c->eip=pc;return 1;case 0xb7:MODRM();READ_EA(a,16);c->r[reg]=a;c->eip=pc;return 1;case 0xbe:MODRM();READ_EA(a,8);c->r[reg]=(uint32_t)(int32_t)(int8_t)a;c->eip=pc;return 1;case 0xbf:MODRM();READ_EA(a,16);c->r[reg]=(uint32_t)(int32_t)(int16_t)a;c->eip=pc;return 1;case 0xaf:MODRM();READ_EA(a,bits);{int64_t z=(int64_t)(int32_t)reg_read(c,reg,bits)*(int64_t)(int32_t)a;r=(uint32_t)z;reg_write(c,reg,bits,r);c->eflags&=~(FL_CF|FL_OF);if(z!=(int64_t)(int32_t)r)c->eflags|=FL_CF|FL_OF;}c->eip=pc;return 1;case 0xb1:MODRM();READ_EA(a,bits);b=reg_read(c,reg,bits);r=alu_sub(c,reg_read(c,0,bits),a,bits,0);(void)r;if(c->eflags&FL_ZF)WRITE_EA(b,bits);else reg_write(c,0,bits,a);c->eip=pc;return 1;case 0xc1:MODRM();READ_EA(a,bits);b=reg_read(c,reg,bits);WRITE_EA(alu_add(c,a,b,bits,0),bits);reg_write(c,reg,bits,a);c->eip=pc;return 1;case 0x31:{clock_t t=clock();c->r[0]=(uint32_t)t;c->r[2]=0;c->eip=pc;return 1;}case 0xa2:{uint32_t leaf=c->r[0];if(leaf==0){c->r[0]=1;c->r[1]=0x20585251u;c->r[2]=0x36385851u;c->r[3]=0x54494a20u;}else{c->r[0]=0x00000600u;c->r[1]=c->r[2]=c->r[3]=0;}c->eip=pc;return 1;}default:unsupported(v,c,start);return 0;}}
default:unsupported(v,c,start);return 0;}
#undef FAILMEM
#undef FETCH8
#undef FETCH16
#undef FETCH32
#undef MODRM
#undef READ_EA
#undef WRITE_EA
}

static uint32_t heap_alloc(QRXVM*v,uint32_t n){uint32_t base,size;uint8_t*p;if(!n)n=1;size=align_up(n,16);base=align_up(v->heap_next,16);if(base+size<base||base+size>QRX_HEAP_LIMIT)return 0;p=(uint8_t*)calloc(1,size);if(!p)return 0;if(!region_add(v,base,size,p,1,1,"HEAP")){free(p);return 0;}v->heap_next=base+size;return base;}
static QRXRegion*heap_region(QRXVM*v,uint32_t base){QRXRegion*r=region_find(v,base,1);if(r&&r->owned&&strcmp(r->tag,"HEAP")==0&&r->base==base)return r;return NULL;}
static uint32_t guest_strlen16(QRXVM*v,uint32_t p){uint32_t n=0;uint16_t ch;if(!p)return 0;while(n<0xfffeu&&vm_r16(v,p+n,&ch)&&ch)n+=2;return n;}
static int unicode_to_ansi(QRXVM*v,uint32_t dst,uint32_t src,int alloc){uint16_t sl=0,sm=0,dm=0;uint32_t sb=0,db=0,n,i;uint16_t wc;uint8_t ch;if(!vm_r16(v,src,&sl)||!vm_r16(v,src+2,&sm)||!vm_r32(v,src+4,&sb))return 0;(void)sm;n=sl/2;if(alloc){db=heap_alloc(v,n+1);if(!db)return 0;dm=(uint16_t)(n+1);vm_w32(v,dst+4,db);vm_w16(v,dst+2,dm);}else{if(!vm_r16(v,dst+2,&dm)||!vm_r32(v,dst+4,&db)||!db)return 0;if(n+1>dm)n=dm?dm-1:0;}for(i=0;i<n;i++){if(!vm_r16(v,sb+i*2,&wc))return 0;ch=(uint8_t)(wc<=0xff?wc:'?');if(!vm_w8(v,db+i,ch))return 0;}if(dm)vm_w8(v,db+n,0);vm_w16(v,dst,(uint16_t)n);return 1;}
static int ansi_to_unicode(QRXVM*v,uint32_t dst,uint32_t src,int alloc){uint16_t sl=0,sm=0,dm=0;uint32_t sb=0,db=0,n,i;uint8_t ch;if(!vm_r16(v,src,&sl)||!vm_r16(v,src+2,&sm)||!vm_r32(v,src+4,&sb))return 0;(void)sm;n=sl;if(alloc){db=heap_alloc(v,(n+1)*2);if(!db)return 0;dm=(uint16_t)((n+1)*2);vm_w32(v,dst+4,db);vm_w16(v,dst+2,dm);}else{if(!vm_r16(v,dst+2,&dm)||!vm_r32(v,dst+4,&db)||!db)return 0;if((n+1)*2>dm)n=dm>=2?(dm/2)-1:0;}for(i=0;i<n;i++){if(!vm_r8(v,sb+i,&ch)||!vm_w16(v,db+i*2,(uint16_t)ch))return 0;}if(dm>=2)vm_w16(v,db+n*2,0);vm_w16(v,dst,(uint16_t)(n*2));return 1;}
static uint32_t arg32(QRXVM*v,QRXCPU*c,unsigned n){uint32_t x=0;vm_r32(v,c->r[4]+4u*n,&x);return x;}
static uint32_t status_to_dos(uint32_t s){switch(s){case 0:return 0;case 0xc0000005u:return 998;case 0xc0000008u:return 6;case 0xc000000du:return 87;case 0xc0000017u:return 8;case 0xc0000034u:return 2;default:return 317;}}

static int bridge_dispatch(QRXVM*v,QRXCPU*c,QRXBridgeRec*b){uint32_t a0=arg32(v,c,0),a1=arg32(v,c,1),a2=arg32(v,c,2),a3=arg32(v,c,3),ret=0,i,cleanup=b->argc;v->bridge_calls++;if(v->trace)printf("QRX86 BRIDGE %s @%08lx esp=%08lx\n",b->name,(unsigned long)b->addr,(unsigned long)c->r[4]);
if(!strcmp(b->name,"RtlZeroMemory")){uint8_t*p=vm_ptr(v,a0,a1,1);if(!p)ret=0;else{memset(p,0,a1);ret=a0;}}
else if(!strcmp(b->name,"RtlFillMemory")){uint8_t*p=vm_ptr(v,a0,a1,1);if(p)memset(p,(int)(a2&0xff),a1);ret=a0;}
else if(!strcmp(b->name,"RtlMoveMemory")||!strcmp(b->name,"RtlCopyMemory")){uint8_t*d=vm_ptr(v,a0,a2,1),*s=vm_ptr(v,a1,a2,0);if(d&&s)memmove(d,s,a2);ret=a0;}
else if(!strcmp(b->name,"RtlCompareMemory")){uint8_t*x=vm_ptr(v,a0,a2,0),*y=vm_ptr(v,a1,a2,0);ret=0;if(x&&y)for(i=0;i<a2&&x[i]==y[i];i++)ret++;}
else if(!strcmp(b->name,"RtlGetProcessHeap")){ret=v->heap_handle;}
else if(!strcmp(b->name,"RtlAllocateHeap")){ret=heap_alloc(v,a2);if(!ret)v->last_error=8;}
else if(!strcmp(b->name,"RtlFreeHeap")){ret=heap_region(v,a2)?1:0;}
else if(!strcmp(b->name,"RtlSizeHeap")){QRXRegion*r=heap_region(v,a2);ret=r?r->size:0xffffffffu;}
else if(!strcmp(b->name,"RtlReAllocateHeap")){QRXRegion*r=heap_region(v,a2);uint32_t nb=heap_alloc(v,a3);if(r&&nb){uint32_t n=r->size<a3?r->size:a3;uint8_t*d=vm_ptr(v,nb,n,1);if(d)memcpy(d,r->data,n);ret=nb;}else ret=0;}
else if(!strcmp(b->name,"RtlGetLastWin32Error")){vm_r32(v,v->teb_base+0x34,&ret);}
else if(!strcmp(b->name,"RtlSetLastWin32Error")){v->last_error=a0;vm_w32(v,v->teb_base+0x34,a0);ret=0;}
else if(!strcmp(b->name,"RtlNtStatusToDosError")||!strcmp(b->name,"RtlNtStatusToDosErrorNoTeb")){ret=status_to_dos(a0);}
else if(!strcmp(b->name,"RtlInitAnsiString")){uint32_t n=0;if(a1){uint8_t ch;while(n<0xffffu&&vm_r8(v,a1+n,&ch)&&ch)n++;}vm_w16(v,a0,(uint16_t)n);vm_w16(v,a0+2,(uint16_t)(n+1));vm_w32(v,a0+4,a1);ret=0;}
else if(!strcmp(b->name,"RtlInitUnicodeString")){uint32_t n=0;if(a1){uint16_t ch;while(n<0xfffeu&&vm_r16(v,a1+n,&ch)&&ch)n+=2;}vm_w16(v,a0,(uint16_t)n);vm_w16(v,a0+2,(uint16_t)(n+2));vm_w32(v,a0+4,a1);ret=0;}
else if(!strcmp(b->name,"RtlUnicodeStringToAnsiString")){ret=unicode_to_ansi(v,a0,a1,a2!=0)?STATUS_SUCCESS:STATUS_NO_MEMORY;}
else if(!strcmp(b->name,"RtlAnsiStringToUnicodeString")){ret=ansi_to_unicode(v,a0,a1,a2!=0)?STATUS_SUCCESS:STATUS_NO_MEMORY;}
else if(!strcmp(b->name,"RtlFreeAnsiString")||!strcmp(b->name,"RtlFreeUnicodeString")){if(a0){vm_w16(v,a0,0);vm_w16(v,a0+2,0);vm_w32(v,a0+4,0);}ret=0;}
else if(!strcmp(b->name,"RtlCreateUnicodeString")){uint32_t n=guest_strlen16(v,a1),db=heap_alloc(v,n+2),i;uint16_t ch;if(!db){ret=0;}else{for(i=0;i<n;i+=2){if(!vm_r16(v,a1+i,&ch)||!vm_w16(v,db+i,ch))break;}vm_w16(v,db+n,0);vm_w16(v,a0,(uint16_t)n);vm_w16(v,a0+2,(uint16_t)(n+2));vm_w32(v,a0+4,db);ret=1;}}
else if(!strcmp(b->name,"RtlDosPathNameToNtPathName_U")){uint32_t n=guest_strlen16(v,a0),db=heap_alloc(v,n+10),i;static const uint16_t pre[4]={'\\','?','?','\\'};uint16_t ch;if(!db){ret=0;}else{for(i=0;i<4;i++)vm_w16(v,db+i*2,pre[i]);for(i=0;i<n;i+=2){if(!vm_r16(v,a0+i,&ch))break;vm_w16(v,db+8+i,ch);}vm_w16(v,db+8+n,0);vm_w16(v,a1,(uint16_t)(n+8));vm_w16(v,a1+2,(uint16_t)(n+10));vm_w32(v,a1+4,db);if(a2)vm_w32(v,a2,0);if(a3){for(i=0;i<16;i+=4)vm_w32(v,a3+i,0);}ret=1;}}
else if(!strcmp(b->name,"RtlInterlockedCompareExchange64")){uint32_t exlo=arg32(v,c,1),exhi=arg32(v,c,2),cmplo=arg32(v,c,3),cmphi=arg32(v,c,4),oldlo=0,oldhi=0;cleanup=5;if(vm_r32(v,a0,&oldlo)&&vm_r32(v,a0+4,&oldhi)&&oldlo==cmplo&&oldhi==cmphi){vm_w32(v,a0,exlo);vm_w32(v,a0+4,exhi);}c->r[0]=oldlo;c->r[2]=oldhi;ret=oldlo;}
else if(!strcmp(b->name,"RtlAcquirePebLock")||!strcmp(b->name,"RtlReleasePebLock")||!strcmp(b->name,"RtlEnterCriticalSection")||!strcmp(b->name,"RtlLeaveCriticalSection")||!strcmp(b->name,"RtlInitializeCriticalSection")||!strcmp(b->name,"RtlDeleteCriticalSection")||!strcmp(b->name,"RtlInitializeCriticalSectionAndSpinCount")){ret=0;}
else if(!strcmp(b->name,"DbgPrint")||!strcmp(b->name,"DbgPrintEx")||!strcmp(b->name,"vDbgPrintEx")||!strcmp(b->name,"vDbgPrintExWithPrefix")){ret=0;}
else if(!strcmp(b->name,"NtClose")||!strcmp(b->name,"NtDelayExecution")||!strcmp(b->name,"NtYieldExecution")||!strcmp(b->name,"NtSetInformationThread")){ret=STATUS_SUCCESS;}
else if(!strcmp(b->name,"NtQuerySystemTime")){time_t now=time(NULL);uint64_t t=((uint64_t)now+11644473600ULL)*10000000ULL;if(a0){vm_w32(v,a0,(uint32_t)t);vm_w32(v,a0+4,(uint32_t)(t>>32));}ret=STATUS_SUCCESS;}
else if(!strcmp(b->name,"NtQueryPerformanceCounter")){uint64_t q=(uint64_t)clock();if(a0){vm_w32(v,a0,(uint32_t)q);vm_w32(v,a0+4,(uint32_t)(q>>32));}if(a1){uint64_t f=(uint64_t)CLOCKS_PER_SEC;vm_w32(v,a1,(uint32_t)f);vm_w32(v,a1+4,(uint32_t)(f>>32));}ret=STATUS_SUCCESS;}
else if(!strcmp(b->name,"NtAllocateVirtualMemory")){uint32_t pbase=arg32(v,c,1),psize=arg32(v,c,3),sz=0,nb;if(!vm_r32(v,psize,&sz))ret=STATUS_INVALID_PARAMETER;else{nb=heap_alloc(v,sz);if(!nb)ret=STATUS_NO_MEMORY;else{vm_w32(v,pbase,nb);vm_w32(v,psize,align_up(sz,16));ret=STATUS_SUCCESS;}}}
else if(!strcmp(b->name,"NtQueryInformationProcess")){uint32_t cls=a1,buf=a2,len=a3,retlen=arg32(v,c,4);if(cls==0&&buf&&len>=24){vm_w32(v,buf+0,STATUS_SUCCESS);vm_w32(v,buf+4,v->peb_base);vm_w32(v,buf+8,1);vm_w32(v,buf+12,8);vm_w32(v,buf+16,1);vm_w32(v,buf+20,0);if(retlen)vm_w32(v,retlen,24);ret=STATUS_SUCCESS;}else ret=STATUS_NOT_IMPLEMENTED;}
else if(!strcmp(b->name,"NtQueryInformationThread")){uint32_t cls=a1,buf=a2,len=a3,retlen=arg32(v,c,4);if(cls==0&&buf&&len>=28){for(i=0;i<28;i+=4)vm_w32(v,buf+i,0);vm_w32(v,buf+4,v->teb_base);vm_w32(v,buf+20,1);if(retlen)vm_w32(v,retlen,28);ret=STATUS_SUCCESS;}else ret=STATUS_NOT_IMPLEMENTED;}
else if(!strcmp(b->name,"NtQueryVirtualMemory")){uint32_t addr=a1,buf=arg32(v,c,3),len=arg32(v,c,4),retlen=arg32(v,c,5);QRXRegion*r=region_find(v,addr,1);if(r&&buf&&len>=28){vm_w32(v,buf+0,r->base);vm_w32(v,buf+4,r->base);vm_w32(v,buf+8,4);vm_w32(v,buf+12,r->size);vm_w32(v,buf+16,0x1000);vm_w32(v,buf+20,r->writable?4:2);vm_w32(v,buf+24,0x20000);if(retlen)vm_w32(v,retlen,28);ret=STATUS_SUCCESS;}else ret=STATUS_NOT_IMPLEMENTED;}
else if(!strcmp(b->name,"NtFreeVirtualMemory")||!strcmp(b->name,"NtProtectVirtualMemory")){ret=STATUS_SUCCESS;}
else if(!strcmp(b->name,"RtlRaiseStatus")||!strcmp(b->name,"RtlExitUserProcess")||!strcmp(b->name,"NtTerminateProcess")){c->halted=1;ret=a0;}
else {ret=STATUS_NOT_IMPLEMENTED;if(v->trace)printf("QRX86 BRIDGE TODO %s -> STATUS_NOT_IMPLEMENTED\n",b->name);}
c->r[0]=ret;if(c->halted)return 1;if(!pop32(v,c,&c->eip)){memfault(c);return 0;}if(!(b->flags&BR_CDECL))c->r[4]+=cleanup*4u;return 1;}

static QRXBlock*block_find(QRXVM*v,uint32_t eip){unsigned i;for(i=0;i<v->block_count;i++)if(v->blocks[i].eip==eip)return &v->blocks[i];return NULL;}
static QRXBlock*block_build(QRXVM*v,QRXCPU*c){QRXBlock*b;QRXCPU t=*c;unsigned i;if(v->block_count>=QRX_MAX_BLOCKS)return NULL;b=&v->blocks[v->block_count++];memset(b,0,sizeof(*b));b->eip=c->eip;for(i=0;i<QRX_MAX_BLOCK_INSNS;i++){int term=0;b->insn_eip[b->count++]=t.eip;/* decode by executing against a private CPU but same memory is unsafe for writes.
 * Therefore the foundation records conservative one-instruction blocks for any
 * instruction that might touch memory, and grows only through obviously pure
 * prefixes/NOP/register-immediate forms by observing opcode bytes. */
{uint8_t op=0;if(!vm_r8(v,t.eip,&op))break;if(op==0x90){t.eip++;continue;}if(op>=0xb8&&op<=0xbf){t.eip+=5;continue;}if(op>=0x40&&op<=0x5f){t.eip++;continue;}}
b->terminal=1;(void)term;break;}if(!b->count)b->count=1;return b;}

/* Native threaded-JIT foundation for 68k. Each generated basic block is real
 * executable m68k code: a sequence of JSR absolute to a helper followed by RTS.
 * The helper executes the already selected instruction EIP from the cached block.
 * This removes dispatcher lookup between instructions and establishes executable
 * code-cache/invalidation/cache-flush machinery. Direct per-op m68k emission can
 * replace these helper calls incrementally without changing block ABI. */
#if defined(__mc68000__) || defined(__m68k__)
static void jit_thread_helper(void){if(!g_jit_vm||!g_jit_cpu||!g_jit_block)return;if(g_jit_cpu->fault||g_jit_cpu->halted)return;if(g_jit_index>=g_jit_block->count)return;{uint32_t expected=g_jit_block->insn_eip[g_jit_index++];int term=0;if(g_jit_cpu->eip!=expected)return;(void)step_one(g_jit_vm,g_jit_cpu,&term);}}
static void put_be16(uint8_t*p,uint16_t x){p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static void put_be32(uint8_t*p,uint32_t x){p[0]=(uint8_t)(x>>24);p[1]=(uint8_t)(x>>16);p[2]=(uint8_t)(x>>8);p[3]=(uint8_t)x;}
static int jit_compile_native(QRXVM*v,QRXBlock*b){unsigned i;uint32_t n=b->count*6u+2u;uint8_t*p=(uint8_t*)malloc(n),*q=p;uintptr_t fn=(uintptr_t)&jit_thread_helper;(void)v;if(!p)return 0;for(i=0;i<b->count;i++){put_be16(q,0x4eb9);put_be32(q+2,(uint32_t)fn);q+=6;}put_be16(q,0x4e75);b->native_code=p;b->native_size=n;CacheClearU();return 1;}
typedef void(*QRXNativeBlockFn)(void);
#endif
static int run_block(QRXVM*v,QRXCPU*c,QRXBlock*b){unsigned i;b->hits++;v->blocks_executed++;
#if defined(__mc68000__) || defined(__m68k__)
if(v->jit_enabled&&b->hits>=v->jit_threshold){if(!b->native_code)jit_compile_native(v,b);if(b->native_code){g_jit_vm=v;g_jit_cpu=c;g_jit_block=b;g_jit_index=0;((QRXNativeBlockFn)b->native_code)();v->jit_blocks_executed++;return !c->fault;}}
#endif
for(i=0;i<b->count&&!c->fault&&!c->halted;i++){int term=0;if(c->eip!=b->insn_eip[i])break;if(v->steps>=v->step_deadline){c->fault=5;return 0;}v->steps++;if(!step_one(v,c,&term))return 0;v->interp_steps++;if(term)break;}return !c->fault;}

static void trace_fault(QRXVM*v,QRXCPU*c,const char*where){uint32_t rva=0;const char*m=module_for(v,c->unsupported_eip?c->unsupported_eip:c->eip,&rva);unsigned i;printf("QRX86 STOP where=%s fault=%lu module=%s eip=%08lx rva=%08lx bytes=",where,(unsigned long)c->fault,m,(unsigned long)(c->unsupported_eip?c->unsupported_eip:c->eip),(unsigned long)rva);for(i=0;i<c->unsupported_len;i++)printf("%02x",c->unsupported_bytes[i]);printf("\n");}

static int run_until_return(QRXVM*v,QRXCPU*c,uint32_t target,uint32_t hinst,uint32_t reason,int is_dll,const char*label){uint64_t start_steps=v->steps;QRXBridgeRec*br;c->halted=0;c->fault=0;c->unsupported_len=0;c->eip=target;c->r[4]=QRX_STACK_BASE+QRX_STACK_SIZE-64;c->r[5]=0;c->fs_base=v->teb_base;c->eflags=0x202u;if(is_dll){if(!push32(v,c,0)||!push32(v,c,reason)||!push32(v,c,hinst)||!push32(v,c,QRX_SENTINEL_RETURN))return 0;}else{if(!push32(v,c,QRX_SENTINEL_RETURN))return 0;}
v->step_deadline=start_steps+v->max_steps;if(v->step_deadline<start_steps)v->step_deadline=~(uint64_t)0;
if(v->verbose)printf("QRX86 EXEC %-26s target=%08lx module=%s\n",label,(unsigned long)target,module_for(v,target,NULL));
while(!c->halted&&v->steps-start_steps<v->max_steps){if(c->eip==QRX_SENTINEL_RETURN){if(v->verbose)printf("QRX86 RETURN %-24s eax=%08lx steps=%lu\n",label,(unsigned long)c->r[0],(unsigned long)(v->steps-start_steps));return 1;}br=bridge_for(v,c->eip);if(br){if(v->steps>=v->step_deadline){c->fault=5;break;}v->steps++;if(!bridge_dispatch(v,c,br))break;continue;}{QRXBlock*b=block_find(v,c->eip);if(!b)b=block_build(v,c);if(!b){c->fault=4;break;}if(v->trace){if(v->trace_lines<2000u){uint32_t rva=0;const char *mn=module_for(v,c->eip,&rva);printf("QRX86 BLOCK module=%s eip=%08lx rva=%08lx count=%u hits=%lu jit=%s\n",mn,(unsigned long)c->eip,(unsigned long)rva,(unsigned)b->count,(unsigned long)b->hits,b->native_code?"native":"ir/interp");v->trace_lines++;}else if(v->trace_lines==2000u){puts("QRX86 TRACE: output limit reached; execution continues under watchdog");v->trace_lines++;}}if(!run_block(v,c,b))break;}}
if(v->steps-start_steps>=v->max_steps){c->fault=5;printf("QRX86 WATCHDOG: guest instruction budget reached (%lu); stopping guest safely\n",(unsigned long)v->max_steps);}
if(c->fault)trace_fault(v,c,label);
return 0;}

static const char*init_name(uint32_t k){switch(k){case INIT_HOST_NTDLL:return"HOST_NTDLL_BOOTSTRAP";case INIT_TLS:return"DLL_TLS_PROCESS_ATTACH";case INIT_DLLMAIN:return"DLLMAIN_PROCESS_ATTACH";case INIT_EXE_TLS:return"EXE_TLS_PROCESS_ATTACH";case INIT_EXE_ENTRY:return"EXE_ENTRY";default:return"UNKNOWN";}}
static int execute_plan(QRXVM*v,int stop_before_entry){QRXCPU c;unsigned i;memset(&c,0,sizeof(c));c.r[4]=QRX_STACK_BASE+QRX_STACK_SIZE-64;c.fs_base=v->teb_base;c.eflags=0x202u;for(i=0;i<v->init_count;i++){QRXInitRec*r=&v->init[i];if(r->module_index>=v->module_count){printf("QRX86: bad init module index\n");return 0;}if(r->kind==INIT_HOST_NTDLL){if(v->verbose)printf("QRX86 INIT %03u HOST_NTDLL_BOOTSTRAP TEB=%08lx PEB=%08lx\n",i,(unsigned long)v->teb_base,(unsigned long)v->peb_base);continue;}if(stop_before_entry&&r->kind==INIT_EXE_ENTRY){puts("QRX86: --init-only reached EXE entry; stopping after TLS/DllMain");break;}if(!run_until_return(v,&c,r->target,r->hinst,r->reason,r->kind!=INIT_EXE_ENTRY,init_name(r->kind))){printf("QRX86 INIT FAIL index=%u kind=%s module=%s\n",i,init_name(r->kind),v->modules[r->module_index].name);return 0;}}
printf("QRX86 EXEC PASS init-calls=%u steps=%lu blocks=%u block-runs=%lu native-jit-runs=%lu interp-steps=%lu bridges=%lu\n",v->init_count,(unsigned long)v->steps,v->block_count,(unsigned long)v->blocks_executed,(unsigned long)v->jit_blocks_executed,(unsigned long)v->interp_steps,(unsigned long)v->bridge_calls);return 1;}

static void dump(QRXVM*v){unsigned i;printf("QRX86GRT handoff modules=%u bridges=%u init=%u root=%u teb=%08lx peb=%08lx heap=%08lx\n",v->module_count,v->bridge_count,v->init_count,v->root_index,(unsigned long)v->teb_base,(unsigned long)v->peb_base,(unsigned long)v->heap_handle);for(i=0;i<v->module_count;i++)printf("  MOD %03u %08lx-%08lx entry=%08lx %s\n",i,(unsigned long)v->modules[i].base,(unsigned long)(v->modules[i].base+v->modules[i].size),(unsigned long)(v->modules[i].base+v->modules[i].entry_rva),v->modules[i].name);}
static void usage(void){puts("QRX86GRTExec - QRXGRT1 executor + m68k tiered JIT foundation");puts("Usage: QRX:C/QRX86GRTExec [options] HANDOFF.qrxgrt");puts("  --trace             block/bridge trace");puts("  --quiet             concise output");puts("  --jit=on|off        EXPERIMENTAL unsafe native m68k threaded blocks; default off");puts("  --jit-threshold N   compile after N block hits (default 2)");puts("  --max-steps N       watchdog budget per init call (trace default: 100000)");puts("  --init-only         execute TLS/DllMain but stop before EXE entry");puts("  --dump              dump mapped modules and exit");puts("Exit 0 means the selected guest init path actually returned through QRX86.");}
/* 22.5.11: QRXVM is ~1.7 MB. It must never live on the AmigaDOS process stack
 * (typically 4-64 KB, no MMU guard on m68k): a stack overflow silently corrupts
 * foreign memory. Keep it in BSS so LoadSeg allocates it or fails cleanly. */
static QRXVM v;
int main(int argc,char**argv){const char*path=NULL;int dump_only=0,init_only=0,i,rc=1;if(argc<2){usage();return 2;}memset(&v,0,sizeof(v));v.verbose=1;v.jit_enabled=0;v.jit_threshold=QRX_BLOCK_HOT_THRESHOLD;v.max_steps=QRX_MAX_STEPS;for(i=1;i<argc;i++){if(!strcmp(argv[i],"--trace"))v.trace=1;else if(!strcmp(argv[i],"--quiet"))v.verbose=0;else if(!strcmp(argv[i],"--dump"))dump_only=1;else if(!strcmp(argv[i],"--init-only"))init_only=1;else if(!strncmp(argv[i],"--jit=",6)){if(!strcmp(argv[i]+6,"off"))v.jit_enabled=0;else if(!strcmp(argv[i]+6,"on"))v.jit_enabled=1;else{usage();return 2;}}else if(!strcmp(argv[i],"--jit-threshold")&&i+1<argc){v.jit_threshold=(unsigned)strtoul(argv[++i],NULL,0);if(!v.jit_threshold)v.jit_threshold=1;}else if(!strcmp(argv[i],"--max-steps")&&i+1<argc){v.max_steps=(uint64_t)strtoul(argv[++i],NULL,0);if(!v.max_steps)v.max_steps=100000u;}else if(!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")){usage();return 0;}else if(argv[i][0]=='-'){printf("unknown option: %s\n",argv[i]);return 2;}else path=argv[i];}
/* load_handoff initializes defaults, so preserve CLI knobs */
{unsigned tr=v.trace,ve=v.verbose,je=v.jit_enabled,jt=v.jit_threshold;uint64_t ms=v.max_steps;if(!path||!load_handoff(path,&v))return 1;v.trace=tr;v.verbose=ve;v.jit_enabled=je;v.jit_threshold=jt?jt:QRX_BLOCK_HOT_THRESHOLD;v.max_steps=ms?ms:QRX_MAX_STEPS;if(v.trace&&v.max_steps==QRX_MAX_STEPS)v.max_steps=100000u;if(v.jit_enabled){puts("QRX86 SAFETY: native JIT quarantined pending on-target instruction-cache/ABI validation; using interpreter");v.jit_enabled=0;}}
if(!bootstrap_vm(&v)){printf("QRX86GRT: VM bootstrap failed\n");goto done;}if(dump_only){dump(&v);rc=0;goto done;}printf("QRX86GRTExec: %s modules=%u bridges=%u init=%u JIT=%s threshold=%u\n",path,v.module_count,v.bridge_count,v.init_count,v.jit_enabled?"on":"off",v.jit_threshold);
#if defined(__mc68000__) || defined(__m68k__)
puts("QRX86GRTExec: backend=m68k native-threaded JIT + interpreter fallback");
#else
puts("QRX86GRTExec: backend=portable interpreter/block-cache (native m68k JIT unavailable on this host)");
#endif
if(execute_plan(&v,init_only))rc=0;
done:
free_vm(&v);
return rc;}
