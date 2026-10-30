-- Localised values retain both languages through concatenation and formatting.
-- Resolve at the player/API boundary, never through a shared "current language".
local L,mt={},{}
local function is(v) return type(v)=="table" and getmetatable(v)==mt end
local function text(v,lang)
    if is(v) then return v[lang] end
    return v
end
function L.new(ru,en)
    assert(type(ru)=="string" and type(en)=="string" and en~="","Both translations are required")
    return setmetatable({ru=ru,en=en},mt)
end
function L.resolve(v,p) return text(v,p and p.lang=="ru" and "ru" or "en") end
function L.deep(v,p)
    if is(v) then return L.resolve(v,p) end
    if type(v)~="table" then return v end
    local out={}; for k,value in pairs(v) do out[k]=L.deep(value,p) end
    return out
end
function L.format(template,...)
    local args=table.pack(...)
    local function format(lang)
        local values={}; for i=1,args.n do values[i]=text(args[i],lang) end
        return string.format(text(template,lang),table.unpack(values,1,args.n))
    end
    return L.new(format("ru"),format("en"))
end
mt.__index={format=L.format}
mt.__tostring=function(v) return v.en end
mt.__concat=function(a,b) return L.new(text(a,"ru")..text(b,"ru"),text(a,"en")..text(b,"en")) end
return setmetatable(L,{__call=function(_,ru,en) return L.new(ru,en) end})
