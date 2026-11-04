/* Direct3D through DirectDraw: version 5 (IDirect3D2 with TLVERTEX) with -DD3DV=5, version 7
 * with -DD3DV=7. Draws textured, transformed triangles, renders 300 frames and reports. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM a, LPARAM b) { return DefWindowProcA(h, m, a, b); }
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED line %d: %08lx\n", __LINE__, (unsigned long)hr_); return 1; } } while (0)
static IDirectDraw7 *dd7;
static IDirectDraw *dd;
static IDirectDrawSurface7 *prim7, *back7;
static D3DTLVERTEX v[6];
static void quad(float t)
{
    int i;
    float x[4] = { 100, 540, 540, 100 }, y[4] = { 80, 80, 400, 400 }, tu[4] = { 0, 1, 1, 0 }, tv[4] = { 0, 0, 1, 1 };
    int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (i = 0; i < 6; i++) {
        int k = order[i];
        v[i].sx = x[k] + t; v[i].sy = y[k]; v[i].sz = 0.5f; v[i].rhw = 1.f;
        v[i].color = 0xFFFFFFFFu; v[i].specular = 0; v[i].tu = tu[k]; v[i].tv = tv[k];
    }
}
int main(void)
{
    WNDCLASSA wc = { 0 };
    HWND h;
    DDSURFACEDESC2 d;
    DWORD t0, t1;
    int f;
    wc.lpfnWndProc = wp; wc.lpszClassName = "d3dl"; wc.hInstance = GetModuleHandleA(NULL);
    RegisterClassA(&wc);
    h = CreateWindowExA(0, "d3dl", "d3d", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, 0, 0, wc.hInstance, 0);
    CHECK(DirectDrawCreate(NULL, &dd, NULL));
    CHECK(IDirectDraw_QueryInterface(dd, &IID_IDirectDraw7, (void **)&dd7));
    CHECK(IDirectDraw7_SetCooperativeLevel(dd7, h, DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE));
    CHECK(IDirectDraw7_SetDisplayMode(dd7, 640, 480, 16, 0, 0));
    memset(&d, 0, sizeof d); d.dwSize = sizeof d; d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE; d.dwBackBufferCount = 1;
    CHECK(IDirectDraw7_CreateSurface(dd7, &d, &prim7, NULL));
    { DDSCAPS2 caps = { DDSCAPS_BACKBUFFER, 0, 0, { 0 } }; CHECK(IDirectDrawSurface7_GetAttachedSurface(prim7, &caps, &back7)); }
    /* a 16x16 checker texture in RGB565 */
    IDirectDrawSurface7 *tex;
    memset(&d, 0, sizeof d); d.dwSize = sizeof d; d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = d.dwHeight = 16; d.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    d.ddpfPixelFormat.dwSize = 32; d.ddpfPixelFormat.dwFlags = DDPF_RGB; d.ddpfPixelFormat.dwRGBBitCount = 16;
    d.ddpfPixelFormat.dwRBitMask = 0xF800; d.ddpfPixelFormat.dwGBitMask = 0x07E0; d.ddpfPixelFormat.dwBBitMask = 0x001F;
    CHECK(IDirectDraw7_CreateSurface(dd7, &d, &tex, NULL));
    memset(&d, 0, sizeof d); d.dwSize = sizeof d;
    CHECK(IDirectDrawSurface7_Lock(tex, NULL, &d, DDLOCK_WAIT, NULL));
    for (f = 0; f < 16; f++) {
        WORD *p = (WORD *)((BYTE *)d.lpSurface + f * d.lPitch);
        int x;
        for (x = 0; x < 16; x++) p[x] = ((x ^ f) & 4) ? 0xF800 : 0x07FF;
    }
    IDirectDrawSurface7_Unlock(tex, NULL);
#if D3DV == 7
    IDirect3D7 *d3d;
    IDirect3DDevice7 *dev;
    CHECK(IDirectDraw7_QueryInterface(dd7, &IID_IDirect3D7, (void **)&d3d));
    CHECK(IDirect3D7_CreateDevice(d3d, &IID_IDirect3DHALDevice, back7, &dev));
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, FALSE);
    IDirect3DDevice7_SetTexture(dev, 0, tex);
    t0 = GetTickCount();
    for (f = 0; f < 300; f++) {
        IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF202080u, 1.f, 0);
        IDirect3DDevice7_BeginScene(dev);
        quad((float)(f % 60));
        CHECK(IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, v, 6, 0));
        IDirect3DDevice7_EndScene(dev);
        IDirectDrawSurface7_Flip(prim7, NULL, DDFLIP_WAIT);
    }
#else
    IDirect3D2 *d3d;
    IDirect3DDevice2 *dev;
    IDirect3DTexture2 *t2;
    D3DTEXTUREHANDLE th;
    IDirect3DViewport2 *vp;
    IDirect3DMaterial2 *mat;
    D3DMATERIALHANDLE mh;
    D3DMATERIAL m;
    D3DVIEWPORT2 vd;
    D3DRECT clr = { 0, 0, 640, 480 };
    IDirectDrawSurface *back1;
    CHECK(IDirectDraw_QueryInterface(dd, &IID_IDirect3D2, (void **)&d3d));
    CHECK(IDirectDrawSurface7_QueryInterface(back7, &IID_IDirectDrawSurface, (void **)&back1));
    CHECK(IDirect3D2_CreateDevice(d3d, &IID_IDirect3DHALDevice, back1, &dev));
    CHECK(IDirectDrawSurface7_QueryInterface(tex, &IID_IDirect3DTexture2, (void **)&t2));
    CHECK(IDirect3DTexture2_GetHandle(t2, dev, &th));
    CHECK(IDirect3D2_CreateViewport(d3d, &vp, NULL));
    CHECK(IDirect3DDevice2_AddViewport(dev, vp));
    memset(&vd, 0, sizeof vd); vd.dwSize = sizeof vd; vd.dwWidth = 640; vd.dwHeight = 480;
    CHECK(IDirect3DViewport2_SetViewport2(vp, &vd));
    CHECK(IDirect3DDevice2_SetCurrentViewport(dev, vp));
    CHECK(IDirect3D2_CreateMaterial(d3d, &mat, NULL));
    memset(&m, 0, sizeof m); m.dwSize = sizeof m; m.diffuse.r = 0.125f; m.diffuse.g = 0.125f; m.diffuse.b = 0.5f; m.diffuse.a = 1.f;
    CHECK(IDirect3DMaterial2_SetMaterial(mat, &m));
    CHECK(IDirect3DMaterial2_GetHandle(mat, dev, &mh));
    CHECK(IDirect3DViewport2_SetBackground(vp, mh));
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, FALSE);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_TEXTUREHANDLE, th);
    t0 = GetTickCount();
    for (f = 0; f < 300; f++) {
        IDirect3DViewport2_Clear(vp, 1, &clr, D3DCLEAR_TARGET);
        IDirect3DDevice2_BeginScene(dev);
        quad((float)(f % 60));
        CHECK(IDirect3DDevice2_DrawPrimitive(dev, D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, v, 6, 0));
        IDirect3DDevice2_EndScene(dev);
        IDirectDrawSurface7_Flip(prim7, NULL, DDFLIP_WAIT);
    }
#endif
    t1 = GetTickCount();
    printf("D3D%d: 300 frames in %lu ms\n", D3DV, (unsigned long)(t1 - t0));
    return 0;
}
