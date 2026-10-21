#pragma once
#include "../shared/protocol.h"
#include "security.h"
#include <atomic>
#include <cstdarg>
#include <mutex>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace json { class Value; }
struct _ENetHost;
struct _ENetPeer;
struct _ENetAddress;

namespace srv {

struct Config {
    std::string name = "DGMP Server";
    std::string mode = "freeroam";
    std::string password;
    std::string bind;          // empty = all interfaces
    uint16_t port = net::kDefaultPort;
    int maxPlayers = 16;
    float spawnX = -85462.8f, spawnY = -301759.8f, spawnZ = -5179.7f, spawnYaw = 0;
    net::NametagMode nametags = net::NametagMode::Visible;
    uint8_t collisions = 0;    // net::CollisionMask
    bool saveWeapons = false;  // keep the weapons of the world save on join (StartWeapons=save)
    // Who the players are (docs/API.md "Steam и баны"): Off - nicknames only; Optional - the Steam account
    // is checked when the server has a Web API key, players without Steam get in unchecked; Required -
    // only players whose Steam account Steam confirmed.
    enum class SteamAuth : uint8_t { Off, Optional, Required } steamAuth = SteamAuth::Optional;
    std::string steamApiKey;          // https://steamcommunity.com/dev/apikey
    uint32_t steamAppId = 1259420;    // Days Gone
    bool reserveNicks = true;         // a nickname belongs to the Steam account that used it first
    bool localAdmin = true;           // a player joining from this computer (127.0.0.1) is an admin
    std::string rconPassword;         // "/rcon login <password>" makes a player an admin ("": off)
    std::vector<std::string> admins;  // SteamIDs; nicknames count only with SteamAuth=off
    std::string master;               // master server host[:port] to be listed on ("": not listed)
    bool http = true;                 // modes may use http.request (Http=0: not)
    // Anti-cheat (docs/API.md "Античит"): limits of the hits a client reports, strikes before a kick.
    float maxHitDamage = 500;         // one hit, before the hit zone multiplier
    float meleeRange = 600;           // units
    int maxHitsPerSecond = 30;
    int cheatKick = 10;               // strikes within a minute that kick (0: never)
};

// A quest marker shown on the map, the minimap and in the world (the game's own waypoint actor).
struct Marker {
    uint16_t id = 0;
    float x = 0, y = 0, z = 0, radius = 300;
    uint8_t category = 9, style = 0;  // EObjectiveCategories (9 = Task) / EObjectiveVisualType (0 = Basic)
    bool route = false;
    bool track = true;                // false: shown only, no enter/leave events
    std::string icon;                 // Texture2D asset path for map and world icon ("" = by category)
    std::string title, description;   // map tooltip
    uint16_t only = 0xFFFF;           // player id it is shown to; 0xFFFF = everyone
    double onlyJoinedAt = 0;          // identity of that player
};

// A spot where players press a key to do something (enter a house, talk to a shopkeeper).
struct Interaction {
    uint16_t id = 0;
    float x = 0, y = 0, z = 0, radius = 200;  // z on the ground; radius in units
    uint8_t key = 'F';                        // virtual key
    std::string text;                         // the prompt
    int32_t world = -1;                       // virtual world it is in (-1: every)
};

// A vehicle (bike) of the mode: anyone may ride it unless locked; the owner may also when locked.
struct Vehicle {
    uint16_t id = 0;
    net::VehicleState state;           // where it stands (or is driven), fuel, health
    int32_t world = 0;
    std::string owner;                 // lower-case nick ("": nobody's)
    bool locked = false;
    uint16_t driver = 0xFFFF;          // player id on it
    uint16_t passenger = 0xFFFF;       // player id behind (next to) the driver: a player's or the NPC's
    uint8_t blip = 1;                  // on the map and radar of: 0 nobody, 1 its owner, 2 everyone
    std::set<std::string> blipFor;     // and of these players (lower-case nicks): a gang's or a job's vehicles
    uint32_t blipColor = 0;            // RGBA of its radar marker for others (0: light blue)
    std::string blipIcon;              // its texture: a MAP_* name of the game's map icons ("": the bike)
    std::string model;                 // "" the bike, "golfcart", "dumptruck", "militarytruck"
    bool npc = false;                  // an NPC of the mode drives it (players do not get on it)
    std::string npcLook, npcName;      // its look ("": the default one) and the name above it
    std::vector<float> route;          // x,y,z of the points its NPC drives through (driveVehicle; the first: where it set off)
    float routeSpeed = 0;              // units a second
    bool routeLoop = false;            // round and round (back to the first point)
    double routeStart = 0;             // Now() when it set off
    std::vector<uint16_t> riders;      // a helicopter's passengers (player ids, seat order; 0xFFFF: free)
    bool Helicopter() const { return model == "helicopter"; }
};
constexpr size_t kHelicopterSeats = 4;

// An image of the mode (modes/<mode>/images): PNG, JPG or GIF, sent to the clients that lack it.
struct ImageFile { std::string name, hash, data; };

// A picture standing at a point of the world (drawn over the game, facing the screen).
struct Icon {
    uint16_t id = 0;
    float x = 0, y = 0, z = 0;
    float size = 48, distance = 5000;   // pixels on screen up close; seen within (units)
    int32_t world = -1;
    uint32_t color = 0xFFFFFFFF;        // RGBA tint
    std::string image, text;            // an image of the mode; a caption under it
    bool throughWalls = false;          // also when something stands between it and the camera
};

// A prop of the game placed by the mode's code (createObject): a gate, a barrier, furniture. It can move
// (moveObject): every client slides it from where it is to the target in the same time.
struct Object {
    uint16_t id = 0;
    std::string asset;                         // static mesh path (or a blueprint class "..._C")
    float x = 0, y = 0, z = 0, pitch = 0, yaw = 0, roll = 0, scale = 1;
    int32_t world = -1;                        // virtual world (-1: every)
    bool collision = true;
    bool moving = false;                       // from x.. to tx.. between moveStart and moveStart + moveTime
    float tx = 0, ty = 0, tz = 0, tpitch = 0, tyaw = 0, troll = 0;
    double moveStart = 0, moveTime = 0;
    void At(double now, float out[6]) const;   // where it is now (x, y, z, pitch, yaw, roll)
};

// An icon of the mode on the big map and the radar (createBlip): the game's map icons (MAP_*).
struct Blip {
    uint16_t id = 0;
    float x = 0, y = 0, z = 0;
    std::string icon = "MAP_player_marker";
    uint32_t color = 0xFFFFFFFF;              // RGBA tint
    float scale = 1;
    bool map = true, radar = true, edge = false;  // edge: on the radar's rim when it is farther away
    int32_t world = -1;
    uint16_t only = 0xFFFF;                   // shown to this player only (0xFFFF: everyone)
    double onlyJoinedAt = 0;
};

// A character standing in the world for the mode (shopkeeper, guard): a look, an animation, a name.
struct Actor {
    uint16_t id = 0;
    float x = 0, y = 0, z = 0, yaw = 0;       // z on the ground
    int32_t world = -1;
    std::string look, anim, name;             // look: asset path or "skin:"; anim: asset path
    bool solid = true;                        // players bump into it
    std::string key, group;                   // placed in the editor (world.json "actors"): its id and group
};

// A region with its own rule for damage between players (world.json "zones" from the editor, or
// createZone): "pve" - players cannot hurt each other in it (a safe town); "pvp" - they can, even
// when the mode turned PvP off (an arena). Zombies and NPCs hurt players everywhere as before.
struct Zone {
    std::string name;
    bool pvp = false;                         // type: false "pve", true "pvp"
    bool box = false;                         // shape: a circle (radius) or a box (half sizes, turned by yaw)
    float x = 0, y = 0, z = 0, yaw = 0;       // the centre (z: on the ground)
    float radius = 2000, halfX = 2000, halfY = 2000;
    float height = 5000;                      // units above and below z
    int32_t world = -1;                       // virtual world (-1: every)
    bool enabled = true;
    bool notify = true;                       // a line on the screen of the players in it
    std::string text;                         // that line ("": the default one)
    bool Contains(float px, float py, float pz) const;
};

// A box the players carry in both hands (docs/API.md "Переноска"): it lies somewhere, or a player
// holds it. Each client shows its own copy on the ground; the one who carries it holds the game's
// own carried item, the others see it in that player's hands (its gear).
struct Carry {
    uint16_t id = 0;
    std::string model = "crate";              // what it is (Carry::Models)
    float x = 0, y = 0, z = 0, yaw = 0;       // where it lies (z: on the ground); carried: where it was last put
    int32_t world = -1;                       // virtual world (-1: every)
    uint16_t carrier = 0xFFFF;                // the player holding it (0xFFFF: it lies)
    std::string tag;                          // the mode's own label ("cargo", an order id...)
    net::CarryRoll roll = net::CarryRoll::Keep;  // a dodge roll with it: nothing, it falls, it breaks
    std::string key;                          // placed in the editor (world.json "carries"): its id; getCarry("key")
    static bool KnownModel(const std::string& m);
    static const char* ModelList();           // the known models, for messages: "crate, ..."
};

// Something a player did in the world, as reported by its client (Msg::GameEvent).
struct GameEvent {
    uint8_t type = 0;               // net::GameEventType
    std::string cls, detail, id;
    float x = 0, y = 0, z = 0;
};

// A pickup placed in the world by the mode (quest items); each client spawns its own copy.
struct WorldItem {
    uint16_t id = 0;
    std::string key;
    int count = 1;
    float x = 0, y = 0, z = 0, yaw = 0;
    uint16_t only = 0xFFFF;           // player id it is shown to; 0xFFFF = everyone
    double onlyJoinedAt = 0;
};

struct Player {
    uint16_t id = 0;
    std::string nick;
    _ENetPeer* peer = nullptr;  // null for bots
    net::PlayerState state;
    bool bot = false;
    bool ready = false;         // received Hello and was welcomed
    double joinedAt = 0;
    float botPhase = 0;
    double botDeadUntil = 0;                       // a shot bot lies dead until then
    bool spectating = false;
    std::string skin;                              // the look the player made itself (character creator), kept by nick
    std::string uniform;                           // worn over the skin (a job's clothes); empty: the skin shows
    std::vector<std::string> offered;              // looks the open skin picker shows (empty: the full creator)
    bool offeredUniform = false;                   // the picker chooses a uniform, not the skin
    int32_t world = 0;                             // virtual world (setWorld): only players of the same world see each other
    uint16_t vehicle = 0;                          // server vehicle it drives (0: none)
    uint16_t passengerOf = 0;                      // server vehicle it rides as a passenger (0: none)
    uint16_t dialogId = 0;                         // the dialog open on its screen (0: none)
    net::DialogStyle dialogStyle = net::DialogStyle::MsgBox;
    std::vector<std::string> dialogItems;          // a list dialog's lines (its answer names one)
    bool inWardrobe = false, wardrobeReturn = true; // moved to the mode's skin choosing place; goes back after
    float backX = 0, backY = 0, backZ = 0, backYaw = 0;
    double poseWindow = 0; int posesInWindow = 0;  // relay rate limit
    std::map<std::string, int> inventory;          // last snapshot reported by the client
    std::string look;                              // look asset path the player wears
    std::string lang = "en";                      // language the client uses ("en", "ru"...)
    std::vector<uint16_t> insideMarkers;           // markers whose radius the player is in
    bool admin = false;
    double eventWindow = 0; int eventCount = 0;    // game event rate limit
    std::map<std::string, int> inventoryAtDeath;   // snapshot when the player died
    std::string gear;                              // last GearList payload (for players joining later)
    uint16_t lastAttacker = 0xFFFF; double lastHitAt = 0;  // who hurt this player last (the killer)
    double hitWindow = 0; int hitCount = 0;        // hit report rate limit
    int8_t pvp = -1;                               // player:setPvp: -1 the rules decide, 0 never hurts / is hurt by players, 1 always (both need it)
    std::vector<std::string> zones;                // names of the enabled zones it stands in
    std::string zoneHud;                           // the zone line on its screen now ("": none)
    uint16_t carrying = 0;                         // the box (Carry) in its hands (0: none)
    int64_t money = 0;                             // the mode's money (docs/API.md "Деньги"); not kept by the server
    std::string ip;                                // address it connects from (no port)
    uint64_t steamId = 0;                          // its Steam account, confirmed by Steam (0: not confirmed)
    uint64_t claimedSteamId = 0;                   // the account its client named (bans also match this one)
    int rconFails = 0;                             // wrong "/rcon login" passwords
    std::vector<double> strikes;                   // anti-cheat strikes of the last minute (times)
    // Protected items (protectItems): how many of each key the server let it have (lower-case keys); a
    // key the server just changed is not judged until its time here passes (the client reports late).
    std::map<std::string, int> granted;
    std::map<std::string, double> itemGrace;
    double ammoGraceUntil = 0;                     // after giveWeapon / setAmmo by weapon: any ammo it gets is fine
    double inventoryGraceUntil = 0;                // after joining or a respawn: the save's items are being taken
    bool inventorySettling = false;                // the first check after that: the save's protected items go without a strike
    std::map<std::string, double> pickedUpAt;      // the game said it picked up this key (lower-case) then
    double inventoryRecheckAt = 0;                 // judge the last snapshot again then (it had keys still in grace)
    std::string anim;                              // the animation the mode plays on it (asset path; "": none)
};

// A point of the world (logic points: z on the ground). Outside Server: g++ cannot use a nested class
// with member initialisers in a default argument of the same class (Customize's place = {}).
struct Spot { bool set = false; float x = 0, y = 0, z = 0, yaw = 0; };

class Script;

class Server {
public:
    Server();
    ~Server();
    bool Start(const Config& cfg, const std::filesystem::path& root);
    void Run();                 // returns when Quit() was requested
    void Quit() { quit_ = true; }
    void PushConsole(const std::string& line);  // thread-safe; executed on the server thread

