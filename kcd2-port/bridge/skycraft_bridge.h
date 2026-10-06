#pragma once

#include "../../protocol/skycraft_protocol.h"

#include <cstdint>

#if !defined(_WIN32)
#error SkyCraft KCD2 bridge requires Windows shared memory.
#endif

struct SkyCraftOverlayFrame
{
	const std::uint8_t* pixels;
	std::uint32_t width;
	std::uint32_t height;
	std::uint32_t flags;
	std::uint64_t frameId;
};

using SkyCraftRenderCallback = void (*)(std::uint32_t type, const std::uint8_t* payload,
	std::uint32_t payloadBytes, void* context);

extern "C"
{
	__declspec(dllexport) bool SkyCraftBridgeCreate();
	__declspec(dllexport) void SkyCraftBridgeClose();
	__declspec(dllexport) std::uint32_t SkyCraftBridgeLastError();
	__declspec(dllexport) void SkyCraftBridgeHeartbeat();
	__declspec(dllexport) bool SkyCraftBridgeMcAlive();
	__declspec(dllexport) std::uint32_t SkyCraftBridgeMcPid();
	__declspec(dllexport) bool SkyCraftBridgeWriteHostState(const skycraft::proto::SkyState* state);
	__declspec(dllexport) bool SkyCraftBridgeWriteWater(const skycraft::proto::WaterGrid* grid);
	__declspec(dllexport) bool SkyCraftBridgeWriteActors(const skycraft::proto::ActorRecord* actors,
		std::uint32_t count);
	__declspec(dllexport) bool SkyCraftBridgeReadMcState(skycraft::proto::McState* state);
	__declspec(dllexport) bool SkyCraftBridgePushInput(const skycraft::proto::InputEvent* event);
	__declspec(dllexport) bool SkyCraftBridgeWriteCollision(std::uint32_t type,
		const void* payload, std::uint32_t payloadBytes);
	__declspec(dllexport) bool SkyCraftBridgePopEvent(skycraft::proto::McEvent* event);
	__declspec(dllexport) bool SkyCraftBridgeReadWorldEntities(skycraft::proto::WorldEntities* entities);
	__declspec(dllexport) std::uint64_t SkyCraftBridgeDrainRender(SkyCraftRenderCallback callback,
		void* context, std::uint64_t maxBytes);
	__declspec(dllexport) bool SkyCraftBridgeAcquireOverlay(SkyCraftOverlayFrame* frame);
}
