#pragma once

#include <cstdint>

// Sound output for the game's XAudio render client and the XMA hardware decoder (see audio.cpp).
namespace audio
{
    // Call after the guest memory is set up. Without a decoder/output the game runs silently.
    void Init();

    // XAudioRegisterRenderDriverClient: returns the client index, or -1.
    int RegisterClient(uint32_t callback, uint32_t callbackArg);
    void UnregisterClient(int index);
    // XAudioSubmitRenderDriverFrame: one frame of 6 x 256 big-endian floats at guest `samples`.
    void SubmitFrame(int index, uint32_t samples);

    // XMACreateContext / XMAReleaseContext: guest address of a hardware XMA context, or 0.
    uint32_t CreateXmaContext();
    void ReleaseXmaContext(uint32_t context);
}
