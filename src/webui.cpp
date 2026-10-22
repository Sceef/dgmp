#include "webui.h"
#include "overlay.h"
#include "mpui.h"
#include "skins.h"
#include "profile.h"
#include "hud.h"
#include "lang.h"
#include "log.h"
#include "shared/json.h"
#include "imgui.h"
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shlwapi.h>
#include <wrl.h>
#include <DispatcherQueue.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

// The WebView2 loader checks the runtime's file version through VERSION.dll, which is this DLL (the
// proxy): its imports go straight to the forwarding stubs (proxy.cpp) instead.
extern "C" {
BOOL WINAPI Proxy_GetFileVersionInfoW(LPCWSTR, DWORD, DWORD, LPVOID);
DWORD WINAPI Proxy_GetFileVersionInfoSizeW(LPCWSTR, LPDWORD);
BOOL WINAPI Proxy_VerQueryValueW(LPCVOID, LPCWSTR, LPVOID*, PUINT);
decltype(&Proxy_GetFileVersionInfoW) __imp_GetFileVersionInfoW = &Proxy_GetFileVersionInfoW;
decltype(&Proxy_GetFileVersionInfoSizeW) __imp_GetFileVersionInfoSizeW = &Proxy_GetFileVersionInfoSizeW;
decltype(&Proxy_VerQueryValueW) __imp_VerQueryValueW = &Proxy_VerQueryValueW;
}

