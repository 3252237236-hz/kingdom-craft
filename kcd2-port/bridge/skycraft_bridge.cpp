#include "skycraft_bridge.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace
{
	using namespace skycraft;

	HANDLE mapping = nullptr;
	std::uint8_t* base = nullptr;
	std::uint32_t overlayFront = 2;
	std::uint32_t lastError = 0;

	template <class T>
	T* At(std::uint64_t offset)
	{
		return reinterpret_cast<T*>(base + offset);
	}

	template <class T>
	class AtomicView
	{
	public:
		explicit AtomicView(T& value) : value_(value) {}

		T load(std::memory_order) const
		{
			if constexpr (sizeof(T) == 4) {
				return static_cast<T>(::InterlockedCompareExchange(
					reinterpret_cast<volatile LONG*>(&value_), 0, 0));
			} else {
				return static_cast<T>(::InterlockedCompareExchange64(
					reinterpret_cast<volatile LONG64*>(&value_), 0, 0));
			}
		}

		void store(T value, std::memory_order) const
		{
			if constexpr (sizeof(T) == 4) {
				::InterlockedExchange(reinterpret_cast<volatile LONG*>(&value_),
					static_cast<LONG>(value));
			} else {
				::InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&value_),
					static_cast<LONG64>(value));
			}
		}

		T exchange(T value, std::memory_order) const
		{
			if constexpr (sizeof(T) == 4) {
				return static_cast<T>(::InterlockedExchange(
					reinterpret_cast<volatile LONG*>(&value_), static_cast<LONG>(value)));
			} else {
				return static_cast<T>(::InterlockedExchange64(
					reinterpret_cast<volatile LONG64*>(&value_), static_cast<LONG64>(value)));
			}
		}

	private:
		T& value_;
	};

	template <class T>
	AtomicView<T> Atomic(T& value)
	{
		return AtomicView<T>(value);
	}

	template <class T>
	void WriteSeqlock(T* destination, const T& source)
	{
		auto sequence = Atomic(destination->seq);
		const auto previous = sequence.load(std::memory_order_relaxed);
		sequence.store(previous + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		std::memcpy(reinterpret_cast<std::uint8_t*>(destination) + 4,
			reinterpret_cast<const std::uint8_t*>(&source) + 4, sizeof(T) - 4);
		sequence.store(previous + 2, std::memory_order_release);
	}

	template <class T>
	bool ReadSeqlock(const T* source, T* destination)
	{
		auto sequence = Atomic(const_cast<std::uint32_t&>(source->seq));
		for (int attempt = 0; attempt < 64; ++attempt) {
			const auto first = sequence.load(std::memory_order_acquire);
			if (first & 1) {
				continue;
			}
			std::memcpy(destination, source, sizeof(T));
			std::atomic_thread_fence(std::memory_order_acquire);
			if (sequence.load(std::memory_order_relaxed) == first) {
				return true;
			}
		}
		return false;
	}

	bool ExistingHostAlive(const proto::Header* header)
	{
		if (Atomic(const_cast<std::uint32_t&>(header->magic)).load(std::memory_order_acquire)
			!= proto::kMagic) {
			return false;
		}
		const auto pid = Atomic(const_cast<std::uint32_t&>(header->skyrimPid))
			.load(std::memory_order_acquire);
		if (!pid) {
			return false;
		}
		HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
		if (!process) {
			return ::GetLastError() != ERROR_INVALID_PARAMETER;
		}
		const bool alive = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
		::CloseHandle(process);
		return alive;
	}
}

bool SkyCraftBridgeCreate()
{
	if (base) {
		return true;
	}
	const auto bytes = proto::kMappingBytes;
	mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
		static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), proto::kMappingName);
	lastError = ::GetLastError();
	if (!mapping) {
		return false;
	}
	const bool existed = lastError == ERROR_ALREADY_EXISTS;
	base = static_cast<std::uint8_t*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
	if (!base) {
		lastError = ::GetLastError();
		::CloseHandle(mapping);
		mapping = nullptr;
		return false;
	}

	auto* header = At<proto::Header>(proto::kOffHeader);
	if (existed && ExistingHostAlive(header)) {
		::UnmapViewOfFile(base);
		base = nullptr;
		::CloseHandle(mapping);
		mapping = nullptr;
		lastError = ERROR_ALREADY_EXISTS;
		return false;
	}

	Atomic(header->magic).store(0, std::memory_order_release);
	std::memset(base + proto::kOffSkyState, 0, sizeof(proto::SkyState));
	std::memset(base + proto::kOffOverlayCtl, 0, 0x100);
	std::memset(base + proto::kOffInputRing, 0, proto::kInputRingDataOff);
	std::memset(base + proto::kOffCollisionRing, 0, proto::kColRingDataOff);
	std::memset(base + proto::kOffActorTable, 0, sizeof(proto::ActorTable));
	std::memset(base + proto::kOffEventRing, 0, proto::kEventRingDataOff);
	std::memset(base + proto::kOffWorldEntities, 0, sizeof(proto::WorldEntities));
	std::memset(base + proto::kOffRenderRing, 0, proto::kRenRingDataOff);
	header->version = proto::kVersion;
	header->skyrimPid = ::GetCurrentProcessId();
	header->skyrimHeartbeatMs = ::GetTickCount64();
	Atomic(header->magic).store(proto::kMagic, std::memory_order_release);
	overlayFront = 2;
	lastError = 0;
	return true;
}

