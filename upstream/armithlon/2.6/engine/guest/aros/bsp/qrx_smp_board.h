#ifndef QRX_SMP_BOARD_H
#define QRX_SMP_BOARD_H
/* QRX-native AROS BSP hooks. Not yet wired into Exec's SMP scheduler.
 * CPU0 comes up alone. CPU1 is unlocked ONLY after the kernel owns a
 * dedicated supervisor stack, IRQ entry point and shared task locks. */
#include "../../qrx68100_smp.h"
#include <stdint.h>
static inline int qrxb_smp_available(void){
    return qrx_smp_rd(0)==QRX_SMP_MAGIC && (qrx_smp_rd(8)&3u)==3u;
}
static inline int qrxb_smp_cpu_count(void){
    return qrxb_smp_available()?(qrx_smp_rd(0x0c)&3u)==3u?2:1:1;
}
static inline int qrxb_smp_start_ap(uint32_t pc,uint32_t stack){
    if(!qrxb_smp_available()||!(pc&~1u)||!(stack&~1u)||(pc&1u)||(stack&1u))return 0;
    qrx_smp_start_secondary(pc,stack);
    for(unsigned n=0;n<100000;n++)if((qrx_smp_rd(0x0c)&3u)==3u)return 1;
    return 0;
}
static inline void qrxb_smp_send_reschedule(unsigned cpu){if(cpu<2)qrx_smp_ipi(cpu);}
static inline void qrxb_smp_ack_reschedule(void){qrx_smp_ipi_ack();}
#endif
