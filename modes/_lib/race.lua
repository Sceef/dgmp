-- race.lua — races in a virtual world of their own (docs/API.md "Гонки").
--
--   local Race = require("race")
--   local logic = require("logic")
--   local highway = Race.new({
--       name = { en = "Highway", ru = "Трасса" },
--       grid = logic.points("race_grid"),                                -- start slots (the arrow: the way)
--       checkpoints = logic.triggers("race_checkpoint", { radius = 12 }), -- in order; the last one: the finish
--   })
--   highway:signup(logic.point("race_start"))   -- [F] there: sign up / leave, while it runs: watch it
--   command("race", function(p) highway:join(p) end)
--
-- While a race runs its racers are in a virtual world of their own (newWorld()): they see only each
-- other, nobody else sees them, so they ride where others play without getting in their way. Whoever
-- watches (race:watch) looks into that world from its own. After the race everyone goes back to its
-- world and to where it was. Several races can run at once, each in its own world.
--
-- Options: laps = 1; vehicle = "bike" (the player's bike) or a createVehicle model ("golfcart",
-- "dumptruck", "militarytruck": made for the race and removed after); lobby = 30 (seconds of signing up
-- after the first one, then it starts; 0: only race:start()); minRacers = 1; countdown = 10;
-- finishWindow = 30 (seconds the others have after the winner); maxTime = 900; returnTo = "back"
-- (where it was) or "stay" (at the finish); objects = {createObject options, ...} (made in the race's
-- world while it runs: barriers, arches); radius = 10 (m, for checkpoints without one);
-- onStart(race), onFinish(race, player, place, seconds), onEnd(race, results),
-- onLeave(race, player, reason, back) (reason "left", "death", "moved", "quit"; back: {x, y, z, yaw, world}
-- where it was before the race, nil if it had not started).
-- Race.of(player):entry(player).back is the same while it rides.
--
-- race.state: "idle", "lobby" (signing up), "countdown", "racing"; race.world while it runs;
-- race.last: the results of the last race ({ {player, name, place, time}, ..., dnf = {{player, name}, ...} }).

local Race = {}
Race.__index = Race

local races = {}
local GREEN, YELLOW, GREY, RED = "#7CD67C", "#F2D16B", "#A0A0A0", "#E07070"

local function L(p, en, ru) return p.lang == "ru" and ru or en end
local function title(v, p)
    if type(v) == "table" then return (p and p.lang == "ru" and v.ru) or v.en or v.ru or "" end
    return v or ""
end
local function clock(s) return string.format("%d:%05.2f", math.floor(s / 60), s % 60) end

