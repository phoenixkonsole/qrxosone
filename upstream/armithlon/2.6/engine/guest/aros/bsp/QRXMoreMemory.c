/* Hands the board RAM above 32 MiB to exec. Runs once from S:Startup-Sequence;
 * the boot-time QRXHostMemory only covers the first 16 MiB of fast RAM.
 * Anything exec already owns (a probe may have found some) is skipped. */
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>
#define BOARD_RAM_SIZE (*(volatile ULONG*)0xFF000888u)
#define START 0x02000000u
#define STEP  0x00100000u
/* Above the boot-time memory, which holds 16 MiB at most. Used last, that fills up and fragments
 * until an allocation falls into AROS's low memory handling, and qrx-win32 hung there. */
#define QRX_HIGH_RAM_PRI 60
int main(void){
    ULONG top=BOARD_RAM_SIZE,run=0,a;
    for(a=START;a<top;a+=STEP){
        if(TypeOfMem((APTR)(a+64))){
            if(run<a)AddMemList(a-run,MEMF_PUBLIC|MEMF_FAST,QRX_HIGH_RAM_PRI,(APTR)run,(STRPTR)"QRX board RAM");
            run=0;
        }else if(!run)run=a;
    }
    if(run&&run<top)AddMemList(top-run,MEMF_PUBLIC|MEMF_FAST,QRX_HIGH_RAM_PRI,(APTR)run,(STRPTR)"QRX board RAM");
    /* The board RAM may be in the list already, behind the boot memory: put it in front. */
    {
        struct MemHeader *mh,*next;
        Forbid();
        for(mh=(struct MemHeader*)SysBase->MemList.lh_Head;(next=(struct MemHeader*)mh->mh_Node.ln_Succ)!=NULL;mh=next){
            if((ULONG)mh->mh_Lower>=START&&mh->mh_Node.ln_Pri<QRX_HIGH_RAM_PRI){
                Remove(&mh->mh_Node);
                mh->mh_Node.ln_Pri=QRX_HIGH_RAM_PRI;
                Enqueue(&SysBase->MemList,&mh->mh_Node);
                break;
            }
        }
        Permit();
    }
    return 0;
}
