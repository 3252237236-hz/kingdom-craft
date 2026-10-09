#include "skycraft_combat.hpp"
#include "skycraft_attack_approvals.hpp"
#include "skycraft_mobs.hpp"
#include "../bridge/skycraft_kcd2_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <set>
#include <string>
#include <vector>
#include <windows.h>

#include <logger/logger.hpp>
#include <sol/sol.hpp>

namespace
{
    using namespace skycraft::proto;

    sol::protected_function combatCommand;
    sol::protected_function explosionCommand;
    bool commandPrepared = false;
    bool explosionPrepared = false;
    std::set<std::uint32_t> publishedIds;
    std::uint64_t lastScanMs = 0;
    std::uint64_t lastReportMs = 0;
    std::uint64_t lastUnsupportedReportMs = 0;
    std::uint64_t lastHealthPollMs = 0;
    std::uint64_t lastHealthWarningMs = 0;
    std::uint32_t unsupportedEvents = 0;
    float pendingPlayerDamage = 0;
    bool minecraftDeathHandled = false;
    std::uint64_t lastDeathAttemptMs = 0;
    bool combatWasActive = false;
    std::uint32_t activeCollisionEpoch = 0;
    struct QueuedTrace {
        skycraft::kcd2::AttackTrace trace;
        std::uint64_t received;
        std::uint32_t request = 0, mode = 0; // 0 legacy, 1 probe, 2 commit
        skycraft::kcd2::MobAttackIdentity identity{};
    };
    std::deque<QueuedTrace> attackTraces;
    skycraft::kcd2::AttackApprovals approvedAttacks;
    std::deque<skycraft::kcd2::AttackResult> attackReplies;

