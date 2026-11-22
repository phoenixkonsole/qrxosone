/* Shared scene for the graphics API tests: a checker texture and a grid of triangles,
 * so the frames read back from the GPU can be checked, and timing is comparable. */
#include <windows.h>
#include <stdio.h>
#define TEST_FRAMES 300
#define GRID 40
static unsigned char checker[64*64*4];
static void make_checker(void){
    for(int y=0;y<64;y++)for(int x=0;x<64;x++){
        unsigned char *p=checker+(y*64+x)*4;
        int on=((x/8)^(y/8))&1;
        p[0]=on?255:40;p[1]=on?200:40;p[2]=on?40:200;p[3]=255;
    }
}
static HWND make_window(int w,int h){
    WNDCLASSA wc;
    memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc=DefWindowProcA;wc.lpszClassName="qrxtest";wc.hInstance=GetModuleHandleA(NULL);
    RegisterClassA(&wc);
    return CreateWindowExA(0,"qrxtest","qrxtest",WS_POPUP|WS_VISIBLE,0,0,w,h,NULL,NULL,wc.hInstance,NULL);
}
static void report(const char *name,DWORD t0,DWORD t1){
    DWORD ms=t1-t0;
    printf("%s frames=%d ms=%lu fps=%lu tris=%d\n",name,TEST_FRAMES,(unsigned long)ms,ms?(unsigned long)(TEST_FRAMES*1000u/ms):0,GRID*GRID*2);
    fflush(stdout);
}
