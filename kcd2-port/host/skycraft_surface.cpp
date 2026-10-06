#include "skycraft_surface.hpp"

#include "../bridge/skycraft_bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>
#include <logger/logger.hpp>

namespace
{
    using namespace skycraft::proto;

    struct Vector
    {
        float x, y, z;
    };

    struct Region
    {
        int x, y, z;
    };

    struct GroundMessage
    {
        ColRegion region;
        ColBlock blocks[4];
    };

    struct GroundColumn
    {
        int baseY;
        int topLayers;
        float height;
    };

    Region previous{};
    bool hasPrevious = false;
    bool loggedHit = false;
    std::uint32_t activeEpoch = 0;
    std::map<std::pair<int, int>, GroundColumn> groundColumns;
    std::array<ColTri, 2> aimTriangles{};
    std::set<std::tuple<int, int, int>> dirtyTriangleRegions;

    Vector Add(Vector a, Vector b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    Vector Subtract(Vector a, Vector b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    Vector Multiply(Vector a, float scale) { return {a.x * scale, a.y * scale, a.z * scale}; }
    Vector Cross(Vector a, Vector b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
    }
    Vector Normalize(Vector a)
    {
        const float length = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
        return length > 0.001f ? Multiply(a, 1.0f / length) : Vector{0, 0, 0};
    }

    ColRegion MakeRegionHeader(Region region, std::uint32_t epoch,
        std::uint32_t count)
    {
        return {region.x, region.y, region.z, region.x + 7, region.y + 7,
            region.z + 7, epoch, count};
    }

    void SetVertex(ColTri& triangle, int index, Vector value)
    {
        triangle.v[index * 3] = value.x;
        triangle.v[index * 3 + 1] = value.y;
        triangle.v[index * 3 + 2] = value.z;
    }

    Region GroundTriangleRegion(int x, int z, float height)
    {
        return {int(std::floor(x / 8.0f)) * 8,
            int(std::floor(height / 8.0f)) * 8,
            int(std::floor(z / 8.0f)) * 8};
    }

    bool SameRegion(Region a, Region b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    void DirtyTriangles(Region region)
    {
        dirtyTriangleRegions.emplace(region.x, region.y, region.z);
    }

    bool PublishTriangles(Region region, std::uint32_t epoch)
    {
        std::vector<ColTri> triangles;
        for (const auto& [position, column] : groundColumns) {
            const auto [x, z] = position;
            if (!SameRegion(GroundTriangleRegion(x, z, column.height), region)) {
                continue;
            }
            const Vector a{float(x), column.height, float(z)};
            const Vector b{float(x), column.height, float(z + 1)};
            const Vector c{float(x + 1), column.height, float(z + 1)};
            const Vector d{float(x + 1), column.height, float(z)};
            ColTri first{}, second{};
            SetVertex(first, 0, a);
            SetVertex(first, 1, b);
            SetVertex(first, 2, c);
            SetVertex(second, 0, a);
            SetVertex(second, 1, c);
            SetVertex(second, 2, d);
            triangles.push_back(first);
            triangles.push_back(second);
        }
        if (hasPrevious && SameRegion(previous, region)) {
            triangles.insert(triangles.end(), aimTriangles.begin(), aimTriangles.end());
        }
        const auto header = MakeRegionHeader(region, epoch,
            static_cast<std::uint32_t>(triangles.size()));
        std::vector<std::uint8_t> message(sizeof(header) + triangles.size() * sizeof(ColTri));
        std::memcpy(message.data(), &header, sizeof(header));
        if (!triangles.empty()) {
            std::memcpy(message.data() + sizeof(header), triangles.data(),
                triangles.size() * sizeof(ColTri));
        }
        return SkyCraftBridgeWriteCollision(kColTris, message.data(),
            static_cast<std::uint32_t>(message.size()));
    }

    bool FlushTriangles(std::uint32_t epoch)
    {
        while (!dirtyTriangleRegions.empty()) {
            const auto [x, y, z] = *dirtyTriangleRegions.begin();
            if (!PublishTriangles({x, y, z}, epoch)) {
                return false;
            }
            dirtyTriangleRegions.erase(dirtyTriangleRegions.begin());
        }
        return true;
    }

    bool ClearPrevious(std::uint32_t epoch)
    {
        if (!hasPrevious) {
            return FlushTriangles(epoch);
        }
        DirtyTriangles(previous);
        hasPrevious = false;
        return FlushTriangles(epoch);
    }

    bool EnsureEpoch(std::uint32_t epoch)
    {
        if (activeEpoch == epoch) {
            return true;
        }
        if (!SkyCraftBridgeWriteCollision(kColClear, &epoch, sizeof(epoch))) {
            return false;
        }
        activeEpoch = epoch;
        hasPrevious = false;
        groundColumns.clear();
        dirtyTriangleRegions.clear();
        return true;
    }

    ColRegion GroundRegion(int x, int z, int baseY, std::uint32_t epoch,
        std::uint32_t count)
    {
        return {x, baseY - 8, z, x, baseY + 2, z, epoch, count};
    }

    bool ClearGround(int x, int z, GroundColumn column, std::uint32_t epoch)
    {
        const auto region = GroundRegion(x, z, column.baseY, epoch, 0);
        return SkyCraftBridgeWriteCollision(kColRegion, &region, sizeof(region));
    }

    bool PublishGround(int x, int z, GroundColumn column,
        std::uint32_t epoch)
    {
        GroundMessage message{};
        const auto count = 3 + (column.topLayers > 0 ? 1 : 0);
        message.region = GroundRegion(x, z, column.baseY, epoch, count);
        for (int i = 0; i < 3; ++i) {
            auto& block = message.blocks[i];
            block.x = x;
            block.y = column.baseY - 3 + i;
            block.z = z;
            std::fill(std::begin(block.bits), std::end(block.bits), ~0ull);
        }
        if (column.topLayers > 0) {
            auto& block = message.blocks[3];
            block.x = x;
            block.y = column.baseY;
            block.z = z;
            std::fill_n(block.bits, column.topLayers, ~0ull);
        }
        return SkyCraftBridgeWriteCollision(kColRegion, &message,
            sizeof(ColRegion) + count * sizeof(ColBlock));
    }
}

void SkyCraftSurfaceClear(std::uint32_t epoch)
{
    if (!SkyCraftBridgeMcAlive()) {
        hasPrevious = false;
        groundColumns.clear();
        dirtyTriangleRegions.clear();
        activeEpoch = 0;
    } else if (EnsureEpoch(epoch)) {
        ClearPrevious(epoch);
    }
}

void SkyCraftSurfaceReset(std::uint32_t epoch)
{
    if (!SkyCraftBridgeMcAlive()) {
        hasPrevious = false;
        groundColumns.clear();
        dirtyTriangleRegions.clear();
        activeEpoch = 0;
    } else if (hasPrevious || !groundColumns.empty()) {
        if (SkyCraftBridgeWriteCollision(kColClear, &epoch, sizeof(epoch))) {
            hasPrevious = false;
            groundColumns.clear();
            dirtyTriangleRegions.clear();
            activeEpoch = epoch;
        }
    }
}

void SkyCraftSurfaceTick(const SkyCraftSurfaceHit& hit, std::uint32_t epoch)
{
    if (!SkyCraftBridgeMcAlive()) {
        hasPrevious = false;
        return;
    }
    if (!EnsureEpoch(epoch)) {
        return;
    }
    if (!std::isfinite(hit.x) || !std::isfinite(hit.y) || !std::isfinite(hit.z)
        || !std::isfinite(hit.nx) || !std::isfinite(hit.ny)
        || !std::isfinite(hit.nz)) {
        ClearPrevious(epoch);
        return;
    }

    const Vector center{hit.x, hit.z, -hit.y};
    const Vector normal = Normalize({hit.nx, hit.nz, -hit.ny});
    if (normal.x == 0 && normal.y == 0 && normal.z == 0) {
        ClearPrevious(epoch);
        return;
    }
    const Vector axis = std::abs(normal.y) < 0.9f ? Vector{0, 1, 0}
        : Vector{1, 0, 0};
    const Vector across = Multiply(Normalize(Cross(axis, normal)), 0.85f);
    const Vector along = Multiply(Normalize(Cross(normal, across)), 0.85f);
    const Vector a = Subtract(Subtract(center, across), along);
    const Vector b = Add(Subtract(center, along), across);
    const Vector c = Add(Add(center, across), along);
    const Vector d = Add(Subtract(center, across), along);

    const Region region{
        int(std::floor(center.x / 8.0f)) * 8,
        int(std::floor(center.y / 8.0f)) * 8,
        int(std::floor(center.z / 8.0f)) * 8
    };
    if (hasPrevious && (region.x != previous.x || region.y != previous.y
        || region.z != previous.z) && !ClearPrevious(epoch)) {
        return;
    }
    SetVertex(aimTriangles[0], 0, a);
    SetVertex(aimTriangles[0], 1, b);
    SetVertex(aimTriangles[0], 2, c);
    SetVertex(aimTriangles[1], 0, a);
    SetVertex(aimTriangles[1], 1, c);
    SetVertex(aimTriangles[1], 2, d);
    const std::uint32_t flags = hit.terrain
        ? kTriTerrain | kTriDiggable | (std::uint32_t(kDigGrass) << 8)
        : 0;
    aimTriangles[0].flags = flags;
    aimTriangles[1].flags = flags;
    previous = region;
    hasPrevious = true;
    DirtyTriangles(region);
    if (FlushTriangles(epoch)) {
        if (!loggedHit) {
            loggedHit = true;
            LOG(INFO) << "SkyCraft: KCD2 aim surface streamed at "
                      << center.x << ", " << center.y << ", " << center.z
                      << " (terrain " << hit.terrain << ")";
        }
    }
}

void SkyCraftGroundSample(std::int32_t x, std::int32_t z, float height,
    bool found, std::uint32_t epoch)
{
    if (!SkyCraftBridgeMcAlive() || !EnsureEpoch(epoch)) {
        return;
    }
    const auto key = std::pair{x, z};
    auto old = groundColumns.find(key);
    if (!found || !std::isfinite(height) || std::abs(height) > 100000.0f) {
        if (old != groundColumns.end()
            && ClearGround(x, z, old->second, epoch)) {
            DirtyTriangles(GroundTriangleRegion(x, z, old->second.height));
            groundColumns.erase(old);
        }
        return;
    }
    const int baseY = int(std::floor(height));
    const int topLayers = std::clamp(int(std::ceil((height - baseY) * 8.0f
        - 0.0001f)), 0, 8);
    const GroundColumn column{baseY, topLayers, height};
    if (old != groundColumns.end()
        && old->second.baseY == baseY
        && old->second.topLayers == topLayers
        && std::abs(old->second.height - height) < 0.005f) {
        return;
    }
    if (old != groundColumns.end()) {
        if (!ClearGround(x, z, old->second, epoch)) {
            return;
        }
        DirtyTriangles(GroundTriangleRegion(x, z, old->second.height));
        groundColumns.erase(old);
    }
    if (PublishGround(x, z, column, epoch)) {
        groundColumns.emplace(key, column);
        DirtyTriangles(GroundTriangleRegion(x, z, column.height));
    }
}

void SkyCraftGroundPrune(float playerX, float playerZ, std::uint32_t epoch)
{
    if (!SkyCraftBridgeMcAlive() || !EnsureEpoch(epoch)) {
        return;
    }
    int removed = 0;
    for (auto it = groundColumns.begin(); it != groundColumns.end()
        && removed < 12;) {
        const float dx = it->first.first + 0.5f - playerX;
        const float dz = it->first.second + 0.5f - playerZ;
        if (dx * dx + dz * dz <= 144.0f) {
            ++it;
            continue;
        }
        if (!ClearGround(it->first.first, it->first.second,
            it->second, epoch)) {
            break;
        }
        DirtyTriangles(GroundTriangleRegion(it->first.first, it->first.second,
            it->second.height));
        it = groundColumns.erase(it);
        ++removed;
    }
    FlushTriangles(epoch);
}
