#include "skycraft_host.hpp"
#include "skycraft_input.hpp"
#include "skycraft_combat.hpp"
#include "skycraft_overlay.hpp"
#include "skycraft_physics.hpp"
#include "skycraft_surface.hpp"
#include "skycraft_world.hpp"
#include "skycraft_session.hpp"
#include "skycraft_projection.hpp"

#include "../bridge/skycraft_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

#include <kcd2_init.hpp>
#include <logger/logger.hpp>
#include <sol/sol.hpp>

namespace
{
    constexpr float kRadiansToDegrees = 57.29577951308232f;

    struct CameraFrame
    {
        std::uint32_t magic;
        std::uint32_t version;
        volatile LONG64 sequence;
        std::uint64_t frame;
        double x, y, z;
        float yaw, pitch, roll, fov;
        float nearPlane, farPlane;
        std::uint32_t width, height;
        std::uint32_t flags;
        std::uint32_t hostPid;
        std::int32_t cursorX, cursorY;
        std::uint32_t cursorVisible;
    };
    static_assert(offsetof(CameraFrame, x) == 24);
    static_assert(offsetof(CameraFrame, flags) == 80);
    static_assert(offsetof(CameraFrame, cursorX) == 88);
    constexpr std::uint32_t kCameraMagic = 0x4332434B; // KC2C
    HANDLE cameraMapping = nullptr;
    CameraFrame* cameraView = nullptr;
    std::uint64_t cameraFrame = 0;
    std::uint32_t lastViewportW = 0;
    std::uint32_t lastViewportH = 0;
    HANDLE passthroughMapping = nullptr;
    const std::uint8_t* passthroughHeader = nullptr;
    std::uint64_t nextPassthroughOpenMs = 0;

