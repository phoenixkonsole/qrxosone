/* Real QRX68100 guest firmware boot and persistence tests, no mock CPU. */
#define _POSIX_C_SOURCE 200809L
#include "qrx_bootshell.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char output[65536];static size_t count;
static void capture(void *ctx,uint8_t c){(void)ctx;if(count+1<sizeof(output)){output[count++]=(char)c;output[count]=0;}}
static void clear_output(void){count=0;output[0]=0;}
static void check_has(const char *needle){if(!strstr(output,needle)){fprintf(stderr,"Missing output [%s] from [%s]\n",needle,output);abort();}}
static void initialize(QRXVM *v,QRXShell *s,uint8_t *ram,const char *disk){
    qrx_vm_init(v,ram,QRX_BOOT_RAM);qrx_vm_set_host(v,capture,NULL,NULL);
    assert(qrx_build_boot_shell(ram,QRX_BOOT_RAM));
    assert(qrx_shell_mount(s,v,disk));assert(qrx_vm_reset(v));
    assert(qrx_vm_run(v,3000)==1);
    check_has("QRX OS One 2.1");check_has("qrx> ");
    assert(v->steps>100);
}
static void line(QRXShell *s,const char *str,const char *expected){
    clear_output(); assert(qrx_shell_run_line(s,str,60000));check_has(expected);
}
int main(void){
    char dir[]="/tmp/qrx-fs-test.XXXXXX";
    assert(mkdtemp(dir));char disk[512];snprintf(disk,sizeof(disk),"%s/data.qrx",dir);
    uint8_t *ram=calloc(QRX_BOOT_RAM,1);assert(ram);
    QRXVM v;QRXShell *s=calloc(1,sizeof(*s));assert(s);
    clear_output();initialize(&v,s,ram,disk);
    line(s,"help","write NAME TEXT");
    line(s,"ls","README.TXT");
    line(s,"cat README.TXT","Welcome to QRX OS One");
    line(s,"run HELLO.QEX","Hello QRX");
    line(s,"bench","retired instructions");
    line(s,"hex RET.QEX 51455831000000024e75","OK: binary written");
    line(s,"run RET.QEX","Launching guest");
    line(s,"run README.TXT","ERR: not a bounded QEX1 program");
    line(s,"write NOTES.TXT Hello from guest 68k!","OK: saved");
    line(s,"append NOTES.TXT second line","OK: saved");
    line(s,"cat NOTES.TXT","Hello from guest 68k!\nsecond line");
    line(s,"write ../ESCAPE bad","ERR:");
    line(s,"cat ../ESCAPE","ERR:");
    line(s,"cpu","core 0 active");
    line(s,"stats","Guest instructions retired");
    line(s,"reboot","Restarting");assert(s->reboot);s->reboot=false;
    assert(qrx_vm_reset(&v));clear_output();assert(qrx_vm_run(&v,3000)==1);check_has("QRX OS One");
    line(s,"cat NOTES.TXT","second line");
    uint32_t out=0;assert(qrx_vm_read(&v,QRX_BOOT_RX_STATUS,4,&out)&&out==0);
    assert(!qrx_vm_read(&v,QRX_BOOT_EXEC,4,&out));
    line(s,"shutdown","shutdown complete");assert(s->shutdown&&v.cpu.halted);
    qrx_shell_detach(s);qrx_vm_destroy(&v);
    free(s);free(ram);
    /* Cold start and persistence; no host-side replay of commands. */
    ram=calloc(QRX_BOOT_RAM,1);s=calloc(1,sizeof(*s));assert(ram&&s);
    clear_output();initialize(&v,s,ram,disk);
    line(s,"cat NOTES.TXT","second line");
    line(s,"rm NOTES.TXT","OK: removed");
    line(s,"cat NOTES.TXT","ERR: file not found");
    qrx_shell_detach(s);qrx_vm_destroy(&v);free(s);free(ram);
    /* Never silently format or overwrite a corrupt disk. */
    FILE *f=fopen(disk,"r+b");assert(f);assert(fseek(f,8,SEEK_SET)==0);assert(fputc(0x99,f)!=EOF);fclose(f);
    ram=calloc(QRX_BOOT_RAM,1);s=calloc(1,sizeof(*s));assert(ram&&s);qrx_vm_init(&v,ram,QRX_BOOT_RAM);
    assert(!qrx_shell_mount(s,&v,disk));
    qrx_vm_destroy(&v);free(s);free(ram);
    unlink(disk);rmdir(dir);
    puts("PASS actual 68k boot firmware + QRXFS-1 save/reload/reboot/corruption protection");
    return 0;
}
