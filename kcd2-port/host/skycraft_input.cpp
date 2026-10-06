#include "skycraft_input.hpp"
#include "skycraft_mouse_delta.hpp"
#include "skycraft_raw_mouse.hpp"
#include "skycraft_window_mouse.hpp"

#include "../bridge/skycraft_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <set>
#include <vector>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <windows.h>
#include <hooks/hooking.hpp>
#include <logger/logger.hpp>
#include <threads/util.hpp>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace
{
    using namespace skycraft::proto;

    struct Key
    {
        int virtualKey;
        std::uint16_t scancode;
        bool down = false;
        std::uint64_t nextRepeatMs = 0;
    };

    struct Button
    {
        int virtualKey;
        std::uint16_t sdlButton;
        bool down = false;
    };

    std::vector<Key> keys{
        {'A', 4}, {'B', 5}, {'C', 6}, {'D', 7}, {'E', 8}, {'F', 9},
        {'G', 10}, {'H', 11}, {'I', 12}, {'J', 13}, {'K', 14}, {'L', 15},
        {'M', 16}, {'N', 17}, {'O', 18}, {'P', 19}, {'Q', 20}, {'R', 21},
        {'S', 22}, {'T', 23}, {'U', 24}, {'V', 25}, {'W', 26}, {'X', 27},
        {'Y', 28}, {'Z', 29},
        {'1', 30}, {'2', 31}, {'3', 32}, {'4', 33}, {'5', 34},
        {'6', 35}, {'7', 36}, {'8', 37}, {'9', 38}, {'0', 39},
        {VK_RETURN, 40}, {VK_ESCAPE, 41}, {VK_BACK, 42},
        {VK_TAB, 43}, {VK_SPACE, 44},
        {VK_OEM_MINUS, 45}, {VK_OEM_PLUS, 46},
        {VK_OEM_4, 47}, {VK_OEM_6, 48}, {VK_OEM_5, 49},
        {VK_OEM_1, 51}, {VK_OEM_7, 52}, {VK_OEM_3, 53},
        {VK_OEM_COMMA, 54}, {VK_OEM_PERIOD, 55}, {VK_OEM_2, 56},
        {VK_CAPITAL, 57}, {VK_F1, 58}, {VK_F2, 59},
        {VK_F3, 60}, {VK_F5, 62},
        {VK_HOME, 74}, {VK_PRIOR, 75}, {VK_DELETE, 76},
        {VK_END, 77}, {VK_NEXT, 78},
        {VK_RIGHT, 79}, {VK_LEFT, 80}, {VK_DOWN, 81}, {VK_UP, 82},
        {VK_CONTROL, 224}, {VK_SHIFT, 225}, {VK_MENU, 226}
    };
    std::array<Button, 5> buttons{{
        {VK_LBUTTON, 1}, {VK_RBUTTON, 3}, {VK_MBUTTON, 2},
        {VK_XBUTTON1, 4}, {VK_XBUTTON2, 5}
    }};
    bool activeLastTick = false;
    bool sessionReleasePending = false;
    bool nativeInput = false;
    bool toggleWasDown = false;
    std::atomic<bool> captureMouse{false};
    std::atomic<bool> captureKeyboard{false};
    std::atomic<bool> captureGuiMouse{false};
    std::atomic<std::uint64_t> stateCalls{0};
    std::atomic<std::uint64_t> dataCalls{0};
    std::atomic<std::uint64_t> keyboardStateCalls{0};
    std::atomic<std::uint64_t> keyboardDataCalls{0};
    std::atomic<std::uint64_t> rawMouseCalls{0};
    std::atomic<std::uint64_t> windowMouseCalls{0};
    void* rawDataTarget = nullptr;
    void* rawBufferTarget = nullptr;
    void* asyncKeyTarget = nullptr;
    void* keyStateTarget = nullptr;
    void* cursorPositionTarget = nullptr;
    void* clipCursorTarget = nullptr;
    RECT previousCursorClip{};
    bool cursorClipSaved = false;
    skycraft::MouseDelta mouseDelta;
    int cursorX = 0;
    int cursorY = 0;
    int cursorWidth = 1, cursorHeight = 1;
    std::uint32_t cursorViewportW = 1, cursorViewportH = 1;
    bool guiLastTick = false;
    std::uint64_t lastInputReport = 0;

    bool MouseDevice(void* device)
    {
        constexpr DWORD kLegacyMouseType = 2;
        DIDEVCAPS capabilities{};
        capabilities.dwSize = sizeof(capabilities);
        auto* input = static_cast<IDirectInputDevice8W*>(device);
        if (FAILED(input->GetCapabilities(&capabilities))) {
            return false;
        }
        const auto type = capabilities.dwDevType & 0xff;
        return type == DI8DEVTYPE_MOUSE || type == kLegacyMouseType;
    }

    bool KeyboardDevice(void* device)
    {
        DIDEVCAPS capabilities{};
        capabilities.dwSize = sizeof(capabilities);
        auto* input = static_cast<IDirectInputDevice8W*>(device);
        if (FAILED(input->GetCapabilities(&capabilities))) {
            return false;
        }
        return (capabilities.dwDevType & 0xff) == DI8DEVTYPE_KEYBOARD;
    }

    SHORT WINAPI HookGetAsyncKeyState(int key)
    {
        const auto original = big::hooking::detour_hook_helper::get_original<
            decltype(&HookGetAsyncKeyState)>(asyncKeyTarget);
        if (captureMouse.load(std::memory_order_acquire)
            && skycraft::NativeMouseVirtualKey(key)) return 0;
        return original(key);
    }

    SHORT WINAPI HookGetKeyState(int key)
    {
        const auto original = big::hooking::detour_hook_helper::get_original<
            decltype(&HookGetKeyState)>(keyStateTarget);
        if (captureMouse.load(std::memory_order_acquire)
            && skycraft::NativeMouseVirtualKey(key)) return 0;
        return original(key);
    }

    SHORT ReadPhysicalKey(int key)
    {
        // The host's own forwarding path must bypass its native polling hook.
        if (asyncKeyTarget) {
            const auto original = big::hooking::detour_hook_helper::get_original<
                decltype(&HookGetAsyncKeyState)>(asyncKeyTarget);
            return original(key);
        }
        return ::GetAsyncKeyState(key);
    }

    BOOL WINAPI HookSetCursorPos(int x, int y)
    {
        const auto original = big::hooking::detour_hook_helper::get_original<
            decltype(&HookSetCursorPos)>(cursorPositionTarget);
        if (captureGuiMouse.load(std::memory_order_acquire)) return TRUE;
        return original(x, y);
    }

    BOOL WINAPI HookClipCursor(const RECT* rect)
    {
        const auto original = big::hooking::detour_hook_helper::get_original<
            decltype(&HookClipCursor)>(clipCursorTarget);
        return original(captureGuiMouse.load(std::memory_order_acquire) ? nullptr : rect);
    }

    void SetGuiCursorCapture(bool gui)
    {
        const bool wasGui = captureGuiMouse.exchange(gui, std::memory_order_acq_rel);
        if (gui == wasGui) return;
        const auto clip = clipCursorTarget
            ? big::hooking::detour_hook_helper::get_original<decltype(&HookClipCursor)>(clipCursorTarget)
            : &::ClipCursor;
        if (gui) {
            cursorClipSaved = ::GetClipCursor(&previousCursorClip) != FALSE;
            clip(nullptr);
        } else if (cursorClipSaved) {
            clip(&previousCursorClip);
            cursorClipSaved = false;
        }
    }

    HRESULT STDMETHODCALLTYPE HookGetDeviceState(void* device, DWORD bytes, void* data)
    {
        const auto target = (*static_cast<void***>(device))[9];
        const auto original = big::hooking::detour_hook_helper::get_original<
            decltype(&HookGetDeviceState)>(target);
        const auto result = original(device, bytes, data);
        if (SUCCEEDED(result) && data && captureMouse.load(std::memory_order_acquire)
            && (bytes == sizeof(DIMOUSESTATE) || bytes == sizeof(DIMOUSESTATE2))
            && MouseDevice(device)) {
            auto* mouse = static_cast<DIMOUSESTATE*>(data);
            const bool gui = captureGuiMouse.load(std::memory_order_acquire);
            mouseDelta.Record(skycraft::MouseDelta::Source::State,
                gui ? mouse->lX : 0, gui ? mouse->lY : 0, mouse->lZ);
            mouse->lZ = 0;
            if (gui) {
                mouse->lX = mouse->lY = 0;
            }
            std::memset(mouse->rgbButtons, 0,
                bytes == sizeof(DIMOUSESTATE2) ? 8 : 4);
            stateCalls.fetch_add(1, std::memory_order_relaxed);
        } else if (SUCCEEDED(result) && data && bytes == 256
            && captureKeyboard.load(std::memory_order_acquire)
            && KeyboardDevice(device)) {
            std::memset(data, 0, bytes);
            keyboardStateCalls.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE HookGetDeviceData(void* device, DWORD objectBytes,
        DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags)
    {
        const auto target = (*static_cast<void***>(device))[10];
        const auto original = big::hooking::detour_hook_helper::get_original<
            decltype(&HookGetDeviceData)>(target);
        const auto result = original(device, objectBytes, data, count, flags);
        const bool mouse = captureMouse.load(std::memory_order_acquire)
            && MouseDevice(device);
        const bool keyboard = captureKeyboard.load(std::memory_order_acquire)
            && KeyboardDevice(device);
        if (SUCCEEDED(result) && data && count && objectBytes >= 8
            && (mouse || keyboard)) {
            auto* raw = reinterpret_cast<std::uint8_t*>(data);
            DWORD kept = 0;
            for (DWORD i = 0; i < *count; ++i) {
                const auto* event = raw + std::size_t(i) * objectBytes;
                DWORD offset = 0, value = 0;
                std::memcpy(&offset, event, sizeof(offset));
                std::memcpy(&value, event + 4, sizeof(value));
                if (mouse && offset == DIMOFS_Z) {
                    if (!(flags & DIGDD_PEEK)) {
                        mouseDelta.Record(skycraft::MouseDelta::Source::Buffered,
                            0, 0, static_cast<LONG>(value));
                    }
                    continue;
                }
                if (mouse && captureGuiMouse.load(std::memory_order_acquire)
                    && (offset == DIMOFS_X || offset == DIMOFS_Y)) {
                    if (!(flags & DIGDD_PEEK)) {
                        mouseDelta.Record(skycraft::MouseDelta::Source::Buffered,
                            offset == DIMOFS_X ? static_cast<LONG>(value) : 0,
                            offset == DIMOFS_Y ? static_cast<LONG>(value) : 0, 0);
                    }
                    continue;
                }
                if ((keyboard || (mouse && offset >= DIMOFS_BUTTON0
                    && offset <= DIMOFS_BUTTON7)) && (value & 0x80)) {
                    (keyboard ? keyboardDataCalls : dataCalls)
                        .fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (kept != i) {
                    std::memmove(raw + std::size_t(kept) * objectBytes,
                        event, objectBytes);
                }
                ++kept;
            }
            *count = kept;
        }
        return result;
    }

    void FilterRawMouse(RAWINPUT* input, skycraft::MouseDelta::Source source)
    {
        if (!input || !captureMouse.load(std::memory_order_acquire)) return;
        // Read wheel before stripping it. Quiet DI reads must not lock out Raw
        // Input. The menu cursor itself uses absolute host-window coordinates.
        if (input->header.dwType == RIM_TYPEMOUSE
            && input->header.dwSize >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE)) {
            const auto& mouse = input->data.mouse;
            if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
                mouseDelta.Record(source, 0, 0, static_cast<SHORT>(mouse.usButtonData));
        }
        if (skycraft::FilterRawMousePacket(*input,
            captureGuiMouse.load(std::memory_order_acquire))) {
            rawMouseCalls.fetch_add(1, std::memory_order_relaxed);
        }
    }

    UINT WINAPI HookGetRawInputData(HRAWINPUT input, UINT command, LPVOID data,
        PUINT bytes, UINT headerBytes)
    {
        const auto original = big::hooking::detour_hook_helper::get_original<
            decltype(&HookGetRawInputData)>(rawDataTarget);
        const auto result = original(input, command, data, bytes, headerBytes);
        if (command == RID_INPUT && data && result != UINT(-1)
            && result >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE)) {
            FilterRawMouse(static_cast<RAWINPUT*>(data), skycraft::MouseDelta::Source::RawData);
        }
        return result;
    }

    UINT WINAPI HookGetRawInputBuffer(PRAWINPUT data, PUINT bytes,
        UINT headerBytes)
    {
        const auto original = big::hooking::detour_hook_helper::get_original<
            decltype(&HookGetRawInputBuffer)>(rawBufferTarget);
        const auto count = original(data, bytes, headerBytes);
        if (data && count != UINT(-1)) {
            auto* packet = data;
            for (UINT i = 0; i < count; ++i) {
                FilterRawMouse(packet, skycraft::MouseDelta::Source::RawBuffered);
                constexpr auto alignment = sizeof(std::uint64_t);
                const auto stride = (std::size_t(packet->header.dwSize)
                    + alignment - 1) & ~(alignment - 1);
                packet = reinterpret_cast<PRAWINPUT>(
                    reinterpret_cast<std::uint8_t*>(packet) + stride);
            }
        }
        return count;
    }

    void CollectDeviceHooks(void* device, std::set<void*>& states,
        std::set<void*>& events)
    {
        auto** methods = *static_cast<void***>(device);
        states.insert(methods[9]);
        events.insert(methods[10]);
    }

    bool Send(std::uint16_t type, std::uint16_t code = 0,
        std::int32_t value = 0, std::int32_t b = 0)
    {
        const InputEvent event{type, code, value, b, 0};
        return SkyCraftBridgePushInput(&event);
    }

    bool PrintableKey(int virtualKey)
    {
        return (virtualKey >= 'A' && virtualKey <= 'Z')
            || (virtualKey >= '0' && virtualKey <= '9')
            || virtualKey == VK_SPACE
            || (virtualKey >= VK_OEM_1 && virtualKey <= VK_OEM_7)
            || virtualKey == VK_OEM_MINUS || virtualKey == VK_OEM_PLUS
            || virtualKey == VK_OEM_COMMA || virtualKey == VK_OEM_PERIOD;
    }

    void SendText(int virtualKey)
    {
        BYTE keyboard[256]{};
        if (!::GetKeyboardState(keyboard)) {
            return;
        }
        const auto layout = ::GetKeyboardLayout(0);
        const auto scan = ::MapVirtualKeyExW(virtualKey, MAPVK_VK_TO_VSC, layout);
        wchar_t characters[8]{};
        const int count = ::ToUnicodeEx(virtualKey, scan, keyboard,
            characters, 8, 0, layout);
        for (int i = 0; i < count; ++i) {
            std::uint32_t codepoint = characters[i];
            if (codepoint >= 0xd800 && codepoint <= 0xdbff
                && i + 1 < count) {
                const std::uint32_t low = characters[i + 1];
                if (low >= 0xdc00 && low <= 0xdfff) {
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10)
                        + (low - 0xdc00);
                    ++i;
                }
            }
            if (codepoint >= 0x20 && codepoint <= 0x10ffff) {
                Send(kInText, 0, static_cast<std::int32_t>(codepoint));
            }
        }
    }

    bool GameIsForeground()
    {
        const auto window = ::GetForegroundWindow();
        DWORD owner = 0;
        if (window) {
            ::GetWindowThreadProcessId(window, &owner);
        }
        return owner == ::GetCurrentProcessId();
    }
}

