-- Quest test: every player who joins gets the quest under test. "Проверить" in the in-game quest
-- editor has the launcher start this mode and send "quest <id>" to the server console. A job file ("kind": "job") is put into the
-- world instead: its giver stands where the editor placed it.
local Quest = require("quest")

local current = storage.get("quest")   -- id of the quest under test (survives reloads)
local running = {}                     -- player id -> quest
local job                              -- the job under test (Quest.runJob)

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
    if def.kind == "job" then
        if not job or Quest.jobs()[current] ~= job then job = Quest.runJob(def) end
        player:message("Проверка работы: " .. Quest.text(def.title, player) .. " — подойдите к тому, кто её даёт, и нажмите F", 0x9FD3FFFF)
        if job and job.at then   -- next to the giver, unless already around
            local x, y = player:position()
            if (x - job.at.x) ^ 2 + (y - job.at.y) ^ 2 > 10000 ^ 2 then player:setPosition(job.at.x + 300, job.at.y, job.at.z + 100) end
        end
        return
    end
    if job then job:stop(); job = nil end
    player:message("Проверка квеста: " .. (Quest.text(def.title, player) or current) .. "   (/quest — заново)", 0x9FD3FFFF)
    running[player.id] = Quest.run(player, def, function(p) p:message("Квест пройден", 0x8FE388FF) end)
end

on("playerJoin", function(player) after(1500, function() if player.valid then give(player) end end) end)
on("playerQuit", function(player) running[player.id] = nil end)

command("quest", function(player) give(player) end)

on("console", function(line)
    local cmd, id = line:match("^(%S+)%s*(%S*)")
    if cmd ~= "quest" then return end
    if id ~= "" then current = id; storage.set("quest", id) end
    if job then job:stop(); job = nil end   -- (put into the world again, as saved)
    for _, p in ipairs(players()) do if not p.bot then give(p) end end
    log("quest under test: " .. tostring(current))
    return true
end)
