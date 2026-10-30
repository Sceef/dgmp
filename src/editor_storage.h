#pragma once
// Editor documents: flushed atomic replacement, previous version, and a recovery copy.
// Kept independent of the engine so disk-full, sharing and concurrent-edit cases can be tested.
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace editor_storage {
namespace fs = std::filesystem;
struct Disk { bool ok = false, exists = false; std::string text; };
inline Disk Read(const fs::path& path) {
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) return {};
    if (!exists) return {true, false, {}};
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string text; char bytes[16384]; DWORD count = 0; bool ok = true;
    for (;;) {
        if (!ReadFile(file, bytes, sizeof(bytes), &count, nullptr)) { ok = false; break; }
        if (!count) break;
        text.append(bytes, count);
    }
    CloseHandle(file);
    return {ok, true, std::move(text)};
}
inline bool Replace(const fs::path& path, const std::string& text) {
    std::error_code ec; fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    static std::atomic<unsigned> sequence{0};
    const fs::path temp = path.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true; size_t offset = 0;
    while (offset < text.size()) {
        const DWORD size = DWORD((std::min)(text.size() - offset, size_t(1024 * 1024)));
        DWORD written = 0;
        if (!::WriteFile(file, text.data() + offset, size, &written, nullptr) || written != size) { ok = false; break; }
        offset += written;
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    if (!CloseHandle(file)) ok = false;
    if (ok) {
        ok = false;
        for (int attempt = 0; attempt < 6; ++attempt) {
            if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { ok = true; break; }
            const DWORD error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) break;
            if (attempt < 5) Sleep(10);
        }
    }
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}
inline bool Write(const fs::path& path, const std::string& text, bool backup = true) {
    const Disk old = Read(path);
    if (!old.ok) return false;
    if (old.exists && old.text == text) return true;
    if (backup && old.exists && !Replace(path.wstring() + L".bak", old.text)) return false;
    return Replace(path, text);
}
enum class Result { Unchanged, Saved, RecoveryOnly, Failed };
struct Document {
    fs::path path;
    std::string saved;
    Disk baseline;
    void Load(const fs::path& file, const std::string& canonical) { path = file; saved = canonical; baseline = Read(file); }
    bool Dirty(const std::string& text) const { return !path.empty() && text != saved; }
    Result Save(const std::string& text, bool automatic) {
        if (path.empty()) return Result::Failed;
        if (automatic && !Dirty(text)) return Result::Unchanged;
        // Even a conflict or locked destination leaves an independent recovery document.
        if (!Write(path.parent_path() / L".autosave" / path.filename(), text)) return Result::Failed;
        const Disk current = Read(path);
        if (automatic && (!baseline.ok || !current.ok || current.exists != baseline.exists || current.text != baseline.text))
            return Result::RecoveryOnly;
        if (!Write(path, text)) return Result::RecoveryOnly;
        saved = text; baseline = {true, true, text};
        return Result::Saved;
    }
};
inline int Interval(int minutes) { return minutes == 0 || minutes == 1 || minutes == 5 || minutes == 10 ? minutes : 1; }
struct Schedule {
    int minutes = 1;
    double next = 0;
    void Set(int value, double now) { minutes = Interval(value); next = now + minutes * 60; }
    bool Due(double now) { if (!minutes || now < next) return false; next = now + minutes * 60; return true; }
};
}