    bool PrepareCommand(sol::state_view& lua)
    {
        if (commandPrepared) {
            return combatCommand.valid();
        }
        commandPrepared = true;
        auto result = lua.safe_script(R"lua(
            local nextKey = 0
            local keysByNativeId = {}
            local nativeByKey = {}
            local lastPlayerId = nil
            local lastPlayerHealth = nil
            local deathSubmitted = false
            local pendingDeaths = {}
            local deathReports = {}
            local pushes = {}
            local lastPushTick = nil
            local cachedActors = nil
            local lastActorQuery = 0
            local querySeen = 0
            local function readHealth(entity)
                -- KCD2's own character tests use soul health, not CryEngine's
                -- generic actor HP. A failed soul read must not silently fall back.
                if entity.soul and entity.soul.GetState then
                    local ok, value = pcall(entity.soul.GetState, entity.soul, "health")
                    if ok and type(value) == "number" then return value, "soul" end
                    return nil, "soul"
                end
                if entity.actor and entity.actor.GetHealth then
                    local ok, value = pcall(entity.actor.GetHealth, entity.actor)
                    if ok and type(value) == "number" then return value, "actor" end
                end
                return nil, "actor"
            end
            local function writeHealth(entity, value, source)
                if source == "soul" then
                    if entity.soul and entity.soul.SetState then
                        return pcall(entity.soul.SetState, entity.soul, "health", value)
                    end
                elseif entity.actor and entity.actor.SetHealth then
                    return pcall(entity.actor.SetHealth, entity.actor, value)
                end
                return false
            end
            local isDead
            local function movePushes(nowMs)
                local dt = lastPushTick and math.min(0.05, math.max(0, (nowMs - lastPushTick) / 1000)) or 0
                lastPushTick = nowMs
                local moved, blocked, failed = 0, 0, 0
                for key, push in pairs(pushes) do
                    local target = System.GetEntity(push.id)
                    if not target or isDead(target) or nowMs - push.at > 650 then
                        pushes[key] = nil
                    elseif dt > 0 then
                        local ok, pos = pcall(target.GetWorldPos, target)
                        if not ok or not pos then
                            pushes[key] = nil
                            failed = failed + 1
                        else
                            local dx, dy = push.vx * dt, push.vy * dt
                            local distance = math.sqrt(dx * dx + dy * dy)
                            local fraction = 1
                            if distance > 0.0001 then
                                -- Sweep the actor's width and body at three heights.
                                -- Native world/MC obstacle colliders stop the push.
                                for _, height in ipairs({0.15, 0.8, 1.5}) do
                                    for _, side in ipairs({-1, 0, 1}) do
                                        local hits = {}
                                        local origin = { x=pos.x-dy/distance*push.radius*side,
                                            y=pos.y+dx/distance*push.radius*side, z=pos.z+height }
                                        local tested, count = pcall(Physics.RayWorldIntersection,
                                            origin, {x=dx/distance*(distance+push.radius),
                                                y=dy/distance*(distance+push.radius),z=0},
                                            1, ent_all, target.id, player.id, hits)
                                        if not tested then
                                            fraction = 0
                                            failed = failed + 1
                                        elseif count and count > 0 and hits[1] then
                                            local hit = hits[1]
                                            local hitDistance = hit.dist
                                            if not hitDistance and hit.pos then
                                                hitDistance = math.sqrt((hit.pos.x-origin.x)^2+(hit.pos.y-origin.y)^2)
                                            end
                                            fraction = math.min(fraction,
                                                math.max(0, ((hitDistance or 0) - push.radius - 0.04) / distance))
                                        end
                                    end
                                end
                            end
                            local destination = {x=pos.x+dx*fraction,y=pos.y+dy*fraction,z=pos.z}
                            local accepted, result = pcall(target.SetWorldPos, target, destination)
                            local read, actual = pcall(target.GetWorldPos, target)
                            if accepted and result ~= false and read and actual
                                and math.sqrt((actual.x-destination.x)^2+(actual.y-destination.y)^2) < 0.05 then
                                if math.sqrt((actual.x-pos.x)^2+(actual.y-pos.y)^2) > 0.001 then moved = moved + 1 end
                                if fraction < 1 then blocked = blocked + 1 end
                            else
                                failed = failed + 1
                                pushes[key] = nil
                            end
                            -- Vanilla ground drag per 20 Hz tick; preserve native
                            -- floor height and gravity instead of teleporting upward.
                            local drag = 0.546 ^ (dt * 20)
                            push.vx, push.vy = push.vx * drag, push.vy * drag
                            if fraction < 1 or math.sqrt(push.vx^2+push.vy^2) < 0.04 then pushes[key] = nil end
                        end
                    end
                end
                return {status="knockback tick",moved=moved,blocked=blocked,failed=failed}
            end
            isDead = function(entity)
                if entity.actor and entity.actor.IsDead then
                    local ok, value = pcall(entity.actor.IsDead, entity.actor)
                    if ok and type(value) == "boolean" then return value end
                end
                local health = readHealth(entity)
                if health and health <= 0 then return true end
                -- Generic actor HP is only a death signal, never Soul damage HP.
                if entity.actor and entity.actor.GetHealth then
                    local ok, value = pcall(entity.actor.GetHealth, entity.actor)
                    if ok and type(value) == "number" and value <= 0 then return true end
                end
                return false
            end
            local function pollDeath(entity, nowMs)
                local pending = pendingDeaths[tostring(entity.id)]
                if not pending then return nil end
                if isDead(entity) then
                    if pending.status ~= "death observed" then
                        pending.status = "death observed"
                        deathReports[#deathReports + 1] = { nativeId = tostring(entity.id), status = pending.status }
                    end
                elseif pending.status == "death submitted" and nowMs - pending.at >= 3000 then
                    pending.status = "death unconfirmed"
                    if entity.actor and entity.actor.IsUnconscious then
                        local ok, unconscious = pcall(entity.actor.IsUnconscious, entity.actor)
                        pending.unconscious = ok and unconscious == true
                    end
                        deathReports[#deathReports + 1] = { nativeId = tostring(entity.id), status = pending.status }
                end
                return pending.status
            end
            local function equippedInterception(target, origin, point, length, bodyDistance)
                -- These are registered WHGame interfaces: GetItemInHand returns
                -- an inventory handle, GetItem exposes its live entity number,
                -- and ToLocal(-1, point) uses that entity's current world transform.
                -- Use the actual held mesh bounds, never a guessed guard angle.
                if not target.human or not target.human.GetItemInHand
                    or not ItemManager or not ItemManager.GetItem then return nil end
                local function finite(value)
                    return type(value)=="number" and value==value and math.abs(value)<1e10
                end
                local function vector(value)
                    return type(value)=="table" and finite(value.x) and finite(value.y) and finite(value.z)
                end
                for _, hand in ipairs({HS_RIGHT, HS_LEFT}) do
                    local held, handle=pcall(target.human.GetItemInHand,target.human,hand)
                    if held and handle then
                        local read, item=pcall(ItemManager.GetItem,handle)
                        if read and type(item)=="table" and item.entity and item.entity~=0 then
                            local found, equipment=pcall(System.GetEntity,item.entity)
                            if found and equipment and equipment.id~=target.id
                                and equipment.GetLocalBBox and equipment.ToLocal then
                                local visible=true
                                if equipment.IsHidden then
                                    local ok,hidden=pcall(equipment.IsHidden,equipment)
                                    visible=ok and hidden~=true and hidden~=1
                                end
                                local bounds,lo,hi=pcall(equipment.GetLocalBBox,equipment)
                                local first,from=pcall(equipment.ToLocal,equipment,-1,origin)
                                local last,to=pcall(equipment.ToLocal,equipment,-1,point)
                                if visible and bounds and first and last
                                    and vector(lo) and vector(hi) and vector(from) and vector(to) then
                                    local enter,leave=0,1
                                    for _,axis in ipairs({"x","y","z"}) do
                                        local delta=to[axis]-from[axis]
                                        if hi[axis]<=lo[axis] then enter=2;break end
                                        if math.abs(delta)<1e-8 then
                                            if from[axis]<lo[axis] or from[axis]>hi[axis] then enter=2;break end
                                        else
                                            local a=(lo[axis]-from[axis])/delta
                                            local b=(hi[axis]-from[axis])/delta
                                            if a>b then a,b=b,a end
                                            enter,leave=math.max(enter,a),math.min(leave,b)
                                            if enter>leave then break end
                                        end
                                    end
                                    local distance=enter*length
                                    if enter<=leave and enter<=1 and distance<bodyDistance-0.01 then
                                        return {status="native equipment interception",distance=distance,
                                            equipment=tostring(equipment.id),equipmentClass=item.class or "",
                                            hand=hand,geometry="held entity local bounds"}
                                    end
                                end
                            end
                        end
                    end
                end
                return nil
            end
            local function soulAttacker(attackerId)
                if not attackerId then return nil end
                local found, source = pcall(function()
                    if attackerId == player.id then return player end
                    return System.GetEntity(attackerId)
                end)
                if not found or not source or source.id ~= attackerId then
                    return nil, "native attacker gone"
                end
                -- WHGame attaches this opaque handle to the live Soul table.
                -- DealDamage reads the same ScriptHandle type and resolves a
                -- Soul WUID (type 5). A Cry EntityId is a different namespace;
                -- numbers/strings lose the handle type and may lose 64-bit bits.
                local read, wuid = pcall(function()
                    return source.soul and source.soul.__ThisWUID
                end)
                if not read or type(wuid) ~= "userdata" or wuid == __null then
                    return nil, "native attacker Soul identity unavailable"
                end
                return wuid
            end
            local function applyDamage(entity, amount, hitType, point, direction, nowMs, attackerId)
                -- Polling only observes an earlier hit. Every subsequent real
                -- hit must still reach the engine while the target is alive.
                -- A submitted lethal hit must not grant three seconds of immunity.
                pollDeath(entity, nowMs)
                -- Current WHGame's Lua registration names these arguments
                -- "stamina,health". The old health-first call drained stamina
                -- and produced a knockout at HP=1. Submit health-only damage
                -- with an actual attacker. Let the victim's native hit reaction
                -- and crime/AI notification execute; only Henry's input animation
                -- is replaced by Minecraft, not the NPC's perception of the hit.
                if entity.soul then
                    local before = readHealth(entity)
                    if not before then
                        return false, "soul health read unavailable", "soul"
                    end
                    local api = "soul.DealDamage(stamina,health,attacker,noReaction,part)"
                    if not entity.soul.DealDamage then
                        return false, "soul damage API unavailable", api
                    end
                    local attacker, identityError = soulAttacker(attackerId)
                    if identityError then return false, identityError, api end
                    local ok, accepted = pcall(entity.soul.DealDamage, entity.soul,
                        0, amount, attacker, false, "torso")
                    if not ok or accepted == false then
                        return false, ok and "native damage rejected" or tostring(accepted), api, nil, attacker
                    end
                    if amount >= before - 0.001 then
                        local pending = pendingDeaths[tostring(entity.id)]
                        if not pending or pending.status ~= "death submitted" then
                            pendingDeaths[tostring(entity.id)] = { at = nowMs, status = "death submitted" }
                        end
                        local status = pollDeath(entity, nowMs)
                        return true, "", api, status, attacker
                    end
                    return true, "", api, nil, attacker
                end
                if not g_gameRules or not g_gameRules.CreateHit then
                    return false, "actor damage API unavailable", "CreateHit"
                end
                local ok, accepted = pcall(g_gameRules.CreateHit, g_gameRules,
                    entity.id, attackerId, attackerId, amount, 0, nil, nil,
                    hitType or "normal", point, direction, { x = 0, y = 0, z = 1 })
                return ok and accepted ~= false,
                    ok and (accepted == false and "damage rejected" or "")
                        or tostring(accepted), "CreateHit", nil, attackerId
            end
        )lua" R"lua(
            return function(action, id, damage, hitType, pushX, pushZ, pushStrength, nowMs, trace)
                nowMs = nowMs or 0
                if action == "reset" then
                    nextKey = 0
                    keysByNativeId = {}
                    nativeByKey = {}
                    lastPlayerId = nil
                    lastPlayerHealth = nil
                    deathSubmitted = false
                    pendingDeaths = {}
                    deathReports = {}
                    pushes = {}
                    lastPushTick = nil
                    cachedActors = nil
                    lastActorQuery = 0
                    return { status = "reset" }
                end
                if not player or not player.id or not System then
                    return { status = "unavailable" }
                end
                if action == "tick" then return movePushes(nowMs) end
                if action == "health" then
                    local health, healthSource = readHealth(player)
                    if not health then
                        return { status = "health read failed" }
                    end
                    if lastPlayerId ~= player.id or not lastPlayerHealth then
                        lastPlayerId = player.id
                        lastPlayerHealth = health
                        return { status = "primed" }
                    end
                    if health + 0.01 < lastPlayerHealth then
                        local delta = lastPlayerHealth - health
                        if health <= 0 then
                            lastPlayerHealth = health
                            return { status = "hurt", damage = delta }
                        end
                        local wrote, accepted = writeHealth(player,
                            lastPlayerHealth, healthSource)
                        local restored = readHealth(player)
                        if wrote and accepted ~= false and restored
                            and restored >= lastPlayerHealth - 0.01 then
                            return { status = "hurt", damage = delta }
                        end
                        lastPlayerHealth = health
                        return { status = "health restore failed" }
                    end
                    lastPlayerHealth = health
                    return { status = "steady" }
                end
                if action == "death" then
                    local health = readHealth(player)
                    if not health then
                        return { status = "death health read failed" }
                    end
                    if isDead(player) then
                        return { status = "already dead" }
                    end
                    if deathSubmitted then
                        return { status = pollDeath(player, nowMs) or "death submitted" }
                    end
                    local positionOk, position = pcall(player.GetWorldPos, player)
                    if not positionOk or not position then
                        return { status = "death position unavailable" }
                    end
                    local ok, errorText, damageApi, status = applyDamage(player,
                        health + 1000, "normal", position, { x = 0, y = 0, z = 1 }, nowMs)
                    deathSubmitted = ok
                    return { status = ok and status or "death failed",
                        error = errorText, damageApi = damageApi }
                end
                if action == "scan" then
                    local center = player:GetWorldPos()
                    if not cachedActors or nowMs - lastActorQuery >= 500 then
                        local queryOk, nearby = pcall(System.GetEntities)
                        if not queryOk or type(nearby) ~= "table" then
                            return { status = "entity query failed" }
                        end
                        cachedActors, querySeen = {}, 0
                        lastActorQuery = nowMs
                        for _, entity in pairs(nearby) do
                            querySeen = querySeen + 1
                            if entity and entity.id and (entity.actor or entity.human)
                                and entity ~= player and entity.id ~= player.id
                                and entity.GetWorldPos then
                                cachedActors[#cachedActors+1] = entity
                            end
                        end
                    end
                    local actors = {}
                    local seen = querySeen
                    local nearbyActors = 0
                    local currentIds = {}
                    for _, entity in ipairs(cachedActors) do
                        if entity and entity ~= player and entity.id
                            and entity.id ~= player.id
                            and not (SkyCraftNativeMobs and SkyCraftNativeMobs.owns(entity))
                            and (entity.actor or entity.human)
                            and entity.GetWorldPos then
                            local posOk, position = pcall(entity.GetWorldPos, entity)
                            if posOk and position then
                                local dx, dy = position.x - center.x,
                                    position.y - center.y
                                local dz = position.z - center.z
                                if dx * dx + dy * dy + dz * dz <= 196 then
                                nearbyActors = nearbyActors + 1
                                local health, healthSource = readHealth(entity)
                                pollDeath(entity, nowMs)
                                local maxHealth = healthSource == "soul" and 100 or 1
                                health = health or maxHealth
                                if healthSource ~= "soul" and entity.GetMaxHealth then
                                    local maxOk, value = pcall(entity.GetMaxHealth, entity)
                                    if maxOk and type(value) == "number" and value > 0 then
                                        maxHealth = value
                                    end
                                end
                                local name = entity.GetName and entity:GetName() or "NPC"
                                local nativeId = tostring(entity.id)
                                local key = keysByNativeId[nativeId]
                                if not key then
                                    nextKey = nextKey + 1
                                    key = nextKey
                                    keysByNativeId[nativeId] = key
                                end
                                currentIds[key] = entity.id
                                local width, height = 0.6, 1.8
                                if entity.GetLocalBBox then
                                    local boxOk, low, high = pcall(entity.GetLocalBBox, entity)
                                    if boxOk and low and high and type(low.x) == "number"
                                        and type(low.y) == "number" and type(low.z) == "number"
                                        and type(high.x) == "number" and type(high.y) == "number"
                                        and type(high.z) == "number" then
                                        width = math.max(0.2, math.min(6, math.max(high.x - low.x, high.y - low.y)))
                                        height = math.max(0.2, math.min(6, high.z - low.z))
                                    end
                                end
                                actors[#actors + 1] = {
                                    id = key,
                                    x = position.x, y = position.z, z = -position.y,
                                    name = name, health = health,
                                    width = width, height = height, dead = isDead(entity),
                                    healthFrac = math.max(0, math.min(1, health / maxHealth)),
                                }
                                if #actors >= 128 then break end
                                end
                            end
                        end
                    end
                    nativeByKey = currentIds
                    local reports = deathReports
                    deathReports = {}
                    return { status = "ok", source = "all",
                        actors = actors, seen = seen,
                        deaths = reports,
                        nearbyActors = nearbyActors }
                end
        )lua" R"lua(
                if action == "hit" or action == "heal" or action == "probe" or action == "commit" then
                    if not System.GetEntity then
                        return { status = "entity API unavailable" }
                    end
                    local nativeId = nativeByKey[id]
                    local target = nativeId and System.GetEntity(nativeId)
                    if not target or target == player
                        or not (target.actor or target.human)
                        or target.id == player.id then
                        return { status = "target gone" }
                    end
                    local origin = player:GetWorldPos()
                    local position = target:GetWorldPos()
                    local dx, dy = position.x - origin.x, position.y - origin.y
                    local dz = position.z - origin.z
                    if dx * dx + dy * dy + dz * dz > 225 then
                        return { status = "target out of range" }
                    end
                    local length = math.sqrt(dx * dx + dy * dy)
                    if length < 0.001 then length = 1 end
                    local direction = { x = dx / length, y = dy / length, z = 0 }
                    local point = { x = position.x, y = position.y,
                        z = position.z + 1.1 }
                    local attackerId = target ~= player and player.id or nil
                    if trace then
                        -- Environmental damage and MC mobs are not Henry attacks.
                        -- Native proxy attacks keep their real actor identity.
                        attackerId = trace.sourceKind == 1 and player.id
                            or trace.sourceKind == 3 and nativeByKey[trace.sourceNativeKey] or nil
                        if trace.sourceKind == 2 then
                            local source = SkyCraftNativeMobs and SkyCraftNativeMobs.resolveAttack(
                                trace.epoch, trace.session, trace.sourceEntity, trace.sourceUuid)
                            if not source then return {status="Minecraft mob identity unavailable"} end
                            attackerId = source.id
                        end
                        if trace.sourceKind == 3 and not attackerId then
                            return {status="native attacker gone"}
                        end
                        if target.soul and action ~= "heal" then
                            local _, identityError = soulAttacker(attackerId)
                            if identityError then return {status=identityError} end
                        end
                        point = trace.point
                        local origin = trace.from
                        local vx,vy,vz=point.x-origin.x,point.y-origin.y,point.z-origin.z
                        local length=math.sqrt(vx*vx+vy*vy+vz*vz)
                        if length > 0.001 then direction={x=vx/length,y=vy/length,z=vz/length} end
                        if (trace.sourceKind ~= 0 or trace.projectile) and length > 0.001 and action ~= "commit" then
                            if not Physics or not Physics.RayWorldIntersection or not ent_all then
                                return {status="native trace unavailable"}
                            end
                            local hits={}
                            local tested,count=pcall(Physics.RayWorldIntersection,origin,
                                {x=vx,y=vy,z=vz},8,ent_all,attackerId,nil,hits)
                            if not tested or type(count)~="number" or count~=count
                                or count<0 or count>8 or count~=math.floor(count) then
                                return {status="native trace failed"}
                            end
                            local nearest,nearestEntity,nearestPart,nearestPoint,nearestNormal=nil,nil,nil,nil,nil
                            for index=1,count do
                                local hit=hits[index]
                                if type(hit)~="table" then return {status="native trace incomplete"} end
                                local hitEntity=hit.entity
                                local name=hitEntity and hitEntity.GetName and hitEntity:GetName() or nil
                                if not name or name:sub(1,19)~="skycraft_collision_" then
                                    local distance=hit.dist
                                    if not distance and hit.pos then distance=math.sqrt(
                                        (hit.pos.x-origin.x)^2+(hit.pos.y-origin.y)^2+(hit.pos.z-origin.z)^2) end
                                    if type(distance)~="number" or distance~=distance or distance<0 then
                                        return {status="native trace incomplete"}
                                    end
                                    if not nearest or distance<nearest then
                                        nearest,nearestEntity,nearestPart=distance,hitEntity,hit.partid
                                        nearestPoint,nearestNormal=hit.pos,hit.normal
                                    end
                                end
                            end
                            if nearest and nearest < length-0.04
                                and (not nearestEntity or nearestEntity.id~=target.id) then
                                return {status="native obstruction",distance=nearest,part=nearestPart,
                                    point=nearestPoint,normal=nearestNormal}
                            end
                            local interception=equippedInterception(target,origin,point,length,
                                nearestEntity and nearestEntity.id==target.id and nearest or length)
                            if interception then return interception end
                        end
                    end
                    local before, healthSource = readHealth(target)
                    if isDead(target) then
                        return { status = "target dead" }
                    end
                    if action == "probe" then return {status="trace clear"} end
                    if action == "heal" then
                        if not before then return { status = "heal health read failed" } end
                        local maximum = healthSource == "soul" and 100 or before
                        if healthSource ~= "soul" and target.GetMaxHealth then
                            local read, value = pcall(target.GetMaxHealth, target)
                            if read and type(value) == "number" then maximum = value end
                        end
                        local wanted = math.min(maximum, before + damage)
                        local ok, accepted = writeHealth(target, wanted, healthSource)
                        local after = readHealth(target)
                        return { status = ok and accepted ~= false
                            and (after and after > before + 0.01 and "health restored"
                                or "heal submitted") or "heal failed",
                            healthBefore = before, healthAfter = after or -1,
                            healthSource = healthSource }
                    end
                    local ok, errorText, damageApi, damageStatus, actualAttacker = true, "", "none", nil, nil
                    if damage > 0 then
                        ok, errorText, damageApi, damageStatus, actualAttacker = applyDamage(target,
                            damage, hitType, point, direction, nowMs, attackerId)
                    end
                    local impulse = false
                    local strength = math.max(0, math.min(pushStrength or 0, 2))
                    local ix, iy = pushX or 0, -(pushZ or 0)
                    local impulseLength = math.sqrt(ix * ix + iy * iy)
                    local pushApi = "none"
                    if ok and not damageStatus and strength > 0.0001 and impulseLength > 0.001 then
                        if target.SetWorldPos and Physics and Physics.RayWorldIntersection and ent_all then
                            local radius = 0.3
                            if target.GetLocalBBox then
                                local read, lo, hi = pcall(target.GetLocalBBox, target)
                                if read and lo and hi then radius = math.max(0.2, math.min(1.5,
                                    math.max(hi.x-lo.x, hi.y-lo.y) * 0.5)) end
                            end
                            pushes[tostring(target.id)] = {id=target.id, at=nowMs,
                                vx=ix/impulseLength*strength*20, vy=iy/impulseLength*strength*20,
                                radius=radius}
                            lastPushTick = lastPushTick or nowMs
                            impulse, pushApi = true, "collision-swept velocity"
                        elseif target.AddImpulse then
                            impulse = pcall(target.AddImpulse, target, -1, point,
                            { x = ix / impulseLength,
                              y = iy / impulseLength, z = 0.1 },
                            strength * 120, 1)
                            pushApi = "AddImpulse fallback"
                        end
                    end
                    local after = readHealth(target)
                    local status = "hit failed"
                    if ok then
                        status = damageStatus or (before and after and after < before - 0.01
                            and "health reduced" or "hit submitted")
                    end
                    return { status = status,
                        error = errorText, damageApi = damageApi,
                        healthBefore = before or -1, healthAfter = after or -1,
                        healthSource = healthSource,
                        impulse = impulse, pushApi = pushApi,
                        attacker = actualAttacker and tostring(actualAttacker) or "none",
                        attackerKind = damageApi == "CreateHit" and "Cry EntityId" or "Soul WUID",
                        attackerEntity = attackerId and tostring(attackerId) or "unmapped/environment" }
                end
                return { status = "unknown action" }
            end
        )lua", sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(ERROR) << "SkyCraft: combat command preparation failed: "
                       << error.what();
            return false;
        }
        combatCommand = result.get<sol::protected_function>();
        return true;
    }

    bool PrepareExplosion(sol::state_view& lua)
    {
        if (explosionPrepared) {
            return explosionCommand.valid();
        }
        explosionPrepared = true;
        auto result = lua.safe_script(R"lua(
            return function(mcX, mcY, mcZ, mcRadius)
                if not player or not player.id or not g_gameRules
                    or not g_gameRules.CreateExplosion then
                    return { status = "explosion API unavailable" }
                end
                local radius = math.max(0.5, math.min(mcRadius, 12))
                local position = { x = mcX, y = -mcZ, z = mcY }
                local direction = { x = 0, y = 0, z = 1 }
                local ok, errorText = pcall(g_gameRules.CreateExplosion,
                    g_gameRules, player.id, player.id, 0, position,
                    direction, radius, nil, radius * 100, 0,
                    "explosions.grenade_air.explosion", radius / 4,
                    0, 0, radius, 4)
                return { status = ok and "effect" or "explosion failed",
                    error = ok and "" or tostring(errorText) }
            end
        )lua", sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(ERROR) << "SkyCraft: explosion command preparation failed: "
                       << error.what();
            return false;
        }
        explosionCommand = result.get<sol::protected_function>();
        return true;
    }

    void PublishActors()
    {
        publishedIds.clear();
        auto result = combatCommand("scan", 0, 0, "normal", 0, 0, 0, ::GetTickCount64());
        if (!result.valid()) {
            const sol::error error = result;
            LOG(WARNING) << "SkyCraft: NPC scan failed: " << error.what();
            SkyCraftBridgeWriteActors(nullptr, 0);
            return;
        }
        const sol::object value = result.get<sol::object>();
        if (!value.is<sol::table>()) {
            SkyCraftBridgeWriteActors(nullptr, 0);
            return;
        }
        const sol::table response = value.as<sol::table>();
        const auto status = response.get_or("status", std::string{});
        if (status != "ok") {
            const auto now = ::GetTickCount64();
            if (now - lastReportMs >= 10000) {
                lastReportMs = now;
                LOG(WARNING) << "SkyCraft: NPC scan " << status;
            }
            SkyCraftBridgeWriteActors(nullptr, 0);
            return;
        }
        const sol::object reports = response["deaths"];
        if (reports.is<sol::table>()) {
            const auto deaths = reports.as<sol::table>();
            for (std::size_t i = 1; i <= deaths.size(); ++i) {
                const sol::table report = deaths[i];
                LOG(INFO) << "SkyCraft: native actor death "
                          << report.get_or("nativeId", std::string{}) << ": "
                          << report.get_or("status", std::string{});
            }
        }
        const sol::object entries = response["actors"];
        if (!entries.is<sol::table>()) {
            SkyCraftBridgeWriteActors(nullptr, 0);
            return;
        }
        const sol::table actors = entries.as<sol::table>();
        std::vector<ActorRecord> records;
        records.reserve(std::min<std::size_t>(actors.size(), 128));
        for (std::size_t i = 1; i <= actors.size() && records.size() < 128; ++i) {
            const sol::object item = actors[i];
            if (!item.is<sol::table>()) {
                continue;
            }
            const sol::table actor = item.as<sol::table>();
            ActorRecord record{};
            record.formId = actor.get_or("id", std::uint32_t{});
            record.x = actor.get_or("x", 0.0f);
            record.y = actor.get_or("y", 0.0f);
            record.z = actor.get_or("z", 0.0f);
            if (!record.formId || !std::isfinite(record.x)
                || !std::isfinite(record.y) || !std::isfinite(record.z)) {
                continue;
            }
            record.width = actor.get_or("width", 0.6f);
            record.height = actor.get_or("height", 1.8f);
            record.healthFrac = actor.get_or("healthFrac", 1.0f);
            record.level = 1;
            record.flags = actor.get_or("dead", false)
                ? kActorDead : 0;
            const auto name = actor.get_or("name", std::string{"NPC"});
            std::memcpy(record.name, name.data(),
                std::min(name.size(), sizeof(record.name) - 1));
            if (!(record.flags & kActorDead)) {
                publishedIds.insert(record.formId);
            }
            records.push_back(record);
        }
        SkyCraftBridgeWriteActors(records.data(), std::uint32_t(records.size()));
        const auto now = ::GetTickCount64();
        if (now - lastReportMs >= 10000) {
            lastReportMs = now;
            LOG(INFO) << "SkyCraft: published " << records.size()
                      << " nearby KCD2 actors via "
                      << response.get_or("source", std::string{})
                      << " (" << response.get_or("nearbyActors", 0)
                      << " actor candidates / "
                      << response.get_or("seen", 0) << " entities)";
        }
    }

    void BridgePlayerDamage()
    {
        const auto now = ::GetTickCount64();
        if (pendingPlayerDamage >= 0.01f) {
            const auto amount = std::clamp(std::lround(pendingPlayerDamage * 100),
                1l, 200000l);
            const InputEvent input{kInHurt, kHurtOther,
                std::int32_t(amount), 0, 0};
            if (SkyCraftBridgePushInput(&input)) {
                LOG(INFO) << "SkyCraft: KCD2 player damage "
                          << pendingPlayerDamage
                          << " forwarded to Minecraft";
                pendingPlayerDamage = 0;
            }
        }
        if (now - lastHealthPollMs < 100) {
            return;
        }
        lastHealthPollMs = now;
        auto result = combatCommand("health", 0, 0);
        if (!result.valid()) {
            const sol::error error = result;
            if (now - lastHealthWarningMs >= 10000) {
                lastHealthWarningMs = now;
                LOG(WARNING) << "SkyCraft: player health bridge failed: "
                             << error.what();
            }
            return;
        }
        const sol::object value = result.get<sol::object>();
        if (!value.is<sol::table>()) {
            return;
        }
        const sol::table response = value.as<sol::table>();
        const auto status = response.get_or("status", std::string{});
        if (status == "hurt") {
            const float amount = response.get_or("damage", 0.0f);
            if (std::isfinite(amount) && amount > 0) {
                pendingPlayerDamage = std::min(pendingPlayerDamage + amount,
                    2000.0f);
            }
        } else if (status != "steady" && status != "primed"
            && now - lastHealthWarningMs >= 10000) {
            lastHealthWarningMs = now;
            LOG(WARNING) << "SkyCraft: player health bridge " << status;
        }
    }

    void ForwardMinecraftDeath()
    {
        const auto now = ::GetTickCount64();
        if (minecraftDeathHandled || now - lastDeathAttemptMs < 1000) {
            return;
        }
        lastDeathAttemptMs = now;
        auto result = combatCommand("death", 0, 0, "normal", 0, 0, 0, now);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(WARNING) << "SkyCraft: Minecraft death bridge failed: "
                         << error.what();
            return;
        }
        const sol::object value = result.get<sol::object>();
        if (!value.is<sol::table>()) {
            return;
        }
        const sol::table response = value.as<sol::table>();
        const auto status = response.get_or("status", std::string{});
        minecraftDeathHandled = status == "death observed"
            || status == "already dead";
        LOG(INFO) << "SkyCraft: Minecraft death in KCD2: "
                  << status << " "
                  << response.get_or("error", std::string{});
    }

