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
    inline constexpr std::uint32_t kRenMobSnapshot = 17;
    inline constexpr std::uint32_t kRenMobDamageAck = 18;
    inline constexpr std::uint32_t kRenMobAttackProbe = 19;
    inline constexpr std::uint32_t kRenMobAttackCommit = 20;
    inline constexpr std::uint32_t kRenMobAttackTrace = 21;
    struct MobUuid {
        std::uint64_t most = 0, least = 0;
        bool operator==(const MobUuid&) const = default;
    };
    struct MobAttackIdentity {
        MobUuid session, entity;
        bool operator==(const MobAttackIdentity&) const = default;
    };
    static_assert(sizeof(MobAttackIdentity) == 32);
    // Packed records are decoded with memcpy, never by unaligned pointer casts.
#pragma pack(push, 1)
    struct MobSnapshotHeader {
        std::uint32_t epoch, count;
        std::uint64_t sequence;
        MobUuid session;
    };
    struct MobRecord {
        std::uint32_t entityId;
        MobUuid uuid;
        double x, y, z;
        float yaw, width, height, health, maxHealth;
    };
    struct MobDamageReceipt {
        std::uint32_t epoch, entityId;
        std::uint64_t serial;
        MobUuid session, entity;
        float damage;
        std::uint32_t kind, attackerKey, flags;
        float from[3], point[3];
    };
    struct MobDamageAck {
        std::uint32_t epoch, entityId;
        std::uint64_t serial;
        MobUuid session, entity;
    };
#pragma pack(pop)
    static_assert(sizeof(MobSnapshotHeader) == 32);
    static_assert(sizeof(MobRecord) == 64);
    static_assert(sizeof(MobDamageReceipt) == 88);
    static_assert(sizeof(MobDamageAck) == 48);
    inline constexpr std::uint32_t kColMobDamage = 6;
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
