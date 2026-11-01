/* SPDX-License-Identifier: MIT
 * Native Windows Direct3D9 reference benchmark for QRX3D scene.
 * Deliberately NOT a proof that the QRX86 Win32 HLE supports Direct3D9.
 * Build using mingw-w64: x86_64-w64-mingw32-gcc -O2 -o qrx-d3d9.exe \
 *   qrx_d3d9_benchmark.c -ld3d9 -lgdi32 -luser32 -lm
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define W 800
#define H 450
#define NTRI 256
#define FVF (D3DFVF_XYZRHW|D3DFVF_DIFFUSE)
typedef struct {float x,y,z,rhw;D3DCOLOR color;} V;
static V tris[NTRI*3];
static LRESULT CALLBACK proc(HWND hwnd,UINT m,WPARAM w,LPARAM l){
  if(m==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProc(hwnd,m,w,l);
}
static void scene(void){
  for(int i=0;i<240;i++){
    int col=i%20,row=i/20;
    float x=col*40.f+2.f,y=row*37.f+2.f,z=.35f+(i%5)*.06f;
    unsigned r=(col*41u+row*17u+64u)%256u;
    unsigned g=(row*59u+col*7u+80u)%256u;
    unsigned b=(i*11u+125u)%256u;
    const float vx[]={x+3,x+36,x+18},vy[]={y+33,y+33,y+2};
    for(int j=0;j<3;j++)tris[i*3+j]=(V){vx[j],vy[j],z,1.f,D3DCOLOR_ARGB(255,r,g,b)};
  }
  for(int i=0;i<16;i++){
    int col=(i*14)%20,row=(i*14)/20;
    float x=col*40.f+7.f,y=row*37.f+7.f,z=.35f+((i*14)%5)*.06f;
    const float vx[]={x+3,x+36,x+18},vy[]={y+33,y+33,y+2};
    for(int j=0;j<3;j++)tris[(240+i)*3+j]=(V){vx[j],vy[j],z,1.f,D3DCOLOR_ARGB(107,255,173,31)};
  }
}
static int snapshot(IDirect3DDevice9* device,const char* path){
  IDirect3DSurface9 *rt=NULL,*sys=NULL;
  if(FAILED(IDirect3DDevice9_GetRenderTarget(device,0,&rt)))return 1;
  if(FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(device,W,H,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&sys,NULL)))goto fail;
  if(FAILED(IDirect3DDevice9_GetRenderTargetData(device,rt,sys)))goto fail;
  D3DLOCKED_RECT lr;
  if(FAILED(IDirect3DSurface9_LockRect(sys,&lr,NULL,D3DLOCK_READONLY)))goto fail;
  FILE *f=fopen(path,"wb");
  if(!f){IDirect3DSurface9_UnlockRect(sys);goto fail;}
  fprintf(f,"P6\n%d %d\n255\n",W,H);
  for(int y=0;y<H;y++){
    const unsigned char *row=(const unsigned char *)lr.pBits+y*lr.Pitch;
    for(int x=0;x<W;x++){
      unsigned char rgb[3]={row[x*4+2],row[x*4+1],row[x*4]};
      fwrite(rgb,1,3,f);
    }
  }
  fclose(f);IDirect3DSurface9_UnlockRect(sys);
  IDirect3DSurface9_Release(sys);IDirect3DSurface9_Release(rt);return 0;
fail:if(sys)IDirect3DSurface9_Release(sys);IDirect3DSurface9_Release(rt);return 1;
}
int main(int argc,char**argv){
  int frames=argc>1?atoi(argv[1]):90,warmup=argc>2?atoi(argv[2]):10;
  const char *image=argc>3?argv[3]:"direct3d9-bench.ppm";
  if(frames<1||frames>2000||warmup<0||warmup>500)return 2;
  WNDCLASSA wc={0};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleA(NULL);wc.lpszClassName="QRXD3D9Bench";
  if(!RegisterClassA(&wc))return 2;
  HWND window=CreateWindowA(wc.lpszClassName,"QRX D3D9 Reference (native)",WS_OVERLAPPEDWINDOW,80,80,W+20,H+45,0,0,wc.hInstance,0);
  if(!window)return 3;ShowWindow(window,SW_SHOW);
  IDirect3D9 *d3d=Direct3DCreate9(D3D_SDK_VERSION);if(!d3d)return 3;
  D3DPRESENT_PARAMETERS p={0};p.Windowed=TRUE;p.SwapEffect=D3DSWAPEFFECT_DISCARD;
  p.BackBufferWidth=W;p.BackBufferHeight=H;p.BackBufferFormat=D3DFMT_X8R8G8B8;
  p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *device=NULL;
  if(FAILED(IDirect3D9_CreateDevice(d3d,D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,
     D3DCREATE_SOFTWARE_VERTEXPROCESSING,&p,&device))){fprintf(stderr,"Direct3D9 device unavailable\n");return 3;}
  scene();
  IDirect3DDevice9_SetFVF(device,FVF);
  IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,FALSE);
  IDirect3DDevice9_SetRenderState(device,D3DRS_LIGHTING,FALSE);
  LARGE_INTEGER freq,start,end;QueryPerformanceFrequency(&freq);
  int snaprc=0;
  for(int i=-warmup;i<frames;i++){
    if(i==0)QueryPerformanceCounter(&start);
    IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,D3DCOLOR_XRGB(17,24,39),1.f,0);
    if(FAILED(IDirect3DDevice9_BeginScene(device)))return 4;
    IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHABLENDENABLE,FALSE);
    IDirect3DDevice9_DrawPrimitiveUP(device,D3DPT_TRIANGLELIST,240,tris,sizeof(V));
    IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHABLENDENABLE,TRUE);
    IDirect3DDevice9_SetRenderState(device,D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(device,D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_DrawPrimitiveUP(device,D3DPT_TRIANGLELIST,16,tris+240*3,sizeof(V));
    IDirect3DDevice9_EndScene(device);
    if(i==frames-1)snaprc=snapshot(device,image);
    if(FAILED(IDirect3DDevice9_Present(device,NULL,NULL,NULL,NULL)))return 5;
  }
  QueryPerformanceCounter(&end);
  double ms=(end.QuadPart-start.QuadPart)*1000.0/freq.QuadPart;
  printf("{\"api\":\"direct3d9-native\",\"kind\":\"native_d3d9_present_walltime\",\"frames\":%d,\"warmup\":%d,\"width\":800,\"height\":450,\"triangles_per_frame\":256,\"elapsed_ms\":%.3f,\"fps\":%.3f,\"screenshot\":\"%s\",\"status\":\"%s\"}\n",
         frames,warmup,ms,frames*1000.0/ms,image,snaprc?"FAIL_SNAPSHOT":"PASS");
  IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(window);
  return snaprc?5:0;
}
