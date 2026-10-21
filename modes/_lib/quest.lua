-- quest.lua — quests made of steps, shown with the game's own objective HUD and quest markers.
--
--   local Quest = require("quest")
--   local q = Quest.start(player, { title = "Припасы для лагеря", category = "camp" })
--   q:step("Доберитесь до заправки", { x = ..., y = ..., z = ..., radius = 5,
--          onEnter = function(q) q:step("Обыщите здание") end })
--   q:hold("Обыщите место", { x = ..., y = ..., z = ..., radius = 6, seconds = 10,
--          onDone = function(q, player) q:complete(150) end })   -- meter fills only inside the zone
--   q:deliver("Отнесите ящики", { pickup = {...}, dropoff = {...}, count = 3, onDone = ... })  -- boxes to carry
--   q:talk("Поговорите с Авой", { actor = "shopkeeper", speaker = "Ava", lines = { "Привет.", "Есть работа." }, onDone = ... })
--   q:complete()        -- or q:fail()
--   Quest.runJob(Quest.load("loader"))   -- a repeatable job from a file (see "jobs" below)
--
-- Categories: story, camp, infestation, ambush, nero, horde, campmission, task.
-- Styles (marker / objective look): basic, kill, killarea, defend, defendarea, search, track, nokill.

local Quest = {}
Quest.__index = Quest

-- Texts can be plain strings or per language: { en = "Supplies", ru = "Припасы" }. The player's
-- language (player.lang, chosen in the launcher) is used, else English, else any.
function Quest.text(value, player)
    if type(value) ~= "table" then return value end
    local lang = player and player.valid and player.lang or "en"
    if value[lang] and value[lang] ~= "" then return value[lang] end
    if value.en and value.en ~= "" then return value.en end
    for _, v in pairs(value) do if v ~= "" then return v end end
    return ""
end

local panelOf = {}  -- player id -> the quest that owns the objective panel (the newest one)

function Quest.start(player, opts)
    opts = opts or {}
    local q = setmetatable({
        player = player,
        title = opts.title or "",
        category = opts.category or "story",
        style = opts.style or "basic",
        marker = nil,
        active = true,
    }, Quest)
    panelOf[player.id] = q
    player:objective({ title = q.title, text = opts.text or "", state = "new", category = q.category, style = q.style })
    return q
end

-- Removes the current step's marker, if any.
function Quest:clearMarker()
    if self.marker then removeMarker(self.marker); self.marker = nil end
end

-- Ends whatever the current step set up (marker, zone meter, counters, boxes).
function Quest:clearSteps()
    self:clearMarker()
    self:clearHold()
    self:clearCollect()
    self:clearDeliver()
end

-- Shows a new step. `target` (optional): {x, y, z, radius = 3 (m), route = true, style,
-- onEnter = function(quest, player) ... end} puts a quest marker with a road route there.
function Quest:step(text, target)
    if not self.active or not self.player.valid then return end
    self:clearSteps()
    self.player:objective({ title = self.title, text = text, state = "update", category = self.category, style = (target and target.style) or self.style })
    if target then
        local quest = self
        self.marker = createMarker({
            x = target.x, y = target.y, z = target.z, radius = target.radius or 3,
            category = self.category, style = target.style or self.style,
            route = target.route ~= false, player = self.player,
            title = self.title, description = text,
            track = target.onEnter ~= nil,
            onEnter = target.onEnter and function(player) if quest.active then target.onEnter(quest, player) end end,
        })
    end
end