    void WriteCameraFrame(double x, double y, double z, float yaw, float pitch,
        float fov, bool valid, bool firstPerson,
        skycraft::camera::ClipPlanes planes = {})
    {
        if (!cameraView) {
            cameraMapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                PAGE_READWRITE, 0, sizeof(CameraFrame), L"Local\\SkyCraftKCD2Camera");
            if (!cameraMapping) {
                return;
            }
            cameraView = static_cast<CameraFrame*>(::MapViewOfFile(cameraMapping,
                FILE_MAP_WRITE, 0, 0, sizeof(CameraFrame)));
            if (!cameraView) {
                ::CloseHandle(cameraMapping);
                cameraMapping = nullptr;
                return;
            }
            cameraView->magic = kCameraMagic;
            cameraView->version = 1;
            cameraView->sequence = 0;
            cameraView->hostPid = ::GetCurrentProcessId();
            LOG(INFO) << "SkyCraft: KCD2 camera mapping ready";
        }
        ::InterlockedIncrement64(&cameraView->sequence);
        cameraView->frame = ++cameraFrame;
        cameraView->x = x;
        cameraView->y = y;
        cameraView->z = z;
        cameraView->yaw = yaw;
        cameraView->pitch = pitch;
        cameraView->roll = 0;
        cameraView->fov = fov;
        cameraView->nearPlane = planes.nearPlane;
        cameraView->farPlane = planes.farPlane;
        cameraView->width = lastViewportW;
        cameraView->height = lastViewportH;
        cameraView->flags = valid ? (firstPerson ? 3u : 1u) : 0u;
        if (valid && planes.valid) cameraView->flags |= 4u;
        static std::uint64_t nextPlaneReport = 0;
        if (valid && ::GetTickCount64() >= nextPlaneReport) {
            nextPlaneReport = ::GetTickCount64() + 15000;
            LOG(INFO) << "SkyCraft: host depth projection " << planes.valid
                      << " near " << planes.nearPlane << " far " << planes.farPlane
                      << " fov " << fov << " source System.ProjectToScreen"
                      << (planes.valid ? "" : " (using fallback planes)");
        }
        int cursorX = 0, cursorY = 0;
        cameraView->cursorVisible = SkyCraftInputCursor(cursorX, cursorY);
        cameraView->cursorX = cursorX;
        cameraView->cursorY = cursorY;
        ::MemoryBarrier();
        ::InterlockedIncrement64(&cameraView->sequence);
    }

    skycraft::camera::ClipPlanes SnapshotPlanes(const sol::table& snapshot)
    {
        return skycraft::camera::DecodeDepthSamples(1.0,
            snapshot.get_or("depth1", -1.0), 8.0,
            snapshot.get_or("depth8", -1.0));
    }

    sol::protected_function gameSnapshot;
    sol::protected_function puppetCommand;
    sol::protected_function gameRaycast;
    sol::protected_function groundRaycast;
    sol::protected_function appearanceCommand;
    sol::protected_function actionRoutingCommand;
    bool snapshotPrepared = false;
    bool puppetPrepared = false;
    bool puppetCapturing = false;
    std::uint64_t lastPuppetReportMs = 0;
    bool raycastPrepared = false;
    bool groundRaycastPrepared = false;
    bool appearancePrepared = false;
    bool appearanceActive = false;
    std::uint64_t lastAppearanceTickMs = 0;
    bool actionRoutingPrepared = false;
    bool actionRoutingActive = false;
    std::uint64_t lastActionRoutingTickMs = 0;
    bool mappingCreated = false;
    bool mcWasAlive = false;
    std::uint32_t teleportSeq = 1;
    std::uint32_t collisionEpoch = 1;
    bool hadPlayer = false;
    skycraft::HostSession hostSession;
    std::string activeLevelName;
    std::uint32_t activeWorldId = 1;
    std::uint64_t discardedRenderMessages = 0;
    std::uint64_t discardedRenderBytes = 0;
    std::uint64_t lastTransportReportMs = 0;
    std::uint64_t lastRaySampleMs = 0;
    std::uint64_t lastRayReportMs = 0;
    std::uint64_t lastGroundScanMs = 0;
    std::uint64_t lastGroundReportMs = 0;
    std::vector<std::pair<int, int>> groundOffsets;
    std::size_t groundCursor = 0;
    int groundCenterX = std::numeric_limits<int>::min();
    int groundCenterZ = std::numeric_limits<int>::min();
    std::uint64_t groundSamples = 0;
    std::uint64_t groundHits = 0;
    std::uint64_t lastClientLaunchMs = 0;

    std::uint32_t WorldIdForLevel(const std::string& level)
    {
        std::uint32_t hash = 2166136261u;
        for (unsigned char value : level) {
            if (value >= 'A' && value <= 'Z') {
                value = static_cast<unsigned char>(value - 'A' + 'a');
            }
            hash = (hash ^ value) * 16777619u;
        }
        return hash == 0 ? 1 : hash;
    }

    void LaunchMinecraftIfMissing(bool inGame, bool mcAlive)
    {
        const auto now = ::GetTickCount64();
        if (!inGame || mcAlive || now - lastClientLaunchMs < 30000) {
            return;
        }
        lastClientLaunchMs = now;
        constexpr wchar_t script[] =
            L"E:\\SkyCraftKCD2Tools\\start-skycraft-kcd2.ps1";
        if (::GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
            LOG(WARNING) << "SkyCraft: Minecraft launch script missing on E drive";
            return;
        }
        std::wstring command =
            L"\"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" "
            L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \""
            + std::wstring(script) + L"\" -MinecraftOnly";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            LOG(INFO) << "SkyCraft: starting hidden Minecraft client";
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);
        } else {
            LOG(WARNING) << "SkyCraft: Minecraft client launch failed ("
                         << ::GetLastError() << ")";
        }
    }

    void ReceiveRenderMessage(std::uint32_t type, const std::uint8_t* payload,
        std::uint32_t bytes, void*)
    {
        ++discardedRenderMessages;
        discardedRenderBytes += bytes;
        SkyCraftWorldAcceptRender(type, payload, bytes);
    }

    bool PrepareSnapshot(sol::state_view& lua)
    {
        if (snapshotPrepared) {
            return gameSnapshot.valid();
        }
        snapshotPrepared = true;
        auto result = lua.safe_script(R"lua(
            local loadRevision = 0
            local wrapped = setmetatable({}, { __mode = "k" })
            local function watchLoads(owner)
                if type(owner) ~= "table" then return end
                for _, name in ipairs({ "OnLoadAI", "OnReset" }) do
                    local original = owner[name]
                    if type(original) == "function" and not wrapped[original] then
                        local wrapper = function(self, ...)
                            loadRevision = loadRevision + 1
                            return original(self, ...)
                        end
                        wrapped[wrapper] = true
                        owner[name] = wrapper
                    end
                end
            end
            return function()
                watchLoads(Player)
                watchLoads(player)
                if not player or not System or not System.GetViewCameraDir then
                    return nil
                end
                if not player.GetWorldPos then return nil end
                local okPosition, position = pcall(player.GetWorldPos, player)
                if not okPosition then return nil end
                local alive = true
                if player.soul and player.soul.GetState then
                    local ok, health = pcall(player.soul.GetState, player.soul, "health")
                    if not ok or type(health) ~= "number" then return nil end
                    alive = health > 0
                end
                local direction = System.GetViewCameraDir()
                local camera = System.GetViewCameraPos and System.GetViewCameraPos()
                if not position or not direction then
                    return nil
                end
                local fov = 70
                if System.GetViewCameraFov then
                    local ok, radians = pcall(System.GetViewCameraFov)
                    if ok and type(radians) == "number" then
                        local degrees = radians * 180 / math.pi
                        if degrees >= 20 and degrees <= 130 then fov = degrees end
                    end
                end
                local depth1, depth8 = nil, nil
                if camera and System.ProjectToScreen then
                    local length = math.sqrt(direction.x * direction.x
                        + direction.y * direction.y + direction.z * direction.z)
                    if length > 0.001 then
                        local function probe(distance)
                            local ok, projected = pcall(System.ProjectToScreen, {
                                x = camera.x + direction.x * distance / length,
                                y = camera.y + direction.y * distance / length,
                                z = camera.z + direction.z * distance / length
                            })
                            if ok and projected and type(projected.z) == "number" then
                                return projected
                            end
                        end
                        local p1, p8 = probe(1), probe(8)
                        if p1 and p8 and type(p1.x) == "number" and type(p1.y) == "number"
                            and type(p8.x) == "number" and type(p8.y) == "number"
                            and math.abs(p1.x - p8.x) < 0.5
                            and math.abs(p1.y - p8.y) < 0.5 then
                            depth1, depth8 = p1.z, p8.z
                        end
                    end
                end
                local levelName = nil
                if System.GetCVar then
                    local ok, value = pcall(System.GetCVar, "sv_map")
                    if ok and type(value) == "string" and #value > 0
                        and #value <= 128 then
                        levelName = value
                    end
                end
                if not levelName then
                    for _, source in ipairs({ System, Game }) do
                        if source then
                            local getter = source.GetLevelName
                                or source.GetCurrentLevelName
                            if getter then
                                local ok, value = pcall(getter, source)
                                if ok and type(value) == "string"
                                    and #value > 0 and #value <= 128 then
                                    levelName = value
                                    break
                                end
                            end
                        end
                    end
                end
                return {
                    x = position.x, y = position.y, z = position.z,
                    dx = direction.x, dy = direction.y, dz = direction.z,
                    cx = camera and camera.x or position.x,
                    cy = camera and camera.y or position.y,
                    cz = camera and camera.z or position.z + 1.6,
                    fov = fov, depth1 = depth1, depth8 = depth8,
                    levelName = levelName,
                    playerIdentity = tostring(player.id) .. ":" .. tostring(player),
                    loadRevision = loadRevision, alive = alive
                }
            end
        )lua", sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(ERROR) << "SkyCraft: unable to prepare game snapshot: " << error.what();
            return false;
        }
        gameSnapshot = result.get<sol::protected_function>();
        return true;
    }

    bool PreparePuppet(sol::state_view& lua)
    {
        if (puppetPrepared) {
            return puppetCommand.valid();
        }
        puppetPrepared = true;
        auto result = lua.safe_script(R"lua(
            local lastPlayerId = nil
            local lastWorld = nil
            return function(action, mcX, mcY, mcZ)
                if action == "reset" then
                    lastPlayerId = nil
                    lastWorld = nil
                    return { status = "reset" }
                end
                if not player or not player.id or not player.GetWorldPos
                    or not player.SetWorldPos then
                    return { status = "player unavailable" }
                end
                local read, current = pcall(player.GetWorldPos, player)
                if not read or not current then
                    return { status = "position unavailable" }
                end
                if lastPlayerId ~= player.id then
                    lastPlayerId = player.id
                    lastWorld = nil
                end
                if lastWorld then
                    local dx = current.x - lastWorld.x
                    local dy = current.y - lastWorld.y
                    local dz = current.z - lastWorld.z
                    if dx * dx + dy * dy + dz * dz > 9 then
                        lastWorld = nil
                        return { status = "native teleport" }
                    end
                end
                local destination = { x = mcX, y = -mcZ, z = mcY }
                local dx = current.x - destination.x
                local dy = current.y - destination.y
                local dz = current.z - destination.z
                if dx * dx + dy * dy + dz * dz < 0.0004 then
                    lastWorld = destination
                    return { status = "steady" }
                end
                local moved, accepted = pcall(player.SetWorldPos,
                    player, destination)
                if not moved or accepted == false then
                    return { status = "move failed" }
                end
                lastWorld = destination
                return { status = "moved" }
            end
        )lua", sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(ERROR) << "SkyCraft: player puppet unavailable: "
                       << error.what();
            return false;
        }
        puppetCommand = result.get<sol::protected_function>();
        return true;
    }

    bool UpdatePuppet(sol::state_view& lua, bool minecraftAlive)
    {
        if (!minecraftAlive || !hadPlayer || SkyCraftInputNativeHandoff()) {
            if (puppetCapturing && puppetCommand.valid()) {
                puppetCommand("reset", 0, 0, 0);
            }
            puppetCapturing = false;
            return false;
        }
        // Alt-Tab only pauses input. Keep the acknowledged teleport and collision
        // session so returning to the game does not freeze the MC player again.
        if (!SkyCraftInputCapturingMouse()) return false;
        if (!PreparePuppet(lua)) {
            return false;
        }
        skycraft::proto::McState mc{};
        if (!SkyCraftBridgeReadMcState(&mc)
            || !(mc.flags & skycraft::proto::kMcInWorld)
            || (mc.flags & skycraft::proto::kMcDead)
            || !std::isfinite(mc.x) || !std::isfinite(mc.y)
            || !std::isfinite(mc.z) || std::abs(mc.x) > 100000
            || std::abs(mc.y) > 100000 || std::abs(mc.z) > 100000) {
            return false;
        }
        if (!puppetCapturing) {
            puppetCapturing = true;
            if (mc.teleportAck == teleportSeq) {
                ++teleportSeq;
                ++collisionEpoch;
            }
            return false;
        }
        if (mc.teleportAck != teleportSeq) {
            return false;
        }
        auto result = puppetCommand("move", mc.x, mc.y, mc.z);
        const auto now = ::GetTickCount64();
        if (!result.valid()) {
            const sol::error error = result;
            if (now - lastPuppetReportMs >= 10000) {
                lastPuppetReportMs = now;
                LOG(WARNING) << "SkyCraft: player puppet failed: "
                             << error.what();
            }
            return false;
        }
        const sol::object value = result.get<sol::object>();
        if (!value.is<sol::table>()) {
            return false;
        }
        const auto status = value.as<sol::table>().get_or("status", std::string{});
        if (status == "native teleport") {
            ++teleportSeq;
            ++collisionEpoch;
            LOG(INFO) << "SkyCraft: KCD2 relocated player; resynchronizing Minecraft";
        } else if (status != "moved" && status != "steady"
            && now - lastPuppetReportMs >= 10000) {
            lastPuppetReportMs = now;
            LOG(WARNING) << "SkyCraft: player puppet " << status;
        }
        return status == "moved";
    }

    bool PrepareRaycast(sol::state_view& lua)
    {
        if (raycastPrepared) {
            return gameRaycast.valid();
        }
        raycastPrepared = true;
        auto result = lua.safe_script(R"lua(
            return function()
                if not player or not player.id or not Physics
                    or not Physics.RayWorldIntersection
                    or not System.GetViewCameraPos or not System.GetViewCameraDir then
                    return { status = "unavailable" }
                end
                local origin = System.GetViewCameraPos()
                local direction = System.GetViewCameraDir()
                if not origin or not direction then
                    return { status = "unavailable" }
                end
                local ray = { x = direction.x * 4.5,
                    y = direction.y * 4.5, z = direction.z * 4.5 }
                local hits = {}
                local ok, count = pcall(Physics.RayWorldIntersection,
                    origin, ray, 1, ent_all, player.id, nil, hits)
                if not ok then
                    return { status = "error", error = tostring(count) }
                end
                local hit = hits[1]
                local pos = hit and hit.pos
                if not count or count < 1 or not pos then
                    return { status = "miss" }
                end
                local normal = hit.normal or hit.n
                local terrain = hit.bTerrain == true or hit.bTerrain == 1
                    or hit.terrain == true or hit.terrain == 1
                return { status = "hit", x = pos.x, y = pos.y, z = pos.z,
                    nx = normal and normal.x or -direction.x,
                    ny = normal and normal.y or -direction.y,
                    nz = normal and normal.z or -direction.z,
                    terrain = terrain, hasNormal = normal ~= nil }
            end
        )lua", sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(ERROR) << "SkyCraft: Lua physics ray unavailable: "
                       << error.what();
            return false;
        }
        gameRaycast = result.get<sol::protected_function>();
        return true;
    }

    bool PrepareGroundRaycast(sol::state_view& lua)
    {
        if (groundRaycastPrepared) {
            return groundRaycast.valid();
        }
        groundRaycastPrepared = true;
        auto result = lua.safe_script(R"lua(
            return function(x, z, feetHeight)
                if not player or not player.id or not Physics
                    or not Physics.RayWorldIntersection then
                    return { status = "unavailable" }
                end
                local origin = { x = x + 0.5, y = -z - 0.5,
                    z = feetHeight + 1.5 }
                local ray = { x = 0, y = 0, z = -8 }
                local hits = {}
                local ok, count = pcall(Physics.RayWorldIntersection,
                    origin, ray, 4, ent_all, player.id, nil, hits)
                if not ok then
                    return { status = "error", error = tostring(count) }
                end
                local highest = nil
                for _, hit in pairs(hits) do
                    local pos = hit and hit.pos
                    local entity = hit and hit.entity
                    local name = nil
                    if entity and entity.GetName then
                        local named, value = pcall(entity.GetName, entity)
                        if named then name = value end
                    end
                    if pos and type(pos.z) == "number"
                        and pos.z <= origin.z
                        and not (entity and (entity.actor or entity.human))
                        and (not name or name:sub(1, 19) ~= "skycraft_collision_")
                        and (not highest or pos.z > highest) then
                        highest = pos.z
                    end
                end
                if highest then
                    return { status = "hit", height = highest }
                end
                return { status = "miss" }
            end
        )lua", sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(ERROR) << "SkyCraft: ground ray preparation failed: "
                       << error.what();
            return false;
        }
        groundRaycast = result.get<sol::protected_function>();
        return true;
    }

    void ScanGround(sol::state_view& lua,
        const skycraft::proto::SkyState& host)
    {
        const auto now = ::GetTickCount64();
        if (now - lastGroundScanMs < 100 || !PrepareGroundRaycast(lua)) {
            return;
        }
        lastGroundScanMs = now;
        if (groundOffsets.empty()) {
            for (int x = -8; x <= 8; ++x) {
                for (int z = -8; z <= 8; ++z) {
                    if (x * x + z * z <= 64) {
                        groundOffsets.emplace_back(x, z);
                    }
                }
            }
            std::sort(groundOffsets.begin(), groundOffsets.end(),
                [](auto a, auto b) {
                    const int da = a.first * a.first + a.second * a.second;
                    const int db = b.first * b.first + b.second * b.second;
                    return da != db ? da < db : a < b;
                });
        }
        const int centerX = int(std::floor(host.posX));
        const int centerZ = int(std::floor(host.posZ));
        if (centerX != groundCenterX || centerZ != groundCenterZ) {
            groundCenterX = centerX;
            groundCenterZ = centerZ;
            groundCursor = 0;
        }
        for (int i = 0; i < 6; ++i) {
            if (groundCursor == groundOffsets.size()) {
                groundCursor = 0;
            }
            const auto [dx, dz] = groundOffsets[groundCursor++];
            const int x = centerX + dx, z = centerZ + dz;
            auto result = groundRaycast(x, z, host.posY);
            if (!result.valid()) {
                const sol::error error = result;
                if (now - lastGroundReportMs >= 10000) {
                    lastGroundReportMs = now;
                    LOG(WARNING) << "SkyCraft: ground ray failed: "
                                 << error.what();
                }
                continue;
            }
            const sol::object value = result.get<sol::object>();
            if (!value.is<sol::table>()) {
                continue;
            }
            const sol::table probe = value.as<sol::table>();
            const auto status = probe.get_or("status", std::string{});
            if (status == "hit") {
                SkyCraftGroundSample(x, z,
                    probe.get_or("height", host.posY), true,
                    host.collisionEpoch);
                ++groundHits;
            } else if (status == "miss") {
                SkyCraftGroundSample(x, z, 0, false,
                    host.collisionEpoch);
            } else {
                continue;
            }
            ++groundSamples;
        }
        SkyCraftGroundPrune(host.posX, host.posZ, host.collisionEpoch);
        if (now - lastGroundReportMs >= 10000) {
            lastGroundReportMs = now;
            LOG(INFO) << "SkyCraft: ground rays " << groundHits
                      << " hit of " << groundSamples << " sampled";
            groundSamples = 0;
            groundHits = 0;
        }
    }

    void UpdateAppearance(sol::state_view& lua, bool active)
    {
        const auto now = ::GetTickCount64();
        if (active == appearanceActive && now - lastAppearanceTickMs < 500) {
            return;
        }
        lastAppearanceTickMs = now;
        if (!appearancePrepared) {
            appearancePrepared = true;
            auto result = lua.safe_script(R"lua(
                local originalNear = nil
                local hiddenPlayerId = nil
                local slotHidden = false
                local function restore()
                    if slotHidden and hiddenPlayerId and System and System.GetEntity then
                        local oldPlayer = System.GetEntity(hiddenPlayerId)
                        if oldPlayer and oldPlayer.DrawSlot then
                            pcall(oldPlayer.DrawSlot, oldPlayer, 0, 1)
                        end
                    end
                    slotHidden = false
                    hiddenPlayerId = nil
                    if originalNear ~= nil and System and System.SetCVar then
                        System.SetCVar("r_NoDrawNear", originalNear)
                        originalNear = nil
                    end
                end
                return function(enabled)
                    if not enabled or not player or not player.id then
                        restore()
                        return { active = false }
                    end
                    if hiddenPlayerId and hiddenPlayerId ~= player.id then
                        restore()
                    end
                    if originalNear == nil and System and System.GetCVar then
                        originalNear = System.GetCVar("r_NoDrawNear")
                    end
                    if originalNear ~= nil and System and System.SetCVar then
                        System.SetCVar("r_NoDrawNear", 1)
                    end
                    hiddenPlayerId = player.id
                    if player.DrawSlot then
                        slotHidden = pcall(player.DrawSlot, player, 0, 0)
                    end
                    return { active = true, slotHidden = slotHidden,
                        nearHidden = originalNear ~= nil }
                end
            )lua", sol::script_pass_on_error);
            if (!result.valid()) {
                const sol::error error = result;
                LOG(ERROR) << "SkyCraft: appearance bridge unavailable: "
                           << error.what();
                return;
            }
            appearanceCommand = result.get<sol::protected_function>();
        }
        if (!appearanceCommand.valid()) {
            return;
        }
        auto result = appearanceCommand(active);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(WARNING) << "SkyCraft: player appearance update failed: "
                         << error.what();
            return;
        }
        if (active != appearanceActive) {
            appearanceActive = active;
            const auto response = result.get<sol::table>();
            LOG(INFO) << "SkyCraft: Minecraft appearance "
                      << (active ? "enabled" : "disabled")
                      << " (player slot hidden "
                      << response.get_or("slotHidden", false)
                      << ", KCD2 near view hidden "
                      << response.get_or("nearHidden", false) << ")";
        }
    }

    void UpdateNativeActionRouting(sol::state_view& lua, bool active)
    {
        const auto now = ::GetTickCount64();
        if (active == actionRoutingActive && now - lastActionRoutingTickMs < 500) {
            return;
        }
        lastActionRoutingTickMs = now;
        if (!actionRoutingPrepared) {
            actionRoutingPrepared = true;
            auto result = lua.safe_script(R"lua(
                local attackMaps = { "combat", "combat_base", "stone_throwing" }
                local previousMaps = {}
                local previousAttackFilter = nil
                local previousDistractFilter = nil
                local wasEnabled = false
                return function(enabled)
                    if not ActionMapManager then
                        return { error = "ActionMapManager unavailable" }
                    end
                    local manager = ActionMapManager
                    local filter = manager.EnableActionFilter or manager.EnableFilter
                    if enabled and not wasEnabled then
                        if manager.IsActionMapEnabled then
                            for _, name in ipairs(attackMaps) do
                                local ok, value = pcall(manager.IsActionMapEnabled, name)
                                if ok and type(value) == "boolean" then
                                    previousMaps[name] = value
                                end
                            end
                        end
                        if manager.IsActionFilterEnabled then
                            local ok, value = pcall(manager.IsActionFilterEnabled, "no_attack")
                            if ok and type(value) == "boolean" then
                                previousAttackFilter = value
                            end
                            ok, value = pcall(manager.IsActionFilterEnabled, "no_distract")
                            if ok and type(value) == "boolean" then
                                previousDistractFilter = value
                            end
                        end
                    end
                    local attackFiltered = false
                    local distractFiltered = false
                    if filter and previousAttackFilter ~= nil then
                        attackFiltered = pcall(filter, "no_attack",
                            enabled or previousAttackFilter == true)
                    end
                    if filter and previousDistractFilter ~= nil then
                        distractFiltered = pcall(filter, "no_distract",
                            enabled or previousDistractFilter == true)
                    end
                    local attackMapsChanged = 0
                    local unknownMapStates = 0
                    if manager.EnableActionMap then
                        for _, name in ipairs(attackMaps) do
                            local previous = previousMaps[name]
                            if previous == nil then
                                unknownMapStates = unknownMapStates + 1
                            elseif enabled then
                                if pcall(manager.EnableActionMap, name, false) then
                                    attackMapsChanged = attackMapsChanged + 1
                                end
                            elseif wasEnabled then
                                if pcall(manager.EnableActionMap, name, previous) then
                                    attackMapsChanged = attackMapsChanged + 1
                                end
                            end
                        end
                    end
                    wasEnabled = enabled
                    if not enabled then
                        previousMaps = {}
                        previousAttackFilter = nil
                        previousDistractFilter = nil
                    end
                    return { attackFiltered = attackFiltered,
                        distractFiltered = distractFiltered,
                        attackMapsChanged = attackMapsChanged,
                        unknownMapStates = unknownMapStates,
                        filterAvailable = filter ~= nil }
                end
            )lua", sol::script_pass_on_error);
            if (!result.valid()) {
                const sol::error error = result;
                LOG(ERROR) << "SkyCraft: native action routing unavailable: "
                           << error.what();
                return;
            }
            actionRoutingCommand = result.get<sol::protected_function>();
        }
        if (!actionRoutingCommand.valid()) {
            return;
        }
        auto result = actionRoutingCommand(active);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(WARNING) << "SkyCraft: native action routing failed: "
                         << error.what();
            return;
        }
        if (active != actionRoutingActive) {
            actionRoutingActive = active;
            const auto response = result.get<sol::table>();
            LOG(INFO) << "SkyCraft: native action routing "
                      << (active ? "Minecraft" : "KCD2")
                      << " (attack filter "
                      << response.get_or("attackFiltered", false)
                      << ", distract filter "
                      << response.get_or("distractFiltered", false)
                      << ", attack maps "
                      << response.get_or("attackMapsChanged", 0)
                      << ", unknown map states "
                      << response.get_or("unknownMapStates", 0)
                      << ", filter API "
                      << response.get_or("filterAvailable", false) << ") "
                      << response.get_or("error", std::string{});
        }
    }

    void UpdateSurface(sol::state_view& lua, std::uint32_t epoch)
    {
        const auto now = ::GetTickCount64();
        if (now - lastRaySampleMs < 100) {
            return;
        }
        lastRaySampleMs = now;
        if (!PrepareRaycast(lua)) {
            SkyCraftSurfaceClear(epoch);
            return;
        }
        auto result = gameRaycast();
        if (!result.valid()) {
            const sol::error error = result;
            if (now - lastRayReportMs >= 10000) {
                lastRayReportMs = now;
                LOG(WARNING) << "SkyCraft: Lua physics ray failed: "
                             << error.what();
            }
            SkyCraftSurfaceClear(epoch);
            return;
        }
        const sol::object value = result.get<sol::object>();
        if (!value.is<sol::table>()) {
            SkyCraftSurfaceClear(epoch);
            return;
        }
        const sol::table probe = value.as<sol::table>();
        const auto status = probe.get_or("status", std::string{});
        const bool report = now - lastRayReportMs >= 10000;
        if (report) {
            lastRayReportMs = now;
        }
        const auto error = probe.get_or("error", std::string{});
        if (status != "hit") {
            if (report) {
                LOG(INFO) << "SkyCraft: Lua physics ray " << status << " " << error;
            }
            SkyCraftSurfaceClear(epoch);
            return;
        }
        const SkyCraftSurfaceHit hit{
            probe.get_or("x", 0.0f), probe.get_or("y", 0.0f),
            probe.get_or("z", 0.0f), probe.get_or("nx", 0.0f),
            probe.get_or("ny", 0.0f), probe.get_or("nz", 0.0f),
            probe.get_or("terrain", false)
        };
        if (report) {
            LOG(INFO) << "SkyCraft: Lua aim surface " << hit.x << ", "
                      << hit.y << ", " << hit.z << " normal "
                      << hit.nx << ", " << hit.ny << ", " << hit.nz
                      << " (native normal " << probe.get_or("hasNormal", false)
                      << ", terrain " << hit.terrain << ")";
        }
        SkyCraftSurfaceTick(hit, epoch);
    }

    void SetViewport(skycraft::proto::SkyState& host)
    {
        HWND window = ::GetForegroundWindow();
        DWORD owner = 0;
        if (window) {
            ::GetWindowThreadProcessId(window, &owner);
        }
        RECT client{};
        if (owner == ::GetCurrentProcessId() && ::GetClientRect(window, &client)) {
            lastViewportW = std::max(1L, client.right - client.left);
            lastViewportH = std::max(1L, client.bottom - client.top);
        }
        if (!lastViewportW || !lastViewportH) {
            lastViewportW = ::GetSystemMetrics(SM_CXSCREEN);
            lastViewportH = ::GetSystemMetrics(SM_CYSCREEN);
        }
        host.viewportW = lastViewportW;
        host.viewportH = lastViewportH;
    }
}

