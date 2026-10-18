#pragma once
// DGMP network protocol shared by dgmp-server, the game client DLL and the launcher.
// Transport: ENet over UDP. Channel 0 reliable+ordered, channel 1 unreliable (player state).
// Server status queries are raw UDP datagrams on the same port, outside ENet (SA-MP style).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace net {

constexpr uint16_t kProtocolVersion = 6;
constexpr uint16_t kDefaultPort = 7777;
constexpr int kLanPortSpan = 10;        // LAN discovery probes kDefaultPort .. kDefaultPort+span-1
constexpr int kChannels = 3;
constexpr int kChanReliable = 0, kChanState = 1, kChanPose = 2;
constexpr float kPoseRelayRange = 25000.f;  // 250 m: bone poses only go to players this close
constexpr int kMaxPlayersLimit = 64;
constexpr size_t kNickMin = 3, kNickMax = 24;  // in UTF-8 code points
constexpr size_t kChatMax = 256;

// Raw query datagram: "DGMQ" + u32 token  ->  reply "DGMR" + u32 token + ServerInfo
constexpr char kQueryMagic[4] = {'D', 'G', 'M', 'Q'};
constexpr char kReplyMagic[4] = {'D', 'G', 'M', 'R'};

enum class Msg : uint8_t {
    // client -> server
    Hello = 1,        // u16 protocol, str nick, str password
    State = 2,        // PlayerState (unreliable)
    Pose = 4,         // PoseFrame payload (unreliable, fragmented); relayed as Msg::PlayerPose
    Chat = 3,         // str text ("/cmd args" is a command)
    Inventory = 5,    // u16 count, count x (str itemKey, i32 amount): full snapshot, sent when it changes
    Look = 6,         // str lookPath: the look the player is wearing now (reported on change)
    DialogResponse = 11,  // u16 dialogId, u8 ok (first button), i16 item (-1 none), str text: the player answered a dialog
    Use = 12,         // u16 interactionId: the player pressed the key of an interaction it stands at
    Key = 13,         // u8 virtualKey: the player pressed a key the mode watches (WatchKeys)
    VehicleEnter = 14, // u16 vehicleId: the player asks to get on a server vehicle (pressed E at it; in a standing helicopter: to get out)
    VehicleExit = 15,  // u16 vehicleId, VehicleState: the player got off it, here (a passenger: the state is ignored)
    VehicleState = 16, // u16 vehicleId, VehicleState: where the vehicle it drives is now (about every second)
    ImageRequest = 17, // str name: send me this image of the mode (ImageList: not in my cache)
    CustomizeDone = 10, // u8 ok, str skin: the player closed the character creator (or picked one of the offered looks)
    Gear = 9,         // GearList payload: what the player carries (weapons in hand, on the back, holstered)
    Hit = 8,          // u16 victimId, f32 damage, u8 HitZone, str weapon: this player hit another one
    GameEvent = 7,    // u8 GameEventType, str class, str detail, str id, f32 x, y, z: something the player did in the world
    // server -> client
    Welcome = 64,     // u16 yourId, str serverName, str modeName, u8 maxPlayers
    Reject = 65,      // str reason
    PlayerJoin = 66,  // u16 id, str nick
    PlayerQuit = 67,  // u16 id, u8 reason
    States = 68,      // u8 count, count x (u16 id, PlayerState) (unreliable)
    Message = 69,     // u32 rgba, str text
    SetTime = 70,     // f32 hours, u8 paused
    SetWeather = 71,  // i32 weatherId (-1 = automatic)
    Teleport = 72,    // f32 x,y,z,yaw
    Hint = 73,        // str title, str text, str image, u8 pause, f32 seconds (tip: empty title, shown for `seconds`)
    Kick = 74,        // str reason
    PlayerPose = 75,  // u16 id + PoseFrame payload
    Item = 76,        // u8 ItemAction, str itemKey, i32 amount
    Bike = 77,        // u8 BikeAction (Give: f32 x,y,z,yaw; x=y=z=0 -> next to the player)
    Settings = 78,    // u8 nametags (NametagMode), u8 collisions (CollisionMask)
    PlayerLook = 79,  // u16 id, str lookPath: that player's look; for your own id it is an order to wear it
    Objective = 81,   // u8 state (EObjectiveHudState), u8 category, u8 style, str title, str text, i32 xp
    ObjectiveMeter = 82,  // u8 show, str title, f32 percent (0..1)
    PlayerGear = 87,  // u16 id + GearList payload
    Customize = 90,   // u8 canCancel, str title, str families (comma separated, empty: all), str current look/skin,
                      // u8 n, n x (str name, str look): a picker of these (n = 0: the full character creator),
                      // u8 hasPlace [f32 x, y, z, yaw]: the server moved the player there to choose (opens on arrival)
    Dialog = 91,      // u16 id, u8 DialogStyle, str title, str text, str button1, str button2, str default,
                      // str headers (tab separated, list only), u8 n, n x str item (columns tab separated); style Close: just the id
    HudElement = 92,  // str id, str json (docs/API.md "HUD"; "" removes it; id "" and json "": remove all)
    Interaction = 93, // u16 id, u8 op (1 add: f32 x,y,z, f32 radius, u8 virtualKey, str text, i32 world (-1 every); 2 remove)
    SetWorld = 95,    // i32 world: the virtual world you are in (players, actors and interactions of other worlds are not there)
    Actor = 96,       // u16 id, u8 op (1 add/update: f32 x,y,z,yaw, i32 world (-1 every), str look, str anim, str name, u8 solid; 2 remove)
    Vehicle = 97,     // u16 id, u8 op (1 add/update: VehicleState, i32 world, u16 driver (0xFFFF none), u8 locked, u8 yours, u8 marked, u32 blipColor (RGBA, 0 default), str blipIcon ("": the bike), str model ("": the bike), u8 npc, str npcLook, str npcName, u16 passenger (0xFFFF none), u8 riders (helicopter seats taken); 2 remove)
    VehicleAllow = 98, // u16 vehicleId, u8 seat (0 driver, 1 passenger; a helicopter: 1-4): get on it (the answer to VehicleEnter, or the mode puts you on it)
    VehicleEject = 99, // u16 vehicleId: get off it (the mode refused it or took it away)
    ImageList = 100,  // u16 n, n x (str name, str sha256): the mode's images (modes/<mode>/images); fetch the ones not cached
    ImageData = 101,  // str name, u16 part, u16 parts, str bytes: an image's file, in parts
    VehicleRoute = 103, // u16 vehicleId, f32 speed (units/s), f32 elapsed (s since it set off), u8 loop, u16 n, n x f32 x,y,z: the way its NPC drives it (n 0: it stopped)
    Icon = 102,       // u16 id, u8 op (1 add/update: f32 x,y,z, f32 size (pixels), f32 distance (units), i32 world (-1 every), u32 rgba, str image, str text, u8 throughWalls; 2 remove)
    WatchKeys = 94,   // u8 n, n x u8 virtualKey: keys whose presses the client reports (Msg::Key)
    Spectate = 89,    // u8 on, u8 count, count x u16 playerId: watch other players (count 0: anyone)
    Freeze = 88,      // u8 on: the player cannot move (on a bike: no throttle, brake or getting off)
    Damage = 86,      // u16 attackerId, f32 damage, u8 HitZone: another player hurt you
    WorldEdits = 85,  // u16 part, u16 parts, str chunk: the mode's world.json (docs/world.md), split in chunks; "" = none
    Corpse = 84,      // u16 playerId: that player died where it stands now; leave a body there
    WorldItem = 83,   // u16 id, u8 op (1 add: str itemKey, i32 count, f32 x,y,z, f32 yaw; 2 remove): a pickup placed by the mode
    Marker = 80,      // u16 id, u8 op (1 add/update: f32 x,y,z, u8 category, u8 style, u8 route, str icon, str title, str description; 2 remove)
};

