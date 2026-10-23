#pragma once
// DGMP network protocol shared by dgmp-server, the game client DLL and the launcher.
// Transport: ENet over UDP. Channel 0 reliable+ordered, channel 1 unreliable (player state), 2 bone
// poses, 3 voice (unreliable).
// Server status queries are raw UDP datagrams on the same port, outside ENet (SA-MP style).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace net {

constexpr uint16_t kProtocolVersion = 10;
constexpr uint16_t kDefaultPort = 7777;
constexpr int kLanPortSpan = 10;        // LAN discovery probes kDefaultPort .. kDefaultPort+span-1
constexpr int kChannels = 4;
constexpr int kChanReliable = 0, kChanState = 1, kChanPose = 2, kChanVoice = 3;
constexpr float kPoseRelayRange = 25000.f;  // 250 m: bone poses only go to players this close
constexpr int kMaxPlayersLimit = 64;
constexpr size_t kNickMin = 3, kNickMax = 24;  // in UTF-8 code points
constexpr size_t kChatMax = 256;
constexpr size_t kTicketMax = 4096;    // Steam auth session ticket as hex (the tickets are ~240 bytes)

// Raw query datagram: "DGMQ" + u32 token  ->  reply "DGMR" + u32 token + ServerInfo
constexpr char kQueryMagic[4] = {'D', 'G', 'M', 'Q'};
constexpr char kReplyMagic[4] = {'D', 'G', 'M', 'R'};
// Master server (public server list), raw UDP like the query:
//   server -> master  "DGMH" + u16 gamePort + u16 protocol   (heartbeat, from the game port, every minute)
//   client -> master  "DGML" + u32 token + u16 protocol       (list request; 0: any protocol)
//   master -> client  "DGMS" + u32 token + u16 part + u16 parts + u16 n + n x (u32 IPv4 as on the wire, u16 port)
constexpr uint16_t kMasterPort = 7700;
constexpr char kHeartbeatMagic[4] = {'D', 'G', 'M', 'H'};
constexpr char kListMagic[4] = {'D', 'G', 'M', 'L'};
constexpr char kListReplyMagic[4] = {'D', 'G', 'M', 'S'};
constexpr int kListPerPacket = 200;

