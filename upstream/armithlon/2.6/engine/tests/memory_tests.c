#ifndef __APPLE__
#define _POSIX_C_SOURCE 200809L
#else
#define _DARWIN_C_SOURCE
#endif
#include "qrx_memory.h"
#include "qrxvirt.h"
#include "qrx_bootshell.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
int main(void){
    uint64_t host=qrx_effective_host_memory_bytes();
    size_t auto_bytes=qrx_ram_auto_bytes();
    assert(qrx_ram_validate(auto_bytes));
    assert(auto_bytes<=QRX_RAM_MAX_BYTES);
    if(host >= 4u*QRX_RAM_MIN_BYTES && host/4u < QRX_RAM_MAX_BYTES)
        assert(auto_bytes <= host/4u && host/4u-auto_bytes < 1024u*1024u);
    assert(!qrx_ram_validate(0));
    assert(!qrx_ram_validate(QRX_RAM_MAX_BYTES+4096u));
    assert(!qrx_ram_validate(65537u));
    QRXHostMemory ram={0};
    assert(qrx_ram_allocate(&ram,128u*1024u*1024u));
    assert(ram.addr && ram.bytes==128u*1024u*1024u);
    /* Sparse reservation: touch only a few pages, and check wide address. */
    QRXVM vm; qrx_vm_init(&vm,ram.addr,ram.bytes);
    assert(qrx_build_boot_shell(ram.addr,ram.bytes));
    uint32_t value=0;
    assert(qrx_vm_write(&vm,64u*1024u*1024u,4,0x11223344u));
    assert(qrx_vm_read(&vm,64u*1024u*1024u,4,&value)&&value==0x11223344u);
    assert(qrx_vm_write(&vm,120u*1024u*1024u,4,0xaabbccddu));
    assert(qrx_vm_read(&vm,120u*1024u*1024u,4,&value)&&value==0xaabbccddu);
    assert(qrx_vm_read(&vm,0,4,&value)&&value==QRX_BOOT_RAM-16u);
    assert(qrx_vm_read(&vm,QRX_IO_BASE,4,&value)&&value==QRX_IO_MAGIC);
    assert(qrx_vm_read(&vm,QRX_IO_RAM_BYTES_LO,4,&value)&&value==128u*1024u*1024u);
    assert(qrx_vm_read(&vm,QRX_IO_RAM_BYTES_HI,4,&value)&&value==0u);
    assert(qrx_vm_read(&vm,QRX_IO_LOW_ALIAS,4,&value)&&value==QRX_IO_BASE);
    assert(!qrx_vm_read(&vm,(uint32_t)ram.bytes,4,&value));
    qrx_vm_destroy(&vm); qrx_ram_release(&ram);
    assert(!ram.addr);
    /* A real large 68040 address test without clearing multi-gigabyte RAM.
       Optional when platform/address space cannot reserve this mapping. */
    if(sizeof(size_t)>=8 && qrx_ram_allocate(&ram,(size_t)QRX_RAM_MAX_BYTES)) {
        qrx_vm_init(&vm,ram.addr,ram.bytes);
        assert(qrx_vm_write(&vm,UINT32_C(0xe0000000),4,0x78563412u));
        assert(qrx_vm_read(&vm,UINT32_C(0xe0000000),4,&value)&&value==0x78563412u);
        assert(!qrx_vm_read(&vm,UINT32_C(0xff000000),4,&value));
        qrx_vm_destroy(&vm);qrx_ram_release(&ram);
        puts("PASS optional 3840 MiB sparse mapping at guest address E0000000");
    } else puts("SKIP 3840 MiB sparse mapping: host address space reservation unavailable");
    printf("PASS 2.1 RAM: host_effective=%llu MiB auto=%zu MiB, 128 MiB sparse 68k bus, legacy MMIO preserved\n",
           (unsigned long long)(host/1048576),auto_bytes/1048576);
    return 0;
}
