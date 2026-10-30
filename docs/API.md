**English** · [Русский](API.ru.md)

# DGMP — Lua API for modes

A mode is a folder `modes/<id>/` with a `main.lua` file and a `meta.ini` description. The server (`dgmp-server.exe`) loads the selected mode, and all clients connect to it. Scripts run on the server; players see the result: messages, hints, teleports, issued items. Part of a mode can also run in the game on each player's machine: client scripts from `modes/<id>/client/` (your own HUD, keys, rays from the camera — without network delay), see ["Client scripts"](#client-scripts) and [`docs/client-lua.md`](client-lua.md).

A ready-made example is [`modes/freeroam/main.lua`](../modes/freeroam/main.lua). The list of item keys is [`docs/items.md`](items.md), of character looks — [`docs/looks.md`](looks.md).

### Example modes

| Mode | What it shows |
|---|---|
| [`freeroam`](../modes/freeroam/main.lua) | the basics: chat and commands, skins, teleports, time and weather, inventory, admin commands |
| [`rp`](../modes/rp/main.lua) | **the base of an RP server**: Steam accounts (money and position are saved), RP chat `/me /do /try`, PayDay, a gun shop with protected items, a hospital, animations `/sit /hands`, `/pay`, admin commands, jobs from `quests/`. The shop (`gunshop`), the hospital (`hospital`) and the `spawn` points are placed in the editor |
| [`race`](../modes/race/main.lua) | motorcycle races through checkpoints from the editor (logic points) |
| [`loader`](../modes/loader/main.lua) | **a loader job**: an NPC foreman, a uniform, personal markers and an on-screen line, crates to carry, cargo delivery, pay in money at the end of the shift (see "Carrying" → "A job: example") |
| [`htmlui`](../modes/htmlui/main.lua) | **HTML interface**: a status panel over the game and a "phone" on F2 (`ui/index.html`, `app.js`): buttons and an input field send messages to Lua, Lua replies to the page |
| [`clientdemo`](../modes/clientdemo) | **client scripts** (`client/main.lua` in the player's game): a speedometer and a compass over the game, the name of the player under the crosshair and G to wave at them (the server checks the distance), local `/fps` and `/ping` |
| [`zombies`](../modes/zombies/main.lua) | **zombies** (`server.ini` `Zombies=1`): a zombie zone at the spawn (or from the editor), `/horde` — a pack in front of the player, a reward for kills |
| [`inventory`](../modes/inventory/main.lua) | **the mode's own inventory and traps**: a 30 kg backpack (saved), a motorcycle trunk, fuel canisters (F — pick up, "Refuel"), a mine, a bear trap and a tripwire from the backpack are placed by aiming with a ghost preview (client script), `/boom` — an explosion |
| [`outpost`](../modes/outpost/README.md) | **Rubezh — the RP mode of the public test server**: registration, professions and shifts, a shop, a warehouse, carrying, expeditions; the player menu (F on a player): squad, trade, radio, money, medic, documents, search, tie up, duel; the downed state instead of death, and medics (`modes/outpost/social.lua`) |
| [`questtest`](../modes/questtest/main.lua) | runs a quest from `quests/<id>.json` (the "Test" button in the quest editor) |

Run any of them: `dgmp-server.exe --mode <id>` or "Host a server" in the launcher.

## Quick start

```
modes/
  myrp/
    meta.ini
    main.lua
```

`meta.ini`:
```ini
[Mode]
Name=My RP
Description=A camp of survivors, runs and objectives.
Author=Nick
Version=0.1
```

`main.lua`:
```lua
on("playerJoin", function(player)
    player:message("Welcome, " .. player.name .. "!", "#7CD67C")
    player:takeWeapons({ knife = true, rock = true })   -- start unarmed
    player:giveItem("kConsumableID_Bandage_Small", 2)
    player:hint("CAMP", "Find the elder by the campfire to get your first objective.")
end)

command("kit", function(player)
    player:giveWeapon("kWeaponID_M9", 30)
    player:tip("You got a pistol.", 4)
end)
```

To run: in the launcher choose "Host a server" and the mode, or run `dgmp-server.exe --mode myrp`. Once you save the file, the mode **reloads by itself**; there is no need to restart the server. Script errors are written to the server console and to `logs/server.log`.

## Units and conventions

- Coordinates are Unreal units: **100 = 1 meter**. The same as in the editor overlay (F2 / F5).
- `yaw` is the rotation in degrees.
- Color is a `"#RRGGBB"` string or a `0xRRGGBB` number.
- Time of day is hours in `[0, 24)`: `18.5` = 18:30.

## Events — `on(name, fn)`

One event can have several handlers; they are called in order.

| Event | Arguments | Note |
|---|---|---|
| `start` | — | the mode is loaded (including after a reload) |
| `stop` | — | the mode is being unloaded |
| `playerConnecting` | `info` | a player is joining: password, bans and the Steam check are passed, but they are not on the server yet. `info`: `name`, `steamId` (only a verified Steam ID), `verified`, `ip`. `return false` or a string — refuse entry (the string is shown to them as the reason). Whitelist, applications, a per-IP limit |
| `playerJoin` | `player` | after a mode reload it is called for everyone already on the server |
| `playerQuit` | `player, reason` | `reason`: `"left"`, `"timeout"`, `"kicked"`. The player's methods still work |
| `chat` | `player, text` | `return false` — the message will not be sent |
| `tick` | `dt` | ~30 times a second; `dt` in seconds |
| `console` | `line` | a line from the server console; `return true` — handled by the mode |
| `inventoryChange` | `player` | the player's inventory changed (the client sends a snapshot once every 2 s) |
| `gameEvent` | `player, ev` | the player did something in the world (see "Game events") |
| `markerEnter` / `markerLeave` | `player, markerId` | the player entered / left the marker's radius (can be disabled per marker: `track = false`) |
| `playerDeath` | `player, inventory` | the player died (without reloading the game: the "Respawning" screen, health is restored). The backpack stays; `inventory` is `{key = count}` at the moment of death. To take something away: `player:restoreInventory(inventory, {drop = {"kWeaponID_", "kAmmoID_"}})` or `takeItem` |
| `playerRespawn` | `player` | the player died. While they are on the "Respawning" screen, the mode picks the place: `player:setPosition(...)` and `return true` (hospital, home...). Without `return true` the server puts them at the spawn point |
| `playerInteract` | `player, target` | the player pressed the player-menu key while looking at `target` (see "Player menu"; usually handled by the `interact` library) |
| `playerDowned` | `player, attacker` | a lethal hit with `setDeathMode("downed")`: the player went down wounded (`attacker` is a player or `nil`). `return false` — they die at once, as usual (see "Downed state") |
| `playerBleedout` | `player` | the downed player bled out; `playerDeath` and `playerRespawn` follow |
| `playerHelp` | `player` | the downed player pressed H — calls for help (no more than once per 10 s) |
| `playerDamage` | `victim, attacker, damage, info` | a player hit another one (`info.zone`: `"body"`, `"head"`, `"arm"`, `"leg"`; `info.weapon`). Called only if the damage is allowed by the PvP rules (see "PvP and zones"). Return a number — the new damage, `false` — cancel |
| `vehicleEnter` | `player, vehicleId, allowed, passenger` | the player pressed E at a mode vehicle. `allowed` is what the lock decided; `passenger` — they sit in the back (someone is already at the wheel). `return false` — do not let them in, `return true` — let them in even if it is locked (see "Vehicles") |
| `vehicleExit` | `player, vehicleId` | the player got off a mode vehicle (driver or passenger) |
| `vehicleArrive` | `vehicleId` | the helicopter reached the last point and landed (see "Helicopter") |
| `zoneEnter` / `zoneLeave` | `player, zone` | the player entered / left a zone (including when the zone was disabled or removed). `zone` is the zone table (see "PvP and zones") |
| `cheat` | `player, kind, info` | the anti-cheat caught something impossible (see "Anti-cheat"). `info`: `detail`, `strikes` — how many within a minute. `return true` — the mode handles it itself, the server does not kick |
| `inventoryViolation` | `player, v` | the player has more of a protected item than the server issued (see "Anti-cheat"). `v`: `item`, `have`, `allowed`, `pickedUp` (the game has just reported picking up such an item). `return true` — keep it, otherwise the excess is taken away |
| `carryPick` | `player, carryId` | the player held F at a crate and wants to pick it up. `return false` — do not allow (see "Carrying") |
| `playerShot` | `player, shot` | the player fired a shot (no more than twice a second per player): `shot.weapon` is the weapon class, `x, y, z`. For example, a city alarm or a wanted level |
| `voice` | `player, talking, how` | the player started / stopped talking: `how` is `"talk"` or `"radio"`. `return false` at the start — they will not be heard (see "Voice chat") |
| `voiceIgnore` | `player, target, on` | the player muted `target` in their Tab list (`on = true`) or restored the sound (see "Voice chat") |
| `objectMoved` | `id` | an object reached the target of `moveObject` (see "Objects") |
| `animEnd` | `player, reason` | the `playAnim` animation ended: `"done"` — it finished playing (or the player died), `"moved"` — the player moved (`stopOnMove`) |
| `uiMessage` | `player, name, data` | the mode's page sent `dgmp.send(name, data)` (see "HTML interface"). `data` is a table / string / number / `nil` from JSON. No more than 30 per second per player |
| `carryDrop` | `player, carryId, info` | the crate left the player's hands. `info`: `x, y, z`, `reason`: `"put"` — the player puts it down here themselves (`return false` — do not put it down, the crate stays in their hands); `"lost"` — the game knocked it out of their hands (a hit, a motorcycle, parkour), `"roll"` — it fell out during a roll, `"death"`, `"quit"` — reported after the fact, the crate is lying there; `"break"` — it broke during a roll: after the handler the crate is deleted (unless the handler moved it with `setCarry`) |

## Chat commands — `command(name, fn)`

```lua
command("tp", function(player, args, target)   -- "/tp Raud"
    -- args   = the whole string after the command
    -- target = the first word, the rest of the words follow
end)
```

The name is without `/` and case-insensitive. For an unknown command the player gets "Unknown command".

## Global functions

| Function | Description |
|---|---|
| `players()` | an array of all players, including bots |
| `getPlayer(id or name)` | a player or `nil` |
| `broadcast(text [, color])` | a chat message to everyone |
| `hint(title, text [, image [, pause]])` | the game's hint window to everyone (see "Hints") |
| `tip(text [, seconds = 5])` | a "Survival tip" at the bottom of the screen to everyone |
| `setTime(hours [, paused])` | time of day for everyone; players who join later get it too |
| `setWeather(id or "auto")` | weather: a preset id or automatic change |
| `setNametags("visible" or "walls" or "hidden")` | nicknames over players: hidden behind walls / visible through walls / hidden |
| `now()` | seconds since the server started |
| `after(ms, fn)` → id | once after `ms` milliseconds |
| `every(ms, fn)` → id | repeat every `ms` (no more often than 10 ms) |
| `cancelTimer(id)` | cancel a timer |
| `sha256(text)` | a hex hash. Not suitable for passwords — use `passwordHash` |
| `passwordHash(password)` / `passwordCheck(password, hash)` | a password hash for storage (PBKDF2-SHA256 with salt, ~30 ms) / verification |
| `http.get(url, cb)`, `http.post(url, body, cb [, type])`, `http.request{...}` | requests to the internet (see "HTTP and JSON") |
| `json.encode(value)` / `json.decode(text)` | table ↔ JSON text |
| `ban(target [, minutes [, reason]])` | a ban: a player, SteamID, IP or nickname (see "Steam, admins and bans") |
| `unban(text)` → number | lift bans by SteamID, IP or nickname; returns how many were lifted |
| `bans()` | the list of bans: `{steamId, ip, name, reason, by, at, expires}` (Unix time; `expires` 0 — permanent) |
| `protectItems(prefixes)` / `protectedItems()` | items that only the server issues (see "Anti-cheat") |
| `createMarker(opts)` → id | a quest marker in the world, on the map and the radar (see "Markers") |
| `removeMarker(id)` | remove a marker |
| `objective(opts)` | the objective panel for everyone (see "Objectives") |
| `spawnItem(opts)` → id | put an item into the world (see "World items") |
| `despawnItem(id)` | remove an item |
| `createVehicle(opts)` → id | a mode vehicle (see "Vehicles") |
| `setVehicle(id, opts)` / `removeVehicle(id)` | change / remove a vehicle |
| `vehicleInfo(id)` / `vehicles()` | a vehicle / all of the mode's vehicles |
| `driveVehicle(id, opts)` / `stopVehicle(id)` | fly a helicopter along points / land where it is now (see "Helicopter") |
| `createIcon(opts)` → id | an image over a point in the world (see "Images") |
| `setIcon(id, opts)` / `removeIcon(id)` | change / remove an image |
| `looks([filter])` | look names from the catalog; a filter by path, for example `looks("npc_female")` or `looks("boozer")` |
| `log(...)`, `print(...)` | output to the server console and log |
| `require("name")` | a module from the mode's folder or from the shared folder `modes/_lib` |

### The `server` table

`server.name`, `server.mode`, `server.maxPlayers`, `server.spawn` (`{x, y, z, yaw}` from `server.ini`).

## Player

The player object is passed to events and commands. If the player has left, calling its methods raises the error "player has left". You can check with `player.valid`.

### Fields

| Field | Type | Description |
|---|---|---|
| `id` | number | the number on the server (freed after the player leaves) |
| `name` | string | nickname |
| `bot` | bool | a server bot (the `bot` console command) |
| `admin` | bool | SteamID in `Admins` in `server.ini`, joining from this same computer, `/rcon login` or `setAdmin` |
| `steamId` | string | SteamID64 if Steam verified the account; otherwise `nil` (see "Steam, admins and bans") |
| `verified` | bool | Steam verified the player's account |
| `ip` | string | the address they are connected from |
| `hasBike` | bool | the player has their own motorcycle |
| `look` | string | the name of the look the player is wearing (`nil` until the client reports it) |
| `ping` | number | ms |
| `valid` | bool | the player is still on the server |
| `data` | table | the mode's storage for this player; cleared on exit |
| `world` | number | virtual world (see "Virtual worlds") |
| `vehicle` | number | the mode vehicle the player is sitting in (at the wheel or in the back), or `nil` |
| `carrying` | number | the crate in hands (see "Carrying") or `nil` |
| `money` | number | the player's money (see "Money") |
| `anim` | string | the animation the mode is currently playing on the player (`playAnim`), or `nil` |
| `health` | number | health in percent of the game's bar (0..100), as the client sent it. The bar shows health above the "under-armor reserve" (35 of 135): at its zero the player goes down or dies |
| `passenger` | bool | the player sits in the back, not at the wheel |
| `downed` | bool | is lying wounded (see "Downed state") |

### Methods

| Method | Description |
|---|---|
| `message(text [, color])` | a private chat message |
| `position()` → `x, y, z, yaw` | position and view direction |
| `velocity()` → `vx, vy, vz` | speed, units/s |
| `setPosition(x, y, z [, yaw])` | teleport; farther than 200 m or while riding — with world loading and together with the motorcycle |
| `isOnBike()` | whether they are riding a motorcycle |
| `kick([reason])` | kick from the server |
| `setHealth(percent)` | health in percent of the game's bar; `0` — death (as from any cause: `playerDeath`, the "Respawning" screen, `playerRespawn`) |
| `heal([percent = 100])` / `kill()` | heal / kill |
| `setTime(hours [, paused])` / `setWeather(id or "auto")` | time of day / weather for this player only (interior, sleep, scene); the global `setTime`/`setWeather` will replace them later |
| `damage(amount [, attacker])` | damage through the game (a hit reaction); `attacker` becomes the killer |
| `ban([reason [, minutes]])` | ban and kick: by nickname and Steam account, and without a verified Steam also by IP; without `minutes` or with 0 — permanent |
| `hint(title, text [, image [, pause]])` | a hint window |
| `tip(text [, seconds = 5])` | a tip at the bottom of the screen |
| `setAdmin(bool)` | grant or revoke admin rights for this session |
| `objective(opts)` | the game's objective panel for this player (see "Objectives") |
| `objectiveMeter(title, percent)` | a meter under the objective, `percent` 0..1; without arguments — hide |
| `playAnim(name [, opts])` / `stopAnim()` | an animation on the player (see "Player animations") |
| `playSound(name [, opts])` | a sound for this player only (see "Sounds") |
| `setLook(name)` | put on a look: a name from [`docs/looks.md`](looks.md) or a `/Game/...` path. All players see it at once, and so do those who join later |
| `giveMoney(amount)` → amount / `setMoney(amount)` | the player's money (see "Money") |
| `giveCarry(id)` / `dropCarry([x, y, z, yaw])` | a crate into the hands / out of the hands (see "Carrying") |
| `setControls({move = false, weapons = false, actions = false})` | take away walking, weapons (shooting, aiming, melee) or all game actions (picking up, rolling, game interactions); `setControls()` — give everything back. Tied hands, a cutscene, a search |
| `ask(text, {seconds = 15}, fn(player, yes, reason))` / `cancelAsk()` / `isAsked()` | a yes/no question at the bottom of the screen (see "Interactions and keys") |
| `revive([percent = 30])` / `setDowned({bleedout =, giveUp =})` / `finishDowned()` | pick up a downed player / new time until death and until "give up" (seconds from now) / finish off (see "Downed state") |
| `setScoreboard({...})` | the player's cells in the Tab list (see "Voice chat") |

### Inventory

| Method | Description |
|---|---|
| `giveItem(key [, count = 1])` | give an item |
| `takeItem(key [, count = 1])` | take an item away |
| `giveWeapon(key [, ammo [, {replace = true}]])` | give a weapon and set its ammo. Without `replace`, a weapon pushed out of its slot falls to the ground, as in the game; with `replace` it disappears |
| `setAmmo(key, count)` | set the amount of ammo. `key` is an ammo key (`kAmmoID_*`) or a weapon key (then its ammo type). Limited by capacity |
| `takeWeapons([opts])` | take away all weapons; `opts`: `{knife = true, rock = true, taser = true}` — take the built-in tools too |
| `takeTools(opts)` | take away only the tools, for example `{rock = true}`; without an argument — the knife, the rock and the taser |
| `clearInventory()` | clear the inventory completely |
| `inventory()` → `{key = count, ...}` | the last inventory snapshot |
| `itemCount(key)` | the count from the snapshot |
| `restoreInventory(inv [, {drop = {...}}])` | clear the backpack and give `inv` (`{key = count}`), except keys that start with the prefixes from `drop` |

The client sends an inventory snapshot on changes, but no more than once every 2 seconds. Right after `giveItem`, the value of `itemCount` is still old; watch the `inventoryChange` event.

### Motorcycle

| Method | Description |
|---|---|
| `giveBike([x, y, z, yaw])` | give the player's motorcycle: next to them or at the given point. If the motorcycle already exists, nothing happens |
| `takeBike()` | take the motorcycle away; if the player is riding, they are dismounted first. Until the motorcycle is given again, the game will not return it even after fast travel |

## Character look

The game has 926 ready-made looks: ordinary people (men and women), Deacon variants and story characters (Boozer, Sarah, Ricky, Copeland...). The look changes on the fly: when handing out a character, changing a job, and so on.

```lua
on("playerJoin", function(player)
    player:setLook("m_sur_copeland_guard_08")     -- camp guard
end)
```

Limitations:
- Animations for all looks are Deacon's, female ones included.
- Freaker and body looks (`Freaker`, `deadbody`) are not meant for players.
- A weapon on the back hangs at Deacon's attachment points and may clip through bulky clothing.

## Skins: your own character and uniforms

A player has two layers of look:
- **skin** — their character. The server stores the skin by nickname in `data/skins.json` and puts it on at every join;
- **uniform** — a look on top of the skin (a job, a faction). It is put on and taken off; the skin does not change.

The uniform is visible if it is worn, otherwise the skin.

### The mode's lists (editor → "Characters")

The mode developer decides what players can dress in: in the DGMP editor, the **"Characters"** section, the mode, two tabs:
- **"Starting skins"** — the player picks a character from them (`player:customize()`);
- **"Uniforms"** — the mode puts them on over the skin (`player:setUniform("id")`) or lets the player choose (`customize{list = "uniforms"}`).

Each entry has an `id`, a name (EN/RU) and a look. The look is assembled in the character editor on a mannequin in front of the camera: a game preset as is, or your own assembly from parts. The lists are stored in `modes/<mode>/skins.json`:

```json
{
  "skins":    [ {"id": "biker", "name": {"en": "Biker", "ru": "Байкер"}, "look": "skin:m_mar_biker_01;hat=-"} ],
  "uniforms": [ {"id": "guard", "name": {"en": "Camp guard", "ru": "Охранник"}, "look": "/Game/Characters/.../m_sur_copeland_guard_08.m_sur_copeland_guard_08"} ]
}
```

### The choosing place

In "Characters" (or in "World" → "Mode logic" → `wardrobe`) you can place a **skin choosing place**: a point in the world, the arrow shows where the character looks. For example, an underground room made of props with lighting. Then `player:customize()` moves the player there, and after the choice or cancel returns them to where they were (on first join — to the spawn). While the choice is open, the player does not see other players and they do not see them: the server separates them as if they were in different virtual worlds (from the move until the return, including while the place is loading), so two players choosing do not stand inside each other and cannot hurt each other. `player.world` does not change. The camera does not go behind the room's walls. The mannequin in the editor also stands at this place.

The player sees only prepared skins: they flip through them with the arrows or pick from the list and see it on themselves at once. The server accepts only a look from the list that was shown. If the mode has no "Starting skins" list, `customize()` opens the full character editor.

### API

| | |
|---|---|
| `player.skin` | the skin look, or `nil` if the player has not chosen a character yet |
| `player.uniform` | the uniform worn, or `nil` |
| `player:customize(opts)` | choosing a look. `place` — where to choose: not set — the mode's choosing place (if placed), `false` — where the player stands, a point `{x, y, z, yaw}` (z is the ground, as with logic points); `back = false` — do not return the player after the choice (the mode sends them itself in `onDone`); `title`; `cancel = false` — cannot be closed without a choice; `list = "skins"` (default), `"uniforms"` or a table of ids (`{"biker", "guard"}`); `uniform = true/false` — the choice is put on as a uniform, not a skin (yes for `list = "uniforms"`); `full = true` — the full character editor instead of a list (`families = {"male", "female", "deacon", "story"}` — which bodies are available); `onDone = function(player, look, id) end` (`look = nil` — the player cancelled; `id` — the entry from `skins.json`). The choice is put on automatically |
| `player:setSkin(x)` | set the skin: an id from `skins.json` (first "skins", then "uniforms"), a look name from [`docs/looks.md`](looks.md), a skin string or `nil` |
| `player:setUniform(x)` | put on a uniform: an id from `skins.json` (first "uniforms"), a look name or a skin string; `nil` — take off |
| `skinList(kind)` | the mode's list: `kind = "skins"` or `"uniforms"`; elements `{id, name, nameRu, look}` |

The event `on("customize", function(player, look, id) end)` arrives every time a choice is closed.

```lua
on("playerJoin", function(player)
    if not player.skin then   -- first time on the server
        player:customize({ title = "Your character", cancel = false })
    end
end)
command("shop", function(player) player:customize({ title = "Clothing shop" }) end)
command("job", function(player) player:customize({ title = "Job", list = "uniforms" }) end)
command("duty", function(player) player:setUniform("guard") end)          -- an id from skins.json
command("offduty", function(player) player:setUniform(nil) end)
command("vip", function(player) player:customize({ list = { "biker", "motocross" } }) end)
```

In the full editor (`full = true` and the editor in "Characters") you choose the body, the base (any look of this body) and parts by slot: face, hair, beard, headwear, glasses and masks, top, arms, hands, bottom, shoes, backpack and gear, holster. Parts are taken from the game's looks (`data/parts.txt`). A skin string looks like this: `skin:<look>;<slot>=<part number>;...`; `-` removes a part.

## The mode's world: lights

Lights are placed in the editor: "World" → "Lights" → "+ Point light" or "+ Spotlight", then LMB in the world. For the selected light you set the color, brightness, range (m), the spotlight cone angle and shadows. A spotlight shines from the base of the cone, down by default; the direction is changed with the rotation gizmo. In the editor a light is seen as a small ball or cone, in the game — only the light itself. Like props, lights appear for players within 400 m. Shadows of dynamic light are expensive: turn them on for only a few.

In `world.json` a light is stored in `add` with `asset` `"light:point"` or `"light:spot"`:

```json
{"uid": "p123", "asset": "light:spot", "x": 0, "y": 0, "z": 0, "pitch": 0, "yaw": 0, "roll": 0,
 "light": {"color": [1, 0.85, 0.7], "intensity": 5000, "radius": 10, "cone": 40, "shadows": false}}
```

## Dialogs

A window over the game, like `ShowPlayerDialog` in SA-MP. While it is open, the player has a cursor and the game does not receive the mouse and keyboard. A new dialog replaces the open one (the answer to the old one will not arrive).

```lua
local id = player:dialog({
    style = "list",                    -- "msgbox" (default without items), "input", "password", "list" (default with items)
    title = "Shop",
    text = "{FFD84A}Choose an item",   -- above the input field or the list; \n — a new line
    items = { "Motorcycle\t{7CD67C}$100", { "Food", "$5" } },   -- list rows; columns separated by \t or given as a table
    headers = { "Item", "Price" },     -- column headers (optional)
    button1 = "Buy",                   -- "OK" by default
    button2 = "Cancel",                -- none by default for msgbox; for the others "Cancel" in the player's language
    default = "",                      -- initial text of the input field
    onResponse = function(player, ok, item, text)
        -- ok: the first button (Enter, double click) — true; the second button or Esc — false
        -- item: the number of the selected list row (from 1) or nil; text: the entered text or the text of the selected row
    end,
})
player:closeDialog()
```

Color in text, column headers and rows is `{RRGGBB}` codes, as in SA-MP. The event `on("dialogResponse", function(player, id, ok, item, text) end)` arrives on every response. The server accepts a response only for the dialog currently open for the player, and takes the list row's text from its own data: the client cannot substitute the text of the selected item.

## HTML interface

Your own phones, inventories, banks and menus as HTML/CSS/JS pages, like NUI in FiveM or WebView in alt:V. The page is drawn over the game (transparent background — the game is visible), and with focus it receives the mouse and keyboard. How it works — [`docs/webui.md`](webui.md).

**Files**: everything from `modes/<mode>/ui/` (subfolders allowed): `html`, `css`, `js`, `json`, `svg`, images, fonts `woff/woff2/ttf/otf`, sounds `mp3/ogg/wav`. Names — Latin letters, digits, `_ - . /`. Up to 4 MB per file, up to 32 MB in total. Clients download them on join and cache by content, like the mode's images. The page is opened at the address `https://mode.dgmp/<path>`: links inside are relative (`<link href="style.css">`, `fetch("data/items.json")`). **The page has no internet access** — put fonts, libraries and images in `ui/`.

```lua
player:ui("index.html")                    -- open a page (one per player; a new one replaces the old one)
player:ui("phone.html", { focus = true })  -- at once with the cursor and keyboard
player:uiFocus(true)                       -- give the page the mouse and keyboard (the game does not get them)
player:uiFocus(false)                      -- give them back to the game; the page stays on screen
player:uiSend("contacts", { { name = "Bob", id = 3 } })   -- a message to the page: dgmp.on("contacts", fn)
player:ui(false)                           -- close

on("uiMessage", function(player, name, data)   -- the page: dgmp.send(name, data)
    if name == "buy" then ... end
end)
```

The `dgmp` object is on the page right away; nothing needs to be included:

```js
dgmp.on('contacts', (list) => render(list));       // player:uiSend("contacts", list)
dgmp.send('buy', { item: 'kAmmoID_Pistol', count: 2 });   // on("uiMessage") with name = "buy"
dgmp.on('focus', (on) => {});                      // the page received / gave up the mouse and keyboard
if (dgmp.focused) { ... }
dgmp.off('contacts', fn);
```

- **Focus.** Without focus the page is only a picture (an HTML HUD: speedometer, money, notifications), and the player plays as usual. With focus the cursor is visible, clicks and keys go to the page, and the game stands still (as in an open dialog). Whether to close the page on Esc is up to the mode: the page catches `keydown` and sends a message (see the example). A `player:dialog` dialog opens over the page and gets input first.
- **Messages** are any JSON values. The name is up to 64 characters; data: to the page up to 64 KB, from the page up to 16 KB and no more than 30 messages per second (extra ones are dropped). Messages sent before the page has loaded arrive after it.
- **Trust.** The page runs on the player's machine, and the player can send anything: check `data` in `uiMessage` the same way as chat text (prices, amounts, distances — on the server).
- **Debugging.** `console.log` and page errors are written to the client log `logs/DGMP.log` (lines `UI page:`), and blocked internet requests go there too. You can also open the page in an ordinary browser: without the game there is no `window.dgmp`, so substitute a stub.
- **What is missing.** One page per player (several windows — inside it, as in FiveM); `alert`/`confirm` are not shown; input of CJK characters (IME) is not supported. Microsoft Edge WebView2 Runtime is required (the launcher needs it too) and Windows 10 1903+; without them the player sees a message in chat.
- **Interface logic** — in the page's JS or in the mode's client script: `onUi(name, fn)` receives `dgmp.send` instead of the server, `uiSend(name, data)` replies to the page (see "Client scripts").

Example — the [`htmlui`](../modes/htmlui/main.lua) mode: `dgmp-server.exe --mode htmlui`.

## Client scripts

The mode's Lua that runs in the game on each player's machine: the files in `modes/<mode>/client/` (entry point `main.lua`, modules via `require`) are served by the server on join, and the game runs them in a sandbox. Everything that must respond instantly: a HUD drawn every frame, keys, local chat commands, rays from the camera. Full description — [`docs/client-lua.md`](client-lua.md).

```lua
-- modes/<mode>/client/main.lua (in the game)
on("frame", function(dt)
    local w, h = screen()
    draw.text(w - 20, h - 60, string.format("%d km/h", me().speed * 0.036), { align = "right", size = 28 })
end)
bindKey("G", function() send("wave", {}) end)       -- to the server: on("clientMessage")
onServer("waved", function(data) chat(data.from .. " waves at you") end)

-- modes/<mode>/main.lua (server)
on("clientMessage", function(player, name, data)
    if name == "wave" then player:playAnim("wave") end
end)
player:clientSend("waved", { from = "Bob" })        -- to the client: onServer("waved", fn)
```

The server still checks what the client sends: a player can modify their game. Example — the [`clientdemo`](../modes/clientdemo) mode.

## HUD

Your own labels and bars on the player's screen, like textdraw in SA-MP. An element is defined by an id and a table; calling again with the same id changes it, without a table — removes it.

```lua
player:hud("score", { text = "{F2D16B}Score: 500", x = 0.97, y = 0.16, align = "right", size = 1.3 })
player:hud("hunger", { bar = 0.6, x = 0.02, y = 0.7, w = 0.12, h = 0.012, barColor = "#E07070", text = "Hunger" })
player:hud("score")      -- remove
player:clearHud()        -- remove all
```

| Field | |
|---|---|
| `x`, `y` | the anchor point, fractions of the screen (0..1) |
| `align` | `"left"` (default), `"center"`, `"right"` — relative to `x` |
| `text` | text with `{RRGGBB}` and `\n` |
| `size` | font scale (1 — normal, 1.5+ — large) |
| `color` | text color `"#RRGGBB"` or `"#RRGGBBAA"` |
| `bg` | a backing under the text (none by default) |
| `shadow` | text shadow (`true` by default) |
| `bar`, `w`, `h`, `barColor` | a bar: fill 0..1, width and height in fractions of the screen; `text` is a caption above it |
| `image`, `w`, `h` | a mode image (see "Images"): size in fractions of the screen; if one is set, the other follows the image's aspect ratio; if neither — its pixels × `size`. `color` tints it (`"#FFFFFF80"` — semi-transparent) |

The HUD is hidden in the pause menu, on the map, on loading screens and for a spectator. In the map menu (Esc) the "days survived" corner shows, from the server: the number of players, the server name and the mode. Other players' blips on the map show the nickname on hover. The game itself occupies the bottom left (health, stamina, weapon), the bottom right (mini-map) and the top right (the DGMP server line) — place your elements away from them.

## Interactions and keys

**Interaction point**: a player within the radius sees the prompt "[F] text" at the bottom of the screen, and the press is delivered to the mode. While the prompt is visible, the game itself does not receive that key. The default is F: E in the game is for getting on a motorcycle and picking up, and in multiplayer the flashlight is moved from F to L for all players.

```lua
local shop = logic.point("shop")       -- a point from the editor (modes/_lib/logic.lua)
local id = createInteraction({
    x = shop.x, y = shop.y, z = shop.z, -- z is the ground
    radius = 2,                          -- meters
    text = "Shop",
    key = "F",                           -- default; A-Z, 0-9, F1-F12
    aim = true,                          -- you must look at the point (default)
    aimHeight = 1.0,                     -- m above z: where to look (a counter, an NPC's chest)
    aimRadius = 0.5,                     -- m: the size of the target for the gaze
    priority = 0,                        -- with equal aiming the larger one wins
    onUse = function(player, id) player:dialog({ ... }) end,
})
removeInteraction(id)
```

The server checks that the player is really standing nearby. Event: `on("use", function(player, id) end)`.

**Aiming.** When several points are in range (a seller, a door, a crate, another player), the prompt goes to the one the camera is looking at: the point `aimHeight` above `z` must be within a ~25° cone from the center of the screen (the selected one holds up to ~35° so that the prompt does not flicker), with no wall between the camera and it. Of the suitable ones, the one closest to the center of the screen is chosen. If there is only one point in range, it is enough for it to be in front (as before). `aim = false` — the radius is enough for the point (a "stand in the circle" zone), but it yields to any point under the gaze. Vehicles and carrying crates get the prompt when no point is selected.

**Player menu.** `setPlayerMenu({key = "F", label = {ru = "Взаимодействие", en = "Interact"}, range = 2.5})` — for another player whom the camera is looking at within `range` meters, "[F] Interact" appears; the press arrives as the event `playerInteract(player, target)` (the server checks distance and world). `setPlayerMenu(nil)` — turn off. What to show next is up to the mode; a ready-made menu with actions and consent is the `interact` library (below).

**Yes/no question.** `player:ask(text, {seconds = 15}, function(player, yes, reason) end)` — at the bottom of the player's screen a question with "[Y] Yes [N] No" and a timer; the game does not take the cursor and does not stop. `reason`: `"answer"` or `"timeout"` (then `yes = false`). Returns `false` if the player already has a question open. `player:cancelAsk()` — remove, `player:isAsked()` — whether one is open. While a question is on screen, Y and N do not go to the game, and N does not turn on voice.

**Keys**: `bindKey("Y", function(player, key) end)` — presses of this key by players (not in chat, a menu, a dialog, and not by the dead); `unbindKey("Y")`. Event: `on("key", function(player, key) end)`. Better not to take occupied keys (WASD, E, F — interaction points, L — flashlight, Tab — player list, T — chat, N and K — voice, F3, F8, F9). The chat command `/voice` is taken by the client (voice settings).

## NPCs (actors)

Extras of the mode, like `CreateActor` in SA-MP: a seller, a guard, a bartender. They stand in place with a look, a looped animation and a name over the head. Visible to players within 250 m.

```lua
local shop = logic.point("shop")
local ava = createActor({
    x = shop.x, y = shop.y, z = shop.z,   -- z is the ground (a logic point from the editor)
    yaw = shop.yaw,                       -- where they look
    look = "f_merch_ava_bergstrom",       -- an id from skins.json, a name from docs/looks.md, a path or a "skin:..." string
    anim = "idle",                        -- a name from data/anims.txt or a path to an animation
    name = "Ava",                         -- over the head (visible from 25 m); "" — no name
    solid = true,                         -- players bump into them
    world = 0,                            -- virtual world; not set — in all worlds
})
setActor(ava, { anim = "talk", name = "Ava (busy)" })   -- change any fields
removeActor(ava)

-- Talk to the seller: an interaction point next to them.
createInteraction({ x = shop.x, y = shop.y, z = shop.z, radius = 2, text = "Talk", onUse = function(player) ... end })
```

`name` in `createActor` / `setActor` and `text` in `createInteraction` accept a string or a table `{en="Commander", ru="Комендант"}`. The server sends each participant the variant for `player.lang`; an unknown language uses English, then Russian. NPC labels are drawn with the interface font, which supports Cyrillic, and are hidden behind walls (except in the see-through-walls nametag mode).


### NPCs from the editor

Usually NPCs are placed in the editor: "World" → "NPCs" → "+ NPC", a click in the world, then the gizmo (move, rotate). For the selected NPC you set the id, group, the name over the head, the look (a choice from the mode's skins and the game's looks, with search), the animation (a choice from the catalog, with search), "solid" and the world. In the editor the NPC stands in the chosen look and animation right away. They are stored in `world.json` (`"actors"`); the server creates them itself when the mode starts, before the mode's code.

The mode's code finds them through `logic`:

```lua
local logic = require("logic")
local ava = logic.actor("shopkeeper")     -- the NPC with id "shopkeeper": {id, key, group, x, y, z, yaw, name, world, placed}
local guards = logic.actors("guard")      -- all NPCs of the group "guard"
if ava.placed then
    createInteraction({ x = ava.x, y = ava.y, z = ava.z, radius = 2, text = "Talk",
                        onUse = function(player) setActor(ava.id, { anim = "talk" }) end })
end
```

If the mode's code writes `logic.actor("shopkeeper")` or `logic.actors("guard")`, the editor shows them in "Mode logic" as NPCs ("not placed", "place", "add to group"). Without the editor the same data is given by `getActor("id")` and `worldActors()`.

NPCs created by code (`createActor`) are not seen by the editor: they are not in `world.json`. Place permanent NPCs (a seller, guards) in the editor and get them via `logic.actor` / `logic.actors`, and leave `createActor` for those who appear during the game. Do not duplicate an editor NPC through `createActor`: the server has already created it, and you would get a second one in a different place.

Animations are in `data/anims.txt`: about 6800 human animations from the game (Deacon, NPCs, story characters, scenes, poses). Short names at the start of the file:
- standing: `idle`, `idle2`, `tense`, `alert`, `guard`, `wait`, `wave`, `attention` (at attention), `at_ease` (at ease), `tired`, `injured`, `talk`, `explain`, `beckon`, `surrender`, `binoculars`, `pushups`;
- sitting: `sit`, `sit_talk`, `sit_warm` (warming hands), `sit_sleep`, `sit_look`, `sit_drink`, `patient`, `campfire`, `campfire_f`, `chair`; lying: `lie`;
- captives: `kneel` (hands on head), `prisoner` (tied up, kneeling).

Sitting animations are made for a seat the height of a crate: put the NPC on a crate, a bench or a step. Not every animation loops well, and poses from `StillPoses` are static. Deacon's animations suit both male and female looks.

Bullets do not hit NPCs: they are scenery, not an enemy. Players and motorcycles bump into them (unless `solid` is `false`).

## Virtual worlds

Like `SetPlayerVirtualWorld` in SA-MP: players in different worlds don't see each other (neither the body, nor the motorcycle, nor the nickname), don't collide and can't hurt each other. World 0 is the shared one; everyone starts in it on join.

```lua
player:setWorld(5)          -- e.g. a house interior underground: only your own people in it
print(player.world)
player:setWorld(0)          -- back
```

Worlds are layers over one map: a race, an event or an objective runs in its own world and doesn't disturb the rest, and the rest don't disturb it. Players in different worlds don't hear each other (voice, RP chat), don't see each other's shots and corpses, and `playSound` sounds with `world` play only in their own world.

The `world` field (omitted or `-1` — in all worlds) exists on NPCs (`createActor`), interaction points (`createInteraction`), vehicles, objects, blips, crates, zones, traps, zombies, markers (`createMarker`), items (`spawnItem`) and loot from the editor. A marker and an item from one world are sent only to its players; you can enter a marker and pick up an item only in its world. HUD, dialogs and messages are not limited by worlds. The map, buildings and props from the editor (`world.json`) are the same for all worlds: put things specific to a world as mode objects with `world`.

**Together with the player**, the mode's vehicle they are driving, its passenger and the crate in their hands move to the other world. A passenger moved alone gets out. The player's own motorcycle (`giveBike`) is seen by others only in their world.

**A free world for a while** (a race, an event) is issued by `newWorld()`: a number from 1000 that is not taken by another `newWorld` call or by a player. Take your own numbers below 1000. `freeWorld(n)` gives the number back; whoever is still in that world returns to world 0.

```lua
local w = newWorld()
for _, p in ipairs(team) do p:setWorld(w) end
-- ...
freeWorld(w)
```

**Watching another world:** `player:spectate(true, players, { world = n })` — the spectator stays in their own world (their own people hear them, they get their messages), but sees world `n`: its players, vehicles, objects, markers. `player.watching` is the world they are watching (`nil` if their own).

The `playerWorld(player, world, old)` event — the player moved to another world (`setWorld`, a door, `freeWorld`). When the mode reloads, everyone returns to world 0 and the issued worlds are freed.

## Races — the `race` library

A race of several players in their own virtual world ([`modes/_lib/race.lua`](../modes/_lib/race.lua)): while it runs, the racers see only each other, and the others don't see them. So you can race even through a city on an RP server: nobody interferes with other people's objectives, and nobody will stand across the track. Those who want to can watch the race from the side. After the finish everyone returns to their own world, to where they were. Several races can run at once, each in its own world.

```lua
local Race = require("race")
local logic = require("logic")
local highway = Race.new({
    name = { en = "Highway", ru = "Трасса" },
    grid = logic.points("race_grid"),                                  -- start positions (the arrow is the direction)
    checkpoints = logic.triggers("race_checkpoint", { radius = 12 }),  -- in order; the last one is the finish
    lobby = 45,                                                        -- sign-up: start 45 s after the first
    onEnd = function(race, results)
        if results[1] then results[1].player:giveMoney(500) end
    end,
})
highway:signup(logic.point("race_start"))   -- [F] there: sign up / leave, during the race — watch
command("race", function(p) highway:join(p) end)
```

| Parameter | Default | |
|---|---|---|
| `name` | `"Race"` | a string or `{en =, ru =}` |
| `grid`, `checkpoints` | — | points `{x, y, z, yaw}` and checkpoints `{x, y, z [, radius (m)]}`, usually from `logic` |
| `laps` | 1 | laps (the checkpoints are passed again) |
| `vehicle` | `"bike"` | the player's own motorcycle (whoever had none, it is taken away after the race) or a `createVehicle` model (`"golfcart"`...): created for the duration of the race |
| `lobby` | 30 | seconds of sign-up after the first one signs up, then the start; `0` — only `race:start()` |
| `minRacers` | 1 | fewer — the sign-up is extended |
| `countdown` | 10 | countdown at the start (the racers stand still) |
| `finishWindow` | 30 | seconds for the rest after the first finish |
| `maxTime` | 900 | the race ends in any case |
| `returnTo` | `"back"` | `"back"` — to where they were before the race; `"stay"` — stay at the finish (in their own world) |
| `objects` | — | `createObject` parameters (barriers, arches): placed in the race's world for the duration of the race |
| `radius` | 10 | radius of checkpoints that have none of their own, m |
| `onStart(race)`, `onFinish(race, player, place, seconds)`, `onEnd(race, results)`, `onLeave(race, player, reason, back)` | | `results`: `{ {player, name, place, time}, ..., dnf = {{player, name}, ...} }`; `reason`: `"left"`, `"death"`, `"moved"`, `"quit"`; `back` — where they were before the race |

| Method | |
|---|---|
| `race:join(player)` → `ok, why` | sign up; during the countdown it puts them on a free spot at once. `why`: `"already"`, `"other"` (in another race), `"running"`, `"full"`, `"notready"` (no track) |
| `race:leave(player)` | leave: before the start — from the sign-up, on the track — retire (DNF) |
| `race:start()` / `race:stop()` | start now / end now with the current results (the sign-up — cancel) |
| `race:watch(player)` / `race:unwatch(player)` / `race:watching(player)` | watch the race from the side (the arrows switch between racers) / stop (returns them to where they stood) |
| `race:signup(point [, opts])` | an [F] point and a map blip; `opts`: `text`, `radius`, `world`, `icon`, `color`, `blip = false` |
| `race:racers()`, `race:entry(player)`, `race.state`, `race.world`, `race.last` | racers on the track; the player's entry (`back` — where they were); `"idle"`, `"lobby"`, `"countdown"`, `"racing"`; the race's world; results of the previous one |
| `Race.of(player)` | the race the player is signed up for |

Retiring from a race: death, leaving the server, `leave`, or being moved to another world by someone else (an admin's `/goto`, a door). Checkpoints are the racer's personal markers in the race's world, with a route on the minimap. Examples: the [`race`](../modes/race/main.lua) mode and the street race in [`rp`](../modes/rp/main.lua) (points `race_start`, `race_grid`, `race_checkpoint` in the editor, `/race`, prizes).

## Vehicles

Motorcycles and other vehicles of the mode, like cars in SA-MP. The server stores where each one stands, its fuel, health, owner and lock. For all players it is the same vehicle: if someone drove off on it, it has driven off for the others too; if they left it, it stands where it was left. You can get into a mode's vehicle only with the server's permission: the player presses E, the server checks the lock and asks the mode.

A motorcycle from the game save is removed in multiplayer. The game's personal motorcycle can be issued via `player:giveBike()` (see "Motorcycle"), but it has no owner, no lock and no place shared by everyone — for RP use the mode's vehicles.

**From the editor.** Permanent vehicles (taxis, service vehicles, motorcycles for sale) are placed in the editor: "World" → "Vehicles" → "+ Vehicle", click in the world, gizmo. For the selected one you set the key, model, lock, owner (nickname), map mark, fuel and world. It is stored in `world.json` (`"vehicles"`); the server creates it when the mode starts, before the mode's code. The code finds it by key:

```lua
local taxi = getVehicle("taxi")          -- a table like vehicleInfo's (with the key field) or nil
setVehicle(taxi.id, { locked = false })
```

```lua
local id = createVehicle({
    x = 1000, y = 2000, z = -5300,   -- z is the ground
    yaw = 90,
    model = "bike",                  -- "bike" (default), "golfcart", "helicopter" (see "Helicopter"); "dumptruck", "militarytruck" — experimental
    owner = player,                  -- a player or a nickname; with no owner — belongs to no one
    locked = true,                   -- locked: only the owner can get in
    fuel = 1, health = 1,            -- 0..1
    world = 0,                       -- virtual world
    blip = "owner",                  -- who sees it on the map and radar: "owner" (default), "all", "none"
    blipColor = "#E05050",           -- the blip color for those it isn't their own (a gang's or a job's vehicle)
})
setVehicle(id, { locked = false })   -- change any fields; x, y, z — move it
removeVehicle(id)                    -- if someone is sitting in it, they are kicked out

local v = vehicleInfo(id)            -- {id, model, x, y, z, yaw, fuel, health, world, locked, blip, owner, driver, passenger}
for _, v in ipairs(vehicles()) do print(v.id, v.owner, v.driver and v.driver.name) end
```

Players:

| Method | Description |
|---|---|
| `player:putInVehicle(id [, passenger])` | put them behind the wheel; `passenger = true` — behind the driver. If the vehicle is far away, the player is moved to it |
| `player:exitVehicle()` | take them out of the vehicle |
| `player:markVehicle(id [, on = true])` | show the vehicle on the map and radar to this player (a job, a gang), `false` — remove |

Lock and mode: a locked vehicle lets in only the owner. The mode has the last word:

```lua
on("vehicleEnter", function(player, id, allowed, passenger)
    local v = vehicleInfo(id)
    if v.owner == "police" and player.data.job ~= "police" then return false end   -- service vehicle: only for its own
    if passenger then return true end                                               -- in the back — anyone
end)
on("vehicleExit", function(player, id) local v = vehicleInfo(id); print("left at", v.x, v.y, v.fuel) end)
```

**Marks.** The player sees their own vehicle as usual in the game: a blip on the map and radar, and an icon with pistons above it. Someone else's is not visible. A vehicle with `blip = "all"` or marked via `markVehicle` is visible as a small motorcycle on the radar and a tinted icon above it, in the `blipColor` color (light blue by default).

**Models.**
- `bike` — Deacon's motorcycle.
- `golfcart` — a golf cart. Doesn't break: the game's repair doesn't work on it.
- `dumptruck`, `militarytruck` — a dump truck and a military truck. **Experimental**: players pass through part of the body.

All models have reverse gear.

**Passenger.** If a player is already behind the wheel, another player near the vehicle sees "[E] Ride along". On a motorcycle — on the back seat, in a golf cart — next to them. You can't hit while sitting behind. If the driver gets out, the passenger gets out too.

## Entrances

Doors to interiors, like house and business entrances on SA-MP RP servers: each entrance has two sides, [F] at one moves the player to the other and into its virtual world. An interior is usually built from props somewhere under the map, in its own world, so that many identical "apartments" can exist in one place.

They are placed in the editor: "World" → "Doors" → "+ Door": the first click is the outside, the second is the inside. For the selected entrance you set the key, the text at [F], "locked", the outside world and the inside world; the "Other end" button selects the second side. Stored in `world.json` (`"doors"`); the server creates them itself. No code is needed; the mode can intervene:

```lua
on("door", function(player, key, side)          -- side: "a" — outside, "b" — inside
    local house = storage.get("house:" .. key)
    if house and house.owner == player.steamId then return true end   -- the owner passes even through a locked one
    if key == "bank_vault" and not player.data.police then return false end  -- don't let in
    -- nil: the entrance's lock decides
end)
setDoor("house1", { locked = true, text = "House (for sale)" })
getDoor("house1")   -- {key, text, locked, a = {x, y, z, yaw, world}, b = {...}}
doors()             -- all entrances
```

A locked entrance without the handler's permission answers the player "Locked".

## Loot

Items that lie in the world and come back some time after being picked up: medkits, ammo, materials. "World" → "Loot" → "+ Loot" places the item selected in the library's "Items" tab. For the selected one you set: the item, the count, in how many seconds it comes back (`0` — once), a tag. Stored in `world.json` (`"loot"`). It can be picked up like a `spawnItem` item (it counts for protected items too, see "Anti-cheat"); the `gameEvent` game event with `type = "pickup"` arrives as usual. The list and state — `loot()`: `{key, item, count, tag, x, y, z, present, itemId}`.

## Spawn points

In "World" → "Mode logic" every mode has a `spawn` list. If it has points, the server puts joining and respawning players on a random one of them (otherwise — on `Spawn` from `server.ini`). The `playerRespawn` handler can still choose its own place.

## Helicopter

A NERO helicopter for fast travel across the map: a taxi between pads, delivery to a job, evacuation. It is controlled not by a player but by the mode. The pilot is an NPC; the helicopter takes off, flies through the route points and lands. It flies the same way for all players, because the server distributes the route and the departure time. Experimental.

```lua
local heli = createVehicle({
    x = pad.x, y = pad.y, z = pad.z,  -- z is the pad's ground
    yaw = pad.yaw,
    model = "helicopter",
    npc = { look = "m_sur_copeland_guard_08", name = "Pilot" },   -- the pilot (look — as for an NPC; with no look — Deacon)
})

driveVehicle(heli, {
    points = { { x = 1000, y = 2000, z = -5300 }, { 5000, 9000, -5100 } },   -- points on the ground; the last one is where to land
    speed = 25,        -- m/s
    altitude = 40,     -- m above the points: takeoff, flight at this height, landing at the last one
    onArrive = function(id) end,   -- landed
})
stopVehicle(heli)      -- abort the flight: it lands where it is now
```

How it flies:
- **Takeoff and landing** are vertical, slower than flight. The rotor and body animation (takeoff, flight, landing, on the ground) switches on by itself.
- **Altitude.** The given `altitude` is held, but not lower than 25 m above the terrain under the helicopter and a few seconds ahead: before a mountain it gains height in advance.
- **Landing** is on the ground under the last point, even if its `z` is set inaccurately.
- **Route** — each new `driveVehicle` replaces the old one, and the flight starts from where the helicopter is now.

**Passengers.** The cabin has 4 seats. A player near a stationary helicopter sees "[E] Board"; inside, the view is from outside on the helicopter, the mouse rotates the camera, the player themselves is not visible. You can get out ("[E] Get out") only when the helicopter is on the ground; in flight neither the client nor the server lets you out. Those sitting in it are returned by `vehicleInfo(heli).passengers`, you can drop them off with `player:exitVehicle()`, and put them in with `player:putInVehicle(heli)`. Boarding can be forbidden in `vehicleEnter`, as for other vehicles (`passenger` will be `true`).

A taxi between two pads from the editor (logic points `heli_a` and `heli_b`): the first to board starts the countdown, after 20 seconds the helicopter flies to the other pad, lands, after 15 seconds drops everyone off and waits there for the next passengers.

```lua
local logic = require("logic")
local a, b = logic.point("heli_a"), logic.point("heli_b")
local heli = createVehicle({ x = a.x, y = a.y, z = a.z, yaw = a.yaw, model = "helicopter", npc = { name = "Pilot" } })
local atA, departing = true, false

on("vehicleEnter", function(player, id)
    if id ~= heli or departing then return end
    departing = true
    broadcast("The helicopter departs in 20 seconds", "#F2D16B")
    after(20000, function()
        local to = atA and b or a
        driveVehicle(heli, { points = { { to.x, to.y, to.z } }, speed = 30, altitude = 60, onArrive = function(vid)
            atA = not atA
            for _, p in ipairs(vehicleInfo(vid).passengers) do p:message("We have arrived", "#7CD67C") end
            after(15000, function()
                for _, p in ipairs(vehicleInfo(vid).passengers) do p:exitVehicle() end
                departing = false
            end)
        end })
    end)
end)
```

## Images

A mode's own images: PNG, JPG or GIF (first frame) in the `modes/<mode>/images` folder. Up to 2 MB and up to 2048×2048 pixels each, up to 32 MB per mode. Clients download them on join and cache them: a changed image is downloaded again, the others are not.

Where they can be shown:
- **HUD**: `player:hud("logo", { image = "logo.png", x = 0.5, y = 0.02, align = "center", h = 0.05 })` (see "HUD");
- **in the text** of dialogs and HUD: `{img:shop.png}` — an image the height of a line, e.g. `items = { "{img:box.gif} Crate\t$5" }`;
- **in the world** — above a point, turned to the camera, shrinking with distance:

```lua
local icon = createIcon({
    x = shop.x, y = shop.y, z = shop.z + 230,   -- e.g. above the seller's head
    image = "shop.png",
    size = 40,              -- height in pixels up close
    distance = 25,          -- visible from 25 m
    text = "Shop",          -- a caption under it (can use {RRGGBB})
    color = "#FFFFFF",      -- tint (can be "#RRGGBBAA")
    world = 0,              -- virtual world; not set — in all
    throughWalls = false,   -- true — visible through walls too
})
setIcon(icon, { text = "Closed" })
removeIcon(icon)
```

Images in the world are drawn over the game: walls hide them, but they are not lit and cast no shadow. Blips on the game map and radar are `createBlip` (see "Map blips").

## Objects

Game props placed by the mode's code, like `CreateObject` / `MoveObject` in SA-MP: gates, a barrier, doors, furniture, crates. Unlike props from the editor (`world.json`), they can be moved and removed during the game. Visible within 400 m, taking virtual worlds into account; the movement is the same for all players: the server distributes the target and the time.

```lua
local gate = createObject({
    model = "SM_Militia_Fort_Gate_01a",   -- mesh name from data/props.txt (the last part of the path) or asset = "/Game/..."
    x = 1000, y = 2000, z = -5300,        -- Unreal units; z is the bottom of the model, as in the editor
    pitch = 0, yaw = 90, roll = 0,
    scale = 1,
    world = 0,                            -- virtual world; not set — in all
    collision = true,                     -- false — you can walk through it
})
moveObject(gate, { z = getObject(gate).z + 400, time = 4,          -- raise by 4 m over 4 seconds
                   onArrive = function(id) print("opened") end })
moveObject(gate, { yaw = 0, speed = 1 })   -- or a speed, m/s (a rotation without a shift — 1 second)
stopObject(gate)                           -- stop where it is now
setObject(gate, { model = "...", scale = 2 })   -- change anything (the movement stops)
removeObject(gate)
getObject(gate)   -- {id, asset, x, y, z, pitch, yaw, roll, scale, world, collision, moving} — where it is now
objects()         -- all of the mode's objects
```

`moveObject` returns the movement time in seconds. The `on("objectMoved", function(id) end)` event arrives after `onArrive`. The model catalog is `data/props.txt` (13 thousand game meshes, by group); in the editor they can be seen in the "Library". The mode's objects are removed when it reloads. A moving object doesn't push players: place gates so that they don't pass through people.

```lua
-- A faction base gate: opens on F at the console if the player is in the faction, and closes after 8 seconds.
local gate = createObject({ model = "SM_Militia_Fort_Gate_01a", x = gx, y = gy, z = gz, yaw = 90 })
createInteraction({ x = px, y = py, z = pz, radius = 2, text = "Gate", onUse = function(p)
    if p.data.faction ~= "militia" then return p:tip("Locked", 3) end
    moveObject(gate, { z = gz + 450, time = 3 })
    after(8000, function() moveObject(gate, { z = gz, time = 3 }) end)
end })
```

## Player animations

Like `ApplyAnimation` in SA-MP: the mode plays a game animation on the player — sit, raise hands, wave. Other players see it, because the body pose is transmitted as usual.

```lua
player:playAnim("surrender")                                  -- hands up, until they move
player:playAnim("sit", { loop = true, freeze = true })         -- sit (put the player on a bench or a crate)
player:playAnim("wave", { loop = false, freeze = false })      -- wave once, walking is allowed
player:stopAnim()
print(player.anim)                                             -- "dcn_surrender" or nil
on("animEnd", function(player, reason) end)                    -- "done" / "moved"
```

| `opts` field | |
|---|---|
| `loop` | repeat (default `true`) |
| `freeze` | the player can't walk while the animation plays (default `true`) |
| `stopOnMove` | WASD or space interrupt the animation (default `true`; the `animEnd` event with `"moved"`) |
| `rate` | speed (1 — normal) |

Names are from `data/anims.txt`, as for NPCs (`idle`, `talk`, `wave`, `surrender`, `sit`, `sit_talk`, `kneel`, `prisoner`…) or a full path. The animation plays on the whole body (the `FullBody` slot of Deacon's animation blueprint). Death interrupts it. For RP commands like `/sit`, `/hands`, `/lay` this is enough:

```lua
for cmd, anim in pairs({ sit = "sit", hands = "surrender", wave = "wave", kneel = "kneel", lay = "lie" }) do
    command(cmd, function(p) p:playAnim(anim) end)
end
command("stop", function(p) p:stopAnim() end)
```

## Map blips

Your own blips on the big map and radar, like `SetPlayerMapIcon` in SA-MP: shops, garages, faction bases, job sites. The blips are taken from the game's map icons.

```lua
local shop = createBlip({
    x = 1000, y = 2000, z = -5300,
    icon = "MAP_objective_encampment_merchant",   -- a game map texture (can be without "MAP_": "fuel")
    title = {ru = "Магазин", en = "Shop"},        -- caption of an ordinary map point; can be just a string
    description = {ru = "Припасы", en = "Supplies"}, -- optional description on hover
    color = "#F2D16B",                            -- tint
    scale = 1,
    map = true, radar = true,                     -- where to show
    edge = false,                                 -- true: at the radar edge when far (by default — only nearby)
    world = 0,                                    -- virtual world
    player = somePlayer,                          -- only for this player (a house, a personal job)
    follow = otherPlayer,                         -- follows this player (once a second, from 3 m); disappears when they leave
})
setBlip(shop, { color = "#E07070" })
removeBlip(shop)
```

On hover the blip shows the name of an ordinary map point, without the story mission card. The `title` and `description` fields accept a string or a `{ru=..., en=...}` table; the server picks the language separately for each player.

Game icons: `MAP_objective_encampment_merchant` (trader), `MAP_objective_encampment_mechanic` (mechanic), `MAP_objective_encampment_food` (food), `MAP_objective_encampment_locker` (locker), `MAP_objective_encampment_bounty`, `MAP_fuel`, `MAP_player_safehouse` (safehouse), `MAP_objective_encampment` (camp), `MAP_encampment_job`, `MAP_objective_NERO_checkpoint`, `MAP_objective_ambush`, `MAP_nest`, `MAP_collectable`, `MAP_plant`, `MAP_alert`, `MAP_enemy`, `MAP_bike`, `MAP_player_marker` (default), etc. The radar shows blips within a radius of about 25 m around the player; distant ones — only with `edge = true`, at the edge. The mode's blips are removed when it reloads.

## Sounds

Game sounds, like `PlayerPlaySound` / `PlayAudioStreamForPlayer` in SA-MP: a horn, a shot, a siren, a door sound, the interface.

```lua
playSound("cc_wpn_m9_npc", { x = x, y = y, z = z })            -- at a world point (3D): heard by everyone in range
playSound("ui_tutorial_x_press")                                 -- to everyone, with no point (2D)
player:playSound("ui_mech_scroll")                               -- only to this player
playSound("cc_amb_infestation", { x = x, y = y, z = z, range = 300, volume = 0.6, pitch = 1, world = 0 })
```

| Field | |
|---|---|
| `x`, `y`, `z` | the sound's point; without them — 2D, no position |
| `range` | who to send a 3D sound to: players closer than this many meters (default 100). The volume itself falls off per the game's sound settings |
| `volume`, `pitch` | volume and pitch (1 — as in the game) |
| `world` | only to players of this virtual world |
| `player` | only to this player |

Names are from `data/sounds.txt` (about 3000 sounds: weapons, explosions, vehicles, doors, interface, environment, music; `cc_*` — sounds with variants, they sound good in 3D) or a full path `/Game/Audio/...`. Character lines are not in the catalog.

**Other players' shots** are audible by themselves: the shooter's client reports the shot, the server passes it to players within 300 m (of the same world), and they hear that weapon's game sound at the shooter's position. The mode learns about shots from the `playerShot` event.

## Voice chat

Push-to-talk voice, as in SA-MP and FiveM voice plugins: players hear those who speak near them, from the place where the speaker stands (3D, the volume falls off with distance), and the mode adds channels: radios on frequencies, phone calls.

**For the player.** `N` — talk, `K` — talk on the radio (if the mode gave one). The microphone is the one selected in Steam (Steam → Settings → Voice), so Steam must be running. While the key is held, "Talking" / "Radio: channel" is lit on the left of the screen. Players speaking nearby have a green dot by the health bar, and those speaking on the radio and phone are listed on the left.

**Settings — the `/voice` chat command** (the client handles it itself, it doesn't go to the server): your own microphone volume (0–250 %) with a level meter (red — overload) and a "hear yourself" check (only you hear your own voice, nothing is sent to anyone), other players' volume (0–200 %), the "talk" and "radio" keys (a button, then the desired key; Esc — no key), microphone always on, microphone off (listen only). Steam delivers the voice already compressed, so the microphone volume travels with the voice and is applied at the listeners. Saved in `DGMP.ini` next to `DGMP.exe`:

```ini
[Voice]
TalkKey=N         ; A-Z, 0-9, F1-F24, CapsLock, Mouse4, Mouse5, Mouse3, LAlt, RAlt, LCtrl, RCtrl, Tilde, None or a code 0x..
RadioKey=K        ; B in the game is the binoculars
MicVolume=100     ; your own microphone volume, 0-250 %
Volume=100        ; volume of other players' voices, 0-200 %
Microphone=1      ; 0 — don't talk at all (listening is possible)
OpenMic=0         ; 1 — the microphone is always on, no key (nearby voice and calls; the radio is still by key)
```

In chat, in a dialog and in a menu the keys don't work.

**Who hears.** The voice goes through the server (the server doesn't decode it itself, it only decides whom to forward it to):

- **nearby**: all players of the same virtual world closer than the speaker's range (`VoiceRange` in `server.ini`, 25 m by default). What is said on the radio is heard too: someone standing nearby hears the person speaking on the radio;
- **channels** of the mode: a player can be in several channels (up to 16), in each in one of these ways:

| Mode | What it sends to the channel | How it hears the channel |
|---|---|---|
| `"radio"` (default) | what is said with the radio key | as a radio: no position, with a "radio" sound |
| `"call"` | everything they say (with any key) | as a phone: no position, narrow band |
| `"listen"` | nothing | as a radio (scanner, eavesdropping) |

A channel member standing next to the speaker hears them once — through the channel.

```lua
-- Radio: a frequency from the command /r 101.5
command("r", function(player, args)
    if args == "" then player:setVoiceChannel(nil); return player:message("Radio off") end
    player:setVoiceChannel("Radio " .. args)  -- the channel name is shown on the player's screen
end)

-- A phone call: both are in the "call" channel until one hangs up
local function call(a, b)
    local ch = "Call " .. a.id .. "-" .. b.id
    a:joinVoiceChannel(ch, "call"); b:joinVoiceChannel(ch, "call")
end
local function hangUp(a, b)
    local ch = "Call " .. a.id .. "-" .. b.id
    a:leaveVoiceChannel(ch); b:leaveVoiceChannel(ch)
end

-- Megaphone: audible at 80 m while it is in the hands
player:setVoiceRange(80)        -- player:setVoiceRange(nil) — as the server's again

-- The dead are silent
on("playerDeath", function(player) player:setVoiceMuted(true) end)
on("playerRespawn", function(player) player:setVoiceMuted(false) end)

-- Don't allow talking on the radio in a "quiet" zone
on("voice", function(player, talking, how)
    if talking and how == "radio" and player:inZone("bunker") then return false end
end)
```

| Player method | |
|---|---|
| `setVoiceChannel(name [, how])` | keep only this channel (`how`: `"radio"`, `"call"`, `"listen"`); `nil` — leave all |
| `joinVoiceChannel(name [, how])` | join one more channel (or change the mode). `false` — a bad name (1–64 bytes) or already 16 channels |
| `leaveVoiceChannel([name])` | leave a channel; without a name — all |
| `voiceChannels()` | `{["name"] = "radio" / "call" / "listen", ...}` |
| `setVoiceRange(metres)` | how many meters this player is audible (`0` — only in channels); `nil` — as the server's |
| `voiceRange()` | their range, m |
| `setVoiceMuted([true / false])` | mute: everything they say goes nowhere; on their screen "Your microphone is muted" |
| `isVoiceMuted()` | |
| `isTalking()` | `false`, `"talk"` or `"radio"` — is talking right now |

| Global function | |
|---|---|
| `setVoiceRange(metres)` | the default voice range for everyone |
| `voiceRange()` | the same, m |
| `voiceChannelPlayers(name)` | players in the channel (an array) |
| `setVoiceEnabled(on)` / `voiceEnabled()` | voice chat on the server at all: turn it off for everyone on the fly and back on. The initial value is `Voice=` in `server.ini`, after the mode reloads — it again. Console: `voice on` / `voice off` |

The `voice(player, talking, how)` event: the player started (`talking = true`) or stopped talking, `how` is `"talk"` or `"radio"` (the key). `return false` at the start — nobody will hear this talk (until the key is released); the end arrives anyway. Channels, ranges and mutes are the mode's state: when it reloads they are reset (`playerJoin` will set them again).

**Player list (Tab) and personal mute.** In multiplayer Tab opens and closes the list of all players on the server: id, nickname, ping, who is talking now, the mode's own columns. At the top is "Talking now": whom you can hear this second. Each has their voice volume (0–200 %) and "Mute". This is only for yourself: the server stops sending the muted voice (no traffic and no `VoiceListeners` slot is spent). The mute and volume are stored in `DGMP.ini` → `[Players]` by the player's Steam account (without Steam — by nickname) and apply on later joins. The "Voice settings" button opens the `/voice` window. The game gets no Tab in multiplayer.

```lua
setScoreboard({ columns = { "Faction", "Level" } })   -- your own columns (up to 8); nil — none
player:setScoreboard({ "Police", 5 })                   -- the player's cells under them
on("voiceIgnore", function(player, target, on)            -- player muted target for themselves (on) / restored the sound
    -- e.g. automute: if target was muted by five in 10 minutes — target:setVoiceMuted(true)
end)
```

Columns and cells are the mode's state: after the mode reloads they are gone until the code sets them again.

**Load.** The voice is Steam Voice (Opus inside), about 2–3 KB/s per speaker. Limits in `server.ini`: `VoiceRate` — bytes per second from one player (the excess is dropped), `VoiceListeners` — how many speakers one player hears at once (a new voice waits until someone falls silent), `VoiceBandwidth` — bytes per second of voice from the whole server (0 — no limit). The `voice` console command shows who is talking, the channels, and how much was passed and dropped.

## Downed state

Like a "knockdown" on RP servers: a lethal hit doesn't kill but puts the player on the ground. Enabled by the mode:

```lua
setDeathMode("downed", {
    bleedout = 300,      -- seconds until death from blood loss
    giveUpAfter = 30,    -- after how long they can give up (hold E for 3 s)
    finish = true,       -- a hit on the downed player (a player, a zombie, an NPC) finishes them off; false — has no effect
    voice = true,        -- the downed player speaks by voice; false — silent
    chat = true,         -- the downed player writes in chat and enters commands; false — no (admins can)
    anim = "lie",        -- pose (data/anims.txt or a path)
})
setDeathMode("instant")  -- as before: the "Respawning" screen at once (default)
```

**The downed player** lies down (the pose holds even if the game plays a hit reaction), doesn't walk, doesn't shoot and uses nothing: no game actions, no interaction points, no mode and client script keys; the mode's animations (`/sit`) don't raise them. There's a red frame at the screen edges, in the middle "You are badly wounded 3:41", **H** — call for help (the `playerHelp` event), **hold E for 3 s** — give up, when allowed. Voice and chat work (unless turned off), the Tab list and yes/no questions. A motorcycle throws the downed player off, and the crate falls from their hands.

The player falls when **the game's health bar is empty**. The bar shows health above the "under-armor reserve" (`UnderArmorHealth`, 35 of 135), and the game itself stops the first strong hit on this reserve. So previously a player with an empty bar stood until the next hit. Health percentages (`player.health`, `setHealth`, `revive`) are percentages of the bar.

**The end of the downed state** is decided by the server:
- `player:revive(percent)` — gets up with `percent` health;
- bled out (`playerBleedout`), gave up (E), finished off by a hit (`finish`), `player:finishDowned()` — an ordinary death: `playerDeath` (the killer is whoever knocked them down, unless another finished them off), then `playerRespawn`;
- `player:setDowned({bleedout = 60, giveUp = 0})` — a new time (e.g. "no medics — can give up at once").

The others see the lying player through the body pose; in the player menu the view of a downed player is directed lower. Shots by a downed player at players are discarded. `return false` in `playerDowned` — death at once (this is what the `instance` library does in the arena and in a duel). When the mode reloads, downed players get up.

Ready-made help is the `medic` library (below). An example is the `outpost` mode (Rubezh).

## Player menu — the `interact` library

`modes/_lib/interact.lua`: look at a nearby player, press F — a list of actions the mode allowed for this pair right now. An action can require another player's consent (a yes/no question on their screen).

```lua
local interact = require("interact")
interact.add({
    id = "squad_invite", order = 10,
    label = { ru = "Пригласить в отряд", en = "Invite to the squad" },   -- a string, {ru, en} or function(player, target)
    range = 3,                                                           -- m
    when = function(player, target) return not sameSquad(player, target) end,
    consent = { ru = "%s зовёт вас в отряд", en = "%s invites you to the squad" },  -- %s — the asker's name
    seconds = 15, cooldown = 10,                                         -- to answer / between questions to this player
    downed = false,                                                      -- show on a downed player too
    onUse = function(player, target) end,                                -- after "yes" (or at once without consent)
    onRefuse = function(player, target, reason) end,                     -- "no", "timeout", "busy", "far"
})
interact.remove("squad_invite")
interact.setup({ key = "F", label = { ru = "Взаимодействие", en = "Interact" }, range = 2.5 })
interact.open(player, target)        -- a menu from code
interact.near(a, b, metres)          -- whether nearby (and in the same world)
```

The menu turns on by itself when the first action is added (`setPlayerMenu`). Before executing, the server checks the distance and `when` again.

### Squad — `squad`

```lua
local squad = require("squad")
squad.setup({ max = 6, chat = "sq", radio = false, color = "#7CD67C" })
squad.of(player)        -- {id, leader, members = {id, ...}} or nil
squad.same(a, b); squad.join(player, leader); squad.leave(player)
squad.onChange = function(s) end
```

In the player menu: "Invite to the squad" (with consent) and "Leave the squad". Squad members see each other on the map and radar (blips with `follow`); `/sq text` — squad chat, `/squad` — the roster, `/squad leave`, `/squad kick <id>`, `/squad lead <id>`. `radio = true` — a shared squad radio channel (`joinVoiceChannel`; don't combine with the mode's `setVoiceChannel`: it keeps only one channel). A squad of one person is disbanded.

### Trade — `trade`

A window for two: the backpack on the left, the "table" on the right — your own offers, the other's offers, money (+$10 / +$100) and a "Ready" button. Any change clears "Ready" for both. When both are ready, the server rechecks that each has what they offered and that they have enough room, and swaps everything at once (or nothing). What a backpack is, is decided by the mode:

```lua
local trade = require("trade")
trade.setup({
    list = function(p) return { { id = "food", name = "Ration", count = 3, icon = "food.png" } } end,
    take = function(p, id, n) return true end, give = function(p, id, n) end,
    fits = function(p, gets, gives) return true end,                     -- optional: whether they can carry it
    money = { get = function(p) end, take = function(p, n) end, give = function(p, n) end },  -- optional
    allowed = function(p, t) return true end,                            -- optional
})
trade.onTrade = function(a, b, offerA, offerB) end
```

In the player menu: "Offer a trade" (with consent). The trade is cancelled if the players move farther than 6 m apart, someone closes the window, leaves or dies.

### Medic — `medic`

Help for downed players (`setDeathMode("downed")`): the downed player presses H, medics on the server get a "accept the call?" question, the first to answer "yes" gets a route, and the downed player gets the message "A medic is on the way". If there are no medics, the downed player is told so, and (by default) can give up at once. In the player menu a medic on a downed player has "Revive": a few seconds kneeling with a progress bar, then the downed player gets up.

```lua
local medic = require("medic")
medic.setup({
    isMedic = function(p) return p.data.job == "medic" end,   -- nil: anyone can revive and accept calls
    seconds = 5, health = 30,                                  -- how long to revive, health afterwards
    use = function(m, patient, check) return true end,         -- check: only verify; false, "reason" — not allowed
    noMedicGiveUp = true, command = "medic",                   -- /medic <id> — accept a call manually
})
medic.onRevive = function(m, patient) end
```

## Instances and duels — the `instance` library

Sub-modes within one server: a duel, an arena, a firing range. Players go into their own virtual world with the instance's equipment and health; on leaving everything is returned — place, world, backpack, health, PvP. Dying inside costs nothing: no downed state, no fee, the round goes on.

```lua
local instance = require("instance")
local arena = instance.new({
    name = { ru = "Арена", en = "Arena" },
    world = "new",                                -- its own world (newWorld) or the number of a shared arena world
    spawns = { {x =, y =, z =, yaw =} },          -- start places in a circle; "here" — around the first player
    loadout = { weapons = { kWeaponID_M9 = 60 }, items = { kItemID_Bandage = 2 } },
    health = 100, rounds = 3, timeLimit = 180, countdown = 3,
    onEnd = function(inst, result) end,           -- result: {winner, score = {[id] = n}, reason}
    onLeave = function(inst, player, reason) end,
})
arena:join(player); arena:start(); arena:leave(player); arena:finish(winner)
instance.of(player)                               -- the player's instance or nil
instance.duel(a, b, { rounds = 3, loadout = {} }) -- two players, right where they stand
instance.enableDuels({ rounds = 3, loadout = {}, allowed = function(a, b) end })  -- "Challenge to a duel" in the player menu
```

The player's snapshot is stored in `storage` (`instance:<player>`) until the player has returned: after a crash, a kick or a server restart the items come back on the next join. The backpack is taken from the client's last snapshot (up to 2 s old). The knife, the rock and the stun gun stay with the player. A mode that saves the player's inventory itself (like Rubezh) must not save it while `player.world ~= 0`.

## PvP and zones

The server decides damage between players. The order is:

1. `player:setPvp(false)` on either of the two — no damage. `player:setPvp(true)` on **both** — damage everywhere, even in a safe zone (a duel).
2. One of the two stands in a **PvE zone** — no damage. You cannot shoot from a safe zone either.
3. The mode's PvP is on (`setPvp(true)`, the default) — there is damage. Off — there is damage only if **both** stand in **PvP zones** (an arena on a peaceful server).
4. The mode rule `setPvpRule(fn)`, if set, receives this decision and may change it.
5. Then the `playerDamage` handlers change or cancel the damage itself.

Zombies, NPCs and falls hurt players everywhere, as usual: zones only affect damage from other players. For the shooter, a hit on another player's body looks as usual (blood, reaction), but the victim takes no damage. Damage from code (`player:damage`) does not follow the rules.

```lua
setPvp(false)                          -- peaceful server: players do not hurt each other (except in PvP zones)
print(pvp())                           -- whether the mode's PvP is on

-- Conditions: police may shoot wanted players even in a safe zone, teammates do not hurt each other.
setPvpRule(function(victim, attacker, info)
    if attacker.data.job == "police" and victim.data.wanted then return true end
    if attacker.data.gang and attacker.data.gang == victim.data.gang then return false end
    -- nil: keep the server's decision (info.allowed)
end)
setPvpRule(nil)                        -- remove the rule

player:setPvp(false)                   -- newcomer: nobody hurts them and they hurt nobody (like god mode against players)
player:setPvp(nil)                     -- back to the general rules
if canDamage(a, b) then ... end        -- whether b can hurt a right now (taking the mode's rule into account)
```

The rule's `info`: `allowed` (the server's decision), `zone` and `weapon` (on a hit), `victimZone`, `attackerZone` (the name of the zone that decides for the player, or `nil`).

### Zones

Zones are placed in the editor: "World" → "PvP and zones" → "+ PvE zone" / "+ PvP zone"; a click in the world sets the center. A selected zone has a name, type, shape (a circle with a radius or a rectangle with length, width and rotation), height up and down from the center, "enabled at start", an on-screen text and a world. In the editor a zone is shown as posts along its border: PvE green, PvP red. The "Player damage (PvP)" checkbox in the same place is the initial value of `pvp()`. Stored in `world.json` (`"zones"`, `"rules"`); the server loads them before the mode code.

A player in a zone that changes the rule (PvE while PvP is on, PvP while it is off) sees a line at the top of the screen: "Safe zone" / "PvP zone". Set your own text with the `text` field; no line with `notify = false`. If zones overlap, PvE decides.

```lua
createZone({ name = "town", type = "pve", x = -85000, y = -301000, z = -5180, radius = 120 })   -- circle, meters
createZone({ name = "pit", type = "pvp", x = 1000, y = 2000, z = 0, length = 40, width = 25, yaw = 30, text = "{E07070}Arena" })
setZone("town", { enabled = false })   -- turn off for a while (a raid on the town), then enabled = true
setZone("pit", { radius = 60 })        -- any field except the name can be changed
removeZone("pit")
local z = getZone("town")              -- the zone table or nil
for _, z in ipairs(zones()) do print(z.name, z.type) end
zonesAt(x, y, z [, world])             -- names of the enabled zones at a point
player:zones()                         -- names of the zones the player stands in
player:inZone("town")                  -- true / false
print(player.zone)                     -- the zone that decides their PvP, or nil
print(player.pvp)                      -- true / false / nil (setPvp)
```

| Zone field | |
|---|---|
| `name` | name (`createZone` without a name gives `zone1`, `zone2`…; the same name replaces the zone) |
| `type` | `"pve"` (default) or `"pvp"` |
| `shape` | `"circle"`, `"box"` or `"polygon"` (with `points` — a polygon; `width`/`length` — a rectangle) |
| `x`, `y`, `z` | center, Unreal units (`z` — ground level) |
| `radius` | circle, meters (default 20) |
| `length`, `width`, `yaw` | rectangle: length along `yaw`, width, rotation in degrees |
| `points` | polygon: 3–128 vertices `{x=..., y=...}` **in meters relative to the zone center**, rotated together with `yaw` |
| `height` | meters up and down from `z` (default 50) |
| `world` | virtual world (`-1` — all of them) |
| `enabled` | whether the zone is active (default `true`) |
| `notify`, `text` | the on-screen line for a player in the zone and its text (`{RRGGBB}` — colors) |

Zones created from code live until the mode is reloaded; after that only the zones from `world.json` remain, `pvp()` comes from it too, and `player:setPvp` and `setPvpRule` are reset.

The polygon outline closes automatically; do not repeat the first vertex at the end. Concave outlines (for example L-shaped), either winding direction, and moving and rotating the whole zone are allowed. Self-intersections, overlapping edges and coinciding vertices are rejected. Points on the border count as inside the zone. `height`, `world`, `enabled`, notifications and PvP rules work as for the other shapes. Holes inside the outline are not supported. The polygon shape is available to PvE/PvP zones; zombie spawn zones are still circular.

```lua
createZone({name="camp", type="pve", shape="polygon", x=10000, y=20000, z=300, height=30,
    points={{x=0,y=0},{x=100,y=0},{x=100,y=40},{x=40,y=40},{x=40,y=100},{x=0,y=100}}})
setZone("camp", {yaw=45})                 -- rotate the whole outline
local outline=getZone("camp").points     -- the same local meters
```

In the editor: "World → PvP and zones", place and select a zone, switch the shape to "Polygon" in the separate "Inspector" window, press "Draw outline". Left-click on the ground adds a vertex, Enter finishes the outline, Backspace removes the last one, Esc restores the previous outline. Vertices can be edited in the list or with the "Move" button followed by a click on the ground. An invalid outline is marked red and is not saved to the main `world.json`; autosave keeps its draft in `.autosave/world.json`. A saved outline supports undo/redo, duplication and moving like the other zones.

## Hints

- **Window** — `hint(title, text, image, pause)` / `player:hint(...)`. The game's native window, like the "Infestation Zones" tutorial windows. Closed by holding E.
  - `image` — a path to a game texture, for example `"/Game/Libraries/UI/Textures/MAP_bike.MAP_bike"`. Without an image the window is compact.
  - `pause = true` — has no effect in multiplayer: the shared world is not stopped (the pause menu and the map also open without a pause).
- **Tip** — `tip(text, seconds)` / `player:tip(...)`. A "Survival tip" line at the bottom of the screen. Does not interfere with controls and disappears on its own after `seconds` seconds.
- Chat messages — `player:message` / `broadcast`.

## Markers

```lua
local id = createMarker({
    x = 1000, y = 2000, z = -5000,  -- exact coordinates (game units, 100 = 1 m), including height
    radius = 5,                     -- m: radius for markerEnter/markerLeave
    category = "task",              -- icon and color: story, camp, infestation, ambush, nero, killednpc, nest, horde, campmission, task
    style = "basic",                -- look: basic, kill, killarea, defend, defendarea, search, track, nokill, nero
    route = true,                   -- road route on the map and minimap (for the newest such marker)
    player = somePlayer,            -- visible only to them; without the field — to everyone
    track = true,                   -- false: display only, no enter/leave events
    title = "Gas station", description = "Hint when selected on the map",
    icon = "/Game/...",             -- your own icon texture (optional)
    onEnter = function(player, id) end, onLeave = function(player, id) end,
})
```

## Objectives

The game's native objective panel (top left), the "New objective" banner, and the "Objective complete" screen with experience.

```lua
player:objective({ title = "Supplies for the camp", text = "Get to the gas station",
                   state = "new", category = "camp", style = "basic", xp = 0 })
```

- `state`: `new` (with the banner), `update` (change the line), `complete` (the "Objective complete" screen, `xp` — the amount of experience on it), `failed`, `remove` (remove the panel).
- `player:objectiveMeter("Search", 0.4)` — a meter under the objective.
- It is usually easier to use the `quest` library (below).

## Game events

`on("gameEvent", function(player, ev) ... end)`. Fields of `ev`:

| Field | Description |
|---|---|
| `type` | `pickup` — picked up an item; `carry` — picked up a gas can, cylinder, etc.; `search` — opened a trunk, hood, ambulance; `body` — searched a corpse; `interact` — a sign, collectible and other interactions; `door` — opened or kicked in a door; `explosion` — an explosion (barrel, cylinder, gas can, grenade); `fire` — arson (Molotov, fire from a gas can) |
| `class` | the game object's class, for example `BP_Interact_SearchCarTrunk_C` |
| `item` | the item key for `pickup`/`carry` |
| `id` | the object's name in the world: permanent for placed objects (`BP_Interact_SearchCarTrunk21`), `dgmp_item_<id>` for items from `spawnItem` |
| `x, y, z` | where it happened |

Explosions and arson are world events near the player, not necessarily caused by them.

## World items

```lua
local id = spawnItem({ item = "kWeaponID_Molotov_Small", count = 1, x = .., y = .., z = .., player = p })
```

The item appears on the client when a player is closer than 200 m, and is placed on the ground. It is picked up with E (a prompt appears nearby), **even with a full backpack**: it goes into the inventory only if there is room, but the `gameEvent` event with `type = "pickup"` and `id = "dgmp_item_<id>"` always arrives. The game's "take/MAX" for such items is disabled. `despawnItem(id)` removes the item. The mode's items and markers are removed when it is reloaded.

## Money

The player's money, like `GivePlayerMoney` in SA-MP (example — the [`loader`](../modes/loader/main.lua) mode: pay for a shift). While the amount is not 0, it is shown at the top right of the screen (`$ 120`, debt in red).

```lua
player:giveMoney(5)          -- add; returns the new amount
player:giveMoney(-20)        -- take away (the amount can go negative — check it yourself)
player:setMoney(0)
if player.money >= 100 then ... end
```

The server does not store money: it is lost when the player leaves and when the mode is reloaded. To keep it, the mode puts it in `storage` by nickname:

```lua
on("playerJoin", function(p) p:setMoney(storage.get("money:" .. p.name:lower()) or 0) end)
on("playerQuit", function(p) storage.set("money:" .. p.name:lower(), p.money) end)
```

`player:clearHud()` does not remove the money line.

## Carrying

Crates carried in front of you with both hands, like the dynamite crate in the story: the native pickup animation, walking with the crate, a player with a crate does not hit. What a roll does to the crate is up to the mode (`roll`). The basis for warehouse work, cargo delivery and the like.

- The crate lies on the ground for all players (appears closer than 200 m). The prompt "[F] Pick up (hold)": the player holds F, the server checks that they are nearby, the mode may refuse (`carryPick`), and the crate ends up in their hands.
- With a crate in hands: "[F] Put down (hold)" — the crate is placed in front of the player (`carryDrop`, `reason = "put"`; the mode may refuse or take the crate — this is how cargo hand-in is done).
- If the game knocked the crate out of the hands (a hit, got on a motorcycle, parkour), or the player died or left, the crate stays where it fell (`carryDrop` with `reason` `"lost"`, `"death"`, `"quit"`).
- Roll (`roll`): `"keep"` — the crate stays in the hands (default); `"drop"` — it falls out and lies there (`reason = "roll"`); `"break"` — it breaks (`reason = "break"`), and after the handler the crate disappears: so the work starts over (place a new one at the warehouse).
- Other players see the crate in the carrier's hands. The mode's interaction point prompt on F takes priority over the crate's prompt.

Crates are usually placed in the editor: "World" → "Carrying" → "+ Box", a click in the world, then the gizmo (move, rotate). A selected crate has a key, model, tag, world and what a roll does; a new crate takes the model, tag, world and roll of the selected one. In the editor the crate immediately appears in its model, with the key and tag above it. Stored in `world.json`:

```json
"carries": [
    { "key": "box1", "model": "crate", "x": 1234, "y": -5678, "z": 90, "yaw": 45, "tag": "cargo" },
    { "key": "box2", "model": "crate", "x": 1300, "y": -5600, "z": 90, "yaw": 0, "world": 2, "roll": "break" }
]
```

The server places them when the mode starts (and reloads), before the mode code; the code finds them by key — `getCarry("box1")` — or by tag — `carries("cargo")`. A crate from the editor is no different from one created by `createCarry`: it can be moved, removed, put into hands.

```lua
local id = createCarry({
    model = "crate",            -- model: "crate", "cooler", "medcase", "centrifuge" (see the table below)
    x = .., y = .., z = ..,     -- z — ground
    yaw = 0,
    world = 0,                  -- virtual world; not set — in all of them
    tag = "cargo",              -- the mode's own tag
    key = "box7",               -- your own key for getCarry("box7"); optional, must be free
    roll = "keep",              -- roll with it: "keep", "drop", "break"
})
setCarry(id, { x = .., y = .., z = .. })  -- move (from hands too), change world/tag/roll
removeCarry(id)                           -- remove (from hands too)
getCarry(id)    -- {id, key, model, x, y, z, yaw, world, tag, roll, carrier} or nil; carrier — the player carrying it
getCarry("box1")  -- the same by key (crates from the editor)
carries("cargo")  -- all crates (with this tag)

player.carrying             -- id of the crate in hands, or nil
player:giveCarry(id)        -- straight into hands (with the pickup animation); false — hands are busy
player:dropCarry()          -- put down at the feet; player:dropCarry(x, y, z, yaw) — at a point. Returns the id

-- Warehouse: a crate put down at the hand-in point counts.
on("carryDrop", function(player, id, info)
    if info.reason == "put" and near(info, depot, 4) then
        removeCarry(id)
        player:message("Crate delivered")
    end
end)
```

| Model | What it is |
|---|---|
| `crate` | dynamite crate (story), two hands |
| `cooler` | portable cooler (story: NBE) |
| `medcase` | antibiotics case (story: NFS) |
| `centrifuge` | micro-centrifuge (story: RFS4) |

All have the native pickup and carrying animations from the story.

### A job: example (loader)

A repeatable job entirely in Lua: take it from the foreman, carry crates from the warehouse to a truck ($5 per crate), turn in the job. The ready-made mode is `modes/loader` (run: `dgmp-server --mode loader`). Places are set in the editor: logic points `loader_boss`, `loader_pickup`, `loader_depot`; crates can lie in `world.json` or be created by code.

The cycle:
1. **Take the job.** An NPC foreman (`createActor`) and an interaction point next to it (`createInteraction`). On F — the "Take job" dialog. The mode puts on a uniform (`setUniform`), places the warehouse and truck markers for this player only (`createMarker{player = ...}`) and an on-screen line (`player:hud`).
2. **Work.** Shared crates tagged `"loader"` lie at the warehouse. Only someone on the job can pick them up: `carryPick` returns `false` to the others. A crate put down at the truck (`carryDrop`, `reason == "put"`, within the hand-in point's radius) is removed by the mode (`removeCarry`), counted, and a new one is placed at the warehouse. One broken by a roll (`roll = "break"`) is also replaced with a new one.
3. **Turn in the job.** F at the foreman again — the "Turn in" dialog: payout (`giveMoney`), the uniform is taken off (`setUniform(nil)`), the markers and the on-screen line are removed. Left the server — the shift ends without pay, and the crate in hands returns to the warehouse.

```lua
local logic = require("logic")
local boss, pickup, depot = logic.point("loader_boss"), logic.point("loader_pickup"), logic.point("loader_depot")
local PAY, shifts = 5, {}

local function newCrate()
    local a = math.random() * 6.28
    createCarry({ model = "crate", x = pickup.x + math.cos(a) * 150, y = pickup.y + math.sin(a) * 150, z = pickup.z,
                  tag = "loader", roll = "break" })
end
on("start", function() for i = 1, 4 do newCrate() end end)

local function startShift(p)
    local s = { delivered = 0, earned = 0 }
    s.markers = { createMarker({ x = depot.x, y = depot.y, z = depot.z, radius = 4, category = "task", route = true, player = p, track = false, title = "Truck" }) }
    shifts[p.id] = s
    p:setUniform("m_mil_diamondlake_worker_001")
    p:hud("job", { text = "Loader: 0 · $0", x = 0.985, y = 0.11, align = "right" })
end
local function endShift(p, pay)
    local s = shifts[p.id]; if not s then return end
    shifts[p.id] = nil
    for _, m in ipairs(s.markers) do removeMarker(m) end
    if p.carrying then setCarry(p.carrying, { x = pickup.x, y = pickup.y, z = pickup.z }) end
    p:hud("job"); p:setUniform(nil)
    if pay then p:giveMoney(s.earned) end
end

createActor({ x = boss.x, y = boss.y, z = boss.z, yaw = boss.yaw, look = "m_mil_diamondlake_worker_003", name = "Foreman" })
createInteraction({ x = boss.x, y = boss.y, z = boss.z, radius = 2.5, text = "Foreman", onUse = function(p)
    local s = shifts[p.id]
    p:dialog({ title = s and "Turn in job" or "Loader job",
               text = s and ("Payout: $" .. s.earned) or ("$" .. PAY .. " per crate"),
               button1 = s and "Turn in" or "Take job", button2 = "No",
               onResponse = function(p, ok) if ok then if s then endShift(p, true) else startShift(p) end end end })
end })

on("carryPick", function(p, id) if getCarry(id).tag == "loader" and not shifts[p.id] then return false end end)
on("carryDrop", function(p, id, info)
    local s = shifts[p.id]
    if info.reason == "put" and s and (info.x - depot.x) ^ 2 + (info.y - depot.y) ^ 2 < 400 ^ 2 then
        removeCarry(id); newCrate()
        s.delivered, s.earned = s.delivered + 1, s.earned + PAY
        p:hud("job", { text = "Loader: " .. s.delivered .. " · $" .. s.earned, x = 0.985, y = 0.11, align = "right" })
    elseif info.reason == "break" then newCrate() end
end)
on("playerQuit", function(p) endShift(p, false) end)
```

Other jobs are built the same way: miner, weeding, delivery — only what counts (a placed crate, a `gameEvent`, a `q:hold` zone) and what pays changes.

**Without code.** The same job can be assembled in the quest editor: a `quests/<id>.json` file with `"kind": "job"` — an NPC, a uniform, pay per cycle and steps that run in a loop, including the `deliver` step (carry crates). The format is [`docs/quests.md`](quests.md), "Job". A ready-made file is `quests/loader_job.json` (loader, like `modes/loader`). In a mode, jobs are started through `require("jobs")` or `Jobs=all` in `meta.ini` (see "Quests — the `quest` library").

The mode's crates are removed when it is reloaded (crates from `world.json` are placed again at their spots). Example — the [`loader`](../modes/loader/main.lua) mode (the whole job).

## Quests — the `quest` library

```lua
local Quest = require("quest")
local q = Quest.start(player, { title = "Looter", category = "task", text = "Collect supplies in the area." })
q:step("Get to the gas station", { x = .., y = .., z = .., radius = 6,
    onEnter = function(q, player)
        q:hold("Search the area", { x = .., y = .., z = .., radius = 8, seconds = 10, meter = "Search",
            onDone = function(q) q:collect("Search the cars", { event = "search", count = 2,
                onDone = function(q) q:complete(150) end }) end })
    end })
```

| Step | Description |
|---|---|
| `q:step(text, target)` | reach a point: a quest marker with a route (`route = false` — without), `onEnter(quest, player)` |
| `q:hold(text, {x, y, z, radius, seconds, meter, leaveTip, onDone})` | stay in an area: the meter grows only inside, on leaving — a "go back" tip |
| `q:collect(text, spec)` | do something `count` times, the objective line has a "(1/3)" counter |
| `q:talk(text, spec)` | conversation: approach an NPC (`actor = "id"` from the editor) or a point (`x, y, z`), the `lines` are shown in dialog windows one after another, `speaker` — the title; `onDone(quest, player)` |
| `q:deliver(text, spec)` | carry crates: put `count` crates down in the unloading area, the objective line has a "(1/3)" counter (below) |
| `q:progress(title, percent)` | your own meter |
| `q:complete([xp])`, `q:fail()` | finish; the panel is removed after 6 s (after the reward screen is closed) |

`spec` fields of `deliver`: `pickup = {x, y, z, radius = 3, boxes = 4, model = "crate", roll = "keep", tag, name}` — loading (with `tag`, the world's crates with that tag are used, otherwise the step places its own `boxes` around the point), `dropoff = {x, y, z, radius = 4, name}` — unloading, `count = 1`, `route = true` (a route to the unloading), `onBox(quest, player, done)`, `onDone(quest, player)`. A crate counts when the player puts it down (`carryDrop`, `reason = "put"`) within the unloading radius; it returns to the loading point. Only this player can pick up the step's own crates, and they are removed when the step ends. Shared crates for several players — `Quest.pool(pickup, allowed(player), denyTip)`, passed as `spec.pool`.

`spec` fields of `collect`: `event` (an event type or a list of types), `item` (a key or a list), `class` (part of a class name), `ids` (a list of specific world objects), `area = {x, y, z, radius}` (counts only there: "blow up the nest", "burn the papers"), `marker = true` (a marker above the area, `route = true` — with a route), `spawn = {{item, count, x, y, z}, ...}` (the step lays out its own items with markers above them; only they count; those not picked up disappear when the step ends), `count`, `onDone(quest, player)`. The same object counts once.

Quests and jobs from files (`quests/<id>.json`, format — [`docs/quests.md`](quests.md)):

| | |
|---|---|
| `Quest.load(id)`, `Quest.list()` | a quest file (table) / list of ids |
| `Quest.run(player, def, onDone)` | a quest for a player: steps in order, reward, completion screen |
| `Quest.runJob(def or id)` → `job` | a job (`"kind": "job"`) in the mode's world: an NPC with [F] "Take job" / "Turn in job", the crates of `deliver` steps; `job:stop()` removes it. A repeated call with the same id replaces the job |
| `require("jobs")` | all jobs from `quests/` (`modes/_lib/jobs.lua`); `.running[id]` — the running ones |
| `Quest.jobs()`, `Quest.shift(player)` | running jobs by id; which job the player is on: `id, {cycles, earned, finished}` or `nil` |
| `quests.watch()` | from now on a change to `quests/*.json` reloads the mode (called by `runJob` and `jobs`) |

Without code, jobs are started by the mode's `meta.ini`: `Jobs=all` (or a comma-separated list of ids: `Jobs=loader_job,miner`).

## Persistent storage — `storage`

The mode's data survives script reloads and server restarts. Stored in `data/<mode>/storage.json` and written to disk at least once every 5 seconds and when the mode is unloaded.

| Function | Description |
|---|---|
| `storage.get(key)` | the value or `nil` |
| `storage.set(key, value)` | save: numbers, strings, bool and nested tables; `nil` deletes |
| `storage.keys()` | an array of keys |
| `storage.save()` | write to disk immediately |

An example of RP server accounts:
```lua
command("register", function(player, args, password)
    if storage.get("acc:" .. player.name) then return player:message("The account already exists.") end
    storage.set("acc:" .. player.name, { pass = passwordHash(password or ""), money = 100 })
    player:message("Account created.")
end)
command("login", function(player, args, password)
    local acc = storage.get("acc:" .. player.name)
    if not acc or not passwordCheck(password or "", acc.pass) then return player:message("Wrong password.") end
    player.data.logged = true
end)
```

With Steam verification (`SteamApiKey`) the nickname is bound to the Steam account, and a password is needed only if you want separate character accounts. Without it nicknames are not protected — implement sign-in (`/login`) in the mode itself, as in the example above.

## HTTP and JSON

Requests to a website, an API, Discord webhooks. They run in the background, and the response arrives in your function on the next server tick. Up to 16 requests at a time, body up to 1 MB, default timeout 10 s. Turned off in `server.ini`: `Http=0`.

```lua
http.get("https://example.com/api/status", function(ok, status, body, err)
    if ok then local data = json.decode(body) end
end)

-- A message to a Discord channel through a webhook (the table is sent as JSON)
http.post("https://discord.com/api/webhooks/<id>/<token>", { content = player.name .. " joined the server" })

http.request({ method = "PUT", url = "https://...", body = json.encode(t), type = "application/json", timeout = 20 },
             function(ok, status, body, err) end)
```

`ok` — a 2xx response; `status` — the HTTP code (0 — no response); `err` — what went wrong. On an error `json.decode` returns `nil, "invalid JSON"`.

## RP chat — the `rpchat` library

Chat like on SA-MP RP servers: a normal message is heard only nearby, and the farther away, the paler it is; players in different virtual worlds do not hear each other.

```lua
local rpchat = require("rpchat")
rpchat.install()                                     -- radius 20 m, /o enabled
rpchat.install({ radius = 15, ooc = false, names = "underscores", adminSees = true })
rpchat.near(player, "* The radio hisses", "#C2A2DA", 10)  -- your own message by the same rules
```

| Command | |
|---|---|
| plain text | "Mark Twain says: …" within the radius, the color fades with distance |
| `/me action` | `* Mark Twain takes out a radio` |
| `/do description` | `* The door is locked (( Mark Twain ))` |
| `/try action` | `* Mark Twain open the door \| success` — random, 50/50 |
| `/b text` | local OOC `(( Mark Twain: … ))` |
| `/s text` | a shout — twice as far |
| `/w text` | a whisper — 2 m |
| `/pm id or nickname text` | a private message |
| `/o text` | global OOC for everyone (`ooc = false` — turned off) |

`names = "spaces"` (default) shows `Mark_Twain` as `Mark Twain`. `adminSees = true` — admins also see distant local messages (in gray, marked `[far]`). The "says", "shouts" etc. texts are in the listener's language (`player.lang`); your own — `prefix = { says = { en = "...", ru = "..." } }`.

## Steam, admins and bans

The server learns the player's Steam account: the client sends the SteamID and a one-time Steam ticket, and the server verifies it through the Steam Web API (`ISteamUserAuth/AuthenticateUserTicket`). Someone else's SteamID cannot be forged.

- A Web API key is free at https://steamcommunity.com/dev/apikey; put it in `server.ini` → `SteamApiKey`. Without a key, accounts are not verified: `player.steamId` is `nil` for everyone.
- `SteamAuth=optional` (default) — verification if there is a key; players without Steam, or during a Steam outage, join unverified. `required` — verified only. `off` — as before, nicknames only.
- **The nickname is bound to the account** (`ReserveNicks=1`): the first player to join under a nickname with verified Steam owns it (`data/names.json`); nobody else is given that nickname, and neither are unverified players. So storing data by nickname is safe, but by `player.steamId` is more reliable.
- **Admins** — SteamIDs separated by commas in `Admins`. Nicknames there work only with `SteamAuth=off`. A player who joins from this same computer (`127.0.0.1`) is an admin (`LocalAdmin=1`). The `RconPassword` password grants rights for the session: `/rcon login <password>` in chat (3 wrong attempts — an IP ban for 10 minutes), then `/rcon <console command>` — the reply arrives in chat (`/rcon status`, `/rcon ban 3 60 cheats`).
- **Bans** — `data/bans.json`: by SteamID, IP or nickname, for a period or permanently. A ban of a player with verified Steam does not touch their IP (many people can share one address: NAT, mobile internet); without Steam the IP is banned too. Checked on join, expired ones are removed automatically. If a player plays from someone else's copy (Family Sharing), a ban on the copy's owner also applies.

```lua
command("ban", function(admin, args, who)          -- /ban Raud 60 cheats
    if not admin.admin then return admin:message("No permission") end
    local target, rest = getPlayer(who), args:match("^%S+%s*(.*)$")
    local minutes, reason = rest:match("^(%d+)%s*(.*)$")
    if target then target:ban(reason or rest, tonumber(minutes) or 0) end
end)

on("playerConnecting", function(info)               -- whitelist by SteamID
    if not info.verified then return "Sign in through Steam" end
    if not storage.get("whitelist:" .. info.steamId) then return "Submit an application on Discord" end
end)
```

## Anti-cheat

The server does not trust what the client sends, wherever that can be checked.

**Hits.** Damage to a player is sent by the shooter's client. The server discards the hit and gives a strike if: the damage is greater than `MaxHitDamage`; farther than 400 m; a melee hit is farther than `MeleeRange` (adjusted for speed and ping); the shooter is dead or spectating; there are more hits per second than `MaxHitsPerSecond`.

**Protected items.** The mode declares keys (prefixes) that only the server hands out:

```lua
protectItems({ "kWeaponID_", "kAmmoID_" })   -- weapons and ammo — only from the mode's shop
```

The server keeps count of how much of what it has given each player (`giveItem`, `giveWeapon`, `setAmmo`, `restoreInventory`, `spawnItem` items) and checks every inventory snapshot against it. The excess is taken away, the mode hears `inventoryViolation`, and without an explanation — a strike. What is allowed automatically:
- Deacon's knife, rock and stun gun (never protected);
- spending: used up ammo, threw away a weapon — the count decreases;
- a weapon that another player dropped or handed over: whoever picks it up in the same place (within 30 minutes) is credited with it — this is how hand-to-hand transfer works;
- `spawnItem` items — to whoever picked them up, once.

The game's world loot (ammo crates, weapons on the map) was not handed out by the server: the mode decides —

```lua
on("inventoryViolation", function(p, v)
    if v.pickedUp and v.item:find("^kAmmoID_") then return true end   -- ammo picked up in the world is allowed
end)
```

Crafted items (a Molotov cocktail is `kWeaponID_Molotov_*`) were not handed out by the server either: do not protect such keys, or allow them in the handler.

**Strikes.** Every violation is a strike and a `cheat` event (`kind`: `hitDamage`, `hitRange`, `hitRate`, `hitWhileDead`, `hitFromHelicopter`, `item`). `CheatKick` strikes per minute (default 10) — a kick. `return true` in `cheat` — the mode decides itself (for example, a ban or just a log). Everything is written to the server log marked `[anticheat]`.

```lua
on("cheat", function(p, kind, info)
    if kind == "hitDamage" then p:ban("damage hack: " .. info.detail) return true end   -- permanently
end)
```

## Item window

A ready-made game window with one or two item lists (backpack | trunk, backpack | stash): icons (the mode's images), quantity, a description line, action buttons for the selected item, transfer between lists (with the "Move" button, "1 only" or a double click). What any click means is decided by the server: the window only shows and reports. It is usually opened by the `inventory` library (below), but it can also be used directly.

```lua
player:itemWindow({
    title = "Backpack and trunk", hint = "Double-click to move",
    panes = {
        { id = "bag", title = "Backpack", info = "12.5 / 30 kg", empty = "Empty",
          items = { { id = "gas", name = "Gas can", count = 2, icon = "gas.png", text = "10 L", actions = { { id = "refuel", label = "Refuel" } } } } },
        { id = "trunk", title = "Trunk", items = {} },
    },
}, function(player, action)
    -- action: {type = "move", pane = 1, item = 1, count = 2} | {type = "action", pane =, item =, id = "refuel"} | {type = "close"}
end)
player:itemWindow({...})      -- while the window is open: update in place (the selection stays)
player:closeItemWindow()
on("itemWindow", function(player, windowId, action) end)   -- the same for all windows
```

The window holds the cursor, like a dialog. Up to 64 KB of description and 20 actions per second from a player.

## The mode's own inventory — the `inventory` library

`modes/_lib/inventory.lua`: the mode's items (a gas can, radio, key, mine), containers limited by weight and slots — the player's backpack, a vehicle trunk, a stash — and the item window for them. The server decides everything; the game's backpack (ammo, weapons — `player:giveItem`) is separate.

```lua
local inventory = require("inventory")
inventory.define("gas", { name = { en = "Gas can", ru = "Канистра" }, icon = "gas.png", weight = 5, stack = 1,
    text = { en = "Fills a tank by 40 %", ru = "Заправляет бак на 40 %" },
    actions = { { id = "refuel", label = { en = "Refuel", ru = "Заправить" },
                  when = function(player) return nearVehicle(player) ~= nil end,   -- the button is visible only near a vehicle
                  fn = function(player, ctx) setVehicle(id, { fuel = 1 }) return true end } } })   -- true: one is used up

local bag = inventory.of(player)              -- the player's backpack: by SteamID (without it — by nickname), persisted
bag:add("gas", 1)                             -- how many fit (weight, slots)
bag:remove("gas", 1); bag:count("gas"); bag:has("gas"); bag:weight(); bag:list()
local trunk = inventory.trunk(vehicleId, { key = "bob_car", maxWeight = 80 })   -- key: also keep it after a restart
local stash = inventory.container("house:12", { title = "Stash", maxWeight = 100, slots = 30, persist = true })
inventory.open(player, bag, trunk, { title = "Backpack and trunk" })   -- one or two storages
inventory.close(player)
inventory.onMove = function(player, item, count, from, to) return false end   -- forbid the transfer
inventory.bag = { maxWeight = 30, slots = 24 }   -- backpack size (before the first inventory.of)
```

An item has: `name`, `icon` (an image from `images/`), `weight` (kg), `stack` (how many in one slot), `text` (a string or `function(item, player)`), `actions` (`id`, `label`, `when`, `fn`). Names and labels are a string or `{en =, ru =}` (by the player's language). An item with data (`bag:add("radio", 1, {freq = 101})`) takes its own slot. Storages with `persist = true` are kept in `storage` under the key `inv:<id>`.

## Traps

The game's mine, IED and bear trap, a tripwire between two points (with an IED at one end) or an invisible zone. When a trap triggers is decided by the **server**: a player (or zombie) within the radius of a point trap, or one who crossed a tripwire at body height. Then `onTrigger` and the `trap` events are called (`false` — nothing happens), and the trap triggers for everyone: explosives explode in the game's way (each game hurts its own player and its own zombies nearby), a bear trap catches the victim on their screen (stuck, struggle free, bleeding) and snaps shut for the others. The game's own triggers and picking a trap up into the backpack are disabled.

```lua
createTrap({ type = "mine", x = x, y = y, z = z })                -- z — ground; radius 1.5 m
createTrap({ type = "beartrap", x = x, y = y, z = z, yaw = 90 })   -- 0.6 m
createTrap({ type = "ied", x = x, y = y, z = z, radius = 3 })
createTrap({ type = "wire", x = x1, y = y1, z = z1 + 30, x2 = x2, y2 = y2, z2 = z2 + 30 })   -- z: height of the wire
createTrap({ type = "wire", explosive = false, x = ..., z = z + 150, ..., onTrigger = function(trap, player, zombieId)
    if not player then return false end            -- zombies pass
    player:damage(25)                                -- a "clothesline" at a motorcyclist's neck
    if player:isOnBike() then player:playAnim("dcn_trap_tripwire_clothsline_bike_long") end
end })
createTrap({ type = "zone", x = x, y = y, z = z, radius = 5, rearm = 10, onTrigger = function(trap, player) ... end })  -- your own mechanic
removeTrap(id); getTrap(id); traps(); fireTrap(id [, player])     -- detonate now
explode(x, y, z [, world])                                         -- an IED explosion at a point
on("trap", function(trap, player, zombieId) end)                   -- return false: do not trigger
```

Fields: `world` (0), `players = true`, `zombies = true` (who sets it off), `rearm` (seconds until ready again; without it the trap is one-shot), `radius` (m), `lethal = true` and `bleed = 20` (bear trap: the caught player bleeds out and dies after `bleed` seconds unless they break free; the game's own bleeding stops at about a quarter of health; `false` — it only holds and hurts), `explosive = true` (tripwire: an IED at the end). A new trap is ready after a second (not under the placer's feet). A tripwire counts as triggered when the movement segment crosses the wire, if it is at a height from the ankles to the motorcyclist's head (−20 … +220 cm from the feet); point traps — by distance to the center. Clients show traps within a 300 m radius.

- **Bear trap.** The caught player gets stuck, breaks free by repeatedly pressing the key the game shows (E), and loses blood. While the trap holds them (the player stands in its jaws), interactions, carrying and the mode's keys do not work — the keys stay with the game.
- **Tripwire.** A thin wire on two stakes, the IED lies on the ground under the first end. After it triggers the wire disappears immediately, the stakes after 7 seconds; the server forgets the trap after 10.

**Aimed placement** is done in a client script (`modes/inventory/client/main.lua`): a ghost of the trap (`createObject{..., ghost = true}`, outlined) stands where the crosshair points; F places it, G cancels. The point is no farther than 4 m from the player (and not underfoot: no closer than 1.5 m). A tripwire works as in the game: the first end is placed (`send("wireStart", ...)`, the server remembers it at that moment), the player walks, stretching the wire (a line with its length is visible), and places the second end also within 4 m of themselves; the length is from 1 to 10 m (`send("placeAt", ...)`). The server checks everything itself and only then places the trap and takes the item from the backpack. The placer can take their own untriggered trap back — "[F] Pick up trap" next to it. `/place mine|beartrap|wire` — the same without an item (for testing).

## Zombies

The game's freakers in multiplayer (experimental, `server.ini` `Zombies=1`). The zombie AI runs in the game of the player closest to it; the others see a copy of it. Details — [`docs/zombies.md`](zombies.md).

- **Zombie zones** are placed in the editor ("World" → "PvP and zones" → "+ Zombies", lime-green posts) or in code. While a player is nearby, the zone keeps the set number of zombies; a killed one returns after `respawn` seconds; with no players nearby for 30 s the zone goes to sleep.
- **Types** — `Swarmer`, `Newt`, `Breaker`, `Screamer`, ...: any type the region lists, even if the game has not loaded it there (it loads on the first spawn).

```lua
createZombieZone({ name = "camp", x = -82000, y = -301000, z = -5250, radius = 30, types = { "Swarmer", "Newt" }, max = 6, respawn = 60 })
setZombieZone("camp", { enabled = false })          -- the zone's zombies disappear
local id = createZombie(x, y, z, "Breaker")         -- a single zombie
local horde = createHorde(x, y, z, 12, { radius = 10 })  -- a horde
on("zombieDeath", function(zombie, killer) if killer then killer:giveMoney(10) end end)  -- zombie.zone / zombie.horde
on("hordeDeath", function(id) broadcast("Horde destroyed") end)
```

## Server scripts

Like filterscripts in SA-MP: Lua files `scripts/<name>.lua` in the server folder that work in any mode — shared admin commands, logs, anti-cheat rules. In `server.ini`: `Scripts=discord_log,admin` (comma-separated). They run in the same Lua state as the mode, before its `main.lua`: the whole API is available to them, and their `on(...)` handlers and commands work together with the mode's handlers. An error in a script skips only that script. A change to a file in `scripts/` reloads the mode, as does an edit to the mode itself.

Example — [`scripts/discord_log.lua`](../scripts/discord_log.lua): joins, leaves, kills and anti-cheat triggers go to a Discord channel through a webhook (the webhook address is put at the top of the file).

## Sandbox and limits

- Available: `string`, `table`, `math`, `utf8`, `coroutine`, `os.time` / `os.clock` / `os.date`.
- No `io`, `os.execute`, native modules or bytecode (`load` accepts text only).
- A single script call may run for no more than **3 seconds**. Otherwise it is aborted with an "infinite loop?" error, and the server keeps running.

## Server console

`help`, `status` (for each player — IP, SteamID: with a `?` sign — unverified, admin), `say <text>`, `kick <id or nickname> [reason]`, `ban <id|nickname|SteamID|IP> [minutes] [reason]`, `unban <SteamID|IP|nickname>`, `bans`, `admin <id|nickname> [off]`, `hitstats` (the largest single-hit damage for each weapon since launch — for choosing `MaxHitDamage`), `voice` (voice chat: who is speaking, channels, sent and dropped), `bot [N]` / `bot off`, `reload`, `mode <id>`, `time <hours>`, `weather <id or auto>`, `quit`. Other lines go to the mode's `console` event. For example, in freeroam: `give`, `weapon`, `disarm`, `bike`, `nobike`, `inv`, `hint`, `tip`, `nametags`.

## `server.ini`

| Key | Value |
|---|---|
| `Name` | name in the server list |
| `Mode` | mode id (a folder in `modes/`) |
| `Port` | UDP port, default 7777 (7777–7786 for LAN discovery) |
| `MaxPlayers` | up to 64 |
| `Password` | join password (empty — no password) |
| `Admins` | administrators' SteamIDs separated by commas (nicknames — only with `SteamAuth=off`) |
| `SteamAuth` | `optional` (default), `required`, `off` — verification of Steam accounts |
| `SteamApiKey` | Steam Web API key for verification |
| `ReserveNicks` | `1` — a nickname is bound to a Steam account |
| `LocalAdmin` | `1` — a player who joins from this computer is an admin |
| `RconPassword` | password for `/rcon login` (empty — RCON is off) |
| `Http` | `1` (default) — `http.request` is available to modes; `0` — not |
| `Scripts` | server scripts (`scripts/<name>.lua`) separated by commas, see "Server scripts" |
| `MaxHitDamage`, `MeleeRange`, `MaxHitsPerSecond`, `CheatKick` | anti-cheat: single-hit damage, melee range (m), hits per second, strikes per minute before a kick (0 — do not kick) |
| `Nametags` | `visible`, `walls` or `hidden` |
| `Voice` | `1` (default) — voice chat on; `0` — off. On the fly: `setVoiceEnabled`, console `voice on|off` |
| `VoiceRange` | how many meters away nearby voice is heard (25) |
| `VoiceRate`, `VoiceListeners`, `VoiceBandwidth` | voice: bytes per second from one player (8000), how many speakers a player hears at once (8), bytes per second of voice from the whole server (0 — unlimited) |
| `SpawnX/Y/Z/Yaw` | spawn point, in Unreal units |
| `Bind` | IP to listen on (all interfaces by default) |
| `Master` | the master server on which the server is visible in the "Internet" tab (`host` or `host:port`); empty — not announced. See [`docs/master.md`](master.md) |

The command-line keys `--name`, `--mode`, `--port`, `--max`, `--password`, `--bind`, `--master-server`, `--config` override the file.