namespace fs = std::filesystem;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace wuc = winrt::Windows::UI::Composition;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace webui {
namespace {
constexpr wchar_t kOrigin[] = L"https://mode.dgmp/";
constexpr int kPageMessagesPerSecond = 30;

std::wstring Wide(const std::string& s) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}
std::string Utf8(const wchar_t* w) {
    if (!w) return {};
    std::string s(size_t(WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), int(s.size()), nullptr, nullptr);
    if (!s.empty()) s.pop_back();
    return s;
}
std::string Quote(const std::string& s) { return json::Value(s).dump(); }

// ---------------------------------------------------------------- the mode's files (cache/ui/<sha256>)
std::mutex filesLock;
std::map<std::string, std::string> files;  // path -> sha256
struct Partial { std::string hash, bytes; int next = 0; };
std::map<std::string, Partial> arriving;   // path -> parts so far
fs::path CacheDir() { return fs::path(profile::Root()) / L"cache" / L"ui"; }
bool SafeHash(const std::string& h) { if (h.size() != 64) return false; for (char c : h) if (!isxdigit(static_cast<unsigned char>(c))) return false; return true; }
bool FilesReady() { std::lock_guard g(filesLock); return arriving.empty(); }
// The file at that path of the mode's UI (web thread): false if it is not one.
bool ReadFile(const std::string& path, std::string& out) {
    std::string hash;
    { std::lock_guard g(filesLock); auto it = files.find(path); if (it == files.end()) return false; hash = it->second; }
    std::ifstream in(CacheDir() / hash, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
const wchar_t* ContentType(const std::string& path) {
    const auto dot = path.rfind('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (auto& c : ext) c = char(tolower(static_cast<unsigned char>(c)));
    static const std::map<std::string, const wchar_t*> types = {
        {"html", L"text/html; charset=utf-8"}, {"htm", L"text/html; charset=utf-8"}, {"css", L"text/css; charset=utf-8"},
        {"js", L"text/javascript; charset=utf-8"}, {"mjs", L"text/javascript; charset=utf-8"}, {"json", L"application/json"},
        {"svg", L"image/svg+xml"}, {"png", L"image/png"}, {"jpg", L"image/jpeg"}, {"jpeg", L"image/jpeg"}, {"gif", L"image/gif"},
        {"webp", L"image/webp"}, {"woff", L"font/woff"}, {"woff2", L"font/woff2"}, {"ttf", L"font/ttf"}, {"otf", L"font/otf"},
        {"mp3", L"audio/mpeg"}, {"ogg", L"audio/ogg"}, {"wav", L"audio/wav"}, {"txt", L"text/plain; charset=utf-8"}};
    auto it = types.find(ext);
    return it != types.end() ? it->second : L"application/octet-stream";
}

// ---------------------------------------------------------------- shared between the threads
std::atomic<bool> focused{false};   // the server gave the page the focus (and a page is open)
std::atomic<bool> shown{false};     // a page has loaded: its frames are drawn
std::atomic<bool> started{false};   // the web thread runs
std::atomic<bool> unavailable{false}, told{false};
std::atomic<HCURSOR> pageCursor{nullptr};
std::atomic<int> screenW{0}, screenH{0};  // the page's size: the game's frame
std::mutex outLock;
std::vector<std::pair<std::string, std::string>> outgoing;  // page -> server (name, json)
// Game thread: an open waiting for the files, and what the server sent the page meanwhile.
struct PendingOpen { bool on = false; std::string page; std::vector<std::string> messages; } pending;

// ---------------------------------------------------------------- the web thread
DWORD webThreadId = 0;
std::mutex tasksLock;
std::vector<std::function<void()>> tasks;
void Post(std::function<void()> fn) {
    { std::lock_guard g(tasksLock); tasks.push_back(std::move(fn)); }
    if (webThreadId) PostThreadMessageW(webThreadId, WM_APP, 0, 0);
}

struct Web {
    HWND host = nullptr;
    wuc::Compositor compositor{nullptr};
    wuc::ContainerVisual root{nullptr};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    wgd::Direct3D11::IDirect3DDevice d3d{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    wgc::Direct3D11CaptureFrame frame{nullptr};  // the newest frame, not handed over yet
    ComPtr<ID3D11Texture2D> shared;              // handed over to the overlay (keyed mutex: 0 ours, 1 the overlay's)
    ComPtr<IDXGIKeyedMutex> mutex;
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2CompositionController> comp;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    int w = 0, h = 0;
    bool creating = false, failed = false, loaded = false;
    std::string page;                  // the page that is open ("" none)
    std::vector<std::string> queued;   // messages for it before it loaded (JSON)
    double windowStart = 0; int windowCount = 0;  // page -> server rate
    int logged = 0;                    // page console lines written to the log
};
// Never destroyed: at the process's exit its COM objects must not be released from another thread.
Web& web = *new Web;

// The texture the overlay reads, a new one after every resize.
std::mutex sharedLock;
struct SharedInfo { HANDLE handle = nullptr; uint64_t gen = 0; } sharedInfo;

void MakeShared(int w, int h) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(w); d.Height = UINT(h); d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    web.shared.Reset(); web.mutex.Reset();
    HANDLE handle = nullptr;
    if (SUCCEEDED(web.device->CreateTexture2D(&d, nullptr, &web.shared))) {
        web.shared.As(&web.mutex);
        if (ComPtr<IDXGIResource> res; SUCCEEDED(web.shared.As(&res))) res->GetSharedHandle(&handle);
    }
    std::lock_guard g(sharedLock);
    sharedInfo.handle = handle; ++sharedInfo.gen;
}

// The newest captured frame into the shared texture, once the overlay has taken the previous one.
void Publish() {
    if (!web.frame || !web.mutex) return;
    if (web.mutex->AcquireSync(0, 0) != S_OK) return;  // (retried by the loop)
    auto access = web.frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&tex)))) {
        D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
        D3D11_BOX box{0, 0, 0, std::min(d.Width, UINT(web.w)), std::min(d.Height, UINT(web.h)), 1};
        web.context->CopySubresourceRegion(web.shared.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
    }
    web.mutex->ReleaseSync(1);
    web.frame = nullptr;
}

void ToPage(const std::string& name, const std::string& json) {
    const std::string message = "{\"name\":" + Quote(name) + ",\"data\":" + json + "}";
    if (web.view && web.loaded) web.view->PostWebMessageAsJson(Wide(message).c_str());
    else if (!web.page.empty() && web.queued.size() < 256) web.queued.push_back(message);
}

void Navigate() {
    web.loaded = false; shown = false;
    if (web.view) web.view->Navigate(web.page.empty() ? L"about:blank" : (kOrigin + Wide(web.page)).c_str());
}

void Resize(int w, int h) {
    if (w <= 0 || h <= 0 || (w == web.w && h == web.h)) return;
    web.w = w; web.h = h;
    // (also while the controller is still being created: the capture exists from the start)
    if (web.root) web.root.Size({float(w), float(h)});
    if (web.controller) web.controller->put_Bounds(RECT{0, 0, w, h});
    if (web.pool) {
        web.pool.Recreate(web.d3d, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, {w, h});
        web.frame = nullptr;
        MakeShared(w, h);
    }
}

// What the page writes to its console, and its errors: in the log (mode authors debug with it).
void LogFromPage(const std::string& line) {
    if (web.logged >= 300) return;
    if (++web.logged == 300) { dg::Log("UI page: (more console lines are not logged)"); return; }
    dg::Log("UI page: %.400s", line.c_str());
}
void Watch(const wchar_t* event, std::function<void(const json::Value&)> fn) {
    ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> receiver;
    if (FAILED(web.view->GetDevToolsProtocolEventReceiver(event, &receiver))) return;
    EventRegistrationToken t;
    receiver->add_DevToolsProtocolEventReceived(Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
        [fn](ICoreWebView2*, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
            LPWSTR text = nullptr;
            if (SUCCEEDED(args->get_ParameterObjectAsJson(&text)) && text) {
                json::Value v;
                if (json::Value::Parse(Utf8(text), v)) fn(v);
                CoTaskMemFree(text);
            }
            return S_OK;
        }).Get(), &t);
}

