-- Native doubles retain XGen perception and a Soul identity. Minecraft owns
-- behaviour; the authored native brain only verifies ownership and despawns.
-- No native hit is invented from proximity or substituted with a player hit.
local M = {}
local epoch, session = nil, nil
local byMinecraft, byNative, reports, deferred = {}, {}, {}, {}
local serial, reportCount = 0, 0
local MAX_MOBS, MAX_REPORTS, MAX_SPAWNS, TIMEOUT = 64, 512, 4, 2
local PROFILE = "7560b395-6ee0-4b56-b1dc-9a9de2456b13"

local function finite(value)
    return type(value) == "number" and value == value and math.abs(value) < 1e9
end
local function clock()
    if not System or not System.GetCurrTime then return nil end
    local ok, now = pcall(System.GetCurrTime)
    return ok and finite(now) and now >= 0 and now or nil
end
local function identity(entity)
    local ok, value = pcall(function() return entity.soul.__ThisWUID end)
    return ok and type(value) == "userdata" and value ~= __null and value or nil
end
local function health(entity)
    local ok, value = pcall(function() return entity.soul:GetState("health") end)
    return ok and finite(value) and value >= 0 and value or nil
end
local function retire(entry)
    if entry.nativeId then byNative[tostring(entry.nativeId)] = nil end
    entry.entity, entry.nativeId, entry.wuid, entry.nativeHealth = nil, nil, nil, nil
    -- The authored brain runs Despawn against its Soul. Removing just the Cry
    -- entity would leave XGen/Soul state behind, so System.RemoveEntity is unused.
end
local function current(entry)
    if not entry.entity or not entry.nativeId or not System.GetEntity then return nil end
    local ok, entity = pcall(System.GetEntity, entry.nativeId)
    if not ok or not entity or entity.id ~= entry.nativeId or identity(entity) ~= entry.wuid then
        retire(entry)
        return nil
    end
    return entity
end
local function report(entry)
    if entry.unreported <= 0 then return true end
    if reportCount >= MAX_REPORTS then return false end
    serial = serial + 1
    reports[serial] = { epoch=epoch, session=session, serial=serial,
        entityId=entry.entityId, uuid=entry.uuid, damage=entry.unreported }
    entry.unreported = 0
    reportCount = reportCount + 1
    return true
end
local function flushDeferred()
    for index=#deferred,1,-1 do
        if report(deferred[index]) then table.remove(deferred,index) end
    end
end
local function sample(entry)
    local entity = current(entry)
    if not entity then return end
    local nowHealth = health(entity)
    if nowHealth == nil then retire(entry); return end
    if nowHealth < entry.nativeHealth then
        entry.unreported = entry.unreported + (entry.nativeHealth-nowHealth)/5
        report(entry)
    end
    if nowHealth <= 0 then retire(entry); return end
    if nowHealth ~= entry.nativeHealth then
        -- Restore only observed native loss; the eventual recipient runs vanilla
        -- MC damage, armour, immunity, effects and death once for this report.
        local ok, accepted = pcall(entity.soul.SetState, entity.soul, "health", entry.nativeHealth)
        local restored = health(entity)
        if not ok or accepted == false or not restored or math.abs(restored-entry.nativeHealth) > .01 then
            retire(entry)
        end
    end
end

function M.reset()
    for _, entry in pairs(byMinecraft) do retire(entry) end
    byMinecraft, byNative, reports, deferred = {}, {}, {}, {}
    epoch, session, reportCount = nil, nil, 0
end

function M.keeps(entity)
    if not entity or not entity.id then return false end
    local entry = byNative[tostring(entity.id)]
    local now = clock()
    return entry ~= nil and now ~= nil and now >= entry.seen and now-entry.seen <= TIMEOUT
        and entry.entity ~= nil and identity(entity) == entry.wuid
end

function M.resolve(entityId, uuid)
    local entry = byMinecraft[entityId]
    if not entry or uuid and entry.uuid ~= uuid then return nil end
    local entity = current(entry)
    return entity and M.keeps(entity) and entity or nil
end

local function valid(row)
    return type(row) == "table" and finite(row.entityId) and row.entityId > 0
        and row.entityId <= 2147483647 and row.entityId == math.floor(row.entityId)
        and type(row.uuid) == "string" and row.uuid:match("^[0-9a-f]+$") ~= nil and #row.uuid == 32
        and finite(row.x) and finite(row.y) and finite(row.z) and finite(row.yaw)
        and finite(row.width) and row.width >= .05 and row.width <= 16
        and finite(row.height) and row.height >= .05 and row.height <= 16
        and finite(row.health) and finite(row.maxHealth) and row.health > 0
        and row.maxHealth > 0 and row.health <= row.maxHealth and row.maxHealth <= 1000000
end

