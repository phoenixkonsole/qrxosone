#define __aros_getbase_SysBase() SysBase
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>
struct ExecBase *SysBase;
__attribute__((section(".text.entry"))) int _start(void){
 SysBase=*(struct ExecBase * volatile *)4;
 if(!TypeOfMem((APTR)0x01000000)) AddMemList(16*1024*1024,MEMF_PUBLIC|MEMF_FAST,10,(APTR)0x01000000,"QRX host RAM");
 return 0;
}