// Injected into every page: window.dgmp.
constexpr wchar_t kBridge[] = LR"JS((() => {
  const wv = window.chrome && window.chrome.webview;
  if (!wv || window.dgmp) return;
  const handlers = new Map();
  const dgmp = {
    focused: false,
    send(name, data) { wv.postMessage(JSON.stringify({ name: String(name), data: data === undefined ? null : data })); },
    on(name, fn) { if (!handlers.has(name)) handlers.set(name, []); handlers.get(name).push(fn); return fn; },
    off(name, fn) { const list = handlers.get(name); const i = list ? list.indexOf(fn) : -1; if (i >= 0) list.splice(i, 1); },
  };
  Object.defineProperty(window, 'dgmp', { value: dgmp });
  wv.addEventListener('message', (e) => {
    const m = e.data;
    if (!m || typeof m.name !== 'string') return;
    if (m.name === 'focus') dgmp.focused = !!m.data;
    for (const fn of (handlers.get(m.name) || []).slice()) { try { fn(m.data); } catch (err) { console.error(err); } }
  });
})();)JS";

void Setup(ICoreWebView2CompositionController* created) {
    web.comp = created;
    web.comp.As(&web.controller);
    web.controller->get_CoreWebView2(&web.view);
    web.comp->put_RootVisualTarget(web.root.as<::IUnknown>().get());
    if (ComPtr<ICoreWebView2Controller2> c2; SUCCEEDED(web.controller.As(&c2))) c2->put_DefaultBackgroundColor({0, 0, 0, 0});
    web.controller->put_Bounds(RECT{0, 0, web.w, web.h});
    web.controller->put_IsVisible(TRUE);
    // Nothing that would open a window of its own (menus, dialogs, autofill, printing) or leave the page.
    ComPtr<ICoreWebView2Settings> settings; web.view->get_Settings(&settings);
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_AreHostObjectsAllowed(FALSE);
    if (ComPtr<ICoreWebView2Settings3> s3; SUCCEEDED(settings.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    if (ComPtr<ICoreWebView2Settings4> s4; SUCCEEDED(settings.As(&s4))) { s4->put_IsPasswordAutosaveEnabled(FALSE); s4->put_IsGeneralAutofillEnabled(FALSE); }
    EventRegistrationToken t;
    web.view->add_ScriptDialogOpening(Callback<ICoreWebView2ScriptDialogOpeningEventHandler>(
        [](ICoreWebView2*, ICoreWebView2ScriptDialogOpeningEventArgs* args) -> HRESULT {
            LPWSTR text = nullptr; args->get_Message(&text);
            LogFromPage("alert: " + Utf8(text));
            if (text) CoTaskMemFree(text);
            return S_OK;  // (not accepted: confirm() is false, prompt() null)
        }).Get(), &t);
    web.view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT { args->put_Handled(TRUE); return S_OK; }).Get(), &t);
    web.view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr; args->get_Uri(&uri);
            if (!uri || (wcsncmp(uri, kOrigin, wcslen(kOrigin)) != 0 && wcscmp(uri, L"about:blank") != 0)) args->put_Cancel(TRUE);
            if (uri) CoTaskMemFree(uri);
            return S_OK;
        }).Get(), &t);
    web.view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL ok = FALSE; args->get_IsSuccess(&ok);
            if (web.page.empty()) return S_OK;
            if (!ok) dg::Log("UI: page %s did not load", web.page.c_str());
            web.loaded = true; shown = true;
            ToPage("focus", focused ? "true" : "false");
            for (auto& m : web.queued) web.view->PostWebMessageAsJson(Wide(m).c_str());
            web.queued.clear();
            return S_OK;
        }).Get(), &t);
    // Only the mode's files: https://mode.dgmp/<path> from the cache; anything else is refused.
    web.view->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    web.view->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            ComPtr<ICoreWebView2WebResourceRequest> request; args->get_Request(&request);
            LPWSTR uri = nullptr; request->get_Uri(&uri);
            std::string url = Utf8(uri);
            if (uri) CoTaskMemFree(uri);
            ComPtr<ICoreWebView2WebResourceResponse> response;
            const std::string origin = "https://mode.dgmp/";
            std::string data;
            if (url.compare(0, origin.size(), origin) == 0) {
                std::string path = url.substr(origin.size());
                path = path.substr(0, path.find_first_of("?#"));
                if (path.empty()) path = "index.html";
                if (ReadFile(path, data)) {
                    ComPtr<IStream> stream; stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(data.data()), UINT(data.size())));
                    const std::wstring headers = std::wstring(L"Content-Type: ") + ContentType(path) + L"\r\nCache-Control: no-store";
                    web.env->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &response);
                } else web.env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
            } else {
                LogFromPage("blocked " + url.substr(0, 200) + " (only the mode's ui files load)");
                web.env->CreateWebResourceResponse(nullptr, 403, L"Forbidden", L"", &response);
            }
            args->put_Response(response.Get());
            return S_OK;
        }).Get(), &t);
    web.view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR text = nullptr;
            if (FAILED(args->TryGetWebMessageAsString(&text)) || !text) return S_OK;
            const std::string message = Utf8(text);
            CoTaskMemFree(text);
            json::Value v;
            if (!json::Value::Parse(message, v) || !v["name"].isString()) return S_OK;
            const std::string name = v["name"].asString(), data = v["data"].dump();
            if (name.empty() || name.size() > net::kUiNameMax || data.size() > net::kUiFromPageMax) { LogFromPage("dgmp.send: name 1..64 characters, data up to 16 KB"); return S_OK; }
            const double now = double(GetTickCount64()) / 1000.0;
            if (now - web.windowStart >= 1.0) { web.windowStart = now; web.windowCount = 0; }
            if (++web.windowCount > kPageMessagesPerSecond) return S_OK;
            std::lock_guard g(outLock);
            outgoing.emplace_back(name, data);
            return S_OK;
        }).Get(), &t);
    web.comp->add_CursorChanged(Callback<ICoreWebView2CursorChangedEventHandler>(
        [](ICoreWebView2CompositionController* c, IUnknown*) -> HRESULT { HCURSOR cur = nullptr; c->get_Cursor(&cur); pageCursor = cur; return S_OK; }).Get(), &t);
    web.view->AddScriptToExecuteOnDocumentCreated(kBridge, nullptr);
    // The page thinks it has the focus (caret, :focus) although no window of it does.
    web.view->CallDevToolsProtocolMethod(L"Emulation.setFocusEmulationEnabled", L"{\"enabled\":true}", nullptr);
    Watch(L"Runtime.consoleAPICalled", [](const json::Value& v) {
        std::string line = v["type"].asString() + ":";
        const auto& list = v["args"];
        for (size_t i = 0; i < list.size(); ++i) {
            const auto& a = list[i];
            line += " " + (a["value"].isNull() ? a["description"].asString() : a["value"].isString() ? a["value"].asString() : a["value"].dump());
        }
        LogFromPage(line);
    });
    Watch(L"Runtime.exceptionThrown", [](const json::Value& v) {
        const auto& d = v["exceptionDetails"];
        LogFromPage("error: " + (d["exception"]["description"].isString() ? d["exception"]["description"].asString() : d["text"].asString()) +
                    " (" + d["url"].asString() + ":" + std::to_string(int(d["lineNumber"].asNumber()) + 1) + ")");
    });
    web.view->CallDevToolsProtocolMethod(L"Runtime.enable", L"{}", nullptr);
    web.creating = false;
    dg::Log("UI: browser ready (%dx%d)", web.w, web.h);
    Navigate();
}

