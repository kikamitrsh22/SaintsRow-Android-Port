// Guest address space, backed by Xenia's xe::Memory (physical aliasing, page watches, MMIO).
#pragma once

#include <cstdint>
#include <map>
#include <mutex>

// Guest memory map (xe::Memory layout):
//   0x40000000 - 0x7EFFFFFF  virtual heap, 64 KB pages (NtAllocateVirtualMemory, thread stacks)
//   0x7F000000 - 0x7FC7FFFF  GPU writeback
//   0x7FC80000 - 0x7FFFFFFF  GPU registers (MMIO, trapped by Xenia)
//   0x82000000 - ...         XEX image + recompiled function lookup table
//   0x90000000 - 0x93FFFFFF  system heap block (kernel objects, PCR, TLS, XamAlloc)
//   0xA0000000 - 0xBFFFFFFF  physical memory, 64 KB pages (MmAllocatePhysicalMemoryEx)
namespace mem
{
    // Sub-allocator over a fixed, committed block (small kernel objects).
    class Heap
    {
    public:
        void Init(uint32_t base, uint32_t size, const char* name);
        uint32_t Alloc(uint32_t size, uint32_t align = 16);
        void Free(uint32_t addr);
        uint32_t SizeOf(uint32_t addr);
        bool Contains(uint32_t addr) const { return addr >= base_ && addr - base_ < size_; }
        uint32_t Used() const { return used_; }

    private:
        std::mutex mutex_;
        uint32_t base_ = 0;
        uint32_t size_ = 0;
        uint32_t used_ = 0;
        const char* name_ = "";
        std::map<uint32_t, uint32_t> free_;  // addr -> size
        std::map<uint32_t, uint32_t> alloc_; // addr -> size
    };

    // Page-granular allocations straight from one of Xenia's heaps.
    class PageHeap
    {
    public:
        PageHeap(uint32_t lo, uint32_t hi, bool physical) : lo_(lo), hi_(hi), physical_(physical) {}
        uint32_t Alloc(uint32_t size, uint32_t align = 0x10000);
        void Free(uint32_t addr);
        bool Contains(uint32_t addr) const { return addr >= lo_ && addr < hi_; }

    private:
        uint32_t lo_, hi_;
        bool physical_;
    };

    extern PageHeap g_virtual;
    extern Heap g_system;
    extern PageHeap g_physical;

    bool Init(); // creates the guest address space, sets guest::g_base
    uint32_t SystemAllocZeroed(uint32_t size, uint32_t align = 16);
}
