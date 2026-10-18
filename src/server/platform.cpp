#include "platform.h"
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace srv {
namespace {
std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// Notepad's "Unicode" (UTF-16 LE after a BOM) to UTF-8.
std::string FromUtf16(const std::string& bytes) {
    std::string out;
    for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
        uint32_t cp = uint8_t(bytes[i]) | uint8_t(bytes[i + 1]) << 8;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < bytes.size()) {
            const uint32_t lo = uint8_t(bytes[i + 2]) | uint8_t(bytes[i + 3]) << 8;
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
        }
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | cp >> 6); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | cp >> 12); out += char(0x80 | (cp >> 6 & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | cp >> 18); out += char(0x80 | (cp >> 12 & 0x3F)); out += char(0x80 | (cp >> 6 & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    return out;
}
}  // namespace

#ifdef _WIN32
std::string Utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::filesystem::path ExecutableDir() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path();
}

FILE* OpenAppend(const std::filesystem::path& file) {
    FILE* f = nullptr; _wfopen_s(&f, file.c_str(), L"a");
    return f;
}

std::tm LocalTime(std::time_t t) { std::tm tm{}; localtime_s(&tm, &t); return tm; }

// The system parser, as before the port (a file without a BOM is read in the ANSI code page).
std::string IniValue(const std::filesystem::path& file, const char* section, const char* key, const char* fallback) {
    auto wide = [](const char* s) { return std::wstring(s, s + std::strlen(s)); };  // names and defaults are ASCII
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(wide(section).c_str(), wide(key).c_str(), wide(fallback).c_str(), buf, 512, file.c_str());
    return Utf8(buf);
}
#else
std::filesystem::path ExecutableDir() {
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path() : exe.parent_path();
}

FILE* OpenAppend(const std::filesystem::path& file) { return std::fopen(file.c_str(), "a"); }

std::tm LocalTime(std::time_t t) { std::tm tm{}; localtime_r(&t, &tm); return tm; }

std::string IniValue(const std::filesystem::path& file, const char* section, const char* key, const char* fallback) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return fallback;
    std::stringstream ss; ss << in.rdbuf();
    return IniParse(ss.str(), section, key, fallback);
}
#endif

std::filesystem::path Utf8Path(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

int StrICmp(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const int ca = std::tolower(static_cast<unsigned char>(*a)), cb = std::tolower(static_cast<unsigned char>(*b));
        if (ca != cb || !ca) return ca - cb;
    }
}

// What GetPrivateProfileString does: names compared case-insensitively, spaces around names and
// values trimmed, one pair of matching quotes removed, the first section of that name only.
std::string IniParse(std::string text, const char* section, const char* key, const char* fallback) {
    if (text.size() >= 2 && uint8_t(text[0]) == 0xFF && uint8_t(text[1]) == 0xFE) text = FromUtf16(text);
    else if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    std::istringstream in(text);
    bool inside = false, seen = false;
    for (std::string line; std::getline(in, line);) {
        line = Trim(line);
        if (!line.empty() && line[0] == '[') {
            if (seen) break;
            const auto end = line.find(']');
            inside = seen = StrICmp(Trim(line.substr(1, end == std::string::npos ? std::string::npos : end - 1)).c_str(), section) == 0;
            continue;
        }
        const auto eq = line.find('=');
        if (!inside || eq == std::string::npos || StrICmp(Trim(line.substr(0, eq)).c_str(), key) != 0) continue;
        std::string value = Trim(line.substr(eq + 1));
        if (value.size() >= 2 && (value[0] == '"' || value[0] == '\'') && value.back() == value[0]) value = value.substr(1, value.size() - 2);
        return value;
    }
    return fallback;
}

std::string Sha256Hex(const void* data, size_t size) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    // The message, 0x80, zero padding and the big-endian bit length fill whole 64-byte blocks.
    std::vector<uint8_t> m(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    const uint64_t bits = uint64_t(size) * 8;
    for (int i = 7; i >= 0; --i) m.push_back(uint8_t(bits >> (i * 8)));
    for (size_t at = 0; at < m.size(); at += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(m[at + i * 4]) << 24 | uint32_t(m[at + i * 4 + 1]) << 16 | uint32_t(m[at + i * 4 + 2]) << 8 | m[at + i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", (h[i / 4] >> (24 - i % 4 * 8)) & 0xFF);
    return std::string(hex, 64);
}

}  // namespace srv
