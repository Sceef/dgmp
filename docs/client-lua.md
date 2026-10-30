**English** · [Русский](client-lua.ru.md)

# Client scripts (client-side Lua)

The part of a mode that runs **in the game of each player**, not on the server. Like client scripts in FiveM or the client side of MTA. The server distributes the files and the game runs them in a sandbox.

Why: everything that has to react **instantly**, without a round trip to the server:
- your own HUD, drawn every frame (speedometer, compass, tags above players, crosshair);
- keys (press — instant result);
- local chat commands (`/fps`, `/hud`);
- rays from the camera ("who am I looking at");
- mini-games and HTML interface logic without the server.

The server still decides everything important: money, items, damage. A client script only shows and asks.

Example — the [`clientdemo`](../modes/clientdemo) mode: `dgmp-server.exe --mode clientdemo`.
- A speedometer and a compass over the game.
- The name of the player under the crosshair; G — wave at them (a request to the server, the server checks the distance).
- `/fps` and `/ping` — local commands.

## Files

`modes/<mode>/client/` (subfolders allowed):
- what gets downloaded: `.lua`, `.json`, `.txt`, up to 1 MB per file;
- the entry point is `client/main.lua`;
- `require("hud")` loads `client/hud.lua`, `require("ui.speedo")` — `client/ui/speedo.lua`; each module runs once.

How it works:
- **Distribution.** The game downloads the files on join and caches them by content (`cache/scripts/<sha256>`), like the mode's images and HTML interface.
- **Hot reload.** When a mode file changes on disk, the server reloads the mode. The scripts on players' machines stop and start again, and only the changed files are downloaded.
- **Errors.** A script error is written:
  - to the game log `logs/DGMP.log` — with a call stack;
  - once to the player's chat (a red `[client] ...` line);
  - to the server console: `[mode] client script error (Nick): ...`.

## Communication with the server

Client:
```lua
send("buy", { item = "kAmmoID_Pistol", count = 2 })   -- to the server; true if it was sent (no more than 60 per second)
onServer("price", function(data) ... end)            -- player:clientSend("price", data) on the server
```

Server (`main.lua`):
```lua
on("clientMessage", function(player, name, data)
    if name == "buy" then ... end                    -- check data: the player can send anything
end)
player:clientSend("price", { item = "...", cost = 120 })
```

Rules:
- **Messages** — any JSON values. The name is up to 64 characters; data: to the client up to 64 KB, to the server up to 16 KB. The server accepts no more than 60 messages per second from a player.
- **Order.** Server messages that arrive before the scripts start will wait for it.

## Events and timers

| Function | What it does |
|---|---|
| `on("frame", fn(dt))` | every frame (`dt` — seconds since the previous frame). Draw here |
| `on("stop", fn())` | the scripts are stopping: the mode was reloaded or the player disconnected |
| `bindKey("G", fn(key) [, {block = true}])` / `unbindKey("G")` | a key press while the player is playing: not in chat, not in a dialog, not in a menu, not dead, the mode's page not focused. `block = true` — the game does not receive this press (in chat and dialogs the key works as usual; mouse buttons are not taken) |
| `isKeyDown("SHIFT")` | the key is held right now (when the game window is active) |
| `command("fps", fn(args, ...))` | the chat command `/fps`: runs in place, does not go to the server. The words are passed after the `args` string, as on the server |
| `after(ms, fn)` / `every(ms, fn)` → id; `cancelTimer(id)` | timers |
| `now()` | seconds (monotonic clock) |

**Keys:**
- `A`–`Z`, `0`–`9`, `F1`–`F12`;
- `SPACE ENTER TAB SHIFT CTRL ALT BACKSPACE`;
- `UP DOWN LEFT RIGHT INSERT DELETE HOME END PAGEUP PAGEDOWN`;
- `MOUSE1`–`MOUSE5`, `NUM0`–`NUM9`.

Without `block` the game also receives the press: pick keys the game does not use (B, for example, is the binoculars), or take them away from the game.

## What a script knows

| Function | Returns |
|---|---|
| `me()` | `{id, name, x, y, z, yaw, speed, health, dead, onBike, world, lang}`. `speed` — units (cm) per second, `health` — percent, `lang` — `"ru"` / `"en"` |
| `players()` | other players visible in this game (the same virtual world, not spectators): `{{id, name, x, y, z, yaw, health, onBike, dead}, ...}` |
| `camera()` | `{x, y, z, pitch, yaw, fx, fy, fz}`, where `fx, fy, fz` is the view direction of length 1 |
| `screen()` | `w, h`: the frame size in pixels |
| `worldToScreen(x, y, z)` | `sx, sy` in pixels, or `nil` if the point is behind the camera |
| `raycast(x1, y1, z1, x2, y2, z2)` | the first obstacle on the segment: `{x, y, z, distance, class, player}` or `nil`. Other players' bodies are tested as capsules: on a hit `player` = their id, `class = "player"` |

Coordinates are the same units as on the server: 1 = 1 cm.