bool SkyCraftHostPassthroughActive()
{
    if (!SkyCraftBridgeMcAlive()) {
        return false;
    }
    if (!passthroughHeader && ::GetTickCount64() >= nextPassthroughOpenMs) {
        nextPassthroughOpenMs = ::GetTickCount64() + 1000;
        passthroughMapping = ::OpenFileMappingW(FILE_MAP_READ, FALSE,
            L"Local\\SkyCraftKCD2Frame");
        if (passthroughMapping) {
            passthroughHeader = static_cast<const std::uint8_t*>(::MapViewOfFile(
                passthroughMapping, FILE_MAP_READ, 0, 0, 48));
            if (!passthroughHeader) {
                ::CloseHandle(passthroughMapping);
                passthroughMapping = nullptr;
            }
        }
    }
    if (!passthroughHeader) {
        return false;
    }
    std::uint32_t magic = 0, version = 0, mcPid = 0;
    std::uint64_t published = 0;
    std::memcpy(&magic, passthroughHeader, sizeof(magic));
    std::memcpy(&version, passthroughHeader + 4, sizeof(version));
    std::memcpy(&published, passthroughHeader + 32, sizeof(published));
    std::memcpy(&mcPid, passthroughHeader + 44, sizeof(mcPid));
    return magic == 0x5450434D && version == 1 && published != 0
        && mcPid == SkyCraftBridgeMcPid();
}

