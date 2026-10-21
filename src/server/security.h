#pragma once
// Who the players are and who may play: Steam auth tickets checked with the Steam Web API, the ban
// list (data/bans.json) and nicknames bound to the Steam account that took them first (data/names.json).
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace srv {

std::string SteamIdText(uint64_t id);                         // "" for 0
bool ParseSteamId(const std::string& text, uint64_t& id);     // a SteamID64: 17 digits, 7656...
bool LooksLikeIp(const std::string& text);                    // 1.2.3.4 or an IPv6 address

// A ban: by Steam account, address or nickname (any of them matches). Times are Unix seconds.
struct Ban {
    std::string steamId, ip, nick, reason, by;
    int64_t at = 0, until = 0;  // until 0: for ever
};

class BanList {
public:
    void Load(const std::filesystem::path& file);
    const Ban* Find(const std::string& steamId, const std::string& ip, const std::string& nick, int64_t now);  // drops expired bans
    void Add(Ban b);
    int Remove(const std::string& what);  // a SteamID, an address or a nickname: how many bans were lifted
    const std::vector<Ban>& All() const { return bans_; }
    void Save() const;
private:
    std::filesystem::path file_;
    std::vector<Ban> bans_;
};

// Nickname -> the Steam account that played under it first; others with that name are turned away.
class NameBook {
public:
    void Load(const std::filesystem::path& file);
    std::string Owner(const std::string& nickKey) const;   // SteamID text or ""
    void Claim(const std::string& nickKey, const std::string& steamId);
    bool Release(const std::string& nickKey);
private:
    void Save() const;
    std::filesystem::path file_;
    std::vector<std::pair<std::string, std::string>> names_;  // sorted by nick
};

// Checks auth session tickets with ISteamUserAuth/AuthenticateUserTicket on worker threads.
class SteamTicketCheck {
public:
    struct Result {
        uint64_t tag = 0;
        enum class Kind { Ok, Invalid, Retry, Unreachable } kind = Kind::Unreachable;
        uint64_t steamId = 0, ownerId = 0;  // owner: the account that owns the game (family sharing)
        bool vacBanned = false, publisherBanned = false;
        std::string error;
    };
    void Start(uint64_t tag, const std::string& apiKey, uint32_t appId, const std::string& ticketHex);
    std::vector<Result> Poll();  // finished checks
    static Result Parse(const std::string& body);  // the Web API answer
private:
    struct Shared { std::mutex lock; std::vector<Result> done; };
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();  // outlives the server for threads still running
};

}  // namespace srv
