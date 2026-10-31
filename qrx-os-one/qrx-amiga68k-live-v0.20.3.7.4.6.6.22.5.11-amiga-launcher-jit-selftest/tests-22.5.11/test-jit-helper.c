/* Portable helper regression. Does not unlock or execute legacy native code. */
#define QRX_TEST_NATIVE 1
#define main qrx_executor_main
#include "../diagnostics-v6.6/tools/qrx86_grt_exec.c"
#undef main
#include <assert.h>
int main(void)
{
    static QRXVM vm;QRXCPU cpu;QRXBlock block;
    uint8_t bytes[]={0x90,0x90,0x90,0x90};
    memset(&vm,0,sizeof vm);memset(&cpu,0,sizeof cpu);memset(&block,0,sizeof block);
    assert(region_add(&vm,0x1000,sizeof bytes,bytes,0,1,"TEST"));
    block.count=4;for(unsigned i=0;i<4;i++)block.insn_eip[i]=0x1000+i;
    g_jit_vm=&vm;g_jit_cpu=&cpu;g_jit_block=&block;g_jit_index=0;
    cpu.eip=0x1000;vm.step_deadline=2;
    for(unsigned i=0;i<4;i++)jit_thread_helper();
    assert(vm.steps==2&&cpu.fault==5&&cpu.eip==0x1002);
    puts("PASS dormant native helper charges exact shared watchdog budget");
    cpu.fault=0;cpu.eip=0x1000;vm.steps=0;vm.step_deadline=20;g_jit_index=0;
    bytes[0]=0xeb;bytes[1]=0;block.count=3;block.insn_eip[1]=0x1002;block.insn_eip[2]=0x1003;
    jit_thread_helper();jit_thread_helper();jit_thread_helper();
    assert(vm.steps==1&&cpu.eip==0x1002&&g_jit_index==block.count);
    puts("PASS terminal jump stops remaining threaded helper calls");
    return 0;
}
