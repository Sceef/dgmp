#include "security.h"
#include "../shared/json.h"
#include "platform.h"
#include <algorithm>
#include <cctype>
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
