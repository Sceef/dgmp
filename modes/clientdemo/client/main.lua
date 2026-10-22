-- Runs in the player's game (docs/client-lua.md). Everything here is instant: no trip to the server.
local hud = require("hud")

local showFps = false
local fps, frames, since = 0, 0, now()
local waved = nil          -- {from =, until =}: someone waved at us
local pingSent = nil

-- The player under the crosshair (another player's body within 30 m), or nil.
local function aimedPlayer()
    local c = camera()
    local hit = raycast(c.x, c.y, c.z, c.x + c.fx * 3000, c.y + c.fy * 3000, c.z + c.fz * 3000)
    if not hit or not hit.player then return nil end
    for _, p in ipairs(players()) do if p.id == hit.player then return p end end
end

on("frame", function(dt)
    frames = frames + 1
    if now() - since >= 1 then fps, frames, since = frames, 0, now() end

    local w, h = screen()
    local m = me()
    hud.speedometer(w, h, m)
    hud.compass(w, camera().yaw)

    local target = aimedPlayer()
    if target then
        draw.text(w / 2, h / 2 + 30, target.name .. "   [G] " .. (m.lang == "ru" and "помахать" or "wave"),
                  { align = "center", size = 20, color = "#FFE08A" })
    end
    if waved and now() < waved["until"] then
        draw.text(w / 2, h * 0.25, waved.from .. (m.lang == "ru" and " машет вам" or " waves at you"), { align = "center", size = 30 })
    end
    if showFps then draw.text(w - 12, 12, fps .. " FPS", { align = "right", color = "#A0FFA0" }) end
end)

bindKey("G", function()
    local target = aimedPlayer()
    if target then send("wave", { to = target.id }) end
end)

onServer("waved", function(data)
    waved = { from = data.from, ["until"] = now() + 3 }
    playSound("ui_tutorial_x_press", { volume = 0.8 })
end)

command("fps", function() showFps = not showFps end)
command("ping", function()
    pingSent = now()
    send("ping", { at = pingSent })
end)
onServer("pong", function(data)
    if data and data.at == pingSent then chat(string.format("ping: %.0f ms", (now() - pingSent) * 1000), "#A0C4FF") end
end)

print("client demo started for " .. me().name)

