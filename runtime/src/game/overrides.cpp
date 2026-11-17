// Game-specific overrides of recompiled functions. The recompiler emits each function as
// `__imp__sub_X` plus a weak alias `sub_X`; a strong `PPC_FUNC(sub_X)` here replaces every direct
// call and the lookup-table entry, and can still run the original through `__imp__sub_X`.
// Fixes ported from Saints Reborn (same default.xex) - see docs/SAINTSREBORN_FINDINGS.md.
#include "../guest.h"
#include "../perf_hint.h"

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
    void HavokStepFix();
    HavokStepFix();
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

// Havok physics step follows the frame rate (Saints Reborn's port of the Juiced Patch / xenia-canary
// TU1 fix by Clippy95, Tervel and uzis, MIT). Physics runs in fixed steps of Havok_step_time (float
// at 0x8370DC9C); above 30 fps less than one step fits into a frame, so physics objects (cutscene
// props, doors, the player in cutscenes) only move every other frame. Set the step each frame from
// the real frame time: one step per frame at 60 fps and above, two between 30 and 60, the game's
// own value below 30. Called once per rendered frame.
void HavokStepFix()
{
    constexpr uint32_t kStep = 0x8370DC9C;
    static float s_original = 0;
    static auto s_last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - s_last).count();
    s_last = now;

    uint32_t bits = guest::r32(kStep);
    float cur;
    memcpy(&cur, &bits, 4);
    if (s_original == 0)
    {
        if (!(cur > 0.001f && cur < 0.1f)) return; // not set up yet
        s_original = cur;
        LOG("Havok step: game value %.5f s (%.1f Hz)", cur, 1.0 / cur);
    }
    float step = s_original;
    if (dt > 1.0 / 400.0 && dt <= 1.0 / 60.0 + 0.0005) step = float(dt);
    else if (dt > 1.0 / 60.0 && dt <= 1.0 / 30.0 + 0.001) step = float(dt * 0.5);
    memcpy(&bits, &step, 4);
    guest::w32(kStep, bits);
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
}
