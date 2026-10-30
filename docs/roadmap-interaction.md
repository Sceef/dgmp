**English** · [Русский](roadmap-interaction.ru.md)

# Plan: sub-modes, player interaction, downed state, pause menu

Draft of 2026-10-30. **Done 2026-10-31 (v0.8.0)**: all items below are in the core and libraries and wired into Rubezh; for the description see `docs/API.md` ("Interactions and keys", "Downed state", "Player menu — the `interact` library", "Instances and duels — the `instance` library", "Voice chat" → the Tab list), for what was checked in the game see `VERIFICATION.md`. Differences from the plan: the action menu is a list dialog (not radial); "lead along" and paired animations are not done; an exact backpack snapshot on request is not done (the last one is used, up to 2 s old); the server does not check the facing direction (the camera and the body are independent in the game), only distance and world; voice settings open from the Tab list, not from the native pause menu. The main rule for every item: **the core provides the mechanism, the mode decides** whether to enable it and how. Whatever can be done in Lua is done as a library in `modes/_lib/`, and only what cannot be done in Lua is added to the core (client and server).

The order of work is at the end of the file.

---

## 1. Sub-modes inside a server (duels, arenas, instances)

### What already exists

- Virtual worlds: `newWorld()` / `freeWorld(n)`, `player:setWorld(n)`, the `playerWorld` event. Worlds separate bodies, vehicles, markers, items, loot, sounds, shots, corpses and voice (commit c8b857e).
- `player:setPvp(true)` on both — damage works even in a safe zone.
- `restoreInventory`, `giveWeapon`, `setAmmo`, `takeWeapons`, accounting of protected items.
- `playerDeath` / `playerRespawn` with `return true` — the mode decides where to respawn.
- `player:spectate(on, players, {world = n})` — watch another world.
- The `race` mode is an example of a sub-mode.

### What is missing

**The `modes/_lib/instance.lua` library** — a common base for any sub-mode:

```lua
local instance = require("instance")
local duel = instance.create({
    name = "Duel",
    world = "new",                       -- newWorld(); or a number — a shared world for an arena
    spawns = { {x=..,y=..,z=..,yaw=..}, ... },   -- or "here": at the same place where the players were
    loadout = { weapons = { kWeaponID_Pistol_1911 = 60 }, items = { kItemID_Bandage = 2 } },
    health = 100,
    pvp = true,
    rounds = 3, timeLimit = 180,         -- seconds per round
    onDeath = "round" ,                  -- "round" | "respawn" | "out"
    onEnd = function(inst, result) end,  -- result.winner, result.score
})
duel:join(player); duel:leave(player); duel:finish(); duel.players
```

`join` takes a **snapshot** of the player: position, world, backpack (weapons, ammo, items), health, money (if the mode says so), voice channels, PvP flag. Then it hands out the arena loadout and moves the player. `leave` and `finish` put everything back as it was. A death inside the arena takes nothing away: the library intercepts `playerDeath` / `playerRespawn` of its players.

**The snapshot survives a crash.** The snapshot is put into `storage` by SteamID before entering the arena and removed after returning. If the player quit, crashed or the server restarted, on the next join the player gets their backpack back, not the arena loadout. This is the main protection against losing and duplicating items.

**Core changes:**

1. **An exact backpack snapshot on request.** Currently `player:inventory()` is a snapshot once per 2 s. We need `player:snapshotInventory(cb)`: the client sends the backpack right away, including weapon upgrades and melee weapon durability (if they can be read — check in the game). Without it the last picked-up item can be lost after a duel.
2. **Anti-cheat accounting during the arena.** The issued loadout must not become "legitimate" forever: on `leave` the accounting goes back to the snapshot. We need `player:pushItemLedger()` / `popItemLedger()` or a parameter of `restoreInventory`.
3. **Zombies and NPCs by world.** Do server zombies and `createActor` already know `world`? Check for zombies (`docs/zombies.md`). The game's own AI is separate on each client and is not shared between worlds: it is better to put the arena where the game spawns nobody, or in an interior made of props.
4. **World details:** `broadcast(text, {world = n})`, `players({world = n})`, a shared `setTime` / `setWeather` for a world (currently only per player).

