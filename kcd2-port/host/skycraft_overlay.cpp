#include "skycraft_overlay.hpp"
#include "skycraft_host.hpp"

#include "../bridge/skycraft_bridge.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>
#include <windows.h>
#include <d3d12.h>
#include <imgui.h>
#include <logger/logger.hpp>

namespace
{
    struct UploadBuffer
    {
        ID3D12Resource* resource = nullptr;
        std::uint8_t* mapped = nullptr;
    };

    ID3D12Device* device = nullptr;
    ID3D12Resource* texture = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    std::vector<UploadBuffer> uploads;
    std::vector<std::uint64_t> fenceValues;
    std::uint64_t nextFenceValue = 1;
    std::atomic<std::uint64_t> textureHandle{0};
    std::atomic<std::uint32_t> textureFlags{0};
    std::atomic<bool> inGame{false};
    D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptor{};
    std::uint32_t textureWidth = 0;
    std::uint32_t textureHeight = 0;
    std::uint32_t rowPitch = 0;
    std::atomic<std::uint64_t> backbufferSize{0};

    void ReleaseTexture()
    {
        textureHandle.store(0, std::memory_order_release);
        for (auto& upload : uploads) {
            if (upload.resource) {
                upload.resource->Unmap(0, nullptr);
                upload.resource->Release();
                upload = {};
            }
        }
        if (texture) {
            texture->Release();
            texture = nullptr;
        }
        textureWidth = 0;
        textureHeight = 0;
        rowPitch = 0;
    }

    bool WaitForFrame(std::uint32_t index)
    {
        if (!fence || index >= fenceValues.size()) {
            return false;
        }
        const auto value = fenceValues[index];
        if (!value || fence->GetCompletedValue() >= value) {
            return true;
        }
        if (FAILED(fence->SetEventOnCompletion(value, fenceEvent))) {
            return false;
        }
        return ::WaitForSingleObject(fenceEvent, 5000) == WAIT_OBJECT_0;
    }

    bool PrepareTexture(std::uint32_t width, std::uint32_t height)
    {
        if (texture && width == textureWidth && height == textureHeight) {
            return true;
        }
        for (std::uint32_t i = 0; i < fenceValues.size(); ++i) {
            if (!WaitForFrame(i)) {
                return false;
            }
        }
        ReleaseTexture();

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC image{};
        image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        image.Width = width;
        image.Height = height;
        image.DepthOrArraySize = 1;
        image.MipLevels = 1;
        image.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        image.SampleDesc.Count = 1;
        image.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
            &image, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)))) {
            return false;
        }

        rowPitch = (width * 4 + 255) & ~255u;
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = std::uint64_t(rowPitch) * height;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (auto& upload : uploads) {
            if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
                &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&upload.resource)))) {
                ReleaseTexture();
                return false;
            }
            if (FAILED(upload.resource->Map(0, nullptr,
                reinterpret_cast<void**>(&upload.mapped)))) {
                ReleaseTexture();
                return false;
            }
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = image.Format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture, &view, cpuDescriptor);
        textureWidth = width;
        textureHeight = height;
        LOG(INFO) << "SkyCraft: overlay texture ready " << width << "x" << height;
        return true;
    }
}

void SkyCraftOverlayInitialize(ID3D12Device* value, ID3D12DescriptorHeap* heap,
    std::uint32_t bufferCount)
{
    if (!value || !heap || !bufferCount) {
        return;
    }
    device = value;
    uploads.resize(bufferCount);
    fenceValues.resize(bufferCount);
    const auto stride = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cpuDescriptor = heap->GetCPUDescriptorHandleForHeapStart();
    gpuDescriptor = heap->GetGPUDescriptorHandleForHeapStart();
    cpuDescriptor.ptr += stride;
    gpuDescriptor.ptr += stride;
    fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&fence)))) {
        LOG(ERROR) << "SkyCraft: could not create overlay GPU fence";
        SkyCraftOverlayShutdown();
    }
}

