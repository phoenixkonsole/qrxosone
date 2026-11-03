#include "qrx_app.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void writebuf(const char *p,const unsigned char *b,size_t n){FILE *f=fopen(p,"wb");assert(f);assert(fwrite(b,1,n,f)==n);assert(fclose(f)==0);}
int main(void){
    char p[]="/tmp/qrx-router-XXXXXX";int fd=mkstemp(p);assert(fd>=0);close(fd);
    unsigned char b[256]={0};
    memcpy(b,"MZ",2);b[0x3c]=0x80;
    memcpy(b+0x80,"PE\0\0",4);b[0x84]=0x4c;b[0x85]=1;b[0x98]=0x0b;b[0x99]=1;
    writebuf(p,b,sizeof(b));assert(qrx_app_inspect(p)==QRX_APP_PE32_I386);
    b[0x84]=0x64;b[0x85]=0x86;writebuf(p,b,sizeof(b));assert(qrx_app_inspect(p)==QRX_APP_UNSUPPORTED);
    b[0x3c]=0xff;b[0x3d]=0xff;b[0x3e]=0xff;b[0x3f]=0xff;
    writebuf(p,b,sizeof(b));assert(qrx_app_inspect(p)==QRX_APP_INVALID);
    memset(b,0,sizeof(b));memcpy(b,"\177ELF",4);b[4]=1;b[5]=1;b[6]=1;b[16]=2;b[18]=3;
    writebuf(p,b,52);assert(qrx_app_inspect(p)==QRX_APP_ELF32_I386);
    b[5]=2;b[16]=0;b[17]=2;b[18]=0;b[19]=4;writebuf(p,b,52);assert(qrx_app_inspect(p)==QRX_APP_ELF68K);
    memset(b,0,sizeof(b));memcpy(b,"QEX1",4);b[7]=2;b[8]=0x4e;b[9]=0x75;
    writebuf(p,b,10);assert(qrx_app_inspect(p)==QRX_APP_QEX1);
    writebuf(p,b,9);assert(qrx_app_inspect(p)==QRX_APP_INVALID);
    memset(b,0,sizeof(b));b[2]=3;b[3]=0xf3;writebuf(p,b,8);assert(qrx_app_inspect(p)==QRX_APP_HUNK);
    const char *args[5];
    assert(qrx_app_plan(QRX_APP_PE32_I386,"/bin/qrxwin",NULL,p,args,5));
    assert(!strcmp(args[0],"/bin/qrxwin")&&!strcmp(args[1],p)&&!args[2]);
    assert(!qrx_app_plan(QRX_APP_ELF68K,"/bin/qrxwin","/bin/qrxlinux",p,args,5));
    assert(!qrx_app_plan(QRX_APP_ELF32_I386,"/bin/qrxwin",NULL,p,args,5));
    assert(qrx_app_plan(QRX_APP_ELF32_I386,NULL,"/bin/qrxlinux",p,args,5));
    assert(!strcmp(args[1],"-e")&&!strcmp(args[2],"DISPLAY=:0")&&
           !strcmp(args[3],p)&&args[4]==NULL);
    unlink(p);puts("PASS: strict PE/ELF/HUNK/QEX parser, no shell-based invocation");return 0;
}
