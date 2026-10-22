#include "overlay.h"
#include "log.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace overlay {
namespace {
using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PresentFn originalPresent = nullptr;
ResizeFn originalResize = nullptr;

ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
ID3D11RenderTargetView* target = nullptr;
HWND window = nullptr;
WNDPROC gameProc = nullptr;
bool ready = false, failed = false;
std::atomic<bool> visible{false}, cursor{false}, typing{false}, overUi{false};
std::atomic<DrawFn> draw{nullptr}, cover{nullptr}, hud{nullptr};
std::atomic<DrawFn> page{nullptr};
std::atomic<FocusFn> pageFocused{nullptr};
std::atomic<InputFn> pageInput{nullptr};
bool PageFocused() { const FocusFn f = pageFocused; return f && f(); }
std::atomic<ULONGLONG> lastPresent{0};
ImFont* large = nullptr;
std::atomic<bool> keepFocus{false};
std::atomic<int> blockedKey{0};
// The game hides the system cursor (ShowCursor counter below zero, WM_SETCURSOR); in cursor mode the
// overlay shows it and gives the game back what it raised afterwards. Window thread only.
bool cursorShown = false;
int shownCalls = 0;
void ApplyCursor() {
    const bool want = visible && cursor;
    if (want == cursorShown) return;
    cursorShown = want;
    if (want) {
        int count;
        do { count = ShowCursor(TRUE); ++shownCalls; } while (count < 0 && shownCalls < 64);
        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
    } else {
        for (; shownCalls > 0; --shownCalls) ShowCursor(FALSE);
    }
}

void CreateTarget(IDXGISwapChain* chain) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back) {
        device->CreateRenderTargetView(back, nullptr, &target);
        back->Release();
    }
}

// The game's window procedure, wrapped: Tab toggles the cursor mode while the UI is shown; in cursor
// mode ImGui gets the input and the game none (key releases still pass, so nothing stays held).
LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    // Editor: losing focus would pause the game (and mute it); the engine just does not learn of it.
    if (keepFocus) {
        if ((msg == WM_ACTIVATEAPP && !w) || (msg == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) || msg == WM_KILLFOCUS) return 0;
    }
    ApplyCursor();
    // A key the multiplayer UI uses right now (an interaction's E): the game must not act on it too.
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && blockedKey && int(w) == blockedKey && !(visible && cursor)) return 0;
    if (visible && msg == WM_KEYDOWN && w == VK_TAB && !(l & (1 << 30)) && !(cursor && ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) && !PageFocused()) {
        cursor = !cursor;
        return 0;
    }
    // Releases reach ImGui in any mode: a key pressed in cursor mode and let go after it would stay
    // held there (an Enter that keeps confirming every dialog).
    if (!(visible && cursor) && ImGui::GetCurrentContext() && (msg == WM_KEYUP || msg == WM_SYSKEYUP)) ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
    if (visible && cursor && ImGui::GetCurrentContext()) {
        const LRESULT handled = ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        // The mode's page has the focus: what ImGui does not want (it has seen it) is the page's; a dialog
        // over the page stays ImGui's.
        if (PageFocused()) {
            const ImGuiIO& io = ImGui::GetIO();
            const bool mouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_SETCURSOR;
            const bool keys = msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
            const bool alt4 = msg == WM_SYSKEYDOWN && w == VK_F4;
            if ((mouse && !io.WantCaptureMouse && (msg != WM_SETCURSOR || LOWORD(l) == HTCLIENT)) || (keys && !io.WantCaptureKeyboard && !alt4)) {
                if (const InputFn in = pageInput) in(h, msg, w, l);
                if (msg == WM_SETCURSOR) return TRUE;
                if (msg == WM_KEYUP || msg == WM_SYSKEYUP) return CallWindowProcW(gameProc, h, msg, w, l);  // releases: the game too
                return 0;
            }
        }
        // The cursor's shape is ImGui's (the game would set none).
        if (msg == WM_SETCURSOR && LOWORD(l) == HTCLIENT) { if (!handled) SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))); return TRUE; }
        switch (msg) {
        case WM_KEYDOWN: case WM_CHAR: case WM_INPUT:
        case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            return 0;
        default: break;
        }
    }
    return CallWindowProcW(gameProc, h, msg, w, l);
}

