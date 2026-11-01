#include "qrxvirt.h"
#include <string.h>

static bool set_word(uint8_t *ram, size_t size, size_t *at, uint16_t val) {
    if (*at > size || size-*at < 2) return false;
    ram[(*at)++]=(uint8_t)(val>>8); ram[(*at)++]=(uint8_t)val;
    return true;
}
static bool set_long(uint8_t *ram, size_t size, size_t *at, uint32_t val) {
    return set_word(ram,size,at,(uint16_t)(val>>16)) &&
           set_word(ram,size,at,(uint16_t)val);
}
bool qrx_build_boot_monitor(uint8_t *ram, size_t size) {
    if (!ram || size < 1024) return false;
    memset(ram,0,size);
    size_t at=0;
    if (!set_long(ram,size,&at,(uint32_t)(size & ~1u)) ||
        !set_long(ram,size,&at,0x100u)) return false;
    at=0x100;
    /* LEA QRX UART,A0; MOVE.B #ascii,(A0) * N; quit via MMIO shutdown. */
    if (!set_word(ram,size,&at,0x41f9) ||
        !set_long(ram,size,&at,QRX_IO_BASE+0x0c)) return false;
    static const char banner[]="QRX68100 qrxvirt board READY\n";
    for (size_t i=0; banner[i]; ++i) {
        if (!set_word(ram,size,&at,0x10bc) ||
            !set_word(ram,size,&at,(unsigned char)banner[i])) return false;
    }
    if (!set_word(ram,size,&at,0x41f9) ||
        !set_long(ram,size,&at,QRX_IO_BASE+0x1c) ||
        !set_word(ram,size,&at,0x20bc) ||
        !set_long(ram,size,&at,1)) return false;
    return true;
}
