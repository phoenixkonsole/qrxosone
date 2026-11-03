/* One-second 440 Hz tone through ahi.device on the QRX host driver, then a
 * verdict on the serial debug line. For checks without someone listening. */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/ahi.h>
#include <proto/exec.h>
#include <proto/dos.h>
#define MODE 0x00510001u
#define RATE 48000u
static void say(const char *s){while(*s)*(volatile UWORD*)0xdff030=0x100|*s++;}
int main(void){
    int ok=0;
    struct MsgPort *port=CreateMsgPort();
    struct AHIRequest *req=port?(struct AHIRequest*)CreateIORequest(port,sizeof(struct AHIRequest)):NULL;
    WORD *pcm=(WORD*)AllocVec(RATE*2u,MEMF_PUBLIC);
    if(req&&pcm){
        req->ahir_Version=4;
        LONG err=OpenDevice((CONST_STRPTR)AHINAME,AHI_DEFAULT_UNIT,(struct IORequest*)req,0);
        if(err==0){
            /* A triangle at 440 Hz: an exact period, no FPU in the guest. */
            for(ULONG i=0;i<RATE;i++){
                ULONG ph=(i*440u)%RATE*65536u/RATE;
                LONG v=ph<32768u?(LONG)ph-16384:49152-(LONG)ph;
                pcm[i]=(WORD)v;
            }
            req->ahir_Std.io_Command=CMD_WRITE;
            req->ahir_Std.io_Data=pcm;
            req->ahir_Std.io_Length=RATE*2u;
            req->ahir_Std.io_Offset=0;
            req->ahir_Type=AHIST_M16S;
            req->ahir_Frequency=RATE;
            req->ahir_Volume=0x10000;
            req->ahir_Position=0x8000;
            req->ahir_Link=NULL;
            err=DoIO((struct IORequest*)req);
            ok=err==0;
            CloseDevice((struct IORequest*)req);
        }else say("[AHITEST] OpenDevice failed\n");
    }
    if(req)DeleteIORequest((struct IORequest*)req);
    if(port)DeleteMsgPort(port);
    if(pcm)FreeVec(pcm);
    say(ok?"[AHITEST] PLAYED\n":"[AHITEST] FAILED\n");
    return ok?0:20;
}
