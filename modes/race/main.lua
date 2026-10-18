-- Bike race: the reference mode for logic objects (modes/_lib/logic.lua).
-- Place in the editor (World > mode "race" > Mode logic):
--   lobby       a point: where players appear
--   grid        points: start positions, one per racer (the arrow is the bike's direction)
--   checkpoint  triggers, in order: the last one is the finish
-- Commands: /join, /leave, /start (starts a countdown for everyone who joined), /results.
-- During a countdown a newcomer gets a free grid slot at once; during a race newcomers watch the
-- racers (spectator) until it ends. After the first finish the others have 15 seconds.

local logic = require("logic")

local lobby = logic.point("lobby")
local grid = logic.points("grid")
local checkpoints = logic.triggers("checkpoint", { radius = 12 })

local GREEN, YELLOW, GREY, RED = "#7CD67C", "#F2D16B", "#A0A0A0", "#E07070"
local FINISH_WINDOW = 15   -- seconds the others have after the first finish
local function L(player, en, ru) return (player.lang == "ru") and ru or en end

-- state: "idle" (joining), "countdown", "racing"
-- racers: everyone who joined (player, slot); r.inRace: rides the current race
local race = { state = "idle", racers = {}, spectators = {}, finished = {}, startedAt = 0 }

local function fmtTime(seconds)
    return string.format("%d:%05.2f", math.floor(seconds / 60), seconds % 60)
end

local function riders()
    local list = {}
    for _, r in pairs(race.racers) do if r.inRace and r.player.valid then list[#list + 1] = r end end
    table.sort(list, function(a, b) return a.slot < b.slot end)
    return list
end

local function riderIds()
    local ids = {}
    for _, r in ipairs(riders()) do ids[#ids + 1] = r.player.id end
    return ids
end

local function freeSlot()
    local used = {}
    for _, r in ipairs(riders()) do used[r.slot] = true end
    for i = 1, #grid do if not used[i] then return i end end
    return nil
end

local function showNext(r)
    if r.marker then removeMarker(r.marker); r.marker = nil end
    local cp = checkpoints[r.next]
    if not cp then return end
    local last = r.next == #checkpoints
    r.marker = cp:marker({ player = r.player, route = true, category = "task", style = last and "defendarea" or "basic",
                           title = last and L(r.player, "Finish", "Финиш") or (L(r.player, "Checkpoint ", "Чекпойнт ") .. r.next) })
    r.player:objective({ state = "update", category = "task", title = L(r.player, "Race", "Гонка"),
                         text = last and L(r.player, "Ride to the finish!", "К финишу!")
                             or string.format(L(r.player, "Checkpoint %d of %d", "Чекпойнт %d из %d"), r.next, #checkpoints) })
    r.player:objectiveMeter(L(r.player, "Progress", "Прогресс"), (r.next - 1) / #checkpoints)
end

-- On its own bike on its grid slot, held there until the start.
local function placeOnGrid(r, slot)
    local g = grid[slot]
    r.slot, r.inRace, r.next, r.done = slot, true, 1, false
    r.player:spectate(false)
    race.spectators[r.player.id] = nil
    r.player:putOnBike(g.x, g.y, g.z, g.yaw)
    r.player:freeze(true)
    r.player:objective({ state = "new", category = "task", title = L(r.player, "Race", "Гонка"),
                         text = L(r.player, "The race starts soon", "Скоро старт") })
end

local function watch(player)
    race.spectators[player.id] = true
    player:spectate(true, riderIds())
    player:message(L(player, "A race is on: watch it (arrows switch riders), you ride in the next one.",
                             "Идёт гонка: смотрите её (стрелки — другой гонщик), поедете в следующей."), YELLOW)
end

local function endRace()
    local lines = {}
    for i, f in ipairs(race.finished) do lines[#lines + 1] = string.format("%d. %s  %s", i, f.name, fmtTime(f.time)) end
    for _, r in ipairs(riders()) do if not r.done then lines[#lines + 1] = r.player.name .. " DNF" end end
    broadcast(#lines > 0 and ("Race over: " .. table.concat(lines, "   ")) or "Race over", YELLOW)
    for _, r in pairs(race.racers) do
        if r.player.valid then
            if r.marker then removeMarker(r.marker) end
            r.player:freeze(false)
            r.player:objective({ state = "remove" })
            r.player:objectiveMeter()
        end
        r.inRace, r.marker, r.done, r.next = false, nil, false, 1
    end
    -- Watchers come back to the lobby.
    for id in pairs(race.spectators) do
        local p = getPlayer(id)
        if p then
            p:spectate(false)
            if lobby.placed then p:setPosition(lobby.x, lobby.y, lobby.z + 100, lobby.yaw) end
        end
    end
    race.lastResults = race.finished
    race.state, race.finished, race.spectators, race.deadline = "idle", {}, {}, nil
end

local function checkReady()
    if #checkpoints == 0 then return "No checkpoints placed yet: the editor's World > race > Mode logic." end
    if #grid == 0 then return "No start grid placed yet: the editor's World > race > Mode logic." end
    return nil
end

local function startCountdown()
    local problem = checkReady()
    if problem then broadcast(problem, RED); return end
    local joined = {}
    for _, r in pairs(race.racers) do if r.player.valid then joined[#joined + 1] = r end end
    if #joined == 0 then broadcast("Nobody joined the race: /join", RED); return end
    table.sort(joined, function(a, b) return a.joinedAt < b.joinedAt end)
    race.state = "countdown"
    for i, r in ipairs(joined) do
        if i <= #grid then placeOnGrid(r, i)
        else r.player:message(L(r.player, "The grid is full: you ride in the next race.", "Решётка заполнена: поедете в следующей гонке."), YELLOW) end
    end
    for n = 10, 1, -1 do
        after((10 - n) * 1000 + 2000, function()
            if race.state ~= "countdown" then return end
            for _, r in ipairs(riders()) do r.player:tip(tostring(n), 1) end
        end)
    end
    after(12000, function()
        if race.state ~= "countdown" then return end
        race.state, race.startedAt = "racing", now()
        for _, r in ipairs(riders()) do r.player:freeze(false); r.player:tip(L(r.player, "GO!", "СТАРТ!"), 2); showNext(r) end
        broadcast("The race has started!", GREEN)
    end)
end

-- Checkpoints in order; a racer only counts the one it rides to.
for i, cp in ipairs(checkpoints) do
    cp:onEnter(function(player)
        local r = race.racers[player.id]
        if race.state ~= "racing" or not r or not r.inRace or r.done or r.next ~= i then return end
        if i < #checkpoints then
            r.next = i + 1
            player:tip(string.format(L(player, "Checkpoint %d / %d", "Чекпойнт %d / %d"), i, #checkpoints), 2)
            showNext(r)
            return
        end
        r.done = true
        local time = now() - race.startedAt
        race.finished[#race.finished + 1] = { name = player.name, time = time }
        if r.marker then removeMarker(r.marker); r.marker = nil end
        player:objective({ state = "update", category = "task", title = L(player, "Race", "Гонка"),
                           text = string.format(L(player, "Finished %d with %s", "Финиш: место %d, %s"), #race.finished, fmtTime(time)) })
        player:objectiveMeter(L(player, "Progress", "Прогресс"), 1)
        broadcast(string.format("%s finished %d. in %s", player.name, #race.finished, fmtTime(time)), GREEN)
        -- The first one in: the others have FINISH_WINDOW seconds.
        if not race.deadline then
            local left = 0
            for _, o in ipairs(riders()) do if not o.done then left = left + 1 end end
            if left > 0 then
                race.deadline = now() + FINISH_WINDOW
                broadcast(string.format("%d seconds left to finish!", FINISH_WINDOW), YELLOW)
            end
        end
    end)
end

local lastShown = nil
on("tick", function()
    if race.state ~= "racing" then return end
    local left = 0
    for _, r in ipairs(riders()) do if not r.done then left = left + 1 end end
    if left == 0 then endRace(); return end
    if race.deadline then
        local remain = math.ceil(race.deadline - now())
        if remain ~= lastShown and remain > 0 then
            lastShown = remain
            for _, r in ipairs(riders()) do if not r.done then r.player:tip(tostring(remain), 1) end end
        end
        if now() >= race.deadline then lastShown = nil; endRace() end
    end
end)

local function join(player)
    if not race.racers[player.id] then
        race.racers[player.id] = { player = player, slot = 0, next = 1, joinedAt = now() }
        broadcast(player.name .. " joined the race", GREY)
    end
    local r = race.racers[player.id]
    if race.state == "countdown" and not r.inRace then
        local slot = freeSlot()
        if slot then placeOnGrid(r, slot)
        else player:message(L(player, "The grid is full: you ride in the next race.", "Решётка заполнена: поедете в следующей гонке."), YELLOW) end
    elseif race.state == "racing" and not r.inRace then
        watch(player)
    end
end

on("playerJoin", function(player)
    if race.state == "racing" then watch(player); return end
    if lobby.placed then player:setPosition(lobby.x, lobby.y, lobby.z + 100, lobby.yaw) end
    if not player.hasBike then player:giveBike() end
    if race.state == "countdown" then join(player); return end
    player:message(L(player, "Bike race! /join to take part, /start to begin.", "Гонки на мотоциклах! /join — участвовать, /start — старт."), GREEN)
end)

on("playerQuit", function(player)
    local r = race.racers[player.id]
    if r and r.marker then removeMarker(r.marker) end
    race.racers[player.id] = nil
    race.spectators[player.id] = nil
end)

command("join", function(player)
    if race.racers[player.id] and race.state == "idle" then player:message("You are in already.", GREY); return end
    join(player)
end)

command("leave", function(player)
    local r = race.racers[player.id]
    if not r then return end
    if r.marker then removeMarker(r.marker) end
    player:freeze(false)
    player:objective({ state = "remove" }); player:objectiveMeter()
    race.racers[player.id] = nil
    broadcast(player.name .. " left the race", GREY)
end)

command("start", function(player)
    if race.state ~= "idle" then player:message("A race is on already.", GREY); return end
    startCountdown()
end)

command("results", function(player)
    local list = race.lastResults or {}
    if #list == 0 then player:message("No results yet.", GREY); return end
    for i, f in ipairs(list) do player:message(string.format("%d. %s  %s", i, f.name, fmtTime(f.time)), YELLOW) end
end)

on("start", function()
    log(string.format("Race: lobby %s, %d grid slots, %d checkpoints", lobby.placed and "placed" or "not placed", #grid, #checkpoints))
end)
