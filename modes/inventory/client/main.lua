-- Setting a trap from the backpack by aiming (the server's "place"): a ghost of it stands where you look; F puts
-- it there, G cancels. A tripwire takes two F: its first end, then the other one (the line to be is drawn).
-- The server checks the points and sets the trap ("placeAt").
local GHOST = {
    mine = "/Game/Weapons/Trap_Weapons/Mines/ProximityMine/proximity_proxy.proximity_proxy",
    beartrap = "/Game/Environments/ManMade/Props/beartrap/BearTrap_Closed_Mesh.BearTrap_Closed_Mesh",
    wire = "/Game/Weapons/Trap_Weapons/Mines/IED/IED.IED",   -- the IED at the wire's first end
}
local RANGE = 400     -- units: as far from you as a point may be put (REACH in main.lua)
local placing = nil   -- {kind, maxLength, a = a wire's first end, ghost = object id}

local function ru() return me().lang == "ru" end
local function aimed()
    local c = camera()
    local hit = raycast(c.x, c.y, c.z, c.x + c.fx * 1500, c.y + c.fy * 1500, c.z + c.fz * 1500)
    if not hit or hit.player then return nil end
    local m = me()
    if (hit.x - m.x) ^ 2 + (hit.y - m.y) ^ 2 + (hit.z - m.z) ^ 2 > RANGE ^ 2 then return nil end
    return hit
end
local function stop()
    if placing and placing.ghost then removeObject(placing.ghost) end
    placing = nil
    unbindKey("F"); unbindKey("G")
end

local function put()
    local p = aimed()
    if not p then return end
    local yaw = camera().yaw
    if placing.kind ~= "wire" then
        send("placeAt", { kind = placing.kind, a = { x = p.x, y = p.y, z = p.z }, yaw = yaw })
        return stop()
    end
    if not placing.a then
        placing.a = p   -- the ghost IED stays at the first end; you walk on stretching the wire
        send("wireStart", { a = { x = p.x, y = p.y, z = p.z } })
        playSound("ui_tutorial_x_press", { volume = 0.5 })
        return
    end
    local len = math.sqrt((p.x - placing.a.x) ^ 2 + (p.y - placing.a.y) ^ 2) / 100
    if len < 1 or len > placing.maxLength then
        chat(ru() and ("Растяжка — от 1 до " .. placing.maxLength .. " м.") or ("A wire is 1 to " .. placing.maxLength .. " m long."), "#E07070")
        return
    end
    send("placeAt", { kind = "wire", a = { x = placing.a.x, y = placing.a.y, z = placing.a.z }, b = { x = p.x, y = p.y, z = p.z } })
    stop()
end

onServer("wireRefused", function() if placing then placing.a = nil end end)  -- (the first end was too far)

onServer("place", function(data)
    stop()
    placing = { kind = data.kind, maxLength = data.maxLength or 10 }
    bindKey("F", put, { block = true })   -- (the game's F would grab or interact)
    bindKey("G", stop, { block = true })
end)

local HINT = {
    first = { en = "F: put it here, G: cancel", ru = "F — поставить сюда, G — отмена" },
    wire1 = { en = "F: the wire's first end, G: cancel", ru = "F — первый конец растяжки, G — отмена" },
    wire2 = { en = "F: the other end, G: cancel", ru = "F — второй конец, G — отмена" },
}
on("frame", function()
    if not placing then return end
    local w, h = screen()
    local p = aimed()
    local hint = placing.kind ~= "wire" and HINT.first or placing.a and HINT.wire2 or HINT.wire1
    draw.text(w / 2, h * 0.72, ru() and hint.ru or hint.en, { align = "center", size = 20 })
    -- The ghost: where it would stand now (a wire's IED: at the first end once it is set).
    local at = (placing.kind == "wire" and placing.a) or p
    if at then
        local fields = { x = at.x, y = at.y, z = at.z, yaw = camera().yaw, ghost = true }
        if placing.ghost then setObject(placing.ghost, fields)
        else fields.asset = GHOST[placing.kind]; placing.ghost = createObject(fields) end
    end
    if not p then
        draw.text(w / 2, h * 0.72 + 26, ru() and "слишком далеко" or "too far", { align = "center", size = 16, color = "#E07070" })
        return
    end
    if placing.kind == "wire" and placing.a then
        local ax, ay = worldToScreen(placing.a.x, placing.a.y, placing.a.z + 30)
        local bx, by = worldToScreen(p.x, p.y, p.z + 30)
        if ax and bx then
            local len = math.sqrt((p.x - placing.a.x) ^ 2 + (p.y - placing.a.y) ^ 2) / 100
            local ok = len >= 1 and len <= placing.maxLength
            draw.line(ax, ay, bx, by, ok and "#FFE08A" or "#E07070", 2)
            draw.circle(bx, by, 5, ok and "#FFE08A" or "#E07070")
            draw.text(bx + 10, by - 24, string.format("%.1f m", len), { color = ok and "#FFE08A" or "#E07070" })
        end
    end
end)
