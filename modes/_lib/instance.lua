-- instance.lua — sub-modes inside one server: a duel, an arena, a training ground. The players go into a
-- virtual world of their own with the instance's weapons and health; nothing they had is lost or used up:
-- on the way out everything comes back (place, world, backpack, health). A death inside does not cost
-- anything either (no wounds, no respawn fee: the round goes on or ends). docs/API.md "Инстансы и дуэли".
--
--   local instance = require("instance")
--   local inst = instance.new({
--       name = { ru = "Арена", en = "Arena" },
--       world = "new",                      -- newWorld() while it runs; or a number (a shared arena world)
--       spawns = { {x=, y=, z=, yaw=}, ... }, -- start places (cycled); "here": around the first player
--       loadout = { weapons = { kWeaponID_M9 = 60 }, items = { kItemID_Bandage = 2 } },
--       health = 100, rounds = 3, timeLimit = 180, countdown = 3,
--       onEnd = function(inst, result) end,  -- result: {winner = player or nil, score = {[id] = n}, reason}
--   })
--   inst:join(player); inst:start(); inst:leave(player); inst:finish()
--   instance.of(player)                    -- the instance it is in, or nil
--   instance.duel(a, b, {rounds = 3, loadout = ...})   -- two players, right where they stand
--
-- With interact.lua the player menu gets "Challenge to a duel" (asks the other one) once
-- instance.enableDuels(opts) is called. Snapshots are kept in storage ("instance:<player>") until the
-- player is back: after a crash, a kick or a server restart it gets its things back at the next join.
-- The backpack snapshot is the last one the client sent (up to 2 s old).

local interact = require("interact")
local M = {}
local I = {}
I.__index = I
local byPlayer = {}  -- player id -> instance
local function T(p, r, e) return p and p.lang == "ru" and r or e end
local function title(v, p) if type(v) == "table" then return (p and p.lang == "ru" and v.ru) or v.en or v.ru or "" end return v or "" end
local function key(p) return "instance:" .. (p.steamId and ("s" .. p.steamId) or ("n" .. p.name:lower())) end

function M.of(p) return p and byPlayer[p.id] end

local function snapshot(p)
    local x, y, z, yaw = p:position()
    local inv = {}
    for k, n in pairs(p:inventory() or {}) do if n > 0 then inv[k] = n end end
    local snap = { x = x, y = y, z = z, yaw = yaw, world = p.world, inv = inv, health = p.health, pvp = p.pvp }
    storage.set(key(p), snap)
    return snap
end

local function restore(p, snap)
    p:setWorld(snap.world or 0)
    p:setPosition(snap.x, snap.y, snap.z, snap.yaw)
    p:restoreInventory(snap.inv or {})
    if snap.health and snap.health > 0 then p:setHealth(snap.health) end
    p:setPvp(snap.pvp)
    storage.set(key(p), nil)
end

local function equip(inst, p)
    local lo = inst.opts.loadout or {}
    p:takeWeapons()  -- (its own come back on the way out; the knife, rock and taser stay)
    for weapon, ammo in pairs(lo.weapons or {}) do p:giveWeapon(weapon, ammo, { replace = true }) end
    for item, n in pairs(lo.items or {}) do p:giveItem(item, n) end
    p:heal(inst.opts.health or 100)
end

