#include "skycraft_scene.hpp"
#include "skycraft_scene_geometry.hpp"
#include "../bridge/skycraft_kcd2_protocol.hpp"
#include <chrono>
#include <deque>
#include <set>
#include <tuple>
#include <windows.h>
#include <sol/sol.hpp>
#include <logger/logger.hpp>

namespace
{
    struct Request { int x,y,z; std::uint32_t epoch; };
    std::deque<Request> requests;
    std::set<std::tuple<int,int,int,std::uint32_t>> queued;
    sol::protected_function sceneQuery;
    std::uint32_t activeEpoch = 0;
    std::vector<std::uint8_t> pendingPacket;
    std::uint64_t acceptedCells = 0;
    std::uint64_t lastFailureReportMs = 0;

    bool PrepareSceneQuery(sol::state_view& lua)
    {
        if (sceneQuery.valid()) return true;
        auto result = lua.safe_script(R"lua(
            return function(x,y,z)
                if not player or not player.id or not Physics
                    or not Physics.RayWorldIntersection or not ent_all then
                    return { status="unavailable" }
                end
                local samples = {}
                local mask=ent_all
                if type(ent_living)=="number" and ent_living>0
                    and math.floor(mask/ent_living)%2==1 then mask=mask-ent_living end
                local function vec(a,b,c) return {x=a,y=-c,z=b} end
                for axis=1,3 do
                    for _, sign in ipairs({-1,1}) do
                        for _, a in ipairs({0.25,0.75}) do
                            for _, b in ipairs({0.25,0.75}) do
                                local p = {x+a,y+a,z+b}
                                if axis==1 then p={x+(sign==1 and -0.001 or 1.001),y+a,z+b}
                                elseif axis==2 then p={x+a,y+(sign==1 and -0.001 or 1.001),z+b}
                                else p={x+a,y+b,z+(sign==1 and -0.001 or 1.001)} end
                                local delta={0,0,0}
                                delta[axis]=sign*1.002
                                local hits={}
                                local ok,count=pcall(Physics.RayWorldIntersection,
                                    vec(p[1],p[2],p[3]),vec(delta[1],delta[2],delta[3]),
                                    8,mask,player.id,nil,hits)
                                if not ok then return {status="error",error=tostring(count)} end
                                if type(count)~="number" or count~=count or count<0
                                    or count>8 or count~=math.floor(count) then
                                    return {status="incomplete",error="ray count unavailable"}
                                end
                                for _,hit in pairs(hits) do
                                    local entity=hit and hit.entity
                                    local name=nil
                                    if entity and entity.GetName then
                                        local named,value=pcall(entity.GetName,entity)
                                        if named then name=value end
                                    end
                                    local pos=hit and hit.pos
                                    local normal=hit and (hit.normal or hit.n)
                                    if pos and not normal
                                        and not (entity and (entity.actor or entity.human))
                                        and (not name or name:sub(1,19)~="skycraft_collision_") then
                                        return {status="incomplete",error="normal unavailable"}
                                    end
                                    if pos and normal
                                        and not (entity and (entity.actor or entity.human))
                                        and (not name or name:sub(1,19)~="skycraft_collision_") then
                                        samples[#samples+1]={x=pos.x,y=pos.z,z=-pos.y,
                                            nx=normal.x,ny=normal.z,nz=-normal.y,
                                            terrain=hit.bTerrain==true or hit.bTerrain==1}
                                    end
                                end
                            end
                        end
                    end
                end
                return {status="ok",samples=samples}
            end
        )lua",sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error error = result;
            LOG(ERROR) << "SkyCraft: native scene sampler unavailable: " << error.what();
            return false;
        }
        sceneQuery = result.get<sol::protected_function>();
        return true;
    }
}