    // ---- API used by the console and by Lua ----
    const Config& config() const { return cfg_; }
    const std::filesystem::path& root() const { return root_; }
    Player* Find(uint16_t id);
    Player* FindByName(const std::string& nick);
    std::vector<Player*> Players();
    int PlayerCount() const;
    net::ServerInfo Info() const;  // answer to status queries
    double Now() const;

    void Message(Player* to, uint32_t rgba, const std::string& text);  // to=null: everyone
    void SetTime(float hours, bool paused);
    void SetWeather(int id);
    // Only this player's sky (an interior, a dream, a cutscene); the next server-wide setTime / setWeather replaces it.
    void SetPlayerTime(Player* p, float hours, bool paused);
    void SetPlayerWeather(Player* p, int id);
    void Teleport(Player* p, float x, float y, float z, float yaw);
    void Hint(Player* to, const std::string& title, const std::string& text, const std::string& image, bool pause, float seconds = 0);
    void Kick(Player* p, const std::string& reason);
    // Bans (data/bans.json): an online player by its nickname and Steam account (address too when Steam did
    // not confirm it); minutes 0: for ever.
    void BanPlayer(Player* p, const std::string& reason, int minutes, const std::string& by);
    void AddBan(Ban b);
    int Unban(const std::string& what);            // SteamID, address or nickname
    const std::vector<Ban>& Bans() const { return bans_.All(); }
    // Anti-cheat: something the client reported is impossible. The mode hears it (cheat); enough strikes kick.
    void Strike(Player& p, const char* kind, const std::string& detail);
    // Items only the server hands out (docs/API.md "Античит"): key prefixes, compared without case.
    void ProtectItems(std::vector<std::string> prefixes);
    const std::vector<std::string>& ProtectedItems() const { return protect_; }
    // Hurts the player through its game's own damage (hit reaction, death); `attacker` becomes the killer.
    void Damage(Player* p, float amount, Player* attacker);
    void Freeze(Player* p, bool on);
    // An animation on the player (docs/API.md "Анимации игрока"); path "": stop. Flags: net::AnimFlags.
    void PlayAnim(Player* p, const std::string& path, uint8_t flags, float rate);
    // Health in percent of the maximum; 0 kills (playerDeath, the respawn cover, as any other death).
    void SetHealth(Player* p, float percent);
    // Skins (docs/API.md "Skins"): the player's own look, kept by nick in data/skins.json, and a uniform
    // over it. WearEffective puts on the uniform, else the skin.
    void SetSkin(Player* p, const std::string& skin);
    void SetUniform(Player* p, const std::string& look);
    void WearEffective(Player* p);
    // The mode's skins made in the editor (modes/<mode>/skins.json): "skins" to start with, "uniforms".
    struct SkinChoice { std::string id, en, ru, look; };
    using Spot = srv::Spot;
    const Spot& Wardrobe() const { return wardrobe_; }
    const std::vector<SkinChoice>& SkinList(const std::string& kind) const;
    const SkinChoice* FindSkin(const std::string& kind, const std::string& id) const;
    // The full character creator (choices empty), or a picker of the given skins seen on the player.
    void Customize(Player* p, bool cancel, const std::string& title, const std::string& families,
                   const std::vector<const SkinChoice*>& choices = {}, bool uniform = false,
                   const Spot& place = {}, bool back = true);
    // Spectator: the player is hidden and watches others (`only`: these players; empty: anyone).
    void Spectate(Player* p, bool on, const std::vector<uint16_t>& only);
    void HurtBot(Player* bot, float amount, Player* attacker);  // bots have no game: the server keeps their health
    void Item(Player* p, net::ItemAction action, const std::string& key, int amount);
    void Bike(Player* p, net::BikeAction action, float x = 0, float y = 0, float z = 0, float yaw = 0);
    void SetNametags(net::NametagMode mode);
    void SetCollisions(uint8_t mask);  // net::CollisionMask
    uint8_t Collisions() const { return cfg_.collisions; }
    void Objective(Player* p, uint8_t state, uint8_t category, uint8_t style, const std::string& title, const std::string& text, int xp = 0);
    void ObjectiveMeter(Player* p, bool show, const std::string& title, float percent);
    uint16_t AddMarker(Marker m);
    bool RemoveMarker(uint16_t id);
    const Marker* FindMarker(uint16_t id) const;
    // Dialogs, HUD elements, interactions and watched keys (docs/API.md "Dialogs", "HUD", "Interactions").
    uint16_t ShowDialog(Player* p, net::DialogStyle style, const std::string& title, const std::string& text, const std::string& button1,
                        const std::string& button2, const std::string& defaultText, const std::string& headers, const std::vector<std::string>& items);
    void CloseDialog(Player* p);
    void SetHud(Player* p, const std::string& id, const std::string& json);  // json "": remove; id and json "": all
    // Money (docs/API.md "Деньги"): the player's amount, shown at the top right of its screen while it is not 0.
    void SetMoney(Player* p, int64_t amount);
    void ShowMoney(Player* p);                    // (again: after the HUD was cleared)
    uint16_t AddInteraction(Interaction i);
    bool RemoveInteraction(uint16_t id);
    void WatchKey(uint8_t key, bool on);
    void SetWorld(Player* p, int32_t world);
    // Players that do not see, touch or hurt each other: other virtual worlds, or one of them is at the
    // skin choosing place (two choosing there would stand inside each other).
    static bool Apart(const Player& a, const Player& b) { return a.world != b.world || a.inWardrobe || b.inWardrobe; }
    // Vehicles (docs/API.md "Vehicles").
    uint16_t AddVehicle(Vehicle v);
    bool UpdateVehicle(const Vehicle& v);  // position, owner, lock, fuel... (a driven one keeps its driver)
    bool RemoveVehicle(uint16_t id);
    Vehicle* FindVehicle(uint16_t id) { auto it = vehicles_.find(id); return it == vehicles_.end() ? nullptr : &it->second; }
    const std::map<uint16_t, Vehicle>& Vehicles() const { return vehicles_; }
    bool PutInVehicle(Player* p, uint16_t id, bool passenger = false);  // the mode puts the player on it (it gets there itself)
    void EjectFromVehicle(Player* p);            // off the vehicle it drives
    void MarkVehicle(Player* p, uint16_t id, bool on);  // on that player's map (Vehicle::blipFor)
    // Its NPC drives it from where it is through these points (x,y,z each) at that speed (units/s);
    // the end: vehicleArrive. False: no such vehicle, or nobody drives it (npc).
    // A helicopter flies `altitude` units above the points: up from where it stands, down onto the last.
    bool DriveVehicle(uint16_t id, const std::vector<float>& points, float speed, bool loop, float altitude = 0);
    bool StopVehicle(uint16_t id);               // stops where it is now
    static std::string NickKey(const std::string& nick);
    uint16_t AddActor(Actor a);
    bool UpdateActor(const Actor& a);
    bool RemoveActor(uint16_t id);
    const Actor* FindActor(uint16_t id) const { auto it = actors_.find(id); return it == actors_.end() ? nullptr : &it->second; }
    const std::map<uint16_t, Actor>& Actors() const { return actors_; }
    std::string ResolveSkinOrLook(const std::string& want) const;  // skins.json id, "skin:", look name or path; "" if unknown
    std::string ResolveAnim(const std::string& nameOrPath) const;  // short name from data/anims.txt or full path; "" if unknown
    bool HasImage(const std::string& name) const { for (auto& i : images_) if (i.name == name) return true; return false; }
    uint16_t AddIcon(Icon i);
    bool UpdateIcon(const Icon& i);
    bool RemoveIcon(uint16_t id);
    Icon* FindIcon(uint16_t id) { auto it = icons_.find(id); return it == icons_.end() ? nullptr : &it->second; }
    // Damage between players (docs/API.md "PvP и зоны"): the mode's switch, PvE/PvP zones and the
    // players' own overrides. CanHurt is the built-in decision; the mode's setPvpRule may change it.
    bool Pvp() const { return pvp_; }
    void SetPvp(bool on);
    bool CanHurt(const Player& victim, const Player& attacker) const;
    const Zone* RuleZone(const Player& p) const;   // the zone that decides for this player (a PvE one first), or null
    void SetZone(Zone z);                          // adds, or replaces the one of that name
    bool RemoveZone(const std::string& name);
    Zone* FindZone(const std::string& name);
    const std::vector<Zone>& Zones() const { return zones_; }
    // A zone as JSON (world.json and Lua: sizes in metres) and back; FromJson changes only the fields given.
    static json::Value ZoneJson(const Zone& z);
    static bool ZoneFromJson(const json::Value& v, Zone& z, std::string& error);
    // Boxes to carry (docs/API.md "Переноска"). Give: into the player's hands (false: it holds one, or
    // no such box); Drop: out of its hands onto that spot (null: where the player stands).
    uint16_t AddCarry(Carry c);
    bool UpdateCarry(const Carry& c);             // where it lies, its world and tag (a carried one: taken out of the hands)
    bool RemoveCarry(uint16_t id);                // also out of the hands
    Carry* FindCarry(uint16_t id) { auto it = carries_.find(id); return it == carries_.end() ? nullptr : &it->second; }
    Carry* FindCarry(const std::string& key) { for (auto& [id, c] : carries_) if (!key.empty() && c.key == key) return &c; return nullptr; }
    const std::map<uint16_t, Carry>& Carries() const { return carries_; }
    bool GiveCarry(Player* p, uint16_t id);
    uint16_t DropCarry(Player* p, const float* xyzYaw = nullptr);  // the box it held (0: none)
    // Objects (docs/API.md "Объекты"): a moving one stops where it is when changed.
    uint16_t AddObject(Object o);
    uint16_t AddBlip(Blip b);
    bool UpdateBlip(const Blip& b);
    bool RemoveBlip(uint16_t id);
    Blip* FindBlip(uint16_t id) { auto it = blips_.find(id); return it == blips_.end() ? nullptr : &it->second; }
    bool UpdateObject(const Object& o);
    bool RemoveObject(uint16_t id);
    bool MoveObject(uint16_t id, const float to[6], double seconds);
    Object* FindObject(uint16_t id) { auto it = objects_.find(id); return it == objects_.end() ? nullptr : &it->second; }
    const std::map<uint16_t, Object>& Objects() const { return objects_; }
    std::string ResolveProp(const std::string& nameOrPath) const;  // a mesh name of data/props.txt or a /Game/ path; "" if unknown
    uint16_t AddWorldItem(WorldItem item);
    bool RemoveWorldItem(uint16_t id);
    // Clears the backpack and gives `inv` back, skipping keys that start with one of `drop`.
    void RestoreInventory(Player* p, const std::map<std::string, int>& inv, const std::vector<std::string>& drop);
    void SetLook(Player* p, const std::string& path);   // order: the player wears it, everyone sees it
    std::string ResolveLook(const std::string& nameOrPath) const;  // short name from data/looks.txt or full path; "" if unknown
    std::vector<std::string> LookNames(const std::string& filter) const;
    Player* AddBot(const std::string& nick);
    void RemoveBots();
    bool ReloadMode();

