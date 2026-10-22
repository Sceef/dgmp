# Freakers in multiplayer (design + prototype)

Status 2026-10-22: research done in the running game (DGMP client, Belknap / Marion Forks, post-prologue
template save); prototype behind `server.ini Zombies=1` (off by default). Verification state is listed at
the end; anything not marked *verified* is design.

Until now `src/client.cpp` Sweep hides every `BendAIPawn` and stops the ambient generator when
`DGMP.ini SuppressAI=1`, so multiplayer has no enemies at all. That stays the default for the game's own
population (each client would roll its own random freakers). Zombies come back only as **server zombies**:
the mode decides where they are, one client runs each one, everybody sees the same one.

## 1. How the game populates the world (verified)

### Ambient population: `BendAmbientGenerator`

One actor, `AmbientSpawnGenerator_C_0` in the persistent level, spawns all individual NPCs (freakers,
animals, marauders, survivors):

- **Spawn types per region.** `PooledSpawnTypes` (TArray of `BendAmbientSpawnType`, 1056 bytes each) holds
  what the current region has loaded. At Marion Forks (18 types): Swarmer 15, Newt 9, LootNewt 1, Survivor 6,
  Marauder 20, RIP 15, Crow 10, FreakCrow 15, Deer 4, Wolf 3 have a loaded pawn class; Bear, Screamer,
  Breaker, Reacher, Bleacher, Cougar, RagerBear, FreakWolf are listed but not loaded (`m_LoadedPawn` null).
  Fields used: `FactionToSpawn` @0x0 (`EAIFactionType`), `LoadedArchetype` @0xD8 (`Swarmer_Tier1`),
  `m_LoadedController` @0x100 (`SwarmerController_C`), `m_LoadedPawn` @0x130 (`SwarmerPawn_C`),
  `m_LoadedBehaviorTree` @0x168 (`SwarmerTree`), `m_LoadedAnimInstance` @0x190 (`SwarmerBVim_C`),
  `MaxSpawnsOfType` @0x3D8. Dump: `sdump BendAmbientGenerator PooledSpawnTypes <i> 1`.
- **Pools.** `PooledSpawns` (44) are pawns created up front and kept hidden; a spawn takes one from the pool,
  a despawn returns it (`find SwarmerPawn_C` shows hidden pool members). Radii: spawn 50-109 m around the
  player, may despawn beyond 120 m, forced beyond 150 m; global limit 15 by importance.
- **Where.** `GlobalPopulationSpawnArea_C` (22 near the player), `BendAmbientSpawnArea` (12),
  `ConditionalFreakerSpawnArea_*`, and placed `XPEC_Spawner_CPPRM_C` (`BendPawnSpawnLocation`, 20) points.
- **Entry points** (UFunctions of the generator):
  - `SpawnForArcadeMode(SpawnType, Location, Rotation, Count, DistanceToDespawn, spawnerID, bForceChasePlayer,
    bCanDespawnByInactivity, Group) -> bool` — **any point**, used by the arcade/horde-mode spawner.
  - `RequestSpawnSimple(Type, Location: BendSpawnLocationBase, OnSpawnedDelegate, Count, deactivateLocation)`,
    `RequestSpawn(BendSpawnRequestData, Count, deactivateLocation)` — need a spawn location actor.
  - `DespawnUnit(Controller, notifyDespawn, runFullDespawn)`, `ForceDespawnAll`, `SetImportanceToBlockBelow`,
    `OverrideGlobalSpawnLimit`, events `OnSpawned/OnDespawned/OnKilled`.

*Verified:* `SpawnForArcadeMode` with a copy of the loaded Swarmer type spawns a `SwarmerPawn_C` (controller
`SwarmerController_C`, behaviour tree running) at exactly the requested point within 0.25 s, during the day,
10-30 m from the player. `bForceChasePlayer=true` did not make it chase by itself.

### Freaker pawns

`BendFreakerPawn : BendAIPawn : BendPawn` — `SwarmerPawn_C`, `NewtPawn_C`, `LootNewtPawn_C`,
`HordeSwarmerPawn_C`, `FreakCrowPawn_C`. Swarmer: Health/MaxHealth 200, `AIControllerClass`
`SwarmerController_C` (`BendSwarmerController : BendFreakerController : BendAIController`),
`BrainComponent` = `BTComponent`, `PathFollowingComponent`, movement `BendClimbingSurfaceCharacterMovementComponent`.

Movement is animation driven (Bend): `desiredSpeed` / `DesiredSpeedSmoothed`, root motion. The AI picks a
speed with `BendAIController::SetMoveSpeed(EAIMoveSpeed)`: Walk 150, FastWalk 320, Jog 415, Run 450,
Sprint 500 units/s (night values separate). `MoveToLocation(..., bUsePathfinding=false)` moves at the
chosen speed; with pathfinding our test crawled (not investigated further).

### Hordes: the crowd system (not pawns)

