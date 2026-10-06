#include "../host/skycraft_surface.hpp"
#include "../bridge/skycraft_bridge.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

using namespace skycraft::proto;

struct Message
{
    std::uint32_t type;
    std::vector<std::uint8_t> bytes;
};
std::vector<Message> messages;
bool failTriangleWrite = false;

extern "C" bool SkyCraftBridgeMcAlive() { return true; }
extern "C" bool SkyCraftBridgeWriteCollision(std::uint32_t type,
    const void* payload, std::uint32_t bytes)
{
    if (type == kColTris && failTriangleWrite) {
        failTriangleWrite = false;
        return false;
    }
    const auto* first = static_cast<const std::uint8_t*>(payload);
    messages.push_back({type, {first, first + bytes}});
    return true;
}

const Message& Latest(std::uint32_t type)
{
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->type == type) return *it;
    }
    assert(false);
    return messages.back();
}

ColRegion RegionHeader(const Message& message)
{
    ColRegion header{};
    assert(message.bytes.size() >= sizeof(header));
    std::memcpy(&header, message.bytes.data(), sizeof(header));
    return header;
}

int main()
{
    SkyCraftGroundSample(1, 2, 10.375f, true, 1);
    const auto ground = RegionHeader(Latest(kColRegion));
    assert(ground.minY <= 2 && ground.maxY >= 10);
    SkyCraftGroundPrune(1.5f, 2.5f, 1);
    const auto& first = Latest(kColTris);
    assert(RegionHeader(first).count == 2);
    ColTri triangle{};
    std::memcpy(&triangle, first.bytes.data() + sizeof(ColRegion), sizeof(triangle));
    assert(std::abs(triangle.v[1] - 10.375f) < 0.001f);
    const float upward = (triangle.v[5] - triangle.v[2]) * (triangle.v[6] - triangle.v[0])
        - (triangle.v[3] - triangle.v[0]) * (triangle.v[8] - triangle.v[2]);
    assert(upward > 0);

    SkyCraftGroundSample(2, 2, 10.5f, true, 1);
    SkyCraftGroundPrune(1.5f, 2.5f, 1);
    assert(RegionHeader(Latest(kColTris)).count == 4);
    SkyCraftSurfaceTick({1.5f, -2.5f, 10.375f, 0, 0, 1, false}, 1);
    assert(RegionHeader(Latest(kColTris)).count == 6);
    SkyCraftSurfaceClear(1);
    assert(RegionHeader(Latest(kColTris)).count == 4);

    SkyCraftGroundSample(1, 2, 0, false, 1);
    failTriangleWrite = true;
    SkyCraftGroundPrune(1.5f, 2.5f, 1);
    assert(RegionHeader(Latest(kColTris)).count == 4);
    SkyCraftGroundPrune(1.5f, 2.5f, 1);
    assert(RegionHeader(Latest(kColTris)).count == 2);

    SkyCraftGroundSample(-1, -1, 10, true, 2);
    SkyCraftGroundPrune(-0.5f, -0.5f, 2);
    const auto reset = RegionHeader(Latest(kColTris));
    assert(reset.epoch == 2 && reset.minX == -8 && reset.minZ == -8);
    assert(reset.count == 2);
    SkyCraftGroundPrune(100, 100, 2);
    assert(RegionHeader(Latest(kColTris)).count == 0);
    std::cout << "surface collision contracts passed\n";
}