enum QuitReason : uint8_t { QuitLeft = 0, QuitTimeout = 1, QuitKicked = 2 };
// Gameplay events the client reports (see src/gameevents.cpp for where each comes from).
enum class GameEventType : uint8_t { Pickup = 1, Search = 2, Body = 3, Interact = 4, Door = 5, Explosion = 6, Fire = 7, Carry = 8,
                                    Respawned = 9, Died = 10 };  // the player died and the game gave it a new body (it reloads a checkpoint)
enum class ItemAction : uint8_t { Give = 1, Take = 2, SetAmmo = 3, TakeWeapons = 4, Clear = 5, TakeTools = 6, GiveReplace = 7 };  // GiveReplace: the weapon it displaces vanishes instead of dropping
enum ToolMask : uint8_t { ToolKnife = 1, ToolRock = 2, ToolTaser = 4 };  // amount of TakeWeapons / TakeTools
enum class BikeAction : uint8_t { Give = 1, Take = 2, Mount = 3 };  // Mount: the bike to x,y,z,yaw and the player onto it
// A server vehicle where it stands: position and heading, fuel and health (0..1).
struct VehicleState { float x = 0, y = 0, z = 0, yaw = 0, fuel = 1, health = 1; };
enum class DialogStyle : uint8_t { MsgBox = 0, Input = 1, Password = 2, List = 3, Close = 255 };
constexpr size_t kDialogTextMax = 4096, kDialogItemsMax = 200;
enum class HitZone : uint8_t { Body = 0, Head = 1, Arm = 2, Leg = 3 };
enum class NametagMode : uint8_t { Visible = 0, ThroughWalls = 1, Hidden = 2 };
enum CollisionMask : uint8_t { CollidePlayers = 1, CollideBikes = 2 };  // players' bodies / bikes block each other
// GearList: u8 count, count x (str meshPath, u8 skeletal, i8 parent (-1 = the body, else an earlier item),
// str socket, f32 x, y, z, pitch, yaw, roll, sx, sy, sz) - meshes attached to the body, relative to their socket.
constexpr size_t kGearMax = 8192;
constexpr size_t kItemKeyMax = 64;
constexpr size_t kLookPathMax = 256;

