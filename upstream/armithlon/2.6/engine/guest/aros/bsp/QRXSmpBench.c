/* Runs one job on each 68100 core at once and prints the time, to show that core 1 is a
 * second processor and not a time slice. AROS itself keeps running on core 0 only. */
#include <proto/dos.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include "../../qrx68100_smp.h"

#define JOB_LOOPS 400000000u
#define STACK_BYTES 8192u

/* What a core 1 job may touch: RAM and the SMP registers, no library calls. */
static volatile uint32_t job_result[2];
static void crunch(unsigned which)
{
    uint32_t a = which + 1u, n = JOB_LOOPS;
    while (n--) a = a * 5u + 1u;
    job_result[which] = a;
}
static void core1_job(void)
{
    crunch(1);
    qrx_smp_wr(0x18, 2);                /* park this core */
    for (;;) {}
}
static LONG ticks_since(const struct DateStamp *t0)
{
    struct DateStamp t1;
    DateStamp(&t1);
    return (t1.ds_Days - t0->ds_Days) * 1440L * 3000L + (t1.ds_Minute - t0->ds_Minute) * 3000L + (t1.ds_Tick - t0->ds_Tick);
}
int main(void)
{
    struct DateStamp t0;
    LONG one, both;
    UBYTE *stack;
    if (qrx_smp_rd(0) != QRX_SMP_MAGIC || !(qrx_smp_rd(8) & (1u << 4))) {
        Printf("No parallel 68100 core 1 on this machine (magic %08lx, features %08lx)\n", (unsigned long)qrx_smp_rd(0), (unsigned long)qrx_smp_rd(8));
        return 20;
    }
    stack = AllocMem(STACK_BYTES, MEMF_PUBLIC);
    if (!stack) return 20;
    DateStamp(&t0);
    crunch(0);
    one = ticks_since(&t0);
    DateStamp(&t0);
    qrx_smp_start_secondary((uint32_t)core1_job, (uint32_t)stack + STACK_BYTES);
    crunch(0);
    while (qrx_smp_rd(0x0C) & 2u) Delay(1);
    both = ticks_since(&t0);
    if (qrx_smp_rd(0x0C) & 4u) Printf("core 1 faulted at %08lx, vector or address %08lx\n", (unsigned long)qrx_smp_rd(0x38), (unsigned long)qrx_smp_rd(0x3C));
    Printf("QRXSmpBench: one job %ld ticks, two jobs in parallel %ld ticks (of 50/s)\n", one, both);
    Printf("core 0 result %08lx, core 1 result %08lx, core 1 instructions %lu\n",
           (unsigned long)job_result[0], (unsigned long)job_result[1], (unsigned long)qrx_smp_rd(0x28));
    FreeMem(stack, STACK_BYTES);
    return job_result[1] ? 0 : 10;
}
