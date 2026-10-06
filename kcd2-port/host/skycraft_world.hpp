#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

struct ID3D12Device;
struct ID3D12DescriptorHeap;
struct ID3D12GraphicsCommandList;

struct SkyCraftSolidBlock
{
    std::int32_t x, y, z;
    std::uint16_t part = 0;
    float minX = 0, minY = 0, minZ = 0;
    float maxX = 1, maxY = 1, maxZ = 1;
    bool operator==(const SkyCraftSolidBlock&) const = default;
};

void SkyCraftWorldAcceptRender(std::uint32_t type, const std::uint8_t* payload,
    std::uint32_t bytes);
void SkyCraftWorldSetCamera(double x, double y, double z, float yaw, float pitch,
    bool valid);
void SkyCraftWorldInitialize(ID3D12Device* device, ID3D12DescriptorHeap* heap,
    std::uint32_t bufferCount);
void SkyCraftWorldShutdown();
void SkyCraftWorldUpload(ID3D12GraphicsCommandList* commands, std::uint32_t bufferIndex);
void SkyCraftWorldDraw();
void SkyCraftWorldCollectNearbySolids(double x, double y, double z, float radius,
    std::size_t limit, std::vector<SkyCraftSolidBlock>& output);