    void Log(const char* fmt, ...);

private:
    void HandleConsole(const std::string& line);
    void OnConnect(_ENetPeer* peer);
    void OnDisconnect(_ENetPeer* peer, uint8_t reason);
    void OnPacket(_ENetPeer* peer, const uint8_t* data, size_t size);
    void OnHello(_ENetPeer* peer, net::Reader& r);
    // A Hello waiting for Steam to confirm the player's account.
    struct PendingJoin {
        _ENetPeer* peer = nullptr; uint32_t connectId = 0;
        std::string nick, lang, ip, ticket;
        uint64_t claimed = 0;
        int attempts = 0; double started = 0, retryAt = 0;  // retryAt 0: a check is running
    };
    std::map<uint64_t, PendingJoin> pending_;
    uint64_t nextPending_ = 1;
    SteamTicketCheck ticketCheck_;
    void TickAuth();
    void TickMaster();                             // heartbeat to the master server every minute
    double nextHeartbeat_ = 0, nextResolve_ = 0;
    int heartbeats_ = 0;
    _ENetAddress* masterAddress_ = nullptr;        // resolved Master= (null: not resolved)
    void RejectPeer(_ENetPeer* peer, const std::string& nick, const std::string& why);
    // Lets the player in (bans, nicknames, the mode's playerConnecting); steamId 0: not confirmed by Steam.
    void Admit(_ENetPeer* peer, const std::string& nick, const std::string& lang, const std::string& ip, uint64_t claimed, uint64_t steamId);
    bool RconChat(Player& p, const std::string& text);  // "/rcon ...": true when handled
    struct HitStat { float max = 0; int count = 0; };
    std::map<std::string, HitStat> hitStats_;      // weapon class -> largest damage of an accepted hit (console: hitstats)
    Player* echo_ = nullptr;                       // also gets what Log prints (an admin's /rcon command)
    BanList bans_;
    NameBook names_;
    // Protected items: ledger updates for the server's own orders, the check of each snapshot.
    std::vector<std::string> protect_;
    bool Protected(const std::string& lowerKey) const;
    void Ledger(Player& p, net::ItemAction action, const std::string& key, int amount);
    void SendItem(Player* p, net::ItemAction action, const std::string& key, int amount);
    void CheckInventory(Player& p);
    void PickedUp(Player& p, const GameEvent& e);  // a pickup the game reported: credit for a dropped weapon or a mode item
    // Protected weapons that left a player's backpack (dropped, given away): whoever picks one up there may have it.
    struct Loose { std::string key; float x, y, z; double until; };
    std::vector<Loose> loose_;
    std::map<uint16_t, std::set<uint16_t>> itemTakers_;  // mode items (spawnItem): players already credited
    void OnChat(Player& p, const std::string& text);
    void Send(Player* to, const net::Writer& w, bool reliable);  // to=null: every ready client
    void SendTo(_ENetPeer* peer, const net::Writer& w, bool reliable);
    void BroadcastStates();
    void RelayPose(Player& from, const uint8_t* payload, size_t size);
    void TickBots(float dt);
    void SendMarker(_ENetPeer* peer, const Marker& m);
    void TickMarkers();
    std::vector<Zone> zones_;
    bool pvp_ = true;               // world.json "rules" "pvp", then setPvp
    double nextZoneTick_ = 0;
    void LoadZones(const std::string& mode);       // world.json "zones" and "rules", before the mode's code runs
    void TickZones();                              // who is in which zone: events and the screen line
    void UpdateZoneHud(Player* p);
    std::map<uint16_t, Marker> markers_;
    std::map<uint16_t, WorldItem> items_;
    std::map<uint16_t, Interaction> interactions_;
    uint16_t nextInteraction_ = 1, nextDialog_ = 1;
    std::set<uint8_t> watchedKeys_;
    void SendInteraction(_ENetPeer* peer, const Interaction& i);
    void SendWatchedKeys(Player* to);  // null: everyone
    std::map<uint16_t, Object> objects_;
    std::map<uint16_t, Blip> blips_;
    uint16_t nextBlip_ = 1;
    void SendBlip(Player* to, const Blip& b);       // to null: every player it is for
    uint16_t nextObject_ = 1;
    void SendObject(_ENetPeer* peer, const Object& o);   // peer null: everyone
    void TickObjects();                                 // finished moves: objectMoved
    std::map<std::string, std::string> props_;          // lower-case mesh name -> asset path (data/props.txt)
    void LoadProps();
    std::map<uint16_t, Actor> actors_;
    std::map<uint16_t, Vehicle> vehicles_;
    std::vector<ImageFile> images_;
    void LoadImages(const std::string& mode);
    void SendImageList(Player* to);    // null: everyone
    std::map<uint16_t, Icon> icons_;
    uint16_t nextIcon_ = 1;
    void SendIcon(_ENetPeer* peer, const Icon& i);
    uint16_t nextVehicle_ = 1;
    void SendVehicle(Player* to, const Vehicle& v);   // to null: everyone (each gets its own "yours")
    void SendRoute(Player* to, const Vehicle& v);     // its NPC's way (none: stopped)
    void TickVehicles();                              // where the driven ones are; the ones that arrived
    void LeaveVehicle(Player* p, const net::VehicleState* where);  // p no longer drives (or rides on) its vehicle
    std::map<uint16_t, Carry> carries_;
    uint16_t nextCarry_ = 1;
    void SendCarry(Player* to, const Carry& c);       // to null: everyone
    void OnCarryAction(Player& p, net::Reader& r);
    // The box leaves the player's hands at that spot; the mode hears it (carryDrop, reason "lost", "death", "quit").
    void LoseCarry(Player& p, float x, float y, float z, float yaw, const char* reason);
    uint16_t nextActor_ = 1;
    void SendActor(_ENetPeer* peer, const Actor& a);
    std::map<std::string, std::string> anims_;  // lower-case name -> asset path (data/anims.txt)
    void LoadAnims();
    uint16_t nextItem_ = 1;
    void SendWorldItem(_ENetPeer* peer, const WorldItem& item);
    std::string worldEdits_;      // modes/<mode>/world.json (validated), sent on join
    void LoadWorldEdits();
    void LoadSkins();
    void SaveSkins();
    std::map<std::string, std::string> skins_;   // lower-case nick -> skin
    std::map<std::string, std::vector<SkinChoice>> modeSkins_;  // "skins", "uniforms" -> list
    void LoadModeSkins(const std::string& mode);
    void LoadWorldActors(const std::string& mode);  // world.json "actors": the mode's NPCs placed in the editor
    void LoadCarries(const std::string& mode);      // world.json "carries": boxes placed in the editor
    Spot wardrobe_;  // world.json logic "wardrobe": where players choose skins
    // Time of day and weather saved with the mode in the editor (world.json "environment"); applied
    // before the mode's code runs, which may still change them.
    void LoadEnvironment(const std::string& mode);
    void SendWorldEdits(_ENetPeer* peer);
    uint16_t nextMarker_ = 1;
    void RemovePlayer(Player* p, uint8_t reason);
    uint16_t FreeId() const;

    Config cfg_;
    std::filesystem::path root_;
    _ENetHost* host_ = nullptr;
    std::map<uint16_t, std::unique_ptr<Player>> players_;
    std::unique_ptr<Script> script_;
    std::vector<std::pair<std::string, std::string>> looks_;  // name, asset path
    void LoadLooks();
    std::atomic<bool> quit_{false};
    std::mutex consoleLock_;
    std::vector<std::string> consoleQueue_;
    float timeHours_ = -1; bool timePaused_ = false; int weather_ = -2;  // -1 auto, -2 never set
    FILE* log_ = nullptr;
};

}  // namespace srv
