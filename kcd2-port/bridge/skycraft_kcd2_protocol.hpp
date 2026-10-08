#pragma once
#include <cstdint>

namespace skycraft::kcd2
{
    // Optional additions to the supplied v11 protocol, only used by this port.
    inline constexpr std::uint32_t kRenCollisionShapes = 12;
    inline constexpr std::uint32_t kRenSceneRequest = 13;
    inline constexpr std::uint32_t kRenAttackTrace = 14;
    struct AttackTrace {
        std::uint32_t epoch, target, sourceKind, sourceNativeKey, sourceEntity;
        std::uint32_t flags, weapon;
        float damage;
        float from[3], point[3];
        float pushX, pushZ, pushStrength;
    };
    static_assert(sizeof(AttackTrace) == 68);
    inline constexpr std::uint32_t kColSceneCell = 4;
    inline constexpr std::uint32_t kEvHealActor = 6;
    inline constexpr std::uint32_t kHitMobSource = 1u << 4;
}
