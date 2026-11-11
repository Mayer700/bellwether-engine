-- Every currently implemented scripting API appears here and is executed by CTest.
local entity = Engine.CreateEntity("Created in Lua")
assert(Engine.Exists(entity))
Engine.SetPosition(entity, 1, 2, 3)
local x, y, z = Engine.GetPosition(entity)
assert(x == 1 and y == 2 and z == 3)
Engine.SetRotation(entity, 0.1, 0.5, -0.25)
local rx, ry, rz = Engine.GetRotation(entity)
assert(math.abs(rx - 0.1) < 1e-5 and math.abs(ry - 0.5) < 1e-5 and math.abs(rz + 0.25) < 1e-5)
Engine.SetScale(entity, 1.5, 2.0, 0.75)
local sx, sy, sz = Engine.GetScale(entity)
assert(math.abs(sx - 1.5) < 1e-5 and math.abs(sy - 2.0) < 1e-5 and math.abs(sz - 0.75) < 1e-5)
assert(Engine.GetName(entity) == "Created in Lua")
Engine.Rename(entity, "Renamed in Lua")
assert(Engine.GetName(entity) == "Renamed in Lua")

-- Safe entity iteration while mutating entities
local listed = Engine.GetEntities()
assert(#listed >= 1)
for _, id in ipairs(listed) do
    if id == entity then
        local spawnedDuringLoop = Engine.CreateEntity("Transient Loop Child")
        assert(Engine.Exists(spawnedDuringLoop))
        Engine.DestroyEntity(spawnedDuringLoop)
    end
end
Engine.DestroyEntity(entity)
assert(not Engine.Exists(entity))

-- Snow queries, deformable tracks, thermal melt, and winter atmosphere
local virginDepth = Engine.GetSnowDepth(0, 0)
assert(virginDepth > 0.1)
assert(Engine.StampSnow(0, 0, 1.5, 0.3, 0.4))
local trackedDepth = Engine.GetSnowDepth(0, 0)
local compression = Engine.GetSnowCompression(0, 0)
local speedFactor = Engine.GetSnowSpeedFactor(0, 0)
assert(trackedDepth < virginDepth)
assert(compression > 0)
assert(speedFactor > 0 and speedFactor <= 1.0)
assert(Engine.MeltSnow(3.0, 3.0, 1.25, 20.0, 0.5))
assert(Engine.GetSnowDepth(3.0, 3.0) < virginDepth)

Engine.SetWind(4.5, -0.5, -2.0)
local wx, wy, wz = Engine.GetWind()
assert(math.abs(wx - 4.5) < 1e-5 and math.abs(wy + 0.5) < 1e-5 and math.abs(wz + 2.0) < 1e-5)
Engine.SetSnowfall(0.85, 0.9, 1.1, 0.7)

local prefab = Engine.SpawnPrefab([[{"Name":"Prefab Child","Transform":{"Translation":[0,0,0],"Rotation":[0,0,0],"Scale":[1,1,1]},"Script":""}]])
assert(Engine.Exists(prefab))
function OnUpdate(timestep)
    local px, py, pz = Engine.GetPosition(prefab)
    Engine.SetPosition(prefab, px + timestep, py, pz)
end
