**English** · [Русский](README.ru.md)

# Rubezh — the DGMP demo RP mode

Iron Mike's camp: registration, professions, a shop and an armory, treatment, a personal motorcycle, storage, crate carrying, a patrol and an expedition. DGMP creates the NPCs and interactions. The coordinates of services and runs are taken from the logic points in `world.json`; the fallback set in `config.lua` matches this layout. Server `/setpoint` values take priority over editor points if they were saved earlier.

## Local rehearsal

Run `start-outpost.bat` from the DGMP root. In the launcher connect to `127.0.0.1:7778`. The local participant gets admin rights. The server is reachable only from this computer; for LAN play change `Bind` in `server-outpost.ini` to the address of the network interface. For a public launch set up Steam verification and admin rights separately — the current config is meant for a local rehearsal.

On the first join choose a look: the survivor in glasses comes first in the list, Deacon is not in the list. At the commander press F and get a pass: it sends you to the quartermaster. The quartermaster has two items: "Open storage" and "Start work". Accepting the job creates a physical crate: pick it up at the marked site and carry it to the quartermaster (pick up and put down by holding F). While the crate is in your hands, F belongs to carrying and does not open a neighbouring NPC. Only one job can be done at a time; changing profession and uniform during a job is forbidden. Further jobs are available at the commander under "Camp jobs", the delivery is given by the quartermaster. `/gps shop`, `/gps garage`, `/gps clinic` show the services. I opens the backpack, N — proximity voice, K — the radio, E — the vehicle. The mode's language follows the launcher language (Russian or English); for other languages English is used. The list of additional commands is available on request through `/help`.

To change profession, return to the commander, choose a profession and start the shift with `/duty`. A guard has the patrol, a medic has `/aid <id>` and reviving downed players, a mechanic has repairs with parts from the backpack.

**Tutorial.** After the first crate delivery the commander gives out the starting allowance ($80) and leads on:
1. buy a motorcycle in the garage;
2. buy a pistol from the trader ("Armory");
3. take the "NERO expedition" in "Camp jobs" at the commander;
4. kill the freakers and bring the medical supplies to the healer in the clinic.

The step is saved in the character and continues after a reconnect. On the first join a hint tells about voice chat (N, K, Tab, `/voice`).

**The job site of your own.** Salvage and the NERO expedition go as "runs". A run is one per player, and a squad (F at a player → "Invite to the squad") has one for everybody. Each run has its own virtual world. A participant who comes closer than 120 m to the site moves into this world together with the motorcycle. One who goes farther than 180 m returns to the shared world. So the caches, freakers and medical supplies at the site are yours alone: other players and squads at the same place do not see them and spoil nothing, and they have their own. The run time is `missionSeconds`. Death does not cancel it: after the clinic you can come back.

**NERO expedition** ("Camp jobs" at the commander or `/mission join`). The site is guarded by four freakers (`Zombies=1` is needed; without it the crate stands there at once). When they are killed, a crate "Take the NERO medical supplies" (F) appears at the site: every participant takes one pack. This is a quest item: it lies in the backpack (2 kg), is not sold, not handed over and not donated. The pack must be brought to the healer: clinic → "Hand over the NERO medical supplies". The reward is $180 and 8 reputation, and the camp gets +8 medical supplies. The progress of the run is shown by `/mission`.

**Salvage** ("Camp jobs" at the commander). At the `salvage` point there are three caches (a toolbox, a backpack, an ammo crate). They are searched with F (4 seconds next to the cache), each gives 1 part. Any participant of the run can search, and when the third is found, everyone gets the reward.

**Where things stand — in the editor:** "World" → the `outpost` mode → "Mode logic":
- `salvage_cache` — the cache places (several points, the mode takes three random ones; without them the caches are placed in a ring of 4 m around `salvage`);
- `expedition_zombie` — the freaker places (up to four points; without them the freakers stand 9 m around `expedition`);
- `expedition_meds` — where the medical supplies crate appears (without it — at the `expedition` point itself).

Put the points on the ground, the arrow sets the rotation of the crate. The caches and the crate are server objects in the run's world, not trunks of game vehicles: the game's vehicles are separate for each player, and searching them affects nothing.

Players who die appear at the `clinic_spawn` point; without it — 2 m in front of the medic (the `clinic` point is the place of the NPC itself, a player put there ends up inside it).

The places where motorcycles are issued are set separately from the mechanic: editor → "World" → the `outpost` mode → "Mode logic" → `garage_spawn` → "add". Put a point on the floor inside the garage, select it and set the direction in the Inspector, then save the world. You can add several places. The motorcycle appears exactly at the first free point, with its Z and yaw, without an offset by player id. Places occupied by a vehicle or a player are skipped. While no points are placed or all are occupied, issuing does not happen and the money for the purchase is not taken.

