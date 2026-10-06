#include "skycraft_bridge.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
	using namespace skycraft;

	#define CHECK(condition) do { if (!(condition)) { \
		std::fprintf(stderr, "failed at line %d: %s\n", __LINE__, #condition); \
		return 1; \
	} } while (false)

	void CountRender(std::uint32_t type, const std::uint8_t*, std::uint32_t size, void* context)
	{
		if (type == proto::kRenClearAll && size == 0) {
			++*static_cast<int*>(context);
		}
	}
}

int main()
{
	HANDLE stale = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
		static_cast<DWORD>(proto::kMappingBytes >> 32),
		static_cast<DWORD>(proto::kMappingBytes), proto::kMappingName);
	CHECK(stale != nullptr && ::GetLastError() != ERROR_ALREADY_EXISTS);
	auto* staleData = static_cast<std::uint8_t*>(::MapViewOfFile(stale, FILE_MAP_ALL_ACCESS, 0, 0, 0));
	CHECK(staleData != nullptr);
	auto* staleHeader = reinterpret_cast<proto::Header*>(staleData);
	staleHeader->magic = proto::kMagic;
	staleHeader->version = proto::kVersion;
	staleHeader->skyrimPid = ::GetCurrentProcessId();
	staleHeader->skyrimHeartbeatMs = ::GetTickCount64();
	CHECK(!SkyCraftBridgeCreate());
	CHECK(SkyCraftBridgeLastError() == ERROR_ALREADY_EXISTS);
	staleHeader->skyrimHeartbeatMs = 1;
	staleHeader->skyrimPid = 0;
	reinterpret_cast<proto::SkyState*>(staleData + proto::kOffSkyState)->seq = 77;
	CHECK(SkyCraftBridgeCreate());
	CHECK(reinterpret_cast<proto::SkyState*>(staleData + proto::kOffSkyState)->seq == 0);
	::UnmapViewOfFile(staleData);
	::CloseHandle(stale);
	HANDLE mapping = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, proto::kMappingName);
	CHECK(mapping != nullptr);
	auto* data = static_cast<std::uint8_t*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
	CHECK(data != nullptr);

	auto* header = reinterpret_cast<proto::Header*>(data + proto::kOffHeader);
	CHECK(header->magic == proto::kMagic);
	CHECK(header->version == proto::kVersion);
	CHECK(header->skyrimPid == ::GetCurrentProcessId());
	CHECK(header->skyrimHeartbeatMs != 0);
	CHECK(!SkyCraftBridgeMcAlive());
	header->mcPid = 1234;
	header->mcHeartbeatMs = ::GetTickCount64();
	CHECK(SkyCraftBridgeMcAlive());
	CHECK(SkyCraftBridgeMcPid() == 1234);

	proto::SkyState host{};
	host.flags = proto::kSkyInGame;
	host.worldId = 42;
	host.posX = 1.25;
	host.posY = 66.5;
	host.posZ = -8.0;
	host.teleportSeq = 3;
	CHECK(SkyCraftBridgeWriteHostState(&host));
	auto* sharedHost = reinterpret_cast<proto::SkyState*>(data + proto::kOffSkyState);
	CHECK(sharedHost->seq == 2 && sharedHost->worldId == 42);
	CHECK(sharedHost->posY == 66.5 && sharedHost->teleportSeq == 3);

	proto::WaterGrid water{};
	water.originX = -8;
	water.originZ = 16;
	water.surface[0] = 64.5f;
	CHECK(SkyCraftBridgeWriteWater(&water));
	auto* sharedWater = reinterpret_cast<proto::WaterGrid*>(data + proto::kOffWaterGrid);
	CHECK(sharedWater->seq == 2 && sharedWater->surface[0] == 64.5f);

	std::array<proto::ActorRecord, proto::kMaxActors + 2> actors{};
	actors[0].formId = 99;
	CHECK(SkyCraftBridgeWriteActors(actors.data(), static_cast<std::uint32_t>(actors.size())));
	auto* sharedActors = reinterpret_cast<proto::ActorTable*>(data + proto::kOffActorTable);
	CHECK(sharedActors->seq == 2 && sharedActors->count == proto::kMaxActors);
	CHECK(sharedActors->actors[0].formId == 99);

	auto* sharedGuest = reinterpret_cast<proto::McState*>(data + proto::kOffMcState);
	sharedGuest->seq = 1;
	sharedGuest->flags = proto::kMcInWorld;
	sharedGuest->x = 12.5;
	sharedGuest->frameCounter = 17;
	sharedGuest->seq = 2;
	proto::McState guest{};
	CHECK(SkyCraftBridgeReadMcState(&guest));
	CHECK(guest.flags == proto::kMcInWorld && guest.x == 12.5);
	CHECK(guest.frameCounter == 17);
	sharedGuest->seq = 3;
	CHECK(!SkyCraftBridgeReadMcState(&guest));
	sharedGuest->seq = 4;

	proto::InputEvent input{ proto::kInKey, 26, 1, 0, 0 };
	for (std::uint32_t i = 0; i < proto::kInputRingEntries; ++i) {
		CHECK(SkyCraftBridgePushInput(&input));
	}
	CHECK(!SkyCraftBridgePushInput(&input));
	auto* inputRing = data + proto::kOffInputRing;
	CHECK(*reinterpret_cast<std::uint64_t*>(inputRing) == proto::kInputRingEntries);
	CHECK(reinterpret_cast<proto::InputEvent*>(inputRing + proto::kInputRingDataOff)[0].code == 26);
	*reinterpret_cast<std::uint64_t*>(inputRing + proto::kInputRingTailOff) = 1;
	CHECK(SkyCraftBridgePushInput(&input));

	auto* collisionRing = data + proto::kOffCollisionRing;
	auto* collisionHead = reinterpret_cast<std::uint64_t*>(collisionRing + proto::kColRingHeadOff);
	auto* collisionTail = reinterpret_cast<std::uint64_t*>(collisionRing + proto::kColRingTailOff);
	*collisionHead = proto::kColRingDataBytes - 8;
	*collisionTail = *collisionHead;
	std::uint32_t epoch = 7;
	CHECK(SkyCraftBridgeWriteCollision(proto::kColClear, &epoch, sizeof(epoch)));
	auto* collisionData = collisionRing + proto::kColRingDataOff;
	CHECK(reinterpret_cast<proto::ColMsgHeader*>(collisionData + proto::kColRingDataBytes - 8)->type
		== proto::kColPad);
	CHECK(reinterpret_cast<proto::ColMsgHeader*>(collisionData)->type == proto::kColClear);
	CHECK(*reinterpret_cast<std::uint32_t*>(collisionData + 8) == 7);
	CHECK(*collisionHead == proto::kColRingDataBytes + 16);

	auto* eventRing = data + proto::kOffEventRing;
	auto* event = reinterpret_cast<proto::McEvent*>(eventRing + proto::kEventRingDataOff);
	event[0].type = proto::kEvHitActor;
	event[0].formId = 99;
	event[0].a = 4.0f;
	*reinterpret_cast<std::uint64_t*>(eventRing + proto::kEventRingHeadOff) = 1;
	proto::McEvent received{};
	CHECK(SkyCraftBridgePopEvent(&received));
	CHECK(received.type == proto::kEvHitActor && received.formId == 99 && received.a == 4.0f);
	CHECK(!SkyCraftBridgePopEvent(&received));

	auto* world = reinterpret_cast<proto::WorldEntities*>(data + proto::kOffWorldEntities);
	world->seq = 1;
	world->count = 1;
	world->entities[0].kind = proto::kWeItem;
	world->entities[0].id = 27;
	world->seq = 2;
	proto::WorldEntities worldCopy{};
	CHECK(SkyCraftBridgeReadWorldEntities(&worldCopy));
	CHECK(worldCopy.count == 1 && worldCopy.entities[0].id == 27);

	auto* renderRing = data + proto::kOffRenderRing;
	auto* renderMessage = reinterpret_cast<proto::ColMsgHeader*>(renderRing + proto::kRenRingDataOff);
	*renderMessage = { proto::kRenClearAll, 0 };
	*reinterpret_cast<std::uint64_t*>(renderRing + proto::kRenRingHeadOff) = 8;
	int clearCount = 0;
	CHECK(SkyCraftBridgeDrainRender(CountRender, &clearCount, 1024) == 8);
	CHECK(clearCount == 1);
	CHECK(*reinterpret_cast<std::uint64_t*>(renderRing + proto::kRenRingTailOff) == 8);

	auto* overlay = reinterpret_cast<proto::OverlayCtl*>(data + proto::kOffOverlayCtl);
	auto* slot = reinterpret_cast<proto::OverlaySlotHdr*>(data + proto::kOffOverlaySlotHdr);
	slot->width = 2;
	slot->height = 1;
	slot->frameId = 5;
	data[proto::kOffOverlayPixels] = 0xAB;
	overlay->state = proto::kOverlayDirty;
	SkyCraftOverlayFrame frame{};
	CHECK(SkyCraftBridgeAcquireOverlay(&frame));
	CHECK(frame.width == 2 && frame.height == 1 && frame.frameId == 5);
	CHECK(frame.pixels[0] == 0xAB);
	CHECK(!SkyCraftBridgeAcquireOverlay(&frame));

	SkyCraftBridgeClose();
	CHECK(header->skyrimPid == 0 && header->skyrimHeartbeatMs == 0);
	CHECK(SkyCraftBridgeCreate());
	CHECK(header->skyrimPid == ::GetCurrentProcessId());
	SkyCraftBridgeClose();
	CHECK(header->skyrimPid == 0);
	::UnmapViewOfFile(data);
	::CloseHandle(mapping);
	std::puts("SkyCraft v11 bridge self-test passed");
	return 0;
}