struct PlayerState {
    // Actor root (capsule centre) and view yaw: what scripts see and what teleports use.
    float x = 0, y = 0, z = 0, yaw = 0;
    float vx = 0, vy = 0, vz = 0;
    uint8_t flags = 0;  // StateFlags
    uint8_t anim = 0;   // Anim fallback when no bone pose is streamed
    uint8_t health = 100;  // percent of the maximum
    // Rendering: body mesh and bike mesh world transforms.
    float mx = 0, my = 0, mz = 0, mqx = 0, mqy = 0, mqz = 0, mqw = 1;
    float bx = 0, by = 0, bz = 0, bqx = 0, bqy = 0, bqz = 0, bqw = 1;
    double time = 0;    // sender clock, seconds
};
enum StateFlags : uint8_t { StateOnBike = 1, StateHasBike = 2, StateFlying = 4, StateHasMesh = 8, StateOwnsBike = 16,
                            StateDead = 32,    // dead: others show only its corpse until it respawns
                            StateHidden = 64,    // spectating: others do not see it
                            StateOtherWorld = 128 };  // in another virtual world than you (with StateHidden): not even its bike
enum Anim : uint8_t { AnimIdle, AnimWalk, AnimJog, AnimRun, AnimSprint, AnimRide, AnimCrouchIdle, AnimCrouchWalk, AnimFall, AnimRideLeft, AnimRideRight, AnimCount };

// Bone pose of one skeleton (body or bike), parent-relative, quantised.
struct QBone { int16_t px, py, pz, qx, qy, qz, qw, sx, sy, sz; };
constexpr int kPoseBody = 0, kPoseBike = 1;
constexpr int kMaxPoseBones = 320;
constexpr float kPosePosScale = 50.f;  // 0.02 cm per unit, +-6.5 m

struct ServerInfo {
    uint16_t protocol = kProtocolVersion;
    uint16_t players = 0, maxPlayers = 0;
    uint8_t password = 0;
    std::string name, mode, version;
};

class Writer {
public:
    std::vector<uint8_t> buf;
    template <typename T> void Put(T v) { const auto at = buf.size(); buf.resize(at + sizeof(T)); std::memcpy(buf.data() + at, &v, sizeof(T)); }
    void U8(uint8_t v) { Put(v); }
    void U16(uint16_t v) { Put(v); }
    void U32(uint32_t v) { Put(v); }
    void I32(int32_t v) { Put(v); }
    void F32(float v) { Put(v); }
    void Raw(const void* p, size_t n) { const auto at = buf.size(); buf.resize(at + n); std::memcpy(buf.data() + at, p, n); }
    void Str(const std::string& s) { const auto n = static_cast<uint16_t>(s.size() > 0xFFFF ? 0xFFFF : s.size()); U16(n); Raw(s.data(), n); }
    void Op(Msg m) { U8(static_cast<uint8_t>(m)); }
    void Vehicle(const VehicleState& v) { F32(v.x); F32(v.y); F32(v.z); F32(v.yaw); F32(v.fuel); F32(v.health); }
    void State(const PlayerState& s) {
        F32(s.x); F32(s.y); F32(s.z); F32(s.yaw); F32(s.vx); F32(s.vy); F32(s.vz); U8(s.flags); U8(s.anim); U8(s.health);
        F32(s.mx); F32(s.my); F32(s.mz); F32(s.mqx); F32(s.mqy); F32(s.mqz); F32(s.mqw);
        F32(s.bx); F32(s.by); F32(s.bz); F32(s.bqx); F32(s.bqy); F32(s.bqz); F32(s.bqw); Put(s.time);
    }
};