    void ConsumeEvents(sol::state_view* lua, bool active)
    {
        McEvent event{};
        for (int i = 0; i < 128 && SkyCraftBridgePopEvent(&event); ++i) {
            if (!active) {
                continue;
            }
            if (event.type == kEvExplosion) {
                if (!lua || !std::isfinite(event.a)
                    || !std::isfinite(event.b) || !std::isfinite(event.c)
                    || !std::isfinite(event.d) || event.d <= 0
                    || !PrepareExplosion(*lua)) {
                    continue;
                }
                auto result = explosionCommand(event.a, event.b,
                    event.c, event.d);
                if (!result.valid()) {
                    const sol::error error = result;
                    LOG(WARNING) << "SkyCraft: explosion bridge failed: "
                                 << error.what();
                    continue;
                }
                const sol::object value = result.get<sol::object>();
                if (value.is<sol::table>()) {
                    const sol::table response = value.as<sol::table>();
                    LOG(INFO) << "SkyCraft: Minecraft explosion radius "
                              << event.d << " in KCD2: "
                              << response.get_or("status", std::string{})
                              << " " << response.get_or("error", std::string{});
                }
                continue;
            }
            if (event.type == kEvPlayerDied) {
                ForwardMinecraftDeath();
                continue;
            }
            if (event.type != kEvHitActor && event.type != skycraft::kcd2::kEvHealActor) {
                ++unsupportedEvents;
                continue;
            }
            if (!publishedIds.contains(event.formId)
                || !std::isfinite(event.a) || event.a < 0
                || !std::isfinite(event.b) || !std::isfinite(event.c)
                || !std::isfinite(event.d) || (event.a == 0 && event.d <= 0)) {
                continue;
            }
            const float damage = std::clamp(event.a * 5.0f, 0.0f, 200.0f);
            const char* hitType = (event.flags & kHitFire) ? "fire" : "normal";
            const bool healing = event.type == skycraft::kcd2::kEvHealActor;
            auto result = combatCommand(healing ? "heal" : "hit", event.formId, damage,
                hitType, event.b, event.c, event.d, ::GetTickCount64());
            if (!result.valid()) {
                const sol::error error = result;
                LOG(WARNING) << "SkyCraft: NPC hit bridge failed: "
                             << error.what();
                continue;
            }
            const sol::object value = result.get<sol::object>();
            if (!value.is<sol::table>()) {
                continue;
            }
            const sol::table response = value.as<sol::table>();
            const auto status = response.get_or("status", std::string{});
            LOG(INFO) << "SkyCraft: Minecraft " << (healing ? "heal" : "hit")
                      << " on KCD2 actor "
                      << event.formId << " for " << damage << " damage: "
                      << status << " " << response.get_or("error", std::string{})
                      << " (" << hitType << ", flags " << event.flags
                      << ", attacker " << ((event.flags & skycraft::kcd2::kHitMobSource) ? "mob" : "player/environment")
                      << ", health " << response.get_or("healthBefore", -1.0f)
                      << " -> " << response.get_or("healthAfter", -1.0f)
                      << " source " << response.get_or("healthSource", std::string{})
                      << ", API " << response.get_or("damageApi", std::string{})
                      << ", impulse " << response.get_or("impulse", false)
                      << ", push " << response.get_or("pushApi", std::string{}) << ")";
        }
        const auto now = ::GetTickCount64();
        if (unsupportedEvents && now - lastUnsupportedReportMs >= 10000) {
            lastUnsupportedReportMs = now;
            LOG(WARNING) << "SkyCraft: " << unsupportedEvents
                         << " Minecraft events have no KCD2 handler yet";
            unsupportedEvents = 0;
        }
    }
}

