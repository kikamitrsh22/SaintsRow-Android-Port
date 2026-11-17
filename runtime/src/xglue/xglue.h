// Bridge between the runtime and the Xenia libraries (memory, GPU, window).
// This header has no Xenia includes so the rest of the runtime stays independent of them.
#pragma once

#include <cstdint>
#include <functional>

namespace xglue
{
    // ---- memory (xe::Memory) ----------------------------------------------------
    // Creates the guest address space; returns the host pointer of guest address 0.
    uint8_t* InitMemory();

    // Allocations from Xenia's heaps (page granular, zero-filled).
    uint32_t AllocVirtual(uint32_t size, uint32_t alignment);  // 0x40000000 heap, 64 KB pages
    uint32_t AllocPhysical(uint32_t size, uint32_t alignment); // 0xA0000000 heap, 64 KB pages
    bool Free(uint32_t address);
    bool AllocFixed(uint32_t address, uint32_t size);          // reserve + commit read/write
    bool CommitRange(uint32_t address, uint32_t size);         // commit (reserving if needed)
    bool IsMapped(uint32_t address);
    uint32_t GetPhysicalAddress(uint32_t address);

    // Kernel memory services, ported from Xenia's xboxkrnl_memory.cc. Return NTSTATUS.
    uint32_t NtAllocateVirtualMemory(uint32_t& base, uint32_t& size, uint32_t allocType, uint32_t protectBits);
    uint32_t NtFreeVirtualMemory(uint32_t& base, uint32_t& size, uint32_t freeType);
    uint32_t NtQueryVirtualMemory(uint32_t address, uint32_t info[7]); // MEMORY_BASIC_INFORMATION fields
    uint32_t MmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t size, uint32_t protectBits, uint32_t minAddress,
        uint32_t maxAddress, uint32_t alignment); // returns address or 0
    void MmFreePhysicalMemory(uint32_t address);
    void MmSetAddressProtect(uint32_t address, uint32_t size, uint32_t protectBits);

    // ---- window + GPU (VulkanGraphicsSystem) ----------------------------------
    // Called (from a GPU thread) when the game's graphics interrupt callback should run.
    using InterruptHook = std::function<void(uint32_t callback, uint32_t userData, uint32_t source, uint32_t cpu)>;

    // Must run on the main (UI) thread.
    bool InitGraphics(InterruptHook hook);

#if defined(__ANDROID__)
    // Android: the Java side hands over the Surface's ANativeWindow (ANativeWindow*).
    void SetNativeWindow(void* nativeWindow);
    void OnSurfaceChanged();
    void OnSurfaceDestroyed();
#endif
    int RunUiLoop();
    void RequestQuit();

    void InitializeRingBuffer(uint32_t ptr, uint32_t sizeLog2);
    void EnableReadPointerWriteBack(uint32_t ptr, uint32_t blockSizeLog2);
    void SetInterruptCallback(uint32_t callback, uint32_t userData);

    // VdSwap: writes the swap packets into the 64 dwords the game reserved in the ring buffer.
    void WriteSwapPackets(uint32_t bufferPtr, uint32_t fetchPtr, uint32_t frontBufferPtr, uint32_t width, uint32_t height);
    // Android: optional TOML file with Xenia GPU settings, read by InitGraphics.
    void SetGpuConfigFile(const char* path);
    // Android: the app's native library folder (adrenotools hooks) and an optional custom Vulkan
    // driver library ("" = system driver). Call before InitGraphics.
    void SetCustomDriver(const char* nativeLibDir, const char* driverLibrary);
}
