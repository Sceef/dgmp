#include "security.h"
#include "../shared/json.h"
#include "platform.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <random>
#include <fstream>
#include <sstream>
#include <thread>

namespace srv {
namespace {
std::string Lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

json::Value ReadJson(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary); std::stringstream ss; ss << in.rdbuf();
    json::Value v;
    if (ss.str().empty() || !json::Value::Parse(ss.str(), v)) return {};
    return v;
}
void WriteJson(const std::filesystem::path& file, const json::Value& v) {
    std::error_code ec; std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file; tmp += ".tmp";
    { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); out << v.dump(); }
    std::filesystem::rename(tmp, file, ec);
}
}  // namespace

std::string SteamIdText(uint64_t id) { return id ? std::to_string(id) : std::string(); }

bool ParseSteamId(const std::string& text, uint64_t& id) {
    if (text.size() != 17 || text.compare(0, 4, "7656") != 0 || !std::all_of(text.begin(), text.end(), ::isdigit)) return false;
    id = std::stoull(text);
    return true;
}

bool LooksLikeIp(const std::string& text) {
    if (text.find(':') != std::string::npos) return std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) || c == ':' || c == '.'; });
    int dots = 0;
    for (char c : text) { if (c == '.') ++dots; else if (!std::isdigit(static_cast<unsigned char>(c))) return false; }
    return dots == 3;
}

// ---------------------------------------------------------------- bans
void BanList::Load(const std::filesystem::path& file) {
    file_ = file; bans_.clear();
    const auto v = ReadJson(file);
    for (auto& e : v.elements()) {
        Ban b;
        b.steamId = e.str("steamId"); b.ip = e.str("ip"); b.nick = e.str("nick"); b.reason = e.str("reason"); b.by = e.str("by");
        b.at = static_cast<int64_t>(e["at"].asNumber()); b.until = static_cast<int64_t>(e["until"].asNumber());
        if (!b.steamId.empty() || !b.ip.empty() || !b.nick.empty()) bans_.push_back(b);
    }
}

void BanList::Save() const {
    json::Value v = json::Value::Array();
    for (auto& b : bans_) {
        json::Value e = json::Value::Object();
        if (!b.steamId.empty()) e.set("steamId", b.steamId);  // text: a SteamID64 does not fit a JSON number exactly
        if (!b.ip.empty()) e.set("ip", b.ip);
        if (!b.nick.empty()) e.set("nick", b.nick);
        e.set("reason", b.reason).set("by", b.by).set("at", double(b.at)).set("until", double(b.until));
        v.push(e);
    }
    WriteJson(file_, v);
}

const Ban* BanList::Find(const std::string& steamId, const std::string& ip, const std::string& nick, int64_t now) {
    const auto before = bans_.size();
    bans_.erase(std::remove_if(bans_.begin(), bans_.end(), [now](const Ban& b) { return b.until && b.until <= now; }), bans_.end());
    if (bans_.size() != before) Save();
    const std::string nickKey = Lower(nick);
    for (auto& b : bans_)
        if ((!b.steamId.empty() && b.steamId == steamId) || (!b.ip.empty() && b.ip == ip) || (!b.nick.empty() && Lower(b.nick) == nickKey)) return &b;
    return nullptr;
}

void BanList::Add(Ban b) { bans_.push_back(std::move(b)); Save(); }

int BanList::Remove(const std::string& what) {
    const std::string key = Lower(what);
    const auto before = bans_.size();
    bans_.erase(std::remove_if(bans_.begin(), bans_.end(), [&](const Ban& b) { return b.steamId == what || b.ip == what || (!b.nick.empty() && Lower(b.nick) == key); }), bans_.end());
    const int lifted = int(before - bans_.size());
    if (lifted) Save();
    return lifted;
}

// ---------------------------------------------------------------- nicknames
void NameBook::Load(const std::filesystem::path& file) {
    file_ = file; names_.clear();
    const auto v = ReadJson(file);
    for (auto& [nick, id] : v.items()) if (id.isString()) names_.emplace_back(nick, id.asString());
    std::sort(names_.begin(), names_.end());
}
void NameBook::Save() const {
    json::Value v = json::Value::Object();
    for (auto& [nick, id] : names_) v.set(nick, id);
    WriteJson(file_, v);
}
std::string NameBook::Owner(const std::string& nickKey) const {
    auto it = std::lower_bound(names_.begin(), names_.end(), std::make_pair(nickKey, std::string()));
    return it != names_.end() && it->first == nickKey ? it->second : std::string();
}
void NameBook::Claim(const std::string& nickKey, const std::string& steamId) {
    auto it = std::lower_bound(names_.begin(), names_.end(), std::make_pair(nickKey, std::string()));
    if (it != names_.end() && it->first == nickKey) { if (it->second == steamId) return; it->second = steamId; }
    else names_.insert(it, {nickKey, steamId});
    Save();
}
bool NameBook::Release(const std::string& nickKey) {
    auto it = std::lower_bound(names_.begin(), names_.end(), std::make_pair(nickKey, std::string()));
    if (it == names_.end() || it->first != nickKey) return false;
    names_.erase(it); Save();
    return true;
}