### Sub-modes built on this

- **1-on-1 duel** — a challenge from the player menu (item 2): confirmation from the other player, 3-2-1 countdown, a fight in a new world at the same place, spectators via `spectate`.
- **Arena / deathmatch / gun game** — a lobby (`/arena`, map blip, interaction point), a queue, a score table in the HUD.
- **Races** — move `modes/race` onto `instance`.
- **Survival against hordes** — its own world, zombies in waves, a reward afterwards.
- **Interiors and tutorials** — one player in their own world.

A mode enables only what it needs: `instance` registers nothing by itself until the mode has called it.

---

## 2. The player interaction menu

### What it looks like for the player

You come closer than ~2.5 m to a player and look at them — at the bottom "[F] Interact" (like with NPCs and interaction points). Pressing it opens a list of the actions the mode allowed **for this pair of players right now**. An action that requires consent shows the other player a window "Raud offers to join the squad — [Y] Yes / [N] No" with a timer.

### Target selection: distance **and aim** — for everything, not only players

Currently the client takes the **nearest** interaction point in the radius (`mpui.cpp`, `Update`: only horizontal distance and |dz| < 3 m), and vehicles and crates are a fallback if there are no points nearby. Where the player is looking is not taken into account. When a seller, a door, a crate, a motorcycle and two players are all nearby, the prompt goes to the wrong one, not the one being looked at, and jumps with every step.

We make one common target selection on the client, for all sources at once:

