#include "master.h"
#include "../shared/protocol.h"
#include "platform.h"
#include <enet/enet.h>
#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <thread>
#include <vector>

namespace srv {
namespace {
double Now() {
    static const auto epoch = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count();
}
FILE* g_log = nullptr;
void Log(const char* fmt, ...) {
    char text[1024];
    va_list a; va_start(a, fmt); std::vsnprintf(text, sizeof(text), fmt, a); va_end(a);
    const std::tm tm = LocalTime(std::time(nullptr));
    char stamp[16]; std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    std::printf("[%s] %s\n", stamp, text); std::fflush(stdout);
    if (g_log) { std::fprintf(g_log, "[%s] %s\n", stamp, text); std::fflush(g_log); }
}
std::string Text(const ENetAddress& a) { char ip[64] = {}; enet_address_get_host_ip(&a, ip, sizeof(ip)); return std::string(ip) + ":" + std::to_string(a.port); }

struct Entry {
    ENetAddress address{};
    uint16_t protocol = 0;
    double lastHeartbeat = 0, lastAnswer = 0, lastQuery = 0;
    uint32_t token = 0;
    bool listed = false;          // answered a query: shown to launchers
    std::string name;
};
uint64_t Key(const ENetAddress& a) { return uint64_t(a.host) << 16 | a.port; }
}  // namespace

int RunMaster(uint16_t port, const std::filesystem::path& root, const std::atomic<bool>& quit) {
    std::error_code ec; std::filesystem::create_directories(root / "logs", ec);
    g_log = OpenAppend(root / "logs" / "master.log");
    if (enet_initialize() != 0) { Log("ENet initialisation failed"); return 1; }
    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    ENetAddress bind{}; bind.host = ENET_HOST_ANY; bind.port = port;
    if (s == ENET_SOCKET_NULL || enet_socket_bind(s, &bind) != 0) { Log("Cannot listen on UDP port %u (already in use?)", port); return 1; }
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    Log("DGMP master server listening on UDP %u", port);

    std::map<uint64_t, Entry> servers;
    std::map<uint32_t, double> heartbeatsFrom;  // per IP: rate limit
    std::mt19937 rng(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    constexpr size_t kMaxServers = 5000, kMaxPerIp = 32;
    auto send = [&](const ENetAddress& to, const std::vector<uint8_t>& data) {
        ENetBuffer b{}; b.data = const_cast<uint8_t*>(data.data()); b.dataLength = data.size();
        enet_socket_send(s, &to, &b, 1);
    };
    auto query = [&](Entry& e, double now) {
        e.token = rng(); e.lastQuery = now;
        send(e.address, net::BuildQuery(e.token));
    };

    double nextSweep = 0;
    while (!quit) {
        enet_uint32 cond = ENET_SOCKET_WAIT_RECEIVE;
        if (enet_socket_wait(s, &cond, 200) == 0 && (cond & ENET_SOCKET_WAIT_RECEIVE)) {
            for (;;) {
                uint8_t buf[1500]; ENetAddress from{};
                ENetBuffer b{}; b.data = buf; b.dataLength = sizeof(buf);
                const int n = enet_socket_receive(s, &from, &b, 1);
                if (n <= 0) break;
                const double now = Now();
                if (n == 8 && std::memcmp(buf, net::kHeartbeatMagic, 4) == 0) {
                    uint16_t gamePort, protocol; std::memcpy(&gamePort, buf + 4, 2); std::memcpy(&protocol, buf + 6, 2);
                    if (!gamePort) continue;
                    double& last = heartbeatsFrom[from.host];
                    if (now - last < 0.5) continue;  // one a half second per address is plenty (a host with many servers)
                    last = now;
                    ENetAddress game = from; game.port = gamePort;
                    auto it = servers.find(Key(game));
                    if (it == servers.end()) {
                        size_t sameIp = 0;
                        for (auto& [k, e] : servers) sameIp += e.address.host == from.host;
                        if (servers.size() >= kMaxServers || sameIp >= kMaxPerIp) continue;
                        Entry e; e.address = game;
                        it = servers.emplace(Key(game), e).first;
                        Log("Heartbeat from new server %s", Text(game).c_str());
                    }
                    it->second.lastHeartbeat = now; it->second.protocol = protocol;
                    if (it->second.lastQuery == 0 || now - it->second.lastQuery > 20) query(it->second, now);
                } else if (n == 10 && std::memcmp(buf, net::kListMagic, 4) == 0) {
                    uint32_t token; uint16_t protocol; std::memcpy(&token, buf + 4, 4); std::memcpy(&protocol, buf + 8, 2);
                    std::vector<const Entry*> list;
                    for (auto& [k, e] : servers) if (e.listed && (!protocol || e.protocol == protocol)) list.push_back(&e);
                    const uint16_t parts = uint16_t(std::max<size_t>(1, (list.size() + net::kListPerPacket - 1) / net::kListPerPacket));
                    for (uint16_t part = 0; part < parts; ++part) {
                        net::Writer w; w.Raw(net::kListReplyMagic, 4); w.U32(token); w.U16(part); w.U16(parts);
                        const size_t first = size_t(part) * net::kListPerPacket, count = std::min<size_t>(net::kListPerPacket, list.size() - std::min(first, list.size()));
                        w.U16(uint16_t(count));
                        for (size_t i = first; i < first + count; ++i) { w.U32(list[i]->address.host); w.U16(list[i]->address.port); }
                        send(from, w.buf);
                    }
                } else {
                    uint32_t token = 0; net::ServerInfo info;
                    if (!net::ParseReply(buf, size_t(n), token, info)) continue;
                    auto it = servers.find(Key(from));
                    if (it == servers.end() || it->second.token != token) continue;
                    Entry& e = it->second;
                    if (!e.listed) Log("Listed %s \"%s\" (%s, %u/%u)", Text(from).c_str(), info.name.c_str(), info.mode.c_str(), info.players, info.maxPlayers);
                    e.listed = true; e.lastAnswer = now; e.name = info.name; e.protocol = info.protocol;
                }
            }
        }
        const double now = Now();
        if (now < nextSweep) continue;
        nextSweep = now + 5;
        for (auto it = servers.begin(); it != servers.end();) {
            Entry& e = it->second;
            // Gone: no heartbeat for 3 minutes, or it stopped answering the queries (a heartbeat brings it back).
            const bool silent = now - e.lastHeartbeat > 180 || (e.listed && now - e.lastAnswer > 180);
            if (silent) { Log("Dropped %s \"%s\"", Text(e.address).c_str(), e.name.c_str()); it = servers.erase(it); continue; }
            if (now - e.lastQuery > 60) query(e, now);
            ++it;
        }
        for (auto it = heartbeatsFrom.begin(); it != heartbeatsFrom.end();) it = now - it->second > 600 ? heartbeatsFrom.erase(it) : std::next(it);
    }
    Log("Master server stopped");
    enet_socket_destroy(s);
    return 0;
}

}  // namespace srv