void SkyCraftOverlayShutdown()
{
    if (fence) {
        for (std::uint32_t i = 0; i < fenceValues.size(); ++i) {
            WaitForFrame(i);
        }
    }
    ReleaseTexture();
    if (fence) {
        fence->Release();
        fence = nullptr;
    }
    if (fenceEvent) {
        ::CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
    uploads.clear();
    fenceValues.clear();
    device = nullptr;
    nextFenceValue = 1;
}

bool SkyCraftOverlayBeginFrame(std::uint32_t bufferIndex)
{
    return !fence || WaitForFrame(bufferIndex);
}

bool SkyCraftOverlayWaitIdle()
{
    if (!fence) {
        return true;
    }
    for (std::uint32_t index = 0; index < fenceValues.size(); ++index) {
        if (!WaitForFrame(index)) {
            return false;
        }
    }
    return true;
}

void SkyCraftOverlayUpload(ID3D12GraphicsCommandList* commands, std::uint32_t bufferIndex)
{
    if (!device || !commands || bufferIndex >= uploads.size()) {
        return;
    }
    SkyCraftOverlayFrame frame{};
    if (!SkyCraftBridgeAcquireOverlay(&frame) || !frame.pixels || !frame.width
        || !frame.height) {
        return;
    }
    if (!PrepareTexture(frame.width, frame.height)) {
        return;
    }
    auto& upload = uploads[bufferIndex];
    const auto rowBytes = std::size_t(frame.width) * 4;
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        std::memcpy(upload.mapped + std::size_t(y) * rowPitch,
            frame.pixels + std::size_t(y) * rowBytes, rowBytes);
    }

    const bool ready = textureHandle.load(std::memory_order_acquire) != 0;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = ready ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        : D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    if (ready) {
        commands->ResourceBarrier(1, &barrier);
    }

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    source.PlacedFootprint.Footprint.Width = frame.width;
    source.PlacedFootprint.Footprint.Height = frame.height;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = rowPitch;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commands->ResourceBarrier(1, &barrier);
    textureFlags.store(frame.flags, std::memory_order_release);
    textureHandle.store(gpuDescriptor.ptr, std::memory_order_release);
}

void SkyCraftOverlayEndFrame(ID3D12CommandQueue* queue, std::uint32_t bufferIndex)
{
    if (queue && fence && bufferIndex < fenceValues.size()) {
        const auto value = nextFenceValue++;
        if (SUCCEEDED(queue->Signal(fence, value))) {
            fenceValues[bufferIndex] = value;
        }
    }
}

void SkyCraftOverlaySetBackbufferSize(std::uint32_t width, std::uint32_t height)
{
    const auto packed = (std::uint64_t(width) << 32) | height;
    const auto previous = backbufferSize.exchange(packed, std::memory_order_acq_rel);
    if (previous != packed) {
        const auto display = ImGui::GetIO().DisplaySize;
        LOG(INFO) << "SkyCraft: backbuffer " << width << "x" << height
                  << ", ImGui display " << display.x << "x" << display.y;
    }
}

void SkyCraftOverlayGetBackbufferSize(std::uint32_t& width, std::uint32_t& height)
{
    const auto packed = backbufferSize.load(std::memory_order_acquire);
    width = std::uint32_t(packed >> 32);
    height = std::uint32_t(packed);
}

void SkyCraftOverlaySetInGame(bool value)
{
    inGame.store(value, std::memory_order_release);
}

void SkyCraftOverlayDraw()
{
    const auto handle = textureHandle.load(std::memory_order_acquire);
    if (!handle || !SkyCraftBridgeMcAlive() || SkyCraftHostPassthroughActive()
        || !inGame.load(std::memory_order_acquire)) {
        return;
    }
    std::uint32_t targetWidth = 0, targetHeight = 0;
    SkyCraftOverlayGetBackbufferSize(targetWidth, targetHeight);
    const auto size = targetWidth && targetHeight
        ? ImVec2(float(targetWidth), float(targetHeight)) : ImGui::GetIO().DisplaySize;
    const bool bottomUp = (textureFlags.load(std::memory_order_acquire) & 1u) != 0;
    ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)handle, ImVec2(0, 0),
        size, bottomUp ? ImVec2(0, 1) : ImVec2(0, 0),
        bottomUp ? ImVec2(1, 0) : ImVec2(1, 1));
}
