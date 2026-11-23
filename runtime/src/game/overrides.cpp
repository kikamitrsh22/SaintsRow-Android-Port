// Game-specific overrides of recompiled functions. The recompiler emits each function as
// `__imp__sub_X` plus a weak alias `sub_X`; a strong `PPC_FUNC(sub_X)` here replaces every direct
// call and the lookup-table entry, and can still run the original through `__imp__sub_X`.
// Fixes ported from Saints Reborn (same default.xex) - see docs/SAINTSREBORN_FINDINGS.md.
#include "../guest.h"
#include "../perf_hint.h"
#include "../gpu/gpu.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

extern "C" PPC_FUNC(__imp__sub_82105B18);
extern "C" PPC_FUNC(__imp__sub_8265F4F0);
extern "C" PPC_FUNC(__imp__sub_8262FFE0);
extern "C" PPC_FUNC(__imp__sub_82185498);
extern "C" PPC_FUNC(__imp__sub_825DAA80);

namespace
{
    bool IsGuestImageAddress(uint32_t addr) { return addr >= 0x82000000u && addr < 0x83000000u; }

    constexpr uint32_t kRenderHookPtr = 0x8370F248;
    constexpr uint32_t kRenderGateA = 0x8370E9DF;
    constexpr uint32_t kRenderGateB = 0x8370E9F6;
}

// "Is this object finished?" used by the post-"Begin Game" loader. The objects are sounds; with
// silent audio they never finish, so after 6 s of continuous "busy" report them done.
PPC_FUNC(sub_82105B18)
{
    const uint32_t obj = ctx.r3.u32;
    __imp__sub_82105B18(ctx, base);
    const bool done = (ctx.r3.u32 & 0xFF) != 0;

    static std::mutex s_lock;
    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_busySince;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(s_lock);
    if (done)
    {
        s_busySince.erase(obj);
        return;
    }
    auto [it, inserted] = s_busySince.try_emplace(obj, now);
    if (!inserted && now - it->second > std::chrono::seconds(6))
    {
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; LOG("sub_82105B18: object %08X busy > 6 s, reporting done (no audio)", obj); }
        ctx.r3.u64 = 1;
    }
}

// Builds a filtered copy of the streaming list. Some callers pass small sentinel values
// (0xF, 0x04000000) instead of a pointer; the original dereferences them and spins.
PPC_FUNC(sub_8265F4F0)
{
    const uint32_t filter = ctx.r3.u32;
    if (filter != 0 && filter < 0x10000000u)
    {
        ctx.r3.u64 = 0;
        static thread_local uint32_t s_rejected = 0;
        if (s_rejected < 50) s_rejected++;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return;
    }
    __imp__sub_8265F4F0(ctx, base);
}

// Mid-asm hooks inside sub_8265F4F0 (config/SaintsRow.toml): keep the loop bounds 8-bit.
void SrClampByteR10(PPCRegister& r10) { r10.u64 &= 0xFF; }
void SrClampByteR7(PPCRegister& r7) { r7.u64 &= 0xFF; }
void SrClampByteR8(PPCRegister& r8) { r8.u64 &= 0xFF; }

// Per-frame render entry: an indirect call through [0x8370F248] is gated by two bytes. Clear the
// gates while the pointer is not code.
PPC_FUNC(sub_8262FFE0)
{
    if (!IsGuestImageAddress(guest::r32(kRenderHookPtr)))
    {
        guest::w8(kRenderGateA, 0);
        guest::w8(kRenderGateB, 0);
    }
    __imp__sub_8262FFE0(ctx, base);
}

// Main game loop: same gate, checked on entry.
PPC_FUNC(sub_82185498)
{
    if (!IsGuestImageAddress(guest::r32(kRenderHookPtr)))
        guest::w8(kRenderGateA, 0);
    __imp__sub_82185498(ctx, base);
}

// ---- frame rate ----------------------------------------------------------------------------------
// Mid-asm hooks at 0x82201830 / 0x82370D40 (config/SaintsRow.toml), the "Unlock FPS" patch:
//   li r3,0 ; stw r3,-4(r11) ; stfs f13,0(r11)
void SrUnlockFpsStore(PPCRegister& r3, PPCRegister& r11, PPCRegister& f13)
{
    r3.u64 = 0;
    guest::w32(r11.u32 - 4, 0);
    float value = float(f13.f64);
    uint32_t bits;
    memcpy(&bits, &value, 4);
    guest::w32(r11.u32, bits);
}