void SkyCraftBridgeClose()
{
	if (base) {
		auto* header = At<proto::Header>(proto::kOffHeader);
		Atomic(header->skyrimHeartbeatMs).store(0, std::memory_order_release);
		Atomic(header->skyrimPid).store(0, std::memory_order_release);
		::UnmapViewOfFile(base);
		base = nullptr;
	}
	if (mapping) {
		::CloseHandle(mapping);
		mapping = nullptr;
	}
	overlayFront = 2;
}

std::uint32_t SkyCraftBridgeLastError()
{
	return lastError;
}

void SkyCraftBridgeHeartbeat()
{
	if (base) {
		Atomic(At<proto::Header>(proto::kOffHeader)->skyrimHeartbeatMs)
			.store(::GetTickCount64(), std::memory_order_release);
	}
}

bool SkyCraftBridgeMcAlive()
{
	if (!base) {
		return false;
	}
	const auto beat = Atomic(At<proto::Header>(proto::kOffHeader)->mcHeartbeatMs)
		.load(std::memory_order_acquire);
	const auto now = ::GetTickCount64();
	return beat != 0 && beat <= now && now - beat < 3000;
}

std::uint32_t SkyCraftBridgeMcPid()
{
	return base ? Atomic(At<proto::Header>(proto::kOffHeader)->mcPid)
		.load(std::memory_order_acquire) : 0;
}

bool SkyCraftBridgeWriteHostState(const proto::SkyState* state)
{
	if (!base || !state) {
		return false;
	}
	WriteSeqlock(At<proto::SkyState>(proto::kOffSkyState), *state);
	return true;
}

bool SkyCraftBridgeWriteWater(const proto::WaterGrid* grid)
{
	if (!base || !grid) {
		return false;
	}
	WriteSeqlock(At<proto::WaterGrid>(proto::kOffWaterGrid), *grid);
	return true;
}

bool SkyCraftBridgeWriteActors(const proto::ActorRecord* actors, std::uint32_t count)
{
	if (!base || (count && !actors)) {
		return false;
	}
	auto* table = At<proto::ActorTable>(proto::kOffActorTable);
	auto sequence = Atomic(table->seq);
	const auto previous = sequence.load(std::memory_order_relaxed);
	sequence.store(previous + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	table->count = std::min(count, proto::kMaxActors);
	if (table->count) {
		std::memcpy(table->actors, actors, table->count * sizeof(proto::ActorRecord));
	}
	sequence.store(previous + 2, std::memory_order_release);
	return true;
}

bool SkyCraftBridgeReadMcState(proto::McState* state)
{
	return base && state && ReadSeqlock(At<proto::McState>(proto::kOffMcState), state);
}

bool SkyCraftBridgePushInput(const proto::InputEvent* event)
{
	if (!base || !event) {
		return false;
	}
	auto* ring = base + proto::kOffInputRing;
	auto& head = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingHeadOff);
	auto& tail = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingTailOff);
	const auto h = Atomic(head).load(std::memory_order_relaxed);
	const auto t = Atomic(tail).load(std::memory_order_acquire);
	if (h < t || h - t >= proto::kInputRingEntries) {
		return false;
	}
	auto* slot = reinterpret_cast<proto::InputEvent*>(ring + proto::kInputRingDataOff)
		+ (h & (proto::kInputRingEntries - 1));
	*slot = *event;
	Atomic(head).store(h + 1, std::memory_order_release);
	return true;
}

bool SkyCraftBridgeWriteCollision(std::uint32_t type, const void* payload, std::uint32_t payloadBytes)
{
	if (!base || (payloadBytes && !payload)) {
		return false;
	}
	constexpr auto capacity = proto::kColRingDataBytes;
	const auto bytes = (sizeof(proto::ColMsgHeader) + std::uint64_t(payloadBytes) + 7) & ~7ull;
	if (bytes > capacity / 2) {
		return false;
	}
	auto* ring = base + proto::kOffCollisionRing;
	auto& head = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingHeadOff);
	auto& tail = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingTailOff);
	auto h = Atomic(head).load(std::memory_order_relaxed);
	const auto t = Atomic(tail).load(std::memory_order_acquire);
	if (h < t || h - t > capacity) {
		return false;
	}
	auto position = h % capacity;
	const auto padding = position + bytes > capacity ? capacity - position : 0;
	if (capacity - (h - t) < bytes + padding) {
		return false;
	}
	auto* data = ring + proto::kColRingDataOff;
	if (padding) {
		*reinterpret_cast<proto::ColMsgHeader*>(data + position) = { proto::kColPad, 0 };
		h += padding;
		position = 0;
	}
	*reinterpret_cast<proto::ColMsgHeader*>(data + position) = { type, payloadBytes };
	if (payloadBytes) {
		std::memcpy(data + position + sizeof(proto::ColMsgHeader), payload, payloadBytes);
	}
	Atomic(head).store(h + bytes, std::memory_order_release);
	return true;
}

