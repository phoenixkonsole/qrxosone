/* Read-only BSP probe: never enables second core behind AROS Exec's back. */
#include <proto/dos.h>
#include "qrx_smp_board.h"
int main(void){
    if(!qrxb_smp_available()){Printf("QRX68100 SMP controller not available\n");return 20;}
    Printf("QRX68100: dual-68040 capability, active cores %ld\n",(long)qrxb_smp_cpu_count());
    Printf("SMP Exec scheduling requires separate AROS m68k integration.\n");
    return 0;
}
