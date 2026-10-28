/* QRX OS One - QRX86/Win32 guest PE32 module loader foundation
 * Maps PE32/i386 EXE/DLL images, applies HIGHLOW relocations, recursively
 * resolves imports against ReactOS System32, supports ordinals and forwarded
 * exports, and produces a deterministic module map for the QRX86 executor.
 *
 * It deliberately DOES NOT execute x86 code or DllMain yet.  The mapped module
 * graph is the hand-off boundary for the next QRX86 execution/NT-host bridge.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

/* Deadwood's native AROS/m68k SDK can expose the libc symbols at link time
 * without publishing the ISO-C stdio/stdlib declarations through the selected
 * include set. Keep the PE loader independent of that header quirk. FILE is
 * opaque here; only FILE* crosses the ABI. */
/* 22.5.11: only when the selected SDK stdio.h did not declare FILE/fopen
 * itself - a complete Deadwood SDK (posixc/stdc stdio) conflicts with this shim. */
#if defined(__AROS__) && !defined(_STDC_STDIO_H_) && !defined(_POSIXC_STDIO_H_) && !defined(_STDIO_H) && !defined(_STDIO_H_)
typedef void QRX_FILE;
extern QRX_FILE *fopen(const char *, const char *);
extern int fclose(QRX_FILE *);
extern int fseek(QRX_FILE *, long, int);
extern long ftell(QRX_FILE *);
extern size_t fread(void *, size_t, size_t, QRX_FILE *);
extern int printf(const char *, ...);
extern int puts(const char *);
extern int snprintf(char *, size_t, const char *, ...);
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern unsigned long strtoul(const char *, char **, int);
#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif
#define FILE QRX_FILE
#endif

#define QRX_MAX_MODULES 128
#define QRX_MAX_PATH 512
#define QRX_MAX_NAME 128
#define QRX_MAX_DEPTH 64
#define QRX_DEFAULT_SYSTEM32 "QRX:Windows/ReactOS/System32"
#define QRX_GUEST_FALLBACK_BASE 0x10000000u
#define QRX_GUEST_ALIGN 0x00010000u

#define IMAGE_FILE_DLL 0x2000u
#define IMAGE_REL_BASED_ABSOLUTE 0u
#define IMAGE_REL_BASED_HIGHLOW 3u
#define IMAGE_ORDINAL_FLAG32 0x80000000u

typedef struct QRXPEMeta {
    uint16_t machine;
    uint16_t sections;
    uint16_t opt_size;
    uint16_t characteristics;
    uint32_t entry_rva;
    uint32_t image_base;
    uint32_t size_image;
    uint32_t size_headers;
    uint32_t export_rva, export_size;
    uint32_t import_rva, import_size;
    uint32_t reloc_rva, reloc_size;
    uint32_t tls_rva, tls_size;
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
    uint32_t preferred_base;
    uint32_t guest_base;
    uint32_t entry_rva;
    uint32_t export_rva, export_size;
    uint32_t import_rva, import_size;
    uint32_t reloc_rva, reloc_size;
    uint32_t tls_rva, tls_size;
    uint16_t characteristics;
    uint16_t subsystem;
    unsigned state; /* 1 loading, 2 loaded, 3 failed */
    unsigned imports_seen;
    unsigned imports_resolved;
    unsigned forwarders;
    unsigned relocations;
    unsigned unsupported_relocs;
} QRXModule;

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
    uint32_t next_base;
    char system32[QRX_MAX_PATH];
    int verbose;
} QRXLoader;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

static int qrx_tolower(int c) { return tolower((unsigned char)c); }
static int qrx_stricmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca=qrx_tolower(*a), cb=qrx_tolower(*b);
        if (ca != cb) return ca-cb;
        ++a; ++b;
    }
    return qrx_tolower(*a)-qrx_tolower(*b);
}
static void qrx_lower(char *s) { while (*s) { *s=(char)qrx_tolower(*s); ++s; } }
static const char *qrx_basename(const char *p) {
    const char *b=p, *q;
    for (q=p; *q; ++q) if (*q=='/' || *q=='\\' || *q==':') b=q+1;
    return b;
}
static int qrx_has_dot(const char *s) { return strchr(s,'.') != NULL; }

