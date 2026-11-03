#ifndef __APPLE__
#define _POSIX_C_SOURCE 200809L
#else
#define _DARWIN_C_SOURCE
#endif
/* This tests qrx_full_bus 68040 upper alias with Musashi STUB; never claims
 * actual guest FPU/ISA execution. */
#include "qrx68100_full.h"
#include "qrx_hostfs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void){
 char p[]="/tmp/qrx-ff-hostfs-XXXXXX";assert(mkdtemp(p));
 QRXFullCPU full;assert(qrx_full_init(&full,1024*1024,68040));
 QRXHostFS fs;assert(qrx_hostfs_mount(&fs,p,false));
 qrx_hostfs_attach(&full.vm,&fs);
 uint32_t val=0;
 assert(qrx_full_bus_read(&full,QRX_FULL_IO_BASE+0x800,4,&val)&&val==0x51525846u);
 assert(qrx_full_bus_read(&full,QRX_FULL_IO_BASE+8,4,&val)&&(val&QRX_FEATURE_HOSTFS));
 const char *name="upper-mmio.txt";
 memcpy(full.vm.ram.data+0x2000,name,strlen(name)+1);
 memcpy(full.vm.ram.data+0x3000,"OK",2);
 assert(qrx_full_bus_write(&full,QRX_FULL_IO_BASE+0x808,4,0x2000));
 assert(qrx_full_bus_write(&full,QRX_FULL_IO_BASE+0x80c,4,0x3000));
 assert(qrx_full_bus_write(&full,QRX_FULL_IO_BASE+0x810,4,0));
 assert(qrx_full_bus_write(&full,QRX_FULL_IO_BASE+0x814,4,2));
 assert(qrx_full_bus_write(&full,QRX_FULL_IO_BASE+0x818,4,4));
 assert(qrx_full_bus_read(&full,QRX_FULL_IO_BASE+0x81c,4,&val)&&val==0);
 qrx_hostfs_attach(&full.vm,NULL);qrx_hostfs_unmount(&fs);
 qrx_full_destroy(&full);
 char file[256];snprintf(file,sizeof(file),"%s/%s",p,name);
 FILE *fp=fopen(file,"rb");assert(fp);char b[4]={0};assert(fread(b,1,2,fp)==2&&memcmp(b,"OK",2)==0);fclose(fp);
 assert(unlink(file)==0);assert(rmdir(p)==0);
 puts("PASS 68040 FF000800 hostfs MMIO alias and DMA with CPU mock");
 return 0;
}
