#pragma once
#include "../bridge/skycraft_kcd2_protocol.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace skycraft::kcd2
{
    // Optional KCD2 render-ring extension. Original v11 readers ignore type 12;
    // REN_SOLIDS remains available for clients without this extension.
    inline constexpr std::uint32_t kMaxSectionBoxes = 65536;
    inline constexpr std::uint32_t kShapeFallback = 1;
    struct ShapeHeader {
        std::int32_t sx, sy, sz;
        std::uint32_t count, flags;
    };
    struct ShapeBox {
        std::uint16_t cell, part;
        float minX, minY, minZ, maxX, maxY, maxZ;
    };
    static_assert(sizeof(ShapeHeader) == 20);
    static_assert(sizeof(ShapeBox) == 28);

    inline bool DecodeShapes(const std::uint8_t* data, std::uint32_t bytes,
        ShapeHeader& header, std::vector<ShapeBox>& boxes)
    {
        if (!data || bytes < sizeof(ShapeHeader)) return false;
        ShapeHeader candidate{};
        std::memcpy(&candidate, data, sizeof(candidate));
        const auto expected = sizeof(ShapeHeader)
            + std::uint64_t(candidate.count) * sizeof(ShapeBox);
        if (candidate.count > kMaxSectionBoxes || bytes != expected
            || candidate.flags > kShapeFallback
            || (candidate.flags == kShapeFallback && candidate.count)) return false;
        for (const auto coord : {candidate.sx, candidate.sy, candidate.sz}) {
            if (coord < std::numeric_limits<std::int32_t>::min() / 16
                || coord > (std::numeric_limits<std::int32_t>::max() - 15) / 16)
                return false;
        }
        std::vector<ShapeBox> decoded(candidate.count);
        if (candidate.count) std::memcpy(decoded.data(), data + sizeof(candidate),
            candidate.count * sizeof(ShapeBox));
        std::set<std::pair<std::uint16_t, std::uint16_t>> keys;
        for (const auto& box : decoded) {
            if (box.cell >= 4096 || !keys.emplace(box.cell, box.part).second)
                return false;
            const float low[]{box.minX, box.minY, box.minZ};
            const float high[]{box.maxX, box.maxY, box.maxZ};
            for (int axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(low[axis]) || !std::isfinite(high[axis])
                    || low[axis] < -1 || high[axis] > 2
                    || high[axis] <= low[axis]) return false;
            }
        }
        header = candidate;
        boxes = std::move(decoded);
        return true;
    }
}