void Fail(const char* what, HRESULT hr) {
    dg::Log("UI: %s failed (%08lX): the mode's HTML pages are not shown", what, static_cast<unsigned long>(hr));
    web.failed = true; web.creating = false; unavailable = true;
}

// The browser, on the first page the server opens (modes without pages pay nothing).
void Create() {
    if (web.view || web.creating || web.failed) return;
    web.creating = true;
    web.w = std::max(int(screenW), 64); web.h = std::max(int(screenH), 64);
    try {
        WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"DGMPWebUiHost";
        RegisterClassExW(&wc);
        web.host = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, web.w, web.h, nullptr, nullptr, wc.hInstance, nullptr);
        web.compositor = wuc::Compositor();
        web.root = web.compositor.CreateContainerVisual();
        web.root.Size({float(web.w), float(web.h)});
        // Our own device on the game's adapter (the frames are shared with the game's device).
        ComPtr<IDXGIAdapter> adapter;
        if (auto* game = static_cast<ID3D11Device*>(overlay::Device())) if (ComPtr<IDXGIDevice> dx; SUCCEEDED(game->QueryInterface(IID_PPV_ARGS(&dx)))) dx->GetAdapter(&adapter);
        HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &web.device, nullptr, &web.context);
        if (FAILED(hr)) return Fail("Direct3D device", hr);
        ComPtr<IDXGIDevice> dxgi; web.device.As(&dxgi);
        winrt::com_ptr<::IInspectable> inspectable;
        if (FAILED(hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()))) return Fail("capture device", hr);
        web.d3d = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();
        web.item = wgc::GraphicsCaptureItem::CreateFromVisual(web.root);
        web.pool = wgc::Direct3D11CaptureFramePool::Create(web.d3d, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, {web.w, web.h});
        web.pool.FrameArrived([](auto&&, auto&&) {
            while (auto f = web.pool.TryGetNextFrame()) web.frame = f;
            Publish();
        });
        web.session = web.pool.CreateCaptureSession(web.item);
        web.session.IsCursorCaptureEnabled(false);
        web.session.StartCapture();
        MakeShared(web.w, web.h);
    } catch (const winrt::hresult_error& e) {
        return Fail("capture", e.code());
    }
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(L"--autoplay-policy=no-user-gesture-required");
    const fs::path data = fs::path(profile::Root()) / L"cache" / L"webui-browser";
    std::error_code ec; fs::create_directories(data, ec);
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            ComPtr<ICoreWebView2Environment3> env3;
            if (FAILED(hr) || !env || FAILED(env->QueryInterface(IID_PPV_ARGS(&env3)))) { Fail("WebView2 runtime", hr); return S_OK; }
            web.env = env;
            env3->CreateCoreWebView2CompositionController(web.host, Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                [](HRESULT hr, ICoreWebView2CompositionController* c) -> HRESULT {
                    if (FAILED(hr) || !c) { Fail("WebView2 controller", hr); return S_OK; }
                    Setup(c);
                    return S_OK;
                }).Get());
            return S_OK;
        }).Get());
    if (FAILED(hr)) Fail("WebView2 runtime", hr);
}

