// dgmp-probe.exe — test client for dgmp-server without the game.
//   dgmp-probe query <host[:port]> [...]     server status
//   dgmp-probe lan                           LAN discovery
//   dgmp-probe join <host[:port]> <nick> [password] [--seconds N] [--say text] [--auto]
//       stays connected, walks a small circle, prints server events; stdin lines are sent as chat, except:
//       !at x y z (stand there: z = ground), !walk (circle again), !use <interaction>, !dlg <id> <1|0> [item],
//       !pick <carry>, !put x y z (put the carried box there), !break x y z (it breaks in a roll).
#include "../shared/netclient.h"
#include <windows.h>
#include <shellapi.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void Print(const net::QueryResult& r) {
    if (r.pingMs < 0) { std::printf("%s:%u  no answer\n", r.host.c_str(), r.port); return; }
    std::printf("%s:%u  \"%s\"  mode=%s  players=%u/%u  ping=%d ms  password=%s  version=%s protocol=%u\n", r.host.c_str(), r.port, r.info.name.c_str(),
                r.info.mode.c_str(), r.info.players, r.info.maxPlayers, r.pingMs, r.info.password ? "yes" : "no", r.info.version.c_str(), r.info.protocol);
}
}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    // Arguments as UTF-8 regardless of the console code page.
    int argc = 0; auto wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args; std::vector<char*> argvList;
    for (int i = 0; i < argc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string a(n > 0 ? n - 1 : 0, char{}); WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, a.data(), n, nullptr, nullptr);
        args.push_back(a);
    }
    for (auto& a : args) argvList.push_back(a.data());
    char** argv = argvList.data();
    if (argc < 2) { std::printf("usage: dgmp-probe query <host[:port]>... | lan | join <host[:port]> <nick> [password] [--seconds N] [--say text]\n"); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "query") {
        std::vector<std::pair<std::string, uint16_t>> targets;
        for (int i = 2; i < argc; ++i) { std::string h; uint16_t p; if (net::ParseAddress(argv[i], h, p)) targets.push_back({h, p}); }
        bool any = false;
        for (auto& r : net::QueryServers(targets, 1500)) { Print(r); any |= r.pingMs >= 0; }
        return any ? 0 : 1;
    }
    if (cmd == "lan") {
        auto found = net::ScanLan(1000);
        if (found.empty()) std::printf("No LAN servers found\n");
        for (auto& r : found) Print(r);
        return found.empty() ? 1 : 0;
    }
    if (cmd == "join" && argc >= 4) {
        std::string host; uint16_t port;
        if (!net::ParseAddress(argv[2], host, port)) return 2;
        std::string nick = argv[3], password;
        double seconds = 0; std::vector<std::string> says; bool sendPose = false, autoUi = false; int posesIn = 0;
        for (int i = 4; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
            else if (a == "--say" && i + 1 < argc) says.push_back(argv[++i]);
            else if (a == "--pose") sendPose = true;
            else if (a == "--auto") autoUi = true;
            else password = a;
        }
        net::Client c;
        bool done = false; int exitCode = 0;
        float px = -85462.8f, py = -301759.8f, pz = -5179.7f;
        bool still = false;     // !at: stand on the spot instead of walking a circle
        uint16_t carried = 0;
        c.on.welcome = [&](const std::string& n, const std::string& m) { std::printf("* connected to \"%s\" (mode %s) as id %u\n", n.c_str(), m.c_str(), c.localId()); for (auto& s : says) c.SendChat(s); };
        c.on.disconnected = [&](const std::string& why) { std::printf("* disconnected: %s\n", why.c_str()); done = true; exitCode = 1; };
        c.on.playerJoin = [](const net::RemotePlayer& p) { std::printf("* %s (id %u) is on the server\n", p.nick.c_str(), p.id); };
        c.on.playerQuit = [](uint16_t id, uint8_t r) { std::printf("* player %u left (reason %u)\n", id, r); };
        c.on.message = [](uint32_t rgba, const std::string& t) { std::printf("[#%06X] %s\n", rgba >> 8, t.c_str()); };
        c.on.setTime = [](float h, bool p) { std::printf("* time %.2f%s\n", h, p ? " (paused)" : ""); };
        c.on.setWeather = [](int w) { std::printf("* weather %d\n", w); };
        c.on.teleport = [&](float x, float y, float z, float yaw) { px = x; py = y; pz = z; std::printf("* teleport %.0f %.0f %.0f yaw %.0f\n", x, y, z, yaw); };
        c.on.hint = [](const std::string& t, const std::string& x, const std::string& img, bool p, float) { std::printf("* HINT [%s] %s (image=%s pause=%d)\n", t.c_str(), x.c_str(), img.c_str(), p); };
        c.on.pose = [&](uint16_t, const uint8_t*, size_t size) { if (++posesIn == 1) std::printf("* first pose received: %zu bytes\n", size); };
        // The mode's UI (--auto answers dialogs with the first button and the last item, uses interactions, presses watched keys).
        c.on.dialog = [&](const net::Client::Events::DialogInfo& d) {
            if (d.style == net::DialogStyle::Close) { std::printf("* DIALOG %u closed\n", d.id); return; }
            std::printf("* DIALOG %u style %d [%s] %s  buttons [%s] [%s] default [%s] headers [%s] items %zu\n", d.id, int(d.style), d.title.c_str(), d.text.c_str(),
                        d.button1.c_str(), d.button2.c_str(), d.defaultText.c_str(), d.headers.c_str(), d.items.size());
            for (auto& it : d.items) std::printf("    - %s\n", it.c_str());
            if (autoUi) c.SendDialogResponse(d.id, true, d.items.empty() ? -1 : int(d.items.size()) - 1, "typed by probe");
        };
        c.on.hud = [](const std::string& id, const std::string& json) { std::printf("* HUD [%s] %s\n", id.c_str(), json.empty() ? "(removed)" : json.c_str()); };
        c.on.interaction = [&](uint16_t id, const net::Client::Events::InteractionInfo* in) {
            if (!in) { std::printf("* INTERACTION %u removed\n", id); return; }
            std::printf("* INTERACTION %u at %.0f %.0f %.0f r=%.0f key=%c [%s]\n", id, in->x, in->y, in->z, in->radius, in->key, in->text.c_str());
            if (autoUi) c.SendUse(id);
        };
        c.on.carry = [&](uint16_t id, const net::Client::Events::CarryInfo* in) {
            if (!in) { std::printf("* CARRY %u removed\n", id); return; }
            if (in->carrier == c.localId()) carried = id; else if (carried == id) carried = 0;
            std::printf("* CARRY %u %s at %.0f %.0f %.0f carrier %d roll %u\n", id, in->model.c_str(), in->x, in->y, in->z, in->carrier == 0xFFFF ? -1 : int(in->carrier), in->roll);
        };
        c.on.marker = [](uint16_t id, const net::Client::Events::MarkerInfo* in) {
            if (!in) { std::printf("* MARKER %u removed\n", id); return; }
            std::printf("* MARKER %u at %.0f %.0f %.0f style %u route %d [%s]\n", id, in->x, in->y, in->z, in->style, in->route, in->title.c_str());
        };
        c.on.objective = [](uint8_t state, uint8_t, uint8_t, const std::string& title, const std::string& text, int xp) {
            std::printf("* OBJECTIVE state %u [%s] %s xp %d\n", state, title.c_str(), text.c_str(), xp);
        };
        c.on.actor = [](uint16_t id, const net::Client::Events::ActorInfo* in) {
            if (!in) { std::printf("* ACTOR %u removed\n", id); return; }
            std::printf("* ACTOR %u at %.0f %.0f %.0f look %s anim %s [%s]\n", id, in->x, in->y, in->z, in->look.c_str(), in->anim.c_str(), in->name.c_str());
        };
        c.on.look = [&](uint16_t id, const std::string& look) { if (id == c.localId()) std::printf("* LOOK %s\n", look.empty() ? "(own)" : look.c_str()); };
        c.on.watchKeys = [&](const std::vector<uint8_t>& keys) {
            std::printf("* WATCH KEYS"); for (auto k : keys) std::printf(" %u", k); std::printf("\n");
            if (autoUi) for (auto k : keys) c.SendKey(k);
        };
        if (!c.Connect(host, port, nick, password)) return 1;

        std::mutex lock; std::vector<std::string> input;
        std::thread([&] { char buf[512]; while (std::fgets(buf, sizeof(buf), stdin)) { std::string s = buf; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); std::lock_guard<std::mutex> g(lock); input.push_back(s); } }).detach();

        const double start = Now(); double nextState = 0, nextList = Now() + 3;
        while (!done) {
            const double now = Now();
            c.Service(now);
            {
                std::lock_guard<std::mutex> g(lock);
                for (auto& s : input) {
                    float x = 0, y = 0, z = 0; unsigned id = 0, ok = 0; int item = -1;
                    if (s == "quit") done = true;
                    else if (std::sscanf(s.c_str(), "!at %f %f %f", &x, &y, &z) == 3) { px = x; py = y; pz = z + 90.f; still = true; nextState = 0; }
                    else if (s == "!walk") still = false;
                    else if (std::sscanf(s.c_str(), "!use %u", &id) == 1) c.SendUse(uint16_t(id));
                    else if (std::sscanf(s.c_str(), "!dlg %u %u %d", &id, &ok, &item) >= 2) c.SendDialogResponse(uint16_t(id), ok != 0, item, "");
                    else if (std::sscanf(s.c_str(), "!pick %u", &id) == 1) c.SendCarry(uint16_t(id), net::CarryOp::Pick);
                    else if (std::sscanf(s.c_str(), "!put %f %f %f", &x, &y, &z) == 3) c.SendCarry(carried, net::CarryOp::Put, x, y, z, 0);
                    else if (std::sscanf(s.c_str(), "!break %f %f %f", &x, &y, &z) == 3) c.SendCarry(carried, net::CarryOp::Break, x, y, z, 0);
                    else c.SendChat(s);
                }
                input.clear();
            }
            if (c.status() == net::Client::Status::Connected && now >= nextState) {
                nextState = now + 0.05;
                const float a = static_cast<float>(now - start) * 0.5f;
                net::PlayerState s; s.x = px + (still ? 0 : std::cos(a) * 300); s.y = py + (still ? 0 : std::sin(a) * 300); s.z = pz; s.yaw = still ? 0 : a * 57.3f;
                c.SendState(s);
                if (sendPose) {  // synthetic 224-bone body pose, the size of a real one
                    net::Writer w; w.U8(0); w.U16(0); w.U32(0x1234); w.U16(224); w.Put(now);
                    for (int k = 0; k < 7; ++k) w.F32(0);
                    std::vector<net::QBone> bones(224); w.Raw(bones.data(), bones.size() * sizeof(net::QBone));
                    c.SendPose(w.buf);
                }
            }
            if (c.status() == net::Client::Status::Connected && now >= nextList) {
                nextList = now + 5;
                for (auto& [id, p] : c.players())
                    std::printf("  - %s (id %u) at %.0f %.0f %.0f, %.1fs ago, ping %d\n", p.nick.c_str(), id, p.state.x, p.state.y, p.state.z, p.stateTime > 0 ? now - p.stateTime : -1.0, c.pingMs());
            }
            if (seconds > 0 && now - start > seconds) done = true;
            std::fflush(stdout);
            Sleep(10);
        }
        if (posesIn) std::printf("* poses received: %d\n", posesIn);
        c.Disconnect();
        return exitCode;
    }
    std::printf("unknown command\n");
    return 2;
}
