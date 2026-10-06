#pragma once
#include <algorithm>
#include <cmath>

namespace skycraft::camera
{
    struct ClipPlanes { float nearPlane = 0.1f, farPlane = 2000.0f; bool valid = false; };

    // Sample the renderer through System.ProjectToScreen, rather than relying on
    // CCamera offsets from a different build. Perspective depth is A + B / z.
    inline ClipPlanes DecodeDepthSamples(double z1, double d1, double z2, double d2)
    {
        if (!std::isfinite(z1) || !std::isfinite(z2) || z1 <= 0 || z2 <= z1
            || !std::isfinite(d1) || !std::isfinite(d2)
            || d1 < 0 || d1 > 1 || d2 < 0 || d2 > 1) return {};
        const double b = (d1 - d2) / (1.0 / z1 - 1.0 / z2);
        const double a = d1 - b / z1;
        if (std::abs(b) < 1e-6) return {};
        const auto endpoint = [&](double d) {
            return std::abs(d - a) < 1e-8 ? 1e8 : b / (d - a);
        };
        const double n = std::min(endpoint(0), endpoint(1));
        const double f = std::max(endpoint(0), endpoint(1));
        if (std::isfinite(n) && std::isfinite(f) && n >= 0.001 && n < z1
            && f > z2 && f <= 1e8) return {float(n), float(f), true};
        return {};
    }

    // D3D perspective depth: d = (A*z+B)/(C*z). Accept either matrix storage
    // order. Decode distances only: CryEngine may reverse its GPU depth later.
    inline ClipPlanes DecodeClipPlanes(const float* p)
    {
        if (!p || !std::isfinite(p[10]) || !std::isfinite(p[15])
            || std::abs(p[15]) > 0.0001f) return {};
        for (const int cIndex : {11, 14}) {
            const double c = p[cIndex], b = p[cIndex == 11 ? 14 : 11];
            if (!std::isfinite(c) || !std::isfinite(b)
                || std::abs(std::abs(c) - 1.0) > 0.0001 || std::abs(b) < 1e-8) continue;
            const auto endpoint = [&](double d) {
                const double denominator = d * c - p[10];
                return std::abs(denominator) < 1e-8 ? 1e8 : std::abs(b / denominator);
            };
            const double n = std::min(endpoint(0), endpoint(1));
            const double f = std::max(endpoint(0), endpoint(1));
            if (std::isfinite(n) && std::isfinite(f) && n >= 0.001 && n <= 20
                && f >= n * 2 && f <= 1e8) return {float(n), float(f), true};
        }
        return {};
    }
}