bool SkyCraftSceneAcceptRequest(std::uint32_t type,const std::uint8_t* payload,
    std::uint32_t bytes)
{
    if (type != skycraft::kcd2::kRenSceneRequest) return false;
    if (!payload || bytes<8) return true;
    std::uint32_t epoch=0,count=0;
    std::memcpy(&epoch,payload,4);
    std::memcpy(&count,payload+4,4);
    if (count>16 || bytes != 8+count*12 || epoch != activeEpoch) return true;
    for (std::uint32_t i=0;i<count && requests.size()<512;++i) {
        int coords[3]{};
        std::memcpy(coords,payload+8+i*12,12);
        if (std::abs(double(coords[0]))>100000 || std::abs(double(coords[1]))>100000
            || std::abs(double(coords[2]))>100000) continue;
        if (queued.emplace(coords[0],coords[1],coords[2],epoch).second)
            requests.push_back({coords[0],coords[1],coords[2],epoch});
    }
    return true;
}

void SkyCraftSceneTick(lua_State* state,const skycraft::proto::SkyState& host,bool active)
{
    if (!active || host.collisionEpoch != activeEpoch) {
        requests.clear(); queued.clear(); pendingPacket.clear();
        activeEpoch = active ? host.collisionEpoch : 0;
        if (!active) return;
    }
    if (!pendingPacket.empty()) {
        if (!SkyCraftBridgeWriteCollision(skycraft::kcd2::kColSceneCell,
            pendingPacket.data(),static_cast<std::uint32_t>(pendingPacket.size()))) return;
        pendingPacket.clear();
    }
    if (requests.empty()) return;
    sol::state_view lua(state);
    if (!PrepareSceneQuery(lua)) return;
    const auto start = std::chrono::steady_clock::now();
    // Each cell uses 24 rays. Bound both work count and elapsed time on the game thread.
    for (int i=0;i<2 && !requests.empty();++i) {
        const auto request=requests.front();
        requests.pop_front();
        queued.erase({request.x,request.y,request.z,request.epoch});
        const float dx=request.x+0.5f-host.posX,dz=request.z+0.5f-host.posZ;
        if (request.epoch!=host.collisionEpoch || dx*dx+dz*dz>225
            || std::abs(request.y+0.5f-host.posY)>10) continue;
        auto result=sceneQuery(request.x,request.y,request.z);
        if (!result.valid()) continue;
        const auto value=result.get<sol::object>();
        if (!value.is<sol::table>()) continue;
        const auto response=value.as<sol::table>();
        if (response.get_or("status",std::string{})!="ok") {
            const auto now=::GetTickCount64();
            if (now-lastFailureReportMs>=10000) {
                lastFailureReportMs=now;
                LOG(WARNING) << "SkyCraft: shared scene sample retained old geometry: "
                    << response.get_or("status",std::string{}) << " "
                    << response.get_or("error",std::string{});
            }
            continue;
        }
        skycraft::scene::Cell cell(request.x,request.y,request.z,request.epoch);
        sol::object rows=response["samples"];
        if (!rows.is<sol::table>()) continue;
        bool complete=true;
        for (const auto& row : rows.as<sol::table>()) {
            if (!row.second.is<sol::table>()) { complete=false; break; }
            const auto sample=row.second.as<sol::table>();
            if (!cell.AddSample({{sample.get_or("x",NAN),sample.get_or("y",NAN),sample.get_or("z",NAN)},
                {sample.get_or("nx",0.0f),sample.get_or("ny",0.0f),sample.get_or("nz",0.0f)},
                sample.get_or("terrain",false)})) { complete=false; break; }
        }
        if (!complete) continue;
        pendingPacket=cell.Packet();
        if (!SkyCraftBridgeWriteCollision(skycraft::kcd2::kColSceneCell,
            pendingPacket.data(),static_cast<std::uint32_t>(pendingPacket.size()))) return;
        pendingPacket.clear();
        ++acceptedCells;
        if (acceptedCells%100==1) LOG(INFO) << "SkyCraft: shared scene cells sampled " << acceptedCells;
        if (std::chrono::steady_clock::now()-start>=std::chrono::milliseconds(2)) break;
    }
}
