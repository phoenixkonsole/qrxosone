/* QRXShare: small *actual AROS 68k user-mode bridge to QRXHostFS hardware.
 * QRXShare list | get HOSTFILE DEST | put SOURCE HOSTFILE | rm HOSTFILE
 * Once a real DOS packet handler exists, this driver can become HOST: volume.
 * WARNING: requires QRX virtual board. Not safe to launch on physical Amigas.
 */
#include <proto/dos.h>
#include <dos/dos.h>
#include <exec/types.h>
#include "qrxhostfs_guest.h"
#include <string.h>
#define CHUNK 4096u
static unsigned char buf[CHUNK];
static char filename[97];
static int copy_name(const char *s){
    size_t n;
    if(!s)return 0;
    n=strlen(s);
    if(n==0||n>96 || s[0]=='.')return 0;
    for(size_t i=0;i<n;i++){
        unsigned char c=(unsigned char)s[i];
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c==' '))return 0;
    }
    memcpy(filename,s,n+1);return 1;
}
static int get_file(const char *src,const char *dest){
    if(!copy_name(src))return 20;
    if(qrxf_request(QRXV_HFS_STAT,filename,buf,0,0))return 20;
    uint32_t remaining=qrxf_rd(QRXV_HFS_SIZE),offset=0;
    BPTR file=Open((STRPTR)dest,MODE_NEWFILE);
    if(!file)return 20;
    int rc=0;
    while(remaining){
        uint32_t amount=remaining>CHUNK?CHUNK:remaining;
        if(qrxf_request(QRXV_HFS_READ,filename,buf,offset,amount)) {rc=20;break;}
        uint32_t got=qrxf_rd(QRXV_HFS_RESULT);
        if(got==0||got>amount||Write(file,buf,got)!=(LONG)got){rc=20;break;}
        offset+=got;remaining-=got;
    }
    Close(file);
    if(rc)DeleteFile((STRPTR)dest);
    return rc;
}
static int put_file(const char *src,const char *dest){
    if(!copy_name(dest)||!(qrxf_rd(QRXV_HFS_FEATURES)&2u))return 20;
    BPTR file=Open((STRPTR)src,MODE_OLDFILE);
    if(!file)return 20;
    uint32_t offset=0;int rc=0;
    for(;;){
        LONG n=Read(file,buf,CHUNK);
        if(n<0){rc=20;break;}
        if(!n)break;
        if(offset>0x04000000u-(uint32_t)n ||
           qrxf_request(QRXV_HFS_WRITE,filename,buf,offset,(uint32_t)n) ||
           qrxf_rd(QRXV_HFS_RESULT)!=(uint32_t)n){rc=20;break;}
        offset+=(uint32_t)n;
    }
    Close(file);
    if(!rc&&qrxf_request(QRXV_HFS_TRUNCATE,filename,buf,offset,0))rc=20;
    return rc;
}
int main(int argc,char **argv){
    if(argc<2){Printf("QRXShare LIST | GET HOSTNAME AMIGAPATH | PUT AMIGAPATH HOSTNAME | RM HOSTNAME\n");return 5;}
    /* Driver only works on QRX virtual hardware; do not probe real Amigas. */
    if(qrxf_rd(0)!=QRXV_HFS_MAGIC){Printf("QRX host-share device unavailable\n");return 20;}
    if(!strcmp(argv[1],"list")&&argc==2){
        if(qrxf_request(QRXV_HFS_LIST,filename,buf,0,CHUNK))return 20;
        uint32_t n=qrxf_rd(QRXV_HFS_RESULT);
        if(n>CHUNK)return 20;
        return Write(Output(),buf,n)==(LONG)n?0:20;
    }
    if(!strcmp(argv[1],"get")&&argc==4)return get_file(argv[2],argv[3]);
    if(!strcmp(argv[1],"put")&&argc==4)return put_file(argv[2],argv[3]);
    if(!strcmp(argv[1],"rm")&&argc==3){
        if(!copy_name(argv[2]))return 20;
        return qrxf_request(QRXV_HFS_DELETE,filename,buf,0,0)?20:0;
    }
    Printf("Invalid arguments. Try QRXShare with no parameters.\n");return 5;
}
