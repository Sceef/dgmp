-- Client scripts demo (docs/client-lua.md): client/main.lua runs in every player's game. It draws a
-- speedometer and a compass, names the player you aim at, and sends "wave" when you press G at them;
-- this file answers what the scripts send (send / on("clientMessage")) and talks back (player:clientSend).

local GREEN = "#7CD67C"

on("playerJoin", function(player)
    player:message("Client scripts demo: aim at a player and press G to wave; /fps, /ping.", GREEN)
end)

on("clientMessage", function(player, name, data)
    if name == "ping" then
        player:clientSend("pong", data)                 -- the script measures the round trip
    elseif name == "wave" and type(data) == "table" then
        local to = getPlayer(tonumber(data.to) or -1)
        if not to or to == player then return end
        local x, y, z = player:position()
        local tx, ty, tz = to:position()
        if (x - tx) ^ 2 + (y - ty) ^ 2 + (z - tz) ^ 2 > 4000 ^ 2 then return end  -- 40 m: the server checks what a client claims
        player:playAnim("wave")
        to:clientSend("waved", { from = player.name })
    end
end)