Buying a motorcycle issues it immediately at the configured place and marks it on the map. If creating the vehicle failed, the payment is not taken. The item "Collect your motorcycle" is used after returning to the garage and does not require buying again. A motorcycle left in the world is removed when the player leaves and stands in the same place on the next join, with the same fuel and condition. `/lock` within 30 m of your motorcycle unlocks or locks it: an unlocked one can be taken by the handlebars by anyone, the seat behind can always be taken. A motorcycle can be returned to the garage from anywhere (for example if it is stuck) if nobody is sitting on it; fuel and condition are kept. A player has one motorcycle: after the purchase there is no buy item in the garage menu. A motorcycle lying on its side stands up by itself when someone gets on it with E. The NPC service works only for a motorcycle at the issue place or next to the mechanic.

The game inventory (weapons, ammo, materials) is kept in the character: the mode saves every snapshot the client sends and issues it again on join (snapshots are not saved for the first 5 seconds after joining — the server is taking away weapons then through `StartWeapons=none`). Weapons bought earlier are restored from the purchase log. Characters, the base look and the economy are saved on the server in `data/outpost/storage.json`: confirmed users — by Steam ID, guests — by nick. The work uniform is temporary and does not replace the base look. The engine also stores the last skin by nick in `data/skins.json`, but on join the mode restores the look from its own account. Different servers must have separate data folders: two processes from one folder share the saves. The `reload` command in the server console rebuilds the mode keeping the characters, but cancels unfinished jobs.

## Players with each other, downed state, duels (`social.lua`)

**The player menu.** Look at a player closer than ~2.5 m and press F (the prompt "[F] Interact"; among several targets nearby F goes to the one the camera looks at). What is in the menu:
- "Invite to the squad" — with consent (the other player sees "[Y] Yes [N] No" at the bottom of the screen). The squad sees each other on the map and radar, `/sq text` is the squad chat, `/squad` is the roster, `/squad leave|kick <id>|lead <id>`.
- "Offer a trade" — a window for two: items from the camp backpack and money onto the "table", both press "Ready", the trade goes through whole or not at all (backpack weight is taken into account).
- "Share your radio channel" — the other player is offered the channel of your radio (both must have a radio).
- "Give money" — the amount in an input field.
- A medic on shift: "Treat (medkit)" — healing; "Revive" — 6 s on the knees, uses a medkit, the downed player gets up with 35%.
- A guard on shift: "Check papers" (with consent: name, number, profession, pass, reputation), "Search" without consent if the player's hands are raised (`/hands`), otherwise "Ask to show belongings"; "Tie up" a player with raised hands — they do not walk, shoot or do anything until untied ("Untie", with the consent of the tied one).
- "Challenge to a duel" — with consent: a fight to two wins in your own virtual world right on the spot, both have an M9 with 60 rounds; after the duel the place, backpack and health come back, camp money and items are not spent, a death in the duel is free.

**Downed state instead of death.** When the health bar is empty, the player falls downed: 4 minutes until death, H — call a medic (medics on shift get a question, the first to agree gets a route), after a minute you can give up (hold E) — and wake up in the clinic for a fee, as before. If there are no medics on the server, you can give up at once. A hit on a downed player finishes them off. A downed player does not walk and does nothing, but talks and writes in chat.

**Tab** — the list of the server's players with profession (● — on shift), ping, who is speaking; there also "Mute" and the voice volume of each — for you only.

## The camp border

If there is no zone named `outpost_camp` in the editor, the code creates a rectangular safe zone around all services with a margin at the edges and in height. To outline the fence or make a complex contour, create a PvE zone in the editor, name it **outpost_camp**, choose "Polygon" and draw the border. The mode uses the saved zone, keeping its shape. In a PvE zone players deal no damage to each other; freakers and NPCs stay dangerous.

## Recording scenario

1. Creating a character, arriving at the camp and registration. The `wardrobe` point is used for the starting choice of look; the commander's menu has no wardrobe.
2. Getting a job from the quartermaster: a physical crate, delivery and payout.
3. The shop, the backpack, the stash, giving money and the bank.
4. Choosing professions and uniforms: treating another participant, buying and servicing a motorcycle, a patrol of two posts.
5. Gathering an expedition, the radio, setting out, searching and delivering medical supplies.
6. Optionally — a staged alarm, freakers, a trap and evacuation by helicopter.

The admin `/director` shows the control panel. `/director kit` issues a filming kit and changes money and items of your own character only; do not use it to test the normal economy. `/scene <point>` moves the admin, `/bring <id>` summons a participant, `/cinema` hides the mode's HUD. `/director dawn|day|night` changes the light, `/director alarm` announces an alarm, `/director horde|trap|evac` creates staged objects at the expedition, `/director fly` sends the helicopter to the camp, `/director clear` removes them. The synchronization of freakers and the landing/flight of the helicopter must be checked separately in the game with several participants before recording.

Check visually in the game: the height and direction of the NPCs, the free motorcycle issue place in front of the mechanic, the path with the crate, the camp border and both helicopter pads. The server checks do not verify collisions and level geometry.

## Development checks

From the DGMP root: `build/lua-test.exe tests/outpost.lua`, `python tests/outpost_smoke.py`, `python tests/zones_smoke.py`. The native checks start separate servers and create data in test folders under `build/`, without touching the characters of the working mode.