static uint8_t *read_whole(const char *path, size_t *out_size) {
    FILE *f=fopen(path,"rb"); long n; uint8_t *b;
    if (!f) return NULL;
    if (fseek(f,0,SEEK_END)!=0) { fclose(f); return NULL; }
    n=ftell(f); if (n<=0) { fclose(f); return NULL; }
    if (fseek(f,0,SEEK_SET)!=0) { fclose(f); return NULL; }
    b=(uint8_t*)malloc((size_t)n);
    if (!b) { fclose(f); return NULL; }
    if (fread(b,1,(size_t)n,f)!=(size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *out_size=(size_t)n; return b;
}

static int range_ok(size_t size, uint32_t off, uint32_t len) {
    return (uint64_t)off + (uint64_t)len <= (uint64_t)size;
}

static int pe_meta(const uint8_t *b, size_t n, QRXPEMeta *m, char *why, size_t whycap) {
    uint32_t pe, opt; uint16_t magic;
    memset(m,0,sizeof(*m));
    if (n<0x40 || b[0]!='M' || b[1]!='Z') { snprintf(why,whycap,"not MZ"); return 0; }
    pe=rd32(b+0x3c);
    if (!range_ok(n,pe,24) || memcmp(b+pe,"PE\0\0",4)!=0) { snprintf(why,whycap,"not PE"); return 0; }
    m->machine=rd16(b+pe+4);
    m->sections=rd16(b+pe+6);
    m->opt_size=rd16(b+pe+20);
    m->characteristics=rd16(b+pe+22);
    opt=pe+24;
    if (!range_ok(n,opt,m->opt_size) || m->opt_size<96) { snprintf(why,whycap,"bad optional header"); return 0; }
    magic=rd16(b+opt);
    if (magic!=0x10b) { snprintf(why,whycap,"not PE32 (magic=%04x)",(unsigned)magic); return 0; }
    if (m->machine!=0x014c) { snprintf(why,whycap,"not i386 (machine=%04x)",(unsigned)m->machine); return 0; }
    m->entry_rva=rd32(b+opt+16);
    m->image_base=rd32(b+opt+28);
    m->size_image=rd32(b+opt+56);
    m->size_headers=rd32(b+opt+60);
    m->subsystem=rd16(b+opt+68);
    if (m->opt_size>=104) {
        m->export_rva=rd32(b+opt+96);  m->export_size=rd32(b+opt+100);
    }
    if (m->opt_size>=112) {
        m->import_rva=rd32(b+opt+104); m->import_size=rd32(b+opt+108);
    }
    if (m->opt_size>=144) {
        m->reloc_rva=rd32(b+opt+136);  m->reloc_size=rd32(b+opt+140);
    }
    if (m->opt_size>=176) {
        m->tls_rva=rd32(b+opt+168);    m->tls_size=rd32(b+opt+172);
    }
    m->sec_table_off=opt+m->opt_size;
    if (!m->sections || m->sections>96 || !range_ok(n,m->sec_table_off,(uint32_t)m->sections*40u)) {
        snprintf(why,whycap,"bad section table"); return 0;
    }
    if (!m->size_image || m->size_image>0x20000000u) { snprintf(why,whycap,"invalid SizeOfImage"); return 0; }
    return 1;
}

static int guest_range_used(const QRXLoader *l, uint32_t base, uint32_t size) {
    uint64_t a0=base, a1=(uint64_t)base+size; unsigned i;
    for (i=0;i<l->count;i++) {
        const QRXModule *m=&l->modules[i]; uint64_t b0=m->guest_base, b1=(uint64_t)m->guest_base+m->image_size;
        if (m->guest_base && a0<b1 && b0<a1) return 1;
    }
    return 0;
}

static uint32_t choose_guest_base(QRXLoader *l, uint32_t preferred, uint32_t size) {
    uint32_t b;
    size=align_up(size,QRX_GUEST_ALIGN);
    if (preferred>=0x00010000u && !guest_range_used(l,preferred,size)) return preferred;
    b=align_up(l->next_base,QRX_GUEST_ALIGN);
    while (guest_range_used(l,b,size)) {
        if (b>0xF0000000u-size) return 0;
        b=align_up(b+size,QRX_GUEST_ALIGN);
    }
    l->next_base=b+size;
    return b;
}

static int map_image(QRXModule *m, const QRXPEMeta *meta, char *why, size_t whycap) {
    unsigned i;
    m->image=(uint8_t*)calloc(1,meta->size_image);
    if (!m->image) { snprintf(why,whycap,"out of memory mapping %u bytes",(unsigned)meta->size_image); return 0; }
    m->image_size=meta->size_image;
    {
        uint32_t nh=meta->size_headers;
        if (nh>m->file_size) nh=(uint32_t)m->file_size;
        if (nh>m->image_size) nh=m->image_size;
        memcpy(m->image,m->file,nh);
    }
    for (i=0;i<meta->sections;i++) {
        uint32_t o=meta->sec_table_off+i*40u;
        uint32_t vsize=rd32(m->file+o+8), va=rd32(m->file+o+12);
        uint32_t rawsize=rd32(m->file+o+16), raw=rd32(m->file+o+20);
        uint32_t take=rawsize;
        (void)vsize;
        if (!rawsize) continue;
        if (va>=m->image_size || raw>=m->file_size) continue;
        if ((uint64_t)va+take>m->image_size) take=m->image_size-va;
        if ((uint64_t)raw+take>m->file_size) take=(uint32_t)(m->file_size-raw);
        memcpy(m->image+va,m->file+raw,take);
    }
    return 1;
}

static int apply_relocs(QRXModule *m, char *why, size_t whycap) {
    uint32_t pos,end; int64_t delta=(int64_t)(uint64_t)m->guest_base-(int64_t)(uint64_t)m->preferred_base;
    if (!delta) return 1;
    if (!m->reloc_rva || !m->reloc_size) {
        snprintf(why,whycap,"relocation required but image has no base relocation directory");
        return 0;
    }
    if ((uint64_t)m->reloc_rva+m->reloc_size>m->image_size) { snprintf(why,whycap,"relocation directory out of image"); return 0; }
    pos=m->reloc_rva; end=m->reloc_rva+m->reloc_size;
    while (pos+8<=end) {
        uint32_t page=rd32(m->image+pos), block=rd32(m->image+pos+4), q;
        if (block<8 || pos+block>end) { snprintf(why,whycap,"bad relocation block"); return 0; }
        for (q=pos+8;q+2<=pos+block;q+=2) {
            uint16_t e=rd16(m->image+q); unsigned type=e>>12; uint32_t rva=page+(e&0x0fffu);
            if (type==IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type==IMAGE_REL_BASED_HIGHLOW) {
                uint32_t v;
                if (rva+4>m->image_size) { snprintf(why,whycap,"HIGHLOW relocation out of image"); return 0; }
                v=rd32(m->image+rva); wr32(m->image+rva,(uint32_t)((int64_t)(uint64_t)v+delta)); m->relocations++;
            } else m->unsupported_relocs++;
        }
        pos+=block;
    }
    if (m->unsupported_relocs) {
        snprintf(why,whycap,"unsupported relocation types=%u",m->unsupported_relocs);
        return 0;
    }
    return 1;
}

static QRXModule *find_module(QRXLoader *l, const char *name) {
    unsigned i; for (i=0;i<l->count;i++) if (qrx_stricmp(l->modules[i].name,name)==0) return &l->modules[i];
    return NULL;
}

static int cstr_image(const QRXModule *m, uint32_t rva, char *out, size_t cap) {
    size_t i=0;
    if (rva>=m->image_size || cap<2) return 0;
    while (rva+i<m->image_size && i+1<cap) { char c=(char)m->image[rva+i]; out[i++]=c; if (!c) return 1; }
    out[cap-1]=0; return 0;
}

static QRXModule *load_dependency(QRXLoader *l, const char *name, unsigned depth);
static uint32_t resolve_export(QRXLoader *l, QRXModule *m, const char *name, uint16_t ordinal, int by_ordinal, unsigned depth, int *ok);

static int resolve_forwarder(QRXLoader *l, const char *forward, unsigned depth, uint32_t *addr) {
    char mod[QRX_MAX_NAME], sym[QRX_MAX_NAME], tmp[QRX_MAX_NAME]; const char *dot=strrchr(forward,'.'); QRXModule *m; int ok=0;
    if (!dot || dot==forward || !dot[1]) return 0;
    if ((size_t)(dot-forward)>=sizeof(mod)) return 0;
    memcpy(mod,forward,(size_t)(dot-forward)); mod[dot-forward]=0;
    snprintf(sym,sizeof(sym),"%s",dot+1);
    snprintf(tmp,sizeof(tmp),"%s",mod);
    if (!qrx_has_dot(tmp)) strncat(tmp,".dll",sizeof(tmp)-strlen(tmp)-1);
    qrx_lower(tmp);
    m=load_dependency(l,tmp,depth+1);
    if (!m || m->state!=2) return 0;
    if (sym[0]=='#') {
        unsigned long o=strtoul(sym+1,NULL,10);
        *addr=resolve_export(l,m,NULL,(uint16_t)o,1,depth+1,&ok);
    } else *addr=resolve_export(l,m,sym,0,0,depth+1,&ok);
    return ok;
}

static uint32_t resolve_export(QRXLoader *l, QRXModule *m, const char *name, uint16_t ordinal, int by_ordinal, unsigned depth, int *ok) {
    uint8_t *e; uint32_t base,nfunc,nname,funcs,names,ords,index=0xffffffffu,frva; unsigned i;
    *ok=0;
    if (depth>QRX_MAX_DEPTH || !m->export_rva || m->export_rva+40>m->image_size) return 0;
    e=m->image+m->export_rva;
    base=rd32(e+16); nfunc=rd32(e+20); nname=rd32(e+24); funcs=rd32(e+28); names=rd32(e+32); ords=rd32(e+36);
    if (!nfunc || funcs+4u*nfunc>m->image_size) return 0;
    if (by_ordinal) {
        if ((uint32_t)ordinal<base || (uint32_t)ordinal>=base+nfunc) return 0;
        index=(uint32_t)ordinal-base;
    } else {
        if (!name || names+4u*nname>m->image_size || ords+2u*nname>m->image_size) return 0;
        for (i=0;i<nname;i++) {
            uint32_t nrva=rd32(m->image+names+4u*i); char nb[QRX_MAX_NAME];
            if (!cstr_image(m,nrva,nb,sizeof(nb))) continue;
            if (strcmp(nb,name)==0) { index=rd16(m->image+ords+2u*i); break; }
        }
        if (index==0xffffffffu || index>=nfunc) return 0;
    }
    frva=rd32(m->image+funcs+4u*index);
    if (!frva) return 0;
    if (m->export_size && frva>=m->export_rva && frva<m->export_rva+m->export_size) {
        char fw[QRX_MAX_PATH]; uint32_t a=0;
        if (!cstr_image(m,frva,fw,sizeof(fw))) return 0;
        if (!resolve_forwarder(l,fw,depth+1,&a)) return 0;
        m->forwarders++; l->total_forwarders++; *ok=1; return a;
    }
    if (frva>=m->image_size) return 0;
    *ok=1; return m->guest_base+frva;
}

static int process_imports(QRXLoader *l, QRXModule *m, unsigned depth, char *why, size_t whycap) {
    uint32_t pos,end;
    if (!m->import_rva || !m->import_size) return 1;
    if ((uint64_t)m->import_rva+m->import_size>m->image_size) { snprintf(why,whycap,"import directory out of image"); return 0; }
    pos=m->import_rva; end=m->import_rva+m->import_size;
    while (pos+20<=end) {
        uint32_t oft=rd32(m->image+pos), name_rva=rd32(m->image+pos+12), ft=rd32(m->image+pos+16), ilt,iat;
        char dll[QRX_MAX_NAME]; QRXModule *dep;
        if (!(oft||name_rva||ft)) break;
        if (!cstr_image(m,name_rva,dll,sizeof(dll))) { snprintf(why,whycap,"bad import DLL name"); return 0; }
        qrx_lower(dll);
        dep=load_dependency(l,dll,depth+1);
        if (!dep || dep->state!=2) { snprintf(why,whycap,"dependency failed: %s",dll); l->unresolved++; return 0; }
        ilt=oft?oft:ft; iat=ft;
        while (1) {
            uint32_t v,addr=0; int ok=0;
            if (ilt+4>m->image_size || iat+4>m->image_size) { snprintf(why,whycap,"import thunk out of image"); return 0; }
            v=rd32(m->image+ilt); if (!v) break;
            m->imports_seen++; l->total_imports++;
            if (v&IMAGE_ORDINAL_FLAG32) {
                addr=resolve_export(l,dep,NULL,(uint16_t)(v&0xffffu),1,depth+1,&ok);
                if (!ok && l->verbose) printf("UNRESOLVED %s!#%u\n",dll,(unsigned)(v&0xffffu));
            } else {
                char fn[QRX_MAX_NAME]; uint32_t hn=v;
                if (hn+2>m->image_size || !cstr_image(m,hn+2,fn,sizeof(fn))) { snprintf(why,whycap,"bad import-by-name"); return 0; }
                addr=resolve_export(l,dep,fn,0,0,depth+1,&ok);
                if (!ok && l->verbose) printf("UNRESOLVED %s!%s\n",dll,fn);
            }
            if (!ok) { l->unresolved++; snprintf(why,whycap,"unresolved import in %s",m->name); return 0; }
            wr32(m->image+iat,addr); m->imports_resolved++; l->total_resolved++;
            ilt+=4; iat+=4;
        }
        pos+=20;
    }
    return 1;
}

static QRXModule *load_path(QRXLoader *l, const char *path, const char *logical_name, unsigned depth) {
    QRXModule *m; QRXPEMeta meta; char why[192]; uint32_t base;
    if (depth>QRX_MAX_DEPTH) { printf("QRXWinGuestLoader: dependency depth exceeded\n"); return NULL; }
    m=find_module(l,logical_name);
    if (m) { if (m->state==1) l->cycles++; return m; }
    if (l->count>=QRX_MAX_MODULES) { printf("QRXWinGuestLoader: module limit exceeded\n"); return NULL; }
    m=&l->modules[l->count++]; memset(m,0,sizeof(*m));
    snprintf(m->name,sizeof(m->name),"%s",logical_name); qrx_lower(m->name);
    snprintf(m->path,sizeof(m->path),"%s",path); m->state=1;
    m->file=read_whole(path,&m->file_size);
    if (!m->file) { printf("MISSING %s (%s)\n",m->name,path); m->state=3; l->missing_files++; return m; }
    if (!pe_meta(m->file,m->file_size,&meta,why,sizeof(why))) { printf("PE ERROR %s: %s\n",m->name,why); m->state=3; return m; }
    m->preferred_base=meta.image_base; m->entry_rva=meta.entry_rva; m->export_rva=meta.export_rva; m->export_size=meta.export_size;
    m->import_rva=meta.import_rva; m->import_size=meta.import_size; m->reloc_rva=meta.reloc_rva; m->reloc_size=meta.reloc_size;
    m->tls_rva=meta.tls_rva; m->tls_size=meta.tls_size; m->characteristics=meta.characteristics; m->subsystem=meta.subsystem;
    base=choose_guest_base(l,meta.image_base,meta.size_image);
    if (!base) { printf("PE ERROR %s: no guest address space\n",m->name); m->state=3; return m; }
    m->guest_base=base;
    if (!map_image(m,&meta,why,sizeof(why))) { printf("PE ERROR %s: %s\n",m->name,why); m->state=3; return m; }
    if (!apply_relocs(m,why,sizeof(why))) { printf("PE ERROR %s: %s\n",m->name,why); m->state=3; return m; }
    if (l->verbose) printf("MAP %-24s base=%08lx preferred=%08lx size=%lu entry=%08lx%s%s\n",m->name,(unsigned long)m->guest_base,(unsigned long)m->preferred_base,(unsigned long)m->image_size,(unsigned long)(m->guest_base+m->entry_rva),(m->characteristics&IMAGE_FILE_DLL)?" DLL":" EXE",m->tls_rva?" TLS":"");
    if (!process_imports(l,m,depth,why,sizeof(why))) { printf("IMPORT ERROR %s: %s\n",m->name,why); m->state=3; return m; }
    m->state=2; l->total_relocations+=m->relocations;
    return m;
}

static QRXModule *load_dependency(QRXLoader *l, const char *name, unsigned depth) {
    char n[QRX_MAX_NAME], path[QRX_MAX_PATH]; QRXModule *m;
    snprintf(n,sizeof(n),"%s",name); qrx_lower(n);
    m=find_module(l,n); if (m) { if (m->state==1) l->cycles++; return m; }
    if (strlen(l->system32)+1u+strlen(n)+1u>sizeof(path)) { printf("System32 path too long\n"); return NULL; }
    strcpy(path,l->system32); strcat(path,"/"); strcat(path,n);
    return load_path(l,path,n,depth);
}

static void free_loader(QRXLoader *l) {
    unsigned i; for (i=0;i<l->count;i++) { free(l->modules[i].file); free(l->modules[i].image); l->modules[i].file=NULL; l->modules[i].image=NULL; }
}

static void usage(void) {
    puts("QRXWinGuestLoader - QRX86 PE32/i386 ReactOS module loader foundation");
    puts("Usage: QRX:C/QRXWinGuestLoader [--quiet] [--system32 PATH] EXE");
    puts("Example: QRX:C/QRXWinGuestLoader QRX:Windows/ReactOS/System32/calc.exe");
    puts("Maps EXE/DLLs, applies HIGHLOW relocations, resolves imports/ordinals/forwarders.");
    puts("Execution and DllMain/TLS callbacks are intentionally the next QRX86 stage.");
}

/* 22.5.11: QRXLoader is large (tens to hundreds of KB); never place it on the
 * AmigaDOS process stack (4-64 KB, no MMU guard on m68k). Keep it in BSS. */
static QRXLoader l;
int main(int argc, char **argv) {
    const char *target=NULL; const char *base; char logical[QRX_MAX_NAME]; QRXModule *root; int i,rc=1;
    memset(&l,0,sizeof(l)); l.next_base=QRX_GUEST_FALLBACK_BASE; l.verbose=1; snprintf(l.system32,sizeof(l.system32),"%s",QRX_DEFAULT_SYSTEM32);
    for (i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--quiet")) l.verbose=0;
        else if (!strcmp(argv[i],"--system32") && i+1<argc) snprintf(l.system32,sizeof(l.system32),"%s",argv[++i]);
        else if (!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")) { usage(); return 0; }
        else if (argv[i][0]=='-' && argv[i][1]) { printf("Unknown option: %s\n",argv[i]); usage(); return 2; }
        else target=argv[i];
    }
    if (!target) { usage(); return 2; }
    base=qrx_basename(target); snprintf(logical,sizeof(logical),"%s",base); qrx_lower(logical);
    printf("QRXWinGuestLoader: target=%s system32=%s\n",target,l.system32);
    root=load_path(&l,target,logical,0);
    printf("QRXWinGuestLoader: modules=%u imports=%u resolved=%u unresolved=%u relocations=%u forwarders=%u cycles=%u missing-files=%u\n",l.count,l.total_imports,l.total_resolved,l.unresolved,l.total_relocations,l.total_forwarders,l.cycles,l.missing_files);
    if (root && root->state==2 && !l.unresolved && !l.missing_files) {
        printf("QRXWinGuestLoader: LINK PASS entry=%08lx dllmain/tls=NOT_EXECUTED execution=NOT_STARTED\n",(unsigned long)(root->guest_base+root->entry_rva));
        rc=0;
    } else puts("QRXWinGuestLoader: LINK FAIL");
    free_loader(&l); return rc;
}
