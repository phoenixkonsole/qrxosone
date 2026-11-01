#define _POSIX_C_SOURCE 200809L
#include "qrx_block.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void dma_written(void *opaque,uint32_t addr,unsigned len){
    unsigned *calls=opaque;assert(addr==0x4000u||addr==0x5000u);assert(len==512);++*calls;
}
int main(void){
    char path[]="/tmp/qrx-block-XXXXXX";
    int f=mkstemp(path);assert(f>=0);close(f);unlink(path);
    QRXBlock d={0},rd={0};
    unsigned char ram[65536]={0};QRXVM vm;
    qrx_vm_init(&vm,ram,sizeof(ram));
    unsigned callbacks=0; vm.dma_ram_write=dma_written;vm.dma_opaque=&callbacks;
    assert(qrx_block_mount(&d,path,16,true,false));
    qrx_block_attach(&vm,&d);
    uint32_t a;
    assert(qrx_vm_read(&vm,QRX_BLOCK_MAGIC,4,&a)&&a==0x51525842u);
    assert(qrx_vm_read(&vm,QRX_BLOCK_SECTORS,4,&a)&&a==16);
    for(unsigned i=0;i<512;i++)ram[0x4000+i]=(unsigned char)(i^0xa5u);
    assert(qrx_vm_write(&vm,QRX_BLOCK_LBA,4,3));
    assert(qrx_vm_write(&vm,QRX_BLOCK_DMA,4,0x4000));
    assert(qrx_vm_write(&vm,QRX_BLOCK_COMMAND,4,2));
    assert(qrx_vm_read(&vm,QRX_BLOCK_STATUS,4,&a)&&a==0);
    memset(ram+0x4000,0,512);
    assert(qrx_vm_write(&vm,QRX_BLOCK_COMMAND,4,1));
    for(unsigned i=0;i<512;i++)assert(ram[0x4000+i]==(unsigned char)(i^0xa5u));
    assert(qrx_vm_read(&vm,QRX_BLOCK_STATUS,4,&a)&&a==0);
    assert(callbacks==1);
    assert(qrx_vm_write(&vm,QRX_BLOCK_LBA,4,16));
    assert(qrx_vm_write(&vm,QRX_BLOCK_COMMAND,4,1));
    assert(qrx_vm_read(&vm,QRX_BLOCK_STATUS,4,&a)&&a==1);
    assert(qrx_vm_write(&vm,QRX_BLOCK_LBA,4,0));
    assert(qrx_vm_write(&vm,QRX_BLOCK_DMA,4,65536-256));
    assert(qrx_vm_write(&vm,QRX_BLOCK_COMMAND,4,1));
    assert(qrx_vm_read(&vm,QRX_BLOCK_STATUS,4,&a)&&a==1);
    qrx_block_attach(&vm,NULL);qrx_block_unmount(&d);
    assert(qrx_block_mount(&rd,path,16,false,true));
    qrx_block_attach(&vm,&rd);
    assert(qrx_vm_write(&vm,QRX_BLOCK_DMA,4,0x5000));
    assert(qrx_vm_write(&vm,QRX_BLOCK_LBA,4,3));
    assert(qrx_vm_write(&vm,QRX_BLOCK_COMMAND,4,1));
    for(unsigned i=0;i<512;i++)assert(ram[0x5000+i]==(unsigned char)(i^0xa5u));
    assert(qrx_vm_write(&vm,QRX_BLOCK_COMMAND,4,2));
    assert(qrx_vm_read(&vm,QRX_BLOCK_STATUS,4,&a)&&a==3);
    assert(callbacks==2);
    qrx_block_attach(&vm,NULL);qrx_block_unmount(&rd);
    assert(!qrx_block_mount(&d,path,32,false,false));
    unlink(path);qrx_vm_destroy(&vm);
    puts("PASS QRX block DMA disk persistence, bounds and read-only");return 0;
}
