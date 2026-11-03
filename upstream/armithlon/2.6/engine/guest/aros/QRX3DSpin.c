/* QRX3D smoke test for the Metal path: a rotating field of 200 triangles for a
 * fixed number of frames, then the frame count on the serial debug line. */
#define QRX_NATIVE_040
#include "../qrxvirt_3d.h"
#include <stdint.h>
#define FRAMES 20000
#define TRIS 200
static void say(const char *s){while(*s)*(volatile uint16_t*)0xdff030=0x100|*s++;}
static const int16_t sine[16]={0,12,23,30,32,30,23,12,0,-12,-23,-30,-32,-30,-23,-12};
int main(void){
    static QRXV3DVertex tri[3];
    say("[Q3D] start\n");
    if(!qrxv3d_available()){say("[Q3D] no door\n");return 10;}
    for(uint32_t f=0;f<FRAMES;f++){
        if(qrxv3d_clear(0xff07152b,65535)){say("[Q3D] clear failed\n");return 11;}
        for(uint32_t i=0;i<TRIS;i++){
            uint32_t a=(f/4+i)&15,b=(a+5)&15,c=(a+10)&15;
            int32_t cx=40+(int32_t)(i%20)*38,cy=40+(int32_t)(i/20)*38;
            uint32_t color=0xff000000u|(i*1277u&0xffffffu);
            tri[0]=(QRXV3DVertex){(cx+sine[a])<<16,(cy+sine[(a+4)&15])<<16,16384,color,0,0};
            tri[1]=(QRXV3DVertex){(cx+sine[b])<<16,(cy+sine[(b+4)&15])<<16,16384,color,65536,0};
            tri[2]=(QRXV3DVertex){(cx+sine[c])<<16,(cy+sine[(c+4)&15])<<16,16384,color,0,65536};
            if(qrxv3d_triangle(tri,QRXV3D_DEPTH_TEST|QRXV3D_DEPTH_WRITE)){say("[Q3D] triangle failed\n");return 12;}
        }
        if(qrxv3d_present()){say("[Q3D] present failed\n");return 13;}
    }
    say("[Q3D] done\n");
    return 0;
}
