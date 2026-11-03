/* Diagnostic native Win32 surface producer: transport test, not a Windows app. */
#include "qrx86_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
int main(int argc,char **argv){
    const char *path=argc>1?argv[1]:getenv("ARMITHLON_QRX86_SOCKET");
    if(!path||!*path){fprintf(stderr,"Set ARMITHLON_QRX86_SOCKET or pass path\n");return 2;}
    enum {W=640,H=360};
    uint32_t *buf=malloc(W*H*sizeof(*buf));if(!buf)return 2;
    unsigned limit=180;
    const char *env=getenv("QRX86_DEMO_FRAMES");
    if(env&&*env){long n=strtol(env,NULL,10);if(n>0&&n<=180)limit=(unsigned)n;}
    unsigned sent=0;
    for(unsigned frame=0;frame<limit;frame++){
        for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++) {
            unsigned r=(x+frame*3)%256,g=(y+frame)%256,b=((x^y)+frame*2)%256;
            if(x>180+frame/3 && x<400+frame/3 && y>90 && y<270){r=220;g=40;b=120;}
            buf[y*W+x]=(r<<16)|(g<<8)|b;
        }
        if(qrx86_ipc_send(path,buf,W,H))sent++;
        else {fprintf(stderr,"QRX86 GPU socket unavailable: %s\n",path);break;}
        struct timespec t={0,33000000};nanosleep(&t,NULL);
    }
    free(buf);
    printf("QRX86 native host GPU diagnostic frames transmitted: %u\n",sent);
    return sent?0:1;
}
