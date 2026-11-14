#include "memory.h"
#include "guest.h"
#include "xglue/xglue.h"

namespace guest
{
    uint8_t* g_base = nullptr;
}

namespace mem
{
    PageHeap g_virtual(0x40000000, 0x7F000000, false);
    Heap g_system;
    PageHeap g_physical(0xA0000000, 0xC0000000, true);

    void Heap::Init(uint32_t base, uint32_t size, const char* name)
    {
        base_ = base;
        size_ = size;
        name_ = name;
        free_.clear();
        alloc_.clear();
        free_[base] = size;
    }

    uint32_t Heap::Alloc(uint32_t size, uint32_t align)
    {
        if (size == 0) size = 1;
        if (align < 16) align = 16;
        size = (size + 15) & ~15u;

        std::lock_guard lock(mutex_);
        for (auto it = free_.begin(); it != free_.end(); ++it)
        {
            uint32_t blockAddr = it->first;
            uint32_t blockSize = it->second;
            uint32_t aligned = (blockAddr + align - 1) & ~(align - 1);
            uint32_t pad = aligned - blockAddr;
            if (pad > blockSize || blockSize - pad < size)
                continue;

            free_.erase(it);
            if (pad)
                free_[blockAddr] = pad;
            uint32_t tail = blockSize - pad - size;
            if (tail)
                free_[aligned + size] = tail;

            alloc_[aligned] = size;
            used_ += size;
            std::memset(guest::ptr(aligned), 0, size);
            return aligned;
        }

        LOG("HEAP %s: out of memory (size=0x%X, used=0x%X)", name_, size, used_);
        return 0;
    }

    void Heap::Free(uint32_t addr)
    {
        std::lock_guard lock(mutex_);
        auto it = alloc_.find(addr);
        if (it == alloc_.end())
            return;
        uint32_t size = it->second;
        alloc_.erase(it);
        used_ -= size;

        auto [ins, ok] = free_.emplace(addr, size);
        // merge with next
        auto next = std::next(ins);
        if (next != free_.end() && ins->first + ins->second == next->first)
        {
            ins->second += next->second;
            free_.erase(next);
        }
        // merge with previous
        if (ins != free_.begin())
        {
            auto prev = std::prev(ins);
            if (prev->first + prev->second == ins->first)
            {
                prev->second += ins->second;
                free_.erase(ins);
            }
        }
    }

    uint32_t Heap::SizeOf(uint32_t addr)
    {
        std::lock_guard lock(mutex_);
        auto it = alloc_.find(addr);
        return it == alloc_.end() ? 0 : it->second;
    }

    uint32_t PageHeap::Alloc(uint32_t size, uint32_t align)
    {
        uint32_t a = physical_ ? xglue::AllocPhysical(size, align) : xglue::AllocVirtual(size, align);
        if (!a) LOG("PageHeap: out of memory (size=0x%X, %s)", size, physical_ ? "physical" : "virtual");
        return a;
    }

    void PageHeap::Free(uint32_t addr) { xglue::Free(addr); }

    bool Init()
    {
        guest::g_base = xglue::InitMemory();
        if (!guest::g_base)
            return false;
        // One committed block for small kernel objects.
        constexpr uint32_t kSystemBase = 0x90000000, kSystemSize = 0x04000000;
        if (!xglue::AllocFixed(kSystemBase, kSystemSize))
            return false;
        g_system.Init(kSystemBase, kSystemSize, "system");
        return true;
    }

    uint32_t SystemAllocZeroed(uint32_t size, uint32_t align)
    {
        return g_system.Alloc(size, align); // Alloc always zero-fills
    }
}