void SkyCraftInputInstallHooks()
{
    static bool installed = false;
    static std::uint64_t nextAttemptMs = 0;
    const auto now = ::GetTickCount64();
    if (installed || now < nextAttemptMs || !big::g_hooking) {
        return;
    }
    nextAttemptMs = now + 1000;
    std::set<void*> stateTargets;
    std::set<void*> eventTargets;
    IDirectInput8W* wide = nullptr;
    if (SUCCEEDED(::DirectInput8Create(::GetModuleHandleW(nullptr),
        DIRECTINPUT_VERSION, IID_IDirectInput8W,
        reinterpret_cast<void**>(&wide), nullptr))) {
        IDirectInputDevice8W* mouse = nullptr;
        if (SUCCEEDED(wide->CreateDevice(GUID_SysMouse, &mouse, nullptr))) {
            CollectDeviceHooks(mouse, stateTargets, eventTargets);
            mouse->Release();
        }
        IDirectInputDevice8W* keyboard = nullptr;
        if (SUCCEEDED(wide->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr))) {
            CollectDeviceHooks(keyboard, stateTargets, eventTargets);
            keyboard->Release();
        }
        wide->Release();
    }
    IDirectInput8A* narrow = nullptr;
    if (SUCCEEDED(::DirectInput8Create(::GetModuleHandleW(nullptr),
        DIRECTINPUT_VERSION, IID_IDirectInput8A,
        reinterpret_cast<void**>(&narrow), nullptr))) {
        IDirectInputDevice8A* mouse = nullptr;
        if (SUCCEEDED(narrow->CreateDevice(GUID_SysMouse, &mouse, nullptr))) {
            CollectDeviceHooks(mouse, stateTargets, eventTargets);
            mouse->Release();
        }
        IDirectInputDevice8A* keyboard = nullptr;
        if (SUCCEEDED(narrow->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr))) {
            CollectDeviceHooks(keyboard, stateTargets, eventTargets);
            keyboard->Release();
        }
        narrow->Release();
    }
    if (stateTargets.empty() || eventTargets.empty()) {
        LOG(WARNING) << "SkyCraft: DirectInput mouse hooks unavailable";
        return;
    }
    {
        struct SuspendOthers {
            bool mine = !big::threads::are_suspended;
            SuspendOthers() { if (mine) big::threads::suspend_all_but_one(); }
            ~SuspendOthers() { if (mine) big::threads::resume_all(); }
        } suspended;
        for (void* target : stateTargets) {
            big::hooking::detour_hook_helper::add("SkyCraft mouse state",
                target, reinterpret_cast<void*>(&HookGetDeviceState));
        }
        for (void* target : eventTargets) {
            big::hooking::detour_hook_helper::add("SkyCraft mouse events",
                target, reinterpret_cast<void*>(&HookGetDeviceData));
        }
        const auto user32 = ::GetModuleHandleW(L"user32.dll");
        asyncKeyTarget = user32 ? reinterpret_cast<void*>(
            ::GetProcAddress(user32, "GetAsyncKeyState")) : nullptr;
        keyStateTarget = user32 ? reinterpret_cast<void*>(
            ::GetProcAddress(user32, "GetKeyState")) : nullptr;
        cursorPositionTarget = user32 ? reinterpret_cast<void*>(
            ::GetProcAddress(user32, "SetCursorPos")) : nullptr;
        clipCursorTarget = user32 ? reinterpret_cast<void*>(
            ::GetProcAddress(user32, "ClipCursor")) : nullptr;
        if (cursorPositionTarget) {
            big::hooking::detour_hook_helper::add("SkyCraft GUI cursor warp",
                cursorPositionTarget, reinterpret_cast<void*>(&HookSetCursorPos));
        }
        if (clipCursorTarget) {
            big::hooking::detour_hook_helper::add("SkyCraft GUI cursor clipping",
                clipCursorTarget, reinterpret_cast<void*>(&HookClipCursor));
        }
        if (asyncKeyTarget) {
            big::hooking::detour_hook_helper::add("SkyCraft native async mouse buttons",
                asyncKeyTarget, reinterpret_cast<void*>(&HookGetAsyncKeyState));
        }
        if (keyStateTarget) {
            big::hooking::detour_hook_helper::add("SkyCraft native mouse button state",
                keyStateTarget, reinterpret_cast<void*>(&HookGetKeyState));
        }
        rawDataTarget = user32 ? reinterpret_cast<void*>(
            ::GetProcAddress(user32, "GetRawInputData")) : nullptr;
        rawBufferTarget = user32 ? reinterpret_cast<void*>(
            ::GetProcAddress(user32, "GetRawInputBuffer")) : nullptr;
        if (rawDataTarget) {
            big::hooking::detour_hook_helper::add("SkyCraft raw mouse data",
                rawDataTarget, reinterpret_cast<void*>(&HookGetRawInputData));
        }
        if (rawBufferTarget) {
            big::hooking::detour_hook_helper::add("SkyCraft raw mouse buffer",
                rawBufferTarget, reinterpret_cast<void*>(&HookGetRawInputBuffer));
        }
    }
    installed = true;
    LOG(INFO) << "SkyCraft: DirectInput mouse hooks registered ("
              << stateTargets.size() << " state, " << eventTargets.size()
              << " buffered event targets, raw data " << (rawDataTarget != nullptr)
              << ", raw buffer " << (rawBufferTarget != nullptr) << ")";
}