DWORD WINAPI WebThread(void*) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    DispatcherQueueOptions dq{sizeof(dq), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    ABI::Windows::System::IDispatcherQueueController* dispatcher = nullptr;
    if (FAILED(CreateDispatcherQueueController(dq, &dispatcher))) { dg::Log("UI: no dispatcher queue"); web.failed = true; unavailable = true; }
    MSG m;
    PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);  // the queue exists from here: tasks posted earlier are run below
    for (;;) {
        // A frame the overlay has not taken yet: tried again soon (rarely while no page is shown).
        MsgWaitForMultipleObjectsEx(0, nullptr, !web.frame ? INFINITE : shown ? 4 : 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) return 0;
            if (m.message == WM_APP && !m.hwnd) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        std::vector<std::function<void()>> run;
        { std::lock_guard g(tasksLock); run.swap(tasks); }
        for (auto& fn : run) {
            try { fn(); } catch (const winrt::hresult_error& e) { dg::Log("UI: %08lX", static_cast<unsigned long>(e.code())); }
        }
        Publish();
    }
}
void Start() {
    if (started.exchange(true)) return;
    DWORD id = 0;
    if (HANDLE t = CreateThread(nullptr, 0, &WebThread, nullptr, CREATE_SUSPENDED, &id)) { webThreadId = id; ResumeThread(t); CloseHandle(t); }
}

