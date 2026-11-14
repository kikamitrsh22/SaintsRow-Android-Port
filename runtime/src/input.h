// Controller state shared between the platform layer (Android gamepad events, XInput on Windows)
// and XamInputGetState. Layout follows XINPUT_GAMEPAD.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace input
{
    // XINPUT_GAMEPAD button bits
    enum : uint16_t
    {
        kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008,
        kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040, kRightThumb = 0x0080,
        kLeftShoulder = 0x0100, kRightShoulder = 0x0200, kGuide = 0x0400,
        kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
    };

    struct PadState
    {
        uint16_t buttons = 0;
        uint8_t leftTrigger = 0, rightTrigger = 0;
        int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
    };

    // Called by the platform layer whenever the pad changes.
    void SetPad(const PadState& state);
    // Snapshot for the guest; packet number increments on every change.
    PadState GetPad(uint32_t* packetNumber);
    bool IsConnected();
    void SetConnected(bool connected);
}
