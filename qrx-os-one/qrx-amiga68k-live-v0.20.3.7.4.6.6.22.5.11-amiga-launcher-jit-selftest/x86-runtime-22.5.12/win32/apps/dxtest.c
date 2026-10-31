/* QRX OS One 22.5.12 - dxtest: DirectX 3 test program (DirectDraw, DirectSound,
 * DirectInput) built with MinGW into a normal Win32 PE.  It uses the interfaces
 * the way games of 1996-1998 do (GTA 1 era): exclusive full screen 640x480x8,
 * primary surface with one back buffer, palette, colour-keyed sprite blits,
 * locked drawing, GetDC text, a looping sound buffer and the keyboard device.
 *   dxtest [frames]      (default 60; Esc quits early)
 * Writes dxtest.log next to the program with one line per check. */
#define DIRECTDRAW_VERSION 0x0300
#define DIRECTSOUND_VERSION 0x0300
#define DIRECTINPUT_VERSION 0x0300
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <dsound.h>
#include <dinput.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *lg;
static int fails;
static void check(const char *what, HRESULT hr)
{
    fprintf(lg, "%s %s (hr=%08lx)\n", SUCCEEDED(hr) ? "OK  " : "FAIL", what, (unsigned long)hr);
    if (FAILED(hr)) fails++;
    fflush(lg);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    HWND hwnd;
    LPDIRECTDRAW dd;
    LPDIRECTDRAWSURFACE prim, back, sprite;
    LPDIRECTDRAWPALETTE pal;
    DDSURFACEDESC sd;
    DDSCAPS caps;
    DDCOLORKEY ck;
    DDBLTFX fx;
    PALETTEENTRY pe[256];
    LPDIRECTSOUND ds;
    LPDIRECTSOUNDBUFFER sb;
    DSBUFFERDESC bd;
    WAVEFORMATEX wf;
    LPDIRECTINPUT di;
    LPDIRECTINPUTDEVICE kbd;
    BYTE keys[256];
    MSG msg;
    int frames = cmd && atoi(cmd) > 0 ? atoi(cmd) : 60, f, i, x, y;
    DWORD t0, play0 = 0, play1 = 0, wr;
    RECT r;
    HRESULT hr;
    char path[MAX_PATH], *sl;

    GetModuleFileNameA(NULL, path, sizeof(path));
    sl = strrchr(path, '\\'); if (!sl) sl = strrchr(path, '/');
    strcpy(sl ? sl + 1 : path, "dxtest.log");
    lg = fopen(path, "w");
    if (!lg) lg = stderr;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.lpszClassName = "QRXDXTest";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(WS_EX_TOPMOST, "QRXDXTest", "QRX DirectX test", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, NULL, NULL, hi, NULL);
    check("CreateWindowEx", hwnd ? DD_OK : E_FAIL);

    /* ---------------- DirectDraw */
    check("DirectDrawCreate", DirectDrawCreate(NULL, &dd, NULL));
    check("SetCooperativeLevel exclusive", IDirectDraw_SetCooperativeLevel(dd, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN));
    check("SetDisplayMode 640x480x8", IDirectDraw_SetDisplayMode(dd, 640, 480, 8));
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd); sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    sd.dwBackBufferCount = 1;
    check("CreateSurface primary+back", IDirectDraw_CreateSurface(dd, &sd, &prim, NULL));
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    check("GetAttachedSurface back", IDirectDrawSurface_GetAttachedSurface(prim, &caps, &back));
    for (i = 0; i < 256; i++) { pe[i].peRed = (BYTE)i; pe[i].peGreen = (BYTE)(i < 128 ? i * 2 : 255 - (i - 128) * 2); pe[i].peBlue = (BYTE)(255 - i); pe[i].peFlags = 0; }
    pe[0].peRed = pe[0].peGreen = pe[0].peBlue = 0;
    pe[255].peRed = pe[255].peGreen = pe[255].peBlue = 255;
    check("CreatePalette", IDirectDraw_CreatePalette(dd, DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, NULL));
    check("SetPalette", IDirectDrawSurface_SetPalette(prim, pal));
    /* 32x32 sprite: a ring in colour 200 on transparent colour 0 */
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd); sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN; sd.dwWidth = 32; sd.dwHeight = 32;
    check("CreateSurface sprite", IDirectDraw_CreateSurface(dd, &sd, &sprite, NULL));
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    hr = IDirectDrawSurface_Lock(sprite, NULL, &sd, DDLOCK_WAIT, NULL);
    check("Lock sprite", hr);
    if (SUCCEEDED(hr)) {
        for (y = 0; y < 32; y++) for (x = 0; x < 32; x++) {
            int d2 = (x - 16) * (x - 16) + (y - 16) * (y - 16);
            ((BYTE *)sd.lpSurface)[y * sd.lPitch + x] = (d2 < 225 && d2 > 64) ? 200 : 0;
        }
        IDirectDrawSurface_Unlock(sprite, NULL);
        fprintf(lg, "INFO sprite pitch %ld\n", (long)sd.lPitch);
    }
    ck.dwColorSpaceLowValue = ck.dwColorSpaceHighValue = 0;
    check("SetColorKey", IDirectDrawSurface_SetColorKey(sprite, DDCKEY_SRCBLT, &ck));

    /* ---------------- DirectSound: 1 s, 22 kHz 8-bit mono square wave, looping */
    check("DirectSoundCreate", DirectSoundCreate(NULL, &ds, NULL));
    check("DS SetCooperativeLevel", IDirectSound_SetCooperativeLevel(ds, hwnd, DSSCL_NORMAL));
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = 22050; wf.wBitsPerSample = 8;
    wf.nBlockAlign = 1; wf.nAvgBytesPerSec = 22050;
    memset(&bd, 0, sizeof(bd));
    bd.dwSize = sizeof(bd); bd.dwFlags = DSBCAPS_CTRLVOLUME; bd.dwBufferBytes = 22050; bd.lpwfxFormat = &wf;
    check("CreateSoundBuffer", IDirectSound_CreateSoundBuffer(ds, &bd, &sb, NULL));
    {
        void *p1, *p2; DWORD n1, n2;
        hr = IDirectSoundBuffer_Lock(sb, 0, 22050, &p1, &n1, &p2, &n2, 0);
        check("SoundBuffer Lock", hr);
        if (SUCCEEDED(hr)) { for (i = 0; i < (int)n1; i++) ((BYTE *)p1)[i] = (i / 25) & 1 ? 0xC0 : 0x40; IDirectSoundBuffer_Unlock(sb, p1, n1, p2, n2); }
    }
    check("SoundBuffer Play", IDirectSoundBuffer_Play(sb, 0, 0, DSBPLAY_LOOPING));
    IDirectSoundBuffer_GetCurrentPosition(sb, &play0, &wr);

    /* ---------------- DirectInput keyboard */
    check("DirectInputCreate", DirectInputCreateA(hi, DIRECTINPUT_VERSION, &di, NULL));
    check("CreateDevice keyboard", IDirectInput_CreateDevice(di, &GUID_SysKeyboard, &kbd, NULL));
    check("SetDataFormat", IDirectInputDevice_SetDataFormat(kbd, &c_dfDIKeyboard));
    check("DI SetCooperativeLevel", IDirectInputDevice_SetCooperativeLevel(kbd, hwnd, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND));
    check("Acquire", IDirectInputDevice_Acquire(kbd));

    /* ---------------- frame loop */
    t0 = GetTickCount();
    for (f = 0; f < frames; f++) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        memset(&fx, 0, sizeof(fx)); fx.dwSize = sizeof(fx); fx.dwFillColor = 0;
        hr = IDirectDrawSurface_Blt(back, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        if (f == 0) check("Blt colorfill", hr);
        memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
        hr = IDirectDrawSurface_Lock(back, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
        if (f == 0) { check("Lock back buffer", hr); fprintf(lg, "INFO back %lux%lu pitch %ld bpp %lu\n", (unsigned long)sd.dwWidth, (unsigned long)sd.dwHeight, (long)sd.lPitch, (unsigned long)sd.ddpfPixelFormat.dwRGBBitCount); }
        if (SUCCEEDED(hr)) {
            for (y = 100; y < 380; y++) {     /* colour bars that scroll with the frame number */
                BYTE *row = (BYTE *)sd.lpSurface + y * sd.lPitch;
                for (x = 40; x < 600; x++) row[x] = (BYTE)(1 + ((x + f * 8) / 4) % 254);
            }
            IDirectDrawSurface_Unlock(back, NULL);
        }
        for (i = 0; i < 8; i++) {
            r.left = 0; r.top = 0; r.right = 32; r.bottom = 32;
            hr = IDirectDrawSurface_BltFast(back, 60 + i * 64 + (f * 4) % 64, 40 + i * 4, sprite, &r, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT);
            if (f == 0 && i == 0) check("BltFast colour key", hr);
        }
        {
            HDC dc;
            hr = IDirectDrawSurface_GetDC(back, &dc);
            if (f == 0) check("GetDC", hr);
            if (SUCCEEDED(hr)) {
                char t[64];
                SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
                sprintf(t, "QRX DirectDraw frame %d", f);
                TextOutA(dc, 40, 420, t, (int)strlen(t));
                IDirectDrawSurface_ReleaseDC(back, dc);
            }
        }
        hr = IDirectDrawSurface_Flip(prim, NULL, DDFLIP_WAIT);
        if (f == 0) check("Flip", hr);
        if (SUCCEEDED(IDirectInputDevice_GetDeviceState(kbd, sizeof(keys), keys)) && (keys[DIK_ESCAPE] & 0x80)) { fprintf(lg, "INFO Esc pressed in frame %d\n", f); break; }
    }
    IDirectSoundBuffer_GetCurrentPosition(sb, &play1, &wr);
    fprintf(lg, "INFO %d frames in %lu ms, sound play cursor %lu -> %lu\n", f, (unsigned long)(GetTickCount() - t0), (unsigned long)play0, (unsigned long)play1);
    check("sound play cursor advances", play1 != play0 ? DD_OK : E_FAIL);

    IDirectInputDevice_Unacquire(kbd);
    IDirectInputDevice_Release(kbd);
    IDirectInput_Release(di);
    IDirectSoundBuffer_Stop(sb);
    IDirectSoundBuffer_Release(sb);
    IDirectSound_Release(ds);
    IDirectDrawSurface_Release(sprite);
    IDirectDrawPalette_Release(pal);
    IDirectDrawSurface_Release(prim);
    check("RestoreDisplayMode", IDirectDraw_RestoreDisplayMode(dd));
    IDirectDraw_Release(dd);
    fprintf(lg, "RESULT %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    fclose(lg);
    DestroyWindow(hwnd);
    return fails ? 1 : 0;
}