class Reader {
public:
    Reader(const void* data, size_t size) : p_(static_cast<const uint8_t*>(data)), n_(size) {}
    bool ok() const { return ok_; }
    size_t left() const { return ok_ ? n_ - at_ : 0; }
    template <typename T> T Get() { T v{}; if (!Need(sizeof(T))) return v; std::memcpy(&v, p_ + at_, sizeof(T)); at_ += sizeof(T); return v; }
    uint8_t U8() { return Get<uint8_t>(); }
    uint16_t U16() { return Get<uint16_t>(); }
    uint32_t U32() { return Get<uint32_t>(); }
    int32_t I32() { return Get<int32_t>(); }
    float F32() { return Get<float>(); }
    std::string Str(size_t max = 0xFFFF) {
        const auto n = U16(); if (!Need(n) || n > max) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_ + at_), n); at_ += n; return s;
    }
    VehicleState Vehicle() { VehicleState v; v.x = F32(); v.y = F32(); v.z = F32(); v.yaw = F32(); v.fuel = F32(); v.health = F32(); return v; }
    PlayerState State() {
        PlayerState s; s.x = F32(); s.y = F32(); s.z = F32(); s.yaw = F32(); s.vx = F32(); s.vy = F32(); s.vz = F32(); s.flags = U8(); s.anim = U8(); s.health = U8();
        s.mx = F32(); s.my = F32(); s.mz = F32(); s.mqx = F32(); s.mqy = F32(); s.mqz = F32(); s.mqw = F32();
        s.bx = F32(); s.by = F32(); s.bz = F32(); s.bqx = F32(); s.bqy = F32(); s.bqz = F32(); s.bqw = F32(); s.time = Get<double>();
        return s;
    }
    const uint8_t* Rest(size_t& size) const { size = ok_ ? n_ - at_ : 0; return p_ + at_; }
private:
    bool Need(size_t k) { if (!ok_ || n_ - at_ < k) { ok_ = false; return false; } return true; }
    const uint8_t* p_; size_t n_, at_ = 0; bool ok_ = true;
};

inline std::vector<uint8_t> BuildQuery(uint32_t token) {
    Writer w; w.Raw(kQueryMagic, 4); w.U32(token); return w.buf;
}
inline std::vector<uint8_t> BuildReply(uint32_t token, const ServerInfo& i) {
    Writer w; w.Raw(kReplyMagic, 4); w.U32(token);
    w.U16(i.protocol); w.U16(i.players); w.U16(i.maxPlayers); w.U8(i.password);
    w.Str(i.name); w.Str(i.mode); w.Str(i.version); return w.buf;
}
inline bool ParseReply(const void* data, size_t size, uint32_t& token, ServerInfo& out) {
    if (size < 8 || std::memcmp(data, kReplyMagic, 4) != 0) return false;
    Reader r(static_cast<const uint8_t*>(data) + 4, size - 4);
    token = r.U32(); out.protocol = r.U16(); out.players = r.U16(); out.maxPlayers = r.U16(); out.password = r.U8();
    out.name = r.Str(128); out.mode = r.Str(64); out.version = r.Str(32);
    return r.ok();
}

// Nicknames: 3..24 code points; letters (any script), digits, and _ - . [ ] ( )
inline bool ValidNick(const std::string& s) {
    size_t cps = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > s.size()) return false;
        for (size_t k = 1; k < len; ++k) if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
        if (len == 1) {
            const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || std::strchr("_-.[]()", c);
            if (!ok || c == 0) return false;
        }
        i += len; ++cps;
    }
    return cps >= kNickMin && cps <= kNickMax;
}

}  // namespace net
