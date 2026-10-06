#include "skycraft_world.hpp"
#include "skycraft_collision_shapes.hpp"

#include "skycraft_host.hpp"
#include "skycraft_overlay.hpp"
#include "../bridge/skycraft_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>
#include <windows.h>
#include <d3d12.h>
#include <imgui.h>
#include <kcd2_address.hpp>
#include <kcd2_init.hpp>
#include <logger/logger.hpp>

namespace
{
    using namespace skycraft::proto;

    constexpr std::size_t kMaxAtlasBytes = 64ull << 20;
    constexpr std::size_t kMaxStoredVertices = 2'000'000;
    constexpr std::size_t kMaxSolidSections = 4096;
    constexpr std::size_t kMaxDrawTriangles = 15'000;
    constexpr double kDrawRadius = 96.0;
    constexpr float kPi = 3.14159265358979323846f;

    struct Camera
    {
        double x = 0, y = 0, z = 0;
        float yaw = 0, pitch = 0;
        bool valid = false;
    };

    struct Section
    {
        std::int32_t sx = 0, sy = 0, sz = 0;
        std::vector<RenVertex> vertices;
    };

    struct UploadBuffer
    {
        ID3D12Resource* resource = nullptr;
        std::uint8_t* mapped = nullptr;
    };

    struct ProjectedVertex
    {
        ImVec2 pos;
        ImVec2 uv;
        ImU32 color;
        float depth;
    };

    struct ProjectedTriangle
    {
        std::array<ProjectedVertex, 3> vertices;
        float depth;
    };

    struct Basis
    {
        double rightX, rightZ;
        double upX, upY, upZ;
        double forwardX, forwardY, forwardZ;
    };

    std::mutex worldMutex;
    Camera camera;
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>,
        std::shared_ptr<const Section>> sections;
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>,
        std::array<std::uint64_t, 64>> solidSections;
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>,
        std::vector<skycraft::kcd2::ShapeBox>> shapeSections;
    std::size_t storedVertices = 0;
    std::size_t storedShapeBoxes = 0;
    std::vector<std::uint8_t> atlasPixels;
    std::uint32_t atlasWidth = 0, atlasHeight = 0;
    std::uint64_t atlasRevision = 0;

    ID3D12Device* device = nullptr;
    ID3D12Resource* texture = nullptr;
    std::vector<UploadBuffer> uploads;
    std::atomic<std::uint64_t> textureHandle{0};
    D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptor{};
    std::uint32_t textureWidth = 0, textureHeight = 0, rowPitch = 0;
    std::atomic<std::uint64_t> uploadedRevision{0};
    std::uint64_t lastUploadMs = 0;
    bool loggedGameProjection = false;

    void ResolveGameProjection()
    {
        if (big::g_CD3D9Renderer_ProjectToScreen) {
            return;
        }
        const auto address = kcd2_address::scan(
            "48 83 EC ? 48 8B 0D ? ? ? ? 0F 29 74 24 ? 0F 28 F2 0F 29 7C 24");
        if (address) {
            big::g_CD3D9Renderer_ProjectToScreen = address
                .as<decltype(big::g_CD3D9Renderer_ProjectToScreen)>();
            LOG(INFO) << "SkyCraft: KCD2 world projection resolved";
        } else {
            LOG(WARNING) << "SkyCraft: KCD2 world projection unavailable";
        }
    }

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
        textureWidth = textureHeight = rowPitch = 0;
        uploadedRevision.store(0, std::memory_order_release);
    }

    bool PrepareTexture(std::uint32_t width, std::uint32_t height)
    {
        if (texture && width == textureWidth && height == textureHeight) {
            return true;
        }
        if (!SkyCraftOverlayWaitIdle()) {
            return false;
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
                IID_PPV_ARGS(&upload.resource)))
                || FAILED(upload.resource->Map(0, nullptr,
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
        LOG(INFO) << "SkyCraft: block atlas texture ready " << width << "x" << height;
        return true;
    }

    bool Project(const RenVertex& source, double worldX, double worldY, double worldZ,
        const Camera& view, const Basis& basis, const ImVec2& screen, float focal,
        ProjectedVertex& out)
    {
        const double dx = worldX + source.x - view.x;
        const double dy = worldY + source.y - view.y;
        const double dz = worldZ + source.z - view.z;
        const double forward = dx * basis.forwardX + dy * basis.forwardY
            + dz * basis.forwardZ;
        if (!std::isfinite(forward) || forward < 0.08 || forward > 160.0) {
            return false;
        }
        double x, y;
        if (big::g_CD3D9Renderer_ProjectToScreen) {
            float screenX = 0, screenY = 0, screenZ = 0;
            if (!big::g_CD3D9Renderer_ProjectToScreen(big::g_CD3D9Renderer,
                float(worldX + source.x), float(-(worldZ + source.z)),
                float(worldY + source.y), &screenX, &screenY, &screenZ)) {
                return false;
            }
            x = double(screenX) * screen.x / 100.0;
            y = double(screenY) * screen.y / 100.0;
        } else {
            const double right = dx * basis.rightX + dz * basis.rightZ;
            const double up = dx * basis.upX + dy * basis.upY + dz * basis.upZ;
            x = screen.x * 0.5 + right * focal / forward;
            y = screen.y * 0.5 - up * focal / forward;
        }
        if (!std::isfinite(x) || !std::isfinite(y)
            || std::abs(x) > 100000.0 || std::abs(y) > 100000.0) {
            return false;
        }
        out = { ImVec2(float(x), float(y)), ImVec2(source.u, source.v),
            source.color, float(forward) };
        return true;
    }
}

