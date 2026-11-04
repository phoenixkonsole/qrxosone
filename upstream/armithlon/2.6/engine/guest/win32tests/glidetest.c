#include "common.h"
typedef struct { float x,y,z,r,g,b,ooz,a,oow,sow,tow,oow0,sow1,tow1,oow1; } GrVertex;
typedef struct { int smallLod,largeLod,aspect,format; void *data; } GrTexInfo;
#define G(ret,name,args,size) typedef ret (__stdcall *p_##name)args; static p_##name name;
G(void,grGlideInit,(void),0) G(int,grSstQueryHardware,(void*),4) G(void,grSstSelect,(int),4)
G(int,grSstWinOpen,(unsigned,int,int,int,int,int,int),28) G(void,grBufferClear,(unsigned,unsigned,unsigned),12)
G(void,grBufferSwap,(int),4) G(void,grDrawTriangle,(const GrVertex*,const GrVertex*,const GrVertex*),12)
G(void,grDepthBufferMode,(int),4) G(void,grDepthBufferFunction,(int),4) G(void,grDepthMask,(int),4)
G(void,grColorCombine,(int,int,int,int,int),20) G(void,grTexDownloadMipMap,(int,unsigned,unsigned,GrTexInfo*),16)
G(void,grTexSource,(int,unsigned,unsigned,GrTexInfo*),16) G(void,grGlideShutdown,(void),0)
static unsigned short tex565[256*256];
static void *imp(HMODULE m,const char *n){void *p=(void*)GetProcAddress(m,n);if(!p){printf("glide: missing %s\n",n);fflush(stdout);ExitProcess(2);}return p;}
#define L(name,size) name=(p_##name)imp(m,"_" #name "@" #size)
int main(void){
    make_window(640,480);
    HMODULE m=LoadLibraryA("glide2x.dll");
    if(!m){puts("glide: no dll");return 2;}
    L(grGlideInit,0);L(grSstQueryHardware,4);L(grSstSelect,4);L(grSstWinOpen,28);L(grBufferClear,12);L(grBufferSwap,4);L(grDrawTriangle,12);
    L(grDepthBufferMode,4);L(grDepthBufferFunction,4);L(grDepthMask,4);L(grColorCombine,20);L(grTexDownloadMipMap,16);L(grTexSource,16);L(grGlideShutdown,0);
    static char hw[256];
    grGlideInit();grSstQueryHardware(hw);grSstSelect(0);
    if(!grSstWinOpen(0,7,0,0,0,2,1)){puts("glide: open failed");return 3;}
    for(int y=0;y<256;y++)for(int x=0;x<256;x++)tex565[y*256+x]=((x/32)^(y/32))&1?0xFFE0:0x001F;
    GrTexInfo ti={0,0,3,0xA,tex565};
    grTexDownloadMipMap(0,0,3,&ti);grTexSource(0,0,3,&ti);
    grDepthBufferMode(2);grDepthBufferFunction(1);grDepthMask(1);
    DWORD t0=timeGetTime();
    for(int f=0;f<TEST_FRAMES;f++){
        grBufferClear(0x00200040,0,0xFFFF);
        grColorCombine(3,1,0,1,0);   /* scale other (texture) by local */
        for(int y=0;y<GRID;y++)for(int x=0;x<GRID;x++){
            float x0=70+x*12.5f+f%20,y0=40+y*10.f,x1=x0+12.5f,y1=y0+10.f;
            GrVertex a={x0,y0,0,255,255,255,30000,255,1,x*6.f,y*6.f,1,0,0,0},b=a,cc=a,d=a;
            b.x=x1;b.sow=(x+1)*6.f;
            cc.x=x1;cc.y=y1;cc.sow=(x+1)*6.f;cc.tow=(y+1)*6.f;
            d.y=y1;d.tow=(y+1)*6.f;
            grDrawTriangle(&a,&b,&cc);grDrawTriangle(&a,&cc,&d);
        }
        grColorCombine(0,0,0,0,0);    /* iterated colour */
        {
            GrVertex a={220,120,0,255,0,0,10000,255,1},b={420,120,0,0,255,0,10000,255,1},cc={320,360,0,0,0,255,10000,255,1};
            grDrawTriangle(&a,&b,&cc);
        }
        grBufferSwap(1);
    }
    report("glide",t0,timeGetTime());
    grGlideShutdown();
    return 0;
}