function Race.new(opts)
    opts = opts or {}
    local race = setmetatable({ opts = opts, name = opts.name or "Race", grid = opts.grid or {}, checkpoints = opts.checkpoints or {},
                                laps = math.max(1, opts.laps or 1), state = "idle", entrants = {}, watchers = {},
                                finished = {}, dropped = {}, made = {}, run = 0 }, Race)
    races[#races + 1] = race
    return race
end

-- The race the player takes part in (signed up or riding), or nil.
function Race.of(player) for _, r in ipairs(races) do if r.entrants[player.id] then return r end end end
function Race.all() return races end

function Race:title(player) return title(self.name, player) end
-- The player's entry: {player, slot, inRace, next, done, place, time, back} or nil.
function Race:entry(player) return self.entrants[player.id] end

-- Everyone it concerns: its entrants and watchers. "{race}" in the text: its name in the player's language.
function Race:tell(en, ru, color)
    local sent = {}
    local function send(p)
        if not p.valid or sent[p.id] then return end
        sent[p.id] = true
        p:message((L(p, en, ru):gsub("{race}", function() return self:title(p) end)), color)
    end
    for _, e in pairs(self.entrants) do send(e.player) end
    for _, w in pairs(self.watchers) do send(w.player) end
end

local function sorted(race)
    local list = {}
    for _, e in pairs(race.entrants) do list[#list + 1] = e end
    table.sort(list, function(a, b) return a.joinedAt < b.joinedAt end)
    return list
end
local function count(t) local n = 0; for _ in pairs(t) do n = n + 1 end; return n end

-- Players riding it now (in its world).
function Race:racers()
    local list = {}
    for _, e in ipairs(sorted(self)) do if e.inRace and e.player.valid then list[#list + 1] = e.player end end
    return list
end
local function racerIds(race)
    local ids = {}
    for _, p in ipairs(race:racers()) do ids[#ids + 1] = p.id end
    return ids
end
local function refreshWatchers(race)
    if not race.world then return end
    local ids = racerIds(race)
    for _, w in pairs(race.watchers) do if w.player.valid then w.player:spectate(true, ids, { world = race.world }) end end
end

local function inside(cp, player, radius)
    local x, y, z = player:position()
    local r = (cp.radius or radius or 10) * 100
    local h = (cp.height or math.max(cp.radius or radius or 10, 5)) * 100
    local dx, dy = x - cp.x, y - cp.y
    return dx * dx + dy * dy <= r * r and math.abs(z - cp.z) <= h + 100   -- z of the player: its middle
end

-- The marker and the task panel of the checkpoint it rides to.
local function showNext(race, e)
    local p = e.player
    if e.marker then removeMarker(e.marker); e.marker = nil end
    local cp = race.route[e.next]
    if not cp then return end
    local total, last = #race.route, e.next == #race.route
    local per = #race.checkpoints
    local lap, at = math.floor((e.next - 1) / per) + 1, (e.next - 1) % per + 1
    e.marker = createMarker({ x = cp.x, y = cp.y, z = cp.z, radius = cp.radius or race.opts.radius or 10, track = false,
                              player = p, world = race.world, route = true, category = "task", style = last and "defendarea" or "basic",
                              title = last and L(p, "Finish", "Финиш") or (L(p, "Checkpoint ", "Чекпойнт ") .. at) })
    local text = last and L(p, "Ride to the finish!", "К финишу!") or string.format(L(p, "Checkpoint %d of %d", "Чекпойнт %d из %d"), at, per)
    if race.laps > 1 then text = string.format(L(p, "Lap %d of %d · ", "Круг %d из %d · "), lap, race.laps) .. text end
    p:objective({ state = "update", category = "task", title = race:title(p), text = text })
    p:objectiveMeter(L(p, "Progress", "Прогресс"), (e.next - 1) / total)
end

local function freeSlot(race)
    local used = {}
    for _, e in pairs(race.entrants) do if e.inRace then used[e.slot] = true end end
    for i = 1, #race.grid do if not used[i] then return i end end
end

-- Into the race's world, onto its grid slot, held there until the start.
local function place(race, e, slot)
    local p, g = e.player, race.grid[slot]
    local w = race.watchers[p.id]
    if w then race:unwatch(p, true) end
    local x, y, z, yaw = p:position()
    e.back = w and w.back or { x = x, y = y, z = z, yaw = yaw }
    e.back.world = p.world
    e.slot, e.inRace, e.next, e.done = slot, true, 1, false
    e.moving = true; p:setWorld(race.world); e.moving = false
    local kind = race.opts.vehicle or "bike"
    if kind == "bike" then
        e.hadBike = p.hasBike
        p:putOnBike(g.x, g.y, g.z, g.yaw)
    else
        e.vehicle = createVehicle({ model = kind, x = g.x, y = g.y, z = g.z, yaw = g.yaw, world = race.world, locked = true, owner = p.name, blip = "none" })
        p:putInVehicle(e.vehicle)
    end
    p:freeze(true)
    p:objective({ state = "new", category = "task", title = race:title(p), text = L(p, "The race starts soon", "Скоро старт") })
end

-- Out of the race's world, back where it came from (reason: "end", "left", "death", "moved", "quit").
local function release(race, e, reason)
    local p = e.player
    if e.marker then removeMarker(e.marker); e.marker = nil end
    if e.vehicle then removeVehicle(e.vehicle); e.vehicle = nil end
    e.inRace = false
    if reason == "quit" or not p.valid then return end
    p:freeze(false)
    p:objective({ state = "remove" }); p:objectiveMeter()
    if e.hadBike == false then
        if reason ~= "death" then p:takeBike()
        else after(4000, function() if p.valid and not Race.of(p) then p:takeBike() end end) end  -- after the respawn
    end
    local b = e.back
    if b and p.world == race.world then e.moving = true; p:setWorld(b.world); e.moving = false end
    if b and (reason == "end" or reason == "left") and (race.opts.returnTo or "back") == "back" then p:setPosition(b.x, b.y, b.z + 20, b.yaw) end
end

-- Signs the player up. Returns true, or false and why ("already", "other", "running", "full", "notready").
function Race:join(p)
    local function no(why, en, ru) if p.valid then p:message(L(p, en, ru), GREY) end; return false, why end
    if p.bot then return false, "bot" end
    local other = Race.of(p)
    if other == self then return no("already", "You are signed up already.", "Вы уже записаны.") end
    if other then return no("other", "You are in another race: " .. other:title(p), "Вы уже в другой гонке: " .. other:title(p)) end
    if self.state == "racing" then return no("running", "The race is on: wait for the next one.", "Гонка уже идёт: ждите следующую.") end
    if #self.grid == 0 or #self.checkpoints == 0 then return no("notready", "This race has no track yet.", "У этой гонки ещё нет трассы.") end
    if count(self.entrants) >= #self.grid then return no("full", "The start grid is full.", "Стартовая решётка заполнена.") end
    local e = { player = p, joinedAt = now(), next = 1 }
    self.entrants[p.id] = e
    if self.state == "idle" then
        self.state = "lobby"
        local wait = self.opts.lobby or 30
        self.lobbyEnds = wait > 0 and now() + wait or nil
    end
    self:tell(p.name .. " signed up for the race", p.name .. " записался на гонку", GREY)
    if self.state == "countdown" then
        place(self, e, freeSlot(self))
        refreshWatchers(self)
    elseif self.lobbyEnds then
        local left = math.max(0, math.ceil(self.lobbyEnds - now()))
        p:message(string.format(L(p, "%s: the start in %d s (%d signed up).", "%s: старт через %d с (записалось: %d)."), self:title(p), left, count(self.entrants)), YELLOW)
    end
    return true
end

-- Out of the race: before the start it signs off, riding it is out of the results.
function Race:leave(p, reason)
    local e = self.entrants[p.id]
    if not e then return false end
    reason = reason or "left"
    local wasIn = e.inRace
    if wasIn then release(self, e, reason) end
    self.entrants[p.id] = nil
    if wasIn and not e.done then self.dropped[#self.dropped + 1] = e end
    if self.opts.onLeave then self.opts.onLeave(self, p, reason, e.back) end
    if reason ~= "end" then self:tell(p.name .. " left the race", p.name .. " выбыл из гонки", GREY) end
    if p.valid and reason == "left" then p:message(L(p, "You left the race.", "Вы вышли из гонки."), GREY) end
    if self.state == "lobby" and count(self.entrants) == 0 then self.state, self.lobbyEnds = "idle", nil end
    if self.state == "countdown" or self.state == "racing" then
        local riding = 0
        for _, o in pairs(self.entrants) do if o.inRace and not o.done then riding = riding + 1 end end
        if riding == 0 then self:stop() else refreshWatchers(self) end
    end
    return true
end

-- Starts the countdown now (with whoever signed up).
function Race:start()
    if self.state ~= "lobby" then return false end
    local list, need = sorted(self), self.opts.minRacers or 1
    if #list < need then
        self:tell(string.format("{race}: not enough racers (%d of %d), waiting for more.", #list, need),
                  string.format("{race}: мало гонщиков (%d из %d), ждём ещё.", #list, need), YELLOW)
        local wait = self.opts.lobby or 30
        self.lobbyEnds = wait > 0 and now() + wait or nil
        return false
    end
    self.world = newWorld()
    self.state, self.finished, self.dropped, self.deadline, self.lastLeft = "countdown", {}, {}, nil, nil
    self.route = {}
    for _ = 1, self.laps do for _, cp in ipairs(self.checkpoints) do self.route[#self.route + 1] = cp end end
    self.made = {}
    for _, o in ipairs(self.opts.objects or {}) do
        local copy = {}
        for k, v in pairs(o) do copy[k] = v end
        copy.world = self.world
        self.made[#self.made + 1] = createObject(copy)
    end
    for i, e in ipairs(list) do place(self, e, i) end
    self.run = self.run + 1
    local run, n = self.run, self.opts.countdown or 10
    local lead = 3   -- the world loads around the grid first
    for i = n, 1, -1 do
        after((lead + n - i) * 1000, function()
            if self.run ~= run or self.state ~= "countdown" then return end
            for _, p in ipairs(self:racers()) do p:tip(tostring(i), 1) end
        end)
    end
    after((lead + n) * 1000, function()
        if self.run ~= run or self.state ~= "countdown" then return end
        self.state, self.startedAt = "racing", now()
        for _, e in pairs(self.entrants) do
            if e.inRace then e.player:freeze(false); e.player:tip(L(e.player, "GO!", "СТАРТ!"), 2); showNext(self, e) end
        end
        refreshWatchers(self)
    end)
    self:tell("{race}: to the start!", "{race}: на старт!", GREEN)
    if self.opts.onStart then self.opts.onStart(self) end
    return true
end

-- Ends it now: results so far, everyone back, its world freed. Signing up (lobby) is called off.
function Race:stop()
    if self.state == "idle" then return end
    if self.state == "lobby" then
        self:tell("{race}: called off", "{race}: отменена", GREY)
        self.entrants, self.state, self.lobbyEnds = {}, "idle", nil
        return
    end
    self.run = self.run + 1
    local results = { dnf = {} }
    for i, e in ipairs(self.finished) do results[i] = { player = e.player, name = e.player.name, place = i, time = e.time } end
    for _, e in pairs(self.entrants) do if e.inRace and not e.done then results.dnf[#results.dnf + 1] = { player = e.player, name = e.player.name } end end
    for _, e in ipairs(self.dropped) do results.dnf[#results.dnf + 1] = { player = e.player, name = e.player.name } end
    local lines = {}
    for i, r in ipairs(results) do lines[#lines + 1] = string.format("%d. %s %s", i, r.name, clock(r.time)) end
    for _, r in ipairs(results.dnf) do lines[#lines + 1] = r.name .. " DNF" end
    local summary = (#lines > 0 and table.concat(lines, "   ") or "-"):gsub("{race}", "")
    self:tell("{race} is over: " .. summary, "{race} окончена: " .. summary, YELLOW)
    for _, w in pairs(self.watchers) do self:unwatch(w.player) end
    for _, e in pairs(self.entrants) do if e.inRace then release(self, e, "end") end end
    for _, id in ipairs(self.made) do removeObject(id) end
    freeWorld(self.world)
    self.world, self.made, self.entrants, self.state, self.deadline = nil, {}, {}, "idle", nil
    self.last = results
    if self.opts.onEnd then self.opts.onEnd(self, results) end
end

-- Watching it from outside (while it runs): hidden, its own world kept, the race's world in view.
function Race:watch(p)
    if self.state ~= "countdown" and self.state ~= "racing" then return false end
    local e = self.entrants[p.id]
    if e and e.inRace then return false end
    if not self.watchers[p.id] then
        local x, y, z, yaw = p:position()
        self.watchers[p.id] = { player = p, back = { x = x, y = y, z = z, yaw = yaw } }
        p:message(L(p, "Watching the race: the arrows switch racers.", "Наблюдение за гонкой: стрелки переключают гонщиков."), YELLOW)
    end
    p:spectate(true, racerIds(self), { world = self.world })
    return true
end
function Race:unwatch(p, stay)
    local w = self.watchers[p.id]
    if not w then return false end
    self.watchers[p.id] = nil
    if not p.valid then return true end
    p:spectate(false)
    if not stay then p:setPosition(w.back.x, w.back.y, w.back.z + 20, w.back.yaw) end  -- the camera left its body at the track
    return true
end
function Race:watching(p) return self.watchers[p.id] ~= nil end

-- A point where [F] signs up / leaves, and watches it while it runs (interaction + map icon).
-- opts: text, radius = 3, world = 0, blip = true, icon, color.
function Race:signup(at, opts)
    if not at or at.placed == false then return nil end
    opts = opts or {}
    local race = self
    self.signupAt = createInteraction({ x = at.x, y = at.y, z = at.z, radius = opts.radius or 3, world = opts.world or 0,
        text = opts.text or ("Race: " .. title(self.name)),
        onUse = function(p)
            if race.entrants[p.id] then race:leave(p) return end
            if race.watchers[p.id] then race:unwatch(p) return end
            if race.state == "racing" then race:watch(p) return end
            race:join(p)
        end })
    if opts.blip ~= false then createBlip({ x = at.x, y = at.y, z = at.z, icon = opts.icon, color = opts.color or YELLOW, world = opts.world or 0 }) end
    return self.signupAt
end

-- ---------------------------------------------------------------- per tick
local function reached(race, e)
    local p = e.player
    if e.next < #race.route then
        local per = #race.checkpoints
        p:tip(string.format(L(p, "Checkpoint %d / %d", "Чекпойнт %d / %d"), (e.next - 1) % per + 1, per), 2)
        e.next = e.next + 1
        showNext(race, e)
        return
    end
    e.done, e.time = true, now() - race.startedAt
    race.finished[#race.finished + 1] = e
    e.place = #race.finished
    if e.marker then removeMarker(e.marker); e.marker = nil end
    p:objective({ state = "update", category = "task", title = race:title(p),
                  text = string.format(L(p, "Finished %d. with %s", "Финиш: место %d, %s"), e.place, clock(e.time)) })
    p:objectiveMeter(L(p, "Progress", "Прогресс"), 1)
    race:tell(string.format("%s finished %d. in %s", p.name, e.place, clock(e.time)),
              string.format("%s финишировал %d-м: %s", p.name, e.place, clock(e.time)), GREEN)
    if race.opts.onFinish then race.opts.onFinish(race, p, e.place, e.time) end
    if not race.deadline then
        local window = race.opts.finishWindow or 30
        race.deadline = now() + window
        race:tell(string.format("%d seconds left to finish!", window), string.format("До конца гонки %d с!", window), YELLOW)
    end
end

function Race:tick()
    if self.state == "lobby" then
        if self.lobbyEnds and now() >= self.lobbyEnds then self:start() end
        return
    end
    if self.state ~= "racing" then return end
    local riding = 0
    for _, e in pairs(self.entrants) do
        if e.inRace and not e.done and e.player.valid then
            local cp = self.route[e.next]
            if cp and inside(cp, e.player, self.opts.radius) then reached(self, e) end
            if not e.done then riding = riding + 1 end
        end
    end
    if riding == 0 then return self:stop() end
    local t = now()
    if self.deadline then
        local left = math.ceil(self.deadline - t)
        if left ~= self.lastLeft and left > 0 and left <= 10 then
            self.lastLeft = left
            for _, e in pairs(self.entrants) do if e.inRace and not e.done then e.player:tip(tostring(left), 1) end end
        end
        if t >= self.deadline then return self:stop() end
    end
    if t - self.startedAt >= (self.opts.maxTime or 900) then self:stop() end
end

local acc = 0
on("tick", function(dt)
    acc = acc + dt
    if acc < 0.1 then return end
    acc = 0
    for _, race in ipairs(races) do race:tick() end
end)

on("playerQuit", function(p)
    for _, race in ipairs(races) do
        race.watchers[p.id] = nil
        if race.entrants[p.id] then race:leave(p, "quit") end
    end
end)
on("playerDeath", function(p)
    local race = Race.of(p)
    if race and race.entrants[p.id].inRace then race:leave(p, "death") end
end)
-- Taken out of the race's world by someone else (an admin's /goto, a door): out of the race.
on("playerWorld", function(p, world, old)
    for _, race in ipairs(races) do
        local e = race.entrants[p.id]
        if e and e.inRace and not e.moving and old == race.world then race:leave(p, "moved") end
    end
end)

return Race