bool SkyCraftBridgePopEvent(proto::McEvent* event)
{
	if (!base || !event) {
		return false;
	}
	auto* ring = base + proto::kOffEventRing;
	auto& head = *reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingHeadOff);
	auto& tail = *reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingTailOff);
	const auto h = Atomic(head).load(std::memory_order_acquire);
	auto t = Atomic(tail).load(std::memory_order_relaxed);
	if (t >= h) {
		return false;
	}
	if (h - t > proto::kEventRingEntries) {
		t = h - proto::kEventRingEntries;
	}
	*event = reinterpret_cast<proto::McEvent*>(ring + proto::kEventRingDataOff)
		[t & (proto::kEventRingEntries - 1)];
	Atomic(tail).store(t + 1, std::memory_order_release);
	return true;
}

bool SkyCraftBridgeReadWorldEntities(proto::WorldEntities* entities)
{
	if (!base || !entities) {
		return false;
	}
	auto* source = At<proto::WorldEntities>(proto::kOffWorldEntities);
	auto sequence = Atomic(source->seq);
	for (int attempt = 0; attempt < 16; ++attempt) {
		const auto first = sequence.load(std::memory_order_acquire);
		if (first & 1) {
			continue;
		}
		const auto count = std::min(source->count, proto::kMaxWorldEntities);
		std::memcpy(entities, source, offsetof(proto::WorldEntities, entities)
			+ sizeof(proto::WorldEntity) * count);
		entities->count = count;
		std::atomic_thread_fence(std::memory_order_acquire);
		if (sequence.load(std::memory_order_relaxed) == first) {
			return true;
		}
	}
	return false;
}

std::uint64_t SkyCraftBridgeDrainRender(SkyCraftRenderCallback callback, void* context,
	std::uint64_t maxBytes)
{
	if (!base || !callback) {
		return 0;
	}
	auto* ring = base + proto::kOffRenderRing;
	auto& head = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingHeadOff);
	auto& tail = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingTailOff);
	const auto h = Atomic(head).load(std::memory_order_acquire);
	auto t = Atomic(tail).load(std::memory_order_relaxed);
	constexpr auto capacity = proto::kRenRingDataBytes;
	if (h < t || h - t > capacity) {
		return 0;
	}
	auto* data = ring + proto::kRenRingDataOff;
	std::uint64_t drained = 0;
	while (t < h && drained < maxBytes) {
		const auto position = t % capacity;
		if (capacity - position < sizeof(proto::ColMsgHeader)) {
			Atomic(tail).store(t, std::memory_order_release);
			return drained;
		}
		const auto* message = reinterpret_cast<const proto::ColMsgHeader*>(data + position);
		if (message->type == proto::kRenPad) {
			if (capacity - position > h - t) {
				Atomic(tail).store(t, std::memory_order_release);
				return drained;
			}
			t += capacity - position;
			continue;
		}
		const auto bytes = (sizeof(*message) + std::uint64_t(message->payloadBytes) + 7) & ~7ull;
		if (bytes > capacity - position || bytes > h - t) {
			Atomic(tail).store(t, std::memory_order_release);
			return drained;
		}
		callback(message->type, data + position + sizeof(*message), message->payloadBytes, context);
		t += bytes;
		drained += bytes;
	}
	Atomic(tail).store(t, std::memory_order_release);
	return drained;
}

bool SkyCraftBridgeAcquireOverlay(SkyCraftOverlayFrame* frame)
{
	if (!base || !frame) {
		return false;
	}
	auto& state = At<proto::OverlayCtl>(proto::kOffOverlayCtl)->state;
	if (!(Atomic(state).load(std::memory_order_acquire) & proto::kOverlayDirty)) {
		return false;
	}
	const auto previous = Atomic(state).exchange(overlayFront, std::memory_order_acq_rel);
	overlayFront = previous & 3;
	if (overlayFront >= proto::kOverlaySlots) {
		overlayFront = 2;
		return false;
	}
	const auto* header = At<proto::OverlaySlotHdr>(proto::kOffOverlaySlotHdr
		+ sizeof(proto::OverlaySlotHdr) * overlayFront);
	if (header->width > proto::kMaxOverlayW || header->height > proto::kMaxOverlayH) {
		return false;
	}
	frame->pixels = base + proto::kOffOverlayPixels + proto::kOverlaySlotBytes * overlayFront;
	frame->width = header->width;
	frame->height = header->height;
	frame->flags = header->flags;
	frame->frameId = header->frameId;
	return true;
}
