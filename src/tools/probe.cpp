// dgmp-probe.exe — test client for dgmp-server without the game.
//   dgmp-probe query <host[:port]> [...]     server status
//   dgmp-probe lan                           LAN discovery
//   dgmp-probe master <host[:port]>          the master server's list, each server queried
//   dgmp-probe join <host[:port]> <nick> [password] [--seconds N] [--say text] [--auto]
//       stays connected, walks a small circle, prints server events; stdin lines are sent as chat, except:
//       !at x y z (stand there: z = ground), !walk (circle again), !use <interaction>, !key <letter> (a bound key), !dlg <id> <1|0> [item],
//       !pick <carry>, !put x y z (put the carried box there), !break x y z (it breaks in a roll), !skin <zero-based choice>,
//       !hit <id> <damage> [zone 0-3] [weapon class], !throw x y z vx vy vz (a rock thrown from there), !dead / !alive, !inv key=n ... (report this backpack: a cheat),
//       !ui <name> [json] (the mode's page sent dgmp.send(name, data)), !cs <name> [json] (the client script sent send(name, data)),
//       !cserr <text> (a client script failed), !iw <window id> <json> (an action in the mode's item window),
//       !pickup <key> [x y z] [object id] (the game says we picked it up), !shot <weapon class> (we fired here). Item orders change a simulated backpack.
//       Zombies (docs/zombies.md): the ones it owns walk toward it and are reported 10 times a second; !zhit <id> <damage>
//       (we hurt its puppet), !zdie <id> [killer id] (the one we own died), !zatk <id> <player> <damage> (the one we own hurt that player);
//       zombie messages are printed.
//       --steam <steamid> [ticket hex]: the Steam identity sent with Hello.
//       Voice: !voice <seconds> [radio] sends dummy frames (20 a second) with the talk / radio key; voice heard is
//       counted per speaker and route. --voice-echo [delay] [radio]: what others say near it, it says again after `delay`
//       seconds (1 by default; radio: with the radio key): a game client next to the probe hears its own voice from
//       the probe's body (or over the radio, when both are in a radio channel).
#include "../shared/netclient.h"
#include <windows.h>
#include <shellapi.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <map>
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
    if (cmd == "master" && argc >= 3) {
        std::string error;
        const auto list = net::QueryMaster(argv[2], 2500, error);
        if (!error.empty()) std::printf("master: %s\n", error.c_str());
        std::printf("%zu servers listed\n", list.size());
        for (auto& r : net::QueryServers(list, 1500)) Print(r);
        return error.empty() ? 0 : 1;
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
        uint64_t steamId = 0; std::string ticket;
        double voiceEcho = 0;  // --voice-echo delay (0: off)
        net::VoiceKey echoKey = net::VoiceKey::Talk;
        for (int i = 4; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
            else if (a == "--say" && i + 1 < argc) says.push_back(argv[++i]);
            else if (a == "--pose") sendPose = true;
            else if (a == "--auto") autoUi = true;
            else if (a == "--voice-echo") {
                voiceEcho = 1.0;
                if (i + 1 < argc && argv[i + 1][0] != '-' && std::atof(argv[i + 1]) > 0) voiceEcho = std::atof(argv[++i]);
                if (i + 1 < argc && std::string(argv[i + 1]) == "radio") { echoKey = net::VoiceKey::Radio; ++i; }
            }
            else if (a == "--steam" && i + 1 < argc) {
                steamId = std::strtoull(argv[++i], nullptr, 10);
                if (i + 1 < argc && argv[i + 1][0] != '-') ticket = argv[++i];
            }
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
        c.on.setWorld = [](int world) { std::printf("* WORLD %d\n", world); };
        c.on.spectate = [](bool on, const std::vector<uint16_t>& only) { std::printf("* SPECTATE %s (%zu players)\n", on ? "on" : "off", only.size()); };
        c.on.corpse = [](uint16_t id) { std::printf("* CORPSE of %u\n", id); };
        c.on.worldItem = [](uint16_t id, const std::string* item, int count, float x, float y, float z, float) {
            if (!item) std::printf("* WORLD ITEM %u removed\n", id); else std::printf("* WORLD ITEM %u %s x%d at %.0f %.0f %.0f\n", id, item->c_str(), count, x, y, z);
        };
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
        c.on.uiList = [](const std::vector<std::pair<std::string, std::string>>& files) {
            std::printf("* UI files:"); for (auto& f : files) std::printf(" %s", f.first.c_str()); std::printf("\n");
        };
        c.on.scriptList = [](const std::vector<std::pair<std::string, std::string>>& files) {
            std::printf("* client scripts:"); for (auto& f : files) std::printf(" %s", f.first.c_str()); std::printf("%s\n", files.empty() ? " (none)" : "");
        };
        c.on.itemWindow = [](uint16_t id, const std::string& json) { std::printf("* ITEM WINDOW %u %s\n", id, json.empty() ? "(closed)" : json.c_str()); };
        c.on.scriptEvent = [](const std::string& name, const std::string& json) { std::printf("* client script event %s %s\n", name.c_str(), json.c_str()); };
        c.on.ui = [](net::UiOp op, const std::string& name, const std::string& text, bool flag) {
            if (op == net::UiOp::Open) std::printf("* UI open %s%s\n", text.c_str(), flag ? " (focus)" : "");
            else if (op == net::UiOp::Close) std::printf("* UI close\n");
            else if (op == net::UiOp::Focus) std::printf("* UI focus %s\n", flag ? "on" : "off");
            else std::printf("* UI event %s %s\n", name.c_str(), text.c_str());
        };
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
        c.on.object = [](uint16_t id, const net::Client::Events::ObjectInfo* in) {
            if (!in) { std::printf("* OBJECT %u removed\n", id); return; }
            std::printf("* OBJECT %u %s at %.0f %.0f %.0f yaw %.0f scale %.2f world %d\n", id, in->asset.c_str(), in->x, in->y, in->z, in->yaw, in->scale, in->world);
        };
        c.on.objectMove = [](uint16_t id, const float to[6], float seconds, float elapsed) {
            std::printf("* OBJECT %u moves to %.0f %.0f %.0f yaw %.0f in %.1f s (%.1f s gone)\n", id, to[0], to[1], to[2], to[4], seconds, elapsed);
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
        c.on.blip = [](uint16_t id, const net::Client::Events::BlipInfo* b) {
            if (!b) { std::printf("* BLIP %u removed\n",id); return; }
            std::printf("* BLIP %u %s [%s] %s\n",id,b->icon.c_str(),b->title.c_str(),b->description.c_str());
        };
        c.on.vehicle = [](uint16_t id, const net::Client::Events::VehicleInfo* v) {
            if (!v) { std::printf("* VEHICLE %u removed\n",id); return; }
            std::printf("* VEHICLE %u model=%s at %.0f %.0f %.0f yours=%d marked=%d world %d driver %d\n",id,
                v->model.empty()?"bike":v->model.c_str(),v->state.x,v->state.y,v->state.z,int(v->yours),int(v->marked),v->world,v->driver==0xFFFF?-1:int(v->driver));
        };
        std::vector<std::pair<std::string, std::string>> skinChoices;
        c.on.customize = [&](bool cancel, const std::string& title, const std::string&, const std::string&,
                             const std::vector<std::pair<std::string, std::string>>& choices, const float*) {
            skinChoices = choices;
            std::printf("* CUSTOMIZE [%s] cancel=%d choices=%zu\n", title.c_str(), int(cancel), choices.size());
            for (size_t i = 0; i < choices.size(); ++i) std::printf("* SKIN %zu [%s] %s\n", i, choices[i].first.c_str(), choices[i].second.c_str());
        };
        c.on.watchKeys = [&](const std::vector<uint8_t>& keys) {
            std::printf("* WATCH KEYS"); for (auto k : keys) std::printf(" %u", k); std::printf("\n");
            if (autoUi) for (auto k : keys) c.SendKey(k);
        };
        // Voice: frames heard per speaker and route; echoed frames wait for their time.
        struct Echo { double at; int gain; std::vector<uint8_t> frame; };
        std::vector<Echo> echoes;
        std::map<std::pair<uint16_t, int>, std::pair<int, size_t>> voiceIn;  // (id, route) -> frames, bytes
        double voiceOutUntil = 0, nextVoiceOut = 0; net::VoiceKey voiceOutKey = net::VoiceKey::Talk;
        c.on.voice = [&](uint16_t id, net::VoiceRoute route, net::VoiceCodec, int gain, float range, const uint8_t* frame, size_t size) {
            auto& v = voiceIn[{id, int(route)}];
            if (!v.first++) std::printf("* VOICE from %u route %d range %.0f m (%zu bytes)\n", id, int(route), range / 100.f, size);
            v.second += size;
            if (voiceEcho > 0 && route == net::VoiceRoute::Near) echoes.push_back({Now() + voiceEcho, gain, std::vector<uint8_t>(frame, frame + size)});
        };
        c.on.voiceSetup = [](uint8_t flags, const std::string& radio, const std::string& call) {
            std::printf("* VOICE SETUP on=%d muted=%d radio=[%s] call=[%s]\n", flags & net::VoiceOn ? 1 : 0, flags & net::VoiceMuted ? 1 : 0, radio.c_str(), call.c_str());
        };
        // A backpack like the game's: the server's item orders change it, the snapshot goes back.
        std::map<std::string, int> backpack;
        bool dead = false;
        auto report = [&] {
            std::vector<std::pair<std::string, int>> snap;
            for (auto& [k, v] : backpack) if (v > 0) snap.emplace_back(k, v);
            c.SendInventory(snap);
        };
        c.on.item = [&](net::ItemAction action, const std::string& key, int amount) {
            std::printf("* ITEM %d %s %d\n", int(action), key.c_str(), amount);
            switch (action) {
            case net::ItemAction::Give: case net::ItemAction::GiveReplace: backpack[key] += amount; break;
            case net::ItemAction::Take: backpack[key] = std::max(0, backpack[key] - amount); break;
            case net::ItemAction::SetAmmo: if (key.rfind("kAmmoID_", 0) == 0) backpack[key] = amount; else backpack["kAmmoID_Pistol"] = amount; break;
            case net::ItemAction::TakeWeapons: for (auto& [k, v] : backpack) if (k.rfind("kWeaponID_", 0) == 0 && k.find("Knife") == std::string::npos) v = 0; break;
            case net::ItemAction::Clear: backpack.clear(); break;
            default: break;
            }
            report();
        };
        // Zombies: what the server said about them; the owned ones walk toward the probe (a stand-in for the game's AI).
        struct ProbeZombie { float x = 0, y = 0, z = 0, health = 200; uint16_t owner = 0xFFFF; bool dead = false; int moves = 0; };
        std::map<uint16_t, ProbeZombie> zombies;
        double nextZombieReport = 0;
        if (!c.on.playerThrow) c.on.playerThrow = [](uint16_t id, const std::string& mesh, float, float x, float y, float z, float, float, float) { std::printf("* THROW by %u %s at %.0f %.0f %.0f\n", id, mesh.c_str(), x, y, z); };
        if (!c.on.damage) c.on.damage = [](uint16_t from, float amount, net::HitZone, const std::string&) { std::printf("* DAMAGE %.0f from %u\n", amount, from); };
        c.on.zombie = [&](net::Msg op, const uint8_t* data, size_t size) {
            net::Reader r(data, size);
            if (op == net::Msg::ZombieMoves) {
                const int n = r.U8();
                for (int i = 0; i < n && r.ok(); ++i) {
                    const uint16_t id = r.U16(); const float x = r.F32(), y = r.F32(), z = r.F32(); r.F32(); r.F32(); const float hp = r.F32(); r.U8(); r.Str();
                    if (!r.ok()) break;
                    auto& zb = zombies[id]; zb.x = x; zb.y = y; zb.z = z; zb.health = hp;
                    if (++zb.moves == 1 || zb.moves % 50 == 0) std::printf("* zombie %u moves (%d): %.0f %.0f %.0f hp %.0f\n", id, zb.moves, x, y, z, hp);
                }
                return;
            }
            const uint16_t id = r.U16(); const auto zop = net::ZombieOp(r.U8());
            auto& zb = zombies[id];
            switch (zop) {
            case net::ZombieOp::Add: { const auto type = r.Str(); zb.x = r.F32(); zb.y = r.F32(); zb.z = r.F32(); r.F32(); r.I32(); zb.owner = r.U16(); r.F32();
                std::printf("* zombie %u %s at %.0f %.0f %.0f, owner %u\n", id, type.c_str(), zb.x, zb.y, zb.z, zb.owner); break; }
            case net::ZombieOp::Owner: zb.owner = r.U16(); std::printf("* zombie %u owner %u%s\n", id, zb.owner, zb.owner == c.localId() ? " (us)" : ""); break;
            case net::ZombieOp::Damage: { const float d = r.F32(); const uint16_t by = r.U16(); zb.health -= d; std::printf("* zombie %u damage %.0f from %u -> hp %.0f\n", id, d, by, zb.health); break; }
            case net::ZombieOp::Died: { const uint16_t killer = r.U16(); zb.dead = true; std::printf("* zombie %u died, killer %u\n", id, killer); break; }
            case net::ZombieOp::Remove: std::printf("* zombie %u removed\n", id); zombies.erase(id); break;
            }
        };
        c.SetIdentity(steamId, ticket);
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
                    else if (s.size() == 6 && s.rfind("!key ", 0) == 0) c.SendKey(uint8_t(std::toupper(static_cast<unsigned char>(s[5]))));  // a watched key (bindKey)
                    else if (std::sscanf(s.c_str(), "!dlg %u %u %d", &id, &ok, &item) >= 2) c.SendDialogResponse(uint16_t(id), ok != 0, item, "");
                    else if (std::sscanf(s.c_str(), "!skin %u", &id) == 1) {
                        if (id < skinChoices.size()) { c.SendCustomizeDone(true, skinChoices[id].second); skinChoices.clear(); }
                    }
                    else if (std::sscanf(s.c_str(), "!pick %u", &id) == 1) c.SendCarry(uint16_t(id), net::CarryOp::Pick);
                    else if (std::sscanf(s.c_str(), "!put %f %f %f", &x, &y, &z) == 3) c.SendCarry(carried, net::CarryOp::Put, x, y, z, 0);
                    else if (std::sscanf(s.c_str(), "!break %f %f %f", &x, &y, &z) == 3) c.SendCarry(carried, net::CarryOp::Break, x, y, z, 0);
                    else if (s.rfind("!hit ", 0) == 0) {
                        unsigned zone = 0; float dmg = 0; char weapon[128] = "";
                        if (std::sscanf(s.c_str(), "!hit %u %f %u %127s", &id, &dmg, &zone, weapon) >= 2) c.SendHit(uint16_t(id), dmg, net::HitZone(zone), weapon);
                    }
                    else if (float v[6]; std::sscanf(s.c_str(), "!throw %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
                        c.SendThrow("/Game/Weapons/Thrown_Weapons/Misc/Rock/Meshes/Static/Rock_Static.Rock_Static", 0.5f, v[0], v[1], v[2], v[3], v[4], v[5]);
                    else if (s.rfind("!ui ", 0) == 0) {
                        const auto rest = s.substr(4); const auto sp = rest.find(' ');
                        c.SendUiMessage(rest.substr(0, sp), sp == std::string::npos ? "null" : rest.substr(sp + 1));
                    }
                    else if (s.rfind("!cs ", 0) == 0) {
                        const auto rest = s.substr(4); const auto sp = rest.find(' ');
                        c.SendScriptMessage(rest.substr(0, sp), sp == std::string::npos ? "null" : rest.substr(sp + 1));
                    }
                    else if (s.rfind("!cserr ", 0) == 0) c.SendScriptError(s.substr(7));
                    else if (s.rfind("!iw ", 0) == 0) {  // !iw <window id> <json>: an action in the item window
                        const auto rest = s.substr(4); const auto sp = rest.find(' ');
                        if (sp != std::string::npos) c.SendItemAction(uint16_t(std::atoi(rest.substr(0, sp).c_str())), rest.substr(sp + 1));
                    }
                    else if (s.rfind("!shot ", 0) == 0) c.SendShot(s.substr(6), px, py, pz);
                    else if (s.rfind("!voice ", 0) == 0) {
                        float secs = 0; char how[16] = "";
                        if (std::sscanf(s.c_str(), "!voice %f %15s", &secs, how) >= 1) {
                            voiceOutUntil = Now() + secs; voiceOutKey = std::string(how) == "radio" ? net::VoiceKey::Radio : net::VoiceKey::Talk;
                        }
                    }
                    else if (std::sscanf(s.c_str(), "!zhit %u %f", &id, &x) == 2) { net::Writer w; w.Op(net::Msg::ZombieHit); w.U16(uint16_t(id)); w.F32(x); c.SendZombie(w, true); }
                    else if (std::sscanf(s.c_str(), "!zatk %u %u %f", &id, &ok, &x) == 3) { net::Writer w; w.Op(net::Msg::ZombieAttack); w.U16(uint16_t(id)); w.U16(uint16_t(ok)); w.F32(x); c.SendZombie(w, true); }
                    else if (unsigned killer = 0xFFFF; std::sscanf(s.c_str(), "!zdie %u %u", &id, &killer) >= 1) { net::Writer w; w.Op(net::Msg::ZombieDied); w.U16(uint16_t(id)); w.U16(uint16_t(killer)); c.SendZombie(w, true); }
                    else if (s == "!dead") { dead = true; nextState = 0; }
                    else if (s == "!alive") { dead = false; nextState = 0; }
                    else if (s.rfind("!inv", 0) == 0) {
                        backpack.clear();
                        char key[128]; int n = 0, used = 0; const char* at = s.c_str() + 4;
                        while (std::sscanf(at, " %127[^=]=%d%n", key, &n, &used) == 2) { backpack[key] = n; at += used; }
                        report();
                    }
                    else if (s.rfind("!pickup ", 0) == 0) {
                        char key[128] = "", object[128] = "probe_pickup"; float ex = px, ey = py, ez = pz;
                        const int got = std::sscanf(s.c_str(), "!pickup %127s %f %f %f %127s", key, &ex, &ey, &ez, object);
                        if (got >= 1) { c.SendGameEvent(net::GameEventType::Pickup, "BendInventoryPickup", key, object, ex, ey, ez); backpack[key] += 1; report(); }
                    }
                    else c.SendChat(s);
                }
                input.clear();
            }
            if (c.status() == net::Client::Status::Connected && now >= nextState) {
                nextState = now + 0.05;
                const float a = static_cast<float>(now - start) * 0.5f;
                net::PlayerState s; s.x = px + (still ? 0 : std::cos(a) * 300); s.y = py + (still ? 0 : std::sin(a) * 300); s.z = pz; s.yaw = still ? 0 : a * 57.3f;
                if (dead) s.flags |= net::StateDead;
                c.SendState(s);
                if (sendPose) {  // synthetic 224-bone body pose, the size of a real one
                    net::Writer w; w.U8(0); w.U16(0); w.U32(0x1234); w.U16(224); w.Put(now);
                    for (int k = 0; k < 7; ++k) w.F32(0);
                    std::vector<net::QBone> bones(224); w.Raw(bones.data(), bones.size() * sizeof(net::QBone));
                    c.SendPose(w.buf);
                }
            }
            if (c.status() == net::Client::Status::Connected && now >= nextZombieReport) {
                nextZombieReport = now + 0.1;
                net::Writer w; int n = 0;
                for (auto& [id, zb] : zombies) {
                    if (zb.owner != c.localId() || zb.dead || n >= int(net::kZombiesPerPacket)) continue;
                    const float dx = px - zb.x, dy = py - zb.y, d = std::sqrt(dx * dx + dy * dy);
                    if (d > 150.f) { zb.x += dx / d * 45.f; zb.y += dy / d * 45.f; }  // 4.5 m/s toward us
                    if (!n) { w.Op(net::Msg::ZombieStates); w.U8(0); }
                    w.U16(id); w.F32(zb.x); w.F32(zb.y); w.F32(zb.z); w.F32(std::atan2(dy, dx) * 57.3f); w.F32(d > 150.f ? 450.f : 0.f); w.F32(zb.health);
                    w.U8(0); w.Str("");
                    ++n;
                }
                if (n) { w.buf[1] = uint8_t(n); c.SendZombie(w, false); }
            }
            if (c.status() == net::Client::Status::Connected && now < voiceOutUntil && now >= nextVoiceOut) {
                nextVoiceOut = now + 0.05;
                std::vector<uint8_t> frame(120, 0x5A);
                c.SendVoice(voiceOutKey, net::VoiceCodec::Steam, 100, frame.data(), frame.size());
            }
            for (size_t i = 0; i < echoes.size();) {
                if (now < echoes[i].at) { ++i; continue; }
                c.SendVoice(echoKey, net::VoiceCodec::Steam, echoes[i].gain, echoes[i].frame.data(), echoes[i].frame.size());
                echoes.erase(echoes.begin() + long(i));
            }
            if (c.status() == net::Client::Status::Connected && now >= nextList) {
                nextList = now + 5;
                for (auto& [key, v] : voiceIn) std::printf("  voice from %u route %d: %d frames, %zu bytes\n", key.first, key.second, v.first, v.second);
                voiceIn.clear();
                for (auto& [id, p] : c.players())
                    std::printf("  - %s (id %u) at %.0f %.0f %.0f, %.1fs ago, ping %d%s\n", p.nick.c_str(), id, p.state.x, p.state.y, p.state.z, p.stateTime > 0 ? now - p.stateTime : -1.0, c.pingMs(),
                                (p.state.flags & net::StateOtherWorld) ? " [other world]" : (p.state.flags & net::StateHidden) ? " [hidden]" : "");
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