enum class Msg : uint8_t {
    // client -> server
    Hello = 1,        // u16 protocol, str nick, str password, str lang, u32 steamId low, u32 steamId high (0: no Steam),
                      // str ticket: Steam auth session ticket (hex) the server checks with the Steam Web API
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
    // Freakers (docs/zombies.md): the client that owns a zombie runs its AI and reports it
    ZombieStates = 30, // u8 n, n x ZombieState: the zombies this client owns, where they are now (unreliable, ~10 Hz)
    ZombieHit = 31,    // u16 zombieId, f32 damage: this player hurt a zombie another client owns (its puppet here)
    ZombieDied = 32,   // u16 zombieId, u16 killer: a zombie this client owns died; killer: who hurt it last (this player, the
                       // player whose ZombieHit the server forwarded, 0xFFFF: neither, e.g. a fall or another freaker)
    ZombieAttack = 33, // u16 zombieId, u16 victimId, f32 damage: a zombie this client owns hurt another player (its target pawn here)
    UiRequest = 40,   // str path: send me this file of the mode's HTML UI (UiList: not in my cache)
    UiMessage = 41,   // str name, str json: the mode's page sent a message (dgmp.send) to the server's Lua (uiMessage)
    ScriptRequest = 42, // str path: send me this file of the mode's client scripts (ScriptList: not in my cache)
    ScriptMessage = 43, // str name, str json: the mode's client script sent a message (send) to the server's Lua (clientMessage)
    ScriptError = 44,   // str text: a client script failed (the server logs it for the mode's author)
    ItemAction = 45,    // u16 windowId, str json: the player did something in the mode's item window ({type = "move" | "action" | "close", ...})
    Shot = 20,        // str weapon class, f32 x,y,z: this player fired (relayed to the players nearby as PlayerShot)
    AnimEnd = 19,     // u8 reason (0 it ended, 1 the player moved): the animation the mode played (PlayAnim) is over
    CarryAction = 18,  // u16 carryId, u8 CarryOp, f32 x,y,z,yaw: pick it up / put it down there / the game dropped it there
    CustomizeDone = 10, // u8 ok, str skin: the player closed the character creator (or picked one of the offered looks)
    Gear = 9,         // GearList payload: what the player carries (weapons in hand, on the back, holstered)
    Hit = 8,          // u16 victimId, f32 damage, u8 HitZone, str weapon: this player hit another one
    Voice = 21,       // u8 VoiceKey, u8 VoiceCodec, u8 gain (%), bytes: a frame of the player's voice while a push-to-talk key is held (kChanVoice)
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
    UiList = 120,     // u16 n, n x (str path, str sha256): the files of the mode's HTML UI (modes/<mode>/ui); fetch the ones not cached
    UiData = 121,     // str path, u16 part, u16 parts, str bytes: a UI file, in parts
    Ui = 122,         // u8 UiOp: Open (str page, u8 focus), Close, Focus (u8 on), Event (str name, str json): the mode's page (docs/webui.md)
    ScriptList = 123, // u16 n, n x (str path, str sha256): the mode's client scripts (modes/<mode>/client, docs/client-lua.md); fetch the ones not cached
    ScriptData = 124, // str path, u16 part, u16 parts, str bytes: a client script's file, in parts
    ScriptEvent = 125, // str name, str json: the server's Lua (player:clientSend) to the client scripts' onServer(name, fn)
    ItemWindow = 126, // u16 windowId, str json: the mode's item window (docs/API.md "Окно предметов"): panes of items; "" closes it
    Trap = 127,       // u16 id, u8 TrapOp: Add (str type, f32 x,y,z,yaw, f32 x2,y2,z2 (a wire's other end), i32 world, u8 explosive),
                      // Fire (u16 victim, 0xFFFF none: explode / catch it), Remove (docs/API.md "Ловушки")
    VehicleRoute = 103, // u16 vehicleId, f32 speed (units/s), f32 elapsed (s since it set off), u8 loop, u16 n, n x f32 x,y,z: the way its NPC drives it (n 0: it stopped)
    Carry = 104,      // u16 id, u8 op (1 add/update: str model, f32 x,y,z,yaw, i32 world (-1 every), u16 carrier (0xFFFF: it lies there), u8 CarryRoll; 2 remove):
                      // a box the players carry in both hands (docs/API.md "Переноска"); carrier = your id: hold it, else let it go
    Icon = 102,       // u16 id, u8 op (1 add/update: f32 x,y,z, f32 size (pixels), f32 distance (units), i32 world (-1 every), u32 rgba, str image, str text, u8 throughWalls; 2 remove)
    WatchKeys = 94,   // u8 n, n x u8 virtualKey: keys whose presses the client reports (Msg::Key)
    Spectate = 89,    // u8 on, u8 count, count x u16 playerId: watch other players (count 0: anyone)
    SetHealth = 105,  // f32 percent of the maximum (0: the player dies, as from any other cause)
    Sound = 109,      // str asset path, u8 3D (1: at x,y,z), f32 x,y,z, f32 volume, f32 pitch: play it (the mode's playSound)
    PlayerShot = 110, // u16 id, str weapon class, f32 x,y,z: that player fired there (its weapon's sound)
    Blip = 108,       // u16 id, u8 op (1 add/update: f32 x,y,z, str icon (MAP_* name or path), u32 rgba, f32 scale, u8 BlipFlags, i32 world (-1 every); 2 remove)
    PlayAnim = 107,   // str animation path ("": stop), u8 AnimFlags, f32 rate: play it on the player (FullBody slot)
    Object = 106,     // u16 id, u8 op: 1 add/update (str asset, f32 x,y,z, f32 pitch,yaw,roll, f32 scale, i32 world (-1 every), u8 collision);
                      // 2 remove; 3 move (f32 x,y,z, f32 pitch,yaw,roll, f32 seconds, f32 elapsed): from where it is to there
    Freeze = 88,      // u8 on: the player cannot move (on a bike: no throttle, brake or getting off)
    Damage = 86,      // u16 attackerId, f32 damage, u8 HitZone: another player hurt you
    WorldEdits = 85,  // u16 part, u16 parts, str chunk: the mode's world.json (docs/world.md), split in chunks; "" = none
    Corpse = 84,      // u16 playerId: that player died where it stands now; leave a body there
    WorldItem = 83,   // u16 id, u8 op (1 add: str itemKey, i32 count, f32 x,y,z, f32 yaw; 2 remove): a pickup placed by the mode
    PlayerVoice = 111, // u16 id, u8 VoiceRoute, u8 VoiceCodec, u8 gain (%), f32 range (units: Near fades out there), bytes: that player's voice (kChanVoice)
    Zombie = 130,     // u16 id, u8 ZombieOp: Add (str type, f32 x,y,z,yaw, i32 world, u16 owner (0xFFFF none), f32 health (<0: the game's)),
                      // Owner (u16 owner), Damage (f32 damage, u16 attacker; to the owner), Died (u16 killer, 0xFFFF none), Remove
    ZombieMoves = 131, // u8 n, n x ZombieState: zombies other clients own, for their puppets here (unreliable)
    VoiceSetup = 112, // u8 VoiceFlags, str radio, str call: voice chat on this server and the channels you talk to (names for the HUD)
    Marker = 80,      // u16 id, u8 op (1 add/update: f32 x,y,z, u8 category, u8 style, u8 route, str icon, str title, str description; 2 remove)
};

