// GPU front-end: the Xenos emulation itself is Xenia's VulkanGraphicsSystem (see xglue).
// This file runs the game's graphics interrupt callback on a guest context and counts swaps.
#pragma once

#include <cstdint>

namespace gpu
{
    // Called by Xenia's GPU threads: runs the guest interrupt callback.
    void DispatchInterrupt(uint32_t callback, uint32_t userData, uint32_t source, uint32_t cpu);
    void OnSwap();
    uint64_t SwapCount();

    // Frame statistics over the last `windowMs` of presented game frames (VdSwap times):
    // frames per second, average / 99th percentile ("1% low") / longest frame time in ms.
    struct FrameStats
    {
        float fps = 0, avgMs = 0, p99Ms = 0, maxMs = 0;
        int frames = 0;
    };
    FrameStats GetFrameStats(int windowMs);
}
