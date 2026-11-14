// Force-included into the recompiled code: reports indirect calls to addresses that have no
// recompiled function instead of jumping to null.
#pragma once

#include <cstdint>

// Note: mftb becomes __rdtsc(); ppc_context.h already provides a cntvct_el0 version on ARM64.

extern "C" [[noreturn]] void PPCIndirectCallFailed(unsigned int target, unsigned int lr);

#define PPC_CALL_INDIRECT_FUNC(x)                                         \
    do                                                                    \
    {                                                                     \
        uint32_t ppcTarget = uint32_t(x);                                 \
        if (ppcTarget - PPC_CODE_BASE >= PPC_CODE_SIZE) [[unlikely]]      \
            PPCIndirectCallFailed(ppcTarget, uint32_t(ctx.lr));           \
        auto* ppcIndirectFn = PPC_LOOKUP_FUNC(base, ppcTarget);           \
        if (!ppcIndirectFn) [[unlikely]]                                  \
            PPCIndirectCallFailed(ppcTarget, uint32_t(ctx.lr));           \
        ppcIndirectFn(ctx, base);                                         \
    } while (0)
