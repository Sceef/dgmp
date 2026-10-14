#include "panel.h"
#include "profile.h"
#include "devmenu.h"
#include "../api/dgmp.h"
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
namespace panel {
namespace {
std::mutex mutex;
std::wstring text=L"DGMP: initializing...";
std::wstring loadingStage=L"Запуск DGMP";
bool loading=true;
float progress=0.03f;
HWND window=nullptr;
std::unique_ptr<Gdiplus::Image> background;
HWND GameWindow() {
    struct Context { DWORD pid; HWND result; } c{GetCurrentProcessId(),nullptr};
    EnumWindows([](HWND h,LPARAM p)->BOOL {
        auto& c=*reinterpret_cast<Context*>(p); DWORD pid; GetWindowThreadProcessId(h,&pid);
        wchar_t name[64]{}; GetClassNameW(h,name,64);
        if(pid==c.pid && IsWindowVisible(h) && !GetWindow(h,GW_OWNER) && wcscmp(name,L"DGMPPanel")) { c.result=h; return FALSE; } return TRUE;
    },reinterpret_cast<LPARAM>(&c)); return c.result;
}
void DrawTextLine(HDC dc,RECT r,const std::wstring& value,int size,COLORREF color,UINT align=DT_LEFT) {
    auto font=CreateFontW(-size,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
    auto old=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,color);
    DrawTextW(dc,value.c_str(),-1,&r,align|DT_NOPREFIX); SelectObject(dc,old); DeleteObject(font);
}
void Fill(HDC dc,RECT r,COLORREF color) { auto brush=CreateSolidBrush(color); FillRect(dc,&r,brush); DeleteObject(brush); }
void PaintDevMenu(HDC dc,RECT r) {
    const auto menu=devmenu::GetSnapshot(); const int width=r.right,height=r.bottom;
    Fill(dc,r,RGB(12,22,28));
    DrawTextLine(dc,{24,22,width-80,62},L"DGMP · Дев-меню",28,RGB(214,242,234));
    DrawTextLine(dc,{24,68,width-24,112},L"Время, погода и метки текущего мира",17,RGB(142,167,173));
    DrawTextLine(dc,{width-72,26,width-16,62},L"× F3",18,RGB(142,167,173));
    const int step=std::max(28,std::min(48,(height-210)/8));
    for(int i=0;i<int(menu.rows.size());++i) {
        const int top=120+i*step;
        Fill(dc,{16,top,width-16,top+step-5},i==menu.selected?RGB(30,68,71):RGB(21,35,43));
        DrawTextLine(dc,{26,top+7,width-125,top+step-5},menu.rows[i],18,RGB(211,229,232));
        Fill(dc,{width-116,top+4,width-72,top+step-9},RGB(38,57,64));
        Fill(dc,{width-62,top+4,width-18,top+step-9},RGB(38,57,64));
        DrawTextLine(dc,{width-116,top+4,width-72,top+step-9},L"−",22,RGB(165,230,210),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        DrawTextLine(dc,{width-62,top+4,width-18,top+step-9},L"+",22,RGB(165,230,210),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    const int bottom=120+8*step+8;
    DrawTextLine(dc,{24,bottom,width-24,bottom+35},menu.status,16,menu.error?RGB(240,148,127):RGB(142,185,173));
    DrawTextLine(dc,{24,bottom+39,width-24,height-12},L"↑↓ выбрать · ←→ изменить · Enter переключить\nВремя: шаг 15 минут · Enter — полдень · F3 / Esc закрыть",16,RGB(142,167,173));
}
void Paint(HDC dc,RECT r) {
    std::wstring value,stage; bool cover; float amount;
    { std::lock_guard lock(mutex); value=text; stage=loadingStage; cover=loading; amount=progress; }
    if(!cover) {
        if(devmenu::IsOpen()) { PaintDevMenu(dc,r); return; }
        Fill(dc,r,RGB(12,18,24)); r.left+=10; r.top+=8;
        auto font=CreateFontW(-18,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Consolas");
        auto old=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(165,230,210));
        DrawTextW(dc,value.c_str(),-1,&r,DT_LEFT|DT_NOPREFIX); SelectObject(dc,old); DeleteObject(font); return;
    }
    Fill(dc,r,RGB(248,248,248));
    const int width=r.right-r.left,height=r.bottom-r.top;
    if(background && background->GetLastStatus()==Gdiplus::Ok && background->GetWidth()) {
        const float scale=std::min(float(width)*0.88f/background->GetWidth(),float(height)*0.60f/background->GetHeight());
        const int w=int(background->GetWidth()*scale),h=int(background->GetHeight()*scale);
        Gdiplus::Graphics graphics(dc); graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(background.get(),(width-w)/2,std::max(30,(height-h)/2-50),w,h);
    }
    const int margin=std::max(32,width/12);
    RECT heading{margin,24,width-margin,66}; DrawTextLine(dc,heading,L"DGMP",26,RGB(25,38,43));
    RECT label{margin,height-145,width-margin,height-94}; DrawTextLine(dc,label,stage,24,RGB(25,38,43));
    RECT bar{margin,height-80,width-margin,height-72}; Fill(dc,bar,RGB(212,219,218));
    bar.right=bar.left+int((bar.right-bar.left)*std::clamp(amount,0.f,1.f)); Fill(dc,bar,RGB(20,120,111));
    RECT info{margin,height-61,width-margin,height-27}; DrawTextLine(dc,info,L"Подготовка мира",17,RGB(87,102,103));
}
LRESULT CALLBACK Proc(HWND h,UINT msg,WPARAM w,LPARAM l) {
    if(msg==WM_SETCURSOR && (IsLoading() || devmenu::IsOpen())) { SetCursor(LoadCursorW(nullptr,MAKEINTRESOURCEW(32512))); return TRUE; }
    if(msg==WM_NCHITTEST) return (IsLoading() || devmenu::IsOpen())?HTCLIENT:HTTRANSPARENT;
    if(msg==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if(msg==WM_LBUTTONUP && devmenu::IsOpen() && !IsLoading()) {
        RECT r; GetClientRect(h,&r); const int x=short(LOWORD(l)),y=short(HIWORD(l));
        if(y<65 && x>r.right-80) { DGMP_SetDevMenuVisible(0); return 0; }
        const int step=std::max(28,std::min(48,(int(r.bottom)-210)/8));
        if(y>=120 && y<120+8*step) devmenu::Click((y-120)/step,x>=r.right-62?1:x>=r.right-116?-1:0);
        return 0;
    }
    if(msg==WM_TIMER) {
        const HWND game=GameWindow();
        if(!game || GetForegroundWindow()!=game || IsIconic(game)) { ShowWindow(h,SW_HIDE); return 0; }
        RECT r; GetClientRect(game,&r); const bool cover=IsLoading(),dev=devmenu::IsOpen();
        bool empty; { std::lock_guard lock(mutex); empty=text.empty(); }
        if(!cover && !dev && empty) { ShowWindow(h,SW_HIDE); return 0; }  // multiplayer: no editor info box
        const int width=cover?int(r.right):dev?std::min(660,std::max(1,int(r.right)-24)):650;
        const int height=cover?int(r.bottom):dev?std::min(620,std::max(1,int(r.bottom)-24)):145;
        POINT p{cover?0:dev?std::max(0,int(r.right)-width-12):12,cover?0:12}; ClientToScreen(game,&p);
        auto style=GetWindowLongPtrW(h,GWL_EXSTYLE);
        SetWindowLongPtrW(h,GWL_EXSTYLE,(cover || dev)?(style&~WS_EX_TRANSPARENT):(style|WS_EX_TRANSPARENT));
        SetLayeredWindowAttributes(h,0,(cover || dev)?255:225,LWA_ALPHA);
        SetWindowPos(h,HWND_TOPMOST,p.x,p.y,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        InvalidateRect(h,nullptr,FALSE); return 0;
    }
    if(msg==WM_ERASEBKGND) return 1;
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT r; GetClientRect(h,&r);
        HDC buffer=CreateCompatibleDC(dc); HBITMAP bitmap=CreateCompatibleBitmap(dc,std::max(1L,r.right),std::max(1L,r.bottom));
        auto old=SelectObject(buffer,bitmap); Paint(buffer,r); BitBlt(dc,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY);
        SelectObject(buffer,old); DeleteObject(bitmap); DeleteDC(buffer); EndPaint(h,&ps); return 0;
    }
    return DefWindowProcW(h,msg,w,l);
}
DWORD WINAPI Thread(LPVOID) {
    ULONG_PTR token=0; Gdiplus::GdiplusStartupInput input; Gdiplus::GdiplusStartup(&token,&input,nullptr);
    const auto root=std::filesystem::path(profile::Root());
    const auto custom=root/L"assets"/L"loading.png";
    const auto image=std::filesystem::exists(custom)?custom:root.parent_path()/L"BendGame"/L"Content"/L"Splash"/L"Splash.bmp";
    background.reset(Gdiplus::Image::FromFile(image.c_str()));
    auto instance=GetModuleHandleW(nullptr); WNDCLASSW wc{}; wc.lpfnWndProc=Proc; wc.hInstance=instance; wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512)); wc.lpszClassName=L"DGMPPanel"; RegisterClassW(&wc);
    window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,wc.lpszClassName,L"DGMP",WS_POPUP,0,0,650,145,nullptr,nullptr,instance,nullptr);
    if(window) {
        SetLayeredWindowAttributes(window,0,255,LWA_ALPHA); SetTimer(window,1,33,nullptr);
        MSG msg; while(GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    background.reset(); if(token) Gdiplus::GdiplusShutdown(token); return 0;
}
}
void Start() { auto t=CreateThread(nullptr,0,Thread,nullptr,0,nullptr); if(t) CloseHandle(t); }
void Text(const std::wstring& value) { std::lock_guard lock(mutex); text=value; }
void Loading(bool visible,float amount,const std::wstring& stage) {
    std::lock_guard lock(mutex); loading=visible; progress=std::clamp(amount,0.f,1.f); loadingStage=stage;
}
bool IsLoading() { std::lock_guard lock(mutex); return loading; }
}