bool Init(IDXGISwapChain* chain) {
    if (FAILED(chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device) return false;
    device->GetImmediateContext(&context);
    DXGI_SWAP_CHAIN_DESC desc{};
    chain->GetDesc(&desc);
    window = desc.OutputWindow;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // Latin and Cyrillic (both UI languages). Segoe UI ships with every Windows.
    wchar_t fonts[MAX_PATH]; GetWindowsDirectoryW(fonts, MAX_PATH);
    char path[MAX_PATH * 2]; WideCharToMultiByte(CP_UTF8, 0, fonts, -1, path, sizeof(path), nullptr, nullptr);
    std::string font = std::string(path) + "\\Fonts\\segoeui.ttf";
    if (GetFileAttributesA(font.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            // Latin, Cyrillic, punctuation (dashes, quotes, ellipsis), arrows and a few symbols.
            static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0400, 0x052F, 0x2000, 0x206F, 0x2190, 0x21FF, 0x2200, 0x22FF, 0x25A0, 0x25FF, 0x2700, 0x27BF, 0};
            io.Fonts->AddFontFromFileTTF(font.c_str(), 19.f, nullptr, ranges);
            large = io.Fonts->AddFontFromFileTTF(font.c_str(), 30.f, nullptr, ranges);  // loading cover
        }
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6; style.FrameRounding = 4; style.GrabRounding = 4;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device, context);
    CreateTarget(chain);
    gameProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Proc)));
    dg::Log("Overlay ready on the game's swap chain (window %p)", window);
    return true;
}

HRESULT __stdcall Present(IDXGISwapChain* chain, UINT sync, UINT flags) {
    if (!ready && !failed) { ready = Init(chain); failed = !ready; }
    lastPresent = GetTickCount64();
    const DrawFn fn = visible ? DrawFn(draw) : nullptr, first = cover, layer = hud, web = page;
    if (ready && (fn || first || layer || web)) {
        if (!target) CreateTarget(chain);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGuiIO& io = ImGui::GetIO();
        if (cursor) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse; else io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        // The UI takes the keyboard from now on: nothing the game had down counts as held here.
        static bool hadCursor = false;
        if (cursor && !hadCursor) io.ClearInputKeys();
        hadCursor = cursor;
        ImGui::NewFrame();
        if (layer) layer();
        if (web) web();
        if (first) first();
        if (fn) fn();
        typing = fn && io.WantTextInput;
        overUi = fn && cursor && io.WantCaptureMouse;
        ImGui::Render();
        context->OMSetRenderTargets(1, &target, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    return originalPresent(chain, sync, flags);
}

HRESULT __stdcall ResizeBuffers(IDXGISwapChain* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    if (target) { target->Release(); target = nullptr; }
    return originalResize(chain, count, width, height, format, flags);
}
}  // namespace

bool Install() {
    if (originalPresent) return true;
    // The swap chain's methods live in one vtable for all DXGI swap chains: take it from a throwaway one.
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"DGMPOverlayProbe";
    RegisterClassExW(&wc);
    HWND probe = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1; desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = probe; desc.SampleDesc.Count = 1; desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* chain = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &chain, &dev, nullptr, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && chain) {
        void** table = *reinterpret_cast<void***>(chain);
        DWORD old = 0;
        if (VirtualProtect(&table[8], sizeof(void*) * 6, PAGE_EXECUTE_READWRITE, &old)) {
            originalPresent = reinterpret_cast<PresentFn>(table[8]);
            originalResize = reinterpret_cast<ResizeFn>(table[13]);
            table[8] = reinterpret_cast<void*>(&Present);
            table[13] = reinterpret_cast<void*>(&ResizeBuffers);
            VirtualProtect(&table[8], sizeof(void*) * 6, old, &old);
            ok = true;
        }
    }
    if (chain) chain->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(probe);
    dg::Log("Overlay hook %s (hr=%08lX)", ok ? "installed" : "FAILED", static_cast<unsigned long>(hr));
    return ok;
}

void SetVisible(bool on) { visible = on; if (!on) cursor = false; }
bool Visible() { return visible; }
void SetCursorMode(bool on) { cursor = on && visible; }
bool CursorMode() { return cursor; }
bool Typing() { return typing; }
bool MouseOverUi() { return overUi; }
void SetDraw(DrawFn fn) { draw = fn; }
void SetCover(DrawFn fn) { cover = fn; }
void SetHud(DrawFn fn) { hud = fn; }
void SetPage(DrawFn fn, FocusFn focused, InputFn input) { pageFocused = focused; pageInput = input; page = fn; }
void* Device() { return device; }
void* Context() { return context; }
bool Presenting() { return ready && GetTickCount64() - lastPresent < 500; }
void* CreateTexture(const void* bgra, int width, int height) {
    if (!device || !bgra || width <= 0 || height <= 0) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(width); desc.Height = UINT(height); desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{bgra, UINT(width) * 4, 0};
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &data, &texture)) || !texture) return nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    device->CreateShaderResourceView(texture, nullptr, &view);
    texture->Release();
    return view;
}
void* LargeFont() { return large; }
void SetBlockedKey(int vk) { blockedKey = vk; }
void SetKeepFocus(bool keep) { keepFocus = keep; }

}  // namespace overlay
