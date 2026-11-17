// xglue, memory half: xe::Memory and the kernel memory services (portable).
#include "xglue.h"
#include "xglue_internal.h"

#include <algorithm>
#include <cstdio>
#include <memory>

#include "xenia/base/cvar.h"
#include "xenia/memory.h"

DECLARE_bool(protect_zero);

namespace xglue
{
    static std::unique_ptr<xe::Memory> s_memory;
    xe::Memory* XMemory() { return s_memory.get(); }

    // ------------------------------------------------------------------------
    // Memory
    // ------------------------------------------------------------------------

    uint8_t* InitMemory()
    {
        // TODO: xe::InitializeLogging fail-fasts when called from here; Xenia logs are off for now.
        std::fputs("[xglue] memory\n", stderr);
        // Saints Row reads the zero page (Xenia compatibility note: requires protect_zero=false).
        cvars::protect_zero = false;
        s_memory = std::make_unique<xe::Memory>();
        if (!s_memory->Initialize())
            return nullptr;
        // Like Saints Reborn: guest 0x00000000-0x00FFFFFF is zeroed read/write memory, so reads
        // through null pointers with large offsets return 0 instead of faulting. Xenia's
        // protect_zero=false only maps the first 64 KB.
        if (auto* low = s_memory->LookupHeap(0x10000))
            low->AllocFixed(0x10000, 0x01000000 - 0x10000, 0x1000,
                xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
                xe::kMemoryProtectRead | xe::kMemoryProtectWrite);
        std::fputs("[xglue] memory ready\n", stderr);
        return s_memory->virtual_membase();
    }

    static uint32_t AllocFrom(uint32_t heapBase, uint32_t size, uint32_t alignment)
    {
        auto* heap = s_memory->LookupHeap(heapBase);
        uint32_t page = heap->page_size();
        uint32_t align = std::max(alignment, page);
        uint32_t rounded = (size + page - 1) & ~(page - 1);
        uint32_t address = 0;
        if (!heap->Alloc(rounded, align, xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
                xe::kMemoryProtectRead | xe::kMemoryProtectWrite, false, &address))
            return 0;
        return address;
    }

    uint32_t AllocVirtual(uint32_t size, uint32_t alignment) { return AllocFrom(0x40000000, size, alignment); }
    uint32_t AllocPhysical(uint32_t size, uint32_t alignment) { return AllocFrom(0xA0000000, size, alignment); }

    bool Free(uint32_t address)
    {
        auto* heap = s_memory->LookupHeap(address);
        return heap && heap->Release(address);
    }

    bool AllocFixed(uint32_t address, uint32_t size)
    {
        auto* heap = s_memory->LookupHeap(address);
        if (!heap) return false;
        uint32_t page = heap->page_size();
        uint32_t base = address & ~(page - 1);
        uint32_t rounded = ((address + size - base) + page - 1) & ~(page - 1);
        return heap->AllocFixed(base, rounded, page, xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
            xe::kMemoryProtectRead | xe::kMemoryProtectWrite);
    }

    bool CommitRange(uint32_t address, uint32_t size)
    {
        auto* heap = s_memory->LookupHeap(address);
        if (!heap) return false;
        uint32_t page = heap->page_size();
        uint32_t base = address & ~(page - 1);
        uint32_t rounded = ((address + size - base) + page - 1) & ~(page - 1);
        // Commit inside an existing reservation first; otherwise reserve and commit.
        if (heap->AllocFixed(base, rounded, page, xe::kMemoryAllocationCommit,
                xe::kMemoryProtectRead | xe::kMemoryProtectWrite))
            return true;
        return heap->AllocFixed(base, rounded, page, xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
            xe::kMemoryProtectRead | xe::kMemoryProtectWrite);
    }

    bool IsMapped(uint32_t address)
    {
        auto* heap = s_memory->LookupHeap(address);
        uint32_t protect = 0;
        return heap && heap->QueryProtect(address, &protect) && protect != 0;
    }

    uint32_t GetPhysicalAddress(uint32_t address) { return s_memory->GetPhysicalAddress(address); }

    // ------------------------------------------------------------------------
    // Kernel memory services (port of xenia/kernel/xboxkrnl/xboxkrnl_memory.cc)
    // ------------------------------------------------------------------------