void SkyCraftWorldAcceptRender(std::uint32_t type, const std::uint8_t* payload,
    std::uint32_t bytes)
{
    if (type == kRenClearAll) {
        std::scoped_lock lock(worldMutex);
        sections.clear();
        solidSections.clear();
        shapeSections.clear();
        storedShapeBoxes = 0;
        storedVertices = 0;
        return;
    }
    if (!payload) {
        return;
    }
    if (type == kRenAtlas) {
        RenAtlas header{};
        if (bytes < sizeof(header)) {
            return;
        }
        std::memcpy(&header, payload, sizeof(header));
        const auto pixelBytes = std::uint64_t(header.width) * header.height * 4;
        if (!header.width || !header.height || header.width > 8192
            || header.height > 8192 || pixelBytes > kMaxAtlasBytes
            || bytes != sizeof(header) + pixelBytes) {
            return;
        }
        std::scoped_lock lock(worldMutex);
        atlasWidth = header.width;
        atlasHeight = header.height;
        atlasPixels.assign(payload + sizeof(header), payload + bytes);
        ++atlasRevision;
        return;
    }
    if (type == kRenAtlasRegion) {
        RenAtlasRegion region{};
        if (bytes < sizeof(region)) {
            return;
        }
        std::memcpy(&region, payload, sizeof(region));
        const auto pixelBytes = std::uint64_t(region.width) * region.height * 4;
        std::scoped_lock lock(worldMutex);
        if (!region.width || !region.height || region.x >= atlasWidth
            || region.y >= atlasHeight || region.width > atlasWidth - region.x
            || region.height > atlasHeight - region.y
            || bytes != sizeof(region) + pixelBytes || atlasPixels.empty()) {
            return;
        }
        for (std::uint32_t y = 0; y < region.height; ++y) {
            const auto destination = (std::size_t(region.y + y) * atlasWidth + region.x) * 4;
            const auto source = sizeof(region) + std::size_t(y) * region.width * 4;
            std::memcpy(atlasPixels.data() + destination, payload + source,
                std::size_t(region.width) * 4);
        }
        ++atlasRevision;
        return;
    }
    if (type == skycraft::kcd2::kRenCollisionShapes) {
        skycraft::kcd2::ShapeHeader header{};
        std::vector<skycraft::kcd2::ShapeBox> boxes;
        if (!skycraft::kcd2::DecodeShapes(payload, bytes, header, boxes)) return;
        const auto key = std::make_tuple(header.sx, header.sy, header.sz);
        std::scoped_lock lock(worldMutex);
        const auto old = shapeSections.find(key);
        const auto oldCount = old == shapeSections.end() ? 0 : old->second.size();
        if (header.flags == skycraft::kcd2::kShapeFallback) {
            storedShapeBoxes -= oldCount;
            shapeSections.erase(key);
            return;
        }
        if ((shapeSections.size() >= kMaxSolidSections
                && old == shapeSections.end())
            || storedShapeBoxes - oldCount + boxes.size() > 2'000'000) return;
        storedShapeBoxes = storedShapeBoxes - oldCount + boxes.size();
        shapeSections[key] = std::move(boxes);
        return;
    }
    if (type == kRenSolids) {
        RenSolids header{};
        if (bytes < sizeof(header)) {
            return;
        }
        std::memcpy(&header, payload, sizeof(header));
        const auto expected = sizeof(header) + (header.count ? 512 : 0);
        if (header.count > 4096 || bytes != expected) {
            return;
        }
        const auto key = std::make_tuple(header.sx, header.sy, header.sz);
        std::scoped_lock lock(worldMutex);
        if (!header.count) {
            solidSections.erase(key);
            return;
        }
        std::array<std::uint64_t, 64> bits{};
        std::memcpy(bits.data(), payload + sizeof(header), 512);
        std::uint32_t counted = 0;
        for (const auto word : bits) {
            counted += std::popcount(word);
        }
        if (counted != header.count) {
            return;
        }
        if (solidSections.size() >= kMaxSolidSections
            && !solidSections.contains(key)) {
            return;
        }
        solidSections[key] = bits;
        return;
    }
    if (type == kRenSection) {
        RenSection header{};
        if (bytes < sizeof(header)) {
            return;
        }
        std::memcpy(&header, payload, sizeof(header));
        const auto vertexBytes = std::uint64_t(header.vertexCount) * sizeof(RenVertex);
        if (header.vertexCount % 3 != 0 || header.vertexCount > kMaxStoredVertices
            || bytes != sizeof(header) + vertexBytes) {
            return;
        }
        const auto key = std::make_tuple(header.sx, header.sy, header.sz);
        auto section = std::make_shared<Section>();
        section->sx = header.sx;
        section->sy = header.sy;
        section->sz = header.sz;
        section->vertices.resize(header.vertexCount);
        if (vertexBytes) {
            std::memcpy(section->vertices.data(), payload + sizeof(header),
                std::size_t(vertexBytes));
        }
        std::scoped_lock lock(worldMutex);
        const auto old = sections.find(key);
        const auto oldCount = old == sections.end() ? 0 : old->second->vertices.size();
        if (storedVertices - oldCount + header.vertexCount > kMaxStoredVertices) {
            return;
        }
        storedVertices = storedVertices - oldCount + header.vertexCount;
        if (header.vertexCount) {
            sections[key] = std::move(section);
        } else if (old != sections.end()) {
            sections.erase(old);
        }
    }
}

