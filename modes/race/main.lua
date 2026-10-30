-- Bike race: the reference mode for logic objects (modes/_lib/logic.lua) and races (modes/_lib/race.lua).
-- Place in the editor (World > mode "race" > Mode logic):
--   lobby       a point: where players appear
--   grid        points: start positions, one per racer (the arrow is the bike's direction)
--   checkpoint  triggers, in order: the last one is the finish
-- Commands: /join, /leave, /start (starts a countdown for everyone who joined), /watch, /results.
-- The race runs in a virtual world of its own: the racers see only each other; who did not join stays
-- in the lobby and may watch it (/watch, newcomers during a race watch at once). After the race the
-- racers come back to the lobby.

local logic = require("logic")
local Race = require("race")

local lobby = logic.point("lobby")
local GREEN, YELLOW, GREY = "#7CD67C", "#F2D16B", "#A0A0A0"
local function L(player, en, ru) return (player.lang == "ru") and ru or en end
local function fmtTime(seconds) return string.format("%d:%05.2f", math.floor(seconds / 60), seconds % 60) end

local race = Race.new({
    name = { en = "Race", ru = "Гонка" },
    grid = logic.points("grid"),
    checkpoints = logic.triggers("checkpoint", { radius = 12 }),
    lobby = 0,             -- no timer: /start starts it
    finishWindow = 15,     -- seconds the others have after the first finish
    onEnd = function(r, results)
        -- Watchers and racers are back; who is still in the lobby hears the results too.
        for _, p in ipairs(players()) do
            if not p.bot then p:message(L(p, "/join for the next race, /results for these.", "/join — следующая гонка, /results — эти результаты."), GREY) end
        end
    end,
})

on("playerJoin", function(player)
    if lobby.placed then player:setPosition(lobby.x, lobby.y, lobby.z + 100, lobby.yaw) end
    if not player.hasBike then player:giveBike() end
    if race.state == "racing" then race:watch(player); return end
    player:message(L(player, "Bike race! /join to take part, /start to begin.", "Гонки на мотоциклах! /join — участвовать, /start — старт."), GREEN)
end)

command("join", function(player) race:join(player) end)
command("leave", function(player)
    if not race:leave(player) and not race:unwatch(player) then player:message(L(player, "You are not in the race.", "Вы не в гонке."), GREY) end
end)
command("start", function(player)
    if race.state == "idle" then race:join(player) end
    if race.state ~= "lobby" then player:message(L(player, "A race is on already.", "Гонка уже идёт."), GREY); return end
    race:start()
end)
command("watch", function(player)
    if race:watching(player) then race:unwatch(player); return end
    if not race:watch(player) then player:message(L(player, "No race to watch now.", "Сейчас нечего смотреть."), GREY) end
end)
command("results", function(player)
    local list = race.last or {}
    if #list == 0 and #(list.dnf or {}) == 0 then player:message(L(player, "No results yet.", "Результатов пока нет."), GREY); return end
    for i, f in ipairs(list) do player:message(string.format("%d. %s  %s", i, f.name, fmtTime(f.time)), YELLOW) end
    for _, f in ipairs(list.dnf or {}) do player:message(f.name .. "  DNF", GREY) end
end)

on("start", function()
    log(string.format("Race: lobby %s, %d grid slots, %d checkpoints", lobby.placed and "placed" or "not placed", #race.grid, #race.checkpoints))
end)
