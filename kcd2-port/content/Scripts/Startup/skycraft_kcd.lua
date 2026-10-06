local mod = {
    blocksByCell = {},
    blocksById = {},
    count = 0,
    nextName = 1,
}

local maxBlocks = 256
local reach = 6
local cubeModel = "EngineAssets/Objects/primitive_cube.cgf"

local function log(message)
    if System and System.Log then
        System.Log("[SkyCraft KCD2] " .. tostring(message))
    end
end

local function notice(message)
    log(message)
    if Game and Game.LogGameEvent then
        Game.LogGameEvent("SkyCraft: " .. tostring(message))
    end
end

local function cellKey(x, y, z)
    return x .. ":" .. y .. ":" .. z
end

local function snap(value)
    return math.floor(value + 0.5)
end

local function gameplayReady()
    if not System or not System.SpawnEntity or not System.RemoveEntity then
        return false
    end
    if not Physics or not Physics.RayWorldIntersection then
        return false
    end
    if not player or not player.id then
        return false
    end
    return not (Game and Game.IsLoadingEngineSaveGame and Game.IsLoadingEngineSaveGame())
end

local function lookHit()
    if not gameplayReady() then
        return nil, "wait until a save is loaded"
    end
    local origin = System.GetViewCameraPos()
    local direction = System.GetViewCameraDir()
    if not origin or not direction then
        return nil, "camera is unavailable"
    end
    local ray = {
        x = direction.x * reach,
        y = direction.y * reach,
        z = direction.z * reach,
    }
    local results = {}
    local count = Physics.RayWorldIntersection(origin, ray, 1, ent_all, player.id, nil, results)
    if not count or count < 1 or not results[1] or not results[1].pos then
        return nil, "look at a nearby surface"
    end
    return results[1], direction
end

local function placeBlock()
    if mod.count >= maxBlocks then
        notice("block limit reached")
        return
    end
    local hit, direction = lookHit()
    if not hit then
        notice(direction)
        return
    end
    local normal = hit.normal or hit.n or {
        x = -direction.x,
        y = -direction.y,
        z = -direction.z,
    }
    local x = snap(hit.pos.x + normal.x * 0.51)
    local y = snap(hit.pos.y + normal.y * 0.51)
    local z = snap(hit.pos.z + normal.z * 0.51)
    local key = cellKey(x, y, z)
    if mod.blocksByCell[key] then
        notice("a block is already there")
        return
    end
    local name = "skycraft_block_" .. mod.nextName
    local entity = System.SpawnEntity({
        class = "BasicEntity",
        name = name,
        position = { x = x, y = y, z = z },
        properties = {
            bSaved_by_game = false,
            object_Model = cubeModel,
            Physics = {
                bPhysicalize = true,
                bRigidBody = false,
                bPushableByPlayers = false,
                Density = -1,
                Mass = -1,
            },
            MultiplayerOptions = { bNetworked = false },
        },
    })
    if not entity or not entity.id then
        notice("could not create block")
        return
    end
    local block = { id = entity.id, key = key }
    mod.blocksByCell[key] = block
    mod.blocksById[entity.id] = block
    mod.count = mod.count + 1
    mod.nextName = mod.nextName + 1
    notice("placed block at " .. x .. ", " .. y .. ", " .. z)
end

local function removeBlock()
    local hit, errorText = lookHit()
    if not hit then
        notice(errorText)
        return
    end
    local id = hit.entity and hit.entity.id
    local block = id and mod.blocksById[id]
    if not block then
        notice("look at a SkyCraft block")
        return
    end
    System.RemoveEntity(block.id)
    mod.blocksByCell[block.key] = nil
    mod.blocksById[block.id] = nil
    mod.count = mod.count - 1
    notice("removed block")
end

local function clearBlocks()
    local removed = 0
    for id in pairs(mod.blocksById) do
        if System.GetEntity and System.GetEntity(id) then
            System.RemoveEntity(id)
        end
        removed = removed + 1
    end
    mod.blocksByCell = {}
    mod.blocksById = {}
    mod.count = 0
    notice("cleared " .. removed .. " blocks")
end

local function run(command)
    local ok, errorText = pcall(command)
    if not ok then
        notice("operation failed: " .. tostring(errorText))
    end
end

function skycraftKcdPlace()
    run(placeBlock)
end

function skycraftKcdBreak()
    run(removeBlock)
end

function skycraftKcdClear()
    run(clearBlocks)
end

function skycraftKcdStatus()
    notice(mod.count .. " blocks in this session")
end

function mod:OnGameplayStarted()
    self.blocksByCell = {}
    self.blocksById = {}
    self.count = 0
    log("gameplay started; block commands ready")
end

if System and System.AddCCommand then
    System.AddCCommand("skycraft_place", "skycraftKcdPlace()", "Place a block at the aimed surface")
    System.AddCCommand("skycraft_break", "skycraftKcdBreak()", "Remove the aimed SkyCraft block")
    System.AddCCommand("skycraft_clear", "skycraftKcdClear()", "Remove all SkyCraft blocks from this session")
    System.AddCCommand("skycraft_status", "skycraftKcdStatus()", "Report SkyCraft block count")
end

if UIAction and UIAction.RegisterEventSystemListener then
    UIAction.RegisterEventSystemListener(mod, "System", "OnGameplayStarted", "OnGameplayStarted")
end

log("block prototype 0.1.1 loaded")