void SkyCraftInputTick(std::uint32_t hostFlags, std::uint32_t viewportW,
    std::uint32_t viewportH)
{
    McState mc{};
    const bool active = (hostFlags & kSkyInGame) && GameIsForeground()
        && SkyCraftBridgeMcAlive() && SkyCraftBridgeReadMcState(&mc)
        && (mc.flags & kMcInWorld);
    const bool toggleDown = (ReadPhysicalKey(VK_F8) & 0x8000) != 0;
    if (active && toggleDown && !toggleWasDown) {
        nativeInput = !nativeInput;
        LOG(INFO) << "SkyCraft: mouse assigned to "
                  << (nativeInput ? "KCD2" : "Minecraft");
    }
    toggleWasDown = toggleDown;
    // An open MC screen always owns its cursor and keyboard.
    if (active && (mc.flags & kMcScreenOpen)) nativeInput = false;
    const bool capture = active && !nativeInput;
    captureMouse.store(capture, std::memory_order_release);
    captureKeyboard.store(capture, std::memory_order_release);
    const bool gui = capture && (mc.flags & kMcScreenOpen);
    SetGuiCursorCapture(gui);
    if (sessionReleasePending) {
        if (!Send(kInReleaseAll)) return;
        sessionReleasePending = false;
    }
    if (!capture) {
        mouseDelta.Reset();
        guiLastTick = false;
        if (activeLastTick && Send(kInReleaseAll)) {
            activeLastTick = false;
            for (auto& key : keys) {
                key.down = false;
            }
            for (auto& button : buttons) {
                button.down = false;
            }
        }
        return;
    }
    if (!activeLastTick) {
        LOG(INFO) << "SkyCraft: Minecraft mouse input active";
        activeLastTick = true;
    }
    const auto now = ::GetTickCount64();
    if (gui != guiLastTick) mouseDelta.Reset();
    const auto motion = mouseDelta.Drain();
    bool cursorDelivered = true;
    if (gui) {
        cursorViewportW = viewportW ? viewportW : 1920;
        cursorViewportH = viewportH ? viewportH : 1080;
        const int width = std::min(cursorViewportW, 2560u);
        const int height = std::min(cursorViewportH, 1440u);
        if (!guiLastTick) {
            cursorX = width / 2;
            cursorY = height / 2;
        } else {
            cursorX = std::clamp(cursorX + motion.x, 0, width - 1);
            cursorY = std::clamp(cursorY + motion.y, 0, height - 1);
        }
        cursorWidth = width;
        cursorHeight = height;
        POINT point{};
        RECT clientRect{};
        const auto window = ::GetForegroundWindow();
        if (window && ::GetCursorPos(&point) && ::ScreenToClient(window, &point)
            && ::GetClientRect(window, &clientRect)
            && clientRect.right > 0 && clientRect.bottom > 0) {
            cursorX = std::clamp(static_cast<int>(std::lround(
                double(point.x) * width / clientRect.right)), 0, width - 1);
            cursorY = std::clamp(static_cast<int>(std::lround(
                double(point.y) * height / clientRect.bottom)), 0, height - 1);
        }
        // Retry every tick and publish the position before mouse-down events.
        cursorDelivered = Send(kInCursor, 0, cursorX, cursorY);
        if (!cursorDelivered) {
            LOG(WARNING) << "SkyCraft: Minecraft GUI cursor update dropped";
        }
    }
    guiLastTick = gui;
    for (auto& key : keys) {
        const bool down = (ReadPhysicalKey(key.virtualKey) & 0x8000) != 0;
        if (down != key.down && Send(kInKey, key.scancode, down ? 1 : 0)) {
            key.down = down;
            key.nextRepeatMs = now + 400;
            if (down && (mc.flags & kMcScreenOpen)
                && PrintableKey(key.virtualKey)) {
                SendText(key.virtualKey);
            }
        } else if (down && key.down && (mc.flags & kMcScreenOpen)
            && now >= key.nextRepeatMs) {
            if (Send(kInKey, key.scancode, 1)
                && PrintableKey(key.virtualKey)) {
                SendText(key.virtualKey);
            }
            key.nextRepeatMs = now + 50;
        }
    }
    for (auto& button : buttons) {
        const bool down = (ReadPhysicalKey(button.virtualKey) & 0x8000) != 0;
        if (gui && down && !cursorDelivered) continue;
        if (down != button.down && Send(kInMouseButton, button.sdlButton, down ? 1 : 0)) {
            button.down = down;
        }
    }
    const int wheel = motion.wheel;
    if (wheel != 0 && !Send(kInScroll, 0, wheel)) {
        mouseDelta.RetryWheel(wheel);
    }
    if (now - lastInputReport >= 10000) {
        lastInputReport = now;
        LOG(INFO) << "SkyCraft: native mouse states filtered "
                  << stateCalls.exchange(0) << ", button events filtered "
                  << dataCalls.exchange(0) << ", keyboard states filtered "
                  << keyboardStateCalls.exchange(0)
                  << ", keyboard events filtered "
                  << keyboardDataCalls.exchange(0)
                  << ", mouse delta source "
                  << static_cast<int>(mouseDelta.Selected());
        LOG(INFO) << "SkyCraft: raw mouse packets filtered "
                  << rawMouseCalls.exchange(0) << ", window mouse messages filtered "
                  << windowMouseCalls.exchange(0);
    }
}

