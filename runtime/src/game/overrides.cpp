// Game-specific overrides of recompiled functions. The recompiler emits each function as
// `__imp__sub_X` plus a weak alias `sub_X`; a strong `PPC_FUNC(sub_X)` here replaces every direct
// call and the lookup-table entry, and can still run the original through `__imp__sub_X`.
// Fixes ported from Saints Reborn (same default.xex) - see docs/SAINTSREBORN_FINDINGS.md.
#include "../guest.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

extern "C" PPC_FUNC(__imp__sub_82105B18);
extern "C" PPC_FUNC(__imp__sub_8265F4F0);
extern "C" PPC_FUNC(__imp__sub_8262FFE0);
extern "C" PPC_FUNC(__imp__sub_82185498);

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
