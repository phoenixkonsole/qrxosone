#include "qrx_app.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static uint16_t le16(const unsigned char *p){return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static uint32_t le32(const unsigned char *p){return (uint32_t)le16(p)|((uint32_t)le16(p+2)<<16);}
static uint32_t be32(const unsigned char *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
QRXAppKind qrx_app_inspect(const char *path){
    unsigned char head[4096]={0};
    if(!path || !*path)return QRX_APP_INVALID;
    struct stat st;
    if(stat(path,&st)!=0 || !S_ISREG(st.st_mode)||st.st_size<4)return QRX_APP_INVALID;
    FILE *f=fopen(path,"rb");if(!f)return QRX_APP_INVALID;
    size_t got=fread(head,1,sizeof(head),f);
    fclose(f);
    if(got<4)return QRX_APP_INVALID;
    if(got>=8 && !memcmp(head,"QEX1",4)) {
        uint32_t sz=be32(head+4);
        return sz>=2 && sz<=4000 && (sz&1u)==0 && (uint64_t)sz+8==(uint64_t)st.st_size ? QRX_APP_QEX1:QRX_APP_INVALID;
    }
    if(got>=64 && head[0]=='M' && head[1]=='Z'){
        uint32_t off=le32(head+0x3c);
        if(off>4096u-26u || off+26>got || off+26>(uint64_t)st.st_size)return QRX_APP_INVALID;
        const unsigned char *p=head+off;
        if(memcmp(p,"PE\0\0",4))return QRX_APP_INVALID;
        return le16(p+4)==0x14c && le16(p+24)==0x10b ? QRX_APP_PE32_I386:QRX_APP_UNSUPPORTED;
    }
    if(got>=52 && !memcmp(head,"\177ELF",4)){
        if(head[4]!=1||head[6]!=1||(head[5]!=1&&head[5]!=2))return QRX_APP_UNSUPPORTED;
        int little=head[5]==1;
        unsigned m=little?le16(head+18):((unsigned)head[18]<<8)|head[19];
        unsigned t=little?le16(head+16):((unsigned)head[16]<<8)|head[17];
        if(t!=2&&t!=3)return QRX_APP_UNSUPPORTED;
        if(m==3&&little)return QRX_APP_ELF32_I386;
        if(m==4&&!little)return QRX_APP_ELF68K;
        return QRX_APP_UNSUPPORTED;
    }
    if(be32(head)==0x3f3u || be32(head)==0x3e7u || be32(head)==0x3e9u)
        return QRX_APP_HUNK;
    return QRX_APP_UNSUPPORTED;
}
const char *qrx_app_name(QRXAppKind kind){
    switch(kind){
        case QRX_APP_PE32_I386:return "Windows PE32/i386";
        case QRX_APP_ELF32_I386:return "Linux ELF32/i386";
        case QRX_APP_ELF68K:return "Motorola 68k ELF";
        case QRX_APP_QEX1:return "QRX 68k QEX1";
        case QRX_APP_HUNK:return "Amiga HUNK";
        case QRX_APP_INVALID:return "invalid/unreadable";
        default:return "unsupported format";
    }
}
int qrx_app_plan(QRXAppKind kind,const char *win_runtime,const char *linux_runtime,
                 const char *guest_path,const char **out,size_t capacity){
    if(!out||capacity<(kind==QRX_APP_ELF32_I386?5u:3u)||!guest_path||!*guest_path)return 0;
    const char *runtime=NULL;
    if(kind==QRX_APP_PE32_I386)runtime=win_runtime;
    else if(kind==QRX_APP_ELF32_I386)runtime=linux_runtime;
    if(!runtime||!*runtime)return 0;
    out[0]=runtime;
    if(kind==QRX_APP_ELF32_I386){
        out[1]="-e";out[2]="DISPLAY=:0";out[3]=guest_path;out[4]=NULL;
    }else {out[1]=guest_path;out[2]=NULL;}
    return 1;
}
