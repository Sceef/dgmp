// Feasibility probe for the in-game HTML UI: a WebView2 in visual hosting mode, its visual captured
// with Windows.Graphics.Capture into D3D11 textures (no window on screen). Writes build\webprobe.bmp.
// Build (x64 developer prompt, in dgmp):
//   cl /std:c++20 /EHsc /utf-8 /MT /Ithird_party\webview2\build\native\include src\tools\webprobe.cpp /Fe"build\webprobe.exe" /link
//      third_party\webview2\build\native\x64\WebView2LoaderStatic.lib windowsapp.lib CoreMessaging.lib d3d11.lib dxgi.lib user32.lib ole32.lib advapi32.lib shell32.lib version.lib
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl.h>
#include <DispatcherQueue.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include "WebView2.h"
#include <cstdio>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace wuc = winrt::Windows::UI::Composition;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

int main() {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    DispatcherQueueOptions dq{sizeof(dq), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    ABI::Windows::System::IDispatcherQueueController* dqc = nullptr;
    if (FAILED(CreateDispatcherQueueController(dq, &dqc))) { puts("no dispatcher queue"); return 1; }
    wuc::Compositor compositor;
    wuc::ContainerVisual root = compositor.CreateContainerVisual();
    const int W = 800, H = 600;
    root.Size({float(W), float(H)});

    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"DGMPWebProbe";
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) { puts("no d3d"); return 1; }
    ComPtr<IDXGIDevice> dxgi; dev->QueryInterface(IID_PPV_ARGS(&dxgi));
    winrt::com_ptr<::IInspectable> insp;
    CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put());
    auto d3d = insp.as<wgd::Direct3D11::IDirect3DDevice>();

    ComPtr<ICoreWebView2CompositionController> comp;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
    bool loaded = false;
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const std::wstring data = std::wstring(tmp) + L"dgmp-webprobe";
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([&](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            printf("env %08lX\n", hr);
            ComPtr<ICoreWebView2Environment3> env3;
            if (FAILED(hr) || FAILED(env->QueryInterface(IID_PPV_ARGS(&env3)))) return S_OK;
            env3->CreateCoreWebView2CompositionController(host, Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                [&](HRESULT hr, ICoreWebView2CompositionController* c) -> HRESULT {
                    printf("controller %08lX\n", hr);
                    if (FAILED(hr)) return S_OK;
                    comp = c;
                    comp.As(&controller);
                    controller->get_CoreWebView2(&web);
                    comp->put_RootVisualTarget(root.as<::IUnknown>().get());
                    if (ComPtr<ICoreWebView2Controller2> c2; SUCCEEDED(controller.As(&c2))) c2->put_DefaultBackgroundColor({0, 0, 0, 0});
                    RECT r{0, 0, W, H}; controller->put_Bounds(r); controller->put_IsVisible(TRUE);
                    EventRegistrationToken t;
                    web->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([&](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT { loaded = true; puts("loaded"); return S_OK; }).Get(), &t);
                    web->NavigateToString(L"<html><body style='margin:0;background:transparent'><div style='margin:40px;padding:30px;background:rgba(200,30,30,.5);color:#fff;font:40px Segoe UI;border-radius:20px'>Привет, DGMP <button>OK</button></div></body></html>");
                    return S_OK;
                }).Get());
            return S_OK;
        }).Get());
    printf("create %08lX\n", hr);

    wgc::GraphicsCaptureItem item = wgc::GraphicsCaptureItem::CreateFromVisual(root);
    auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(d3d, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
    auto session = pool.CreateCaptureSession(item);
    session.StartCapture();

    int frames = 0;
    wgc::Direct3D11CaptureFrame last{nullptr};
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 15000) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        if (auto f = pool.TryGetNextFrame()) { ++frames; last = f; }
        if (auto frame = last) {
            if (loaded && GetTickCount64() - start > 3000) {
                auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                ComPtr<ID3D11Texture2D> tex; access->GetInterface(IID_PPV_ARGS(&tex));
                D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
                d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
                ComPtr<ID3D11Texture2D> staging; dev->CreateTexture2D(&d, nullptr, &staging);
                ctx->CopyResource(staging.Get(), tex.Get());
                D3D11_MAPPED_SUBRESOURCE map; ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
                FILE* f = fopen("build\\webprobe.bmp", "wb");
                BITMAPFILEHEADER fh{0x4D42}; BITMAPINFOHEADER ih{sizeof(ih), LONG(d.Width), -LONG(d.Height), 1, 32};
                fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
                fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
                int opaque = 0, clear = 0;
                for (UINT y = 0; y < d.Height; ++y) {
                    const uint8_t* row = static_cast<uint8_t*>(map.pData) + y * map.RowPitch;
                    fwrite(row, 4, d.Width, f);
                    for (UINT x = 0; x < d.Width; ++x) { const uint8_t a = row[x * 4 + 3]; if (a == 255) ++opaque; else if (a == 0) ++clear; }
                }
                fclose(f);
                printf("frame %ux%u after %d frames: alpha 255 %d, alpha 0 %d, between %d\n", d.Width, d.Height, frames, opaque, clear, int(d.Width * d.Height) - opaque - clear);
                return 0;
            }
        }
        Sleep(5);
    }
    printf("timeout, frames %d loaded %d\n", frames, loaded);
    return 2;
}
