-- HTML UI demo (docs/API.md "HTML-интерфейс"): one page, ui/index.html, on every player's screen.
-- Without the focus it is a status panel (the game is played as usual); F2 or /phone opens the phone
-- in it and gives it the mouse and keyboard; the page's buttons talk back to this file.

local GREEN, GREY = "#7CD67C", "#A0A0A0"
local phoneOpen = {}  -- player id -> true

local function status(player)
    player:uiSend("status", { name = player.name, players = #players(), time = os.date("%H:%M") })
end

local function openPhone(player)
    phoneOpen[player.id] = true
    local list = {}
    for _, p in ipairs(players()) do list[#list + 1] = { id = p.id, name = p.name } end
    player:uiSend("phone", { open = true, players = list })
    player:uiFocus(true)
end

local function closePhone(player)
    phoneOpen[player.id] = nil
    player:uiSend("phone", { open = false })
    player:uiFocus(false)
end

on("playerJoin", function(player)
    player:ui("index.html")   -- no focus: only drawn over the game
    status(player)
    player:message("HTML UI demo: F2 or /phone opens the phone.", GREEN)
end)
on("playerQuit", function(player) phoneOpen[player.id] = nil end)

bindKey("F2", function(player) if phoneOpen[player.id] then closePhone(player) else openPhone(player) end end)
command("phone", function(player) openPhone(player) end)

-- What the page sends: dgmp.send(name, data).
on("uiMessage", function(player, name, data)
    if name == "close" then
        closePhone(player)
    elseif name == "ping" then
        player:uiSend("pong", { at = now(), clicks = type(data) == "table" and tonumber(data.clicks) or 0 })
    elseif name == "say" and type(data) == "table" and type(data.text) == "string" then
        local text = data.text:sub(1, 120)
        if text ~= "" then broadcast(player.name .. " (phone): " .. text, GREY) end
    elseif name == "sms" and type(data) == "table" then
        local to = getPlayer(tonumber(data.to) or -1)
        if to and type(data.text) == "string" then
            to:uiSend("sms", { from = player.name, text = data.text:sub(1, 200) })
            to:message("SMS from " .. player.name .. ": " .. data.text:sub(1, 200), GREEN)
        end
    end
end)

-- Server console: "phone" opens or closes the phone of every player (testing without the key).
on("console", function(line)
    if line ~= "phone" then return end
    for _, p in ipairs(players()) do if phoneOpen[p.id] then closePhone(p) else openPhone(p) end end
    return true
end)

-- The clock and the player count on everyone's panel.
every(10000, function() for _, p in ipairs(players()) do status(p) end end)
