-- discord_log.lua — a server script (server.ini: Scripts=discord_log): joins, quits, bans, anti-cheat
-- strikes and chat commands of admins go to a Discord channel through a webhook. Works with any mode.
-- Discord: channel settings > Integrations > Webhooks > New webhook > Copy URL, and paste it here.
local WEBHOOK = ""   -- "https://discord.com/api/webhooks/<id>/<token>"

if WEBHOOK == "" then
    log("discord_log: no webhook set (scripts/discord_log.lua)")
    return
end

local queue, sending = {}, false
local function flush()
    if sending or #queue == 0 then return end
    sending = true
    local text = table.concat(queue, "\n", 1, math.min(#queue, 15))
    for _ = 1, math.min(#queue, 15) do table.remove(queue, 1) end
    http.post(WEBHOOK, { content = text, allowed_mentions = { parse = {} } }, function(ok, status, body, err)
        sending = false
        if not ok then log("discord_log: " .. tostring(err)) end
    end)
end
local function post(line) queue[#queue + 1] = line:gsub("@", "@\u{200B}") end   -- no pings
every(2000, flush)   -- a few lines at once: Discord limits the rate of webhooks

local function who(p) return p.name .. (p.steamId and (" (" .. p.steamId .. ")") or "") end
on("playerJoin", function(p) post(":inbox_tray: " .. who(p) .. " joined") end)
on("playerQuit", function(p, reason) post(":outbox_tray: " .. who(p) .. " left (" .. reason .. ")") end)
on("cheat", function(p, kind, info) post(":warning: anti-cheat: " .. who(p) .. " " .. kind .. " — " .. info.detail .. " (" .. info.strikes .. ")") end)
on("playerDeath", function(p, inv, killer) if killer then post(":skull: " .. p.name .. " was killed by " .. killer.name) end end)