// ---------------------------------------------------------------- input (the game window's thread)
int Modifiers() {
    return (GetKeyState(VK_MENU) < 0 ? 1 : 0) | (GetKeyState(VK_CONTROL) < 0 ? 2 : 0) | (GetKeyState(VK_SHIFT) < 0 ? 8 : 0);
}
// DOM key and code of a virtual key (the text itself comes with WM_CHAR).
void KeyNames(UINT vk, std::string& key, std::string& code) {
    static const std::map<UINT, std::pair<const char*, const char*>> named = {
        {VK_BACK, {"Backspace", "Backspace"}}, {VK_TAB, {"Tab", "Tab"}}, {VK_RETURN, {"Enter", "Enter"}}, {VK_SHIFT, {"Shift", "ShiftLeft"}},
        {VK_CONTROL, {"Control", "ControlLeft"}}, {VK_MENU, {"Alt", "AltLeft"}}, {VK_ESCAPE, {"Escape", "Escape"}}, {VK_SPACE, {" ", "Space"}},
        {VK_PRIOR, {"PageUp", "PageUp"}}, {VK_NEXT, {"PageDown", "PageDown"}}, {VK_END, {"End", "End"}}, {VK_HOME, {"Home", "Home"}},
        {VK_LEFT, {"ArrowLeft", "ArrowLeft"}}, {VK_UP, {"ArrowUp", "ArrowUp"}}, {VK_RIGHT, {"ArrowRight", "ArrowRight"}}, {VK_DOWN, {"ArrowDown", "ArrowDown"}},
        {VK_INSERT, {"Insert", "Insert"}}, {VK_DELETE, {"Delete", "Delete"}}, {VK_CAPITAL, {"CapsLock", "CapsLock"}}};
    if (auto it = named.find(vk); it != named.end()) { key = it->second.first; code = it->second.second; return; }
    if (vk >= VK_F1 && vk <= VK_F24) { key = code = "F" + std::to_string(vk - VK_F1 + 1); return; }
    if (vk >= 'A' && vk <= 'Z') { const bool upper = (GetKeyState(VK_SHIFT) < 0) != ((GetKeyState(VK_CAPITAL) & 1) != 0); key = std::string(1, char(upper ? vk : vk + 32)); code = std::string("Key") + char(vk); return; }
    if (vk >= '0' && vk <= '9') { key = std::string(1, char(vk)); code = std::string("Digit") + char(vk); return; }
    const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
    key = ch >= 0x20 && ch < 0x7F ? std::string(1, char(ch)) : "Unidentified";
    code.clear();
}
void Key(const char* type, UINT vk, bool repeat) {
    std::string key, code; KeyNames(vk, key, code);
    std::string p = std::string("{\"type\":\"") + type + "\",\"windowsVirtualKeyCode\":" + std::to_string(vk) + ",\"nativeVirtualKeyCode\":" + std::to_string(vk) +
                    ",\"key\":" + Quote(key) + (code.empty() ? "" : ",\"code\":" + Quote(code)) + ",\"modifiers\":" + std::to_string(Modifiers()) +
                    (repeat ? ",\"autoRepeat\":true" : "") + "}";
    Post([p] { if (web.view) web.view->CallDevToolsProtocolMethod(L"Input.dispatchKeyEvent", Wide(p).c_str(), nullptr); });
}
void Char(wchar_t c) {
    static wchar_t high = 0;
    if (c >= 0xD800 && c <= 0xDBFF) { high = c; return; }
    wchar_t text[3] = {0};
    if (c >= 0xDC00 && c <= 0xDFFF) { if (!high) return; text[0] = high; text[1] = c; high = 0; } else text[0] = c;
    if (text[0] < 0x20 && text[0] != L'\r') return;  // (Backspace, Tab, Escape... act as keys)
    const std::string s = Utf8(text);
    std::string p = "{\"type\":\"char\",\"text\":" + Quote(s) + ",\"unmodifiedText\":" + Quote(s) + ",\"key\":" + Quote(text[0] == L'\r' ? "Enter" : s) +
                    ",\"windowsVirtualKeyCode\":" + std::to_string(int(text[0])) + ",\"modifiers\":" + std::to_string(Modifiers()) + "}";
    Post([p] { if (web.view) web.view->CallDevToolsProtocolMethod(L"Input.dispatchKeyEvent", Wide(p).c_str(), nullptr); });
}
void Input(void* window, unsigned msg, unsigned long long w, long long l) {
    if (!started) return;
    const WPARAM wp = WPARAM(w); const LPARAM lp = LPARAM(l);
    switch (msg) {
    case WM_SETCURSOR: { const HCURSOR c = pageCursor; SetCursor(c ? c : LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))); return; }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: Key("rawKeyDown", UINT(wp), (lp & (1 << 30)) != 0); return;
    case WM_KEYUP: case WM_SYSKEYUP: Key("keyUp", UINT(wp), false); return;
    case WM_CHAR: Char(wchar_t(wp)); return;
    default: break;
    }
    if (msg < WM_MOUSEFIRST || msg > WM_MOUSELAST) return;
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) ScreenToClient(static_cast<HWND>(window), &pt);
    UINT32 data = 0;
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) data = UINT32(int16_t(GET_WHEEL_DELTA_WPARAM(wp)));
    else if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP || msg == WM_XBUTTONDBLCLK) data = GET_XBUTTON_WPARAM(wp);
    const auto keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS(GET_KEYSTATE_WPARAM(wp));
    const auto kind = COREWEBVIEW2_MOUSE_EVENT_KIND(msg);  // (the kinds are the window messages)
    Post([kind, keys, data, pt] { if (web.comp) web.comp->SendMouseInput(kind, keys, data, pt); });
}
bool FocusedForOverlay() { return focused && shown; }

