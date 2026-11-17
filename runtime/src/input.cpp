#include "input.h"

#include <chrono>

namespace input
{
    using Clock = std::chrono::steady_clock;

    // The game reads the pad twice per frame and uses only the second read, so a tap must stay
    // visible for a minimum time rather than for one read: every button press is held for at least
    // kMinHold after it went down, even if it was released sooner.
    constexpr auto kMinHold = std::chrono::milliseconds(100);

    static std::mutex s_lock;
    static PadState s_state;
    static Clock::time_point s_pressedAt[16];
    static uint16_t s_reported = 0;
    static uint32_t s_packet = 1;
    static std::atomic<bool> s_connected{ true };

    void SetPad(const PadState& state)
    {
        std::lock_guard lock(s_lock);
        if (std::memcmp(&s_state, &state, sizeof(state)) != 0)
        {
            uint16_t pressed = uint16_t(state.buttons & ~s_state.buttons);
            auto now = Clock::now();
            for (int i = 0; i < 16; i++)
                if (pressed & (1u << i)) s_pressedAt[i] = now;
            s_state = state;
            ++s_packet;
        }
    }

    PadState GetPad(uint32_t* packetNumber)
    {
        std::lock_guard lock(s_lock);
        PadState out = s_state;
        auto now = Clock::now();
        for (int i = 0; i < 16; i++)
            if (s_pressedAt[i] != Clock::time_point{} && now - s_pressedAt[i] < kMinHold)
                out.buttons |= uint16_t(1u << i);
        if (out.buttons != s_reported)
        {
            s_reported = out.buttons;
            ++s_packet; // the state the guest sees changed
        }
        if (packetNumber) *packetNumber = s_packet;
        return out;
    }

    bool IsConnected() { return s_connected; }
    void SetConnected(bool connected) { s_connected = connected; }
}
