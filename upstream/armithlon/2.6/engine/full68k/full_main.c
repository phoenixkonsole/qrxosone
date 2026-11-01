/* CLI for complete Musashi instruction decoding on a QRX virtual board.
 * This is a CPU+bus diagnostic, not a complete Amiga/AROS machine emulator. */
#include "qrx68100_full.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void putc_host(void *p,uint8_t ch){(void)p;fputc(ch,stdout);fflush(stdout);}
static uint8_t *load(const char *p,size_t *size){
    FILE *f=fopen(p,"rb");if(!f)return NULL;
    if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}
    long n=ftell(f);if(n<=0 || n>(long)QRX_FULL_ROM_MAX){fclose(f);return NULL;}
    rewind(f);uint8_t *b=malloc((size_t)n);
    if(!b||fread(b,1,(size_t)n,f)!=(size_t)n){free(b);fclose(f);return NULL;}
    fclose(f);*size=(size_t)n;return b;
}
int main(int argc,char **argv){
    unsigned cpu=68040;const char *mainrom=NULL,*extrom=NULL;
    uint64_t budget=1000000;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--cpu")&&i+1<argc)cpu=(unsigned)strtoul(argv[++i],NULL,10);
        else if(!strcmp(argv[i],"--rom")&&i+1<argc)mainrom=argv[++i];
        else if(!strcmp(argv[i],"--ext-rom")&&i+1<argc)extrom=argv[++i];
        else if(!strcmp(argv[i],"--cycles")&&i+1<argc)budget=strtoull(argv[++i],NULL,10);
        else {fprintf(stderr,"usage: qrx68100_full_run [--cpu 68000|68010|68020|68030|68040] [--rom main.bin] [--ext-rom ext.bin] [--cycles N]\n");return 2;}
    }
    QRXFullCPU c;
    if(!qrx_full_init(&c,32u*1024u*1024u,cpu)){fprintf(stderr,"CPU init failed; models 68000..68040, one active VM\n");return 2;}
    qrx_full_set_host(&c,putc_host,NULL,NULL);
    uint8_t *rom=NULL;size_t sz=0;
    int rc=1;
    if(mainrom){
        rom=load(mainrom,&sz);
        if(!rom||!qrx_full_load_rom(&c,QRX_FULL_ROM_MAIN_BASE,rom,sz)){fprintf(stderr,"invalid main ROM\n");goto done;}
        free(rom);rom=NULL;
        if(extrom){rom=load(extrom,&sz);if(!rom||!qrx_full_load_rom(&c,QRX_FULL_ROM_EXT_BASE,rom,sz)){fprintf(stderr,"invalid ext ROM\n");goto done;}free(rom);rom=NULL;}
        qrx_full_set_rom_overlay(&c,true);
        fprintf(stderr,"[ARMITHLON] ROM CPU probe only: Amiga custom chipset, AROS RTG/AHI/input and full desktop boot not implemented\n");
    }else if(!qrx_build_boot_monitor(c.ram_owned,c.vm.ram.size)){fprintf(stderr,"monitor generation failed\n");goto done;}
    if(!qrx_full_reset(&c)){fprintf(stderr,"invalid vectors / reset failed\n");goto done;}
    fprintf(stderr,"[ARMITHLON] model=%u PC=%08x cycles_budget=%llu\n",cpu,qrx_full_pc(&c),(unsigned long long)budget);
    rc=qrx_full_run_cycles(&c,budget);
    fprintf(stderr,"\n[ARMITHLON] result=%d PC=%08x instructions=%llu cycles=%llu fault=%d fault_addr=%08x shutdown=%u\n",
        rc,qrx_full_pc(&c),(unsigned long long)c.instructions,(unsigned long long)c.cycles,
        c.mem_fault,c.fault_address,c.vm.cpu.halted);
    if(mainrom)rc=rc<0?1:3; /* ROM probe never claims desktop boot success */
    else rc=rc==0?0:1;
done:
    free(rom);qrx_full_destroy(&c);return rc;
}
