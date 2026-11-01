/* Full 68040 bus uses the real bus adapter with a Musashi test stub, not CPU execution. */
#include "qrx68100_full.h"
#include "qrxvirt.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
int main(void){
 QRXFullCPU c;
 assert(qrx_full_init(&c,128u*1024u*1024u,68040));
 uint32_t v=0;
 assert(qrx_full_bus_write(&c,96u*1024u*1024u,4,0x11223344u));
 assert(qrx_full_bus_read(&c,96u*1024u*1024u,4,&v)&&v==0x11223344u);
 assert(qrx_full_bus_read(&c,QRX_FULL_IO_BASE,4,&v)&&v==QRX_IO_MAGIC);
 assert(qrx_full_bus_read(&c,QRX_FULL_IO_BASE+0x34u,4,&v)&&v==128u*1024u*1024u);
 uint8_t *rom=calloc(512u*1024u,1);assert(rom);
 rom[0]=0x4e;rom[1]=0x71;
 assert(qrx_full_load_rom(&c,QRX_FULL_ROM_MAIN_BASE,rom,512u*1024u));free(rom);
 assert(qrx_full_bus_read(&c,QRX_FULL_ROM_MAIN_BASE,2,&v)&&v==0x4e71u);
 assert(!qrx_full_bus_write(&c,QRX_FULL_ROM_MAIN_BASE,2,0xffffu));
 assert(qrx_full_bus_read(&c,QRX_FULL_ROM_MAIN_BASE,2,&v)&&v==0x4e71u);
 /* With a real Kickstart ROM, the legacy low MMIO alias is covered by ROM.
    Use FF000000 for the QRX board, which stays disjoint. */
 assert(qrx_full_bus_read(&c,QRX_FULL_IO_BASE,4,&v)&&v==QRX_IO_MAGIC);
 assert(qrx_full_bus_read(&c,QRX_IO_BASE,4,&v)&&v==0u);
 qrx_full_destroy(&c);
 puts("PASS 68040 guest 128MiB high RAM, Kickstart ROM read protection, upper MMIO with classic ROM overlay, CPU stub");
 return 0;
}
