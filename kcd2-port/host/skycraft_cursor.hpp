#pragma once
#include <algorithm>
#include <cstdint>

namespace skycraft {
    // Camera cursor coordinates belong to the published host viewport. The
    // window client size, render target and output backbuffer may all differ.
    inline float CursorFraction(float coordinate, std::uint32_t sourceExtent) {
        if (!sourceExtent) return 0.0f;
        return std::clamp((coordinate + 0.5f) / static_cast<float>(sourceExtent), 0.0f, 1.0f);
    }
}
