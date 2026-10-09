#include "skycraft_mobs.hpp"
#include "skycraft_mob_packets.hpp"
#include "../bridge/skycraft_bridge.h"
#include <algorithm>
#include <deque>
#include <optional>
#include <cstdio>
#include <string>
#include <windows.h>
#include <logger/logger.hpp>
#include <sol/sol.hpp>

namespace {
    using namespace skycraft::kcd2;
    std::optional<MobSnapshot> pendingSnapshot;
    std::deque<MobDamageAck> acknowledgements;
    sol::protected_function command;
    bool prepared=false;
    MobUuid activeSession{};
    std::uint32_t activeEpoch=0;
    std::uint64_t lastSequence=0,lastReport=0,lastLoadAttempt=0;

    bool PrepareMobs(sol::state_view& lua) {
        if (prepared) return command.valid();
        prepared=true;
        auto result=lua.safe_script(R"lua(
            return function(action, epoch, session, rows, serial, id, uuid)
                if action~="reset" and not SkyCraftNativeMobs and Script and Script.ReloadScript then
                    pcall(Script.ReloadScript, "Scripts/Mods/kingdomcraft_mobs/NativeMobs.lua")
                end
                local mobs=SkyCraftNativeMobs
                if not mobs then return {status="native mob profile unavailable"} end
                if action=="reset" then mobs.reset();return {status="reset"} end
                if action=="sync" then return mobs.sync(epoch,session,rows) end
                if action=="reports" then return mobs.damageReports() end
                if action=="ack" then return mobs.acknowledge(epoch,session,serial,id,uuid) end
            end
        )lua",sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error error=result;
            LOG(WARNING)<<"SkyCraft: native mob controller binding failed: "<<error.what();
            return false;
        }
        command=result.get<sol::protected_function>();
        return command.valid();
    }
    bool uuidFromText(const std::string& text,MobUuid& out) {
        if (text.size()!=32) return false;
        out={};
        for (std::size_t i=0;i<32;++i) {
            const auto c=text[i];
            const int value=c>='0'&&c<='9' ? c-'0' : c>='a'&&c<='f' ? c-'a'+10 : -1;
            if (value<0) return false;
            auto& word=i<16 ? out.most : out.least;
            word=(word<<4)|static_cast<std::uint64_t>(value);
        }
        return present(out);
    }
}

std::string SkyCraftMobUuidText(const skycraft::kcd2::MobUuid& uuid) {
    char buffer[33]{};
    std::snprintf(buffer,sizeof(buffer),"%016llx%016llx",
        static_cast<unsigned long long>(uuid.most),static_cast<unsigned long long>(uuid.least));
    return buffer;
}

bool SkyCraftMobsAccept(std::uint32_t type,const std::uint8_t* data,std::uint32_t bytes) {
    if (type==skycraft::kcd2::kRenMobSnapshot) {
        MobSnapshot snapshot;
        if (decodeMobSnapshot(data,bytes,snapshot)) {
            if (pendingSnapshot && snapshot.header.session==pendingSnapshot->header.session
                && snapshot.header.epoch==pendingSnapshot->header.epoch
                && snapshot.header.sequence<=pendingSnapshot->header.sequence) return true;
            pendingSnapshot=std::move(snapshot);
        }
        return true;
    }
    if (type==skycraft::kcd2::kRenMobDamageAck) {
        if (!data || bytes!=sizeof(MobDamageAck) || acknowledgements.size()>=512) return true;
        MobDamageAck ack{};std::memcpy(&ack,data,sizeof(ack));
        if (ack.entityId && ack.entityId<=0x7fffffffu && ack.serial && ack.serial<=0x1fffffffffffffull
            && present(ack.session) && present(ack.entity)) acknowledgements.push_back(ack);
        return true;
    }
    return false;
}

void SkyCraftMobsReset(lua_State* state) {
    pendingSnapshot.reset();acknowledgements.clear();
    activeSession={};lastSequence=0;lastLoadAttempt=0;
    if (state && command.valid()) command("reset",0,"");
}