- **Candidates:** the mode's interaction points (`createInteraction`, including those on NPCs), the NPCs themselves, players (item 2), downed players (item 3), the mode's vehicles, carrying crates, entrances, items and loot on the ground.
- **Filter:** within the candidate's radius, the same world, and **within the camera's view cone**: the angle between the camera direction and the direction to the target point is at most ~25° (at close range the cone is wider: we take the angle to the nearest point of the target's capsule/sphere, not to the center). Each candidate has its own "aim point": for a player and an NPC — the body capsule, for an interaction point — a sphere of `aimRadius` at the height `aimHeight`.
- **Line of sight:** the ray from the camera to the target does not hit a wall (the same `raycast` as in client scripts). We do not select through a wall or a door.
- **Score:** a smaller angle is better, distance is the second weight. One candidate wins.
- **Stickiness:** the selected target holds while it stays in a slightly wider cone (~35°), so that the prompt does not flicker between two neighbouring targets.
- **Prompt at the target:** "[F] Talk" is drawn above the selected target (like an NPC name), not only at the bottom of the screen: this shows what exactly is selected. The bottom one stays as it is now, for compatibility.
- **Without aiming:** large zones (an entrance as wide as a door, "stand in the circle") do not need aiming — `aim = false`, then they take part as now, by distance, but lose to any target under the gaze.

API (backward compatible: without the new fields — as now, only taking the gaze into account):

```lua
createInteraction({ x =, y =, z =, radius = 2, text = "Shop",
    aim = true,            -- you must look at the point (default true)
    aimHeight = 1.0,       -- m above z: where to look (the counter, an NPC's face)
    aimRadius = 0.5,       -- m: the size of the "target" for aiming
    priority = 0,          -- with equal aim the greater one wins
    actor = ava,           -- the point is bound to an NPC: aim at its body, follow it
})
```

The **server** checks, as now, distance and world, plus roughly the direction: the client already sends the camera rotation in the pose; refusal if the target is behind the player (angle > 90°). Precise aiming is the client's business: faking it only gives a choice among what is nearby anyway.

Client scripts need the same target selection: `target()` → `{kind = "player" | "npc" | "interaction" | ..., id}` — what is selected now, for their own prompts and HUD.

### Core (client + server)

1. **A player as a target.** The client `raycast` already hits the capsules of other bodies (`player = id`). Players become candidates of the common target selection (above): the prompt "[F] Interact", and on press the client sends `Msg::PlayerInteract{target}`. The server checks distance, world, direction, that both are alive (or the target is downed, see item 3).
2. **Action registration** (server, Lua):

   ```lua
   addPlayerAction({
       id = "squad_invite",
       label = { ru = "Пригласить в отряд", en = "Invite to squad" },
       category = "Squad",                     -- a group in the menu
       range = 3,                              -- meters
       when = function(player, target) return not squad.same(player, target) end,  -- whether to show
       consent = { ru = "%s зовёт вас в отряд", en = "%s invites you to a squad" },  -- the target's consent is needed
       onUse = function(player, target) squad.join(target, player) end,  -- after consent
       downed = false,                          -- whether available on a downed player (item 3)
   })
   removePlayerAction("squad_invite")
   on("playerInteract", function(player, target, actions) end)   -- items can be added / removed on the fly
   ```

   The server builds the list from `when`, sends it to the client, and on selection checks `when` and distance again.
3. **Consent:** `player:ask(target, text, {timeout = 15}, function(ok) end)` — a separate call, used by both the actions and any mode code. A rate limit, `/ignore <nick>` — receive no offers from that player.
4. **The menu window.** Version 1 — a short list in the style of a dialog (there is already `player:dialog`, but better a small window of its own at the cursor, which does not take the whole screen). Version 2 — a radial menu. A mode can replace the window with its own HTML page: the event with the ready list arrives in `onUi`.
5. **Controlling the player** — needed for tied, downed and detained players: `player:setControls({ move = false, sprint = false, weapons = false, vehicle = false, jump = false })`. Currently there is only `freeze` in `playAnim`. Holstering the weapon and not letting it be drawn — to be researched in the game.
6. **A blip that follows a player:** `createBlip({ follow = player, player = viewer })`. Currently blips stand still; moving them with `setBlip` every second for every squad member is extra traffic.
7. **Paired animations** (handshake, search, handcuffs): both animations start in the same tick, the second player is placed at a point opposite the first. Optional, after everything else.

### Ready-made actions — `modes/_lib/` (the mode includes what it needs)

| Library | Actions | What the core needs to provide |
|---|---|---|
| `squad.lua` — squad | invite (with consent), leave, kick, hand over leadership. Members see each other on the map and radar, the nick in a different color, the `/s` chat, a shared radio channel, optionally per the mode — no damage to your own (`setPvpRule`) | the `follow` blip, nick color for selected viewers |
| `radio.lua` — frequencies and numbers | "Share a frequency" — the target gets the frequency in the list of their radio; "Exchange numbers" — a contact in the phone, a call through the `"call"` channel. The lists are kept in `storage` by SteamID | nothing, voice already supports channels |
| `trade.lua` — trade | a window for two: my offer / their offer, money, a "Ready" button. Any change takes "Ready" back from both. When both are ready the server checks the amounts again and moves everything at once | for the game's backpack — an exact snapshot (as in item 1) and a window with two sides that both players see |
| `police.lua` / `faction.lua` — jobs and factions | check papers (a card: name, faction, wanted status — from the mode's data), search / check for contraband (marked items; confiscate into an evidence container), tie / untie, handcuffs, lead along, put into a vehicle, issue a fine | `setControls`, "lead along" (`player:follow(leader)` — the target walks after the leader, movement computed by the target's client) |
| `social.lua` | give money (there is `/pay`), show papers, shake hands | paired animations |

Each faction action checks the role in `when` (`player.data.job == "police"`) and the state of the target: searching without consent is allowed only for a tied or downed player or one with raised hands (`target.anim == "dcn_surrender"`) — the rule is the mode's decision.

---

## 3. Downed state instead of instant death

### Now

A lethal hit is intercepted (`ModifyDamage` → 0 and a flag), what remains is a dummy corpse, the "Respawn" screen, and `playerRespawn` chooses the place. There is no real death of the game.

### How it should be (if the mode enabled it)

```lua
setDeathMode("downed", {
    bleedout = 300,        -- seconds until death
    giveUpAfter = 30,      -- after how long one can give up
    crawl = false,         -- crawl (slowly) or lie in place
    talk = "near",         -- voice: "near" (quieter and closer), "off", "all"
    hitDowned = "finish",  -- a hit on a downed player: "finish" — finishes off, "ignore" — does not
})
setDeathMode("instant")    -- as now (default)
```

**The player falls:** health holds at 1, the body plays a "lies wounded" animation (`bzr_injury_lay_down` → `bzr_injury_lay_down_pillow_loop`, for Deacon — the end of the death animation; when crawling — `dcn_army_crawl`), weapons and running are unavailable (`setControls`). A red vignette (the bear trap already has its own), a timer "You are bleeding out 4:32", prompts "[H] Call a medic", "Hold [E] — give up" (after `giveUpAfter`). Others see the pose as usual — the body pose is already transmitted.

**Events and methods:**

| | |
|---|---|
| `playerDowned(player, attacker, info)` | `return false` — immediate death, as now (for example, in an arena) |
| `playerRevived(player, by)` | picked up |
| `playerBleedout(player)` | bled out; then the usual `playerDeath` / `playerRespawn` |
| `player:down([attacker])`, `player:revive([health])`, `player.downed`, `player:setBleedout(seconds)` | |

**Medic — `modes/_lib/medic.lua`:**

- "Call a medic": if there are players with the medic role on the server (the mode decides: `medic.isMedic = function(p) return p.data.job == "medic" end`), they receive a call — a sound, a message, a map blip at the downed player. The first one to accept gets a route marker, the downed player sees "A medic is on the way: Raud".
- No medics — the downed player sees "No medics on the server", and "Give up" becomes available immediately.
- A medic sees "Revive" in the player menu (item 2): hold for 5 s, a bar (`objectiveMeter`), the `kneel` animation on the medic, uses a medkit, revives with 30% health. The mode can give "Bandage" to everyone: it stops the timer but does not revive.
- "Give up" and bleeding out — the old path: the "Respawn" screen → `playerRespawn` → hospital.

**Edge cases to close:**

- Downed on a motorcycle — throw off it. In water, from a fall from a height, from `player:kill()` — no downed state, immediate death (or as the mode decides).
- Leaving the server while downed = death with the mode's penalty (against leaving a fight).
- A downed player does not shoot: anti-cheat counts them as dead (`hitWhileDead`).
- Server zombies: whether to attack a downed player or not is a setting. The game's own AI will keep hitting them: we hold health at 1, but check whether the game goes into its own death.
- The voice of a downed player and the mode's HUD — do not hide them, unlike the "Respawn" screen.

**To research in the game:** lay the local player down with an animation in the `FullBody` slot and hold the pose; forbid drawing a weapon and aiming; how the game reacts to damage at health 1; the camera when Deacon is lying down.

---

## 4. The server name in the pause menu instead of "days lived"

Found (`logs/pause-menu-before.txt`): the pause menu widget has a function `SetNumDaysGone` (takes `GetWorldDaysGone` and formats the text) and a field `NumDaysGone` (a text widget, `0x620`). `policy.cpp` already hooks this object (`TrackPauseMenu` / `Install`) and already changes the text of the "Return" item through `SetText` (`FilterPauseMenu`).

Plan:

1. After `SetNumDaysGone` (and on every opening of the pause) call `SetText` on `NumDaysGone` with the server name — it already reaches the client (the DGMP line at top right).
2. Find the neighbouring widget with the "Days lived" caption, if it is separate, and replace it with "Server" / the mode name, or hide it.
3. `server.ini` → `PauseTitle=` (default `ServerName`), in Lua — `player:setPauseText(text)`: for example, "Raud · Police · 12 / 32".
4. Check Cyrillic, long names (cut to ~40 characters), that the text is not reset when switching menu tabs.

A small job, can be done first.

---

## 5. Voice: settings in the pause, a player list, personal mute

### Now

- Voice settings are only through the `/voice` command in chat (a client window: volumes, keys, "hear yourself", microphone always on / off).
- `server.ini` → `Voice=0/1` — only at startup; it cannot be switched on the fly. A mode can mute one player (`player:setVoiceMuted`), but not the whole server.
- There is no player list. You cannot mute an annoying player for yourself — only ask an admin.

### How it should be

**A server panel in the pause menu.** While the pause menu is open, to the right of it is our panel (an overlay, with the menu cursor). This is also the "Tab list": Tab is taken in the game, and the pause opens with one key anyway.

- Header: the server name, the mode, `12 / 32` players, ping.
- **Player list:** id, nick, ping, a "speaking now" icon, the mode's own columns (`setScoreboard({columns = {"Faction", "Level"}})`, `player:setScoreboard({"Police", 5})`). At the top — "Speaking nearby": those you can hear right now, to find a spammer quickly.
- For each: **"Mute"** (for you only) and the volume of this player 0–200%. The muted list is kept by SteamID in `DGMP.ini` and survives a reconnect. The client reports it to the server, which **does not forward** the muted voice: traffic is not wasted and no slot in `VoiceListeners` is taken (otherwise a spammer with a loud microphone keeps pushing out normal players).
- A **"Voice"** tab — the same settings as `/voice` (the `/voice` window stays).
- Muting is also possible from the player menu (item 2: "Mute for yourself").
- Bots and yourself are not to be muted in the list; admins also see "Kick" / "Server mute" (through `/rcon`) for each.

**Server:**

- `setVoiceEnabled(false)` / `voiceEnabled()` in Lua and the console command `voice on|off` — switch voice off for everyone on the fly (players see "Voice chat is disabled on the server"). `Voice=` in `server.ini` is the initial value.
- `player:setVoiceMuted(true, minutes)` — a timed mute (an admin command in the mode examples).
- The `voiceMute(player, target, on)` event — who muted whom for themselves (a mode can count complaints: "5 people muted X within 10 minutes" → auto-mute).

---

## Order of work

| Stage | What | Why earlier |
|---|---|---|
| 0 | Item 4 — the pause menu; item 5 — the server panel in the pause with a player list, personal mute and voice | quick, separate from the rest; mute is needed against spammers already at the first test |
| 1 | Core: common target selection by aim (for interaction points, NPCs, vehicles, crates — at once, this improves already existing modes too), the action menu, `player:ask`, `setControls`, the `follow` blip, an exact backpack snapshot | items 1–3 stand on this |
| 2 | `squad`, `radio`, `trade` | the most needed for RP, tests the menu in practice |
| 3 | Downed state and `medic` | depends on `setControls` and the menu (the "Revive" action) |
| 4 | `instance` and a duel from the menu | depends on the snapshot and anti-cheat accounting |
| 5 | Faction actions: search, papers, tie, lead, handcuffs; paired animations | rely on everything above |

At each stage: a section in `docs/API.md`, an example in `modes/rp` (the duel — a separate small mode), lines in `VERIFICATION.md`, support in `dgmp-probe` and in bots (`botpose`) — so that it can be tested with two players without a second computer. New network messages — a protocol version bump.

## Questions to decide

1. The player menu key: F (as with NPCs — which target gets F is decided by aiming) or a separate one, for example G?
2. The aiming cone angle (~25°) and the prompt above the target — suitable, or keep the prompt only at the bottom?
3. The menu window: a list at the cursor (quick) or radial right away?
4. Duel: at the same place in a new world or in a separate arena? After the fight — return to the exact place?
5. Downed state by default: bleed-out time, whether crawling and talking are allowed?
6. Squad: only the map and radar, or also nicks through walls for squadmates?