void SkyCraftHostTick(lua_State* state)
{
    SkyCraftInputInstallHooks();
    if (!mappingCreated) {
        mappingCreated = SkyCraftBridgeCreate();
        if (!mappingCreated) {
            return;
        }
        LOG(INFO) << "SkyCraft: KCD2 shared-memory host started (session-recovery 20261006)";
    }
    SkyCraftBridgeHeartbeat();

    const bool mcAlive = SkyCraftBridgeMcAlive();
    const bool linkRestart = mcAlive && !mcWasAlive;
    if (mcAlive != mcWasAlive) {
        mcWasAlive = mcAlive;
        SkyCraftInputResetSession();
        puppetCapturing = false;
        if (puppetCommand.valid()) puppetCommand("reset", 0, 0, 0);
        LOG(INFO) << "SkyCraft: Minecraft link " << (mcAlive ? "up" : "down");
    }

    sol::state_view lua(state);
    if (!PrepareSnapshot(lua)) {
        return;
    }
    skycraft::proto::SkyState host{};
    SetViewport(host);

    host.worldId = activeWorldId;
    host.collisionEpoch = collisionEpoch;
    host.teleportSeq = teleportSeq;
    host.gameHour = 12.0f;

    bool cameraUpdated = false;
    auto result = gameSnapshot();
    if (result.valid()) {
        sol::object value = result.get<sol::object>();
        if (value.is<sol::table>()) {
            const sol::table snapshot = value.as<sol::table>();
            const float x = snapshot.get_or("x", 0.0f);
            const float y = snapshot.get_or("y", 0.0f);
            const float z = snapshot.get_or("z", 0.0f);
            const float dx = snapshot.get_or("dx", 0.0f);
            const float dy = snapshot.get_or("dy", 0.0f);
            const float dz = snapshot.get_or("dz", 0.0f);
            const float horizontal = std::hypot(dx, dy);
            if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)
                && horizontal > 0.001f && std::isfinite(dz)
                && snapshot.get_or("alive", true)) {
                const auto levelName = snapshot.get_or("levelName", std::string{});
                if (!levelName.empty() && levelName != activeLevelName) {
                    const auto previousWorldId = activeWorldId;
                    activeLevelName = levelName;
                    activeWorldId = WorldIdForLevel(levelName);
                    host.worldId = activeWorldId;
                    groundCenterX = std::numeric_limits<int>::min();
                    groundCenterZ = std::numeric_limits<int>::min();
                    LOG(INFO) << "SkyCraft: KCD2 level " << levelName
                              << " world ID " << previousWorldId << " -> "
                              << activeWorldId;
                }
                const bool newSession = hostSession.Observe(true,
                    snapshot.get_or("playerIdentity", std::string{}),
                    snapshot.get_or("loadRevision", 0u), activeWorldId);
                if (newSession || linkRestart) {
                    ++teleportSeq;
                    ++collisionEpoch;
                    host.collisionEpoch = collisionEpoch;
                    SkyCraftInputResetSession();
                    puppetCapturing = false;
                    if (puppetCommand.valid()) puppetCommand("reset", 0, 0, 0);
                    groundCenterX = std::numeric_limits<int>::min();
                    groundCenterZ = std::numeric_limits<int>::min();
                    groundCursor = 0;
                    LOG(INFO) << "SkyCraft: host save session recovered; teleport "
                              << teleportSeq << ", collision epoch " << collisionEpoch;
                }
                hadPlayer = true;
                host.flags = skycraft::proto::kSkyInGame;
                host.posX = x;
                host.posY = z;
                host.posZ = -y;
                host.yaw = std::atan2(-dx, -dy) * kRadiansToDegrees;
                host.pitch = -std::atan2(dz, horizontal) * kRadiansToDegrees;
                host.teleportSeq = teleportSeq;
                const float cx = snapshot.get_or("cx", x);
                const float cy = snapshot.get_or("cy", y);
                const float cz = snapshot.get_or("cz", z + 1.6f);
                if (std::isfinite(cx) && std::isfinite(cy) && std::isfinite(cz)) {
                    SkyCraftWorldSetCamera(cx, cz, -cy, host.yaw, host.pitch, true);
                    const bool firstPerson = std::hypot(cx - x, cy - y,
                        cz - z - 1.6f) < 1.25f;
                    WriteCameraFrame(cx, cz, -cy, host.yaw, host.pitch,
                        snapshot.get_or("fov", 70.0f), true, firstPerson,
                        SnapshotPlanes(snapshot));
                    cameraUpdated = true;
                }
            }
        }
    }
    if (!(host.flags & skycraft::proto::kSkyInGame)) {
        hostSession.Observe(false, {}, 0, 0);
        hadPlayer = false;
    }
    // Detect reload/death before moving the native player, otherwise a stale MC
    // position can overwrite the freshly loaded save in this very frame.
    SkyCraftInputTick(host.flags, host.viewportW, host.viewportH);
    if (UpdatePuppet(lua, mcAlive)) {
        auto refreshed = gameSnapshot();
        if (refreshed.valid()) {
            const sol::object value = refreshed.get<sol::object>();
            if (value.is<sol::table>()) {
                const auto current = value.as<sol::table>();
                host.posX = current.get_or("x", host.posX);
                host.posY = current.get_or("z", host.posY);
                host.posZ = -current.get_or("y", -host.posZ);
                const float cx = current.get_or("cx", host.posX);
                const float cy = current.get_or("cy", -host.posZ);
                const float cz = current.get_or("cz", host.posY + 1.6f);
                if (std::isfinite(cx) && std::isfinite(cy) && std::isfinite(cz)) {
                    SkyCraftWorldSetCamera(cx, cz, -cy, host.yaw, host.pitch, true);
                    WriteCameraFrame(cx, cz, -cy, host.yaw, host.pitch,
                        current.get_or("fov", 70.0f), true, std::hypot(cx - host.posX,
                            cy + host.posZ, cz - host.posY - 1.6f) < 1.25f,
                        SnapshotPlanes(current));
                }
            }
        }
    }
    host.teleportSeq = teleportSeq;
    host.collisionEpoch = collisionEpoch;
    if (!cameraUpdated) {
        SkyCraftWorldSetCamera(0, 0, 0, 0, 0, false);
        WriteCameraFrame(0, 0, 0, 0, 0, 70.0f, false, true);
    }
    if (cameraUpdated && mcAlive) {
        UpdateSurface(lua, host.collisionEpoch);
        ScanGround(lua, host);
    } else {
        SkyCraftSurfaceReset(host.collisionEpoch);
    }
    SkyCraftOverlaySetInGame((host.flags & skycraft::proto::kSkyInGame) != 0);
    SkyCraftBridgeWriteHostState(&host);
    LaunchMinecraftIfMissing((host.flags & skycraft::proto::kSkyInGame) != 0,
        mcAlive);
    UpdateNativeActionRouting(lua, SkyCraftInputCapturingMouse());
    skycraft::proto::McState mc{};
    const bool minecraftWorld = mcAlive && SkyCraftBridgeReadMcState(&mc)
        && (mc.flags & skycraft::proto::kMcInWorld);
    UpdateAppearance(lua, (host.flags & skycraft::proto::kSkyInGame)
        && minecraftWorld);

    SkyCraftBridgeDrainRender(ReceiveRenderMessage, nullptr, 64ull << 20);
    SkyCraftPhysicsTick(state, host, mcAlive);
    SkyCraftCombatTick(state, host, mcAlive);
    const auto now = ::GetTickCount64();
    if (mcAlive && now - lastTransportReportMs >= 10000) {
        LOG(INFO) << "SkyCraft: render transport received " << discardedRenderMessages
                  << " messages (" << discardedRenderBytes << " bytes)";
        discardedRenderMessages = 0;
        discardedRenderBytes = 0;
        lastTransportReportMs = now;
    }
}
