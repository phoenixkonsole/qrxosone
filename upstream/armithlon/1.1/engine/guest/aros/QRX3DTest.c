/* Compile as m68k, execute ONLY on QRXvirt with QRX3D ABI 0.6.
 * Not a classic Amiga/FS-UAE or a shipped AROS display driver. */
#include "../qrxvirt_3d.h"
#include <stdint.h>
static QRXV3DVertex verts[3] = {
    { 150<<16,  65<<16, 16384, 0xffff0000u,0,0 },
    { 640<<16,  95<<16, 16384, 0xff00ff00u,65536,0 },
    { 325<<16, 380<<16, 16384, 0xff0000ffu,0,65536 }
};
int main(void){
    if(!qrxv3d_available())return 10;
    if(qrxv3d_clear(0xff07152b,65535))return 11;
    if(qrxv3d_triangle(verts,QRXV3D_DEPTH_TEST|QRXV3D_DEPTH_WRITE))return 12;
    if(qrxv3d_present())return 13;
    return 0;
}
