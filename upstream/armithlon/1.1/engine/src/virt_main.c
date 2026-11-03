#include "qrxvirt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void console(void *ctx, uint8_t c) {(void)ctx; putchar((int)c);}
static uint32_t mono(void *ctx) {
    (void)ctx;
    struct timespec t={0};
    if (clock_gettime(CLOCK_MONOTONIC,&t)!=0) return 0;
    return (uint32_t)((uint64_t)t.tv_sec*1000u + (uint64_t)t.tv_nsec/1000000u);
}
static void help(const char *exe) {
    fprintf(stderr,"Usage: %s [--ram-kib 1024..65536] [--raw-image <file>] [--steps <n>] [--jit]\n"
        "Default runs embedded chipset-less boot monitor. --raw-image expects reset vectors at offset zero.\n",exe);
}
int main(int argc,char **argv) {
    size_t bytes=1024*1024;
    const char *raw=NULL;
    bool enable_jit=false;
    uint64_t budget=1000000;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--jit")) enable_jit=true;
        else if (!strcmp(argv[i],"--ram-kib") && i+1<argc) {
            char *end=NULL; unsigned long kib=strtoul(argv[++i],&end,10);
            if (*end || kib<1024 || kib>65536) {help(argv[0]); return 2;}
            bytes=kib*1024u;
        } else if (!strcmp(argv[i],"--raw-image") && i+1<argc) raw=argv[++i];
        else if (!strcmp(argv[i],"--steps") && i+1<argc) {
            char *end=NULL; unsigned long long val=strtoull(argv[++i],&end,10);
            if (*end || !val) {help(argv[0]); return 2;}
            budget=val;
        } else {help(argv[0]);return 2;}
    }
    uint8_t *ram=calloc(bytes,1);
    if (!ram) {perror("RAM");return 1;}
    bool loaded=false;
    if (raw) {
        FILE *f=fopen(raw,"rb");
        if (f) {
            size_t n=fread(ram,1,bytes,f);
            int extra=fgetc(f);
            loaded=n>=8 && extra==EOF && !ferror(f);
            fclose(f);
        }
    } else loaded=qrx_build_boot_monitor(ram,bytes);
    if (!loaded) {fprintf(stderr,"Cannot load raw image / generate ROM\n");free(ram);return 1;}
    QRXVM vm;
    qrx_vm_init(&vm,ram,bytes);
    qrx_vm_set_host(&vm,console,mono,NULL);
    vm.jit_enabled=enable_jit;
    if (!qrx_vm_reset(&vm)) {fputs("Invalid reset vectors\n",stderr);free(ram);return 1;}
    int rc=qrx_vm_run(&vm,budget);
    fprintf(stderr,"[qrxvirt] result=%s steps=%llu PC=%08x exception=%u vCPU=1/2 JIT_blocks=%llu compiled=%llu\n",
        rc==0?"halt":(rc==1?"budget":"fault"),
        (unsigned long long)vm.steps,vm.cpu.pc,vm.last_exception,
        (unsigned long long)vm.jit_executed,(unsigned long long)vm.jit_compiled);
    qrx_vm_destroy(&vm);
    free(ram);
    return rc<0 ? 1 : (rc==1 ? 3 : 0);
}
