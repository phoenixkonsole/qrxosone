/* QRX OS One 22.5.12 - QRXClock: a small analog/digital clock for Win32.
 *
 * ReactOS 0.4.15 ships no stand-alone clock program (its clock lives inside the
 * Date/Time control panel), so QRX provides this open-source Win32 clock.  It is
 * compiled with i686-w64-mingw32-gcc into an ordinary PE/i386 executable and is
 * run unmodified by qrx-win32 - it is a real Windows program, not a QRX stub.
 *
 *   i686-w64-mingw32-gcc -O2 -mwindows -o qrxclock.exe qrxclock.c -lgdi32
 *
 * Menu: Ansicht/View -> Analog | Digital, Sekunden/Seconds; Hilfe -> Info.
 */
#include <windows.h>
#include <math.h>

#define IDM_ANALOG  101
#define IDM_DIGITAL 102
#define IDM_SECONDS 103
#define IDM_ABOUT   104
#define IDM_EXIT    105

static int g_digital = 0, g_seconds = 1;

static void hand(HDC dc, int cx, int cy, double angle, int len, int width, COLORREF col)
{
    POINT p[4];
    double s = sin(angle), c = cos(angle);
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HBRUSH br = CreateSolidBrush(col);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    p[0].x = cx + (int)(s * len);            p[0].y = cy - (int)(c * len);
    p[1].x = cx + (int)(c * width);          p[1].y = cy + (int)(s * width);
    p[2].x = cx - (int)(s * len / 6);        p[2].y = cy + (int)(c * len / 6);
    p[3].x = cx - (int)(c * width);          p[3].y = cy - (int)(s * width);
    Polygon(dc, p, 4);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(br);
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    RECT rc;
    SYSTEMTIME st;
    HDC dc = BeginPaint(hwnd, &ps);
    int w, h, cx, cy, r, i;
    GetClientRect(hwnd, &rc);
    w = rc.right; h = rc.bottom;
    GetLocalTime(&st);
    FillRect(dc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
    if (g_digital) {
        WCHAR buf[32];
        HFONT f = CreateFontW(-25, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, VARIABLE_PITCH, L"Arial");
        HGDIOBJ of = SelectObject(dc, f);
        if (g_seconds) wsprintfW(buf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        else wsprintfW(buf, L"%02d:%02d", st.wHour, st.wMinute);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 0, 128));
        DrawTextW(dc, buf, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
        DeleteObject(f);
    } else {
        HBRUSH face = CreateSolidBrush(RGB(255, 255, 240));
        HGDIOBJ ob = SelectObject(dc, face);
        cx = w / 2; cy = h / 2; r = (w < h ? w : h) / 2 - 8;
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
        SelectObject(dc, ob);
        DeleteObject(face);
        for (i = 0; i < 60; i++) {
            double a = i * 3.14159265358979 / 30.0;
            int inner = i % 5 ? r - 4 : r - 10;
            MoveToEx(dc, cx + (int)(sin(a) * inner), cy - (int)(cos(a) * inner), NULL);
            LineTo(dc, cx + (int)(sin(a) * (r - 1)), cy - (int)(cos(a) * (r - 1)));
        }
        hand(dc, cx, cy, ((st.wHour % 12) + st.wMinute / 60.0) * 3.14159265358979 / 6.0, r * 5 / 10, 4, RGB(0, 0, 0));
        hand(dc, cx, cy, (st.wMinute + st.wSecond / 60.0) * 3.14159265358979 / 30.0, r * 8 / 10, 3, RGB(0, 0, 0));
        if (g_seconds) {
            double a = st.wSecond * 3.14159265358979 / 30.0;
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(200, 0, 0));
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, cx, cy, NULL);
            LineTo(dc, cx + (int)(sin(a) * (r - 6)), cy - (int)(cos(a) * (r - 6)));
            SelectObject(dc, op);
            DeleteObject(pen);
        }
    }
    EndPaint(hwnd, &ps);
}

static void update_menu(HWND hwnd)
{
    HMENU m = GetMenu(hwnd);
    CheckMenuItem(m, IDM_ANALOG, g_digital ? MF_UNCHECKED : MF_CHECKED);
    CheckMenuItem(m, IDM_DIGITAL, g_digital ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m, IDM_SECONDS, g_seconds ? MF_CHECKED : MF_UNCHECKED);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 1000, NULL);
        update_menu(hwnd);
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_ANALOG: g_digital = 0; break;
        case IDM_DIGITAL: g_digital = 1; break;
        case IDM_SECONDS: g_seconds = !g_seconds; break;
        case IDM_ABOUT:
            MessageBoxW(hwnd, L"QRXClock 1.0\nWin32-Uhr fuer QRX OS One\n(kompiliert mit MinGW, laeuft in qrx-win32)", L"Info", MB_OK);
            return 0;
        case IDM_EXIT: DestroyWindow(hwnd); return 0;
        }
        update_menu(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSW wc;
    HMENU bar = CreateMenu(), view = CreatePopupMenu(), help = CreatePopupMenu();
    HWND hwnd;
    MSG msg;
    (void)prev; (void)cmd;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"QRXClock";
    RegisterClassW(&wc);
    AppendMenuW(view, MF_STRING, IDM_ANALOG, L"&Analog");
    AppendMenuW(view, MF_STRING, IDM_DIGITAL, L"&Digital");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, IDM_SECONDS, L"&Sekunden");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, IDM_EXIT, L"&Beenden");
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&Info");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&Ansicht");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Hilfe");
    hwnd = CreateWindowW(L"QRXClock", L"Uhr", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 200, 220, NULL, bar, hi, NULL);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
