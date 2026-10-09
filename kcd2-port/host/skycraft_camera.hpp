#pragma once

#include "../../protocol/skycraft_protocol.h"
#include <cmath>
#include <cstdint>

namespace skycraft::camera
{
    // Camera angles are separate from body/attack angles, especially for front F5.
    inline bool ValidVanillaFrame(const proto::McState& mc, std::uint32_t teleport)
    {
        return (mc.flags & proto::kMcInWorld) && !(mc.flags & proto::kMcDead)
            && (mc.flags & proto::kMcCameraFrame) && mc.teleportAck == teleport
            && mc.cameraMode <= 2 && std::isfinite(mc.eyeX) && std::isfinite(mc.eyeY)
            && std::isfinite(mc.eyeZ) && std::abs(mc.eyeX) < 100000
            && std::abs(mc.eyeY) < 100000 && std::abs(mc.eyeZ) < 100000
            && std::isfinite(mc.cameraYaw) && std::isfinite(mc.cameraPitch)
            && std::abs(mc.cameraPitch) <= 90.01f;
    }

    struct NativeAngles { float x, y, z; };
    inline NativeAngles ToNativeAngles(float renderedYaw, float renderedPitch)
    {
        // Cry forward +Y, up +Z; MC forward +Z, up +Y. No world scale factor.
        return {-renderedPitch, 0, 180 - renderedYaw};
    }
}