A ray from the crosshair:
```lua
local c = camera()
local hit = raycast(c.x, c.y, c.z, c.x + c.fx * 3000, c.y + c.fy * 3000, c.z + c.fz * 3000)  -- 30 m
if hit and hit.player then ... end
```

## Drawing — `draw.*`

Call in `on("frame")`. What is drawn is shown in this frame and disappears in the next: draw anew every frame (immediate mode). In menus, on the map and on the loading screen drawings are hidden. Coordinates are pixels from the top-left corner (`screen()` gives the size).

```lua
draw.text(x, y, "text", { size = 16, color = "#FFE08A", align = "left|center|right", shadow = true })
draw.text3d(x, y, z, "above a world point", { distance = 5000 })   -- centered above the point; true if visible
draw.rect(x, y, w, h, "#00000088", { fill = true, rounding = 6, thickness = 1 })
draw.line(x1, y1, x2, y2, "#FFFFFF", 2)
draw.circle(x, y, radius, "#FF0000", { fill = false, thickness = 2 })
draw.image("logo.png", x, y, w, h)   -- an image from modes/<mode>/images
```

**Colors.** A string `"#RRGGBB"` or `"#RRGGBBAA"` (with transparency). A number `0xRRGGBB`, or `0xRRGGBBAA` if the number is greater than `0xFFFFFF`. Transparent black cannot be written as a number (`0x00000088` is read as blue `0x000088`) — use a string.

The limit is 5000 shapes per frame. Cyrillic in text is supported.

## Local actions

| Function | What it does |
|---|---|
| `chat(text [, color])` | a chat line for this player only |
| `hint(title, text [, seconds])` | a game hint (like `player:hint`) |
| `playSound(name, {x=, y=, z=, volume=1, pitch=1})` | a sound for you only. A name from `data/sounds.txt` or a `/Game/...` path; without `x, y, z` — no position |
| `uiSend(name, data)` | a message to the mode's HTML page: `dgmp.on(name, fn)` |
| `onUi(name, fn(data))` | a page message `dgmp.send(name, data)` goes to the client script, not the server |

With `onUi` the interface logic can live in the game. The page sends `dgmp.send("tab", "map")` and the client script answers it by itself: no server, no delay. Messages without their own `onUi` still go to the server (`on("uiMessage")`).

### Your own objects and blips

Only for this player (others do not see them): a furniture preview before buying, "my goals" marks, highlighting
a place. Objects and blips shared by everyone are still created by the server (`createObject`, `createBlip` in `main.lua`).

| Function | |
|---|---|
| `createObject{model= or asset=, x, y, z, pitch, yaw, roll, scale = 1, collision = true, ghost = false}` → id | `model` — a mesh from `data/props.txt`, `asset` — a `/Game/...` path; up to 500. `ghost = true` — a "ghost": the game's outline (like survival instinct), no collisions — a "will stand here" preview |
| `setObject(id, {fields})` | instantly in place (movement stops) |
| `moveObject(id, {x, y, z, pitch, yaw, roll, time = seconds or speed = m/s})` → seconds | smoothly there |
| `removeObject(id)` | |
| `createBlip{x, y, z, icon = "MAP_...", color, scale = 1, map = true, radar = true, edge = false}` → id | a blip on the map and radar, up to 200 |
| `setBlip(id, {fields})`, `removeBlip(id)` | |

When the scripts stop, their objects and blips disappear.

### Storage — `storage.*`

Script data on the player's computer, separate for each server and mode (`cache/scriptdata/`): interface
settings, window positions, "the hint was already shown". Up to 256 KB. Saved automatically (every 5 s and on stop).

```lua
local runs = (storage.get("runs") or 0) + 1
storage.set("runs", runs)        -- any JSON values; nil deletes
for _, key in ipairs(storage.keys()) do print(key) end
```

This is no place for money and items: the player can rewrite the file. Keep important data on the server (`storage` in `main.lua`).

Also available: `json.encode` / `json.decode`, `print` / `log` (to `logs/DGMP.log` with a `[client]` prefix), the standard `string`, `table`, `math`, `utf8`, `coroutine`, `os.time` / `os.clock` / `os.date`.

## Sandbox and limits

Code from the server runs on the player's computer, therefore:
- **No system access:** no `io`, `os.execute` / `getenv` / `remove`, `package`, `debug`, `dofile` / `loadfile`.
- **No bytecode:** `load` accepts text only, `string.dump` is removed.
- **Time.** One call — a frame, a key, a message, a timer — runs no longer than 100 ms; the first run of `main.lua` — up to 2 s. Longer — a "ran longer than…" error. An `on(...)` handler or a repeating timer that exceeded the time is disabled: on every frame it would freeze the game again.
- **Memory.** Up to 64 MB for all the mode's scripts.
- **The client cannot be trusted.** The player can modify their game and send anything. Everything that gives an advantage (money, items, damage, teleport) is checked by the server itself: distances, prices, permissions. A client script only shows and asks.

## What is not there yet

- **NPCs and objective markers for a single player only.** They are created by the server (`createActor`, `createMarker`).
- **Movement and camera locks** (only individual keys, `bindKey(..., {block = true})`).
