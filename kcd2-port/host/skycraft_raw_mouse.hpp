#pragma once

#include <cstddef>
#include <windows.h>

namespace skycraft
{
    inline bool FilterRawMousePacket(RAWINPUT& input, bool gui)
    {
        if (input.header.dwType != RIM_TYPEMOUSE
            || input.header.dwSize < offsetof(RAWINPUT, data) + sizeof(RAWMOUSE))
            return false;
        auto& mouse = input.data.mouse;
        mouse.usButtonFlags = 0;
        mouse.usButtonData = 0;
        mouse.ulRawButtons = 0;
        if (gui) {
            mouse.usFlags &= ~MOUSE_MOVE_ABSOLUTE;
            mouse.lLastX = mouse.lLastY = 0;
        }
        return true;
    }
}