-- A conversation: the player comes to the NPC (or the point) and its lines show one after another in a
-- dialog ("Next", the last one "OK"; Esc skips the rest). spec: {actor = "id" (an NPC placed in the editor,
-- world.json "actors") or x, y, z; radius = 3 (m); speaker = "Ava" (the dialog's title); lines = {"...", ...};
-- route = true; onDone = function(quest, player) end}.
function Quest:talk(text, spec)
    if not self.active or not self.player.valid then return end
    local at = spec
    if spec.actor then
        local a = getActor(spec.actor)
        if a then at = { x = a.x, y = a.y, z = a.z } else log("quest talk: no NPC \"" .. tostring(spec.actor) .. "\" in world.json") end
    end
    local quest, lines = self, spec.lines or {}
    local function finish(player) if quest.active and spec.onDone then spec.onDone(quest, player) end end
    local function say(player, i)
        if not quest.active or not player.valid then return end
        if i > #lines then return finish(player) end
        local last = i == #lines
        player:dialog({
            style = "msgbox", title = spec.speaker or "", text = lines[i],
            button1 = last and "OK" or (player.lang == "ru" and "Далее" or "Next"),
            onResponse = function(p, ok) if ok then say(p, i + 1) else finish(p) end end,
        })
    end
    local talking = false
    self:step(text, {
        x = at.x, y = at.y, z = at.z, radius = spec.radius or 3, route = spec.route, style = spec.style,
        onEnter = function(q, player)
            if talking then return end
            talking = true
            q:clearMarker()
            say(player, 1)
        end,
    })
end

-- A step that fills a meter only while the player stays in the zone ("search the place",
-- "defend the area"). target: {x, y, z, radius = 6 (m), seconds = 10, meter = "Поиск",
-- leaveTip = "...", style = "search", onDone = function(quest, player) end}.
function Quest:hold(text, target)
    if not self.active or not self.player.valid then return end
    self:clearSteps()
    local quest, player = self, self.player
    local seconds = target.seconds or 10
    local meter = target.meter or Quest.text({ en = "Searching", ru = "Поиск" }, player)
    local hold = { inside = false, progress = 0 }
    self.holdState = hold
    player:objective({ title = self.title, text = text, state = "update", category = self.category, style = target.style or "search" })
    self.marker = createMarker({
        x = target.x, y = target.y, z = target.z, radius = target.radius or 6,
        category = self.category, style = target.style or "search", route = false, player = player,
        title = self.title, description = text, track = true,
        onEnter = function() hold.inside = true end,
        onLeave = function()
            hold.inside = false
            if quest.active and quest.holdState == hold then
                player:tip(target.leaveTip or "Вернитесь в зону, чтобы продолжить", 3)
            end
        end,
    })
    local step = 0.25
    hold.timer = every(math.floor(step * 1000), function()
        if not quest.active or quest.holdState ~= hold or not player.valid then cancelTimer(hold.timer); return end
        if not hold.inside then return end
        hold.progress = math.min(1, hold.progress + step / seconds)
        player:objectiveMeter(meter, hold.progress)
        if hold.progress >= 1 then
            quest:clearHold()
            quest:clearMarker()
            player:objectiveMeter()
            if target.onDone then target.onDone(quest, player) end
        end
    end)
    player:objectiveMeter(meter, 0)
end

function Quest:clearHold()
    if self.holdState then
        if self.holdState.timer then cancelTimer(self.holdState.timer) end
        self.holdState = nil
    end
end

-- A step done by doing something `count` times (game events, see docs/API.md "gameEvent"):
--   event = "pickup" | "carry" | "search" | "body" | "interact" | "door" | "explosion" | "fire" (or a list of them)
--   item  = item key or a list of keys (pickup), class = part of the object class name,
--   ids   = list of world object names (ev.id): only these objects count, e.g. two given car trunks,
--   spawn = { {item = "kWeaponID_Molotov_Small", count = 1, x = .., y = .., z = ..}, ... }: the step
--           places these pickups for the player; only they count (count defaults to how many) and
--           whatever was not taken disappears when the step or the quest ends,
--   area  = {x, y, z, radius (m)}: only events there count; marker = true shows a quest marker
--           over the area (route = true for a road route to it),
--   count = 1, onDone = function(quest, player) end.
-- The objective line shows "(done/count)" when count > 1. The same object counts once.
--   q:collect("Обыщите багажники", { event = "search", count = 3, onDone = ... })
--   q:collect("Возьмите коктейль Молотова", { event = "pickup", item = "kConsumableID_Molotov" })
--   q:collect("Подожгите сарай", { event = "fire", area = { x = .., y = .., z = .., radius = 15 }, marker = true })
local collecting = {}  -- player id -> quest with an active collect step
function Quest:collect(text, spec)
    if not self.active or not self.player.valid then return end
    self:clearSteps()
    local spawned, ids, markers, markerOf = {}, nil, {}, {}
    if spec.spawn then
        ids = {}
        for _, s in ipairs(spec.spawn) do
            local id = spawnItem({ item = s.item, count = s.count or 1, x = s.x, y = s.y, z = s.z, yaw = s.yaw, player = self.player })
            spawned[#spawned + 1] = id
            ids[#ids + 1] = "dgmp_item_" .. id
            -- A quest marker over each item (removed when it is taken or the step ends).
            markers[#markers + 1] = createMarker({ x = s.x, y = s.y, z = s.z, radius = 2, category = self.category, style = self.style,
                                                   player = self.player, title = self.title, description = text, track = false })
            markerOf["dgmp_item_" .. id] = markers[#markers]
        end
    end
    local count = spec.count or (spec.spawn and #spec.spawn) or 1
    local state = { spec = spec, text = text, count = count, done = 0, seen = {}, spawned = spawned, ids = ids, markers = markers, markerOf = markerOf }
    self.collectState = state
    collecting[self.player.id] = self
    self:showCollect()
    if spec.area and spec.marker then
        self.marker = createMarker({
            x = spec.area.x, y = spec.area.y, z = spec.area.z, radius = spec.area.radius or 10,
            category = self.category, style = spec.style or self.style, route = spec.route == true,
            player = self.player, title = self.title, description = text, track = false,
        })
    end
end

function Quest:showCollect()
    local c = self.collectState
    local line = c.count > 1 and string.format("%s (%d/%d)", c.text, c.done, c.count) or c.text
    self.player:objective({ title = self.title, text = line, state = "update", category = self.category, style = c.spec.style or self.style })
end

local function oneOf(want, value)  -- want: a value or a list of values
    if type(want) ~= "table" then return want == value end
    for _, v in ipairs(want) do if v == value then return true end end
    return false
end

local function matches(spec, ev, spawnedIds)
    if spec.event and not oneOf(spec.event, ev.type) then return false end
    if spec.item and not oneOf(spec.item, ev.item) then return false end
    if spec.class and not string.find(ev.class or "", spec.class, 1, true) then return false end
    if spec.ids and not oneOf(spec.ids, ev.id) then return false end
    if spawnedIds and not oneOf(spawnedIds, ev.id) then return false end
    if spec.area then
        local a = spec.area
        local dx, dy, dz = ev.x - a.x, ev.y - a.y, ev.z - (a.z or ev.z)
        local r = (a.radius or 10) * 100
        if dx * dx + dy * dy + dz * dz > r * r then return false end
    end
    return true
end

on("gameEvent", function(player, ev)
    local q = collecting[player.id]
    if not q or not q.active or not q.collectState or q.player.id ~= player.id then return end
    local c = q.collectState
    local key = ev.type .. ":" .. (ev.id ~= "" and ev.id or string.format("%d,%d", ev.x // 100, ev.y // 100))
    if c.seen[key] or not matches(c.spec, ev, c.ids) then return end
    c.seen[key] = true
    c.done = c.done + 1
    if c.markerOf and c.markerOf[ev.id] then removeMarker(c.markerOf[ev.id]); c.markerOf[ev.id] = nil end
    if c.done >= c.count then
        q:clearCollect()
        q:clearMarker()
        if c.spec.onDone then c.spec.onDone(q, player) end
    else
        q:showCollect()
    end
end)
on("playerQuit", function(player) collecting[player.id] = nil end)

function Quest:clearCollect()
    if self.collectState then
        for _, id in ipairs(self.collectState.spawned or {}) do despawnItem(id) end
        for _, id in ipairs(self.collectState.markers or {}) do removeMarker(id) end
        self.collectState = nil
        if collecting[self.player.id] == self then collecting[self.player.id] = nil end
    end
end

-- ---------------------------------------------------------------- boxes to carry (deliver steps)
-- A pool: the boxes of a deliver step and the spots at the pick-up area they go back to.
--   pickup = {x, y, z, radius = 3 (m), boxes = 4, model = "crate", roll = "keep", tag}
-- With `tag` and boxes of that tag in the mode's world (editor: World > Carrying) those boxes are used
-- and go back to where the editor put them; otherwise `boxes` new ones are laid out around the point.
-- allowed(player) says who may pick them up; denyTip (a text, or function(player) -> text) is shown
-- to the others.
local pools = {}       -- carry id -> its pool
local poolList = {}    -- pool -> true
local delivering = {}  -- player id -> quest with an active deliver step

local Pool = {}
Pool.__index = Pool

local function dist2(a, b) local dx, dy = a.x - b.x, a.y - b.y; return dx * dx + dy * dy end
local function inZone(at, zone, defaultRadius)
    if not zone or not zone.x or not zone.y then return false end
    local r = (zone.radius or defaultRadius) * 100
    return dist2(at, zone) <= r * r and math.abs(at.z - (zone.z or at.z)) <= math.max(r, 300)
end

function Quest.pool(pickup, allowed, denyTip)
    local p = pickup or {}
    local pool = setmetatable({ pickup = p, spots = {}, ids = {}, own = {}, allowed = allowed, denyTip = denyTip, strayed = {} }, Pool)
    poolList[pool] = true
    if p.tag and p.tag ~= "" then
        for _, c in ipairs(carries(p.tag)) do
            if not pools[c.id] and not c.carrier then
                pool.spots[#pool.spots + 1] = { x = c.x, y = c.y, z = c.z, yaw = c.yaw }
                pool.ids[c.id] = true; pools[c.id] = pool
            end
        end
    end
    if next(pool.ids) then return pool end
    if not p.x or not p.y then
        log("deliver: the pick-up area has no point (and no boxes of tag \"" .. tostring(p.tag) .. "\" in the mode's world)")
        return pool
    end
    local n = math.max(1, math.floor(p.boxes or 4))
    local ring = n == 1 and 0 or math.max(80, math.min(300, (p.radius or 3) * 50), n * 25)
    for i = 1, n do
        local a = (i - 1) / n * 2 * math.pi
        local s = { x = p.x + math.cos(a) * ring, y = p.y + math.sin(a) * ring, z = p.z or 0 }
        pool.spots[i] = s
        local id = createCarry({ model = p.model or "crate", x = s.x, y = s.y, z = s.z, yaw = math.random(0, 359),
                                 roll = p.roll or "keep", tag = (p.tag and p.tag ~= "") and p.tag or "deliver" })
        pool.ids[id] = true; pool.own[id] = true; pools[id] = pool
    end
    return pool
end

-- Puts a box of the pool back on a free spot of the pick-up area (out of the hands too).
function Pool:home(id)
    if not self.ids[id] or #self.spots == 0 then return end
    local lying = {}
    for other in pairs(self.ids) do
        if other ~= id then local c = getCarry(other); if c and not c.carrier then lying[#lying + 1] = c end end
    end
    local n = #self.spots
    local first = math.random(1, n)
    local spot = self.spots[first]
    for k = 0, n - 1 do
        local s = self.spots[(first + k - 1) % n + 1]
        local free = true
        for _, c in ipairs(lying) do if dist2(c, s) < 60 * 60 then free = false; break end end
        if free then spot = s; break end
    end
    self.strayed[id] = nil
    setCarry(id, { x = spot.x, y = spot.y, z = spot.z, yaw = spot.yaw or math.random(0, 359) })
end

-- The pool's own boxes go, the editor's go back to their places.
function Pool:free()
    for id in pairs(self.ids) do
        if self.own[id] then removeCarry(id) else self:home(id) end
        pools[id] = nil
    end
    self.ids, self.own = {}, {}
    poolList[self] = nil
end

-- A box left lying away from the pick-up area (dropped on the way, its carrier left) goes back after a minute.
every(10000, function()
    local t = now()
    for pool in pairs(poolList) do
        for id in pairs(pool.ids) do
            local c = getCarry(id)
            if not c then pool.ids[id] = nil; pools[id] = nil   -- removed by the mode
            elseif c.carrier then pool.strayed[id] = nil
            else
                local home = false
                for _, s in ipairs(pool.spots) do if dist2(c, s) < 300 * 300 then home = true; break end end
                if home then pool.strayed[id] = nil
                else
                    pool.strayed[id] = pool.strayed[id] or t
                    if t - pool.strayed[id] > 60 then pool:home(id) end
                end
            end
        end
    end
end)

on("carryPick", function(player, id)
    local pool = pools[id]
    if pool and pool.allowed and not pool.allowed(player) then
        local tip = type(pool.denyTip) == "function" and pool.denyTip(player) or Quest.text(pool.denyTip, player)
        if tip and tip ~= "" then player:tip(tip, 4) end
        return false
    end
end)

on("carryDrop", function(player, id, info)
    local pool = pools[id]
    if not pool then return end
    local q = delivering[player.id]
    local d = q and q.active and q.deliverState
    if info.reason == "put" then
        if d and d.pool == pool and inZone(info, d.dropoff, 4) then
            pool:home(id)            -- delivered: it leaves the hands, and a box lies at the pick-up area again
            q:deliveredBox()
        end
    elseif info.reason == "break" then
        pool:home(id)                -- (moved, so the server does not remove it)
        if d and d.pool == pool then
            player:tip(Quest.text({ en = "The box broke! Take another one at the pick-up area", ru = "Ящик разбит! Возьмите другой на месте погрузки" }, player), 4)
        end
    elseif info.reason == "quit" or info.reason == "death" then
        pool:home(id)
    end
end)

-- A step done by carrying boxes (docs/API.md "Переноска"): `count` boxes put down (hold F) in the drop-off area.
--   spec = { pickup = {x, y, z, radius = 3, boxes = 4, model = "crate", roll = "keep", tag, name},
--            dropoff = {x, y, z, radius = 4, name}, count = 1, route = true, style,
--            pool = Quest.pool(...), onBox = function(quest, player, done) end, onDone = function(quest, player) end }
-- Without `pool` the step lays out boxes of its own: only this player may take them, and they go when
-- the step ends. A delivered box goes back to the pick-up area. The objective line shows "(done/count)".
--   q:deliver("Отнесите ящики к грузовику", { pickup = { x = .., y = .., z = .., boxes = 4, roll = "break" },
--             dropoff = { x = .., y = .., z = .., radius = 4 }, count = 3, onDone = function(q) q:complete(100) end })
function Quest:deliver(text, spec)
    if not self.active or not self.player.valid then return end
    local again = self.deliverState
    -- The same step once more (spec.step: the step of a file, as runStep passes it): its markers stay.
    if again and again.finished and spec.step and again.spec.step == spec.step and (spec.pool == nil or spec.pool == again.pool) then
        again.done, again.finished, again.spec, again.text = 0, false, spec, text
        self:showDeliver()
        return
    end
    self:clearSteps()
    local quest, player = self, self.player
    local pool, own = spec.pool, false
    if not pool then
        pool = Quest.pool(spec.pickup, function(p) return p.id == player.id end,
                          { en = "These boxes are someone else's task", ru = "Эти ящики — чужое задание" })
        own = true
    end
    local d = { pool = pool, own = own, spec = spec, text = text, count = math.max(1, math.floor(spec.count or 1)), done = 0,
                dropoff = spec.dropoff, markers = {} }
    self.deliverState = d
    delivering[player.id] = self
    local L = function(v) return Quest.text(v, player) end
    local p = spec.pickup or {}
    local at = (p.x and p.y) and p or pool.spots[1]
    if at then
        d.markers[#d.markers + 1] = createMarker({ x = at.x, y = at.y, z = at.z, radius = p.radius or 3, category = self.category, style = "search",
                                                   route = false, player = player, track = false, description = text,
                                                   title = L(p.name or { en = "Pick-up", ru = "Погрузка" }) })
    end
    local drop = spec.dropoff
    if drop and drop.x and drop.y then
        d.markers[#d.markers + 1] = createMarker({ x = drop.x, y = drop.y, z = drop.z, radius = drop.radius or 4, category = self.category,
                                                   style = spec.style or self.style, route = spec.route ~= false, player = player, track = false,
                                                   description = text, title = L(drop.name or { en = "Drop-off", ru = "Разгрузка" }) })
    else
        log("deliver: the drop-off area has no point")
    end
    quest:showDeliver()
end

function Quest:showDeliver()
    local d = self.deliverState
    local line = d.count > 1 and string.format("%s (%d/%d)", d.text, d.done, d.count) or d.text
    self.player:objective({ title = self.title, text = line, state = "update", category = self.category, style = d.spec.style or self.style })
end

-- One box was put down in the drop-off area.
function Quest:deliveredBox()
    local d = self.deliverState
    if not d then return end
    d.done = d.done + 1
    if d.spec.onBox then d.spec.onBox(self, self.player, d.done) end
    if self.deliverState ~= d then return end   -- (onBox moved the quest on)
    if d.done >= d.count then
        -- Kept until onDone has run: the same step started again (a job's loop) reuses its markers.
        d.finished = true
        if d.spec.onDone then d.spec.onDone(self, self.player) end
        if self.deliverState == d and d.finished then self:clearDeliver() end
    else
        self:showDeliver()
    end
end

function Quest:clearDeliver()
    local d = self.deliverState
    if not d then return end
    self.deliverState = nil
    for _, m in ipairs(d.markers) do removeMarker(m) end
    if d.own then d.pool:free() end
    if delivering[self.player.id] == self then delivering[self.player.id] = nil end
end

on("playerQuit", function(player)
    local q = delivering[player.id]
    if q then q:clearDeliver() end
    delivering[player.id] = nil
    panelOf[player.id] = nil
end)

-- Progress bar under the objective; percent 0..1, nil hides it.
function Quest:progress(title, percent)
    if percent == nil then self.player:objectiveMeter() else self.player:objectiveMeter(title, percent) end
end

function Quest:finish(state)
    if not self.active then return end
    self.active = false
    self:clearSteps()
    if self.player.valid then
        self.player:objectiveMeter()
        self.player:objective({ title = self.title, text = "", state = state, category = self.category, style = self.style, xp = self.xp or 0 })
        -- The completion callout stays a moment, then the panel is released.
        -- (not when a newer quest took the panel meanwhile: a job taken again at once)
        local player, category, quest = self.player, self.category, self
        after(6000, function()
            if player.valid and panelOf[player.id] == quest then
                panelOf[player.id] = nil
                player:objective({ state = "remove", category = category })
            end
        end)
    end
end

-- complete([xp]): the completion callout shows the XP number when given
function Quest:complete(xp) self.xp = xp; self:finish("complete") end
function Quest:fail() self:finish("failed") end

-- ---------------------------------------------------------------- quests from files
-- quests/<id>.json, made by the quest editors (format: docs/quests.md).
function Quest.load(id) return quests.load(id) end
function Quest.list() return quests.list() end

local STEP_TYPES = { ["goto"] = true, hold = true, collect = true, deliver = true, talk = true }

-- Starts step `s` of a quest file on q; onDone() when it is done. pool: the boxes of a deliver step (jobs).
local function runStep(q, s, onDone, pool)
    local L = function(v) return Quest.text(v, q.player) end
    if s.type == "goto" then
        q:step(L(s.text), { x = s.x, y = s.y, z = s.z, radius = s.radius or 5, route = s.route ~= false, style = s.style,
                         onEnter = function() onDone() end })
    elseif s.type == "hold" then
        q:hold(L(s.text), { x = s.x, y = s.y, z = s.z, radius = s.radius or 6, seconds = s.seconds or 10, meter = L(s.meter),
                         leaveTip = L(s.leaveTip), style = s.style, onDone = function() onDone() end })
    elseif s.type == "collect" then
        q:collect(L(s.text), { event = s.event, count = s.count, item = s.item, class = s.class, ids = s.ids, area = s.area,
                            marker = s.marker, route = s.route, spawn = s.spawn, style = s.style, onDone = function() onDone() end })
    elseif s.type == "deliver" then
        q:deliver(L(s.text), { pickup = s.pickup, dropoff = s.dropoff, count = s.count, route = s.route, style = s.style, pool = pool, step = s,
                            onDone = function() onDone() end })
    elseif s.type == "talk" then
        local lines = {}
        for i, line in ipairs(s.lines or {}) do lines[i] = L(line) end
        q:talk(L(s.text), { actor = s.actor, x = s.x, y = s.y, z = s.z, radius = s.radius or 3, speaker = L(s.speaker), lines = lines,
                         route = s.route ~= false, style = s.style, onDone = function() onDone() end })
    else
        log("quest " .. tostring(q.id) .. ": unknown step type " .. tostring(s.type))
        onDone()
    end
end

-- Runs a quest definition for one player: its steps in order, then the reward and the completion
-- screen. onDone(player, quest) is called at the end. Returns the quest (q:fail() aborts it).
-- A job file ("kind": "job") is started with Quest.runJob instead.
function Quest.run(player, def, onDone)
    if not def or not player.valid then return nil end
    local L = function(v) return Quest.text(v, player) end
    local q = Quest.start(player, { title = L(def.title), category = def.category or "task", style = def.style, text = L(def.text) })
    q.id = def.id
    local steps, index = def.steps or {}, 0
    local nextStep
    nextStep = function()
        if not q.active or not player.valid then return end
        index = index + 1
        local s = steps[index]
        if not s then
            for _, r in ipairs(def.reward or {}) do player:giveItem(r.item, r.count or 1) end
            q:complete(def.xp)
            if onDone then onDone(player, q) end
            return
        end
        runStep(q, s, nextStep)
    end
    nextStep()
    return q
end

-- ---------------------------------------------------------------- jobs
-- A job file ("kind": "job", docs/quests.md) is a repeatable task: a giver NPC with [F] "Take the job" /
-- "Hand the job in", a work uniform, money for every round of its steps. While a player is on the job its
-- steps run in a loop, with markers, the objective and a HUD line only for that player; handing the job
-- in (or leaving the server) ends it.
--   local job = Quest.runJob(Quest.load("loader"))   -- or Quest.runJob("loader")
--   job:stop()
--   require("jobs")                                  -- every job file at once (modes/_lib/jobs.lua)
local jobsRunning = {}   -- job id -> job
local shifts = {}        -- player id -> { job, q, cycles, earned, index, text, uniform, finished }
local Job = {}
Job.__index = Job

local function T(player, ru, en) return player.lang == "ru" and ru or en end
-- One text for everyone at once (a name over the head, the [F] prompt): "Foreman / Бригадир".
local function both(v)
    if type(v) ~= "table" then return v or "" end
    local en, ru = v.en or "", v.ru or ""
    if en ~= "" and ru ~= "" and en ~= ru then return en .. " / " .. ru end
    return en ~= "" and en or ru
end

function Quest.runJob(def)
    if type(def) == "string" then def = Quest.load(def) end
    if not def then return nil end
    local id = def.id or "job"
    if jobsRunning[id] then jobsRunning[id]:stop() end
    local job = setmetatable({ def = def, id = id, steps = {}, pools = {}, blips = {} }, Job)
    for _, s in ipairs(def.steps or {}) do
        if STEP_TYPES[s.type] then job.steps[#job.steps + 1] = s
        else log("job " .. id .. ": unknown step type " .. tostring(s.type)) end
    end
    if #job.steps == 0 then log("job " .. id .. ": no steps") end
    local deny = function(p) return T(p, "Сначала возьмите работу: ", "Take the job first: ") .. Quest.text(def.title, p) end
    for i, s in ipairs(job.steps) do
        if s.type == "deliver" then
            job.pools[i] = Quest.pool(s.pickup, function(p) local sh = shifts[p.id]; return sh ~= nil and sh.job == job end, deny)
        end
    end
    -- The giver: an NPC of the mode's world (editor: World > NPC) or one of its own.
    local g = def.giver or {}
    local a = (g.actor and g.actor ~= "") and getActor(g.actor) or nil
    local world
    if a then
        job.at = { x = a.x, y = a.y, z = a.z }
        if a.world and a.world >= 0 then world = a.world end
    elseif g.x and g.y then
        job.at = { x = g.x, y = g.y, z = g.z or 0 }
        local ok, res = pcall(createActor, { x = g.x, y = g.y, z = g.z or 0, yaw = g.yaw or 0, look = (g.look ~= "" and g.look) or nil,
                                             anim = (g.anim and g.anim ~= "") and g.anim or "idle", name = Quest.text(g.name) or "" })
        if ok then job.actor = res else log("job " .. id .. ": giver: " .. tostring(res)) end
    else
        log("job " .. id .. ": no giver (giver.actor of the mode's world, or giver x, y, z)")
    end
    if job.at then
        local label = both(g.name)
        if label == "" then label = both(def.title) end
        job.interaction = createInteraction({ x = job.at.x, y = job.at.y, z = job.at.z, radius = g.radius or 2.5, text = label, world = world,
                                              onUse = function(player) job:use(player) end })
    end
    jobsRunning[id] = job
    for _, p in ipairs(players()) do job:blip(p) end
    if quests.watch then quests.watch() end   -- saving a quest file reloads the mode
    return job
end

-- Running jobs: Quest.jobs()[id]; the job a player is on: Quest.shift(player) -> job id, {cycles, earned}.
function Quest.jobs() return jobsRunning end
function Quest.shift(player)
    local sh = shifts[player.id]
    if not sh then return nil end
    return sh.job.id, { cycles = sh.cycles, earned = sh.earned, finished = sh.finished == true }
end

-- The giver on this player's map (giver.marker = false: none).
function Job:blip(p)
    if p.bot or not p.valid or self.blips[p.id] or not self.at or (self.def.giver and self.def.giver.marker == false) then return end
    self.blips[p.id] = createMarker({ x = self.at.x, y = self.at.y, z = self.at.z, radius = 3, category = self.def.category or "task", style = "basic",
                                      route = false, track = false, player = p,
                                      title = Quest.text(self.def.title, p), description = Quest.text(self.def.text, p) })
end

function Job:use(player)
    local def = self.def
    local L = function(v) return Quest.text(v, player) end
    local sh = shifts[player.id]
    if sh and sh.job ~= self then
        player:tip(T(player, "Сначала сдайте работу: ", "Hand your job in first: ") .. L(sh.job.def.title), 4)
        return
    end
    local pay = math.floor(def.pay or 0)
    local each = def.payout == "each"
    if not sh then
        if #self.steps == 0 then player:tip(T(player, "Эта работа пока не настроена", "This job is not set up yet"), 4); return end
        local lines = {}
        if L(def.text) ~= "" then lines[#lines + 1] = L(def.text) end
        if pay ~= 0 then
            lines[#lines + 1] = string.format(T(player, "{7CD67C}$%d{FFFFFF} за каждый раз, %s", "{7CD67C}$%d{FFFFFF} each time, %s"), pay,
                each and T(player, "сразу", "paid at once") or T(player, "оплата при сдаче работы", "paid when you hand the job in"))
        end
        if (def.cycles or 0) > 0 then lines[#lines + 1] = string.format(T(player, "Не больше %d раз за смену", "Up to %d times a shift"), def.cycles) end
        player:dialog({
            title = L(def.title), text = table.concat(lines, "\n"),
            button1 = T(player, "Взять работу", "Take the job"), button2 = T(player, "Нет", "No"),
            onResponse = function(p, ok) if ok then self:start(p) end end,
        })
    else
        local text = T(player, "Выполнено: ", "Done: ") .. sh.cycles
        if pay ~= 0 then
            text = text .. (each and T(player, "\nЗаработано: ", "\nEarned: ") or T(player, "\nК выплате: ", "\nTo be paid: ")) .. "{7CD67C}$" .. sh.earned
        end
        player:dialog({
            title = T(player, "Сдать работу", "Hand the job in"), text = text,
            button1 = T(player, "Сдать", "Hand in"), button2 = T(player, "Работать дальше", "Keep working"),
            onResponse = function(p, ok) if ok then self:finishShift(p, true) end end,
        })
    end
end

function Job:start(player)
    if shifts[player.id] or not player.valid or jobsRunning[self.id] ~= self then return end
    local def = self.def
    local q = Quest.start(player, { title = Quest.text(def.title, player), category = def.category or "task", style = def.style, text = Quest.text(def.text, player) })
    q.id = self.id
    local sh = { job = self, q = q, cycles = 0, earned = 0, index = 0, uniform = player.uniform }
    shifts[player.id] = sh
    if def.uniform and def.uniform ~= "" then
        local ok, err = pcall(function() player:setUniform(def.uniform) end)
        if not ok then log("job " .. self.id .. ": uniform \"" .. def.uniform .. "\": " .. tostring(err)) end
    end
    self:nextStep(player, sh)
end

-- The next step of the round; after the last one the round is paid and the steps start over.
function Job:nextStep(player, sh)
    if shifts[player.id] ~= sh or not player.valid then return end
    local def = self.def
    sh.index = sh.index + 1
    if sh.index > #self.steps then
        sh.cycles = sh.cycles + 1
        local pay = math.floor(def.pay or 0)
        if pay ~= 0 then
            sh.earned = sh.earned + pay
            if def.payout == "each" then player:giveMoney(pay) end
            player:tip(T(player, "Готово: ", "Done: ") .. (pay > 0 and "+" or "") .. "$" .. pay, 3)
        end
        if (def.cycles or 0) > 0 and sh.cycles >= def.cycles then
            sh.finished = true
            sh.text = T(player, "Сдайте работу", "Hand the job in")
            sh.q:step(sh.text, self.at and { x = self.at.x, y = self.at.y, z = self.at.z, radius = 3, route = true } or nil)
            self:hud(player, sh)
            return
        end
        sh.index = 1
    end
    local s = self.steps[sh.index]
    sh.text = Quest.text(s.text, player)
    self:hud(player, sh)
    runStep(sh.q, s, function() self:nextStep(player, sh) end, self.pools[sh.index])
end

function Job:hud(player, sh)
    local def = self.def
    local count = tostring(sh.cycles) .. ((def.cycles or 0) > 0 and ("/" .. def.cycles) or "")
    local money = (def.pay or 0) ~= 0 and (" · {7CD67C}$" .. sh.earned) or ""
    player:hud("dgmp_job", {
        text = "{F2D16B}" .. Quest.text(def.title, player) .. "{FFFFFF}: " .. count .. money .. ((sh.text or "") ~= "" and ("\n{A0A0A0}" .. sh.text) or ""),
        x = 0.985, y = 0.11, align = "right", size = 1.1, bg = "#00000088",
    })
end

-- Ends the player's shift: handIn pays what is owed ("payout": "end") and shows the completion screen;
-- otherwise (left the server, the job stopped) nothing is paid. The uniform, markers and HUD line go.
function Job:finishShift(player, handIn)
    local sh = shifts[player.id]
    if not sh or sh.job ~= self then return end
    shifts[player.id] = nil
    local def = self.def
    if player.valid then
        local carried = player.carrying
        if carried and pools[carried] then pools[carried]:home(carried) end
        player:hud("dgmp_job")
        pcall(function() player:setUniform(sh.uniform) end)
        if handIn then
            if def.payout ~= "each" and sh.earned ~= 0 then player:giveMoney(sh.earned) end
            player:message(string.format(T(player, "Работа сдана: выполнено %d, заработано $%d", "Job handed in: done %d, earned $%d"), sh.cycles, sh.earned), 0x8FE388FF)
        end
    end
    if handIn and sh.cycles > 0 then sh.q:complete(def.xp) else sh.q:finish("remove") end
end

-- Takes the job out of the world: shifts end unpaid, its boxes and the giver go.
function Job:stop()
    if jobsRunning[self.id] == self then jobsRunning[self.id] = nil end
    for _, sh in pairs(shifts) do if sh.job == self then self:finishShift(sh.q.player, false) end end
    for _, pool in pairs(self.pools) do pool:free() end
    for _, m in pairs(self.blips) do removeMarker(m) end
    if self.interaction then removeInteraction(self.interaction) end
    if self.actor then removeActor(self.actor) end
    self.pools, self.blips, self.interaction, self.actor = {}, {}, nil, nil
end

on("playerJoin", function(player) for _, job in pairs(jobsRunning) do job:blip(player) end end)
on("playerQuit", function(player)
    local sh = shifts[player.id]
    if sh then sh.job:finishShift(player, false) end
    for _, job in pairs(jobsRunning) do
        if job.blips[player.id] then removeMarker(job.blips[player.id]); job.blips[player.id] = nil end
    end
end)

return Quest
