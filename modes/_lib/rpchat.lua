-- rpchat.lua — role-play chat as on SA-MP RP servers: what a player says is heard only nearby (the
-- farther, the paler), plus the usual commands. Players in other virtual worlds never hear each other.
--
--   local rpchat = require("rpchat")
--   rpchat.install()                                   -- defaults below
--   rpchat.install({ radius = 15, ooc = false, names = "underscores" })
--   rpchat.near(player, "* the radio crackles", "#C2A2DA", 10)   -- your own messages, same rules
--
-- Commands: /me action, /do description, /try action (success or failure), /b local OOC,
-- /s shout (twice as far), /w whisper (2 m), /pm <id or name> text, /o global OOC (if ooc = true).
-- Options:
--   radius = 20            metres a normal line is heard
--   ooc = true             /o for everyone (false: off)
--   names = "spaces"       "Mark_Twain" shows as "Mark Twain"; "underscores" keeps the nickname
--   adminSees = false      admins also get every local line (as [far]) — for moderation
--   prefix = {}            override texts: says, shouts, whispers, success, failure ({en, ru} each)

local M = {}
local cfg = { radius = 20, ooc = true, names = "spaces", adminSees = false }

local TEXT = {
    says = { en = "says", ru = "говорит" },
    shouts = { en = "shouts", ru = "кричит" },
    whispers = { en = "whispers", ru = "шепчет" },
    success = { en = "succeeded", ru = "удачно" },
    failure = { en = "failed", ru = "неудачно" },
    usage = { en = "Usage: ", ru = "Используйте: " },
    noOne = { en = "No such player", ru = "Нет такого игрока" },
    pmTo = { en = "PM to", ru = "ЛС для" },
    pmFrom = { en = "PM from", ru = "ЛС от" },
    oocOff = { en = "Global OOC chat is off", ru = "Общий OOC-чат выключен" },
}
local function tr(key, player)
    local t = (cfg.prefix and cfg.prefix[key]) or TEXT[key]
    return t[player.lang] or t.en
end

-- Five shades from white to grey, as SA-MP RP servers fade local chat with distance.
local FADE = { "#E6E6E6", "#C8C8C8", "#AAAAAA", "#8C8C8C", "#6E6E6E" }
local ACTION = "#C2A2DA"   -- /me /do /try
local OOC = "#9BB1C7"

function M.name(player)
    return cfg.names == "spaces" and (player.name:gsub("_", " ")) or player.name
end

local function distance(a, b)
    local ax, ay, az = a:position()
    local bx, by, bz = b:position()
    return math.sqrt((ax - bx) ^ 2 + (ay - by) ^ 2 + (az - bz) ^ 2) / 100
end

-- Sends `text` to everyone within `radius` metres of `from` in its virtual world. `color` nil: fades
-- with distance; a function(player) -> text: each listener gets its own language.
function M.near(from, text, color, radius)
    radius = radius or cfg.radius
    for _, p in ipairs(players()) do
        if not p.bot and p.world == from.world then
            local d = distance(from, p)
            local line = type(text) == "function" and text(p) or text
            if d <= radius then
                p:message(line, color or FADE[math.min(#FADE, 1 + math.floor(d / radius * #FADE))])
            elseif cfg.adminSees and p.admin then
                p:message("[far] " .. line, "#555555")
            end
        end
    end
end

local function usage(player, text) player:message(tr("usage", player) .. text, "#E07070") end

function M.install(opts)
    for k, v in pairs(opts or {}) do cfg[k] = v end

    on("chat", function(player, text)
        M.near(player, function(p) return M.name(player) .. " " .. tr("says", p) .. ": " .. text end)
        log("<" .. player.name .. "> " .. text)
        return false   -- not the global chat
    end)

    command("me", function(player, args)
        if args == "" then return usage(player, "/me <action>") end
        M.near(player, "* " .. M.name(player) .. " " .. args, ACTION)
    end)
    command("do", function(player, args)
        if args == "" then return usage(player, "/do <description>") end
        M.near(player, "* " .. args .. " (( " .. M.name(player) .. " ))", ACTION)
    end)
    command("try", function(player, args)
        if args == "" then return usage(player, "/try <action>") end
        local ok = math.random() < 0.5
        M.near(player, function(p) return "* " .. M.name(player) .. " " .. args .. " | " .. tr(ok and "success" or "failure", p) end,
               ok and "#7CD67C" or "#E07070")
    end)
    command("b", function(player, args)
        if args == "" then return usage(player, "/b <text>") end
        M.near(player, "(( " .. M.name(player) .. ": " .. args .. " ))", OOC)
    end)
    command("s", function(player, args)
        if args == "" then return usage(player, "/s <text>") end
        M.near(player, function(p) return M.name(player) .. " " .. tr("shouts", p) .. ": " .. args .. "!" end, "#F2F2F2", cfg.radius * 2)
    end)
    command("w", function(player, args)
        if args == "" then return usage(player, "/w <text>") end
        M.near(player, function(p) return M.name(player) .. " " .. tr("whispers", p) .. ": " .. args end, "#A0A0A0", 2)
    end)
    command("pm", function(player, args, who)
        local text = args:match("^%S+%s+(.+)$")
        if not who or not text then return usage(player, "/pm <id or name> <text>") end
        local target = getPlayer(tonumber(who) or who)
        if not target or target.bot then return player:message(tr("noOne", player), "#E07070") end
        target:message("(( " .. tr("pmFrom", target) .. " " .. player.name .. " [" .. player.id .. "]: " .. text .. " ))", "#F2D16B")
        player:message("(( " .. tr("pmTo", player) .. " " .. target.name .. " [" .. target.id .. "]: " .. text .. " ))", "#D9B74F")
        log("PM " .. player.name .. " -> " .. target.name .. ": " .. text)
    end)
    command("o", function(player, args)
        if not cfg.ooc then return player:message(tr("oocOff", player), "#E07070") end
        if args == "" then return usage(player, "/o <text>") end
        broadcast("(( [OOC] " .. player.name .. ": " .. args .. " ))", OOC)
    end)
end

return M
