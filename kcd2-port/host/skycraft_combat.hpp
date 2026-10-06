#pragma once

#include "../bridge/skycraft_bridge.h"

struct lua_State;

void SkyCraftCombatTick(lua_State* state, const skycraft::proto::SkyState& host,
    bool minecraftAlive);