void SkyCraftWorldCollectNearbySolids(double x, double y, double z, float radius,
    std::size_t limit, std::vector<SkyCraftSolidBlock>& output)
{
    output.clear();
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)
        || !std::isfinite(radius) || radius <= 0 || !limit) {
        return;
    }
    struct Candidate
    {
        double distanceSquared;
        SkyCraftSolidBlock block;
    };
    std::vector<Candidate> candidates;
    {
        std::scoped_lock lock(worldMutex);
        for (const auto& [key, bits] : solidSections) {
            const auto [sx, sy, sz] = key;
            const double nearestX = std::clamp(x, double(sx) * 16, double(sx) * 16 + 16);
            const double nearestZ = std::clamp(z, double(sz) * 16, double(sz) * 16 + 16);
            if (std::abs(y - (double(sy) * 16 + 8)) > 12
                || (nearestX - x) * (nearestX - x)
                    + (nearestZ - z) * (nearestZ - z) > radius * radius) {
                continue;
            }
            const auto shaped = shapeSections.find(key);
            if (shaped != shapeSections.end()) {
                for (const auto& shape : shaped->second) {
                    const auto bit = shape.cell;
                    const SkyCraftSolidBlock block{
                        sx * 16 + (bit & 15), sy * 16 + (bit >> 8),
                        sz * 16 + ((bit >> 4) & 15), shape.part,
                        shape.minX, shape.minY, shape.minZ,
                        shape.maxX, shape.maxY, shape.maxZ
                    };
                    const double dx = block.x + (shape.minX + shape.maxX) / 2 - x;
                    const double dy = block.y + (shape.minY + shape.maxY) / 2 - y;
                    const double dz = block.z + (shape.minZ + shape.maxZ) / 2 - z;
                    if (dy < -2.5 || dy > 4.0 || dx * dx + dz * dz > radius * radius)
                        continue;
                    candidates.push_back({dx * dx + dz * dz + dy * dy, block});
                }
                continue;
            }
            for (std::size_t wordIndex = 0; wordIndex < bits.size(); ++wordIndex) {
                auto word = bits[wordIndex];
                while (word) {
                    const int bit = int(wordIndex * 64 + std::countr_zero(word));
                    word &= word - 1;
                    const SkyCraftSolidBlock block{
                        sx * 16 + (bit & 15),
                        sy * 16 + (bit >> 8),
                        sz * 16 + ((bit >> 4) & 15)
                    };
                    const double dx = double(block.x) + 0.5 - x;
                    const double dy = double(block.y) + 0.5 - y;
                    const double dz = double(block.z) + 0.5 - z;
                    if (dy < -2.5 || dy > 4.0
                        || dx * dx + dz * dz > radius * radius) {
                        continue;
                    }
                    candidates.push_back({dx * dx + dz * dz + dy * dy, block});
                }
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.distanceSquared != b.distanceSquared)
            return a.distanceSquared < b.distanceSquared;
        return std::tie(a.block.x, a.block.y, a.block.z, a.block.part)
            < std::tie(b.block.x, b.block.y, b.block.z, b.block.part);
    });
    output.reserve(std::min(limit, candidates.size()));
    for (std::size_t i = 0; i < std::min(limit, candidates.size()); ++i) {
        output.push_back(candidates[i].block);
    }
}

