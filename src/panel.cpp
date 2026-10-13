#include "panel.h"
#include <windows.h>
#include <mutex>
namespace panel {
namespace {
std::mutex mutex;
std::wstring text=L"DGMP: initializing...";
HWND window=nullptr;
HWND GameWindow() {
    struct Context { DWORD pid; HWND result; } c{GetCurrentProcessId(),nullptr};
    EnumWindows([](HWND h,LPARAM p)->BOOL {
        auto& c=*reinterpret_cast<Context*>(p); DWORD pid; GetWindowThreadProcessId(h,&pid);
        wchar_t name[64]{}; GetClassNameW(h,name,64);
        if (pid==c.pid && IsWindowVisible(h) && !GetWindow(h,GW_OWNER) && wcscmp(name,L"DGMPPanel")) { c.result=h; return FALSE; } return TRUE;
    },reinterpret_cast<LPARAM>(&c)); return c.result;
}
LRESULT CALLBACK Proc(HWND h,UINT msg,WPARAM w,LPARAM l) {
    if (msg==WM_NCHITTEST) return HTTRANSPARENT;
    if (msg==WM_TIMER) {
        const HWND game=GameWindow();
        if (!game || GetForegroundWindow()!=game || IsIconic(game)) { ShowWindow(h,SW_HIDE); return 0; }
        RECT r; GetClientRect(game,&r); POINT p{12,12}; ClientToScreen(game,&p);
        SetWindowPos(h,HWND_TOPMOST,p.x,p.y,650,145,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        InvalidateRect(h,nullptr,FALSE); return 0;
    }
    if (msg==WM_PAINT) {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT r; GetClientRect(h,&r);
        auto brush=CreateSolidBrush(RGB(12,18,24)); FillRect(dc,&r,brush); DeleteObject(brush);
        auto font=CreateFontW(-18,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Consolas");
        auto old=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(165,230,210));
        std::wstring value; { std::lock_guard lock(mutex); value=text; }
        r.left+=10; r.top+=8; DrawTextW(dc,value.c_str(),-1,&r,DT_LEFT|DT_NOPREFIX);
        SelectObject(dc,old); DeleteObject(font); EndPaint(h,&ps); return 0;
    }
    return DefWindowProcW(h,msg,w,l);
}
DWORD WINAPI Thread(LPVOID) {
    auto instance=GetModuleHandleW(nullptr); WNDCLASSW wc{}; wc.lpfnWndProc=Proc; wc.hInstance=instance; wc.lpszClassName=L"DGMPPanel"; RegisterClassW(&wc);
    window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,wc.lpszClassName,L"DGMP",WS_POPUP,0,0,650,145,nullptr,nullptr,instance,nullptr);
    if (!window) return 1;
    SetLayeredWindowAttributes(window,0,225,LWA_ALPHA); SetTimer(window,1,150,nullptr);
    MSG msg; while (GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg); DispatchMessageW(&msg); } return 0;
}
}
void Start() { auto t=CreateThread(nullptr,0,Thread,nullptr,0,nullptr); if(t) CloseHandle(t); }
void Text(const std::wstring& value) { std::lock_guard lock(mutex); text=value; }
}
