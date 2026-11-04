/* Built twice: -DD3DV=9 and -DD3DV=8. Uses the C COM macros. */
#define COBJMACROS
#include "common.h"
#if D3DV==9
#include <d3d9.h>
#define NAME "direct3d9"
#define CREATE "Direct3DCreate9"
typedef IDirect3D9 ID3D; typedef IDirect3DDevice9 IDev; typedef IDirect3DVertexBuffer9 IVB; typedef IDirect3DTexture9 ITex;
typedef IDirect3DIndexBuffer9 IIB;
#define DDEV(x) IDirect3DDevice9_##x
#define DD3D(x) IDirect3D9_##x
#define DVB(x) IDirect3DVertexBuffer9_##x
#define DIB(x) IDirect3DIndexBuffer9_##x
#define DTEX(x) IDirect3DTexture9_##x
#else
#include <d3d8.h>
#define NAME "direct3d8"
#define CREATE "Direct3DCreate8"
typedef IDirect3D8 ID3D; typedef IDirect3DDevice8 IDev; typedef IDirect3DVertexBuffer8 IVB; typedef IDirect3DTexture8 ITex;
typedef IDirect3DIndexBuffer8 IIB;
#define DDEV(x) IDirect3DDevice8_##x
#define DD3D(x) IDirect3D8_##x
#define DVB(x) IDirect3DVertexBuffer8_##x
#define DIB(x) IDirect3DIndexBuffer8_##x
#define DTEX(x) IDirect3DTexture8_##x
#endif
typedef struct { float x,y,z; DWORD col; float u,v; } V;
#define FVF (D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1)
typedef struct { float x,y,z,rhw; DWORD col; } VR;
#define FVFR (D3DFVF_XYZRHW|D3DFVF_DIFFUSE)
static void set_identity(float *m){memset(m,0,64);m[0]=m[5]=m[10]=m[15]=1;}
int main(void){
    HWND wnd=make_window(640,480);
    HMODULE lib=LoadLibraryA(D3DV==9?"d3d9.dll":"d3d8.dll");
    if(!lib){puts("d3d: no dll");return 2;}
    ID3D *(WINAPI *create)(UINT)=(void*)GetProcAddress(lib,CREATE);
    if(!create){puts("d3d: no create");return 2;}
    ID3D *d3d=create(D3D_SDK_VERSION);
    if(!d3d){puts("d3d: create failed");return 3;}
    D3DPRESENT_PARAMETERS pp;memset(&pp,0,sizeof(pp));
    pp.BackBufferWidth=640;pp.BackBufferHeight=480;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferCount=1;
    pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=wnd;pp.Windowed=TRUE;pp.EnableAutoDepthStencil=TRUE;pp.AutoDepthStencilFormat=D3DFMT_D24X8;
    IDev *dev=NULL;
    HRESULT hr=DD3D(CreateDevice)(d3d,D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,wnd,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev);
    if(FAILED(hr)||!dev){printf("d3d: device %08lx\n",(unsigned long)hr);fflush(stdout);return 3;}
    make_checker();
    ITex *tex=NULL;
#if D3DV==9
    hr=DDEV(CreateTexture)(dev,64,64,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&tex,NULL);
#else
    hr=DDEV(CreateTexture)(dev,64,64,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&tex);
#endif
    if(FAILED(hr)){puts("d3d: texture failed");return 3;}
    D3DLOCKED_RECT lr;DTEX(LockRect)(tex,0,&lr,NULL,0);
    for(int i=0;i<64*64;i++){unsigned char *s=checker+i*4,*d=(unsigned char*)lr.pBits+i*4;d[0]=s[2];d[1]=s[1];d[2]=s[0];d[3]=s[3];}
    DTEX(UnlockRect)(tex,0);
    /* vertex and index buffers hold the grid */
    IVB *vb=NULL;IIB *ib=NULL;
    int vcount=(GRID+1)*(GRID+1),icount=GRID*GRID*6;
#if D3DV==9
    DDEV(CreateVertexBuffer)(dev,vcount*sizeof(V),0,FVF,D3DPOOL_MANAGED,&vb,NULL);
    DDEV(CreateIndexBuffer)(dev,icount*2,0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib,NULL);
    V *pv;DVB(Lock)(vb,0,0,(void**)&pv,0);
    WORD *pi;DIB(Lock)(ib,0,0,(void**)&pi,0);
#else
    DDEV(CreateVertexBuffer)(dev,vcount*sizeof(V),0,FVF,D3DPOOL_MANAGED,&vb);
    DDEV(CreateIndexBuffer)(dev,icount*2,0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib);
    V *pv;DVB(Lock)(vb,0,0,(BYTE**)&pv,0);
    WORD *pi;DIB(Lock)(ib,0,0,(BYTE**)&pi,0);
#endif
    for(int y=0;y<=GRID;y++)for(int x=0;x<=GRID;x++){
        V *v=&pv[y*(GRID+1)+x];
        v->x=(x-GRID/2)*0.05f;v->y=(y-GRID/2)*0.05f;v->z=0;v->col=0xFFFFFFFF;v->u=x*0.1f;v->v=y*0.1f;
    }
    for(int y=0;y<GRID;y++)for(int x=0;x<GRID;x++){
        WORD a=(WORD)(y*(GRID+1)+x),b=(WORD)(a+1),cc=(WORD)(a+GRID+1),d=(WORD)(cc+1);
        *pi++=a;*pi++=cc;*pi++=d;*pi++=a;*pi++=d;*pi++=b;
    }
    DVB(Unlock)(vb);DIB(Unlock)(ib);
    DDEV(SetRenderState)(dev,D3DRS_LIGHTING,FALSE);
    DDEV(SetRenderState)(dev,D3DRS_ZENABLE,TRUE);
    DDEV(SetTexture)(dev,0,(void*)tex);
    float proj[16],view[16],world[16];
    set_identity(view);view[14]=3;
    /* left-handed perspective, 1:1 from near 1 to far 100 */
    memset(proj,0,64);proj[0]=1;proj[5]=1.333f;proj[10]=100.f/99.f;proj[11]=1;proj[14]=-100.f/99.f;
    DDEV(SetTransform)(dev,D3DTS_PROJECTION,(void*)proj);
    DDEV(SetTransform)(dev,D3DTS_VIEW,(void*)view);
#if D3DV==9
    DDEV(SetFVF)(dev,FVF);
    DDEV(SetStreamSource)(dev,0,vb,0,sizeof(V));
    DDEV(SetIndices)(dev,ib);
#else
    DDEV(SetVertexShader)(dev,FVF);
    DDEV(SetStreamSource)(dev,0,vb,sizeof(V));
    DDEV(SetIndices)(dev,ib,0);
#endif
    DWORD t0=timeGetTime();
    for(int f=0;f<TEST_FRAMES;f++){
        DDEV(Clear)(dev,0,NULL,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF102040,1.0f,0);
        DDEV(BeginScene)(dev);
        set_identity(world);world[0]=1;
        float an=f*0.01f;
        /* rotate about y, tilted a little by a fixed y offset */
        float s=an-((int)(an/6.28f))*6.28f;
        float sn=s-s*s*s/6+s*s*s*s*s/120,cs=1-s*s/2+s*s*s*s/24;
        if(s>1.5f){sn=1;cs=0;}
        world[0]=cs;world[2]=-sn;world[8]=sn;world[10]=cs;
        DDEV(SetTransform)(dev,D3DTS_WORLD,(void*)world);
        DDEV(SetTexture)(dev,0,(void*)tex);
#if D3DV==9
        DDEV(SetFVF)(dev,FVF);
        DDEV(DrawIndexedPrimitive)(dev,D3DPT_TRIANGLELIST,0,0,vcount,0,icount/3);
#else
        DDEV(SetVertexShader)(dev,FVF);
        DDEV(DrawIndexedPrimitive)(dev,D3DPT_TRIANGLELIST,0,vcount,0,icount/3);
#endif
        /* pre-transformed triangle in front, from user memory */
        VR t[3]={{220,120,0.1f,1,0xFFFF0000},{420,120,0.1f,1,0xFF00FF00},{320,360,0.1f,1,0xFF0000FF}};
        DDEV(SetTexture)(dev,0,NULL);
#if D3DV==9
        DDEV(SetFVF)(dev,FVFR);
#else
        DDEV(SetVertexShader)(dev,FVFR);
#endif
        DDEV(DrawPrimitiveUP)(dev,D3DPT_TRIANGLELIST,1,t,sizeof(VR));
        DDEV(EndScene)(dev);
        DDEV(Present)(dev,NULL,NULL,NULL,NULL);
    }
    report(NAME,t0,timeGetTime());
    return 0;
}