local function spawn(entry)
    if not XGenAIModule or not XGenAIModule.SpawnEntity then return false, "spawn API unavailable" end
    local row = entry.row
    serial = serial + 1 -- ensure names also differ after a dead double is replaced
    local name = "kingdomcraft_mob_" .. session .. "_" .. tostring(epoch) .. "_" .. row.uuid .. "_" .. tostring(serial)
    local ok, id = pcall(XGenAIModule.SpawnEntity, {
        Name=name, ClassName="NPC", SharedSoulGuid=PROFILE,
        Pos={x=row.x,y=-row.z,z=row.y}, NoAI=false, SuspendedAI=false,
        PerceptorObjectAI=false, PerceptibleObjectAI=true,
    })
    if not ok or not id then return false, "native spawn failed" end
    local found, entity = pcall(System.GetEntity, id)
    if not found or not entity or not entity.id then return false, "native entity unavailable" end
    local wuid, hp = identity(entity), health(entity)
    if not wuid or not hp or hp <= 0 or not entity.DrawSlot or not entity.SetWorldPos then
        return false, "native Soul or body unavailable"
    end
    local hidden, accepted = pcall(entity.DrawSlot, entity, 0, 0)
    if not hidden or accepted == false then return false, "native render slot unavailable" end
    entry.entity, entry.nativeId, entry.wuid, entry.nativeHealth = entity, entity.id, wuid, hp
    byNative[tostring(entity.id)] = entry
    return true
end

-- Validate the whole snapshot before changing identity maps. Reused MC IDs are
-- distinguished by UUID; a changed epoch/session releases every old double.
function M.sync(newEpoch, newSession, rows)
    local now = clock()
    if not now or not System.GetEntity then return {status="native clock/entity API unavailable"} end
    if not finite(newEpoch) or newEpoch < 0 or newEpoch ~= math.floor(newEpoch)
        or type(newSession) ~= "string" or #newSession ~= 32 or not newSession:match("^[0-9a-f]+$")
        or type(rows) ~= "table" or #rows > MAX_MOBS then return {status="invalid snapshot"} end
    local wanted, uuids, count = {}, {}, 0
    for index, row in pairs(rows) do
        if type(index) ~= "number" or index < 1 or index > #rows or index ~= math.floor(index)
            or not valid(row) or wanted[row.entityId] or uuids[row.uuid] then
            return {status="invalid snapshot"}
        end
        wanted[row.entityId], uuids[row.uuid], count = row, true, count+1
    end
    if count ~= #rows then return {status="invalid snapshot"} end
    if epoch ~= newEpoch or session ~= newSession then M.reset(); epoch, session = newEpoch, newSession end
    flushDeferred()
    for id, entry in pairs(byMinecraft) do
        if now < entry.seen or now-entry.seen > TIMEOUT then retire(entry)
        else sample(entry) end
        if not wanted[id] or wanted[id].uuid ~= entry.uuid then
            if not report(entry) then deferred[#deferred+1] = entry end
            retire(entry); byMinecraft[id] = nil
        end
    end
    local spawned, attempts, live, failures, reason = 0, 0, 0, 0, ""
    for _, row in ipairs(rows) do
        local entry = byMinecraft[row.entityId]
        if not entry then
            entry = {entityId=row.entityId,uuid=row.uuid,unreported=0}
            byMinecraft[row.entityId] = entry
        end
        entry.row, entry.seen = row, now
        local entity = current(entry)
        -- Stop adding native damage sources when delivery is congested. At most
        -- the existing 64 identities can then hold an unreported accumulator.
        if not entity and attempts < MAX_SPAWNS and reportCount < MAX_REPORTS and #deferred == 0 then
            attempts=attempts+1
            local ok, failure = spawn(entry)
            if ok then spawned=spawned+1; entity=entry.entity
            else failures=failures+1; reason=failure end
        end
        if entity then
            local ok, accepted = pcall(entity.SetWorldPos, entity, {x=row.x,y=-row.z,z=row.y})
            local read, position = pcall(entity.GetWorldPos, entity)
            if not ok or accepted == false or not read or not position
                or not finite(position.x) or not finite(position.y) or not finite(position.z)
                or math.abs(position.x-row.x)+math.abs(position.y+row.z)+math.abs(position.z-row.y) > .05 then
                retire(entry); failures=failures+1; reason="native move failed"
            else
                live=live+1
            end
        end
        report(entry)
    end
    return {status="ok",spawned=spawned,live=live,failed=failures,error=reason,
        geometry="native humanoid body; Minecraft dimensions not yet applied"}
end

function M.damageReports()
    flushDeferred()
    for _, entry in pairs(byMinecraft) do sample(entry); report(entry) end
    local out = {}
    for _, receipt in pairs(reports) do
        out[#out+1] = {epoch=receipt.epoch,session=receipt.session,serial=receipt.serial,
            entityId=receipt.entityId,uuid=receipt.uuid,damage=receipt.damage}
    end
    table.sort(out, function(a,b) return a.serial<b.serial end)
    return out
end

function M.acknowledge(reportEpoch, reportSession, receiptSerial, entityId, uuid)
    local receipt = reports[receiptSerial]
    if not receipt or receipt.epoch ~= reportEpoch or receipt.session ~= reportSession
        or receipt.entityId ~= entityId or receipt.uuid ~= uuid then return false end
    reports[receiptSerial], reportCount = nil, reportCount-1
    return true
end

if SkyCraftNativeMobs and SkyCraftNativeMobs.reset then SkyCraftNativeMobs.reset() end
SkyCraftNativeMobs = M
return M
