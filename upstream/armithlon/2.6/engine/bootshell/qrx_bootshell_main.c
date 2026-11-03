/* ARMITHLON 1.8 boots actual 68k firmware; shell commands go through MMIO.
 * Optional alternate binary links the tested full Musashi 68040 backend. */
#ifndef __APPLE__
#define _POSIX_C_SOURCE 200809L
#else
#define _DARWIN_C_SOURCE
#endif
#include "qrx_bootshell.h"
#include "qrx_block.h"
#include "qrx_hostfs.h"
#include "qrx_memory.h"
#ifdef QRX_BOOT_USE_MUSASHI
#include "qrx68100_full.h"
#include "qrx68100_dual.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void output(void *ctx,uint8_t c){(void)ctx;fputc(c,stdout);fflush(stdout);}
static void usage(const char *p){fprintf(stderr,"Usage: %s [--disk PATH] [--script FILE] [--command CMD] [--rom PATH] [--share DIR] [--share-rw] [--share2 DIR] [--share2-rw] [--ram auto|MiB] [--ram-mib MiB] [--volatile-disk] [--no-interactive] [--dual (Musashi only)]\nDefault guest RAM: one quarter of effective host RAM (capped at 3840 MiB by 32-bit address space).\n",p);}
#ifdef QRX_BOOT_USE_MUSASHI
static int musashi_run(void *p,uint64_t budget){return qrx_full_run_cycles((QRXFullCPU*)p,budget);}
static bool musashi_subroutine(void *p,uint32_t entry,uint64_t budget,uint64_t *retired){return qrx_full_run_subroutine((QRXFullCPU*)p,entry,budget,retired);}
static int dual_run(void *p,uint64_t budget){return qrx_dual_run((QRXDual*)p,budget);}
static bool dual_subroutine(void *p,uint32_t entry,uint64_t budget,uint64_t *retired){
    QRXDual *dual=p;
    return qrx_full_run_subroutine(dual->board,entry,budget,retired);
}
/* Core 1 executes a finite CPU-side counter task and parks in STOP.
   Core 0 continues to host the interactive 68k firmware.  This is a
   dual-68040 scheduling diagnostic, NOT AmigaOS/AROS SMP. */
