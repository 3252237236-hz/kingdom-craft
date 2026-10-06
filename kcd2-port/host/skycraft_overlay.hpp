#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12DescriptorHeap;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;

void SkyCraftOverlayInitialize(ID3D12Device* device, ID3D12DescriptorHeap* heap,
    std::uint32_t bufferCount);
void SkyCraftOverlayShutdown();
bool SkyCraftOverlayBeginFrame(std::uint32_t bufferIndex);
bool SkyCraftOverlayWaitIdle();
void SkyCraftOverlayUpload(ID3D12GraphicsCommandList* commands, std::uint32_t bufferIndex);
void SkyCraftOverlayEndFrame(ID3D12CommandQueue* queue, std::uint32_t bufferIndex);
void SkyCraftOverlaySetBackbufferSize(std::uint32_t width, std::uint32_t height);
void SkyCraftOverlayGetBackbufferSize(std::uint32_t& width, std::uint32_t& height);
void SkyCraftOverlaySetInGame(bool inGame);
void SkyCraftOverlayDraw();
