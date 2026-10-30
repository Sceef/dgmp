**English** · [Русский](README.ru.md)

# DGMP — multiplayer for Days Gone

DGMP is a mod that adds multiplayer to Days Gone: a dedicated server and game modes written in Lua, in the spirit of SA-MP, MTA and FiveM.

What it is made of:

- **`DGMP.exe`** — the launcher: your nickname, the server browser (favorites, recent, LAN, Internet, connect by IP), hosting your own server with a live console, the editor, and updates.
- **`dgmp-server.exe`** — the dedicated server (UDP, port 7777). It does not need the game and runs on Windows and Linux, including a VPS.
- **The client** — `version.dll`. The launcher puts it next to a separate copy of the game's EXE, so the original game and its saves are left untouched.
- **Modes** — Lua scripts in `modes/<id>/`. A mode runs on the server and, if it wants, on the players' machines too (client scripts, HTML interface).
- **The editor** — an in-game editor of a mode's world: props, lights, NPCs, vehicles, doors, loot, logic points, skins, quests.

## Download and play

1. Download `DGMP-client-<version>.zip` from [Releases](https://github.com/Sceef/dgmp/releases/latest).
2. Unpack the `dgmp` folder into the root of your Days Gone installation, next to `BendGame` (not into `Binaries/Win64`):
   ```
   Days Gone/
     BendGame/
     Engine/
     dgmp/DGMP.exe
   ```
3. Start Steam, close Days Gone if it is running, and run `dgmp/DGMP.exe`. Pick a nickname and join a server.

The launcher updates itself: when a new version is out, an **update** button appears in it; it downloads, checks and installs the release and keeps your settings.

## Public test server

**Rubezh | DGMP RP** — `179.66.187.53:7777` (mode `outpost`, a survivor camp RP). A fresh launcher already has it in Favorites. It needs a client of the same network protocol: DGMP 0.8.x (protocol 11).

## What is new in 0.8

Protocol 11: 0.7 clients and servers cannot play with 0.8 ones.

- **The player menu.** Look at a player close by and press **F**: the mode shows its actions (in Rubezh: squad, trade, radio channel, money, medic's help, papers, search, tie up, duel). When the other player has to agree, a question with **Y** / **N** appears at the bottom of their screen; the game does not pause. Libraries `interact`, `squad`, `trade`, `medic`, `instance` are in `modes/_lib/`.
- **Aiming.** With several NPCs, doors, crates or players in range, the "[F] …" prompt goes to the one the camera looks at and that is not behind a wall.
- **Downed instead of dead** (`setDeathMode("downed")`). A player falls when the health bar is empty and lies there until bleeding out: **H** calls for help, holding **E** gives up, a medic revives them from the player menu.
- **Duels and arenas** (`instance`). The fight takes place in a virtual world of its own with its own weapons; afterwards the place, backpack and health come back, nothing is used up.
- **Tab — the player list.** Ping, who is talking, the mode's own columns, a personal mute and volume for each player (the server stops sending a muted voice) and the voice settings. Voice chat can be switched on and off on the fly (`voice on|off`, `setVoiceEnabled`).
- **The map menu (Esc)** shows the number of players, the server name and the mode instead of "days gone". A remote player's map mark shows their name on hover.
- **Chat** takes `{RRGGBB}` colour codes, like dialogs and the HUD.
- **The launcher** shows each server's version (red when the protocol differs) and updates itself from GitHub Releases ([`docs/releases.md`](docs/releases.md)).
- **Rubezh:** salvage and the NERO expedition happen at job sites with a layer of their own for each player or squad; NERO medical supplies are a quest item for the clinic; a tutorial leads from the first delivery to the expedition.
- **API:** `player:ask`, `setPlayerMenu` / `playerInteract`, `player:setControls`, `createBlip({follow = player})`, `setScoreboard`, events `playerDowned`, `playerBleedout`, `playerHelp`, `voiceIgnore`. Everything is in [`docs/API.md`](docs/API.md); what has been checked is in [`VERIFICATION.md`](VERIFICATION.md).

## Requirements

- Days Gone from Steam (only the EXE build with TimeDateStamp `1752517958` is supported) and Steam running.
- Windows 10 1903+ x64 and the WebView2 Runtime (usually installed with Edge).
- To build: Visual Studio 2022 (Community is fine) with the "Desktop development with C++" workload.
- For a Linux server: g++ 10+ or clang 12+ and CMake 3.16+.

## Building from source

The `dgmp/` folder must be in the root of the game, next to `BendGame/`:

```
Days Gone/
  BendGame/
  Engine/
  dgmp/        ← this repository
```

Build everything at once (client DLL, server, launcher, test client):

```bat
build.bat
```

The script calls `vcvars64.bat` from the default VS 2022 Community path; for another edition or path, change the first line of `build.bat`. Third-party libraries (Lua, ENet, Dear ImGui, ImGuizmo) are in `third_party/` and are built once into `build/*.lib`; delete a `.lib` to rebuild it.

Output:

| File | What it is |
|---|---|
| `DGMP.exe` | the launcher |
| `dgmp-server.exe` | the dedicated server (+ `.pdb` for crash stacks) |
| `build/version.dll` | the client; the launcher copies it into `runtime/` at start |
| `build/dgmp-probe.exe` | a test client without the game |

`python tools/package-client.py` builds the client ZIP, `python tools/release.py` publishes a release ([`docs/releases.md`](docs/releases.md)).

## Running the game

1. Start Steam and close Days Gone if it is open.
2. Run `DGMP.exe`, enter a nickname and join a server from the list or by IP.

On the first start the launcher prepares a separate copy of the game in `runtime/`. The content is not copied but linked (directory junctions). A separate profile is made in `profiles/`. The world is loaded from the shared starting save in `worlds/bootstrap/`, so your own progress is neither used nor changed. Do not delete game files through the links inside `runtime/`.

Launcher options:

- `DGMP.exe --editor` — open the editor right away.
- `DGMP.exe --prepare-only` — only prepare `runtime/` and the profile, without starting.
- `DGMP.exe --refresh-seed` — restore the starting save from the template.

In the game the chat opens with **T**; voice chat is **N** (radio — **K**). Keys and volumes are in `DGMP.ini`, section `[Voice]`, or the `/voice` command. **Tab** is the server's player list: ping, who is talking, mute and volume for each player (yours only), voice settings. **F** at another player opens the mode's player menu (if the mode has one); questions of the mode are answered with **Y** / **N**.

### Editor: saving and diagnostics

The editor window has autosave: off, or every **1, 5 or 10 minutes** (1 minute by default). It saves the changed world, skins and the open quest. Files are replaced atomically; the previous version stays as `.bak`, a recovery copy in `.autosave/` next to the document. When another program changed the file, autosave leaves it alone and writes the recovery copy. The status is shown in the editor. To restore, close the editor, back up the current file and copy the version you want from `.autosave/` or `.bak` over it.

**M** opens the game's map. A left double click sets the editor's teleport point; **M/Esc** closes the map. After the terrain loads the camera stays flying above the ground.

"Development diagnostics" adds extra events, memory state and exception dumps in `logs/diagnostics/<session>/`. It is on by default in the editor; the setting is kept in `editor.ini`. A session keeps the exact DLL and its build symbols. A dump marked `FIRST-CHANCE` is an exception the game may still handle: not a crash by itself. At most eight different addresses are dumped per session. Writing dumps needs `build/dgmp-crashdump.exe`, built with the client. Diagnostics send nothing over the network.

Storage and dump-writer checks: `tests\editor-tools.bat` from a Visual Studio x64 prompt. Results stay in separate test folders under `build/`; the modes' files are not touched.

## Your own server

### Quick, from the launcher

"Host a server" → pick a mode. The launcher starts `dgmp-server.exe` and shows its console. A player from the same computer (127.0.0.1) is an admin by default (`LocalAdmin=1`).

### Dedicated server on Windows

```bat
dgmp-server.exe --mode freeroam --port 7777
dgmp-server.exe --config my.ini --name "My server" --max 32 --password secret --bind 0.0.0.0
```

Command-line flags (`--name --mode --port --max --password --bind --master-server --config`) override `server.ini`. Open **UDP 7777** in the firewall and on the router. No TCP is needed: the same port answers the launcher's status queries.

### Server on Linux / VPS

Building with CMake, a systemd unit, the firewall and crash analysis are in [`docs/server-linux.md`](docs/server-linux.md). In short:

```bash
cmake -S . -B build-linux && cmake --build build-linux -j
./dgmp-server --mode freeroam
```

### `server.ini`

Every key is described by comments in [`server.ini`](server.ini) itself. The main ones:

| Key | Purpose |
|---|---|
| `Name`, `Mode`, `Port`, `MaxPlayers`, `Password` | name, mode from `modes/`, port, slots, password |
| `Admins` | admins' SteamIDs, comma separated |
| `SteamAuth`, `SteamApiKey` | Steam account checks: `optional` / `required` / `off`. Get an API key at [steamcommunity.com/dev/apikey](https://steamcommunity.com/dev/apikey) |
| `RconPassword` | `/rcon login <password>` in chat makes a player an admin for the session |
| `MaxHitDamage`, `MeleeRange`, `MaxHitsPerSecond`, `CheatKick` | anti-cheat |
| `Scripts` | scripts from `scripts/` that run in any mode (like filterscripts) |
| `Http` | let modes make HTTP requests (Discord webhooks and the like) |
| `Voice*` | voice chat: range, limits |
| `Zombies` | experimental freakers ([`docs/zombies.md`](docs/zombies.md)) |
| `Master` | master server, so that the server shows up in the Internet tab |
| `SpawnX/Y/Z/Yaw` | default spawn point (Unreal units, 100 = 1 m) |

### Server console

`help`, `status`, `say <text>`, `kick`, `ban <id|nick|SteamID|IP> [minutes] [reason]`, `unban`, `bans`, `admin <id|nick> [off]`, `hitstats`, `voice [on|off]`, `bot [N]` / `bots off`, `reload`, `mode <id>`, `time <hours>`, `weather <id|auto>`, `quit`. Other lines go to the mode as the `console` event.

The mode reloads by itself when a `.lua`, `.ini` or `.json` file is saved; no server restart is needed. The log is `logs/server.log`. Modes keep their data in `data/<mode>/`; bans and reserved nicknames are in `data/`.

### Master server

`dgmp-server --master [--port 7700]` runs the server list for the Internet tab. How to host your own master and point the launcher at it: [`docs/master.md`](docs/master.md).

## Modes

A mode is a folder `modes/<id>/` with `meta.ini` (name, description, author) and `main.lua`. A minimal example and the whole API are in [`docs/API.md`](docs/API.md).

Examples to start from:

| Mode | What it shows |
|---|---|
| `freeroam` | the basics: chat, commands, skins, teleports, time and weather, admin commands |
| `rp` | the base of an RP server: Steam accounts, money, a shop, a hospital, RP chat, jobs |
| `race` | races along checkpoints from the editor |
| `loader` | a loader's job: an NPC, carrying crates, pay |
| `htmlui` | an HTML interface over the game |
| `clientdemo` | client scripts: HUD, keys, rays from the camera |
| `inventory` | a mode's own inventory with pictures |
| `zombies` | freakers and hordes (`Zombies=1`) |
| `outpost` | Rubezh, the test server's RP mode: professions, economy, job sites with their own layer, the player menu (squad, trade, radio, medic, guards, duels), downed instead of dead, a tutorial |

Shared libraries for modes (`logic`, `quest`, `jobs`, `rpchat`, `inventory`, `race`, `interact` — the player menu, `squad` — squads, `trade` — trading, `medic` — help for the wounded, `instance` — duels and arenas) are in `modes/_lib/`.

Documentation (in Russian for now):

- [`docs/API.md`](docs/API.md) — the server Lua API: events, players, vehicles, NPCs, objects, HUD, storage, HTTP, bans, anti-cheat, `server.ini`.
- [`docs/client-lua.md`](docs/client-lua.md) — client scripts (`modes/<id>/client/`).
- [`docs/webui.md`](docs/webui.md) — how the HTML interface works.
- [`docs/quests.md`](docs/quests.md) — the quest and job format (`quests/<id>.json`).
- [`docs/items.md`](docs/items.md) — item and weapon keys.
- [`docs/looks.md`](docs/looks.md) — character looks.
- [`docs/zombies.md`](docs/zombies.md) — freakers in multiplayer (prototype).
- [`docs/releases.md`](docs/releases.md) — releasing a version: GitHub Releases, `update.json` and launcher updates.

### Testing without the game

`build/dgmp-probe.exe` joins a server as a player and helps to check a mode or the network:

```bat
build\dgmp-probe.exe query 127.0.0.1:7777
build\dgmp-probe.exe lan
build\dgmp-probe.exe master 127.0.0.1
build\dgmp-probe.exe join 127.0.0.1 TestBot --seconds 30 --say "/help"
```

The server console's `bot [N]` adds bots as well.

## Editor

Open it with the launcher's "Editor" button or `DGMP.exe --editor`. Pick a mode first: each mode has its own world edits, kept in `modes/<mode>/world.json` and loaded by the server when the mode starts. Quests are saved to `quests/`, skin lists to `modes/<mode>/skins.json`.

| Key | Action |
|---|---|
| F2 | flight on/off |
| WASD, Space / Ctrl | move along the camera, up / down |
| Shift, `[` / `]` | ×4 speed, flight speed |
| F3 | dev menu: time, weather, map marks |
| F5 / F8 | save the start point / go back to it |
| F6 | write a marker to `maps/markers.jsonl` |

Coordinates are Unreal units: 100 units = 1 metre, the same coordinates the Lua API uses.

## Client settings — `DGMP.ini`

The file is personal and not in the repository: when building from source, copy `DGMP.ini.example` to `DGMP.ini` (and `editor.ini.example` to `editor.ini`); the client package brings its own. Mutes and volumes from the Tab list are kept there too, in `[Players]`.

- `[DGMP]` — `AutoLoad`, `AutoFly`, `FlySpeed`, `SuppressStory` (turn off story missions), `SuppressAI` (remove the game's own NPCs and freakers).
- `[World] RevealMap=1` — reveal the whole map without fog.
- `[Map]` — which of the game's marks the map shows.
- `[Spawn]` — the editor's start point (`Enabled=0` returns to the default).
- `[Voice]` — microphone, keys, volumes.

## Repository layout

```
src/              client (version.dll): UE hooks, network, HUD, editor, voice, client Lua
src/server/       dedicated server and master server
src/launcher/     launcher (WebView2); its interface is launcher/ui/
src/shared/       network code shared by the client, the launcher and the probe
src/tools/        the dgmp-probe test client
api/              the DLL's C API (dgmp.h) and an example
modes/            modes; modes/_lib/ — shared Lua libraries
scripts/          server scripts for every mode
quests/           quests and jobs
data/             game catalogues (looks, parts, props, animations, sounds)
worlds/bootstrap/ the shared starting save and its manifest
docs/             documentation
tools/            systemd units, packaging and release scripts, research helpers
third_party/      Lua, ENet, Dear ImGui, ImGuizmo, WebView2 SDK
```

Not kept in git, made while working: `build/`, `runtime/`, `profiles/`, `logs/`, `cache/`, `data/<mode>/`, built `.exe` files, `DGMP.ini`, `editor.ini`.

## Diagnostics

- `logs/DGMP.log` — the client in the game.
- `logs/launcher.log` — the launcher, `runtime/` preparation and updates.
- `logs/server.log` — the server; a crash writes its call stack here too.
- `telemetry.json` — client state and position, refreshed every second.

If the game does not start from the launcher, check that Steam is running, Days Gone is closed and the EXE version is supported (see "Requirements"). `DGMP.exe --prepare-only` prepares the copy and the profile again. Do not delete `runtime/` recursively with scripts: it holds links to the game's own files.

## License

DGMP is released under the **GNU GPL 3.0** ([`LICENSE`](LICENSE)): you may use, change and share the code, but changed versions of DGMP (client, server, launcher, editor) must also come with their source code under the GPL 3.0.

**Your modes are yours.** Modes and scripts for DGMP (Lua in `modes/<id>/`, `scripts/`, client scripts, HTML interfaces, `world.json`, quests) only use the server's and the client's API and are not derivative works of DGMP: their authors decide how to share them, closed source included. The example modes and libraries in this repository's `modes/` are GPL 3.0 along with it.

Third-party components keep their own licenses (`third_party/`): ENet, Dear ImGui, ImGuizmo and Lua — MIT; the Microsoft WebView2 SDK — Microsoft's license.

Days Gone is a game by Bend Studio / Sony Interactive Entertainment. DGMP is an unofficial fan project, not affiliated with them, and contains no game files: you need your own copy of Days Gone.
