#pragma once

#include "../bridge/skycraft_bridge.h"
#include <cstdint>

struct lua_State;

bool SkyCraftSceneAcceptRequest(std::uint32_t type, const std::uint8_t* payload,
    std::uint32_t bytes);
void SkyCraftSceneTick(lua_State* state, const skycraft::proto::SkyState& host,
    bool active);