// ---------------------------------------------------------------- passwords
namespace {
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block[64]; size_t used = 0; uint64_t total = 0;
    static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void Compress(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void Update(const void* data, size_t n) {
        auto p = static_cast<const uint8_t*>(data);
        total += n;
        while (n) {
            const size_t take = std::min(n, 64 - used);
            std::memcpy(block + used, p, take); used += take; p += take; n -= take;
            if (used == 64) { Compress(block); used = 0; }
        }
    }
    void Final(uint8_t out[32]) {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        Update(&one, 1);
        while (used != 56) Update(&zero, 1);
        uint8_t len[8]; for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - i * 8));
        Update(len, 8);
        for (int i = 0; i < 32; ++i) out[i] = uint8_t(h[i / 4] >> (24 - i % 4 * 8));
    }
};
// HMAC-SHA256 with the key's inner and outer states prepared once (PBKDF2 calls it many times).
struct Hmac {
    Sha256 inner, outer;
    explicit Hmac(const std::string& key) {
        uint8_t k[64] = {};
        if (key.size() > 64) { Sha256 s; s.Update(key.data(), key.size()); s.Final(k); } else std::memcpy(k, key.data(), key.size());
        uint8_t ipad[64], opad[64];
        for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
        inner.Update(ipad, 64); outer.Update(opad, 64);
    }
    void Mac(const void* data, size_t n, uint8_t out[32]) const {
        Sha256 i = inner, o = outer; uint8_t mid[32];
        i.Update(data, n); i.Final(mid);
        o.Update(mid, 32); o.Final(out);
    }
};
std::string Hex(const uint8_t* p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string s; for (size_t i = 0; i < n; ++i) { s += digits[p[i] >> 4]; s += digits[p[i] & 15]; }
    return s;
}
bool Unhex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        const int a = nib(s[i]), b = nib(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(uint8_t(a << 4 | b));
    }
    return true;
}
void Pbkdf2(const std::string& password, const std::vector<uint8_t>& salt, int iterations, uint8_t out[32]) {
    const Hmac mac(password);
    std::vector<uint8_t> first(salt); first.insert(first.end(), {0, 0, 0, 1});  // block 1: 32 bytes are enough
    uint8_t u[32]; mac.Mac(first.data(), first.size(), u);
    std::memcpy(out, u, 32);
    for (int i = 1; i < iterations; ++i) { mac.Mac(u, 32, u); for (int k = 0; k < 32; ++k) out[k] ^= u[k]; }
}
constexpr int kPasswordIterations = 60000;
}  // namespace

std::string PasswordHash(const std::string& password) {
    std::random_device rd;
    std::vector<uint8_t> salt(16);
    for (auto& b : salt) b = uint8_t(rd());
    uint8_t dk[32]; Pbkdf2(password, salt, kPasswordIterations, dk);
    return "pbkdf2-sha256$" + std::to_string(kPasswordIterations) + "$" + Hex(salt.data(), salt.size()) + "$" + Hex(dk, 32);
}

bool PasswordCheck(const std::string& password, const std::string& stored) {
    // pbkdf2-sha256$<iterations>$<salt>$<hash>
    const auto a = stored.find('$'), b = stored.find('$', a + 1), c = stored.find('$', b + 1);
    if (a == std::string::npos || b == std::string::npos || c == std::string::npos || stored.substr(0, a) != "pbkdf2-sha256") return false;
    const int iterations = std::atoi(stored.substr(a + 1, b - a - 1).c_str());
    std::vector<uint8_t> salt, expect;
    if (iterations < 1 || iterations > 10000000 || !Unhex(stored.substr(b + 1, c - b - 1), salt) || !Unhex(stored.substr(c + 1), expect) || expect.size() != 32) return false;
    uint8_t dk[32]; Pbkdf2(password, salt, iterations, dk);
    uint8_t diff = 0; for (int i = 0; i < 32; ++i) diff |= dk[i] ^ expect[i];
    return diff == 0;
}

// ---------------------------------------------------------------- Steam tickets
SteamTicketCheck::Result SteamTicketCheck::Parse(const std::string& body) {
    Result r;
    json::Value v;
    if (!json::Value::Parse(body, v)) { r.error = "unreadable answer"; return r; }
    const auto& response = v["response"];
    if (const auto& err = response["error"]; err.isObject()) {
        const int code = err["errorcode"].asInt();
        r.error = std::to_string(code) + " " + err.str("errordesc");
        // 101 "Invalid ticket" also comes for a ticket Steam has not activated yet (just made): try again.
        r.kind = code == 101 ? Result::Kind::Retry : Result::Kind::Invalid;
        return r;
    }
    const auto& params = response["params"];
    if (params.str("result") != "OK" || !ParseSteamId(params.str("steamid"), r.steamId)) { r.kind = Result::Kind::Invalid; r.error = "result " + params.str("result"); return r; }
    ParseSteamId(params.str("ownersteamid"), r.ownerId);
    r.vacBanned = params["vacbanned"].asBool(); r.publisherBanned = params["publisherbanned"].asBool();
    r.kind = Result::Kind::Ok;
    return r;
}

void SteamTicketCheck::Start(uint64_t tag, const std::string& apiKey, uint32_t appId, const std::string& ticketHex) {
    const std::string url = "https://api.steampowered.com/ISteamUserAuth/AuthenticateUserTicket/v1/?key=" + apiKey +
                            "&appid=" + std::to_string(appId) + "&ticket=" + ticketHex;
    std::thread([shared = shared_, tag, url] {
        std::string body, error;
        Result r;
        const bool ok = HttpGet(url, body, error, 10);
        if (ok) r = Parse(body);
        else {
            r.kind = Result::Kind::Unreachable;
            r.error = error == "HTTP 403" || error == "HTTP 401" ? "the Steam Web API key was refused (" + error + ")" : error;
        }
        r.tag = tag;
        std::lock_guard<std::mutex> g(shared->lock);
        shared->done.push_back(std::move(r));
    }).detach();
}

std::vector<SteamTicketCheck::Result> SteamTicketCheck::Poll() {
    std::lock_guard<std::mutex> g(shared_->lock);
    std::vector<Result> out; out.swap(shared_->done);
    return out;
}

}  // namespace srv