static int start_guest_worker(QRXFullCPU *full,QRXDual *dual){
    static const unsigned char worker[]={
      0x20,0x3c,0,0,0x07,0xd0,       /* MOVE.L #2000,D0 */
      0x22,0x3c,0,0,0,0,             /* MOVE.L #0,D1 */
      0x52,0x81,                     /* ADDQ.L #1,D1 */
      0x53,0x80,                     /* SUBQ.L #1,D0 */
      0x66,0xfa,                     /* BNE $-6 */
      0x23,0xc1,0,1,0xb0,0,          /* MOVE.L D1,$1B000 */
      0x4e,0x72,0x27,0               /* STOP #$2700 */
    };
    if(full->vm.ram.size<0x1e000u || !qrx_dual_set_mode(dual,QRX_BOOT_QRX_DUAL))return 0;
    memcpy(full->vm.ram.data+0x18000u,worker,sizeof(worker));
    qrx_dual_notify_ram_write(dual,0x18000u,sizeof(worker));
    return qrx_full_bus_write(full,QRX_DUAL_CORE1_PC,4,0x18000u) &&
           qrx_full_bus_write(full,QRX_DUAL_CORE1_SP,4,0x1d000u) &&
           qrx_full_bus_write(full,QRX_DUAL_CORE1_CTL,4,1u) && qrx_dual_commit(dual);
}
#endif
static int execute_line(QRXShell *shell,const char *text){
    if(!qrx_shell_run_line(shell,text,60000)){
        fprintf(stderr,"[QRX] Guest did not process command, PC/state invalid or guest step budget exhausted\n");return 1;
    }
    return 0;
}
int main(int argc,char **argv){
    const char *disk=getenv("QRX_BOOT_DISK"),*script=NULL,*single=NULL,*rom_path=NULL,*raw_disk=NULL;
    unsigned raw_sectors=0;int create_raw=0;
    size_t guest_ram_bytes=qrx_ram_auto_bytes();
    const char *share_dir=NULL;int share_rw=0,volatile_disk=0;
    const char *share2_dir=NULL;int share2_rw=0;
    int interactive=1, dual_requested=0;
    if(!disk||!*disk)disk="qrx-system.img";
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--disk")&&i+1<argc)disk=argv[++i];
        else if(!strcmp(argv[i],"--script")&&i+1<argc){script=argv[++i];interactive=0;}
        else if(!strcmp(argv[i],"--command")&&i+1<argc){single=argv[++i];interactive=0;}
        else if(!strcmp(argv[i],"--rom")&&i+1<argc){rom_path=argv[++i];interactive=0;}
        else if((!strcmp(argv[i],"--ram-mib")||!strcmp(argv[i],"--ram"))&&i+1<argc){
            const char *value=argv[++i];
            if(!strcmp(value,"auto"))guest_ram_bytes=qrx_ram_auto_bytes();
            else {
                char *end=NULL;unsigned long long n=strtoull(value,&end,10);
                if(end==value || !end || *end || n>QRX_RAM_MAX_BYTES/(1024u*1024u) ||
                   !qrx_ram_validate(n*1024u*1024u)){
                    fprintf(stderr,"Guest RAM must be 1..3840 MiB or auto (32-bit guest limit)\n");return 2;
                }
                guest_ram_bytes=(size_t)n*1024u*1024u;
            }
        }
        else if(!strcmp(argv[i],"--share")&&i+1<argc)share_dir=argv[++i];
        else if(!strcmp(argv[i],"--share-rw"))share_rw=1;
        else if(!strcmp(argv[i],"--share2")&&i+1<argc)share2_dir=argv[++i];
        else if(!strcmp(argv[i],"--share2-rw"))share2_rw=1;
        else if(!strcmp(argv[i],"--volatile-disk"))volatile_disk=1;
        else if(!strcmp(argv[i],"--raw-disk")&&i+1<argc)raw_disk=argv[++i];
        else if(!strcmp(argv[i],"--raw-sectors")&&i+1<argc)raw_sectors=(unsigned)strtoul(argv[++i],NULL,10);
        else if(!strcmp(argv[i],"--create-raw-disk"))create_raw=1;
        else if(!strcmp(argv[i],"--no-interactive"))interactive=0;
        else if(!strcmp(argv[i],"--dual"))dual_requested=1;
        else{usage(argv[0]);return 2;}
    }
#ifndef QRX_BOOT_USE_MUSASHI
    if(dual_requested){fprintf(stderr,"--dual requires ./armithlon boot-full and pinned Musashi CPU\n");return 2;}
#endif
    if((raw_disk!=NULL)!=(raw_sectors!=0u) || (create_raw&&!raw_disk)){
        fprintf(stderr,"--raw-disk PATH requires --raw-sectors N; --create-raw-disk only with explicit path\n");return 2;
    }
    if(volatile_disk)disk=":memory:";
    if(share_rw&&!share_dir){fprintf(stderr,"--share-rw requires --share DIRECTORY\n");return 2;}
    if(share2_rw&&!share2_dir){fprintf(stderr,"--share2-rw requires --share2 DIRECTORY\n");return 2;}
    int rc=1;QRXBlock raw_block={0};QRXHostFS hostfs={0};QRXHostFS hostfs2={0};
    QRXShell *shell=calloc(1,sizeof(QRXShell));
    if(!shell)return 2;
    QRXVM *vm=NULL;
#ifndef QRX_BOOT_USE_MUSASHI
    QRXVM guest;
    QRXHostMemory allocation={0};
    if(!qrx_ram_allocate(&allocation,guest_ram_bytes)){
        fprintf(stderr,"[QRX] Cannot reserve %zu MiB of guest RAM\n",guest_ram_bytes/(1024u*1024u));
        free(shell);return 2;
    }
    uint8_t *ram=allocation.addr;
    qrx_vm_init(&guest,ram,guest_ram_bytes);vm=&guest;