- Every ambient horde is a streamed sublevel `/Game/maps/ORWorld/<region>/Ambient_Hordes/<cell>_Horde<ID>_<Name>`
  (near the spawn: `04-12_HordeB072_MarionForks`, `02-13_HordeB045_ShadowLake`) with
  - a `BendCrowdGroup` (`HordeB072_HordeGroup`: MaxTotalSpawns 50, PickupSubclassName Swarmer, a `BendDefendZone`,
    `Controller` = a `BendCrowdController` with a needs system, POIs, awareness),
  - three `BendCrowdGeneratorBox_C`: `Horde_Gen_Drink`, `Horde_Gen_Eat`, `Horde_Gen_Hibernate` (NumSlots 300,
    spigot, ActivationRadius 120 m, min 50 m, importance Job) — the daily cycle of the horde.
- Members are **crowd agents**, not actors: `BendCrowdFunctionLibrary` addresses them by a u16 handle
  (`GetCrowdMemberTransform`, `KillCrowdMember`, `GetClosestCrowdMemberLocation` after
  `RegisterClosestCrowdMemberQuery`). They are drawn by shared-animation mesh components and **promoted** to
  real pawns near the player: 8 `HordeSwarmerPawn_C` are pooled for that (`IsPawnPromotedHordeMember`).
- Global groups: `BendCrowdGroupGlobal_0..2` (the generator's `ScreamCrowdGroup`, `DemotedCrowdGroup`).
- In our save the hordes are **inactive** (generators `bIsActive=false`, 0 members). `SetActive(true)` +
  `GenerateCrowdMember(Location, Rotation, bSpawnImmediately, SystemID, bForceChasePlayer)` did create a member
  (`GetNumMembersAlive` 1, closest member reported at the requested point) but **nothing was drawn**, and the
  horde level reloaded soon after (new group object). The horde rendering/simulation depends on story
  progression state the template save does not have. Real hordes are therefore a later research item.
- Nests: `Nest_C` actors in the `<cell>_Pop` levels (`BendNestComponent`: NestID, burning, despawn distance),
  `BendNestingZoneActor` volumes for infested zones.

## 2. One client runs the AI, the others see puppets (verified locally)

Test (`z mirror`, one game): a real swarmer driven by its AI controller, and a second swarmer as a *puppet*
following the first one's sampled place, yaw, speed and montages with a 0.25 s delay and 2.5 m to the side —
exactly what a remote client would get over the network.

- **Stop the AI:** `BrainComponent.StopLogic("dgmp")` — the pawn stands, does not attack, keeps its
  controller (needed for rotation and movement input). `RestartLogic` gives the AI back.
- **Walk it:** every frame `AddMovementInput(direction, 1, bForce=true)` toward the reported place, the
  movement component's `MaxWalkSpeed` = reported speed + catch-up, and `SetControlRotation` (freakers turn
  with the controller: `bUseControllerRotationYaw`). Farther than 8 m (or 4 m in height): teleport.
- **Result:** the puppet ran beside the real one with the game's run animation (screenshot evidence in the
  session; both pawns in a running pose 10-15 m from the camera). No pose streaming needed: ~35 bytes per
  zombie per update instead of the ~1 KB bone frames the player ghosts use.
- **Attacks / reactions:** the owner reports the `AnimInstance.GetCurrentActiveMontage()` asset path; the
  puppet plays it with `Montage_Play` (prototype; not yet seen in game).

## 3. Prototype architecture (`Zombies=1`)

```
mode (Lua)  createZombie(x,y,z,type)        zombieDeath(z, killer)
   |                                              ^
 server: Zombie {id,type,pos,yaw,speed,health,world,owner,dead}
   |  owner = nearest player within ZombieOwnRange (150 m), same world, alive;
   |  switches only when another player is < 70 % of the owner's distance for 2 s
   |-- Zombie Add/Owner/Died/Remove (reliable) --> all clients
   |-- ZombieMoves (unreliable, <=250 m) ---------> other clients        <-- ZombieStates (owner, 10 Hz)
   |-- Zombie Damage (to the owner) <------------- ZombieHit (a player hurt a puppet)
   |                                                ZombieDied (owner)
 client (src/zombies.cpp):
   within 200 m of the zombie -> SpawnForArcadeMode at its place; owner: brain on, report;
   others: brain off, puppet health 100000, walk after ZombieMoves, play montages;
   a hit on a puppet (health below 100000) -> ZombieHit(damage), health back to 100000;
   owner applies forwarded damage with ApplyDamage; owner sees health <= 0 -> ZombieDied;
   Died -> puppets die the game's way (ApplyDamage), ragdoll/body stay; server forgets it after 30 s.
```

Why ownership by distance: the game's AI only perceives the local player (other players are drawn as
`SkeletalMeshActor` ghosts, not pawns), so a zombie can only hunt the player whose game runs it. Handing the
zombie to the nearest player makes it hunt whoever is closest; when someone else comes clearly closer it
switches (the new owner restarts the behaviour tree with the last reported health).

