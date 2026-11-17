// Force-included into the recompiled code: indirect calls to addresses that have no recompiled
// function (null vtable slots, garbage function pointers) are logged and return 0 instead of
// jumping to null - the same policy as Saints Reborn's SDK. On the console such calls are bugs the
// game happens to survive; killing the process for them is worse than skipping them.
#pragma once

#include <cstdint>

// mftb: the Xbox 360 timebase ticks at a fixed 49.875 MHz and the game relies on that (its frame
// time came out at 0.385x real time with the raw 19.2 MHz ARM counter: slow motion). The recompiler
// emits PPC_QUERY_TIMEBASE() for mftb; it scales the host counter to 49.875 MHz with the 32.32
// fixed-point factor the runtime sets up at start (InitGuestTimebase, timebase.cpp).
#if defined(__aarch64__) || defined(_M_ARM64)
#elif defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

extern "C" uint64_t g_guestTimebaseMul;

inline uint64_t PPCGuestTimebase()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    uint64_t host;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(host));
#else
    uint64_t host = __rdtsc();
#endif
    return uint64_t((unsigned __int128)host * g_guestTimebaseMul >> 32);
}
#define PPC_QUERY_TIMEBASE() PPCGuestTimebase()

extern "C" void PPCIndirectCallFailed(unsigned int target, unsigned int lr);

#define PPC_CALL_INDIRECT_FUNC(x)                                         \
    do                                                                    \
    {                                                                     \
        uint32_t ppcTarget = uint32_t(x);                                 \
        PPCFunc* ppcIndirectFn = nullptr;                                 \
        if (ppcTarget - PPC_CODE_BASE < PPC_CODE_SIZE) [[likely]]         \
            ppcIndirectFn = PPC_LOOKUP_FUNC(base, ppcTarget);             \
        if (ppcIndirectFn) [[likely]]                                     \
        {                                                                 \
            ppcIndirectFn(ctx, base);                                     \
        }                                                                 \
        else                                                              \
        {                                                                 \
            PPCIndirectCallFailed(ppcTarget, uint32_t(ctx.lr));           \
            ctx.r3.u64 = 0;                                               \
        }                                                                 \
    } while (0)
