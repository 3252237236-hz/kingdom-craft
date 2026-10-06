#include "../host/skycraft_projection.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

int main()
{
    using skycraft::camera::DecodeClipPlanes;
    for (bool reversed : {false, true}) {
        for (bool transposed : {false, true}) {
            float p[16] = {};
            const double n = 0.15, f = 1500;
            p[10] = float(reversed ? n/(f-n) : -f/(f-n));
            p[transposed ? 14 : 11] = -1;
            p[transposed ? 11 : 14] = float(reversed ? n*f/(f-n) : -n*f/(f-n));
            auto planes = DecodeClipPlanes(p);
            assert(planes.valid && std::abs(planes.nearPlane - n) < 1e-5);
            assert(std::abs(planes.farPlane - f) / f < 0.002);
        }
    }
    float p[16] = {};
    p[11] = -1; p[14] = 0.05f; // infinite reversed depth
    assert(DecodeClipPlanes(p).valid && DecodeClipPlanes(p).farPlane == 1e8f);
    p[10] = -1; p[14] = -0.05f; // infinite conventional depth
    assert(DecodeClipPlanes(p).valid && DecodeClipPlanes(p).farPlane == 1e8f);
    p[15] = 1; assert(!DecodeClipPlanes(p).valid);
    p[15] = 0; p[10] = NAN; assert(!DecodeClipPlanes(p).valid);
    assert(!DecodeClipPlanes(nullptr).valid);
    float zero[16] = {}; assert(!DecodeClipPlanes(zero).valid);
    using skycraft::camera::DecodeDepthSamples;
    for (bool reversed : {false, true}) {
        const double n = 0.15, f = 2000;
        const auto depth = [&](double z) {
            return reversed ? (n*f/z-n)/(f-n) : (f-n*f/z)/(f-n);
        };
        auto planes = DecodeDepthSamples(1, float(depth(1)), 8, float(depth(8)));
        assert(planes.valid && std::abs(planes.nearPlane - n) < 1e-5);
        assert(std::abs(planes.farPlane - f) / f < 0.01);
    }
    assert(!DecodeDepthSamples(1, 0, 8, 0).valid);
    assert(!DecodeDepthSamples(1, 1.4013e-45, 8, 4.23297e21).valid);
    assert(!DecodeDepthSamples(1, NAN, 8, 0.1).valid);
    assert(!DecodeDepthSamples(1, -1, 8, 0.1).valid);
    std::cout << "projection depth contracts passed\n";
}
