#include "images.h"
#include "overlay.h"
#include "profile.h"
#include "log.h"
#include <windows.h>
#include <wincodec.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace fs = std::filesystem;

namespace images {
namespace {
struct Pixels { std::vector<uint8_t> bgra; int w = 0, h = 0; void* texture = nullptr; };
std::mutex lock;
std::map<std::string, std::string> names;              // image name -> sha256 (the mode's list)
std::map<std::string, Pixels> decoded;                 // sha256 -> pixels / texture (kept across sessions)
struct Partial { std::string hash, bytes; int next = 0; };
std::map<std::string, Partial> arriving;               // image name -> parts so far

fs::path CacheDir() { return fs::path(profile::Root()) / L"cache" / L"images"; }
bool SafeHash(const std::string& h) { if (h.size() != 64) return false; for (char c : h) if (!isxdigit(static_cast<unsigned char>(c))) return false; return true; }

// PNG, JPG, GIF (its first frame) to straight-alpha BGRA through Windows Imaging Component.
bool Decode(const std::string& file, Pixels& out) {
    static thread_local bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;
    (void)com;
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory) return false;
    bool ok = false;
    IWICStream* stream = nullptr; IWICBitmapDecoder* decoder = nullptr; IWICBitmapFrameDecode* frame = nullptr; IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(file.data())), DWORD(file.size()))) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        if (w && h && w <= 2048 && h <= 2048) {
            out.w = int(w); out.h = int(h); out.bgra.resize(size_t(w) * h * 4);
            ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(out.bgra.size()), out.bgra.data()));
        }
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{conv, frame, decoder, stream, factory}) if (u) u->Release();
    return ok;
}
// From the cache file of that hash: true when it is there and decodes.
bool FromCache(const std::string& hash) {
    if (decoded.count(hash)) return true;
    std::ifstream in(CacheDir() / hash, std::ios::binary);
    if (!in) return false;
    std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Pixels px;
    if (!Decode(file, px)) return false;
    decoded[hash] = std::move(px);
    return true;
}
}  // namespace

void OnList(const std::vector<std::pair<std::string, std::string>>& list, net::Client& client) {
    std::lock_guard g(lock);
    names.clear(); arriving.clear();
    int asked = 0;
    for (auto& [name, hash] : list) {
        if (!SafeHash(hash)) continue;
        names[name] = hash;
        if (!FromCache(hash)) { client.SendImageRequest(name); arriving[name] = {hash, {}, 0}; ++asked; }
    }
    dg::Log("Images: %zu of the mode, %d to download", names.size(), asked);
}

void OnData(const std::string& name, int part, int parts, const std::string& bytes) {
    std::lock_guard g(lock);
    auto it = arriving.find(name);
    if (it == arriving.end() || part != it->second.next || parts <= 0) return;
    it->second.bytes += bytes; ++it->second.next;
    if (it->second.next < parts) return;
    const Partial done = std::move(it->second);
    arriving.erase(it);
    Pixels px;
    if (!Decode(done.bytes, px)) { dg::Log("Images: %s does not decode (PNG, JPG or GIF)", name.c_str()); return; }
    std::error_code ec; fs::create_directories(CacheDir(), ec);
    { std::ofstream out(CacheDir() / done.hash, std::ios::binary | std::ios::trunc); out << done.bytes; }
    decoded[done.hash] = std::move(px);
}

void Reset() { std::lock_guard g(lock); names.clear(); arriving.clear(); }

View Get(const std::string& name) {
    std::lock_guard g(lock);
    auto n = names.find(name);
    if (n == names.end()) return {};
    auto d = decoded.find(n->second);
    if (d == decoded.end()) return {};
    Pixels& px = d->second;
    if (!px.texture && !px.bgra.empty()) {
        px.texture = overlay::CreateTexture(px.bgra.data(), px.w, px.h);
        if (px.texture) { px.bgra.clear(); px.bgra.shrink_to_fit(); }
    }
    return {px.texture, float(px.w), float(px.h)};
}

}  // namespace images