#else
    QRXFullCPU full;
    QRXDual *dual=NULL;
    if(!qrx_full_init(&full,guest_ram_bytes,68040)){
        fprintf(stderr,"Musashi 68040 backend initialization failed\n");free(shell);return 2;
    }
    vm=&full.vm;
#endif
    qrx_vm_set_host(vm,output,NULL,NULL);
    if(share_dir){
        if(!qrx_hostfs_mount(&hostfs,share_dir,!share_rw)){
            fprintf(stderr,"[QRX] Host filesystem directory mount failed (no symlinks allowed)\n");goto done;
        }
        qrx_hostfs_attach(vm,&hostfs);
        fprintf(stderr,"[QRX] HOST: directory mounted %s (%s)\n",share_dir,share_rw?"read/write":"read-only");
    }
    if(share2_dir){
        if(!qrx_hostfs_mount(&hostfs2,share2_dir,!share2_rw)){
            fprintf(stderr,"[QRX] Second host directory mount failed (no symlinks allowed)\n");goto done;
        }
        qrx_hostfs_attach2(vm,&hostfs2);
        fprintf(stderr,"[QRX] TEST: directory mounted %s (%s)\n",share2_dir,share2_rw?"read/write":"read-only");
    }
    if(raw_disk){
        if(!qrx_block_mount(&raw_block,raw_disk,raw_sectors,create_raw,false)){
            fprintf(stderr,"QRX raw disk mount failed (refuses overwrite/mismatched size)\n");goto done;
        }
        qrx_block_attach(vm,&raw_block);
    }
    fprintf(stderr,"[QRX] Guest RAM %zu MiB (sparse host reservation, host effective %llu MiB; 32-bit limit 3840 MiB)\n",
            vm->ram.size/(1024u*1024u),
            (unsigned long long)(qrx_effective_host_memory_bytes()/(1024u*1024u)));
    if(!qrx_build_boot_shell(vm->ram.data,vm->ram.size)){
        fprintf(stderr,"Could not prepare executable 68k firmware\n");goto done;
    }
    if(rom_path){
        /* Export firmware bootstrap, not the full sparsely reserved host RAM. */
        FILE *out=fopen(rom_path,"wb");
        if(!out){fprintf(stderr,"Could not open RAM image %s\n",rom_path);goto done;}
        size_t saved=fwrite(vm->ram.data,1,QRX_BOOT_RAM,out);
        int closed=fclose(out);
        if(saved!=QRX_BOOT_RAM || closed){
            fprintf(stderr,"Could not export RAM image to %s\n",rom_path);goto done;
        }
        printf("QRX boot firmware image exported: %s (%u bytes, guest reservation %zu MiB)\n",rom_path,(unsigned)QRX_BOOT_RAM,vm->ram.size/(1024u*1024u));
        rc=0;goto done;
    }
    if(!qrx_shell_mount(shell,vm,disk)){
        fprintf(stderr,"[QRX] Cannot mount QRXFS-1 image %s; image is corrupt or path is unavailable. No data overwritten.\n",disk);
        goto done;
    }
#ifdef QRX_BOOT_USE_MUSASHI
    shell->cpu_opaque=&full; shell->run_cycles=musashi_run; shell->run_subroutine=musashi_subroutine;
    if(!qrx_full_reset(&full)){fprintf(stderr,"Musashi CPU reset failed\n");goto done;}
    if(dual_requested){
        dual=calloc(1,sizeof(*dual));
        if(!dual || !qrx_dual_init(dual,&full) || !start_guest_worker(&full,dual)){
            fprintf(stderr,"QRX68100 secondary core did not start\n");goto done;
        }
        shell->cpu_opaque=dual; shell->run_cycles=dual_run; shell->run_subroutine=dual_subroutine;
        printf("QRX68100: dual 68040 mode enabled; core 1 running diagnostic worker\n");
    }
