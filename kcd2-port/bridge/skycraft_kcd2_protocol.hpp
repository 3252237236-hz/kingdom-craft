#pragma once
#include <cstdint>

namespace skycraft::kcd2
{
    // Optional additions to the supplied v11 protocol, only used by this port.
    inline constexpr std::uint32_t kRenCollisionShapes = 12;
    inline constexpr std::uint32_t kEvHealActor = 6;
    inline constexpr std::uint32_t kHitMobSource = 1u << 4;
}
