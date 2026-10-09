#pragma once
#include "../bridge/skycraft_kcd2_protocol.hpp"
#include <string>
struct lua_State;
bool SkyCraftMobsAccept(std::uint32_t type,const std::uint8_t* data,std::uint32_t bytes);
void SkyCraftMobsReset(lua_State* state);
void SkyCraftMobsTick(lua_State* state,std::uint32_t epoch);
std::string SkyCraftMobUuidText(const skycraft::kcd2::MobUuid& uuid);
