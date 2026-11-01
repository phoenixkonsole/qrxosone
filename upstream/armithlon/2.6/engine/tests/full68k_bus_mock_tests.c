/* Explicit mock test – no 68k instruction execution and no CPU pass claim. */
#include "qrx68100_full.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void put32(unsigned char *p,unsigned v) {p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
int main(void){
  QRXFullCPU c; assert(qrx_full_init(&c,4u*1024*1024,68040));
  unsigned v;
  put32(c.ram_owned,0x40000);put32(c.ram_owned+4,0x100);
  assert(qrx_full_reset(&c));
  assert(qrx_full_bus_read(&c,QRX_FULL_IO_BASE,4,&v)&&v==QRX_IO_MAGIC);
  assert(qrx_full_bus_write(&c,0x2000,4,0xdeadbeef));
  assert(qrx_full_bus_read(&c,0x2001,2,&v)&&v==0xadbe);
  assert(!qrx_full_bus_write(&c,0x3ffffe,4,0));
  assert(qrx_full_bus_write(&c,QRX_FULL_TIMER_PERIOD,4,1024));
  assert(qrx_full_bus_write(&c,QRX_FULL_TIMER_CTRL,4,QRX_FULL_TIMER_ENABLE));
  assert(qrx_full_run_cycles(&c,2048)==1); /* mock consumes cycles without executing */
  assert(qrx_full_bus_read(&c,QRX_FULL_IRQ_PENDING,4,&v)&&v==(1u<<2));
  assert(qrx_full_bus_write(&c,QRX_FULL_IRQ_ACK,4,(1u<<2)));
  assert(qrx_full_bus_read(&c,QRX_FULL_IRQ_PENDING,4,&v)&&!v);
  unsigned char *rom=calloc(512u*1024,1);assert(rom);
  put32(rom,0x40000);put32(rom+4,QRX_FULL_ROM_MAIN_BASE+0x100);
  rom[0x100]=0x4e;rom[0x101]=0x71;
  assert(qrx_full_load_rom(&c,QRX_FULL_ROM_MAIN_BASE,rom,512u*1024));
  free(rom);
  qrx_full_set_rom_overlay(&c,true);
  assert(qrx_full_reset(&c));
  assert(qrx_full_pc(&c)==QRX_FULL_ROM_MAIN_BASE+0x100);
  assert(qrx_full_bus_read(&c,QRX_FULL_ROM_MAIN_BASE+0x100,2,&v)&&v==0x4e71);
  assert(!qrx_full_bus_write(&c,QRX_FULL_ROM_MAIN_BASE+0x100,2,0));
  assert(qrx_full_bus_read(&c,QRX_FULL_IO_BASE,4,&v)&&v==QRX_IO_MAGIC);
  assert(qrx_full_bus_write(&c,QRX_FULL_ROM_OVERLAY_CTRL,4,0));
  assert(qrx_full_bus_read(&c,QRX_FULL_ROM_OVERLAY_CTRL,4,&v)&&v==0);
  assert(qrx_full_bus_read(&c,0,4,&v)&&v==0x40000);
  qrx_full_destroy(&c);
  printf("PASS QRX full68k bus mock: RAM / timer / IRQ / ROM overlay / aliases (NO CPU ISA emulated)\n");
  return 0;
}
