#define _POSIX_C_SOURCE 200809L
#include "qrx_hostfs.h"
#include "qrx_bootshell.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void write32(QRXVM*v,uint32_t a,uint32_t n){assert(qrx_vm_write(v,a,4,n));}
static uint32_t rd32(QRXVM*v,uint32_t a){uint32_t n=~0u;assert(qrx_vm_read(v,a,4,&n));return n;}
static void name(QRXVM*v,const char*s){memset(v->ram.data+0x2000,0,128);memcpy(v->ram.data+0x2000,s,strlen(s));write32(v,QRX_HOSTFS_NAME_DMA,0x2000);}
static uint32_t operation(QRXVM*v,unsigned op,unsigned offset,unsigned n){write32(v,QRX_HOSTFS_OFFSET,offset);write32(v,QRX_HOSTFS_LENGTH,n);write32(v,QRX_HOSTFS_COMMAND,op);return rd32(v,QRX_HOSTFS_STATUS);}
static unsigned invalidated;
static void dma(void*x,uint32_t a,unsigned n){(void)x;assert(a==0x3000);assert(n>0&&n<=4096);invalidated++;}
int main(void){
 char folder[]="/tmp/qrx-hostfs-XXXXXX";assert(mkdtemp(folder));
 char file[512],linkpath[512],dirpath[512];
 snprintf(file,sizeof(file),"%s/hello.txt",folder);
 snprintf(linkpath,sizeof(linkpath),"%s/secret",folder);
 snprintf(dirpath,sizeof(dirpath),"%s/sub",folder);
 FILE *f=fopen(file,"wb");assert(f);assert(fwrite("HELLO",1,5,f)==5);fclose(f);
 assert(symlink("/etc/passwd",linkpath)==0);assert(mkdir(dirpath,0700)==0);
 uint8_t ram[65536]={0};QRXVM vm;QRXHostFS fs;
 qrx_vm_init(&vm,ram,sizeof(ram));assert(qrx_hostfs_mount(&fs,folder,true));qrx_hostfs_attach(&vm,&fs);
 vm.dma_ram_write=dma;
 write32(&vm,QRX_HOSTFS_DATA_DMA,0x3000);
 assert(rd32(&vm,QRX_HOSTFS_MAGIC)==0x51525846u);
 assert(rd32(&vm,QRX_HOSTFS_FEATURES)==1);
 name(&vm,"hello.txt");assert(operation(&vm,QRX_HOSTFS_CMD_STAT,0,0)==0);
 assert(rd32(&vm,QRX_HOSTFS_SIZE)==5);
 assert(operation(&vm,QRX_HOSTFS_CMD_READ,1,4)==0);
 assert(rd32(&vm,QRX_HOSTFS_RESULT)==4);
 assert(memcmp(ram+0x3000,"ELLO",4)==0);assert(vm.ram_write_epoch==1);assert(invalidated==1);
 assert(operation(&vm,QRX_HOSTFS_CMD_READ,5,4)==0);assert(rd32(&vm,QRX_HOSTFS_RESULT)==0);
 assert(operation(&vm,QRX_HOSTFS_CMD_WRITE,0,4)==QRX_HOSTFS_RDONLY);
 name(&vm,"../hello.txt");assert(operation(&vm,QRX_HOSTFS_CMD_STAT,0,0)==QRX_HOSTFS_INVALID);
 name(&vm,"/etc/passwd");assert(operation(&vm,QRX_HOSTFS_CMD_STAT,0,0)==QRX_HOSTFS_INVALID);
 name(&vm,"secret");assert(operation(&vm,QRX_HOSTFS_CMD_READ,0,20)==QRX_HOSTFS_ACCESS);
 name(&vm,"sub");assert(operation(&vm,QRX_HOSTFS_CMD_STAT,0,0)==QRX_HOSTFS_ACCESS);
 assert(operation(&vm,QRX_HOSTFS_CMD_LIST,0,1024)==0);
 size_t n=rd32(&vm,QRX_HOSTFS_RESULT);assert(n>0);assert(n<1024);ram[0x3000+n]=0;
 assert(strstr((char *)ram+0x3000,"hello.txt\n"));assert(!strstr((char *)ram+0x3000,"secret"));
 assert(!strstr((char *)ram+0x3000,"sub"));
 qrx_hostfs_unmount(&fs);qrx_hostfs_attach(&vm,NULL);
 assert(qrx_hostfs_mount(&fs,folder,false));qrx_hostfs_attach(&vm,&fs);
 assert(rd32(&vm,QRX_HOSTFS_FEATURES)==3);
 write32(&vm,QRX_HOSTFS_DATA_DMA,0x3000);
 name(&vm,"new.txt");memcpy(ram+0x3000,"ABC",3);
 assert(operation(&vm,QRX_HOSTFS_CMD_WRITE,0,3)==0);
 assert(operation(&vm,QRX_HOSTFS_CMD_STAT,0,0)==0);assert(rd32(&vm,QRX_HOSTFS_SIZE)==3);
 memcpy(ram+0x3000,"X",1);assert(operation(&vm,QRX_HOSTFS_CMD_WRITE,1,1)==0);
 assert(operation(&vm,QRX_HOSTFS_CMD_READ,0,3)==0);assert(memcmp(ram+0x3000,"AXC",3)==0);
 assert(operation(&vm,QRX_HOSTFS_CMD_TRUNCATE,2,0)==0);
 assert(operation(&vm,QRX_HOSTFS_CMD_STAT,0,0)==0);assert(rd32(&vm,QRX_HOSTFS_SIZE)==2);
 assert(operation(&vm,QRX_HOSTFS_CMD_DELETE,0,0)==0);
 assert(operation(&vm,QRX_HOSTFS_CMD_STAT,0,0)==QRX_HOSTFS_NOENT);
 name(&vm,"secret");assert(operation(&vm,QRX_HOSTFS_CMD_WRITE,0,3)==QRX_HOSTFS_ACCESS);
 assert(operation(&vm,QRX_HOSTFS_CMD_DELETE,0,0)==QRX_HOSTFS_ACCESS);
 /* 32-bit full 68040 MMIO alias normalizes these addresses upstream. */
 assert((QRX_HOSTFS_BASE-QRX_IO_BASE)==0x800);
 assert(!qrx_vm_read(&vm,QRX_HOSTFS_SIZE+4,4,&(uint32_t){0}));
 qrx_hostfs_unmount(&fs);qrx_vm_destroy(&vm);
 unlink(linkpath);unlink(file);rmdir(dirpath);rmdir(folder);
 puts("PASS QRXHostFS: real directory, DMA, offset, RW, readonly, symlink+traversal denial, persistence");
 return 0;
}
