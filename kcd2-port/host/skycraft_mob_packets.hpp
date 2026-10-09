#pragma once
#include "../bridge/skycraft_kcd2_protocol.hpp"
#include <cmath>
#include <cstring>
#include <set>
#include <vector>

namespace skycraft::kcd2 {
    inline bool present(const MobUuid& uuid) { return uuid.most || uuid.least; }
    struct MobSnapshot { MobSnapshotHeader header{}; std::vector<MobRecord> rows; };
    inline bool decodeMobSnapshot(const std::uint8_t* data, std::uint32_t bytes, MobSnapshot& out) {
        if (!data || bytes<sizeof(MobSnapshotHeader)) return false;
        MobSnapshot next;
        std::memcpy(&next.header,data,sizeof(next.header));
        const auto& h=next.header;
        if (h.count>64 || !h.sequence || h.sequence>0x1fffffffffffffull || !present(h.session)
            || bytes!=sizeof(h)+h.count*sizeof(MobRecord)) return false;
        std::set<std::uint32_t> ids;
        std::set<std::pair<std::uint64_t,std::uint64_t>> uuids;
        next.rows.resize(h.count);
        for (std::size_t i=0;i<h.count;++i) {
            auto& r=next.rows[i];
            std::memcpy(&r,data+sizeof(h)+i*sizeof(r),sizeof(r));
            if (!r.entityId || r.entityId>0x7fffffffu || !present(r.uuid)
                || !ids.insert(r.entityId).second || !uuids.emplace(r.uuid.most,r.uuid.least).second
                || !std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.z)
                || std::abs(r.x)>3e7 || std::abs(r.y)>3e7 || std::abs(r.z)>3e7
                || !std::isfinite(r.yaw) || std::abs(r.yaw)>1e7
                || !std::isfinite(r.width) || r.width<.05f || r.width>16
                || !std::isfinite(r.height) || r.height<.05f || r.height>16
                || !std::isfinite(r.health) || !std::isfinite(r.maxHealth)
                || r.health<=0 || r.health>r.maxHealth || r.maxHealth>1e6f) return false;
        }
        out=std::move(next);
        return true;
    }
}
