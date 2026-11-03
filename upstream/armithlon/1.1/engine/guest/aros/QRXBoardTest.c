/* AROS/68k developer diagnostic (not a graphics.hidd driver yet).
 * Build on the proper AROS m68k cross toolchain and run ONLY on qrxvirt.
 * The QRX-board check itself reads MMIO and will fault on non-QRX hardware.
 */
#include <proto/dos.h>
#include "../qrxvirt_gpu.h"
int main(void) {
    uint32_t rc;
    if (!qrxv_gpu_available()) {
        Printf("QRXBoardTest: no QRXGPU v0.5 board detected\n");
        return 20;
    }
    Printf("QRXGPU v0.5 detected (%ld x %ld)\n",(long)QRXV_GFX_W,(long)QRXV_GFX_H);
    rc=qrxv_gpu_clear(0x00112255u);
    if (!rc)rc=qrxv_gpu_fill(40,40,280,140,0x00f0ac28u);
    if (!rc)rc=qrxv_gpu_present();
    Printf("QRXGPU result %ld\n",(long)rc);
    return rc?20:0;
}
