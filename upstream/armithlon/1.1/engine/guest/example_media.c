/* Guest-side minimal MMIO demonstration for a future 68k toolchain.
   This is NOT yet built into an AROS graphics.hidd, AHI or input.device. */
#include "qrxvirt_media.h"
void qrx_guest_example(void) {
    qrxv_rgb(25,25,0x11,0xaa,0xff);
    qrxv_rgb(26,25,0xff,0xaa,0x11);
    qrxv_present();
    qrxv_audio_s16(1000,-1000);
    if(qrxv_input_count()) (void)qrxv_input_pop();
}
