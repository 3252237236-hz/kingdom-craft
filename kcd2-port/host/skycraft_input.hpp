#pragma once

#include <cstdint>

void SkyCraftInputTick(std::uint32_t hostFlags, std::uint32_t viewportW,
    std::uint32_t viewportH);
void SkyCraftInputInstallHooks();
bool SkyCraftInputCapturingMouse();
bool SkyCraftInputCursor(int& x, int& y);
bool SkyCraftInputConsumeWindowMessage(std::uint32_t message);
void SkyCraftInputResetSession();
bool SkyCraftInputNativeHandoff();
