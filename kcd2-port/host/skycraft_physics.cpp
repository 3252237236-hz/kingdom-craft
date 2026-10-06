#include "skycraft_physics.hpp"

#include "skycraft_world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include <windows.h>

#include <logger/logger.hpp>
#include <sol/sol.hpp>

namespace
{
    using Cell = std::tuple<std::int32_t, std::int32_t, std::int32_t, std::uint16_t>;

    constexpr float kProxyRadius = 10.0f;
    constexpr std::size_t kMaxProxies = 384;
    constexpr std::size_t kSpawnPerTick = 12;
    constexpr std::size_t kRemovePerTick = 24;

    sol::protected_function blockCommand;
    sol::protected_function npcCommand;
    bool commandPrepared = false;
    bool npcCommandPrepared = false;
    std::map<Cell, SkyCraftSolidBlock> activeBlocks;
    std::uint64_t lastTickMs = 0;
    std::uint64_t lastReportMs = 0;
    std::uint64_t retrySpawnAfterMs = 0;
    bool reportedProbe = false;
    std::uint32_t activeEpoch = 0;

    bool PrepareCommand(sol::state_view& lua)
    {
        if (commandPrepared) {
            return blockCommand.valid();
        }
        commandPrepared = true;
        auto result = lua.safe_script(R"lua(
            local nextName = 0
            local entities = {}
            local primitiveBounds = nil
            local function key(x, y, z, part)
                return x .. ":" .. y .. ":" .. z .. ":" .. (part or 0)
            end
            local function bor(...)
                local result = 0
                for _, value in ipairs({...}) do
                    local place = 1
                    while value > 0 do
                        if value % 2 == 1 and math.floor(result / place) % 2 == 0 then
                            result = result + place
                        end
                        value = math.floor(value / 2)
                        place = place * 2
                    end
                end
                return result
            end
            return function(operation, id, x, y, z, part,
                minX, minY, minZ, maxX, maxY, maxZ)
                if not System or not System.GetEntity or not System.RemoveEntity then
                    return { error = "entity API unavailable" }
                end
                local cell = key(x, y, z, part)
                if operation == "remove" then
                    local nativeId = entities[cell]
                    entities[cell] = nil
                    local entity = nativeId and System.GetEntity(nativeId)
                    if entity and entity.GetName then
                        local name = entity:GetName()
                        if name and name:sub(1, 19) == "skycraft_collision_" then
                            System.RemoveEntity(nativeId)
                        end
                    end
                    return { ok = true }
                end
                if not System.SpawnEntity then
                    return { error = "spawn API unavailable" }
                end
                if entities[cell] then
                    local existing = System.GetEntity(entities[cell])
                    if existing and existing.GetName
                        and existing:GetName():sub(1, 19) == "skycraft_collision_" then
                        return { ok = true }
                    end
                    entities[cell] = nil
                end
                nextName = nextName + 1
                minX, minY, minZ = minX or 0, minY or 0, minZ or 0
                maxX, maxY, maxZ = maxX or 1, maxY or 1, maxZ or 1
                local center = { x = x + (minX + maxX) / 2,
                    y = -z - (minZ + maxZ) / 2, z = y + (minY + maxY) / 2 }
                local params = {
                    class = "RigidBodyEx",
                    name = "skycraft_collision_" .. nextName,
                    position = center,
                    scale = { x = 1, y = 1, z = 1 },
                    properties = {
                        bSerialize = 0,
                        bSaved_by_game = false,
                        object_Model = "Objects/special/primitive_cube.cgf",
                        AI = { bUsedAsDynamicObstacle = 1 },
                        Physics = {
                            bPhysicalize = true,
                            bRigidBody = false,
                            bRigidBodyActive = true,
                            bPushableByPlayers = false,
                            Density = -1,
                            Mass = -1,
                        },
                        MultiplayerOptions = { bNetworked = false },
                    },
                }
                local function applyBounds()
                    local low, high = primitiveBounds[1], primitiveBounds[2]
                    params.scale = { x = (maxX - minX) / (high.x - low.x),
                        y = (maxZ - minZ) / (high.y - low.y),
                        z = (maxY - minY) / (high.z - low.z) }
                    params.position = {
                        x = center.x - (low.x + high.x) / 2 * params.scale.x,
                        y = center.y - (low.y + high.y) / 2 * params.scale.y,
                        z = center.z - (low.z + high.z) / 2 * params.scale.z }
                end
                if primitiveBounds then applyBounds() end
                local entity = System.SpawnEntity(params)
                if not entity or not entity.id then
                    return { error = "RigidBodyEx spawn failed" }
                end
                if not primitiveBounds then
                    local read, low, high = false, nil, nil
                    if entity.GetLocalBBox then
                        read, low, high = pcall(entity.GetLocalBBox, entity)
                    end
                    System.RemoveEntity(entity.id)
                    if not read or not low or not high
                        or not low.x or not low.y or not low.z
                        or not high.x or not high.y or not high.z
                        or high.x - low.x < 0.001 or high.y - low.y < 0.001
                        or high.z - low.z < 0.001 then
                        return { error = "primitive cube bounds unavailable" }
                    end
                    primitiveBounds = { low, high }
                    applyBounds()
                    entity = System.SpawnEntity(params)
                    if not entity or not entity.id then
                        return { error = "scaled RigidBodyEx spawn failed" }
                    end
                end
                if not entity.DrawSlot then
                    System.RemoveEntity(entity.id)
                    return { error = "RigidBodyEx cannot hide model" }
                end
                local hidden, hideError = pcall(entity.DrawSlot, entity, 0, 0)
                if not hidden then
                    System.RemoveEntity(entity.id)
                    return { error = "model hide failed: " .. tostring(hideError) }
                end
                local interactiveClass = gcc_interactive or 262144
                local rigidClass = gcc_rigid or 0
                local npcAvoidMask = gcc_npc_avoiding_types or 0
                local npcClassNames = {}
                if type(g_PhysicsCollisionClass) == "table"
                    and bit32 and bit32.band then
                    for name, value in pairs(g_PhysicsCollisionClass) do
                        if type(value) == "number"
                            and bit32.band(value, npcAvoidMask) ~= 0 then
                            npcClassNames[#npcClassNames + 1] = tostring(name)
                        end
                    end
                    table.sort(npcClassNames)
                end
                local obstacleClass = bor(interactiveClass, rigidClass, npcAvoidMask)
                local classApplied = false
                if entity.SetPhysicParams and PHYSICPARAM_COLLISION_CLASS then
                    local called, applied = pcall(entity.SetPhysicParams, entity,
                        PHYSICPARAM_COLLISION_CLASS,
                        { collisionClass = obstacleClass, collisionClassIgnore = 0 })
                    classApplied = called and applied ~= false
                end
                local geometryApplied = false
                if entity.SetPhysicParams and PHYSICPARAM_PART_FLAGS
                    and geom_colltype_player and geom_colltype_ray then
                    local flags = bor(geom_colltype_player, geom_colltype_ray)
                    local called, applied = pcall(entity.SetPhysicParams, entity,
                        PHYSICPARAM_PART_FLAGS, { flags_mask = flags, flags = flags })
                    geometryApplied = called and applied ~= false
                end
                entities[cell] = entity.id
                local rayHit = false
                local rayCount = 0
                local firstHitIdType = "none"
                if nextName <= 5 and Physics and Physics.RayWorldIntersection
                    and player and player.id then
                    local results = {}
                    local origin = { x = center.x, y = center.y, z = center.z + 0.8 }
                    local ok, count = pcall(Physics.RayWorldIntersection,
                        origin, { x = 0, y = 0, z = -1.5 }, 1,
                        ent_all, player.id, nil, results)
                    if ok and count and count > 0 then
                        rayCount = count
                        for _, hit in pairs(results) do
                            if hit.entity and hit.entity.id then
                                firstHitIdType = type(hit.entity.id)
                            end
                            if hit.entity and hit.entity.id == entity.id then
                                rayHit = true
                                break
                            end
                        end
                    end
                end
                return { ok = true, rayHit = rayHit,
                    probe = nextName <= 5, idType = type(entity.id),
                    rayCount = rayCount, hitIdType = firstHitIdType,
                    classApplied = classApplied, obstacleClass = obstacleClass,
                    geometryApplied = geometryApplied,
                    npcAvoidMask = npcAvoidMask,
                    npcClassNames = table.concat(npcClassNames, ","),
                    dynamicObstacle = entity.Properties and entity.Properties.AI
                        and entity.Properties.AI.bUsedAsDynamicObstacle or 0,
                    rigidClass = rigidClass }
            end
        )lua", sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(ERROR) << "SkyCraft: collision command preparation failed: "
                       << error.what();
            return false;
        }
        blockCommand = result.get<sol::protected_function>();
        return true;
    }

