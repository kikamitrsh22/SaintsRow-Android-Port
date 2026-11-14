// Shared state between the xglue translation units (Xenia headers allowed here).
#pragma once

#include "xenia/gpu/graphics_system.h"
#include "xenia/memory.h"

namespace xglue
{
    xe::Memory* XMemory();
    // Set by the platform file once the VulkanGraphicsSystem exists.
    extern xe::gpu::GraphicsSystem* g_gpu;
}