// Voice chat (docs/API.md "Голосовой чат"): frames are Steam Voice (ISteamUser::GetVoice, Opus inside),
// relayed by the server as they are to the players who hear the speaker.
enum class VoiceCodec : uint8_t { Steam = 1 };
enum class VoiceKey : uint8_t { Talk = 0, Radio = 1 };               // which push-to-talk key the speaker holds
enum class VoiceRoute : uint8_t { Near = 0, Radio = 1, Call = 2 };   // heard at the speaker's body / over a radio / a phone
enum VoiceFlags : uint8_t { VoiceOn = 1, VoiceMuted = 2, VoiceRadio = 4, VoiceCall = 8 };
constexpr size_t kVoiceFrameMax = 2048;
constexpr uint8_t kVoiceGainMax = 250;  // the speaker's microphone volume in percent: listeners apply it (Steam frames come compressed)
constexpr size_t kVoiceNameMax = 64;

enum QuitReason : uint8_t { QuitLeft = 0, QuitTimeout = 1, QuitKicked = 2 };
// Gameplay events the client reports (see src/gameevents.cpp for where each comes from).
enum class ZombieOp : uint8_t { Add = 1, Owner = 2, Damage = 3, Died = 4, Remove = 5 };
// One zombie's state: u16 id, f32 x,y,z, f32 yaw, f32 speed (units/s), f32 health, u8 montageSeq, str montage
// (the attack/reaction animation playing, asset path; repeated while it plays so a lost packet does not drop it).
struct ZombieState { uint16_t id = 0; float x = 0, y = 0, z = 0, yaw = 0, speed = 0, health = 0; uint8_t montageSeq = 0; std::string montage; };
constexpr size_t kZombieTypeMax = 32, kZombieMontageMax = 200, kZombiesPerPacket = 24;
enum class GameEventType : uint8_t { Pickup = 1, Search = 2, Body = 3, Interact = 4, Door = 5, Explosion = 6, Fire = 7, Carry = 8,
                                    Respawned = 9, Died = 10 };  // the player died and the game gave it a new body (it reloads a checkpoint)
enum class ItemAction : uint8_t { Give = 1, Take = 2, SetAmmo = 3, TakeWeapons = 4, Clear = 5, TakeTools = 6, GiveReplace = 7 };  // GiveReplace: the weapon it displaces vanishes instead of dropping
enum ToolMask : uint8_t { ToolKnife = 1, ToolRock = 2, ToolTaser = 4 };  // amount of TakeWeapons / TakeTools
// CarryAction: Pick asks for it (hold F at it), Put asks to set it down (hold F with it), Lost reports
// that the game took it out of the hands (a hit, the bike, parkour) and where it fell; Roll: it fell in a
// dodge roll (CarryRoll::Drop); Break: it broke in one (CarryRoll::Break).
enum class CarryOp : uint8_t { Pick = 1, Put = 2, Lost = 3, Roll = 4, Break = 5 };
// What a dodge roll does to a carried box: nothing, it falls, it breaks (the server removes it).
enum class CarryRoll : uint8_t { Keep = 0, Drop = 1, Break = 2 };
enum class BikeAction : uint8_t { Give = 1, Take = 2, Mount = 3 };  // Mount: the bike to x,y,z,yaw and the player onto it
// A server vehicle where it stands: position and heading, fuel and health (0..1).
struct VehicleState { float x = 0, y = 0, z = 0, yaw = 0, fuel = 1, health = 1; };
enum class UiOp : uint8_t { Open = 1, Close = 2, Focus = 3, Event = 4 };
enum class TrapOp : uint8_t { Add = 1, Fire = 2, Remove = 3 };
constexpr size_t kUiPathMax = 160, kUiNameMax = 64, kUiToPageMax = 65536, kUiFromPageMax = 16384;
// Client scripts (docs/client-lua.md): file paths, message names and sizes each way, an error report.
constexpr size_t kItemWindowMax = 65536, kItemActionMax = 1024;  // an item window's description / one action
constexpr size_t kScriptPathMax = 160, kScriptNameMax = 64, kScriptToClientMax = 65536, kScriptFromClientMax = 16384, kScriptErrorMax = 1024;
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
enum BlipFlags : uint8_t { BlipMap = 1, BlipRadar = 2, BlipEdge = 4 };
enum AnimFlags : uint8_t { AnimLoop = 1, AnimFreeze = 2, AnimStopOnMove = 4 };
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
