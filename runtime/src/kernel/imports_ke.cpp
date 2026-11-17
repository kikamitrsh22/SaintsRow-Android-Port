// Ke*/Kf*/Ex* thread + synchronisation imports, TLS, IRQL, spinlocks, critical sections.
#include "kernel.h"
#include "../memory.h"

#include <chrono>
#include <thread>

#include "../platform.h"

using namespace kernel;
using namespace std::chrono;

static std::vector<std::shared_ptr<Object>> GuestObjects(uint32_t count, uint32_t arrayAddr)
{
    std::vector<std::shared_ptr<Object>> objs;
    for (uint32_t i = 0; i < count; i++)
        objs.push_back(GetGuestObject(guest::r32(arrayAddr + i * 4)));
    return objs;
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

// ExCreateThread(pHandle, stackSize, pThreadId, xapiStartup, startAddress, startContext, flags)
GUEST_IMPORT(ExCreateThread)
{
    uint32_t pHandle = ctx.r3.u32, stackSize = ctx.r4.u32, pThreadId = ctx.r5.u32;
    uint32_t xapi = ctx.r6.u32, start = ctx.r7.u32, param = ctx.r8.u32, flags = ctx.r9.u32;
    auto t = CreateThread(start, param, xapi, stackSize ? stackSize : 0x10000, flags);
    if (pHandle) guest::w32(pHandle, t->handle);
    if (pThreadId) guest::w32(pThreadId, t->id);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(ExTerminateThread)
{
    throw ThreadExit{ ctx.r3.u32 };
}

GUEST_IMPORT(ExRegisterTitleTerminateNotification) { ctx.r3.u64 = 0; }

// KeResumeThread(KTHREAD*) -> previous suspend count
GUEST_IMPORT(KeResumeThread)
{
    auto t = GetGuestObjectAs<GuestThread>(ctx.r3.u32);
    int32_t prev = 0;
    if (t) { prev = t->suspendCount; ResumeThread(*t); }
    ctx.r3.u64 = uint32_t(prev);
}

// NtResumeThread(handle, pSuspendCount)
GUEST_IMPORT(NtResumeThread)
{
    auto t = GetHandleAs<GuestThread>(ctx.r3.u32);
    if (!t) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (ctx.r4.u32) guest::w32(ctx.r4.u32, uint32_t(t->suspendCount));
    ResumeThread(*t);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(KeDelayExecutionThread)
{
    // (mode, alertable, pInterval)
    int64_t interval = int64_t(guest::r64(ctx.r5.u32));
    if (interval < 0)
    {
        auto ns = nanoseconds(-interval * 100);
        if (ns.count() == 0) std::this_thread::yield();
        else std::this_thread::sleep_for(ns);
    }
    else
    {
        std::this_thread::yield();
    }
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(KeQueryBasePriorityThread) { ctx.r3.u64 = 0; }
GUEST_IMPORT(KeSetBasePriorityThread) { ctx.r3.u64 = 0; }
GUEST_IMPORT(KeSetDisableBoostThread) { ctx.r3.u64 = 0; }
// KeSetAffinityThread(KTHREAD*, affinity, pPreviousAffinity)
GUEST_IMPORT(KeSetAffinityThread)
{
    uint32_t affinity = ctx.r4.u32;
    if (!affinity) { ctx.r3.u64 = STATUS_INVALID_PARAMETER; return; }
    auto t = GetGuestObjectAs<GuestThread>(ctx.r3.u32);
    if (!t) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (ctx.r5.u32) guest::w32(ctx.r5.u32, 1u << t->cpu);
    SetActiveCpu(*t, CpuForAffinity(uint8_t(affinity)));
    ctx.r3.u64 = STATUS_SUCCESS;
}
GUEST_IMPORT(KeGetCurrentProcessType) { ctx.r3.u64 = 1; } // title process
GUEST_IMPORT(KeEnterCriticalRegion) {}
GUEST_IMPORT(KeLeaveCriticalRegion) {}
GUEST_IMPORT(KiApcNormalRoutineNop) {}

// ---------------------------------------------------------------------------
// Dispatcher objects living in guest memory
// ---------------------------------------------------------------------------

GUEST_IMPORT(KeInitializeSemaphore)
{
    // (sem, count, limit)
    uint32_t s = ctx.r3.u32;
    guest::w8(s + 0, 5);
    guest::w8(s + 2, 5); // size in dwords
    guest::w32(s + 4, ctx.r4.u32);
    guest::w32(s + 8, s + 8);  // empty wait list
    guest::w32(s + 12, s + 8);
    guest::w32(s + 0x10, ctx.r5.u32);
    int32_t count = int32_t(ctx.r4.u32), limit = int32_t(ctx.r5.u32);

    // The game re-initialises its IO semaphores in place while threads are already waiting on them.
    // Replacing the host object would orphan those waiters, so keep it and reset its state instead
    // (Saints Reborn: a re-init with count 0 gets 2 so the parked IO threads make progress).
    if (auto existing = std::dynamic_pointer_cast<Semaphore>(FindGuestObject(s)))
    {
        if (count == 0) count = 2;
        if (limit > 0 && count > limit) count = limit;
        std::lock_guard lock(g_lock);
        existing->limit = limit;
        existing->count = count;
        existing->NotifyWaiters();
        return;
    }
    RegisterGuestObject(s, std::make_shared<Semaphore>(count, limit));
}

GUEST_IMPORT(KeReleaseSemaphore)
{
    // (sem, increment, adjustment, wait) -> previous count
    auto s = GetGuestObjectAs<Semaphore>(ctx.r3.u32);
    ctx.r3.u64 = s ? uint32_t(ReleaseSemaphore(*s, int32_t(ctx.r5.u32), nullptr)) : 0;
}

GUEST_IMPORT(KeSetEvent)
{
    auto e = GetGuestObjectAs<Event>(ctx.r3.u32);
    ctx.r3.u64 = e ? SetEvent(*e) : 0;
}

GUEST_IMPORT(KeResetEvent)
{
    auto e = GetGuestObjectAs<Event>(ctx.r3.u32);
    ctx.r3.u64 = e ? ResetEvent(*e) : 0;
}

GUEST_IMPORT(KeWaitForSingleObject)
{
    // (object, reason, mode, alertable, pTimeout)
    auto obj = GetGuestObject(ctx.r3.u32);
    ctx.r3.u64 = Wait({ obj }, false, DeadlineFromGuest(ctx.r7.u32));
}

GUEST_IMPORT(KeWaitForMultipleObjects)
{
    // (count, objects[], waitType, reason, mode, alertable, pTimeout, waitBlocks)
    auto objs = GuestObjects(ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = Wait(objs, ctx.r5.u32 == 0, DeadlineFromGuest(ctx.r9.u32));
}

// ---------------------------------------------------------------------------
// TLS
// ---------------------------------------------------------------------------

static std::mutex s_tlsLock;
static std::vector<bool> s_tlsUsed;

GUEST_IMPORT(KeTlsAlloc)
{

    std::lock_guard lock(s_tlsLock);
    if (s_tlsUsed.empty()) s_tlsUsed.resize(g_tls.slotCount, false);
    for (uint32_t i = 0; i < s_tlsUsed.size(); i++)
    {
        if (!s_tlsUsed[i]) { s_tlsUsed[i] = true; ctx.r3.u64 = i; return; }
    }
    ctx.r3.u64 = 0xFFFFFFFF;
}

GUEST_IMPORT(KeTlsFree)
{
    std::lock_guard lock(s_tlsLock);
    if (ctx.r3.u32 < s_tlsUsed.size()) s_tlsUsed[ctx.r3.u32] = false;
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(KeTlsGetValue)
{
    auto* t = CurrentThread();
    uint32_t slot = ctx.r3.u32;
    uint32_t v = (t && slot < g_tls.slotCount) ? guest::r32(t->tlsDynamic + slot * 4) : 0;
    if (!v)
    {
        static std::atomic<int> s_logged{ 0 };
        if (s_logged++ < 20) LOG("KeTlsGetValue(slot %u) = 0 on thread %u (lr=%08X)", slot, t ? t->id : 0, uint32_t(ctx.lr));
    }
    ctx.r3.u64 = v;
}

GUEST_IMPORT(KeTlsSetValue)
{
    auto* t = CurrentThread();
    if (t && ctx.r3.u32 < g_tls.slotCount)
    {
        static std::atomic<int> s_logged{ 0 };
        if (s_logged++ < 60) LOG("KeTlsSetValue(slot %u, %08X) on thread %u", ctx.r3.u32, ctx.r4.u32, t->id);
        guest::w32(t->tlsDynamic + ctx.r3.u32 * 4, ctx.r4.u32);
        ctx.r3.u64 = 1;
    }
    else
    {
        ctx.r3.u64 = 0;
    }
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

static uint64_t MeasureTimebaseFrequency()
{
#if defined(__aarch64__)
    return HostTimebaseFrequency();
#else
    auto t0 = steady_clock::now();
    uint64_t c0 = HostTimebase();
    std::this_thread::sleep_for(milliseconds(50));
    uint64_t c1 = HostTimebase();
    auto t1 = steady_clock::now();
    double secs = duration<double>(t1 - t0).count();
    return uint64_t(double(c1 - c0) / secs);
#endif
}

// The recompiler turns mftb into the host cycle counter, so report that counter's frequency.
GUEST_IMPORT(KeQueryPerformanceFrequency)
{
    static uint64_t s_freq = MeasureTimebaseFrequency();
    ctx.r3.u64 = s_freq;
}

GUEST_IMPORT(KeQuerySystemTime)
{
    if (ctx.r3.u32) guest::w64(ctx.r3.u32, SystemTime());
}

// ---------------------------------------------------------------------------
// IRQL / spinlocks (no real IRQL on the host)
// ---------------------------------------------------------------------------

static void SpinAcquire(uint32_t lockAddr)
{
    auto* p = reinterpret_cast<std::atomic<uint32_t>*>(guest::ptr(lockAddr));
    auto* t = CurrentThread();
    uint32_t owner = __builtin_bswap32(t ? t->kthread : 1);
    uint32_t expected = 0;
    int spins = 0;
    while (!p->compare_exchange_weak(expected, owner, std::memory_order_acquire))
    {
        expected = 0;
        if (++spins > 64) std::this_thread::yield();
    }
}

static void SpinRelease(uint32_t lockAddr)
{
    reinterpret_cast<std::atomic<uint32_t>*>(guest::ptr(lockAddr))->store(0, std::memory_order_release);
}

GUEST_IMPORT(KeAcquireSpinLockAtRaisedIrql) { SpinAcquire(ctx.r3.u32); }
GUEST_IMPORT(KeReleaseSpinLockFromRaisedIrql) { SpinRelease(ctx.r3.u32); }
GUEST_IMPORT(KfAcquireSpinLock) { SpinAcquire(ctx.r3.u32); ctx.r3.u64 = 0; }
GUEST_IMPORT(KfReleaseSpinLock) { SpinRelease(ctx.r3.u32); }
GUEST_IMPORT(KeRaiseIrqlToDpcLevel) { ctx.r3.u64 = 0; }
GUEST_IMPORT(KfLowerIrql) {}
GUEST_IMPORT(KeLockL2) {}
GUEST_IMPORT(KeUnlockL2) {}

GUEST_IMPORT(KeBugCheck)
{
    LOG("KeBugCheck(%08X)", ctx.r3.u32);
    std::abort();
}

GUEST_IMPORT(KeBugCheckEx)
{
    LOG("KeBugCheckEx(%08X, %08X, %08X, %08X, %08X)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    std::abort();
}

// ---------------------------------------------------------------------------
// Critical sections (RTL_CRITICAL_SECTION in guest memory)
//   +0x00 DISPATCHER_HEADER (0x10)   +0x10 LockCount   +0x14 RecursionCount   +0x18 OwningThread
// ---------------------------------------------------------------------------

static void CsInit(uint32_t cs)
{
    std::memset(guest::ptr(cs), 0, 0x1C);
    guest::w8(cs, 1);
    guest::w32(cs + 0x10, 0xFFFFFFFF);
}

static uint32_t CurrentOwnerTag()
{
    auto* t = CurrentThread();
    return t ? t->kthread : 1;
}

static bool CsTryEnter(uint32_t cs)
{
    auto* owner = reinterpret_cast<std::atomic<uint32_t>*>(guest::ptr(cs + 0x18));
    uint32_t me = __builtin_bswap32(CurrentOwnerTag());
    uint32_t expected = 0;
    if (owner->compare_exchange_strong(expected, me, std::memory_order_acquire))
    {
        guest::w32(cs + 0x14, 1);
        guest::w32(cs + 0x10, 0);
        return true;
    }
    if (expected == me)
    {
        guest::w32(cs + 0x14, guest::r32(cs + 0x14) + 1);
        guest::w32(cs + 0x10, guest::r32(cs + 0x10) + 1);
        return true;
    }
    return false;
}

GUEST_IMPORT(RtlInitializeCriticalSection) { CsInit(ctx.r3.u32); }
GUEST_IMPORT(RtlInitializeCriticalSectionAndSpinCount) { CsInit(ctx.r3.u32); ctx.r3.u64 = STATUS_SUCCESS; }

GUEST_IMPORT(RtlEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    int spins = 0;
    while (!CsTryEnter(cs))
    {
        if (++spins < 256) continue;
        if (spins < 1024) std::this_thread::yield();
        else std::this_thread::sleep_for(microseconds(50));
    }
}

GUEST_IMPORT(RtlTryEnterCriticalSection)
{
    ctx.r3.u64 = CsTryEnter(ctx.r3.u32) ? 1 : 0;
}

GUEST_IMPORT(RtlLeaveCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t rec = guest::r32(cs + 0x14);
    if (rec > 1)
    {
        guest::w32(cs + 0x14, rec - 1);
        guest::w32(cs + 0x10, guest::r32(cs + 0x10) - 1);
        return;
    }
    guest::w32(cs + 0x14, 0);
    guest::w32(cs + 0x10, 0xFFFFFFFF);
    reinterpret_cast<std::atomic<uint32_t>*>(guest::ptr(cs + 0x18))->store(0, std::memory_order_release);
}
