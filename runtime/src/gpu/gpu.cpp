#include "gpu.h"
#include "../guest.h"
#include "../kernel/kernel.h"

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

    void OnSwap()
    {
        uint64_t n = ++s_swaps;
        static auto last = steady_clock::now();
        static uint64_t lastN = 0;
        auto now = steady_clock::now();
        if (now - last >= seconds(5))
        {
            double fps = double(n - lastN) / duration<double>(now - last).count();
            LOG("gpu: %llu swaps (%.1f fps)", (unsigned long long)n, fps);
            last = now;
            lastN = n;
        }
    }

    uint64_t SwapCount() { return s_swaps; }
}
