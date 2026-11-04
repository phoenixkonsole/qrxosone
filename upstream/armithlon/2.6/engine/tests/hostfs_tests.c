#ifndef __APPLE__
#define _POSIX_C_SOURCE 200809L
#else
#define _DARWIN_C_SOURCE
#endif
#include "qrx_hostfs.h"
#include "qrx_bootshell.h"
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
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
 assert(rd32(&vm,QRX_HOSTFS_FEATURES)==0x11);
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
 assert(rd32(&vm,QRX_HOSTFS_FEATURES)==0x13);
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
 assert(operation(&vm,0x10a,0,0)==0);
 assert(rd32(&vm,QRX_HOSTFS_SIZE)>0);
 assert(rd32(&vm,QRX_HOSTFS_RESULT)<=rd32(&vm,QRX_HOSTFS_SIZE));
 name(&vm,"missing/subdir/file");assert(operation(&vm,0x102,0,0)==QRX_HOSTFS_NOENT);
 /* Tree ABI: nested, case-insensitive paths, persistence, and confinement. */
 name(&vm,"Nested");assert(operation(&vm,0x107,0,0)==0);
 name(&vm,"Nested/hello.bin");assert(operation(&vm,0x109,0,0)==0);
 memcpy(ram+0x3000,"TREE",4);assert(operation(&vm,0x104,0,4)==0);
 /* Durability: the flush thread drains, and a killed emulator keeps the data. */
 qrx_hostfs_sync();assert(qrx_hostfs_flush_pending()==0);
 {
     pid_t child=fork();assert(child>=0);
     if(child==0){
         memcpy(ram+0x3000,"KILL",4);
         if(operation(&vm,0x104,4,4)!=0)_exit(9);
         raise(SIGKILL);
     }
     int status=0;assert(waitpid(child,&status,0)==child);
     assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
     memset(ram+0x3000,0,16);assert(operation(&vm,0x103,0,8)==0);
     assert(!memcmp(ram+0x3000,"TREEKILL",8));
 }
 name(&vm,"nested/HELLO.BIN");assert(operation(&vm,0x103,0,4)==0);assert(!memcmp(ram+0x3000,"TREE",4));
 name(&vm,"Nested");assert(operation(&vm,0x101,0,256)==0);assert(!strcmp((char*)ram+0x3000,"hello.bin"));
 name(&vm,"Nested/../hello.txt");assert(operation(&vm,0x102,0,0)!=0);
 name(&vm,"secret");assert(operation(&vm,0x102,0,0)==QRX_HOSTFS_ACCESS);
 name(&vm,"Nested/hello.bin");assert(operation(&vm,0x105,0,0)==0);
 name(&vm,"Nested");assert(operation(&vm,0x105,0,0)==0);
 /* 32-bit full 68040 MMIO alias normalizes these addresses upstream. */
 assert((QRX_HOSTFS_BASE-QRX_IO_BASE)==0x800);
 assert(!qrx_vm_read(&vm,QRX_HOSTFS_SIZE+4,4,&(uint32_t){0}));
 /* Second share (TEST:) is an independent register block: drives stay isolated. */
 {
  char folder2[]="/tmp/qrx-hostfs2-XXXXXX";char file2[512];QRXHostFS fs2;
  assert(mkdtemp(folder2));
  snprintf(file2,sizeof(file2),"%s/world.txt",folder2);
  FILE *f2=fopen(file2,"wb");assert(f2);assert(fwrite("WORLD",1,5,f2)==5);fclose(f2);
  assert(qrx_hostfs_mount(&fs2,folder2,true));qrx_hostfs_attach2(&vm,&fs2);
  { uint32_t feat=0;assert(qrx_vm_read(&vm,QRX_IO_BASE+0x08,4,&feat));
    assert(feat&(QRX_FEATURE_HOSTFS|QRX_FEATURE_HOSTFS2)); }
  assert(rd32(&vm,QRX_HOSTFS2_MAGIC)==0x51525846u);
  assert(rd32(&vm,QRX_HOSTFS2_FEATURES)==0x11);
  memset(ram+0x2000,0,128);memcpy(ram+0x2000,"world.txt",9);
  assert(qrx_vm_write(&vm,QRX_HOSTFS2_NAME_DMA,4,0x2000));
  assert(qrx_vm_write(&vm,QRX_HOSTFS2_DATA_DMA,4,0x3000));
  assert(qrx_vm_write(&vm,QRX_HOSTFS2_OFFSET,4,0)==true);
  assert(qrx_vm_write(&vm,QRX_HOSTFS2_LENGTH,4,0)==true);
  assert(qrx_vm_write(&vm,QRX_HOSTFS2_COMMAND,4,QRX_HOSTFS_CMD_STAT)==true);
  assert(rd32(&vm,QRX_HOSTFS2_STATUS)==0);assert(rd32(&vm,QRX_HOSTFS2_SIZE)==5);
  /* First share still answers on its own base. */
  name(&vm,"new.txt");assert(operation(&vm,QRX_HOSTFS_CMD_STAT,0,0)==QRX_HOSTFS_NOENT);
  qrx_hostfs_unmount(&fs2);qrx_hostfs_attach2(&vm,NULL);
  unlink(file2);rmdir(folder2);
 }
 qrx_hostfs_unmount(&fs);qrx_vm_destroy(&vm);
 unlink(linkpath);unlink(file);rmdir(dirpath);rmdir(folder);
 puts("PASS QRXHostFS: real directory, DMA, offset, RW, readonly, symlink+traversal denial, persistence");
 return 0;
}