    bool PrepareNpcCommand(sol::state_view& lua)
    {
        if (npcCommandPrepared) {
            return npcCommand.valid();
        }
        npcCommandPrepared = true;
        auto result = lua.safe_script(R"lua(
            local function key(x, y, z)
                return x .. ":" .. y .. ":" .. z
            end
            local nearbyIds = nil
            local scanTicks = 0
            local activeEpoch = nil
            return function(blocks, centerX, centerY, centerZ, epoch)
                if not player or not player.id or not System
                    or not System.GetEntities or not System.GetEntity then
                    return { status = "entity API unavailable" }
                end
                if activeEpoch ~= epoch then
                    activeEpoch = epoch
                    nearbyIds = nil
                end
                scanTicks = scanTicks + 1
                if not nearbyIds or scanTicks >= 3 then
                    local queried, entities = pcall(System.GetEntities)
                    if not queried or type(entities) ~= "table" then
                        return { status = "entity query failed" }
                    end
                    nearbyIds = {}
                    for _, entity in pairs(entities) do
                        if entity and entity ~= player and entity.id
                            and entity.id ~= player.id
                            and (entity.actor or entity.human)
                            and entity.GetWorldPos then
                            local read, pos = pcall(entity.GetWorldPos, entity)
                            if read and pos and type(pos.x) == "number"
                                and type(pos.y) == "number"
                                and type(pos.z) == "number" then
                                local dx, dy, dz = pos.x - centerX,
                                    pos.z - centerY, -pos.y - centerZ
                                if dx * dx + dz * dz <= 196
                                    and math.abs(dy) <= 5 then
                                    nearbyIds[#nearbyIds + 1] = entity.id
                                    if #nearbyIds >= 128 then break end
                                end
                            end
                        end
                    end
                    scanTicks = 0
                end
                local solids = {}
                for _, block in ipairs(blocks) do
                    local cell = key(block[1], block[2], block[3])
                    solids[cell] = solids[cell] or {}
                    solids[cell][#solids[cell] + 1] = block
                end
                local examined, moved = 0, 0
                for _, nativeId in ipairs(nearbyIds) do
                    local found, entity = pcall(System.GetEntity, nativeId)
                    if not found then entity = nil end
                    if entity and entity ~= player and entity.id
                        and entity.id ~= player.id and (entity.actor or entity.human)
                        and entity.GetWorldPos and entity.SetWorldPos then
                        local read, pos = pcall(entity.GetWorldPos, entity)
                        if read and pos and type(pos.x) == "number"
                            and type(pos.y) == "number" and type(pos.z) == "number" then
                            local x, feet, z = pos.x, pos.z, -pos.y
                            local dx, dy, dz = x - centerX,
                                feet - centerY, z - centerZ
                            if dx * dx + dz * dz <= 144 and math.abs(dy) <= 4 then
                                local alive = true
                                if entity.soul and entity.soul.GetState then
                                    local healthOk, health = pcall(entity.soul.GetState,
                                        entity.soul, "health")
                                    alive = not healthOk or type(health) ~= "number"
                                        or health > 0
                                elseif entity.actor and entity.actor.GetHealth then
                                    local healthOk, health = pcall(entity.actor.GetHealth,
                                        entity.actor)
                                    alive = not healthOk or type(health) ~= "number"
                                        or health > 0
                                end
                                if alive then
                                    examined = examined + 1
                                    local startX, startZ = x, z
                                    local radius = 0.35
                                    local low = math.floor(feet + 0.25) - 2
                                    local high = math.floor(feet + 1.7) + 1
                                    for pass = 1, 3 do
                                        local pushX, pushZ = 0, 0
                                        for bx = math.floor(x - radius) - 1,
                                            math.floor(x + radius) + 1 do
                                            for bz = math.floor(z - radius) - 1,
                                                math.floor(z + radius) + 1 do
                                                for by = low, high do
                                                  for _, box in ipairs(solids[key(bx, by, bz)] or {}) do
                                                    local minX, minY, minZ = bx + (box[4] or 0),
                                                        by + (box[5] or 0), bz + (box[6] or 0)
                                                    local maxX, maxY, maxZ = bx + (box[7] or 1),
                                                        by + (box[8] or 1), bz + (box[9] or 1)
                                                    if feet + 0.25 < maxY and feet + 1.7 > minY then
                                                    local qx = math.max(minX,
                                                        math.min(x, maxX))
                                                    local qz = math.max(minZ,
                                                        math.min(z, maxZ))
                                                    local nx, nz = x - qx, z - qz
                                                    local distance = math.sqrt(nx * nx + nz * nz)
                                                    if distance < radius then
                                                        if distance > 0.00001 then
                                                            pushX = pushX + nx / distance
                                                                * (radius - distance)
                                                            pushZ = pushZ + nz / distance
                                                                * (radius - distance)
                                                        else
                                                            local west, east = x - minX,
                                                                maxX - x
                                                            local north, south = z - minZ,
                                                                maxZ - z
                                                            local nearest = math.min(west,
                                                                east, north, south)
                                                            if nearest == west then
                                                                pushX = pushX - west - radius
                                                            elseif nearest == east then
                                                                pushX = pushX + east + radius
                                                            elseif nearest == north then
                                                                pushZ = pushZ - north - radius
                                                            else
                                                                pushZ = pushZ + south + radius
                                                            end
                                                        end
                                                    end
                                                    end
                                                  end
                                                end
                                                end
                                            end
                                        if pushX * pushX + pushZ * pushZ < 0.000001 then
                                            break
                                        end
                                        local length = math.sqrt(pushX * pushX
                                            + pushZ * pushZ)
                                        local step = math.min(length, 0.5)
                                        x = x + pushX / length * step
                                        z = z + pushZ / length * step
                                    end
                                    local totalX, totalZ = x - startX, z - startZ
                                    local total = math.sqrt(totalX * totalX
                                        + totalZ * totalZ)
                                    if total > 0.001 then
                                        local step = math.min(total, 0.5)
                                        local destination = {
                                            x = startX + totalX / total * step,
                                            y = -(startZ + totalZ / total * step),
                                            z = feet
                                        }
                                        local wrote, accepted = pcall(entity.SetWorldPos,
                                            entity, destination)
                                        if wrote and accepted ~= false then
                                            moved = moved + 1
                                        end
                                    end
                                end
                            end
                        end
                    end
                end
                return { status = "ok", examined = examined, moved = moved }
            end
        )lua", sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(ERROR) << "SkyCraft: NPC block separation unavailable: "
                       << error.what();
            return false;
        }
        npcCommand = result.get<sol::protected_function>();
        return true;
    }

    void SeparateNpcs(sol::state_view& lua,
        const skycraft::proto::SkyState& host,
        const std::vector<SkyCraftSolidBlock>& blocks)
    {
        if (blocks.empty() || !PrepareNpcCommand(lua)) {
            return;
        }
        sol::table values = lua.create_table(static_cast<int>(blocks.size()), 0);
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            sol::table block = lua.create_table(9, 0);
            block[1] = blocks[i].x;
            block[2] = blocks[i].y;
            block[3] = blocks[i].z;
            block[4] = blocks[i].minX;
            block[5] = blocks[i].minY;
            block[6] = blocks[i].minZ;
            block[7] = blocks[i].maxX;
            block[8] = blocks[i].maxY;
            block[9] = blocks[i].maxZ;
            values[i + 1] = block;
        }
        auto result = npcCommand(values, host.posX, host.posY, host.posZ,
            host.collisionEpoch);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(WARNING) << "SkyCraft: NPC block separation failed: "
                         << error.what();
            return;
        }
        const sol::object value = result.get<sol::object>();
        if (!value.is<sol::table>()) {
            return;
        }
        const sol::table response = value.as<sol::table>();
        const auto status = response.get_or("status", std::string{});
        if (status != "ok") {
            if (::GetTickCount64() - lastReportMs >= 10000) {
                LOG(WARNING) << "SkyCraft: NPC block separation " << status;
            }
            return;
        }
        const auto moved = response.get_or("moved", 0);
        if (moved > 0 && ::GetTickCount64() - lastReportMs >= 10000) {
            LOG(INFO) << "SkyCraft: separated " << moved << " of "
                      << response.get_or("examined", 0)
                      << " nearby KCD2 NPCs from Minecraft blocks";
        }
    }

    bool Remove(const Cell& cell)
    {
        const auto [x, y, z, part] = cell;
        auto result = blockCommand("remove", 0, x, y, z, part);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(WARNING) << "SkyCraft: collision proxy removal failed: "
                         << error.what();
            return false;
        }
        const sol::object value = result.get<sol::object>();
        return value.is<sol::table>() && value.as<sol::table>().get_or("ok", false);
    }

    bool Spawn(const SkyCraftSolidBlock& block)
    {
        auto result = blockCommand("spawn", 0, block.x, block.y, block.z,
            block.part, block.minX, block.minY, block.minZ,
            block.maxX, block.maxY, block.maxZ);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(WARNING) << "SkyCraft: collision proxy spawn failed: "
                         << error.what();
            return false;
        }
        const sol::object value = result.get<sol::object>();
        if (!value.is<sol::table>()) {
            return false;
        }
        const sol::table response = value.as<sol::table>();
        if (!response.get_or("ok", false)) {
            LOG(WARNING) << "SkyCraft: collision proxy spawn rejected: "
                         << response.get_or("error", std::string{});
            return false;
        }
        if (response.get_or("probe", false) && !reportedProbe) {
            const bool hit = response.get_or("rayHit", false);
            if (hit) {
                reportedProbe = true;
            }
            LOG(INFO) << "SkyCraft: collision proxy ray hit " << hit
                      << ", entity ID type "
                      << response.get_or("idType", std::string{})
                      << ", ray count " << response.get_or("rayCount", 0)
                      << ", hit ID type "
                      << response.get_or("hitIdType", std::string{})
                      << ", interactive class "
                      << response.get_or("obstacleClass", 0)
                      << " applied " << response.get_or("classApplied", false)
                      << ", living/ray geometry flags "
                      << response.get_or("geometryApplied", false)
                      << ", rigid class "
                      << response.get_or("rigidClass", 0)
                      << ", NPC avoid mask "
                      << response.get_or("npcAvoidMask", 0)
                      << " (" << response.get_or("npcClassNames", std::string{})
                      << "), dynamic obstacle "
                      << response.get_or("dynamicObstacle", 0);
        }
        return true;
    }
}

