#pragma once
#include "../bridge/skycraft_kcd2_protocol.hpp"
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace skycraft::kcd2
{
    // Engine-thread ownership: replies approve a specific segment once, while
    // vanilla fills in effective damage, critical state and knockback afterward.
    class AttackApprovals
    {
        struct Entry { AttackTrace trace; std::uint64_t issued; MobAttackIdentity identity; };
        std::unordered_map<std::uint32_t,Entry> entries;
        static constexpr std::uint64_t lifetime=1500;
        static bool current(std::uint64_t issued,std::uint64_t now) {
            return now>=issued && now-issued<=lifetime;
        }
        static bool matches(const AttackTrace& a,const AttackTrace& b) {
            return a.epoch==b.epoch && a.target==b.target && a.sourceKind==b.sourceKind
                && a.sourceNativeKey==b.sourceNativeKey && a.sourceEntity==b.sourceEntity
                && a.weapon==b.weapon && std::memcmp(a.from,b.from,sizeof(a.from))==0
                && std::memcmp(a.point,b.point,sizeof(a.point))==0;
        }
    public:
        void clear() { entries.clear(); }
        void expire(std::uint64_t now) {
            for (auto it=entries.begin();it!=entries.end();) {
                if (!current(it->second.issued,now)) it=entries.erase(it);
                else ++it;
            }
        }
        bool approve(std::uint32_t request,const AttackTrace& trace,std::uint64_t now,
            const MobAttackIdentity& identity={}) {
            if (!request || entries.size()>=512) return false;
            return entries.emplace(request,Entry{trace,now,identity}).second;
        }
        bool consume(std::uint32_t request,const AttackTrace& trace,std::uint64_t now,
            const MobAttackIdentity& identity={}) {
            const auto it=entries.find(request);
            if (it==entries.end()) return false;
            const bool accepted=current(it->second.issued,now) && matches(it->second.trace,trace)
                && it->second.identity==identity;
            entries.erase(it);
            return accepted;
        }
    };
}
