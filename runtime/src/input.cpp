#include "input.h"

namespace input
{
    static std::mutex s_lock;
    static PadState s_state;
    static uint32_t s_packet = 1;
    // Buttons pressed since the guest last read the pad: a tap shorter than one game frame still
    // shows up as pressed for one read.
    static uint16_t s_latched = 0;
    static std::atomic<bool> s_connected{ true };

    void SetPad(const PadState& state)
    {
        std::lock_guard lock(s_lock);
        if (std::memcmp(&s_state, &state, sizeof(state)) != 0)
        {
            s_latched |= uint16_t(state.buttons & ~s_state.buttons); // newly pressed
            s_state = state;
            ++s_packet;
        }
    }

    PadState GetPad(uint32_t* packetNumber)
    {
        std::lock_guard lock(s_lock);
        if (packetNumber) *packetNumber = s_packet;
        PadState out = s_state;
        if (s_latched & ~out.buttons)
        {
            out.buttons |= s_latched;   // report the missed tap once...
            ++s_packet;                 // ...and make the following read a new packet (released)
        }
        s_latched = 0;
        return out;
    }

    bool IsConnected() { return s_connected; }
    void SetConnected(bool connected) { s_connected = connected; }
}