void SkyCraftWorldSetCamera(double x, double y, double z, float yaw, float pitch,
    bool valid)
{
    std::scoped_lock lock(worldMutex);
    camera = { x, y, z, yaw, pitch, valid };
}

void SkyCraftWorldInitialize(ID3D12Device* value, ID3D12DescriptorHeap* heap,
    std::uint32_t bufferCount)
{
    if (!value || !heap || !bufferCount) {
        return;
    }
    device = value;
    uploads.resize(bufferCount);
    const auto stride = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cpuDescriptor = heap->GetCPUDescriptorHandleForHeapStart();
    gpuDescriptor = heap->GetGPUDescriptorHandleForHeapStart();
    cpuDescriptor.ptr += 2 * stride;
    gpuDescriptor.ptr += 2 * stride;
    ResolveGameProjection();
}

void SkyCraftWorldShutdown()
{
    SkyCraftOverlayWaitIdle();
    ReleaseTexture();
    uploads.clear();
    device = nullptr;
}

void SkyCraftWorldUpload(ID3D12GraphicsCommandList* commands, std::uint32_t bufferIndex)
{
    if (!device || !commands || bufferIndex >= uploads.size()) {
        return;
    }
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0, height = 0;
    std::uint64_t revision = 0;
    {
        std::scoped_lock lock(worldMutex);
        if (atlasPixels.empty() || atlasRevision == uploadedRevision.load(
            std::memory_order_acquire)) {
            return;
        }
        const auto now = ::GetTickCount64();
        if (texture && now - lastUploadMs < 100) {
            return;
        }
        pixels = atlasPixels;
        width = atlasWidth;
        height = atlasHeight;
        revision = atlasRevision;
    }
    if (!PrepareTexture(width, height)) {
        return;
    }
    auto& upload = uploads[bufferIndex];
    const auto rowBytes = std::size_t(width) * 4;
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(upload.mapped + std::size_t(y) * rowPitch,
            pixels.data() + std::size_t(y) * rowBytes, rowBytes);
    }

    const bool ready = textureHandle.load(std::memory_order_acquire) != 0;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
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
    source.PlacedFootprint.Footprint.Width = width;
    source.PlacedFootprint.Footprint.Height = height;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = rowPitch;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commands->ResourceBarrier(1, &barrier);
    uploadedRevision.store(revision, std::memory_order_release);
    lastUploadMs = ::GetTickCount64();
    textureHandle.store(gpuDescriptor.ptr, std::memory_order_release);
}