#else
    if(!qrx_vm_reset(vm)){fprintf(stderr,"68k CPU reset failed\n");goto done;}
#endif
    if((shell->run_cycles?shell->run_cycles(shell->cpu_opaque,3000):qrx_vm_run(vm,3000))<0){
        fprintf(stderr,"Boot firmware faulted\n");goto done;
    }
    if(single){if(execute_line(shell,single))goto done;}
    if(script){
        FILE *in=fopen(script,"r");if(!in){perror("script");goto done;}
        char line[256];
        while(fgets(line,sizeof(line),in)&&!shell->shutdown){
            size_t len=strlen(line);
            if(len && line[len-1]!='\n' && !feof(in)){
                fprintf(stderr,"[QRX] Refusing script command longer than 125 chars\n");fclose(in);goto done;
            }
            if(line[0]=='#')continue;
            if(execute_line(shell,line)){fclose(in);goto done;}
            if(shell->reboot){
                shell->reboot=false;shell->input_head=shell->input_tail=0;
#ifdef QRX_BOOT_USE_MUSASHI
                if(!qrx_full_reset(&full)){fclose(in);goto done;}
                if(dual){qrx_dual_reset(dual);if(!start_guest_worker(&full,dual)){fclose(in);goto done;}}
#else
                if(!qrx_vm_reset(vm)){fclose(in);goto done;}
#endif
                if((shell->run_cycles?shell->run_cycles(shell->cpu_opaque,3000):qrx_vm_run(vm,3000))<0){fclose(in);goto done;}
            }
        }
        fclose(in);
    }
    if(interactive){
        char line[256];
        while(!shell->shutdown){
            if(!fgets(line,sizeof(line),stdin))break;
            size_t n=strlen(line);
            if(n>125){fprintf(stderr,"Command too long; maximum 124 characters\n");
                if(line[n-1]!='\n')while((n=(size_t)getchar())!='\n'&&n!=(size_t)EOF){}
                continue;}
            if(execute_line(shell,line))goto done;
            if(shell->reboot){
                shell->reboot=false;shell->input_head=shell->input_tail=0;
#ifdef QRX_BOOT_USE_MUSASHI
                if(!qrx_full_reset(&full))goto done;
                if(dual){qrx_dual_reset(dual);if(!start_guest_worker(&full,dual))goto done;}
#else
                if(!qrx_vm_reset(vm))goto done;
#endif
                if((shell->run_cycles?shell->run_cycles(shell->cpu_opaque,3000):qrx_vm_run(vm,3000))<0)goto done;
            }
        }
    }
    rc=0;
done:
#ifdef QRX_BOOT_USE_MUSASHI
    if(dual){
        /* A short scripted shell session may finish before the asynchronous
         * diagnostic worker. Give it a bounded scheduling drain before
         * reporting its result; do not replace the guest counter on the host. */
        if(rc==0)for(unsigned i=0;i<100 && !dual->core[1].idle;i++){
            if(qrx_dual_run(dual,1000)<0){rc=1;break;}
        }
        uint32_t count=0;
        (void)qrx_full_bus_read(&full,0x1b000u,4,&count);
        fprintf(stderr,"QRX68100 core1 guest worker counter=%u (expect 2000 after scheduling)\n",count);
        if(rc==0 && count!=2000)rc=1;
        qrx_dual_destroy(dual);free(dual);
    }
#endif
    qrx_shell_detach(shell);
    if(vm){qrx_block_attach(vm,NULL);qrx_hostfs_attach(vm,NULL);qrx_hostfs_attach2(vm,NULL);}
    qrx_hostfs_unmount(&hostfs);
    qrx_hostfs_unmount(&hostfs2);
    qrx_block_unmount(&raw_block);
#ifdef QRX_BOOT_USE_MUSASHI
    qrx_full_destroy(&full);
#else
    qrx_vm_destroy(vm);qrx_ram_release(&allocation);
#endif
    free(shell);return rc;
}
