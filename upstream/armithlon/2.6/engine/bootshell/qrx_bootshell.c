/* QRX68100 standalone 68k boot ROM and QRXFS-1 storage peripheral.
 * The command-line editing/loop is real M68000 machine code executed by QRX.
 * File operations are explicitly firmware device services (similar to a BIOS),
 * NOT evidence of native AmigaOS, AROS, Exec, DOS or a full 68040 implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "qrx_bootshell.h"
#include "qrx_hunk.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DISK_META 16u
#define ENTRY_BYTES 30u
#define MAX_NAME 21u
#define RX_MASK 255u

static void write_be32(unsigned char *p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static uint32_t read_be32(const unsigned char *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint32_t crc32(const unsigned char *p,size_t n){
    uint32_t c=0xffffffffu;
    for(size_t i=0;i<n;i++){c^=p[i];for(unsigned b=0;b<8;b++)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}
    return c^0xffffffffu;
}
static unsigned char *meta(QRXShell *s,unsigned i){return s->disk+DISK_META+i*ENTRY_BYTES;}
static unsigned char *payload(QRXShell *s,unsigned i){return s->disk+512u+i*QRX_DISK_SLOT_BYTES;}
static bool valid_name(const char *name){
    size_t n=strlen(name);
    if(!n||n>MAX_NAME||name[0]=='.'||strstr(name,".."))return false;
    for(size_t i=0;i<n;i++) if(!isalnum((unsigned char)name[i]) && name[i]!='.' && name[i]!='_' && name[i]!='-')return false;
    return true;
}
static int find(QRXShell *s,const char *name){
    for(unsigned i=0;i<QRX_DISK_SLOTS;i++)if(meta(s,i)[0] && !strcmp((const char*)meta(s,i),name))return (int)i;
    return -1;
}
static int empty_slot(QRXShell *s){for(unsigned i=0;i<QRX_DISK_SLOTS;i++)if(!meta(s,i)[0])return (int)i;return -1;}
static bool sync_image(QRXShell *s){
    if(!strcmp(s->disk_path,":memory:"))return true; /* explicit no-image volatile mode */
    if(strlen(s->disk_path)>sizeof(s->disk_path)-16)return false;
    char tmp[1040];snprintf(tmp,sizeof(tmp),"%s.tmp.XXXXXX",s->disk_path);
    int fd=mkstemp(tmp);if(fd<0)return false;
    (void)fchmod(fd,0600);
    size_t written=0;
    while(written<sizeof(s->disk)){
        ssize_t n=write(fd,s->disk+written,sizeof(s->disk)-written);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){close(fd);unlink(tmp);return false;}
        written+=(size_t)n;
    }
    bool ok=fsync(fd)==0;
    if(close(fd))ok=false;
    if(ok && rename(tmp,s->disk_path)==0)return true;
    unlink(tmp);return false;
}
static bool store_bytes(QRXShell *s,const char *name,const void *val,size_t n,bool append){
    if(!valid_name(name))return false;
    int idx=find(s,name);
    if(idx<0)idx=empty_slot(s);
    if(idx<0)return false;
    unsigned char snapshot[QRX_DISK_BYTES];memcpy(snapshot,s->disk,sizeof(snapshot));
    unsigned char *e=meta(s,(unsigned)idx),*b=payload(s,(unsigned)idx);
    size_t used=(e[0]&&append)?read_be32(e+22):0;
    if(used+n>QRX_DISK_SLOT_BYTES)return false;
    if(!append)memset(b,0,QRX_DISK_SLOT_BYTES);
    memcpy(b+used,val,n);
    memset(e,0,ENTRY_BYTES);memcpy(e,name,strlen(name));
    write_be32(e+22,(uint32_t)(used+n));write_be32(e+26,crc32(b,used+n));
    if(sync_image(s))return true;
    memcpy(s->disk,snapshot,sizeof(snapshot));return false;
}
static bool store(QRXShell *s,const char *name,const char *val,bool append){
    return store_bytes(s,name,val,strlen(val),append);
}
static bool erase(QRXShell *s,const char *name){
    int i=find(s,name);if(i<0)return false;
    unsigned char snapshot[QRX_DISK_BYTES];memcpy(snapshot,s->disk,sizeof(snapshot));
    memset(meta(s,(unsigned)i),0,ENTRY_BYTES);memset(payload(s,(unsigned)i),0,QRX_DISK_SLOT_BYTES);
    if(sync_image(s))return true;
    memcpy(s->disk,snapshot,sizeof(snapshot));return false;
}
static bool validate_disk(QRXShell *s){
    if(memcmp(s->disk,"QRXFS1\r\n",8)||read_be32(s->disk+8)!=1||
       read_be32(s->disk+12)!=QRX_DISK_SLOTS)return false;
    for(unsigned i=0;i<QRX_DISK_SLOTS;i++){
        const unsigned char *e=meta(s,i);
        if(!e[0]){for(unsigned j=0;j<ENTRY_BYTES;j++)if(e[j])return false;continue;}
        if(!memchr(e,0,22)||!valid_name((const char*)e))return false;
        unsigned n=read_be32(e+22);
        if(n>QRX_DISK_SLOT_BYTES||read_be32(e+26)!=crc32(payload(s,i),n))return false;
        for(unsigned j=0;j<i;j++)if(meta(s,j)[0]&&!strcmp((const char*)meta(s,j),(const char*)e))return false;
    }
    return true;
}
static void emit(QRXShell *s,const char *str){
    if(!s||!s->vm||!s->vm->console)return;
    for(;*str;str++)s->vm->console(s->vm->host_ctx,(uint8_t)*str);
}
static void result(QRXShell *s,const char *str){emit(s,str);emit(s,"\n");}
static void replyf(QRXShell *s,const char *prefix,uint64_t value,const char *suffix){
    char b[256];snprintf(b,sizeof(b),"%s%" PRIu64 "%s",prefix,value,suffix);result(s,b);
}
static int hexval(char c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
static bool hexstore(QRXShell *s,const char *name,const char *hex){
    size_t n=strlen(hex);if((n&1u)||n<2||n>248)return false;
    unsigned char bin[124];
    for(size_t i=0;i<n/2;i++){
        int h=hexval(hex[i*2]),l=hexval(hex[i*2+1]);
        if(h<0||l<0)return false;
        bin[i]=(unsigned char)(h*16+l);
    }
    return store_bytes(s,name,bin,n/2,false);
}
/* HostFS requests always cross the guest MMIO bus (not direct fopen calls).
 * Guest memory is a bounded DMA staging region, shared with real 68k clients. */
#define SHARE_NAME_ADDR 0x6000u
#define SHARE_DATA_ADDR 0x7000u
static bool share_req(QRXShell *s,unsigned op,const char *name,uint32_t ofs,
                      void *buffer,unsigned *len,uint32_t *filesize){
    if(!s->vm->hostfs){result(s,"ERR: no host directory mounted (--share)");return false;}
    if(name){
        size_t n=strlen(name);
        if(n==0||n>QRX_HOSTFS_MAX_NAME)return false;
        memcpy(s->vm->ram.data+SHARE_NAME_ADDR,name,n+1);
    }
    unsigned bytes=len?*len:0;
    if(bytes>QRX_HOSTFS_MAX_TRANSFER)return false;
    if(op==QRX_HOSTFS_CMD_WRITE && bytes && buffer)
        memcpy(s->vm->ram.data+SHARE_DATA_ADDR,buffer,bytes);
    bool ok=qrx_vm_write(s->vm,QRX_HOSTFS_NAME_DMA,4,SHARE_NAME_ADDR) &&
            qrx_vm_write(s->vm,QRX_HOSTFS_DATA_DMA,4,SHARE_DATA_ADDR) &&
            qrx_vm_write(s->vm,QRX_HOSTFS_OFFSET,4,ofs) &&
            qrx_vm_write(s->vm,QRX_HOSTFS_LENGTH,4,bytes) &&
            qrx_vm_write(s->vm,QRX_HOSTFS_COMMAND,4,op);
    uint32_t status=QRX_HOSTFS_INVALID,count=0,size=0;
    ok=ok&&qrx_vm_read(s->vm,QRX_HOSTFS_STATUS,4,&status)&&
           qrx_vm_read(s->vm,QRX_HOSTFS_RESULT,4,&count)&&
           qrx_vm_read(s->vm,QRX_HOSTFS_SIZE,4,&size);
    if(!ok||status!=QRX_HOSTFS_OK)return false;
    if(count>bytes)return false;
    if(len)*len=count;
    if(filesize)*filesize=size;
    if(buffer && (op==QRX_HOSTFS_CMD_LIST||op==QRX_HOSTFS_CMD_READ) && count)
        memcpy(buffer,s->vm->ram.data+SHARE_DATA_ADDR,count);
    return true;
}
static bool share_program(QRXShell *s,const char *name){
    unsigned len=4096;uint32_t fsz=0;
    unsigned char data[4096];
    if(!share_req(s,QRX_HOSTFS_CMD_STAT,name,0,NULL,NULL,&fsz)||fsz<8||fsz>4096){
        result(s,"ERR: host executable unavailable/too large");return false;
    }
    len=fsz;
    if(!share_req(s,QRX_HOSTFS_CMD_READ,name,0,data,&len,NULL)||len!=fsz){
        result(s,"ERR: host program read failed");return false;
    }
    if(len>=10 && !memcmp(data,"QEX1",4) && read_be32(data+4)==len-8 && !(len&1u)){
        memcpy(s->vm->ram.data+0x10000u,data+8,len-8);
        s->program_pending=true;s->program_bytes=len-8;
        result(s,"Launching host-mounted guest M68k executable (QEX1)...");
        return true;
    }
    uint32_t entry=0;size_t loaded=0;
    if(qrx_hunk_load(data,len,s->vm->ram.data,s->vm->ram.size,
                     QRX_HUNK_STAGE_ADDR,&entry,&loaded)){
        s->program_pending=true;s->program_bytes=(unsigned)loaded;
        result(s,"Launching host-mounted Amiga HUNK 68k code (no Exec/DOS ABI)...");
        return true;
    }
    result(s,"ERR: not a valid host QEX1 or bounded Amiga HUNK");return false;
}
static void share_command(QRXShell *s,const char *verb,char *arg){
    if(!s->vm->hostfs){result(s,"ERR: no host directory mounted (--share)");return;}
    if(!strcmp(verb,"hostinfo")){
        uint32_t flags=0;
        (void)qrx_vm_read(s->vm,QRX_HOSTFS_FEATURES,4,&flags);
        result(s,flags&2?"HOST: mounted read/write (single directory)":"HOST: mounted read-only (single directory)");return;
    }
    if(!strcmp(verb,"hostls")){
        unsigned n=4096;char out[4097]={0};
        if(!share_req(s,QRX_HOSTFS_CMD_LIST,NULL,0,out,&n,NULL))result(s,"ERR: host directory listing failed");
        else{out[n]=0;result(s,n?out:"(empty)");}return;
    }
    if(!arg||!*arg){result(s,"ERR: host command requires a filename");return;}
    if(!strcmp(verb,"hostrm")){
        result(s,share_req(s,QRX_HOSTFS_CMD_DELETE,arg,0,NULL,NULL,NULL)?"OK: host file deleted":"ERR: delete denied or failed");return;
    }
    if(!strcmp(verb,"hostcat")||!strcmp(verb,"hostget")){
        char *dest=arg?strpbrk(arg," \t"):NULL;
        if(dest){*dest++=0;while(*dest==' '||*dest=='\t')dest++;}
        uint32_t size=0;
        if(!share_req(s,QRX_HOSTFS_CMD_STAT,arg,0,NULL,NULL,&size)){
            result(s,"ERR: host file unavailable");return;
        }
        if(!strcmp(verb,"hostget")&&size>QRX_DISK_SLOT_BYTES){result(s,"ERR: QRXFS file limit 4096 bytes");return;}
        if(!strcmp(verb,"hostcat")&&size>16384u){result(s,"ERR: hostcat limited to 16 KiB");return;}
        unsigned char data[16384+1];uint32_t pos=0;
        while(pos<size){unsigned chunk=size-pos>4096?4096:size-pos;
            if(!share_req(s,QRX_HOSTFS_CMD_READ,arg,pos,data+pos,&chunk,NULL)||chunk==0){
                result(s,"ERR: host file read failed");return;
            }pos+=chunk;
        }
        if(!strcmp(verb,"hostget")){
            const char *target=(dest&&*dest)?dest:arg;
            result(s,store_bytes(s,target,data,pos,false)?"OK: host file copied to QRXFS-1":"ERR: unable to save QRXFS file");
        }else{
            if(s->vm->console)for(uint32_t i=0;i<pos;i++)s->vm->console(s->vm->host_ctx,data[i]);
            if(!pos||data[pos-1]!='\n')emit(s,"\n");
        }return;
    }
    if(!strcmp(verb,"hostput")){
        char *dest=strpbrk(arg," \t");
        if(dest){*dest++=0;while(*dest==' '||*dest=='\t')dest++;}
        const char *target=dest&&*dest?dest:arg;
        int slot=find(s,arg);if(slot<0){result(s,"ERR: QRXFS source not found");return;}
        unsigned size=read_be32(meta(s,(unsigned)slot)+22);
        if(size==0){result(s,"ERR: empty source not supported by this device");return;}
        unsigned chunk=size;
        bool ok=share_req(s,QRX_HOSTFS_CMD_WRITE,target,0,payload(s,(unsigned)slot),&chunk,NULL)&&chunk==size;
        if(ok)ok=share_req(s,QRX_HOSTFS_CMD_TRUNCATE,target,size,NULL,NULL,NULL);
        result(s,ok?"OK: QRXFS file copied to HOST:":"ERR: host share is read-only or write failed");
        return;
    }
}

static bool stage_program(QRXShell *s,const char *name){
    int i=find(s,name);
    if(i<0){result(s,"ERR: executable not found");return false;}
    unsigned n=read_be32(meta(s,(unsigned)i)+22);
    unsigned char *b=payload(s,(unsigned)i);
    if(n>=10 && n<=4096 && !memcmp(b,"QEX1",4) && read_be32(b+4)==n-8 &&
       !(n&1u) && read_be32(b+4)<=4000){
        memcpy(s->vm->ram.data+0x10000u,b+8,n-8);
        s->program_bytes=n-8;s->program_pending=true;
        result(s,"Launching guest M68k executable (QEX1)...");return true;
    }
    uint32_t entry=0;size_t loaded=0;
    if(qrx_hunk_load(b,n,s->vm->ram.data,s->vm->ram.size,
                     QRX_HUNK_STAGE_ADDR,&entry,&loaded)){
        s->program_bytes=(unsigned)loaded;s->program_pending=true;
        result(s,"Launching Amiga HUNK 68k machine code (no Exec/DOS ABI)...");return true;
    }
    result(s,"ERR: not a bounded QEX1 program or supported Amiga HUNK");return false;
}
static void command(QRXShell *s,char *line){
    char *p=line;
    while(*p==' '||*p=='\t')p++;
    size_t n=strlen(p);while(n && (p[n-1]==' '||p[n-1]=='\t'||p[n-1]=='\r'))p[--n]=0;
    if(!*p)return;
    char *arg=strpbrk(p," \t");
    if(arg){*arg++=0;while(*arg==' '||*arg=='\t')arg++;}
    if(!strcmp(p,"help")||!strcmp(p,"?")){
        result(s,"HELP: help ver cpu mem ls cat NAME write NAME TEXT append NAME TEXT rm NAME");
        result(s,"      stats uptime date disk echo TEXT hex NAME HEX run NAME bench reboot shutdown");
        result(s,"      hostinfo hostls hostcat FILE hostget FILE [DEST] hostput FILE [DEST] hostrm FILE");
        result(s,"      run HOST:PROGRAM.QEX or HOST:PROGRAM.HUNK (bounded 68k; no Exec ABI)");
    }else if(!strcmp(p,"ver"))result(s,"QRX OS One / QRX68100 firmware shell v2.1 (68000 diagnostic ISA subset)");
    else if(!strcmp(p,"cpu")){
        result(s,"QRX68100 virtual CPU: 68040-compatible design target, core 0 active");
        result(s,"Core 1 parked; JIT for selected 68k loops is separate and experimental.");
    }else if(!strcmp(p,"mem"))replyf(s,"Guest RAM bytes: ",(uint64_t)s->vm->ram.size,"");
    else if(!strcmp(p,"stats")){
        replyf(s,"Guest instructions retired: ",s->vm->steps,"");
        replyf(s,"Firmware commands executed: ",s->commands,"");
    }else if(!strcmp(p,"disk"))replyf(s,"QRXFS-1 disk bytes: ",sizeof(s->disk),"; 16 files / 4096 bytes each");
    else if(!strcmp(p,"ls")){
        bool any=false;
        for(unsigned i=0;i<QRX_DISK_SLOTS;i++)if(meta(s,i)[0]){
            char b[128];snprintf(b,sizeof(b),"%-22s %u bytes",meta(s,i),read_be32(meta(s,i)+22));result(s,b);any=true;
        }
        if(!any)result(s,"(empty)");
    }else if(!strcmp(p,"cat")){
        int i=arg?find(s,arg):-1;
        if(i<0)result(s,"ERR: file not found");
        else{
            unsigned nbytes=read_be32(meta(s,(unsigned)i)+22);
            if(s->vm->console)for(unsigned k=0;k<nbytes;k++)s->vm->console(s->vm->host_ctx,payload(s,(unsigned)i)[k]);
            if(!nbytes || payload(s,(unsigned)i)[nbytes-1]!='\n') emit(s,"\n");
        }
    }else if(!strcmp(p,"write")||!strcmp(p,"append")){
        char *text=arg?strpbrk(arg," \t"):NULL;
        if(!text){result(s,"ERR: usage: write|append NAME TEXT");return;}
        *text++=0;while(*text==' '||*text=='\t')text++;
        char buf[128];if(strlen(text)>sizeof(buf)-2){result(s,"ERR: line too long");return;}
        snprintf(buf,sizeof(buf),"%s\n",text);
        result(s,store(s,arg,buf,!strcmp(p,"append"))?"OK: saved to QRXFS-1":"ERR: invalid filename, full disk or save failed");
    }else if(!strcmp(p,"hex")){
        char *hex=arg?strpbrk(arg," \t"):NULL;
        if(!hex){result(s,"ERR: usage: hex NAME HEXBYTES");return;}
        *hex++=0;while(*hex==' '||*hex=='\t')hex++;
        result(s,hexstore(s,arg,hex)?"OK: binary written":"ERR: invalid hex or disk full");
    }else if(!strcmp(p,"hostinfo")||!strcmp(p,"hostls")||!strcmp(p,"hostcat")||
              !strcmp(p,"hostget")||!strcmp(p,"hostput")||!strcmp(p,"hostrm")){
        share_command(s,p,arg);
    }else if(!strcmp(p,"run")){
        if(!arg || !*arg)result(s,"ERR: usage run NAME");
        else if(!strncmp(arg,"HOST:",5))(void)share_program(s,arg+5);
        else (void)stage_program(s,arg);
    }else if(!strcmp(p,"bench")){
        (void)stage_program(s,"BENCH.QEX");
    }else if(!strcmp(p,"rm")){
        result(s,arg&&erase(s,arg)?"OK: removed":"ERR: file not found or save failed");
    }else if(!strcmp(p,"echo"))result(s,arg?arg:"");
    else if(!strcmp(p,"date")){
        time_t now=time(NULL);struct tm *tm=localtime(&now);char b[96];
        if(tm&&strftime(b,sizeof(b),"%Y-%m-%d %H:%M:%S local",tm))result(s,b);
        else result(s,"Clock unavailable");
    }else if(!strcmp(p,"uptime"))replyf(s,"Firmware commands since reset: ",s->commands,"");
    else if(!strcmp(p,"shutdown")||!strcmp(p,"exit")){
        result(s,"QRX68100 shutdown complete.");s->shutdown=true;s->vm->cpu.halted=1;
    }else if(!strcmp(p,"reboot")){
        result(s,"Restarting QRX firmware...");s->reboot=true;
    }else result(s,"ERR: unknown command. Type help.");
}

/* Build with explicit 68k opcode words; branch displacement is relative to PC+2. */
typedef struct{uint8_t *ram;size_t at,limit;} Code;
static bool w16(Code *c,uint16_t x){if(c->at+2>c->limit)return false;c->ram[c->at++]=(uint8_t)(x>>8);c->ram[c->at++]=(uint8_t)x;return true;}
static bool w32(Code *c,uint32_t x){return w16(c,(uint16_t)(x>>16))&&w16(c,(uint16_t)x);}
static bool lea(Code *c,unsigned a,uint32_t addr){return w16(c,(uint16_t)(0x41f9u|(a<<9)))&&w32(c,addr);}
static bool literal(Code *c,unsigned a,uint8_t x){return w16(c,(uint16_t)(0x10bcu|(a<<9)))&&w16(c,x);}
static bool patch_branch(Code *c,size_t at,size_t target){
    if(target>0xffff || at+4>c->limit)return false;
    int32_t delta=(int32_t)target-(int32_t)(at+2);
    if(delta < -32768 || delta >32767)return false;
    c->ram[at+2]=(uint8_t)((uint16_t)delta>>8);c->ram[at+3]=(uint8_t)delta;return true;
}
static bool branch(Code *c,uint16_t op,size_t *at){*at=c->at;return w16(c,op)&&w16(c,0);}
bool qrx_build_boot_shell(uint8_t *ram,size_t bytes){
    if(!ram||bytes<QRX_BOOT_RAM)return false;
    /* Sparse guest RAM: clear only the bootstrap page range. Never fault in
       gigabytes of untouched RAM during firmware construction. */
    memset(ram,0,QRX_BOOT_RAM);
    Code v={ram,0,QRX_BOOT_RAM};
    if(!w32(&v,(uint32_t)(QRX_BOOT_RAM-16u))||!w32(&v,QRX_BOOT_ENTRY))return false;
    v.at=QRX_BOOT_ENTRY;
    if(!lea(&v,0,QRX_BOOT_RX)||!lea(&v,1,QRX_BOOT_BUFFER)||
       !lea(&v,2,QRX_IO_BASE+0x0cu)||!lea(&v,3,QRX_BOOT_EXEC))return false;
    static const char banner[]="\nQRX OS One 2.1 | QRX68100 boot firmware\nStandalone 68k shell / QRXFS-1 disk or volatile + HOST: folder\nType help for commands.\n";
    for(size_t i=0;banner[i];i++)if(!literal(&v,2,(unsigned char)banner[i]))return false;
    size_t prompt=v.at;
    if(!literal(&v,2,'q')||!literal(&v,2,'r')||!literal(&v,2,'x')||
       !literal(&v,2,'>')||!literal(&v,2,' '))return false;
    size_t loop=v.at,beq_idle,beq_command,bra_loop,bra_prompt;
    if(!w16(&v,0x1010u)|| /* MOVE.B (A0),D0: UART RX */
       !w16(&v,0x4a00u)|| /* TST.B D0 */
       !branch(&v,0x6700u,&beq_idle)||
       !w16(&v,0x0c00u)||!w16(&v,10u)|| /* CMPI.B #LF,D0 */
       !branch(&v,0x6700u,&beq_command)||
       !w16(&v,0x12c0u)|| /* MOVE.B D0,(A1)+ */
       !branch(&v,0x6000u,&bra_loop))return false;
    size_t process=v.at;
    if(!literal(&v,1,0)|| /* MOVE.B #0,(A1) */
       !w16(&v,0x26bcu)||!w32(&v,QRX_BOOT_BUFFER)|| /* MOVE.L #buffer,(A3) */
       !lea(&v,1,QRX_BOOT_BUFFER)||
       !branch(&v,0x6000u,&bra_prompt))return false;
    return patch_branch(&v,beq_idle,loop)&&patch_branch(&v,beq_command,process)&&
           patch_branch(&v,bra_loop,loop)&&patch_branch(&v,bra_prompt,prompt);
}

bool qrx_shell_mount(QRXShell *s,QRXVM *vm,const char *path){
    if(!s||!vm||!path||!path[0]||strlen(path)>=sizeof(s->disk_path))return false;
    memset(s,0,sizeof(*s));s->vm=vm;strcpy(s->disk_path,path);
    FILE *f=!strcmp(path,":memory:")?NULL:fopen(path,"rb");
    if(f){
        if(fread(s->disk,1,sizeof(s->disk),f)!=sizeof(s->disk)||fgetc(f)!=EOF||ferror(f)){
            fclose(f);return false;
        }
        fclose(f);
        if(!validate_disk(s))return false;
    }else{
        if(strcmp(path,":memory:") && errno!=ENOENT)return false;
        memcpy(s->disk,"QRXFS1\r\n",8);
        write_be32(s->disk+8,1);write_be32(s->disk+12,QRX_DISK_SLOTS);
        if(!store(s,"README.TXT","Welcome to QRX OS One standalone firmware v1.8.\nUse help, ls, cat, write, append, hex, run and rm.\n",false))return false;
        /* Actual 68000 code, starting at the QEX1 entry point, using UART and RTS. */
        static const unsigned char hello_code[]={
            'Q','E','X','1', 0,0,0,0x30,
            0x49,0xf9,0x00,0xff,0x00,0x0c, /* LEA UART,A4 */
            0x18,0xbc,0, 'H',0x18,0xbc,0,'e',0x18,0xbc,0,'l',0x18,0xbc,0,'l',
            0x18,0xbc,0,'o',0x18,0xbc,0,' ',0x18,0xbc,0,'Q',0x18,0xbc,0,'R',
            0x18,0xbc,0,'X',0x18,0xbc,0,10,0x4e,0x75
        };
        if(!store_bytes(s,"HELLO.QEX",hello_code,sizeof(hello_code),false))return false;
        /* Guest timing workload: 5,000 SUBQ/BNE iterations and real RTS. */
        static const unsigned char bench_code[]={
            'Q','E','X','1',0,0,0,12,
            0x20,0x3c,0x00,0x00,0x13,0x88,0x53,0x80,0x66,0xfc,0x4e,0x75
        };
        if(!store_bytes(s,"BENCH.QEX",bench_code,sizeof(bench_code),false))return false;
    }
    vm->system=s;return true;
}
void qrx_shell_detach(QRXShell *s){if(s&&s->vm&&s->vm->system==s)s->vm->system=NULL;}
bool qrx_shell_enqueue(QRXShell *s,const char *line){
    if(!s||!line||s->shutdown)return false;
    size_t n=strlen(line);
    if(n>125)return false;
    if(n && line[n-1]=='\n')n--;
    if(n>124 || (s->input_head-s->input_tail)+n+1>sizeof(s->input))return false;
    for(size_t i=0;i<n;i++)s->input[s->input_head++&RX_MASK]=(unsigned char)line[i];
    s->input[s->input_head++&RX_MASK]='\n';return true;
}
bool qrx_shell_dev_read(QRXVM *v,uint32_t addr,unsigned width,uint32_t *out){
    QRXShell *s=v?v->system:NULL;
    if(!s||!out)return false;
    if(addr==QRX_BOOT_RX && width==1){*out=s->input_head==s->input_tail?0:s->input[s->input_tail++&RX_MASK];return true;}
    if(addr==QRX_BOOT_RX_STATUS && width==4){*out=s->input_head-s->input_tail;return true;}
    return false;
}
bool qrx_shell_dev_write(QRXVM *v,uint32_t addr,unsigned width,uint32_t val){
    QRXShell *s=v?v->system:NULL;
    if(!s||addr!=QRX_BOOT_EXEC||width!=4)return false;
    if(val < 0x1000u || (uint64_t)val+128u > v->ram.size){result(s,"ERR: invalid guest command pointer");s->commands++;return true;}
    char command_buffer[129];size_t i;
    for(i=0;i<128;i++){
        command_buffer[i]=(char)v->ram.data[val+i];
        if(!command_buffer[i])break;
    }
    if(i==128){result(s,"ERR: unterminated guest command");s->commands++;return true;}
    s->commands++;
    command(s,command_buffer);
    return true;
}
bool qrx_shell_run_line(QRXShell *s,const char *line,uint64_t max_steps){
    if(!s||!s->vm||max_steps<100||!qrx_shell_enqueue(s,line))return false;
    uint64_t before=s->commands;
    uint64_t total=0;
    while(total<max_steps && !s->shutdown){
        uint64_t slice=max_steps-total>256?256:max_steps-total;
        int r=s->run_cycles?s->run_cycles(s->cpu_opaque,slice):qrx_vm_run(s->vm,slice);
        total+=slice;
        if(r<0)return false;
        if(s->commands>before){
            if(s->program_pending){
                s->program_pending=false;
                /* Guest-visible supervisor stack trampoline: 68k RTS to firmware. */
                if(!s->run_cycles && s->vm->cpu.a[7]<4){result(s,"ERR: guest stack underflow");return false;}
                if(s->run_subroutine){
                    uint64_t retired=0;
                    /* Guest stack trampoline and CPU state owned by Musashi. */
                    if(!s->run_subroutine(s->cpu_opaque,0x10000u,200000u,&retired)){
                        result(s,"ERR: Musashi guest app fault/timeout");return false;
                    }
                    if(retired>100){
                        char msg[140];
                        snprintf(msg,sizeof(msg),"Musashi guest app retired %llu instructions",
                                 (unsigned long long)retired);
                        result(s,msg);
                    }
                }else
                {
                QRXVM *vm=s->vm;
                uint32_t return_pc=vm->cpu.pc, sp=vm->cpu.a[7]-4u;
                if(!qrx_vm_write(vm,sp,4,return_pc))return false;
                vm->cpu.a[7]=sp;vm->cpu.ssp=sp;vm->cpu.pc=0x10000u;
                bool returned=false;
                unsigned executed=0;
                struct timespec begin={0},end={0};
                (void)clock_gettime(CLOCK_MONOTONIC,&begin);
                for(unsigned k=0;k<20000;k++){
                    executed++;
                    if(!qrx_vm_step(vm) || vm->cpu.halted)break;
                    if(vm->cpu.pc==return_pc){returned=true;break;}
                }
                if(!returned){result(s,"ERR: guest app trapped, halted or timed out");return false;}
                (void)clock_gettime(CLOCK_MONOTONIC,&end);
                if(executed>100){
                    double secs=(end.tv_sec-begin.tv_sec)+(end.tv_nsec-begin.tv_nsec)*1e-9;
                    char msg[160];
                    if(secs>0)snprintf(msg,sizeof(msg),"Guest app: %u retired instructions, %.3f ms, %.0f instr/s (host interpreter)",executed,secs*1000,(double)executed/secs);
                    else snprintf(msg,sizeof(msg),"Guest app: %u retired instructions; elapsed below clock resolution",executed);
                    result(s,msg);
                }
                }
            }
            /* Allow the guest to execute the post-command prompt after BIOS service. */
            if(!s->shutdown && !s->reboot && (s->run_cycles?
                s->run_cycles(s->cpu_opaque,64):qrx_vm_run(s->vm,64))<0)return false;
            return true;
        }
    }
    return s->shutdown && s->commands>before;
}