void SkyCraftPhysicsTick(lua_State* state, const skycraft::proto::SkyState& host,
    bool minecraftAlive)
{
    const auto now = ::GetTickCount64();
    if (now - lastTickMs < 200) {
        return;
    }
    lastTickMs = now;
    skycraft::proto::McState mc{};
    const bool active = minecraftAlive
        && (host.flags & skycraft::proto::kSkyInGame) != 0
        && SkyCraftBridgeReadMcState(&mc)
        && (mc.flags & skycraft::proto::kMcInWorld) != 0;
    if (!active && activeBlocks.empty()) {
        return;
    }
    sol::state_view lua(state);
    if (!PrepareCommand(lua)) {
        return;
    }
    std::vector<SkyCraftSolidBlock> desired;
    if (active) {
        SkyCraftWorldCollectNearbySolids(host.posX, host.posY, host.posZ,
            kProxyRadius, kMaxProxies, desired);
    }
    const bool changingWorld = active && activeEpoch != host.collisionEpoch;
    std::map<Cell, SkyCraftSolidBlock> desiredCells;
    for (const auto& block : desired) {
        if (!changingWorld)
            desiredCells.emplace(Cell{block.x, block.y, block.z, block.part}, block);
    }
    if (active) {
        SeparateNpcs(lua, host, desired);
    }
    std::size_t removed = 0;
    for (auto it = activeBlocks.begin(); it != activeBlocks.end()
        && removed < kRemovePerTick;) {
        const auto wanted = desiredCells.find(it->first);
        if (wanted != desiredCells.end() && wanted->second == it->second) {
            ++it;
            continue;
        }
        if (!Remove(it->first)) {
            break;
        }
        it = activeBlocks.erase(it);
        ++removed;
    }
    std::size_t spawned = 0;
    std::size_t attempts = 0;
    for (const auto& block : desired) {
        if (changingWorld || attempts >= kSpawnPerTick || now < retrySpawnAfterMs) {
            break;
        }
        const Cell cell{block.x, block.y, block.z, block.part};
        if (activeBlocks.contains(cell)) {
            continue;
        }
        ++attempts;
        if (Spawn(block)) {
            activeBlocks.emplace(cell, block);
            ++spawned;
        } else {
            retrySpawnAfterMs = now + 3000;
            break;
        }
    }
    if (changingWorld && activeBlocks.empty()) activeEpoch = host.collisionEpoch;
    if (now - lastReportMs >= 10000) {
        lastReportMs = now;
        LOG(INFO) << "SkyCraft: collision proxies " << activeBlocks.size()
                  << " of " << desired.size() << " nearby solids";
    }
}
