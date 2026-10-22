-- Drawing helpers of the demo (require("hud")): a speedometer and a compass strip.
local hud = {}

function hud.speedometer(w, h, m)
    local kmh = m.speed * 0.036            -- units (cm) a second -> km/h
    local x, y = w - 190, h - 120
    draw.rect(x, y, 170, 70, "#00000088", { rounding = 8 })
    draw.text(x + 85, y + 6, string.format("%d", math.floor(kmh + 0.5)), { align = "center", size = 40 })
    draw.text(x + 85, y + 48, m.onBike and "km/h  ·  bike" or "km/h", { align = "center", size = 14, color = "#C0C0C0" })
end

local MARKS = { [0] = "N", [45] = "NE", [90] = "E", [135] = "SE", [180] = "S", [225] = "SW", [270] = "W", [315] = "NW" }
function hud.compass(w, yaw)
    local cx, y, span = w / 2, 18, 360                 -- 360 pixels show 120 degrees
    draw.rect(cx - span / 2, y - 4, span, 28, "#00000066", { rounding = 6 })
    -- The game's yaw 0 looks along +X; the map's north is -Y there (90 degrees to the left).
    local heading = (yaw + 90) % 360
    for deg = 0, 345, 15 do
        local d = (deg - heading + 540) % 360 - 180
        if math.abs(d) <= 60 then
            local x = cx + d * span / 120
            if MARKS[deg] then draw.text(x, y, MARKS[deg], { align = "center", size = 18, color = deg == 0 and "#FF8080" or "#FFFFFF" })
            else draw.line(x, y + 6, x, y + 14, 0xFFFFFFAA, 2) end
        end
    end
    draw.line(cx, y + 22, cx, y + 28, "#FFE08A", 2)
end

return hud