// Havok physics step follows the frame rate (idea from the Juiced Patch / xenia-canary TU1 "Havok
// physics FPS fix" by Clippy95, Tervel and uzis, MIT). Physics runs in fixed steps of
// Havok_step_time (float 0x8370DC9C, 1/60 s). sub_82255FD0 works out how much to simulate this
// frame: floor((carry + frame time) / step) whole steps, capped. With a fixed 1/60 step, frames
// just under 1/60 get no step at all and the next one two, so physics objects (doors, cutscene
// props) stutter at 60 fps. Set the step from the game's own frame time (0x827AA61C) right before
// that calculation: one step per frame up to 1/60 s, two up to 1/30 s, the game's value beyond.
// The step is made a hair smaller than frame/n so the floor never loses a step (a step computed
// from a host-measured, previous-frame time did, and the world ran at 60-80% speed).
extern "C" PPC_FUNC(__imp__sub_82255FD0);
PPC_FUNC(sub_82255FD0)
{
    constexpr uint32_t kStep = 0x8370DC9C, kFrameTime = 0x827AA61C;
    static float s_original = 0;
    static const bool s_off = perf::Switch("havok_step_original");
    auto rf = [](uint32_t a) { uint32_t b = guest::r32(a); float f; memcpy(&f, &b, 4); return f; };
    auto wf = [](uint32_t a, float f) { uint32_t b; memcpy(&b, &f, 4); guest::w32(a, b); };

    const float cur = rf(kStep);
    if (s_original == 0 && cur > 0.001f && cur < 0.1f)
    {
        s_original = cur;
        LOG("Havok step: game value %.5f s (%.1f Hz)", cur, 1.0 / cur);
    }
    const float dt = rf(kFrameTime);
    if (s_original != 0 && !s_off)
    {
        float step = s_original;
        if (dt > 1.0f / 400 && dt <= s_original * 1.03f) step = dt * 0.999f;
        else if (dt > s_original * 1.03f && dt <= s_original * 2.06f) step = dt * 0.5f * 0.999f;
        wf(kStep, step);
    }

    __imp__sub_82255FD0(ctx, base);

    // Diagnostics every 10 s: simulated time against frame time (1.00 = real speed).
    static double s_sim = 0, s_dt = 0;
    static int s_calls = 0;
    static auto s_next = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    s_sim += ctx.f1.f64;
    s_dt += dt;
    s_calls++;
    if (std::chrono::steady_clock::now() >= s_next)
    {
        LOG("Havok step: %d calls, frame time %.2f ms avg, simulated/frame time %.3f%s", s_calls,
            s_calls ? s_dt / s_calls * 1000 : 0.0, s_dt > 0 ? s_sim / s_dt : 0.0, s_off ? " [fix off]" : "");
        s_sim = s_dt = 0;
        s_calls = 0;
        s_next = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
}

// Guest CRT memset (dst = r3, byte = r4, len = r5) and memcpy/memmove (dst = r3, src = r4, len = r5):
// ~5% of the CPU time in the city when recompiled; run them natively (as Saints Reborn does).
// r3 stays dst, as in the originals.
PPC_FUNC(sub_82702DD0)
{
    if (ctx.r5.u32) memset(base + ctx.r3.u32, int(ctx.r4.u32 & 0xFF), ctx.r5.u32);
}
PPC_FUNC(sub_82702900)
{
    if (ctx.r5.u32 && ctx.r3.u32 != ctx.r4.u32) memmove(base + ctx.r3.u32, base + ctx.r4.u32, ctx.r5.u32);
}

// D3D "wait for the GPU" step (called in a loop by sub_825D39A8 and friends until the GPU's fence
// passes a batch): the original busy-spins between checks. Most waits end within microseconds
// (ring space), but at 60 fps the frame-slot wait is long - spinning there on the fastest core is
// heat that lowers the clocks. Spin as before for the first 300 us of a wait, then sleep briefly
// between checks. Switch file "spin_gpu_wait" (perf::Switch) keeps the original spin.
PPC_FUNC(sub_825DAA80)
{
    using clock = std::chrono::steady_clock;
    thread_local clock::time_point t_waitStart, t_lastCall;
    const auto now = clock::now();
    if (now - t_lastCall > std::chrono::microseconds(200)) t_waitStart = now; // a new wait
    t_lastCall = now;
    static const bool s_spin = perf::Switch("spin_gpu_wait");
    if (!s_spin && now - t_waitStart > std::chrono::microseconds(300))
    {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        t_lastCall = clock::now();
    }
    __imp__sub_825DAA80(ctx, base);
    gpu::g_gpuWaitUs += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - now).count());
}
