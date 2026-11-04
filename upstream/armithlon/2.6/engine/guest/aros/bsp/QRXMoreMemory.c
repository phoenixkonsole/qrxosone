/* Hands the board RAM above 32 MiB to exec. Runs once from S:Startup-Sequence;
 * the boot-time QRXHostMemory only covers the first 16 MiB of fast RAM.
 * Anything exec already owns (a probe may have found some) is skipped. */
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>
#define BOARD_RAM_SIZE (*(volatile ULONG*)0xFF000888u)
#define START 0x02000000u
#define STEP  0x00100000u
int main(void){
    ULONG top=BOARD_RAM_SIZE,run=0,a;
    for(a=START;a<top;a+=STEP){
        if(TypeOfMem((APTR)(a+64))){
            if(run<a)AddMemList(a-run,MEMF_PUBLIC|MEMF_FAST,0,(APTR)run,(STRPTR)"QRX board RAM");
            run=0;
        }else if(!run)run=a;
    }
    if(run&&run<top)AddMemList(top-run,MEMF_PUBLIC|MEMF_FAST,0,(APTR)run,(STRPTR)"QRX board RAM");
    return 0;
}