    enum : uint32_t
    {
        X_MEM_COMMIT = 0x00001000, X_MEM_RESERVE = 0x00002000, X_MEM_DECOMMIT = 0x00004000,
        X_MEM_RELEASE = 0x00008000, X_MEM_RESET = 0x00080000, X_MEM_TOP_DOWN = 0x00100000,
        X_MEM_NOZERO = 0x00800000, X_MEM_LARGE_PAGES = 0x20000000, X_MEM_16MB_PAGES = 0x80000000,
        X_MEM_FREE = 0x00010000, X_MEM_PRIVATE = 0x00020000,
        X_PAGE_NOACCESS = 0x01, X_PAGE_READONLY = 0x02, X_PAGE_READWRITE = 0x04,
        X_PAGE_EXECUTE = 0x10, X_PAGE_EXECUTE_READ = 0x20, X_PAGE_EXECUTE_READWRITE = 0x40,
        X_PAGE_NOCACHE = 0x200, X_PAGE_WRITECOMBINE = 0x400,
        X_STATUS_SUCCESS_ = 0, X_STATUS_UNSUCCESSFUL_ = 0xC0000001, X_STATUS_INVALID_PARAMETER_ = 0xC000000D,
        X_STATUS_NO_MEMORY_ = 0xC0000017, X_STATUS_ACCESS_DENIED_ = 0xC0000022,
        X_STATUS_MEMORY_NOT_ALLOCATED_ = 0xC00000A0,
    };

    static uint32_t ToXdkProtect(uint32_t protect)
    {
        uint32_t r;
        if (!(protect & xe::kMemoryProtectRead) && !(protect & xe::kMemoryProtectWrite)) r = X_PAGE_NOACCESS;
        else if ((protect & xe::kMemoryProtectRead) && !(protect & xe::kMemoryProtectWrite)) r = X_PAGE_READONLY;
        else r = X_PAGE_READWRITE;
        if (protect & xe::kMemoryProtectNoCache) r |= X_PAGE_NOCACHE;
        if (protect & xe::kMemoryProtectWriteCombine) r |= X_PAGE_WRITECOMBINE;
        return r;
    }

    static uint32_t FromXdkProtect(uint32_t protect)
    {
        uint32_t r = 0;
        if ((protect & X_PAGE_READONLY) | (protect & X_PAGE_EXECUTE_READ)) r = xe::kMemoryProtectRead;
        else if ((protect & X_PAGE_READWRITE) | (protect & X_PAGE_EXECUTE_READWRITE))
            r = xe::kMemoryProtectRead | xe::kMemoryProtectWrite;
        if (protect & X_PAGE_NOCACHE) r |= xe::kMemoryProtectNoCache;
        if (protect & X_PAGE_WRITECOMBINE) r |= xe::kMemoryProtectWriteCombine;
        return r;
    }

    static uint32_t RoundUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

    uint32_t NtAllocateVirtualMemory(uint32_t& baseRef, uint32_t& sizeRef, uint32_t allocType, uint32_t protectBits)
    {
        if (!sizeRef) return X_STATUS_INVALID_PARAMETER_;
        if (!(allocType & (X_MEM_COMMIT | X_MEM_RESET | X_MEM_RESERVE))) return X_STATUS_INVALID_PARAMETER_;
        if ((allocType & X_MEM_RESET) && (allocType & ~X_MEM_RESET)) return X_STATUS_INVALID_PARAMETER_;

        uint32_t pageSize;
        if (baseRef)
        {
            auto* heap = s_memory->LookupHeap(baseRef);
            if (!heap || heap->heap_type() != xe::HeapType::kGuestVirtual) return X_STATUS_INVALID_PARAMETER_;
            pageSize = heap->page_size();
        }
        else
        {
            pageSize = (allocType & X_MEM_LARGE_PAGES) ? 64 * 1024 : 4 * 1024;
        }

        uint32_t adjustedBase = baseRef - (baseRef % pageSize);
        uint32_t adjustedSize = int32_t(sizeRef) < 0 ? uint32_t(-int32_t(sizeRef)) : sizeRef;
        adjustedSize = RoundUp(adjustedSize, adjustedBase ? pageSize : 64 * 1024);

        uint32_t allocationType = 0;
        if (allocType & X_MEM_RESERVE) allocationType |= xe::kMemoryAllocationReserve;
        if (allocType & X_MEM_COMMIT) allocationType |= xe::kMemoryAllocationCommit;
        if (allocType & X_MEM_RESET) return X_STATUS_SUCCESS_; // MEM_RESET: contents may be discarded

        uint32_t protect = FromXdkProtect(protectBits);
        uint32_t address = 0;
        xe::BaseHeap* heap;
        bool wasCommitted = false;
        if (adjustedBase)
        {
            heap = s_memory->LookupHeap(adjustedBase);
            if (heap->page_size() != pageSize) return X_STATUS_ACCESS_DENIED_;
            xe::HeapAllocationInfo prev = {};
            wasCommitted = heap->QueryRegionInfo(adjustedBase, &prev) && (prev.state & xe::kMemoryAllocationCommit);
            if (heap->AllocFixed(adjustedBase, adjustedSize, pageSize, allocationType, protect))
                address = adjustedBase;
        }
        else
        {
            heap = s_memory->LookupHeapByType(false, pageSize);
            heap->Alloc(adjustedSize, pageSize, allocationType, protect, (allocType & X_MEM_TOP_DOWN) != 0, &address);
        }
        if (!address) return X_STATUS_NO_MEMORY_;

        if (!(allocType & X_MEM_NOZERO) && (allocType & X_MEM_COMMIT))
        {
            if (!(protect & xe::kMemoryProtectWrite))
                heap->Protect(address, adjustedSize, xe::kMemoryProtectRead | xe::kMemoryProtectWrite);
            if (!wasCommitted) s_memory->Zero(address, adjustedSize);
            if (!(protect & xe::kMemoryProtectWrite)) heap->Protect(address, adjustedSize, protect);
        }

        baseRef = address;
        sizeRef = adjustedSize;
        return X_STATUS_SUCCESS_;
    }