bool SkyCraftInputCapturingMouse()
{
    return captureMouse.load(std::memory_order_acquire);
}

bool SkyCraftInputNativeHandoff() { return nativeInput; }

void SkyCraftInputResetSession()
{
    nativeInput = false;
    toggleWasDown = (ReadPhysicalKey(VK_F8) & 0x8000) != 0;
    mouseDelta.Reset();
    SetGuiCursorCapture(false);
    captureMouse.store(false, std::memory_order_release);
    captureKeyboard.store(false, std::memory_order_release);
    guiLastTick = false;
    activeLastTick = false;
    sessionReleasePending = !Send(kInReleaseAll);
    for (auto& key : keys) key.down = false;
    for (auto& button : buttons) button.down = false;
}

bool SkyCraftInputConsumeWindowMessage(std::uint32_t message)
{
    const bool consumed = skycraft::ConsumeNativeMouseMessage(message,
        captureMouse.load(std::memory_order_acquire),
        captureGuiMouse.load(std::memory_order_acquire));
    if (consumed) windowMouseCalls.fetch_add(1, std::memory_order_relaxed);
    return consumed;
}

bool SkyCraftInputCursor(int& x, int& y)
{
    x = static_cast<int>(std::lround((cursorX + 0.5) * cursorViewportW
        / cursorWidth - 0.5));
    y = static_cast<int>(std::lround((cursorY + 0.5) * cursorViewportH
        / cursorHeight - 0.5));
    return guiLastTick;
}
