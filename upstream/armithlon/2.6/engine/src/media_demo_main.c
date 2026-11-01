#include "qrx_demo.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static uint8_t *last_frame;
static void capture(void *ctx,const uint8_t *pixels,uint32_t w,uint32_t h,uint32_t pitch) {
    (void)ctx;
    if(w!=QRX_GFX_WIDTH||h!=QRX_GFX_HEIGHT||pitch!=QRX_GFX_PITCH)abort();
    memcpy(last_frame,pixels,(size_t)h*pitch);
}
static int write_ppm(const char *filename){
    FILE *f=fopen(filename,"wb"); if(!f)return 0;
    fprintf(f,"P6\n%d %d\n255\n",QRX_GFX_WIDTH,QRX_GFX_HEIGHT);
    for(size_t i=0;i<(size_t)QRX_GFX_WIDTH*QRX_GFX_HEIGHT;i++)
        fwrite(&last_frame[i*4+1],1,3,f);
    return fclose(f)==0;
}
int main(int argc,char **argv){
    const char *file=argc==3 && strcmp(argv[1],"--ppm")==0?argv[2]:NULL;
    last_frame=calloc((size_t)QRX_GFX_HEIGHT,QRX_GFX_PITCH);
    QRXDemo *demo=qrx_demo_create(capture,NULL);
    if(!demo||!last_frame){fputs("allocation failure\n",stderr);return 1;}
    qrx_media_set_mouse(qrx_demo_media(demo),400,225,0);
    qrx_media_push_input(qrx_demo_media(demo),(QRX_INPUT_KEY_DOWN<<24)|(12<<16));
    for(int i=0;i<5;i++)if(!qrx_demo_frame(demo)){fputs("68k demo execution failed\n",stderr);return 2;}
    int16_t pcm[1600];size_t have=qrx_media_pull_audio(qrx_demo_media(demo),pcm,800);
    printf("native 68k media demo: frames=%u input=0x%08x audio_frames=%zu first_pixel=%02x%02x%02x\n",
      qrx_demo_frames(demo),qrx_demo_last_input(demo),have,last_frame[1],last_frame[2],last_frame[3]);
    if(have!=800||qrx_demo_frames(demo)!=5||!last_frame[3])return 3;
    if(file&&!write_ppm(file))return 4;
    qrx_demo_destroy(demo);free(last_frame);
    return 0;
}
