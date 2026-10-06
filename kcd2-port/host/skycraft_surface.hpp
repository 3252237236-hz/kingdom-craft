#pragma once

#include <cstdint>

struct SkyCraftSurfaceHit
{
    float x, y, z;
    float nx, ny, nz;
    bool terrain;
};

void SkyCraftSurfaceTick(const SkyCraftSurfaceHit& hit, std::uint32_t epoch);
void SkyCraftSurfaceClear(std::uint32_t epoch);
void SkyCraftSurfaceReset(std::uint32_t epoch);
void SkyCraftGroundSample(std::int32_t x, std::int32_t z, float height,
    bool found, std::uint32_t epoch);
void SkyCraftGroundPrune(float playerX, float playerZ, std::uint32_t epoch);
