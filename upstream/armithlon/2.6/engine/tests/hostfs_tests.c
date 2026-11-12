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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
static void write32(QRXVM*v,uint32_t a,uint32_t n){assert(qrx_vm_write(v,a,4,n));}
static uint32_t rd32(QRXVM*v,uint32_t a){uint32_t n=~0u;assert(qrx_vm_read(v,a,4,&n));return n;}
static void name(QRXVM*v,const char*s){memset(v->ram.data+0x2000,0,128);memcpy(v->ram.data+0x2000,s,strlen(s));write32(v,QRX_HOSTFS_NAME_DMA,0x2000);}
static uint32_t operation(QRXVM*v,unsigned op,unsigned offset,unsigned n){write32(v,QRX_HOSTFS_OFFSET,offset);write32(v,QRX_HOSTFS_LENGTH,n);write32(v,QRX_HOSTFS_COMMAND,op);return rd32(v,QRX_HOSTFS_STATUS);}
static void net_put(QRXVM*v,unsigned i,uint32_t n){assert(qrx_vm_write(v,0x4000+i*4,4,n));}
static uint32_t net_get(QRXVM*v,unsigned i){return rd32(v,0x4000+i*4);}
static uint32_t net_call(QRXVM*v,unsigned op,const uint32_t*a,uint32_t*err){
 net_put(v,0,op);for(unsigned i=0;i<12;i++)net_put(v,i+1,a?a[i]:0);
 write32(v,QRX_HOSTFS_DATA_DMA,0x4000);write32(v,QRX_HOSTFS_LENGTH,QRX_HOSTFS_NET_WORDS*4);
 write32(v,QRX_HOSTFS_COMMAND,QRX_HOSTFS_CMD_NET);assert(rd32(v,QRX_HOSTFS_STATUS)==0);
 *err=net_get(v,10);return net_get(v,9);
}
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
 assert(rd32(&vm,QRX_HOSTFS_FEATURES)==0x15);
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
 assert(rd32(&vm,QRX_HOSTFS_FEATURES)==0x17);
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
  assert(rd32(&vm,QRX_HOSTFS2_FEATURES)==0x15);
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
 /* Socket door: a real host listener, guest connect/send/recv through MMIO. */
 {
  int srv=socket(AF_INET,SOCK_STREAM,0);assert(srv>=0);
  struct sockaddr_in sa;memset(&sa,0,sizeof(sa));sa.sin_family=AF_INET;
  sa.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(bind(srv,(struct sockaddr*)&sa,sizeof(sa))==0);
  assert(listen(srv,4)==0);socklen_t sl=sizeof(sa);assert(getsockname(srv,(struct sockaddr*)&sa,&sl)==0);
  unsigned port=ntohs(sa.sin_port);uint32_t a[12]={0},e=0;
  a[0]=1;uint32_t g=net_call(&vm,1,a,&e);assert((int32_t)g>=0);
  memset(a,0,sizeof(a));a[0]=g;a[1]=INADDR_LOOPBACK;a[2]=port;
  uint32_t r=net_call(&vm,5,a,&e);
  for(int i=0;i<2000&&(int32_t)r<0&&(e==10035||e==10036||e==10037);i++){
   usleep(1000);memset(a,0,sizeof(a));a[0]=g;a[1]=2;r=net_call(&vm,10,a,&e);if(r)break;}
  assert((int32_t)r>=0);
  int cl=-1;for(int i=0;i<2000&&cl<0;i++){struct pollfd p={srv,POLLIN,0};if(poll(&p,1,10)>0)cl=accept(srv,NULL,NULL);if(cl<0)usleep(1000);}
  assert(cl>=0);
  memcpy(ram+0x4100,"DOOR",4);memset(a,0,sizeof(a));a[0]=g;a[1]=0x4100;a[2]=4;
  for(int i=0;i<2000;i++){r=net_call(&vm,6,a,&e);if((int32_t)r>=0)break;assert(e==10035);usleep(1000);}
  assert(r==4);struct pollfd cp={cl,POLLIN,0};assert(poll(&cp,1,2000)>0);
  char got[4];assert(read(cl,got,4)==4);assert(!memcmp(got,"DOOR",4));
  memset(ram+0x4200,0,16);assert(write(cl,"BACK",4)==4);
  memset(a,0,sizeof(a));a[0]=g;a[1]=0x4200;a[2]=4;
  for(int i=0;i<2000;i++){r=net_call(&vm,7,a,&e);if((int32_t)r>0)break;assert(e==10035);usleep(1000);}
  assert(r==4);assert(!memcmp(ram+0x4200,"BACK",4));
  memset(a,0,sizeof(a));a[0]=g;assert(net_call(&vm,11,a,&e)==0);
  close(cl);close(srv);
 }
 /* The server side of the same door: guest bind/listen/accept. */
 {
  int cl=socket(AF_INET,SOCK_STREAM,0);assert(cl>=0);uint32_t a[12]={0},e=0;
  a[0]=1;uint32_t g=net_call(&vm,1,a,&e);assert((int32_t)g>=0);
  memset(a,0,sizeof(a));a[0]=g;a[1]=INADDR_LOOPBACK;a[2]=0;assert(net_call(&vm,2,a,&e)==0);
  memset(a,0,sizeof(a));a[0]=g;a[1]=4;assert(net_call(&vm,3,a,&e)==0);
  memset(a,0,sizeof(a));a[0]=g;assert(net_call(&vm,15,a,&e)==0);
  unsigned port=net_get(&vm,12);assert(port!=0);
  struct sockaddr_in sa;memset(&sa,0,sizeof(sa));sa.sin_family=AF_INET;
  sa.sin_addr.s_addr=htonl(INADDR_LOOPBACK);sa.sin_port=htons((unsigned short)port);
  assert(connect(cl,(struct sockaddr*)&sa,sizeof(sa))==0);
  uint32_t r=0;for(int i=0;i<2000;i++){memset(a,0,sizeof(a));a[0]=g;a[1]=1;r=net_call(&vm,10,a,&e);if(r)break;usleep(1000);}
  assert(r&1u);memset(a,0,sizeof(a));a[0]=g;uint32_t s=net_call(&vm,4,a,&e);assert((int32_t)s>=0);
  memcpy(ram+0x4300,"SRV!",4);memset(a,0,sizeof(a));a[0]=s;a[1]=0x4300;a[2]=4;
  for(int i=0;i<2000;i++){r=net_call(&vm,6,a,&e);if((int32_t)r>=0)break;assert(e==10035);usleep(1000);}
  assert(r==4);char got[4];assert(read(cl,got,4)==4);assert(!memcmp(got,"SRV!",4));
  memset(ram+0x4400,0,16);assert(write(cl,"CLNT",4)==4);
  memset(a,0,sizeof(a));a[0]=s;a[1]=0x4400;a[2]=4;
  for(int i=0;i<2000;i++){r=net_call(&vm,7,a,&e);if((int32_t)r>0)break;assert(e==10035);usleep(1000);}
  assert(r==4);assert(!memcmp(ram+0x4400,"CLNT",4));
  memset(a,0,sizeof(a));a[0]=s;assert(net_call(&vm,11,a,&e)==0);
  memset(a,0,sizeof(a));a[0]=g;assert(net_call(&vm,11,a,&e)==0);
  close(cl);
 }
 qrx_hostfs_unmount(&fs);qrx_vm_destroy(&vm);
 unlink(linkpath);unlink(file);rmdir(dirpath);rmdir(folder);
 puts("PASS QRXHostFS: real directory, DMA, offset, RW, readonly, symlink+traversal denial, persistence");
 return 0;
}
