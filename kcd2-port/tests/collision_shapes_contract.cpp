#include "../host/skycraft_collision_shapes.hpp"
#include <cassert>
#include <iostream>

int main()
{
    using namespace skycraft::kcd2;
    ShapeHeader header{-1, 2, -3, 1, 0}, parsed{};
    ShapeBox slab{0x0321, 7, 0, 0, 0, 1, 0.5f, 1};
    std::vector<std::uint8_t> payload(sizeof(header) + sizeof(slab));
    auto pack = [&] {
        std::memcpy(payload.data(), &header, sizeof(header));
        std::memcpy(payload.data() + sizeof(header), &slab, sizeof(slab));
    };
    std::vector<ShapeBox> boxes;
    pack();
    assert(DecodeShapes(payload.data(), payload.size(), parsed, boxes));
    assert(parsed.sx == -1 && parsed.sz == -3 && boxes.size() == 1);
    assert(boxes[0].cell == 0x0321 && boxes[0].part == 7 && boxes[0].maxY == 0.5f);
    // A failed update preserves the previous usable section.
    slab.maxY = std::numeric_limits<float>::quiet_NaN(); pack();
    assert(!DecodeShapes(payload.data(), payload.size(), parsed, boxes));
    assert(boxes[0].maxY == 0.5f);
    slab.maxY = 1.5f; pack();  // fence extends outside its source cell
    assert(DecodeShapes(payload.data(), payload.size(), parsed, boxes));
    slab.maxY = slab.minY; pack();
    assert(!DecodeShapes(payload.data(), payload.size(), parsed, boxes));
    slab.maxY = 1.5f; slab.cell = 4096; pack();
    assert(!DecodeShapes(payload.data(), payload.size(), parsed, boxes));
    slab.cell = 0; header.sx = std::numeric_limits<int>::max(); pack();
    assert(!DecodeShapes(payload.data(), payload.size(), parsed, boxes));
    header.sx = 1; header.count = 2; pack();
    payload.resize(sizeof(header) + 2 * sizeof(slab));
    std::memcpy(payload.data() + sizeof(header) + sizeof(slab), &slab, sizeof(slab));
    assert(!DecodeShapes(payload.data(), payload.size(), parsed, boxes)); // duplicate part
    header.count = 0; header.flags = kShapeFallback;
    payload.resize(sizeof(header));
    std::memcpy(payload.data(), &header, sizeof(header));
    assert(DecodeShapes(payload.data(), payload.size(), parsed, boxes));
    assert(parsed.flags == kShapeFallback && boxes.empty());
    header.flags = 0; std::memcpy(payload.data(), &header, sizeof(header));
    assert(DecodeShapes(payload.data(), payload.size(), parsed, boxes)); // authoritative empty
    assert(!DecodeShapes(payload.data(), payload.size() - 1, parsed, boxes));
    assert(!DecodeShapes(nullptr, 0, parsed, boxes));
    std::cout << "collision shape wire contracts passed\n";
}