// ---------------------------------------------------------------- drawing (the thread that presents)
struct Reader {
    uint64_t gen = 0;
    ComPtr<ID3D11Texture2D> shared, local;
    ComPtr<IDXGIKeyedMutex> mutex;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11BlendState> premultiplied;
    bool hasFrame = false;
};
Reader& reader = *new Reader;  // (never destroyed: the game's device may be gone at exit)  // (never destroyed: the game's device may be gone at exit)

void Draw() {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const int sw = int(size.x), sh = int(size.y);
    if (sw > 0 && sh > 0 && (sw != screenW || sh != screenH)) {
        screenW = sw; screenH = sh;
        if (started) Post([sw, sh] { Resize(sw, sh); });
    }
    if (!shown) return;
    auto* device = static_cast<ID3D11Device*>(overlay::Device());
    auto* context = static_cast<ID3D11DeviceContext*>(overlay::Context());
    if (!device || !context) return;
    SharedInfo info; { std::lock_guard g(sharedLock); info = sharedInfo; }
    if (info.gen != reader.gen) {
        reader.gen = info.gen; reader.shared.Reset(); reader.mutex.Reset(); reader.local.Reset(); reader.view.Reset(); reader.hasFrame = false;
        if (info.handle && SUCCEEDED(device->OpenSharedResource(info.handle, IID_PPV_ARGS(&reader.shared)))) {
            reader.shared.As(&reader.mutex);
            D3D11_TEXTURE2D_DESC d; reader.shared->GetDesc(&d);
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0;
            if (SUCCEEDED(device->CreateTexture2D(&d, nullptr, &reader.local))) device->CreateShaderResourceView(reader.local.Get(), nullptr, &reader.view);
        }
    }
    if (reader.mutex && reader.local && reader.mutex->AcquireSync(1, 0) == S_OK) {
        context->CopyResource(reader.local.Get(), reader.shared.Get());
        reader.mutex->ReleaseSync(0);
        reader.hasFrame = true;
    }
    if (!reader.hasFrame || !reader.view) return;
    if (!reader.premultiplied) {
        D3D11_BLEND_DESC b{};
        b.RenderTarget[0].BlendEnable = TRUE;
        b.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&b, &reader.premultiplied);
    }
    // The frames' colours are premultiplied by their alpha: ImGui's straight-alpha blending would darken the edges.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddCallback([](const ImDrawList*, const ImDrawCmd*) {
        const float factor[4] = {0, 0, 0, 0};
        static_cast<ID3D11DeviceContext*>(overlay::Context())->OMSetBlendState(reader.premultiplied.Get(), factor, 0xFFFFFFFF);
    }, nullptr);
    dl->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(reader.view.Get())), ImVec2(0, 0), size);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}
}  // namespace

