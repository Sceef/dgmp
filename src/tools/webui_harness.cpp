// Desktop test of src/webui.cpp without the game: a D3D11 window with ImGui stands in for the game's
// overlay; the mode's ui folder is served as if the server had sent it, and a tiny stand-in for the
// mode's Lua answers the page (ping -> pong, close -> focus off). F2 toggles the page's focus.
//   build\webui-harness.exe [modes\htmlui\ui] [page.html] [--test]
//   --test: clicks at (200,340) and (250,435), types there and presses Enter (posted to its own window),
//   F2 at 6 s; build\harness-1.bmp .. -3.bmp at 2.5, 5 and 7.5 s, then quits.
// Build (x64 developer prompt, in dgmp; build\imgui.lib from build.bat):
//   cl /std:c++20 /EHsc /utf-8 /MT /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Isrc /Ithird_party\enet\include /Ithird_party\imgui
//      /Ithird_party\webview2\build\native\include src\tools\webui_harness.cpp src\webui.cpp /Fe"build\webui-harness.exe"
//      /link build\imgui.lib third_party\webview2\build\native\x64\WebView2LoaderStatic.lib windowsapp.lib CoreMessaging.lib
//      shlwapi.lib d3d11.lib dxgi.lib user32.lib gdi32.lib ole32.lib advapi32.lib shell32.lib bcrypt.lib
#include "../webui.h"
#include "../overlay.h"
#include "../log.h"
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include <windows.h>
#include <d3d11.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace fs = std::filesystem;