    uint32_t NtFreeVirtualMemory(uint32_t& baseRef, uint32_t& sizeRef, uint32_t freeType)
    {
        if (!baseRef) return X_STATUS_MEMORY_NOT_ALLOCATED_;
        auto* heap = s_memory->LookupHeap(baseRef);
        if (!heap || heap->heap_type() != xe::HeapType::kGuestVirtual) return X_STATUS_INVALID_PARAMETER_;
        uint32_t size = sizeRef;
        bool ok;
        if (freeType == X_MEM_DECOMMIT)
        {
            size = RoundUp(size, heap->page_size());
            ok = heap->Decommit(baseRef, size);
        }
        else
        {
            ok = heap->Release(baseRef, &size);
        }
        if (!ok) return X_STATUS_UNSUCCESSFUL_;
        sizeRef = size;
        return X_STATUS_SUCCESS_;
    }

    uint32_t NtQueryVirtualMemory(uint32_t address, uint32_t info[7])
    {
        auto* heap = s_memory->LookupHeap(address);
        xe::HeapAllocationInfo a;
        if (!heap || !heap->QueryRegionInfo(address, &a)) return X_STATUS_INVALID_PARAMETER_;
        info[0] = a.base_address;
        info[1] = a.allocation_base;
        info[2] = ToXdkProtect(a.allocation_protect);
        info[3] = a.region_size;
        info[4] = (a.state & xe::kMemoryAllocationCommit) ? X_MEM_COMMIT
                : (a.state & xe::kMemoryAllocationReserve) ? X_MEM_RESERVE : X_MEM_FREE;
        info[5] = ToXdkProtect(a.protect);
        info[6] = X_MEM_PRIVATE;
        return X_STATUS_SUCCESS_;
    }

    uint32_t MmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t size, uint32_t protectBits, uint32_t minAddress,
        uint32_t maxAddress, uint32_t alignment)
    {
        if (!(protectBits & (X_PAGE_READONLY | X_PAGE_READWRITE | X_PAGE_EXECUTE_READ | X_PAGE_EXECUTE_READWRITE)))
            protectBits |= X_PAGE_READWRITE;

        // 4 KB-page requests would land in the 0xE0000000 heap, whose host mapping is shifted by
        // 0x1000 on Windows; recompiled code doesn't apply that shift, so use 64 KB pages instead.
        uint32_t pageSize = (protectBits & X_MEM_16MB_PAGES) ? 16 * 1024 * 1024 : 64 * 1024;
        uint32_t adjustedSize = RoundUp(size, pageSize);
        uint32_t adjustedAlignment = RoundUp(std::max<uint32_t>(alignment, 1), pageSize);
        uint32_t protect = FromXdkProtect(protectBits);

        auto* heap = static_cast<xe::PhysicalHeap*>(s_memory->LookupHeapByType(true, pageSize));
        uint32_t heapBase = heap->heap_base();
        uint32_t physOffset = heap->GetPhysicalAddress(heapBase);
        uint32_t heapMin = minAddress > physOffset ? minAddress - physOffset : 0;
        uint32_t heapMax = maxAddress > physOffset ? maxAddress - physOffset : 0;
        uint32_t heapSize = heap->heap_size();
        heapMin = heapBase + std::min(heapMin, heapSize - 1);
        heapMax = heapBase + std::min(heapMax, heapSize - 1);

        uint32_t address = 0;
        if (!heap->AllocRange(heapMin, heapMax, adjustedSize, adjustedAlignment,
                xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit, protect, true, &address))
            return 0;
        return address;
    }

    void MmFreePhysicalMemory(uint32_t address)
    {
        if (auto* heap = s_memory->LookupHeap(address)) heap->Release(address);
    }

    void MmSetAddressProtect(uint32_t address, uint32_t size, uint32_t protectBits)
    {
        auto* heap = s_memory->LookupHeap(address);
        if (!heap || heap->heap_type() == xe::HeapType::kGuestXex) return;
        heap->Protect(address, size, FromXdkProtect(protectBits));
    }

}
