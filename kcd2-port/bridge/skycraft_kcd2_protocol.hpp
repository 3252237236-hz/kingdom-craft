#pragma once
#include <cstdint>

namespace skycraft::kcd2
{
    // Optional additions to the supplied v11 protocol, only used by this port.
    inline constexpr std::uint32_t kRenCollisionShapes = 12;
    inline constexpr std::uint32_t kRenSceneRequest = 13;
    inline constexpr std::uint32_t kRenAttackTrace = 14;
    inline constexpr std::uint32_t kRenAttackProbe = 15;
    inline constexpr std::uint32_t kRenAttackCommit = 16;
    struct AttackTrace {
        std::uint32_t epoch, target, sourceKind, sourceNativeKey, sourceEntity;
        std::uint32_t flags, weapon;
        float damage;
        float from[3], point[3];
        float pushX, pushZ, pushStrength;
    };
    static_assert(sizeof(AttackTrace) == 68);
    struct AttackTransaction { std::uint32_t request; AttackTrace trace; };
    static_assert(sizeof(AttackTransaction) == 72);
    inline constexpr std::uint32_t kColAttackResult = 5;
    enum AttackDecision : std::uint32_t { kAttackClear=1, kAttackWorld=2,
        kAttackEquipment=3, kAttackRejected=4 };
    struct AttackResult {
        std::uint32_t epoch, request, target, decision;
        float distance, point[3], normal[3];
    };
    static_assert(sizeof(AttackResult) == 44);
    inline constexpr std::uint32_t kColSceneCell = 4;
    inline constexpr std::uint32_t kEvHealActor = 6;
    inline constexpr std::uint32_t kHitMobSource = 1u << 4;
}
