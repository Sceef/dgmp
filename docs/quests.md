**English** · [Русский](quests.ru.md)

# Quests: file format and launch

A quest is a file `quests/<id>.json` in the DGMP folder (UTF-8). `id` — Latin letters, digits, `_` and `-`. The file is created by the in-game quest editor (the "Quest" window), but you can also write it by hand. The same file with `"kind": "job"` is a repeatable job (see "Job").

Texts (`title`, `text`, the steps' `text`, `meter`, names) — a string or `{ "en": "...", "ru": "..." }`: the player sees their own language, otherwise English.

```json
{
  "id": "supplies",
  "title": "Supplies for the camp",
  "category": "camp",
  "style": "basic",
  "text": "The elder asks you to check the old gas station.",
  "xp": 150,
  "reward": [ { "item": "kConsumableID_Bandage_Small", "count": 2 } ],
  "steps": [
    { "type": "goto", "text": "Get to the gas station", "x": -79464, "y": -301637, "z": -5250, "radius": 6, "route": true },
    { "type": "hold", "text": "Search the place", "x": -79464, "y": -301637, "z": -5250, "radius": 8, "seconds": 10, "meter": "Search" },
    { "type": "collect", "text": "Search the vehicles", "event": "search", "count": 2,
      "ids": ["BP_Interact_SearchCarTrunk21", "BP_Interact_SearchAmbulance_74"], "marker": true },
    { "type": "collect", "text": "Collect Molotov cocktails", "event": "pickup",
      "spawn": [ { "item": "kWeaponID_Molotov_Small", "count": 1, "x": -85160, "y": -302467, "z": -5249 } ] },
    { "type": "collect", "text": "Burn the evidence", "event": "fire",
      "area": { "x": -85000, "y": -302000, "z": -5250, "radius": 4 }, "marker": true }
  ]
}
```

## Quest fields

| Field | Description |
|---|---|
| `title` | the name (objective panel, the "Objective complete" screen) |
| `category` | `story`, `camp`, `infestation`, `ambush`, `nero`, `horde`, `campmission`, `task` — icon and color |
| `style` | the marker look: `basic`, `kill`, `killarea`, `defend`, `defendarea`, `search`, `track`, `nokill` |
| `text` | the string on the "New objective" banner |
| `xp` | experience on the completion screen |
| `reward` | items given on completion |
| `steps` | steps in order |

## Steps

Coordinates are game units (100 = 1 m), as in `telemetry.json` and `/pos`. Radii are in meters.

| `type` | Fields | Completed when |
|---|---|---|
| `goto` | `text, x, y, z, radius = 5, route = true` | the player reached the point (quest marker, route) |
| `hold` | `text, x, y, z, radius = 6, seconds = 10, meter = "Search"` | stayed in the zone for `seconds` seconds (the meter grows only inside) |
| `collect` | `text, event, count, item, class, ids, area, marker, route, spawn` | `count` game events matching the filters happened |
| `deliver` | `text, pickup, dropoff, count = 1, route = true` | the player carried `count` crates from the loading point and put them down (hold F) in the unloading zone |
| `talk` | `text, actor` (an NPC id from `world.json`) or `x, y, z`, `radius = 3, route = true, speaker, lines` | the player approached the NPC and read the `lines` (dialog windows one after another: "Next", the last one — "OK"; Esc skips the rest). `speaker` — the window title. Texts, as everywhere, a string or `{"en": ..., "ru": ...}` |

The `collect` filters are the same as in `q:collect` (`docs/API.md`):
- `event` — `pickup`, `carry`, `search`, `body`, `interact`, `door`, `explosion`, `fire` or a list;
- `item` — an item key or a list;
- `class` — part of the object's class name;
- `ids` — specific world objects (their names);
- `area` — `{x, y, z, radius}`;
- `marker` — a marker above the zone;
- `spawn` — items that the step lays out itself (with markers; only they count; `count` defaults to their number).

### `deliver` — carry crates

Crates carried with both hands (`docs/API.md`, "Carrying"): the player takes a crate at the loading point and puts it down in the unloading zone.

```json
{ "type": "deliver", "text": "Take the crate to the truck", "count": 1,
  "pickup":  { "x": -85450, "y": -302250, "z": -5348, "radius": 3, "boxes": 4, "model": "crate", "roll": "break", "name": "Warehouse" },
  "dropoff": { "x": -84900, "y": -301700, "z": -5340, "radius": 4, "name": "Truck" } }
```

| Field | Description |
|---|---|
| `pickup` | loading: the point, `radius` (m, default 3), `boxes` — how many crates lie there (4), `model` — `crate`, `cooler`, `medcase`, `centrifuge`, `roll` — what a roll does to the crate: `keep` (nothing), `drop` (it falls), `break` (it breaks), `name` — the marker label |
| `pickup.tag` | instead of new crates, the crates placed in the editor ("World" → "Carrying") with this tag are used; they return to their places. If the mode's world has none, the crates are laid around the point, as without a tag |
| `dropoff` | unloading: the point, `radius` (m, default 4), `name` |
| `count` | how many crates to carry |
| `route` | a route to the unloading point (yes by default) |

- A crate counts when the player put it down themselves (hold F) within the unloading radius. It immediately returns to the loading point: there are always the same number of crates there.
- Only a player on this step can pick up these crates (for a job — the one on the job); everyone else gets the hint "Take a job first".
- A crate dropped on the way (knocked out of the hands, the player left or died) returns to the loading point after a minute; one broken in a roll — immediately.
- In an ordinary quest the step lays out its own crates and removes them when the step ends.

## Job: `"kind": "job"`

A repeatable job (loader, miner, weed clearing...): an NPC gives the job, the player puts on a uniform, the steps run in a loop, and each loop pays money (`player:giveMoney`). The same job in Lua can be seen in `modes/loader`.

```json
{
  "id": "loader_job",
  "kind": "job",
  "title": { "en": "Loader", "ru": "Грузчик" },
  "category": "task",
  "text": "Carry crates from the warehouse to the truck.",
  "giver": { "x": -85300, "y": -301500, "z": -5340, "yaw": 180, "look": "m_mil_diamondlake_worker_003", "name": "Foreman" },
  "uniform": "m_mil_diamondlake_worker_001",
  "pay": 5,
  "payout": "end",
  "steps": [ { "type": "deliver", "text": "Take the crate to the truck", "pickup": { ... }, "dropoff": { ... } } ]
}
```

| Field | Description |
|---|---|
| `kind` | `"job"` — a job (without the field — an ordinary quest) |
| `giver` | who gives the job. `actor` — an NPC id from the mode's world ("World" → "NPC"): that NPC gives the job. Or the job's own NPC: `x, y, z, yaw`, `look` (look), `anim` (`idle` by default), `name` (the name above the head and the "[F] ..." prompt). With `actor` these fields are a copy for modes where there is no such NPC (for example, `questtest`). `marker: false` — no map mark, `radius` — the [F] radius (2.5 m) |
| `uniform` | the uniform for the duration of the job: a uniform id from the mode's `skins.json` or a game look; without the field — no uniform |
| `pay` | money for one loop (all steps once) |
| `payout` | `"each"` — right after each loop, `"end"` (default) — when the job is turned in |
| `cycles` | how many loops per shift; after the last one — "Turn in the job" with a route to the NPC. Without the field — unlimited |
| `xp` | experience on the completion screen when turning in (if at least one loop was done) |
| `title`, `category`, `style`, `text` | as for a quest; `text` — the description in the "Take the job" dialog |

How it looks in the game:
1. The NPC has the prompt "[F] Foreman". A dialog: the description, the pay, the buttons "Take the job" / "No".
2. On the job: the uniform, the objective panel with the current step, a line at the top right (`Loader: 3 · $15`), markers and a route — for this player only. The steps run in a loop; after a loop — "Done: +$5".
3. [F] at the NPC again — "Turn in the job": the payout (for `"end"`), the "Objective complete" screen, the uniform is taken off, the markers and the line disappear. Left the server — the shift ends without pay, the crate in the hands returns to the loading point.

A player can be on only one job at a time. A job does not give item rewards (`reward`).

## Launching from a mode

```lua
local Quest = require("quest")
local def = Quest.load("supplies")          -- quests/supplies.json or nil
Quest.run(player, def, function(player) end) -- steps in order, reward and experience at the end
```

`Quest.list()` — a list of all quest ids.

Jobs are placed in the world once for the whole mode, not for a player:

```lua
require("jobs")                              -- all jobs from quests/ (files with "kind": "job")
local job = Quest.runJob("loader_job")       -- one job; job:stop() removes it from the world
```

Without code — in the mode's `meta.ini`:

```ini
[Mode]
Jobs=all                 ; or Jobs=loader_job,miner
```

When a mode has jobs, saving a file in `quests/` reloads the mode (like editing `world.json`).

## Testing ("Test")

The "Test" button in the in-game editor's "Quest" window saves the quest, and the launcher (DGMP.exe) starts a local server with the `questtest` mode and the game. The mode gives the quest to every player who joins (server console: `quest <id>` — change the quest, `quest` — give it again). It places the job in the world and moves the player to the NPC who gives it (if that NPC is farther than 100 m).

## Editor

Quests and jobs are made only in the in-game editor; the launcher has no quest editor.

In the in-game editor (the "Quest" window):
- "Kind": "Quest (once)" / "Job (repeats)". A job gets a "Job" section: an NPC from the mode's world (the list of NPCs of the mode open in the "World" tab) or its own NPC — "Place NPC" and a click in the world (facing the camera), name, look, animation, map mark; the uniform; pay, when to pay, how many times per shift.
- The "Carry boxes" step: "Place the pick-up" / "Place the drop-off" and a click in the world, radii, how many crates lie there, model, roll, the tag of the world's crates, labels, how many to carry.
- The loading points, unloading points and the job's NPC are selected with a click and moved with the gizmo, like the step points.

## Preview in the editor

While a quest is open in the "Quest" window, the editor writes it (with unsaved edits and `"selected"`: the number of the selected step) to `quest_preview.json` in the DGMP folder, and the game shows the quest's markers in the world from it; the selected step is gold. Close the quest — the file is deleted.
