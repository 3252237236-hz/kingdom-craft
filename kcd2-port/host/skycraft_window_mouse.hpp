#pragma once

#include <cstdint>
#include <windows.h>

namespace skycraft
{
    inline bool NativeMouseVirtualKey(int key)
    {
        return key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON
            || key == VK_XBUTTON1 || key == VK_XBUTTON2;
    }
    inline bool ConsumeNativeMouseMessage(std::uint32_t message,
        bool captured, bool gui)
    {
        if (!captured) return false;
        switch (message) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            return true;
        case WM_MOUSEMOVE:
            return gui;
        default:
            return false;
        }
    }
}