void SkyCraftWorldDraw()
{
    const auto handle = textureHandle.load(std::memory_order_acquire);
    if (!handle || !SkyCraftBridgeMcAlive() || SkyCraftHostPassthroughActive()) {
        return;
    }
    Camera view;
    std::vector<std::shared_ptr<const Section>> visible;
    {
        std::scoped_lock lock(worldMutex);
        view = camera;
        if (!view.valid) {
            return;
        }
        for (const auto& [key, section] : sections) {
            const double dx = double(section->sx) * 16 + 8 - view.x;
            const double dy = double(section->sy) * 16 + 8 - view.y;
            const double dz = double(section->sz) * 16 + 8 - view.z;
            if (dx * dx + dy * dy + dz * dz < kDrawRadius * kDrawRadius) {
                visible.push_back(section);
            }
        }
    }
    if (visible.empty()) {
        return;
    }
    std::sort(visible.begin(), visible.end(), [&view](const auto& a, const auto& b) {
        const auto distance = [&view](const auto& section) {
            const double dx = double(section->sx) * 16 + 8 - view.x;
            const double dy = double(section->sy) * 16 + 8 - view.y;
            const double dz = double(section->sz) * 16 + 8 - view.z;
            return dx * dx + dy * dy + dz * dz;
        };
        return distance(a) < distance(b);
    });
    std::uint32_t targetWidth = 0, targetHeight = 0;
    SkyCraftOverlayGetBackbufferSize(targetWidth, targetHeight);
    const auto screen = targetWidth && targetHeight
        ? ImVec2(float(targetWidth), float(targetHeight)) : ImGui::GetIO().DisplaySize;
    if (screen.x <= 0 || screen.y <= 0) {
        return;
    }
    if (!loggedGameProjection && big::g_CD3D9Renderer_ProjectToScreen) {
        loggedGameProjection = true;
        LOG(INFO) << "SkyCraft: KCD2 game camera projection active";
    }
    McState mc{};
    const float fov = SkyCraftBridgeReadMcState(&mc) && std::isfinite(mc.fovDeg)
        && mc.fovDeg >= 40 && mc.fovDeg <= 110 ? mc.fovDeg : 70.0f;
    const float focal = screen.y / (2 * std::tan(fov * kPi / 360.0f));
    const double yaw = double(view.yaw) * kPi / 180.0;
    const double pitch = double(view.pitch) * kPi / 180.0;
    const double sine = std::sin(yaw), cosine = std::cos(yaw);
    const double pitchSine = std::sin(pitch), pitchCosine = std::cos(pitch);
    const Basis basis{ -cosine, -sine, -pitchSine * sine, pitchCosine,
        pitchSine * cosine, -sine * pitchCosine, -pitchSine,
        cosine * pitchCosine };
    std::vector<ProjectedTriangle> triangles;
    triangles.reserve(kMaxDrawTriangles);
    for (const auto& section : visible) {
        const double originX = double(section->sx) * 16;
        const double originY = double(section->sy) * 16;
        const double originZ = double(section->sz) * 16;
        for (std::size_t i = 0; i + 2 < section->vertices.size(); i += 3) {
            ProjectedTriangle triangle{};
            bool projected = true;
            for (int vertex = 0; vertex < 3; ++vertex) {
                if (!Project(section->vertices[i + vertex], originX, originY, originZ,
                    view, basis, screen, focal, triangle.vertices[vertex])) {
                    projected = false;
                    break;
                }
            }
            if (!projected) {
                continue;
            }
            const auto& a = triangle.vertices[0].pos;
            const auto& b = triangle.vertices[1].pos;
            const auto& c = triangle.vertices[2].pos;
            if (std::max({ a.x, b.x, c.x }) < 0 || std::min({ a.x, b.x, c.x }) > screen.x
                || std::max({ a.y, b.y, c.y }) < 0
                || std::min({ a.y, b.y, c.y }) > screen.y) {
                continue;
            }
            triangle.depth = (triangle.vertices[0].depth + triangle.vertices[1].depth
                + triangle.vertices[2].depth) / 3;
            triangles.push_back(triangle);
            if (triangles.size() >= kMaxDrawTriangles) {
                break;
            }
        }
        if (triangles.size() >= kMaxDrawTriangles) {
            break;
        }
    }
    if (triangles.empty()) {
        return;
    }
    std::sort(triangles.begin(), triangles.end(), [](const auto& a, const auto& b) {
        return a.depth > b.depth;
    });
    auto* list = ImGui::GetBackgroundDrawList();
    list->PushTextureID((ImTextureID)handle);
    for (const auto& triangle : triangles) {
        list->PrimReserve(3, 3);
        const auto index = static_cast<ImDrawIdx>(list->_VtxCurrentIdx);
        list->PrimWriteIdx(index);
        list->PrimWriteIdx(index + 1);
        list->PrimWriteIdx(index + 2);
        for (const auto& vertex : triangle.vertices) {
            list->PrimWriteVtx(vertex.pos, vertex.uv, vertex.color);
        }
    }
    list->PopTextureID();
}
