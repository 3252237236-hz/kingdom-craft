local commands = {}
local messages = {}
local spawned = {}
local removed = {}
local nextId = 100
local hit = {
    pos = { x = 1.2, y = 2.2, z = 3.2 },
    normal = { x = 0, y = 0, z = 1 },
}
local listener

System = {
    Log = function(message)
        messages[#messages + 1] = message
    end,
    AddCCommand = function(name, expression)
        commands[name] = expression
    end,
    GetViewCameraPos = function()
        return { x = 1, y = 2, z = 5 }
    end,
    GetViewCameraDir = function()
        return { x = 0, y = 0, z = -1 }
    end,
    SpawnEntity = function(params)
        local entity = { id = nextId, params = params }
        nextId = nextId + 1
        spawned[#spawned + 1] = entity
        return entity
    end,
    GetEntity = function(id)
        for _, entity in ipairs(spawned) do
            if entity.id == id and not removed[id] then
                return entity
            end
        end
        return nil
    end,
    RemoveEntity = function(id)
        removed[id] = true
    end,
}

Physics = {
    RayWorldIntersection = function(_, _, _, _, _, _, results)
        results[1] = hit
        return 1
    end,
}

Game = {
    IsLoadingEngineSaveGame = function()
        return false
    end,
    LogGameEvent = function(message)
        messages[#messages + 1] = message
    end,
}

UIAction = {
    RegisterEventSystemListener = function(value)
        listener = value
    end,
}

ent_all = 1
player = { id = 1 }

dofile("kcd2-port/content/Scripts/Startup/skycraft_kcd.lua")
assert(commands.skycraft_place == "skycraftKcdPlace()")
assert(commands.skycraft_break == "skycraftKcdBreak()")
assert(commands.skycraft_clear == "skycraftKcdClear()")
assert(commands.skycraft_status == "skycraftKcdStatus()")
assert(listener ~= nil)
listener:OnGameplayStarted()

skycraftKcdPlace()
assert(#spawned == 1)
assert(spawned[1].params.class == "BasicEntity")
assert(spawned[1].params.position.x == 1)
assert(spawned[1].params.position.y == 2)
assert(spawned[1].params.position.z == 4)
assert(spawned[1].params.properties.Physics.bRigidBody == false)

skycraftKcdPlace()
assert(#spawned == 1)
hit.entity = spawned[1]
skycraftKcdBreak()
assert(removed[spawned[1].id])

hit.entity = nil
hit.pos.x = 5.2
skycraftKcdPlace()
assert(#spawned == 2)
skycraftKcdClear()
assert(removed[spawned[2].id])

local lastMessage = messages[#messages]
skycraftKcdStatus()
assert(messages[#messages] == "SkyCraft: 0 blocks in this session")
assert(lastMessage ~= messages[#messages])

Physics.RayWorldIntersection = function()
    error("raycast failed")
end
skycraftKcdPlace()
assert(string.find(messages[#messages], "operation failed", 1, true))

print("block prototype mock tests passed")
