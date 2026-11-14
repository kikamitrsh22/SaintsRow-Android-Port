// Vd* (video) imports.
#include "kernel.h"
#include "../gpu/gpu.h"
#include "../xglue/xglue.h"

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;

static void FillVideoMode(uint32_t m)
{
    std::memset(guest::ptr(m), 0, 0x30);
    guest::w32(m + 0x00, kWidth);
    guest::w32(m + 0x04, kHeight);
    guest::w32(m + 0x08, 0);    // interlaced
    guest::w32(m + 0x0C, 1);    // widescreen
    guest::w32(m + 0x10, 1);    // hi-def
    float hz = 60.0f;
    uint32_t bits;
    std::memcpy(&bits, &hz, 4);
    guest::w32(m + 0x14, bits); // refresh rate
    guest::w32(m + 0x18, 1);    // video standard NTSC
    guest::w32(m + 0x1C, 0x8A); // pixel rate
    guest::w32(m + 0x20, 0x01); // widescreen flag
}

GUEST_IMPORT(VdQueryVideoMode) { FillVideoMode(ctx.r3.u32); }
GUEST_IMPORT(VdQueryVideoFlags) { ctx.r3.u64 = 1 | 2; } // widescreen, >= 1280 wide

GUEST_IMPORT(VdGetCurrentDisplayInformation)
{
    uint32_t d = ctx.r3.u32;
    std::memset(guest::ptr(d), 0, 0x58);
    guest::w16(d + 0x00, kWidth);
    guest::w16(d + 0x02, kHeight);
    // scaler parameters (+0x08): source rect x2/y2, output size, filter types
    guest::w32(d + 0x08 + 0x08, kWidth);
    guest::w32(d + 0x08 + 0x0C, kHeight);
    guest::w32(d + 0x08 + 0x10, kWidth);
    guest::w32(d + 0x08 + 0x14, kHeight);
    guest::w32(d + 0x08 + 0x18, 1);
    guest::w32(d + 0x08 + 0x24, 1);
    guest::w16(d + 0x40, 320);
    guest::w16(d + 0x42, 180);
    guest::w16(d + 0x44, 320);
    guest::w16(d + 0x46, 180);
    guest::w16(d + 0x48, kWidth);
    guest::w16(d + 0x4A, kHeight);
    float hz = 60.0f;
    uint32_t bits;
    std::memcpy(&bits, &hz, 4);
    guest::w32(d + 0x4C, bits);
    guest::w16(d + 0x56, kWidth);
}

// VdGetCurrentDisplayGamma(pType, pPower)
GUEST_IMPORT(VdGetCurrentDisplayGamma)
{
    guest::w32(ctx.r3.u32, 2);
    float p = 2.22222233f;
    uint32_t bits;
    std::memcpy(&bits, &p, 4);
    guest::w32(ctx.r4.u32, bits);
}

GUEST_IMPORT(VdInitializeEngines) { ctx.r3.u64 = 1; }
GUEST_IMPORT(VdShutdownEngines) {}
GUEST_IMPORT(VdSetGraphicsInterruptCallback) { xglue::SetInterruptCallback(ctx.r3.u32, ctx.r4.u32); }
GUEST_IMPORT(VdInitializeRingBuffer) { xglue::InitializeRingBuffer(ctx.r3.u32, ctx.r4.u32); }
GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack) { xglue::EnableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32); }

GUEST_IMPORT(VdGetSystemCommandBuffer)
{
    std::memset(guest::ptr(ctx.r3.u32), 0, 0x94);
    guest::w32(ctx.r3.u32, 0xBEEF0000);
    guest::w32(ctx.r4.u32, 0xBEEF0001);
}

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) {}

// 12 args; dest pointer is the 11th (stack), count the 12th.
GUEST_IMPORT(VdInitializeScalerCommandBuffer)
{
    uint32_t dest = StackArg32(ctx, 2);
    uint32_t count = StackArg32(ctx, 3);
    LOG("VdInitializeScalerCommandBuffer: args9..12 = %08X %08X %08X %08X", StackArg32(ctx, 0), StackArg32(ctx, 1), dest, count);
    if (count > 0x10000 || !xglue::IsMapped(dest))
    {
        LOG("VdInitializeScalerCommandBuffer: unmapped destination, skipped");
        ctx.r3.u64 = count;
        return;
    }
    for (uint32_t i = 0; i < count; i++) guest::w32(dest + i * 4, 0x80000000);
    ctx.r3.u64 = count;
}

GUEST_IMPORT(VdRetrainEDRAM) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
GUEST_IMPORT(VdPersistDisplay) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdEnableDisableClockGating) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdSetDisplayMode) { ctx.r3.u64 = 0; }

// VdSwap(buffer, fetch, unk2, unk3, unk4, pFrontBuffer, pFormat, pColorSpace, pWidth, pHeight):
// the game reserves 64 dwords in the ring buffer for the swap packets.
GUEST_IMPORT(VdSwap)
{
    uint32_t pWidth = StackArg32(ctx, 0), pHeight = StackArg32(ctx, 1);
    uint32_t w = pWidth ? guest::r32(pWidth) : 0, h = pHeight ? guest::r32(pHeight) : 0;
    xglue::WriteSwapPackets(ctx.r3.u32, ctx.r4.u32, ctx.r8.u32 ? guest::r32(ctx.r8.u32) : 0, w, h);
    gpu::OnSwap();
    ctx.r3.u64 = 0;
}