// ---------------------------------------------------------------- stand-ins for the game DLL
namespace { ID3D11Device* gDevice; ID3D11DeviceContext* gContext; overlay::DrawFn gPage; overlay::FocusFn gFocused; overlay::InputFn gInput; bool gVisible, gCursor; std::wstring gRoot; }
namespace overlay {
void SetPage(DrawFn d, FocusFn f, InputFn i) { gPage = d; gFocused = f; gInput = i; }
void* Device() { return gDevice; }
void* Context() { return gContext; }
void SetVisible(bool v) { gVisible = v; if (!v) gCursor = false; }
bool Visible() { return gVisible; }
void SetCursorMode(bool on) { gCursor = on; }
bool CursorMode() { return gCursor; }
}
namespace mpui { bool DialogOpen() { return false; } }
namespace skins { bool CreatorOpen() { return false; } }
namespace hud { void AddMessage(uint32_t, const std::wstring& t) { wprintf(L"[chat] %s\n", t.c_str()); } }
namespace profile { const std::wstring& Root() { return gRoot; } }
#define VERSION_FN(ret, name, params, args) extern "C" ret WINAPI Proxy_##name params { \
    static auto fn = reinterpret_cast<ret(WINAPI*) params>(GetProcAddress(LoadLibraryW(L"version.dll"), #name)); return fn args; }
VERSION_FN(BOOL, GetFileVersionInfoW, (LPCWSTR f, DWORD h, DWORD l, LPVOID d), (f, h, l, d))
VERSION_FN(DWORD, GetFileVersionInfoSizeW, (LPCWSTR f, LPDWORD h), (f, h))
VERSION_FN(BOOL, VerQueryValueW, (LPCVOID b, LPCWSTR s, LPVOID* p, PUINT l), (b, s, p, l))

// The server: the files it would send, and the mode's Lua.
namespace { std::map<std::string, std::string> gFiles; std::vector<std::string> gAsked; int gClicks = 0; }
namespace net {
Client::~Client() {}
void Client::SendUiRequest(const std::string& path) { gAsked.push_back(path); }  // (answered after OnList)
void Client::SendUiMessage(const std::string& name, const std::string& json) {
    printf("[page -> lua] %s %s\n", name.c_str(), json.c_str());
    if (name == "ping") webui::OnUi(UiOp::Event, "pong", "{\"at\":" + std::to_string(GetTickCount64() / 1000.0) + ",\"clicks\":" + std::to_string(++gClicks) + "}", false);
    if (name == "close") { webui::OnUi(UiOp::Event, "phone", "{\"open\":false}", false); webui::OnUi(UiOp::Focus, "", "", false); }
}
}

namespace {
void Shot(IDXGISwapChain* chain, const char* file) {
    ID3D11Texture2D* back = nullptr; chain->GetBuffer(0, IID_PPV_ARGS(&back));
    D3D11_TEXTURE2D_DESC d; back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr; gDevice->CreateTexture2D(&d, nullptr, &staging);
    gContext->CopyResource(staging, back);
    D3D11_MAPPED_SUBRESOURCE map; gContext->Map(staging, 0, D3D11_MAP_READ, 0, &map);
    FILE* f = fopen(file, "wb");
    BITMAPFILEHEADER fh{0x4D42}; BITMAPINFOHEADER ih{sizeof(ih), LONG(d.Width), -LONG(d.Height), 1, 32};
    fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
    std::vector<uint8_t> row(d.Width * 4);
    for (UINT y = 0; y < d.Height; ++y) {
        const uint8_t* src = static_cast<uint8_t*>(map.pData) + y * map.RowPitch;
        for (UINT x = 0; x < d.Width; ++x) { row[x * 4] = src[x * 4 + 2]; row[x * 4 + 1] = src[x * 4 + 1]; row[x * 4 + 2] = src[x * 4]; row[x * 4 + 3] = 255; }
        fwrite(row.data(), 4, d.Width, f);
    }
    fclose(f); gContext->Unmap(staging, 0); staging->Release(); back->Release();
    printf("[test] %s\n", file);
}
std::string Sha256(const std::string& data) {
    BCRYPT_ALG_HANDLE alg; BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    unsigned char h[32]; BCryptHash(alg, nullptr, 0, (PUCHAR)data.data(), ULONG(data.size()), h, 32); BCryptCloseAlgorithmProvider(alg, 0);
    std::string s; char b[3]; for (unsigned char c : h) { snprintf(b, 3, "%02x", c); s += b; } return s;
}
void TogglePhone() {
    static bool open = false;
    open = !open;
    webui::OnUi(net::UiOp::Event, "phone", open ? "{\"open\":true,\"players\":[{\"id\":1,\"name\":\"Deacon\"},{\"id\":2,\"name\":\"Boozer\"}]}" : "{\"open\":false}", false);
    webui::OnUi(net::UiOp::Focus, "", "", open);
}
LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (msg == WM_KEYDOWN && w == VK_F2 && !(l & (1 << 30))) { TogglePhone(); return 0; }
    // As overlay.cpp: ImGui first, then what it does not want goes to a focused page.
    if (ImGui::GetCurrentContext()) ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
    if (gFocused && gFocused() && ImGui::GetCurrentContext()) {
        const ImGuiIO& io = ImGui::GetIO();
        const bool mouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_SETCURSOR;
        const bool keys = msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
        if ((mouse && !io.WantCaptureMouse && (msg != WM_SETCURSOR || LOWORD(l) == HTCLIENT)) || (keys && !io.WantCaptureKeyboard)) {
            gInput(h, msg, w, l);
            if (msg == WM_SETCURSOR) return TRUE;
            return 0;
        }
    }
    return DefWindowProcW(h, msg, w, l);
}
}

int main(int argc, char** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::path("modes/htmlui/ui");
    const std::string page = argc > 2 ? argv[2] : "index.html";
    const bool test = argc > 3 && std::string(argv[3]) == "--test";
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); gRoot = std::wstring(tmp) + L"dgmp-webui-harness";
    dg::LogFile() = stdout;
    setvbuf(stdout, nullptr, _IONBF, 0);

    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"DGMPWebUiHarness"; wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"DGMP webui harness (F2: phone)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = window; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* chain = nullptr;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &chain, &gDevice, nullptr, &gContext))) return 1;
    ID3D11RenderTargetView* target = nullptr;
    { ID3D11Texture2D* back; chain->GetBuffer(0, IID_PPV_ARGS(&back)); gDevice->CreateRenderTargetView(back, nullptr, &target); back->Release(); }
    ImGui::CreateContext(); ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplWin32_Init(window); ImGui_ImplDX11_Init(gDevice, gContext);

    // The server's side of joining: the file list, then the page.
    std::vector<std::pair<std::string, std::string>> list;
    for (auto& e : fs::recursive_directory_iterator(dir)) if (e.is_regular_file()) {
        std::ifstream in(e.path(), std::ios::binary); std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string path = fs::relative(e.path(), dir).generic_string();
        gFiles[path] = data; list.emplace_back(path, Sha256(data));
    }
    net::Client client;
    webui::Install();
    webui::OnList(list, client);
    for (auto& path : gAsked) webui::OnData(path, 0, 1, gFiles[path]);
    webui::OnUi(net::UiOp::Open, "", page, false);
    webui::OnUi(net::UiOp::Event, "status", "{\"name\":\"Harness\",\"players\":3,\"time\":\"12:34\"}", false);

    const ULONGLONG start = GetTickCount64();
    int step = 0;
    auto post = [&](UINT msg, WPARAM w, LPARAM l) { PostMessageW(window, msg, w, l); };
    for (MSG m;;) {
        const ULONGLONG t = GetTickCount64() - start;
        if (test && step == 0 && t > 3000) {
            ++step;
            webui::OnUi(net::UiOp::Focus, "", "", true);
            webui::OnUi(net::UiOp::Event, "echo", "{\"n\":1,\"text\":\"привет\"}", false);
        }
        if (test && step == 1 && t > 3500) {
            ++step;
            post(WM_MOUSEMOVE, 0, MAKELPARAM(200, 340)); post(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(200, 340)); post(WM_LBUTTONUP, 0, MAKELPARAM(200, 340));
            post(WM_MOUSEMOVE, 0, MAKELPARAM(250, 435)); post(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(250, 435)); post(WM_LBUTTONUP, 0, MAKELPARAM(250, 435));
        }
        if (test && step == 2 && t > 4000) {
            ++step;
            for (const wchar_t c : std::wstring(L"Hi Привет")) post(WM_CHAR, c, 1);
            post(WM_KEYDOWN, VK_BACK, 1); post(WM_KEYUP, VK_BACK, 1);
            post(WM_KEYDOWN, VK_RETURN, 1); post(WM_CHAR, L'\r', 1); post(WM_KEYUP, VK_RETURN, 1);
        }
        if (test && step == 3 && t > 6000) { ++step; post(WM_KEYDOWN, VK_F2, 1); post(WM_KEYUP, VK_F2, 1); }
        if (test && t > 9000) return 0;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { if (m.message == WM_QUIT) return 0; TranslateMessage(&m); DispatchMessageW(&m); }
        webui::Update(client);
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        if (gPage) gPage();
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Once);
        ImGui::Begin("Game stand-in");
        ImGui::Text("F2: phone. Page focus: %s", webui::Focused() ? "yes" : "no");
        ImGui::End();
        ImGui::Render();
        const float sky[4] = {0.25f, 0.4f, 0.3f, 1};  // "the game"
        gContext->OMSetRenderTargets(1, &target, nullptr);
        gContext->ClearRenderTargetView(target, sky);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        static int shots = 0;
        if (test && shots < 3 && t > 2500ull + 2500ull * shots) { const std::string f = "build\\harness-" + std::to_string(++shots) + ".bmp"; Shot(chain, f.c_str()); }
        chain->Present(1, 0);
    }
}