bool SkyCraftCombatAcceptTrace(std::uint32_t type, const std::uint8_t* data, std::uint32_t bytes)
{
    if (SkyCraftMobsAccept(type,data,bytes)) return true;
    const bool extended=type==skycraft::kcd2::kRenMobAttackProbe || type==skycraft::kcd2::kRenMobAttackCommit
        || type==skycraft::kcd2::kRenMobAttackTrace;
    const bool probe=type==skycraft::kcd2::kRenAttackProbe || type==skycraft::kcd2::kRenMobAttackProbe;
    const bool commit=type==skycraft::kcd2::kRenAttackCommit || type==skycraft::kcd2::kRenMobAttackCommit;
    if (type != skycraft::kcd2::kRenAttackTrace && !extended && !probe && !commit) return false;
    const auto expected=((probe||commit) ? sizeof(skycraft::kcd2::AttackTransaction) : sizeof(skycraft::kcd2::AttackTrace))
        +(extended ? sizeof(skycraft::kcd2::MobAttackIdentity) : 0);
    if (!data || bytes!=expected) return true;
    std::uint32_t request=0;
    if (probe||commit) {
        std::memcpy(&request,data,4);
        if (!request) return true;
        data+=4;
    }
    skycraft::kcd2::AttackTrace trace{};
    std::memcpy(&trace, data, sizeof(trace));
    skycraft::kcd2::MobAttackIdentity identity{};
    if (extended) std::memcpy(&identity,data+sizeof(trace),sizeof(identity));
    if ((trace.sourceKind==2)!=extended || (extended && (!trace.sourceEntity
        || trace.sourceEntity>0x7fffffffu
        || (!identity.session.most && !identity.session.least) || (!identity.entity.most && !identity.entity.least)))) return true;
    if (!trace.target || trace.sourceKind > 3 || !std::isfinite(trace.damage)
        || (probe ? trace.damage!=0 : trace.damage<=0) || trace.damage>std::numeric_limits<float>::max()/5
        || !std::isfinite(trace.pushX) || !std::isfinite(trace.pushZ)
        || !std::isfinite(trace.pushStrength) || trace.pushStrength < 0 || trace.pushStrength > 2) return true;
    double lengthSquared = 0;
    for (int i=0;i<3;++i) {
        if (!std::isfinite(trace.from[i]) || !std::isfinite(trace.point[i])
            || std::abs(trace.from[i]) > 3e7f || std::abs(trace.point[i]) > 3e7f) return true;
        const double d = trace.point[i] - trace.from[i];
        lengthSquared += d*d;
    }
    if (lengthSquared > 128*128) return true;
    if (attackTraces.size() >= 512) {
        LOG(WARNING) << "SkyCraft: native attack queue full; rejecting hit";
        return true;
    }
    attackTraces.push_back({trace, ::GetTickCount64(),request,probe?1u:commit?2u:0u,identity});
    return true;
}