local function place(inst, p, i)
    local s = inst.spawns[((i - 1) % #inst.spawns) + 1]
    if s then p:setPosition(s.x, s.y, s.z, s.yaw or 0) end
end

local function tellAll(inst, fn) for id in pairs(inst.players) do local p = getPlayer(id); if p then p:message(fn(p)) end end end

function M.new(opts)
    opts = opts or {}
    local inst = setmetatable({ opts = opts, players = {}, order = {}, score = {}, state = "idle", spawns = {}, round = 0 }, I)
    if type(opts.spawns) == "table" then inst.spawns = opts.spawns end
    return inst
end

function I:title(p) return title(self.opts.name or { ru = "Инстанс", en = "Instance" }, p) end

function I:join(p)
    if byPlayer[p.id] then return false end
    if p.downed or p.vehicle then return false end
    if not self.world then
        if self.opts.world == nil or self.opts.world == "new" then self.world = newWorld(); self.ownWorld = true
        else self.world = self.opts.world end
    end
    local snap = snapshot(p)
    self.players[p.id] = { snap = snap }
    self.order[#self.order + 1] = p.id
    self.score[p.id] = 0
    byPlayer[p.id] = self
    if self.opts.spawns == "here" and #self.spawns == 0 then
        -- Around the first player: two places 12 m apart, facing each other.
        local a = math.rad(snap.yaw or 0)
        local cx, cy = snap.x + math.cos(a) * 600, snap.y + math.sin(a) * 600
        self.spawns = { { x = cx - math.cos(a) * 600, y = cy - math.sin(a) * 600, z = snap.z, yaw = snap.yaw },
                        { x = cx + math.cos(a) * 600, y = cy + math.sin(a) * 600, z = snap.z, yaw = (snap.yaw or 0) + 180 } }
    end
    p:setWorld(self.world)
    place(self, p, #self.order)
    equip(self, p)
    p:setPvp(true)
    return true
end

function I:leave(p, reason)
    local e = self.players[p.id]; if not e then return end
    self.players[p.id] = nil; byPlayer[p.id] = nil
    for i, id in ipairs(self.order) do if id == p.id then table.remove(self.order, i) break end end
    p:freeze(false)
    if reason ~= "quit" then restore(p, e.snap) end  -- (a player who quits gets it back at its next join)
    if self.opts.onLeave then pcall(self.opts.onLeave, self, p, reason) end
    if self.state ~= "idle" and self.state ~= "over" then
        local left = 0; for _ in pairs(self.players) do left = left + 1 end
        if left <= 1 then
            local winner
            for id in pairs(self.players) do winner = getPlayer(id) end
            self:finish(winner, reason == "quit" and "quit" or "left")
        end
    end
end

local function startRound(inst)
    inst.round = inst.round + 1
    inst.roundAt = now()
    local i = 0
    for _, id in ipairs(inst.order) do
        local p = getPlayer(id)
        if p then i = i + 1; place(inst, p, i); equip(inst, p); p:freeze(true) end
    end
    local n = inst.opts.countdown or 3
    local run = inst.run
    for k = n, 1, -1 do
        after((n - k) * 1000, function()
            if inst.run ~= run then return end
            for id in pairs(inst.players) do local p = getPlayer(id); if p then p:tip(T(p, "Раунд ", "Round ") .. inst.round .. ": " .. k, 1) end end
        end)
    end
    after(n * 1000, function()
        if inst.run ~= run then return end
        inst.state = "fighting"
        for id in pairs(inst.players) do local p = getPlayer(id); if p then p:freeze(false); p:tip(T(p, "Бой!", "Fight!"), 2) end end
    end)
end

function I:start()
    if self.state ~= "idle" then return end
    self.state = "countdown"; self.run = (self.run or 0) + 1
    startRound(self)
    if self.opts.timeLimit then
        local run = self.run
        after(self.opts.timeLimit * 1000 + (self.opts.countdown or 3) * 1000, function()
            if self.run == run and self.state ~= "over" then self:finish(nil, "time") end
        end)
    end
end

function I:finish(winner, reason)
    if self.state == "over" then return end
    self.state = "over"; self.run = (self.run or 0) + 1
    local result = { winner = winner, score = self.score, reason = reason or "done" }
    for id in pairs(self.players) do
        local p = getPlayer(id)
        if p then
            p:message("{F2D16B}" .. self:title(p) .. ": " .. (winner and (winner == p and T(p, "победа!", "you win!") or (T(p, "победил ", "won by ") .. winner.name))
                or T(p, "ничья", "a draw")))
        end
    end
    local ids = {}; for id in pairs(self.players) do ids[#ids + 1] = id end
    for _, id in ipairs(ids) do local p = getPlayer(id); if p then self:leave(p, "finish") end end
    if self.ownWorld and self.world then freeWorld(self.world) end
    self.world = nil
    if self.opts.onEnd then pcall(self.opts.onEnd, self, result) end
end

-- Deaths inside: no wounds, no fee; the other one gets the round, the best of `rounds` wins.
on("playerDowned", function(p) if byPlayer[p.id] then return false end end)
on("playerDeath", function(p)
    local inst = byPlayer[p.id]; if not inst or inst.state ~= "fighting" then return end
    inst.state = "between"
    local alive = {}
    for _, id in ipairs(inst.order) do if id ~= p.id then alive[#alive + 1] = id end end
    if #alive == 1 then inst.score[alive[1]] = (inst.score[alive[1]] or 0) + 1 end
    local need = math.floor((inst.opts.rounds or 1) / 2) + 1
    for id, n in pairs(inst.score) do
        if n >= need then inst.pendingWinner = id end
    end
    tellAll(inst, function(q)
        local parts = {}
        for _, id in ipairs(inst.order) do local o = getPlayer(id); if o then parts[#parts + 1] = o.name .. " " .. (inst.score[id] or 0) end end
        return "{F2D16B}" .. table.concat(parts, " : ")
    end)
end)
on("playerRespawn", function(p)
    local inst = byPlayer[p.id]; if not inst then return end
    if inst.pendingWinner then
        local w = getPlayer(inst.pendingWinner); inst.pendingWinner = nil
        after(1500, function() inst:finish(w, "score") end)
    else
        after(1500, function() if inst.state == "between" then inst.state = "countdown"; startRound(inst) end end)
    end
    place(inst, p, 1)
    return true
end)
on("playerQuit", function(p) local inst = byPlayer[p.id]; if inst then inst:leave(p, "quit") end end)
-- Back from a crash or a restart while inside: its things back.
on("playerJoin", function(p)
    local snap = storage.get(key(p))
    if snap then after(6000, function() if p.valid and not byPlayer[p.id] then restore(p, snap); p:message(T(p, "Вещи после дуэли возвращены.", "Your things from before the duel are back.")) end end) end
end)

function M.duel(a, b, opts)
    opts = opts or {}
    local inst = M.new({ name = opts.name or { ru = "Дуэль", en = "Duel" }, world = "new", spawns = "here", loadout = opts.loadout,
        health = opts.health or 100, rounds = opts.rounds or 3, timeLimit = opts.timeLimit or 300, countdown = 3, onEnd = opts.onEnd })
    if not inst:join(a) then return nil end
    if not inst:join(b) then inst:leave(a, "finish"); return nil end
    inst:start()
    return inst
end

function M.enableDuels(opts)
    opts = opts or {}
    interact.add({ id = "duel", order = 60,
        label = { ru = "Вызвать на дуэль", en = "Challenge to a duel" },
        when = function(p, t) return not byPlayer[p.id] and not byPlayer[t.id] and not p.vehicle and not t.vehicle and (not opts.allowed or opts.allowed(p, t)) end,
        consent = { ru = "%s вызывает вас на дуэль (вещи не тратятся)", en = "%s challenges you to a duel (nothing of yours is used up)" },
        onUse = function(p, t) M.duel(p, t, opts) end })
end

return M