Protocol (`src/shared/protocol.h`): client->server `ZombieStates = 30`, `ZombieHit = 31`, `ZombieDied = 32`;
server->client `Zombie = 130` (`ZombieOp` Add/Owner/Damage/Died/Remove), `ZombieMoves = 131`.
`ZombieState` = u16 id, f32 x,y,z,yaw,speed,health, u8 montageSeq, str montage. No protocol bump: old
clients ignore the new messages (and get no zombies).

Server checks: only the owner's reports move a zombie; positions/health must be finite; a hit must come
from the zombie's world within 250 m and is capped by `MaxHitDamage`; at most 200 zombies.

### Lua API (prototype)

| Function | |
|---|---|
| `createZombie(x, y, z [, type [, yaw [, world]]])` → `id` or `nil` | `type`: `Swarmer` (default), `Newt`, ... (an `EAIFactionType` the region has loaded); `nil` when `Zombies=0` |
| `removeZombie(id)` → `bool` | gone for everyone (a body too) |
| `getZombie(id)` → table or `nil` | `id, type, x, y, z, yaw, world, health` (once reported), `dead`, `owner` (player) |
| `zombies()` → list | every zombie (dead ones until they are forgotten) |
| event `zombieDeath(zombie, killer)` | `killer`: the player who hurt it last (the owner when nobody else did); nil possible |

Test mode `modes/zz_zombietest` (temporary): `/z [count] [type] [metres]`, `/zclear`, `/zlist`.

## 4. Next steps (design)

1. **Mode-side spawners** (`modes/_lib/zombies.lua`, no engine work): world.json `zombieZones`
   `[{name, x, y, z, radius, type, max, respawn, night}]` placed in the editor like PvP zones; the library keeps
   `max` alive while a player is within ~200 m, respawns after `respawn` s, despawns when nobody is near.
   `createHorde(x, y, z, count, radius [, type])` = a group of zombies spread over the radius sharing an id
   (`hordeDeath` when all died). Count limited by what clients can run: the arcade pool gave us pawns well
   beyond the ambient pool's 15, the practical limit needs measuring (FPS with 30-50 AI freakers).
2. **Attacks on players who are not the owner:** today a puppet's attack is only an animation. Options: make
   the owner's AI target a proxy pawn standing in for the remote player (needs an invisible `BendPawn` of the
   player faction registered in `BendAIManager.m_DetectablePawns`), or let the victim's client judge melee
   hits from the reported attack montage + distance (cheap, less exact).
3. **Who killed it:** the owner's own kill is not reported separately; `killer` is the last remote attacker
   if there was one. Report the owner's last damage causer with `ZombieDied`.
4. **Real hordes** (crowd system): needs the story flag / world state that activates `Ambient_Hordes`; then
   either sync only the horde's state (POI, frenzy target) and let every client simulate the crowd itself, or
   keep hordes single-player-like (one owner runs the crowd, others get promoted members only). Research first.
5. **Types per region:** `Spawn` fails where the faction is not loaded (e.g. no Breaker at Marion Forks).
   Loading archetypes on demand (`AllArchetypes` holds 300 asset references) is needed for any-type-anywhere.
6. **Ambient generator interplay:** with `SuppressAI=1` the generator is blocked below importance 255 and
   force-despawned; the server zombies use its arcade path. If that path turns out blocked by the importance
   gate, lift the gate while `zombies::Active()` and block the population areas instead.

## 5. Dev console (research tools)

| Command | |
|---|---|
| `ai [radius m] [all]` | AI pawns: class, controller, health, distance (hidden pool members with `all`) |
| `sdump <obj> <Prop> [index] [depth]` | fields of a struct property / struct array element (spawn types etc.) |
| `z spawn <faction> [dist m] [chase]` / `z last` | arcade spawn in front of the player; the pawn it produced |
| `z brain <obj> on\|off` | behaviour tree running / stopped |
| `z drive <obj> x y z [yaw]`, `z mirror <src> <puppet> [delay] [side]`, `z release <obj>`, `z list` | puppet tests |
| `z horde <BendCrowdGroup> <faction> [count] [dist]` | arcade spawn into a crowd group |
| `z hp <obj>`, `z kill <obj>` | health, lethal damage |

Screenshots while another window covers the game: the engine's own `shot` command works
(`call cdo:KismetSystemLibrary ExecuteConsoleCommand WorldContextObject=pawn Command=shot SpecificPlayer=controller`,
file in `profiles/editor/BendGame/Saved/Screenshots/WindowsNoEditor`).

## 6. Verification

| What | State |
|---|---|
| Spawn types, pools, spawn areas, horde levels, crowd groups/generators, nests found in the running game | verified |
| `SpawnForArcadeMode` spawns a Swarmer at a chosen point | verified |
| `StopLogic` stops the AI; puppet walk/run with correct animation via movement input | verified (one game, mirror test) |
| Crowd member generated by hand exists but is not drawn (inactive hordes in our save) | verified |
| Server: createZombie, owner choice, relays, hit forwarding, death event | built (compiles); live test pending |
| Client: spawn near player, owner AI / puppet switch, montage relay, hit relay, death | built; live test pending |
| Arcade spawn while `SuppressAI=1` blocks the generator | pending |
| Two real clients | pending (needs the user / two PCs or two game copies) |
