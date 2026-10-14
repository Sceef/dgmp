-- Quest test: every player who joins gets the quest under test. The launcher's quest editor starts
-- this mode and sends "quest <id>" to the server console.
local Quest = require("quest")

local current = storage.get("quest")   -- id of the quest under test (survives reloads)
local running = {}                     -- player id -> quest

local function give(player)
    if running[player.id] and running[player.id].active then running[player.id]:fail() end
    running[player.id] = nil
    if not current then
        player:message("Нет квеста для проверки (консоль сервера: quest <id>)", 0xFFCC66FF)
        return
    end
    local def = Quest.load(current)
    if not def then
        player:message("Квест '" .. current .. "' не найден или повреждён (quests/" .. current .. ".json)", 0xFF6666FF)
        return
    end
    player:message("Проверка квеста: " .. (def.title or current) .. "   (/quest — заново)", 0x9FD3FFFF)
    running[player.id] = Quest.run(player, def, function(p) p:message("Квест пройден", 0x8FE388FF) end)
end

on("playerJoin", function(player) after(1500, function() if player.valid then give(player) end end) end)
on("playerQuit", function(player) running[player.id] = nil end)

command("quest", function(player) give(player) end)

on("console", function(line)
    local cmd, id = line:match("^(%S+)%s*(%S*)")
    if cmd ~= "quest" then return end
    if id ~= "" then current = id; storage.set("quest", id) end
    for _, p in ipairs(players()) do if not p.bot then give(p) end end
    log("quest under test: " .. tostring(current))
    return true
end)
