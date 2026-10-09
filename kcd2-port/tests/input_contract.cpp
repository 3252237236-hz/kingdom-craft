#include "../host/skycraft_mouse_delta.hpp"
#include "../host/skycraft_raw_mouse.hpp"
#include "../host/skycraft_window_mouse.hpp"
#include "../host/skycraft_session.hpp"
#include "../host/skycraft_cursor.hpp"
#include "../host/skycraft_camera.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

int main()
{
    skycraft::proto::McState camera{};
    camera.flags = skycraft::proto::kMcInWorld | skycraft::proto::kMcCameraFrame;
    camera.teleportAck = 42;
    assert(skycraft::camera::ValidVanillaFrame(camera, 42));
    assert(!skycraft::camera::ValidVanillaFrame(camera, 43));
    camera.flags |= skycraft::proto::kMcDead;
    assert(!skycraft::camera::ValidVanillaFrame(camera, 42));
    camera.flags &= ~skycraft::proto::kMcDead;
    camera.cameraMode = 3;
    assert(!skycraft::camera::ValidVanillaFrame(camera, 42));
    camera.cameraMode = 2;
    camera.cameraYaw = 240; camera.cameraPitch = -15;
    const auto front = skycraft::camera::ToNativeAngles(camera.cameraYaw, camera.cameraPitch);
    assert(front.x == 15 && front.y == 0 && front.z == -60);
    camera.eyeX = std::numeric_limits<double>::quiet_NaN();
    assert(!skycraft::camera::ValidVanillaFrame(camera, 42));
    // Reproduce the user's fullscreen mismatch: client 1920x1080, game
    // backbuffer 1280x720. A cursor at 2/3 must stay at 2/3 after composition.
    assert(std::abs(skycraft::CursorFraction(1279.5f,1920)-2.f/3.f)<1e-6);
    assert(std::abs(skycraft::CursorFraction(719.5f,1080)-2.f/3.f)<1e-6);
    assert(std::abs(skycraft::CursorFraction(1279.5f,1920)*1280-853.3333f)<.001);
    assert(std::abs(skycraft::CursorFraction(1919.f,1920)-1919.5f/1920)<1e-6);
    assert(skycraft::CursorFraction(-1,1920)==0);
    assert(skycraft::CursorFraction(3000,1920)==1);
    assert(skycraft::CursorFraction(123,0)==0);
    assert(skycraft::NativeMouseVirtualKey(VK_LBUTTON));
    assert(skycraft::NativeMouseVirtualKey(VK_RBUTTON));
    assert(!skycraft::NativeMouseVirtualKey('W'));
    assert(!skycraft::NativeMouseVirtualKey(VK_F8));
    for (const auto msg : {WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN,
        WM_RBUTTONUP, WM_LBUTTONDBLCLK, WM_XBUTTONDOWN, WM_MOUSEWHEEL}) {
        assert(skycraft::ConsumeNativeMouseMessage(msg, true, false));
        assert(!skycraft::ConsumeNativeMouseMessage(msg, false, true));
    }
    assert(!skycraft::ConsumeNativeMouseMessage(WM_MOUSEMOVE, true, false));
    assert(skycraft::ConsumeNativeMouseMessage(WM_MOUSEMOVE, true, true));
    assert(!skycraft::ConsumeNativeMouseMessage(WM_INPUT, true, true));
    assert(!skycraft::ConsumeNativeMouseMessage(WM_CLOSE, true, true));
    using Delta = skycraft::MouseDelta;
    Delta delta;
    // Two DirectInput APIs report the same movement and wheel notch.
    delta.Record(Delta::Source::State, 7, -3, 120);
    delta.Record(Delta::Source::Buffered, 7, -3, 120);
    auto sample = delta.Drain();
    assert(sample.x == 7 && sample.y == -3 && sample.wheel == 120);
    // An alternative source must not replay the same motion in a later tick.
    delta.Record(Delta::Source::Buffered, 7, -3, 120);
    sample = delta.Drain();
    assert(sample.x == 0 && sample.y == 0 && sample.wheel == 0);
    delta.Record(Delta::Source::State, 4, 2, 0);
    sample = delta.Drain();
    assert(sample.x == 4 && sample.y == 2 && sample.wheel == 0);
    // A new menu/input session can select buffered-only mouse input.
    delta.Reset();
    delta.Record(Delta::Source::Buffered, -9, 6, -120);
    delta.Record(Delta::Source::State, -9, 6, -120);
    sample = delta.Drain();
    assert(sample.x == -9 && sample.y == 6 && sample.wheel == -120);
    // A full shared-memory input ring should not lose scroll on retry.
    delta.RetryWheel(sample.wheel);
    assert(delta.Drain().wheel == -120);
    delta.RetryMotion(7, -3);
    sample = delta.Drain();
    assert(sample.x == 7 && sample.y == -3);
    assert(delta.Drain().x == 0);
    delta.Record(Delta::Source::Buffered, 3, 4, 120);
    delta.Reset();
    sample = delta.Drain();
    assert(sample.x == 0 && sample.y == 0 && sample.wheel == 0);
    assert(delta.Selected() == Delta::Source::None);
    // Quiet DI reads previously stole the source from the game's Raw Input.
    delta.Record(Delta::Source::State, 0, 0, 0);
    delta.Record(Delta::Source::RawData, 0, 0, 120);
    delta.Record(Delta::Source::RawBuffered, 0, 0, 120);
    assert(delta.Drain().wheel == 120);
    // Cursor motion and wheel can legitimately come from different APIs.
    delta.Record(Delta::Source::Buffered, 5, -2, 0);
    delta.Record(Delta::Source::RawData, 0, 0, -120);
    sample = delta.Drain();
    assert(sample.x == 5 && sample.y == -2 && sample.wheel == -120);
    skycraft::HostSession session;
    assert(session.Observe(true, "player1", 0, 1));
    assert(!session.Observe(true, "player1", 0, 1));
    // Same player/level reloaded, then death and a reload with reused ID.
    assert(session.Observe(true, "player1", 1, 1));
    assert(!session.Observe(false, "player1", 1, 1));
    assert(session.Observe(true, "player1", 1, 1));
    assert(!session.Observe(true, "player1", 1, 1));
    assert(session.Observe(true, "player2", 1, 1));
    assert(session.Observe(true, "player2", 1, 2));
    RAWINPUT packet{};
    packet.header.dwType = RIM_TYPEMOUSE;
    packet.header.dwSize = sizeof(packet);
    packet.data.mouse.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_WHEEL;
    packet.data.mouse.usButtonData = 120;
    packet.data.mouse.lLastX = 12;
    packet.data.mouse.lLastY = -8;
    assert(skycraft::FilterRawMousePacket(packet, false));
    assert(packet.data.mouse.usButtonFlags == 0);
    assert(packet.data.mouse.usButtonData == 0);
    assert(packet.data.mouse.lLastX == 12 && packet.data.mouse.lLastY == -8);
    packet.data.mouse.usFlags = MOUSE_MOVE_ABSOLUTE;
    assert(skycraft::FilterRawMousePacket(packet, true));
    assert(packet.data.mouse.lLastX == 0 && packet.data.mouse.lLastY == 0);
    assert(!(packet.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE));
    packet.header.dwType = RIM_TYPEKEYBOARD;
    packet.data.keyboard.VKey = 'W';
    const auto keyboard = packet;
    assert(!skycraft::FilterRawMousePacket(packet, true));
    assert(std::memcmp(&keyboard, &packet, sizeof(packet)) == 0);
    packet.header.dwType = RIM_TYPEMOUSE;
    packet.header.dwSize = sizeof(RAWINPUTHEADER);
    assert(!skycraft::FilterRawMousePacket(packet, true));
    std::cout << "mouse input contracts passed\n";
}