// ---------------------------------------------------------------- game thread
void OnList(const std::vector<std::pair<std::string, std::string>>& list, net::Client& client) {
    {
        std::lock_guard g(filesLock);
        files.clear(); arriving.clear();
        std::error_code ec;
        for (auto& [path, hash] : list) {
            if (!SafeHash(hash)) continue;
            files[path] = hash;
            if (!fs::exists(CacheDir() / hash, ec)) { client.SendUiRequest(path); arriving[path] = {hash, {}, 0}; }
        }
        if (!files.empty()) dg::Log("UI files: %zu of the mode, %zu to download", files.size(), arriving.size());
    }
    // The mode (re)started: whatever page it had open is gone.
    pending = PendingOpen{}; focused = false;
    if (started) Post([] { web.page.clear(); web.queued.clear(); Navigate(); });
}

void OnData(const std::string& path, int part, int parts, const std::string& bytes) {
    std::lock_guard g(filesLock);
    auto it = arriving.find(path);
    if (it == arriving.end() || part != it->second.next || parts <= 0) return;
    it->second.bytes += bytes; ++it->second.next;
    if (it->second.next < parts) return;
    std::error_code ec; fs::create_directories(CacheDir(), ec);
    const fs::path file = CacheDir() / it->second.hash, temp = file.wstring() + L".part";
    { std::ofstream out(temp, std::ios::binary | std::ios::trunc); out << it->second.bytes; }
    fs::rename(temp, file, ec);
    arriving.erase(it);
}

void OnUi(net::UiOp op, const std::string& name, const std::string& text, bool focus) {
    switch (op) {
    case net::UiOp::Open:
        pending = PendingOpen{true, text, {}};
        focused = focus;
        Start();
        break;
    case net::UiOp::Close:
        pending = PendingOpen{}; focused = false;
        if (started) Post([] { web.page.clear(); web.queued.clear(); Navigate(); });
        break;
    case net::UiOp::Focus:
        focused = focus;
        if (started) Post([focus] { if (!web.page.empty()) ToPage("focus", focus ? "true" : "false"); });
        break;
    case net::UiOp::Event:
        if (pending.on) { if (pending.messages.size() < 256) pending.messages.push_back("{\"name\":" + Quote(name) + ",\"data\":" + text + "}"); }
        else if (started) Post([name, text] { ToPage(name, text); });
        break;
    }
}

void Reset() {
    pending = PendingOpen{}; focused = false;
    { std::lock_guard g(filesLock); files.clear(); arriving.clear(); }
    { std::lock_guard g(outLock); outgoing.clear(); }
    if (started) Post([] { web.page.clear(); web.queued.clear(); Navigate(); });
}

void Update(net::Client& client) {
    std::vector<std::pair<std::string, std::string>> out;
    { std::lock_guard g(outLock); out.swap(outgoing); }
    for (auto& [name, json] : out) client.SendUiMessage(name, json);
    if (pending.on && FilesReady()) {
        Post([page = pending.page, messages = pending.messages] {
            web.page = page; web.queued = messages;
            if (web.view) Navigate(); else Create();
        });
        pending = PendingOpen{};
    }
    if (unavailable && !told.exchange(true))
        hud::AddMessage(0xE07070FF, lang::Pick(L"Интерфейс режима не открылся: нужен Microsoft Edge WebView2 Runtime (его ставит лаунчер DGMP).",
                                               L"The mode's interface did not open: it needs the Microsoft Edge WebView2 Runtime (the DGMP launcher installs it)."));
    // The cursor is the page's while it has the focus (a dialog of the mode over it still gets it first).
    static bool owned = false;
    if (Focused()) {
        if (!overlay::Visible()) overlay::SetVisible(true);
        if (!overlay::CursorMode()) overlay::SetCursorMode(true);
        owned = true;
    } else if (owned) {
        owned = false;
        if (mpui::DialogOpen() || skins::CreatorOpen()) return;  // (they keep the cursor)
        overlay::SetCursorMode(false);
        overlay::SetVisible(false);
    }
}

bool Focused() { return focused && started && !unavailable; }

void Install() { overlay::SetPage(&Draw, &FocusedForOverlay, &Input); }

}  // namespace webui
