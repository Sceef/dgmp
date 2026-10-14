#include "hud.h"
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <deque>
#include <mutex>

namespace hud {
namespace {
struct Line { uint32_t rgba; std::wstring text; ULONGLONG at; };

std::mutex lock;
std::deque<Line> lines;
std::wstring status, bannerTitle, bannerText, hintTitle, hintText, input;
ULONGLONG hintUntil = 0;
std::vector<std::wstring> submitted, history;
int historyPos = -1;
bool visible = false, allowed = false, open = false, skipChar = false;
HWND overlay = nullptr, game = nullptr;
WNDPROC gameProc = nullptr;
bool dirty = true;
constexpr ULONGLONG kFadeAfter = 15000;
constexpr size_t kInputMax = 200;

HWND FindGameWindow() {
    struct Ctx { DWORD pid; HWND found; } ctx{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        auto& c = *reinterpret_cast<Ctx*>(p); DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
        if (pid == c.pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && wcsncmp(cls, L"DGMP", 4) != 0) { c.found = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

void CloseChat() { open = false; input.clear(); historyPos = -1; dirty = true; }

// Runs on the game thread (the game's own window procedure). While the chat is open, key presses,
// characters, mouse buttons and raw mouse input never reach the game; key releases still do, so
// keys held when the chat opened are not stuck.
LRESULT CALLBACK GameProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    {
        std::lock_guard g(lock);
        if (!open) {
            const bool fresh = !(l & (1 << 30));
            if (msg == WM_KEYDOWN && fresh && allowed && visible && (w == 'T' || w == VK_OEM_2) && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
                open = true; skipChar = true; input = w == VK_OEM_2 ? L"/" : L""; historyPos = -1; dirty = true;
                return 0;
            }
        } else {
            switch (msg) {
            case WM_CHAR:
                if (skipChar) { skipChar = false; return 0; }
                if (w >= 32 && w != 127 && input.size() < kInputMax) { input += static_cast<wchar_t>(w); dirty = true; }
                return 0;
            case WM_KEYDOWN:
                if (w == VK_RETURN) {
                    if (!input.empty() && input != L"/") {
                        submitted.push_back(input);
                        history.insert(history.begin(), input);
                        if (history.size() > 30) history.pop_back();
                    }
                    CloseChat();
                } else if (w == VK_ESCAPE) {
                    CloseChat();
                } else if (w == VK_BACK) {
                    if (!input.empty()) {
                        input.pop_back();
                        if (!input.empty() && IS_HIGH_SURROGATE(input.back())) input.pop_back();
                        dirty = true;
                    }
                } else if (w == VK_UP || w == VK_DOWN) {
                    const int next = std::clamp(historyPos + (w == VK_UP ? 1 : -1), -1, static_cast<int>(history.size()) - 1);
                    historyPos = next; input = next < 0 ? L"" : history[next]; dirty = true;
                }
                return 0;
            case WM_SYSCHAR: case WM_KEYUP: case WM_SYSKEYUP: case WM_SYSKEYDOWN:
                break;  // releases and Alt+Tab keep working
            case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
            case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MOUSEWHEEL:
                return 0;
            case WM_INPUT:
                return DefWindowProcW(h, msg, w, l);  // swallow raw mouse look, but let Windows clean up
            case WM_ACTIVATEAPP: case WM_KILLFOCUS:
                CloseChat();
                break;
            }
        }
    }
    return CallWindowProcW(gameProc, h, msg, w, l);
}

uint32_t Rgb(uint32_t rgba) { return rgba >> 8; }
Gdiplus::Color Col(uint32_t rgb, BYTE alpha = 255) { return Gdiplus::Color(alpha, BYTE(rgb >> 16), BYTE(rgb >> 8), BYTE(rgb)); }

void ShadowText(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font& f, Gdiplus::RectF r, Gdiplus::Color c, Gdiplus::StringAlignment align = Gdiplus::StringAlignmentNear) {
    Gdiplus::StringFormat fmt; fmt.SetAlignment(align); fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap); fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    Gdiplus::SolidBrush shadow(Gdiplus::Color(BYTE(c.GetA() * 0.85f), 0, 0, 0)), brush(c);
    Gdiplus::RectF sr = r; sr.X += 1.5f; sr.Y += 1.5f;
    g.DrawString(s.c_str(), -1, &f, sr, &fmt, &shadow);
    g.DrawString(s.c_str(), -1, &f, r, &fmt, &brush);
}

void Render(int w, int h) {
    BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { DeleteDC(mem); ReleaseDC(nullptr, screen); return; }
    auto old = SelectObject(mem, dib);
    {
        Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&canvas);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        const float unit = std::max(14.f, h / 56.f);
        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font chatFont(&family, unit, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font boldFont(&family, unit, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font bigFont(&family, unit * 1.9f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font smallFont(&family, unit * 0.85f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

        std::lock_guard gl(lock);
        const ULONGLONG now = GetTickCount64();
        // Chat, bottom-left above the game's own HUD corner.
        const float lineH = unit * 1.45f, left = unit * 1.6f, width = std::min(w * 0.45f, unit * 42.f);
        float y = h * 0.62f;
        const size_t shown = open ? 12 : 8;
        std::vector<const Line*> recent;
        for (auto it = lines.rbegin(); it != lines.rend() && recent.size() < shown; ++it)
            if (open || now - it->at < kFadeAfter + 1500) recent.push_back(&*it);
        if (open) {
            Gdiplus::SolidBrush panel(Gdiplus::Color(110, 10, 10, 10));
            g.FillRectangle(&panel, left - unit * 0.6f, y - shown * lineH - unit * 0.4f, width + unit * 1.2f, shown * lineH + unit * 0.4f);
        }
        float ly = y - lineH;
        for (auto l : recent) {
            const ULONGLONG age = now - l->at;
            const BYTE alpha = open || age < kFadeAfter ? 255 : BYTE(255 * (1.0 - double(age - kFadeAfter) / 1500.0));
            ShadowText(g, l->text, chatFont, {left, ly, width, lineH}, Col(Rgb(l->rgba), alpha));
            ly -= lineH;
        }
        if (open) {
            Gdiplus::SolidBrush box(Gdiplus::Color(185, 14, 13, 12));
            Gdiplus::Pen edge(Gdiplus::Color(255, 210, 101, 45), 1.5f);
            const Gdiplus::RectF r(left - unit * 0.6f, y + unit * 0.3f, width + unit * 1.2f, lineH + unit * 0.4f);
            g.FillRectangle(&box, r); g.DrawRectangle(&edge, r);
            const bool caret = (now / 500) % 2 == 0;
            ShadowText(g, L"Сказать: " + input + (caret ? L"|" : L""), chatFont, {left, y + unit * 0.45f, width, lineH}, Col(0xF2EADF));
        }
        // Status, top-right.
        if (!status.empty()) ShadowText(g, status, smallFont, {w - unit * 30.f, unit * 0.8f, unit * 29.f, lineH}, Col(0xD8D0C4, 220), Gdiplus::StringAlignmentFar);
        // Centre banner: persistent problem message, else the latest server hint.
        const bool hint = now < hintUntil;
        const std::wstring& title = !bannerTitle.empty() ? bannerTitle : hint ? hintTitle : bannerTitle;
        const std::wstring& text = !bannerTitle.empty() ? bannerText : hint ? hintText : bannerText;
        if (!title.empty() || !text.empty()) {
            const float bw = std::min(w * 0.7f, unit * 40.f), bx = (w - bw) / 2, by = h * 0.22f;
            Gdiplus::SolidBrush bg(Gdiplus::Color(200, 14, 13, 12));
            Gdiplus::SolidBrush accent(Gdiplus::Color(255, 210, 101, 45));
            g.FillRectangle(&bg, bx, by, bw, unit * 5.6f);
            g.FillRectangle(&accent, bx, by, unit * 0.3f, unit * 5.6f);
            ShadowText(g, title, bigFont, {bx + unit, by + unit * 0.5f, bw - unit * 2, unit * 2.6f}, Col(0xF2EADF));
            Gdiplus::StringFormat wrap;
            Gdiplus::SolidBrush body(Col(0xC9C0B3));
            g.DrawString(text.c_str(), -1, &chatFont, Gdiplus::RectF(bx + unit, by + unit * 3.0f, bw - unit * 2, unit * 2.6f), &wrap, &body);
        }
    }
    POINT origin{0, 0}; ClientToScreen(game, &origin);
    SIZE size{w, h}; POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(overlay, screen, &origin, &size, mem, &src, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old); DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, w, l);
}

DWORD WINAPI Thread(LPVOID) {
    ULONG_PTR token = 0; Gdiplus::GdiplusStartupInput in; Gdiplus::GdiplusStartup(&token, &in, nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = OverlayProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"DGMPHud";
    RegisterClassW(&wc);
    overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"DGMP HUD",
                              WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    SetTimer(overlay, 1, 33, nullptr);
    bool shown = false; RECT lastRect{}; ULONGLONG lastAnim = 0;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_TIMER && m.hwnd == overlay) {
            if (!game || !IsWindow(game)) {
                game = FindGameWindow();
                if (game && !gameProc) gameProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(game, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GameProc)));
            }
            bool want;
            { std::lock_guard g(lock); want = visible; }
            want = want && game && !IsIconic(game) && GetForegroundWindow() == game;
            if (!want) { if (shown) { ShowWindow(overlay, SW_HIDE); shown = false; } continue; }
            RECT rc; GetClientRect(game, &rc);
            POINT tl{0, 0}; ClientToScreen(game, &tl); OffsetRect(&rc, tl.x, tl.y);
            const ULONGLONG now = GetTickCount64();
            bool redraw;
            { std::lock_guard g(lock); redraw = dirty || open || !EqualRect(&rc, &lastRect) || now - lastAnim < 2000 || now < hintUntil + 100; dirty = false;
              for (auto& l : lines) if (now - l.at < kFadeAfter + 1600) { redraw = redraw || now - l.at >= kFadeAfter; } }
            if (redraw && rc.right > rc.left && rc.bottom > rc.top) {
                Render(rc.right - rc.left, rc.bottom - rc.top);
                lastRect = rc;
            }
            if (!shown) { SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); shown = true; }
            else SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            continue;
        }
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    if (token) Gdiplus::GdiplusShutdown(token);
    return 0;
}
}  // namespace

void Start() { if (HANDLE t = CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr)) CloseHandle(t); }
void SetVisible(bool on) { std::lock_guard g(lock); if (visible != on) { visible = on; dirty = true; if (!on && open) CloseChat(); } }
void SetChatAllowed(bool on) { std::lock_guard g(lock); allowed = on; if (!on && open) CloseChat(); }
void SetStatus(const std::wstring& text) { std::lock_guard g(lock); if (status != text) { status = text; dirty = true; } }
void SetBanner(const std::wstring& title, const std::wstring& text) {
    std::lock_guard g(lock); if (bannerTitle != title || bannerText != text) { bannerTitle = title; bannerText = text; dirty = true; }
}
void ShowHint(const std::wstring& title, const std::wstring& text, double seconds) {
    std::lock_guard g(lock); hintTitle = title; hintText = text; hintUntil = GetTickCount64() + ULONGLONG(seconds * 1000); dirty = true;
}
void AddMessage(uint32_t rgba, const std::wstring& text) {
    std::lock_guard g(lock); lines.push_back({rgba, text, GetTickCount64()}); if (lines.size() > 100) lines.pop_front(); dirty = true;
}
bool ChatOpen() { std::lock_guard g(lock); return open; }
std::vector<std::wstring> TakeSubmitted() { std::lock_guard g(lock); std::vector<std::wstring> out; out.swap(submitted); return out; }

}  // namespace hud
