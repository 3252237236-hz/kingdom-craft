#pragma once

#include "../bridge/skycraft_bridge.h"

struct lua_State;

bool SkyCraftCombatAcceptTrace(std::uint32_t type, const std::uint8_t* data, std::uint32_t bytes);

void SkyCraftCombatTick(lua_State* state, const skycraft::proto::SkyState& host,
    bool minecraftAlive);
