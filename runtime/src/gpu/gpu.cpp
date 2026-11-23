#include "gpu.h"
#include "../guest.h"
#include "../kernel/kernel.h"
#include "../perf_hint.h"
#include "../xglue/xglue.h"

#include <algorithm>
#include <thread>
#include <vector>

#include <atomic>
#include <chrono>
#include <mutex>

using namespace std::chrono;

namespace gpu
{
    static std::atomic<uint64_t> s_swaps{ 0 };
    static std::mutex s_interruptLock;
    static std::shared_ptr<kernel::GuestThread> s_interruptCtx;

    void DispatchInterrupt(uint32_t callback, uint32_t userData, uint32_t source, uint32_t cpu)
    {
        if (!callback) return;
        std::lock_guard lock(s_interruptLock);
        if (!s_interruptCtx) s_interruptCtx = kernel::CreateHostContext("gpu interrupt");
        kernel::BindCurrentThread(s_interruptCtx.get());
        // Interrupts run as a DPC on the target CPU.
        guest::w8(s_interruptCtx->pcr + 0x10C, uint8_t(cpu == 0xFFFFFFFF ? 2 : cpu));
        PPCContext& ctx = *s_interruptCtx->ctx;
        ctx.r1.u64 = s_interruptCtx->stackBase - 0x100;
        ctx.r3.u64 = source;
        ctx.r4.u64 = userData;
        GuestCall(ctx, callback);
    }

    // Frame rate cap (menu: 30 / 60 / 120): hold the game thread at VdSwap until the frame's time
    // slot comes. Xenia's own limiter only paces what gets presented; the game kept running ahead.
    static void LimitFrameRate()
    {
        static steady_clock::time_point s_next;
        const int cap = xglue::FrameRateCap();
        if (cap <= 0) return;
        const auto period = duration_cast<steady_clock::duration>(duration<double>(1.0 / cap));
        auto now = steady_clock::now();
        if (s_next.time_since_epoch().count() == 0 || now > s_next + period * 2) s_next = now; // behind: no catch-up burst
        if (now < s_next) std::this_thread::sleep_until(s_next);
        s_next += period;
    }

    // Swap times for the performance overlay (ring of the last 1024 frames).
    std::atomic<uint64_t> g_gpuWaitUs{ 0 };
    static std::mutex s_frameTimesLock;
    static steady_clock::time_point s_frameTimes[1024];
    static uint32_t s_frameTimesNext = 0;

    FrameStats GetFrameStats(int windowMs)
    {
        FrameStats st;
        std::vector<double> deltas;
        steady_clock::time_point newest, oldest;
        {
            std::lock_guard lock(s_frameTimesLock);
            const uint32_t n = std::min<uint32_t>(s_frameTimesNext, 1024);
            if (n < 2) return st;
            const auto now = steady_clock::now();
            const auto cutoff = now - milliseconds(windowMs);
            newest = s_frameTimes[(s_frameTimesNext - 1) % 1024];
            oldest = newest;
            for (uint32_t i = 1; i < n; i++)
            {
                const auto t = s_frameTimes[(s_frameTimesNext - 1 - i) % 1024];
                if (t < cutoff) break;
                deltas.push_back(duration<double, std::milli>(oldest - t).count());
                oldest = t;
            }
            // A stalled game: no recent frames at all.
            if (newest < cutoff) return st;
        }
        if (deltas.empty()) return st;
        double sum = 0;
        for (double d : deltas) sum += d;
        st.frames = int(deltas.size());
        st.avgMs = float(sum / deltas.size());
        st.fps = float(deltas.size() * 1000.0 / sum);
        std::sort(deltas.begin(), deltas.end());
        st.maxMs = float(deltas.back());
        st.p99Ms = float(deltas[std::min(deltas.size() - 1, size_t(deltas.size() * 0.99))]);
        return st;
    }

    void OnSwap()
    {
        LimitFrameRate();
        {
            std::lock_guard lock(s_frameTimesLock);
            s_frameTimes[s_frameTimesNext++ % 1024] = steady_clock::now();
        }
        perf::OnFrame();
        uint64_t n = ++s_swaps;
        static auto last = steady_clock::now();
        static auto lastFrame = steady_clock::now();
        static uint64_t lastN = 0;
        // Frame-time spikes: frames over 20 ms (a missed 60 Hz frame), over 33 ms, the longest.
        static uint32_t s_over20 = 0, s_over33 = 0;
        static double s_maxMs = 0;
        auto now = steady_clock::now();
        const double frameMs = duration<double, std::milli>(now - lastFrame).count();
        lastFrame = now;
        // Stutter log: every frame over 30 ms (at most 4 lines a second) with the file I/O done
        // since the previous frame.
        {
            static uint64_t s_lastOpens = 0, s_lastReads = 0, s_lastBytes = 0, s_lastReadUs = 0, s_lastWaitUs = 0;
            const uint64_t waitUs = g_gpuWaitUs;
            const uint64_t opens = kernel::g_io.opens, reads = kernel::g_io.reads,
                           bytes = kernel::g_io.readBytes, readUs = kernel::g_io.readUs;
            static steady_clock::time_point s_lastSpikeLog;
            if (frameMs > 30 && now - s_lastSpikeLog > milliseconds(250))
            {
                s_lastSpikeLog = now;
                LOG("spike: %.0f ms frame | waiting for GPU %.1f ms | files: %llu opens, %llu reads, %.2f MB in %.1f ms",
                    frameMs, (waitUs - s_lastWaitUs) / 1000.0,
                    (unsigned long long)(opens - s_lastOpens), (unsigned long long)(reads - s_lastReads),
                    (bytes - s_lastBytes) / 1048576.0, (readUs - s_lastReadUs) / 1000.0);
            }
            s_lastWaitUs = waitUs;
            s_lastOpens = opens;
            s_lastReads = reads;
            s_lastBytes = bytes;
            s_lastReadUs = readUs;
        }
        if (frameMs > 20) s_over20++;
        if (frameMs > 33) s_over33++;
        if (frameMs > s_maxMs) s_maxMs = frameMs;
        if (now - last >= seconds(5))
        {
            double fps = double(n - lastN) / duration<double>(now - last).count();
            LOG("gpu: %llu swaps (%.1f fps) slow frames: %u >20ms, %u >33ms, max %.0f ms", (unsigned long long)n, fps,
                s_over20, s_over33, s_maxMs);
            last = now;
            lastN = n;
            s_over20 = s_over33 = 0;
            s_maxMs = 0;
        }
    }

    uint64_t SwapCount() { return s_swaps; }
}
