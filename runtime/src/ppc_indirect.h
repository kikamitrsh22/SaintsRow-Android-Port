// Force-included into the recompiled code: indirect calls to addresses that have no recompiled
// function (null vtable slots, garbage function pointers) are logged and return 0 instead of
// jumping to null - the same policy as Saints Reborn's SDK. On the console such calls are bugs the
// game happens to survive; killing the process for them is worse than skipping them.
#pragma once

#include <cstdint>

// Note: mftb becomes __rdtsc(); ppc_context.h already provides a cntvct_el0 version on ARM64.

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