void SkyCraftMobsTick(lua_State* state,std::uint32_t epoch) {
    sol::state_view lua(state);
    if (!PrepareMobs(lua)) return;
    const auto now=::GetTickCount64();
    if (!lua["SkyCraftNativeMobs"].valid() && now-lastLoadAttempt<5000) return;
    lastLoadAttempt=now;
    if (pendingSnapshot) {
        auto snapshot=std::move(*pendingSnapshot);pendingSnapshot.reset();
        if (snapshot.header.epoch==epoch && (snapshot.header.session!=activeSession
            || activeEpoch!=epoch || snapshot.header.sequence>lastSequence)) {
            auto rows=lua.create_table(static_cast<int>(snapshot.rows.size()),0);
            int index=0;
            for (const auto& r:snapshot.rows) rows[++index]=lua.create_table_with(
                "entityId",r.entityId,"uuid",SkyCraftMobUuidText(r.uuid),
                "x",r.x,"y",r.y,"z",r.z,"yaw",r.yaw,"width",r.width,"height",r.height,
                "health",r.health,"maxHealth",r.maxHealth);
            auto result=command("sync",epoch,SkyCraftMobUuidText(snapshot.header.session),rows);
            if (result.valid() && result.get<sol::object>().is<sol::table>()) {
                auto response=result.get<sol::table>();
                if (response.get_or("status",std::string{})=="ok") {
                    activeSession=snapshot.header.session;activeEpoch=epoch;lastSequence=snapshot.header.sequence;
                }
                if (now-lastReport>=5000) {
                    lastReport=now;
                    LOG(INFO)<<"SkyCraft: native mob sync "<<response.get_or("status",std::string{})
                        <<" live "<<response.get_or("live",0)<<" spawned "<<response.get_or("spawned",0)
                        <<" failed "<<response.get_or("failed",0)<<" "<<response.get_or("error",std::string{});
                }
            }
        }
    }
    while (!acknowledgements.empty()) {
        const auto ack=acknowledgements.front();acknowledgements.pop_front();
        if (ack.epoch!=epoch || ack.session!=activeSession) continue;
        command("ack",ack.epoch,SkyCraftMobUuidText(ack.session),sol::nil,ack.serial,
            ack.entityId,SkyCraftMobUuidText(ack.entity));
    }
    if (!present(activeSession)) return;
    auto result=command("reports",epoch,SkyCraftMobUuidText(activeSession));
    if (!result.valid() || !result.get<sol::object>().is<sol::table>()) return;
    auto reports=result.get<sol::table>();
    for (int index=1;index<=std::min(64,static_cast<int>(reports.size()));++index) {
        const auto maybe=reports.get<sol::optional<sol::table>>(index);
        if (!maybe) continue;
        const auto row=*maybe;
        MobDamageReceipt receipt{};
        receipt.epoch=row.get_or("epoch",0u);receipt.entityId=row.get_or("entityId",0u);
        const double serial=row.get_or("serial",0.0);
        if (!std::isfinite(serial) || serial<=0 || serial>9007199254740991.0 || serial!=std::floor(serial)) continue;
        receipt.serial=static_cast<std::uint64_t>(serial);
        receipt.damage=row.get_or("damage",0.0f);
        receipt.flags=1; // sampled HP loss; exact native hit event not yet captured
        if (receipt.epoch!=epoch || !receipt.entityId || !std::isfinite(receipt.damage)
            || receipt.damage<=0 || receipt.damage>1e6f
            || !uuidFromText(row.get_or("session",std::string{}),receipt.session)
            || !uuidFromText(row.get_or("uuid",std::string{}),receipt.entity)
            || receipt.session!=activeSession) continue;
        if (!SkyCraftBridgeWriteCollision(kColMobDamage,&receipt,sizeof(receipt))) break;
    }
}