void SkyCraftCombatTick(lua_State* state, const SkyState& host, bool minecraftAlive)
{
    McState mc{};
    const bool active = minecraftAlive && (host.flags & kSkyInGame) != 0
        && SkyCraftBridgeReadMcState(&mc) && (mc.flags & kMcInWorld) != 0
        && mc.teleportAck == host.teleportSeq;
    if (!active) {
        SkyCraftMobsReset(state);
        if (combatWasActive && combatCommand.valid()) {
            combatCommand("reset", 0, 0);
        }
        combatWasActive = false;
        publishedIds.clear();
        attackTraces.clear();
        approvedAttacks.clear();
        attackReplies.clear();
        pendingPlayerDamage = 0;
        minecraftDeathHandled = false;
        lastDeathAttemptMs = 0;
        SkyCraftBridgeWriteActors(nullptr, 0);
        ConsumeEvents(nullptr, false);
        return;
    }
    const bool newSession = !combatWasActive
        || activeCollisionEpoch != host.collisionEpoch;
    combatWasActive = true;
    activeCollisionEpoch = host.collisionEpoch;
    sol::state_view lua(state);
    if (!PrepareCommand(lua)) {
        SkyCraftBridgeWriteActors(nullptr, 0);
        ConsumeEvents(nullptr, false);
        return;
    }
    if (newSession) {
        SkyCraftMobsReset(state);
        combatCommand("reset", 0, 0);
        publishedIds.clear();
        attackTraces.clear();
        approvedAttacks.clear();
        attackReplies.clear();
        pendingPlayerDamage = 0;
        minecraftDeathHandled = false;
        lastDeathAttemptMs = 0;
        lastHealthPollMs = 0;
        lastScanMs = 0;
        SkyCraftBridgeWriteActors(nullptr, 0);
        ConsumeEvents(nullptr, false);
    }
    const auto now = ::GetTickCount64();
    SkyCraftMobsTick(state,host.collisionEpoch);
    approvedAttacks.expire(now);
    const auto flushReplies=[]() {
        while (!attackReplies.empty()) {
            if (!SkyCraftBridgeWriteCollision(skycraft::kcd2::kColAttackResult,
                &attackReplies.front(),sizeof(skycraft::kcd2::AttackResult))) break;
            attackReplies.pop_front();
        }
    };
    flushReplies();
    auto pushResult = combatCommand("tick", 0, 0, "normal", 0, 0, 0, now);
    if (pushResult.valid()) {
        const sol::object value = pushResult.get<sol::object>();
        if (value.is<sol::table>()) {
            const auto response = value.as<sol::table>();
            static std::uint64_t nextPushReport = 0;
            const auto moved = response.get_or("moved", 0);
            const auto blocked = response.get_or("blocked", 0);
            const auto failed = response.get_or("failed", 0);
            if ((moved || blocked || failed) && now >= nextPushReport) {
                nextPushReport = now + 250;
                LOG(INFO) << "SkyCraft: native knockback moved " << moved
                    << " blocked " << blocked << " failed " << failed;
            }
        }
    }
    if (mc.flags & kMcDead) {
        ForwardMinecraftDeath();
    } else {
        minecraftDeathHandled = false;
        BridgePlayerDamage();
    }
    if (now - lastScanMs >= 100) {
        lastScanMs = now;
        PublishActors();
    }
    for (int count=0;count<64 && !attackTraces.empty();++count) {
        const auto queued = attackTraces.front();
        attackTraces.pop_front();
        const auto& trace = queued.trace;
        if (trace.epoch != host.collisionEpoch || now-queued.received > 1000
            || !publishedIds.contains(trace.target)) {
            if (queued.mode==1 && attackReplies.size()<512) {
                skycraft::kcd2::AttackResult reply{trace.epoch,queued.request,trace.target,skycraft::kcd2::kAttackRejected};
                std::copy(std::begin(trace.point),std::end(trace.point),reply.point);
                attackReplies.push_back(reply);
            }
            continue;
        }
        if (queued.mode==2) {
            if (!approvedAttacks.consume(queued.request,trace,now,queued.identity)) continue;
        }
        const auto vec = [&](const float* p) { return lua.create_table_with("x",p[0],"y",-p[2],"z",p[1]); };
        auto data = lua.create_table_with("sourceKind",trace.sourceKind,
            "sourceNativeKey",trace.sourceNativeKey,"sourceEntity",trace.sourceEntity,
            "projectile",(trace.flags & kHitProjectile)!=0,"from",vec(trace.from),"point",vec(trace.point));
        data["epoch"]=trace.epoch;
        data["session"]=SkyCraftMobUuidText(queued.identity.session);
        data["sourceUuid"]=SkyCraftMobUuidText(queued.identity.entity);
        auto result = combatCommand(queued.mode==1 ? "probe" : queued.mode==2 ? "commit" : "hit",
            trace.target,trace.damage*5.0f,
            (trace.flags & kHitFire) ? "fire" : "normal",trace.pushX,trace.pushZ,trace.pushStrength,now,data);
        if (!result.valid()) {
            const sol::error error=result;
            LOG(WARNING) << "SkyCraft: native attack trace failed: " << error.what();
            if (queued.mode==1 && attackReplies.size()<512) {
                skycraft::kcd2::AttackResult reply{trace.epoch,queued.request,trace.target,skycraft::kcd2::kAttackRejected};
                std::copy(std::begin(trace.point),std::end(trace.point),reply.point);
                attackReplies.push_back(reply);
            }
        } else if (result.get<sol::object>().is<sol::table>()) {
            const auto response=result.get<sol::table>();
            if (queued.mode==1) {
                skycraft::kcd2::AttackResult reply{trace.epoch,queued.request,trace.target,skycraft::kcd2::kAttackRejected};
                const auto status=response.get_or("status",std::string{});
                reply.decision=status=="trace clear" ? skycraft::kcd2::kAttackClear
                    : status=="native obstruction" ? skycraft::kcd2::kAttackWorld
                    : status=="native equipment interception" ? skycraft::kcd2::kAttackEquipment
                    : skycraft::kcd2::kAttackRejected;
                const float dx=trace.point[0]-trace.from[0],dy=trace.point[1]-trace.from[1],dz=trace.point[2]-trace.from[2];
                const float length=std::sqrt(dx*dx+dy*dy+dz*dz);
                reply.distance=std::clamp(response.get_or("distance",length),0.0f,length);
                const float t=length>0 ? reply.distance/length : 0;
                for (int i=0;i<3;++i) {
                    reply.point[i]=trace.from[i]+(trace.point[i]-trace.from[i])*t;
                    reply.normal[i]=length>0 ? -(trace.point[i]-trace.from[i])/length : 0;
                }
                const auto normal=response.get<sol::optional<sol::table>>("normal");
                if (normal) {
                    const float x=normal->get_or("x",0.0f),y=normal->get_or("y",0.0f),z=normal->get_or("z",0.0f);
                    if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && x*x+y*y+z*z>0.01f) {
                        reply.normal[0]=x;reply.normal[1]=z;reply.normal[2]=-y;
                    }
                }
                if (reply.decision==skycraft::kcd2::kAttackClear && !approvedAttacks.approve(queued.request,trace,now,queued.identity))
                    reply.decision=skycraft::kcd2::kAttackRejected;
                if (attackReplies.size()<512) attackReplies.push_back(reply);
                flushReplies();
                continue;
            }
            LOG(INFO) << "SkyCraft: traced Minecraft hit on actor " << trace.target
                << " source " << trace.sourceKind << ":" << trace.sourceEntity
                << " weapon " << trace.weapon << " flags " << trace.flags
                << " result " << response.get_or("status",std::string{})
                << " native attacker " << response.get_or("attacker",std::string{})
                << " identity type " << response.get_or("attackerKind",std::string{})
                << " entity " << response.get_or("attackerEntity",std::string{})
                << " error " << response.get_or("error",std::string{})
                << " health " << response.get_or("healthBefore",-1.0f)
                << " -> " << response.get_or("healthAfter",-1.0f);
        }
    }
    flushReplies();
    ConsumeEvents(&lua, true);
}
