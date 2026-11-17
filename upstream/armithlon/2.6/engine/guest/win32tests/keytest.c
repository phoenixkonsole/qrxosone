/* keytest: DirectInput's buffered keyboard data, as SDL and many games read it. Prints every key
 * change. A key tapped between two polls must show as a press and a release, and keys held
 * together must all show as held (with -s keytest.txt in qrx-win32's headless mode). */
#define DIRECTINPUT_VERSION 0x0500
#include <windows.h>
#include <dinput.h>
#include <stdio.h>

int main(void)
{
    LPDIRECTINPUTA di; LPDIRECTINPUTDEVICEA kb;
    DIPROPDWORD buf = { { sizeof(DIPROPDWORD), sizeof(DIPROPHEADER), 0, DIPH_DEVICE }, 64 };
    HWND h = CreateWindowA("STATIC", "keytest", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 200, 100, 0, 0, 0, 0);
    int i, held_max = 0, held = 0, events = 0;
    if (DirectInputCreateA(GetModuleHandleA(0), 0x0500, &di, 0) || IDirectInput_CreateDevice(di, &GUID_SysKeyboard, &kb, 0)) { printf("keytest: no DirectInput\n"); return 1; }
    IDirectInputDevice_SetDataFormat(kb, &c_dfDIKeyboard);
    IDirectInputDevice_SetCooperativeLevel(kb, h, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
    IDirectInputDevice_SetProperty(kb, DIPROP_BUFFERSIZE, &buf.diph);
    IDirectInputDevice_Acquire(kb);
    for (i = 0; i < 40; i++) {
        MSG m; DIDEVICEOBJECTDATA d[64]; DWORD n = 64, k;
        while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
        Sleep(100);
        if (IDirectInputDevice_GetDeviceData(kb, sizeof d[0], d, &n, 0) >= 0)
            for (k = 0; k < n; k++) {
                held += (d[k].dwData & 0x80) ? 1 : -1;
                if (held > held_max) held_max = held;
                events++;
                printf("key %02lx %s\n", d[k].dwOfs, (d[k].dwData & 0x80) ? "down" : "up");
            }
    }
    printf("keytest: %d events, at most %d keys held at once\n", events, held_max);
    return 0;
}
