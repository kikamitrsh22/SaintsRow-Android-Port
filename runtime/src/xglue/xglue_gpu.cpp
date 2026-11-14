// xglue, GPU half shared by all platforms: ring buffer setup and VdSwap packets.
#include "xglue.h"
#include "xglue_internal.h"

#include "xenia/base/logging.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/xenos.h"

namespace xglue
{
    xe::gpu::GraphicsSystem* g_gpu = nullptr;

    void InitializeRingBuffer(uint32_t ptr, uint32_t sizeLog2) { g_gpu->InitializeRingBuffer(ptr, sizeLog2); }
    void EnableReadPointerWriteBack(uint32_t ptr, uint32_t blockSizeLog2) { g_gpu->EnableReadPointerWriteBack(ptr, blockSizeLog2); }
    void SetInterruptCallback(uint32_t callback, uint32_t userData) { g_gpu->SetInterruptCallback(callback, userData); }

    // Same packet stream xenia's VdSwap writes: the front buffer texture fetch, then PM4_XE_SWAP.
    void WriteSwapPackets(uint32_t bufferPtr, uint32_t fetchPtr, uint32_t frontBufferPtr, uint32_t width, uint32_t height)
    {
        namespace xenos = xe::gpu::xenos;
        xenos::xe_gpu_texture_fetch_t fetch;
        xe::copy_and_swap_32_unaligned(&fetch, XMemory()->TranslateVirtual<uint32_t*>(fetchPtr), 6);

        uint32_t frontVirtual = fetch.base_address << 12;
        uint32_t frontPhysical = XMemory()->GetPhysicalAddress(frontVirtual);
        if (frontPhysical == UINT32_MAX)
        {
            XELOGE("VdSwap: invalid front buffer address {:08X} (game passed {:08X})", frontVirtual, frontBufferPtr);
            return;
        }
        fetch.base_address = frontPhysical >> 12;
        if (!width || !height)
        {
            width = fetch.size_2d.width + 1;
            height = fetch.size_2d.height + 1;
        }

        auto* dwords = XMemory()->TranslateVirtual<xe::be<uint32_t>*>(bufferPtr);
        uint32_t o = 0;
        dwords[o++] = xenos::MakePacketType0(xe::gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 6);
        dwords[o++] = fetch.dword_0;
        dwords[o++] = fetch.dword_1;
        dwords[o++] = fetch.dword_2;
        dwords[o++] = fetch.dword_3;
        dwords[o++] = fetch.dword_4;
        dwords[o++] = fetch.dword_5;
        dwords[o++] = xenos::MakePacketType3(xenos::PM4_XE_SWAP, 4);
        dwords[o++] = xenos::kSwapSignature;
        dwords[o++] = frontPhysical;
        dwords[o++] = width;
        dwords[o++] = height;
        while (o < 64) dwords[o++] = xenos::MakePacketType2();
    }
}
