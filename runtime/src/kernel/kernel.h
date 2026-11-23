// Host-side kernel objects (events, semaphores, mutants, threads, files) and handle table.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "../guest.h"

namespace kernel
{
    // One global lock + condition variable for every dispatcher object.
    // Simple and correct; good enough until profiling says otherwise.
    extern std::mutex g_lock;
    extern std::condition_variable_any g_cv;

    struct GuestThread;

    // A thread blocked in Wait(): woken only by the objects it waits on (with g_lock held).
    struct Waiter
    {
        std::condition_variable cv;
    };

    struct Object
    {
        virtual ~Object() = default;
        // Threads currently blocked on this object (g_lock).
        std::vector<Waiter*> waiters;
        // Wake the threads waiting on this object. Call with g_lock held after changing its state.
        void NotifyWaiters()
        {
            for (Waiter* w : waiters) w->cv.notify_one();
        }
        // Called with g_lock held.
        virtual bool IsSignaled(GuestThread*) const { return false; }
        virtual void OnAcquire(GuestThread*) {}
        virtual bool IsWaitable() const { return true; }
    };

    struct Event : Object
    {
        bool manualReset;
        bool signaled;
        Event(bool manual, bool initial) : manualReset(manual), signaled(initial) {}
        bool IsSignaled(GuestThread*) const override { return signaled; }
        void OnAcquire(GuestThread*) override { if (!manualReset) signaled = false; }
    };

    struct Semaphore : Object
    {
        int32_t count;
        int32_t limit;
        Semaphore(int32_t c, int32_t l) : count(c), limit(l) {}
        bool IsSignaled(GuestThread*) const override { return count > 0; }
        void OnAcquire(GuestThread*) override { --count; }
    };

    struct Mutant : Object
    {
        GuestThread* owner = nullptr;
        uint32_t recursion = 0;
        bool IsSignaled(GuestThread* t) const override { return owner == nullptr || owner == t; }
        void OnAcquire(GuestThread* t) override { owner = t; ++recursion; }
    };

    struct GuestThread : Object
    {
        uint32_t id = 0;
        uint32_t handle = 0;
        uint32_t kthread = 0;     // guest KTHREAD
        uint32_t pcr = 0;         // guest KPCR (r13)
        uint32_t stackBase = 0;   // high address
        uint32_t stackLimit = 0;  // low address
        uint32_t tlsStatic = 0;
        uint32_t tlsDynamic = 0;
        uint32_t startAddress = 0;
        uint32_t startContext = 0;
        uint32_t xapiStartup = 0;
        PPCContext* ctx = nullptr;
        int32_t suspendCount = 0;
        uint8_t cpu = 0; // guest hardware thread 0..5 (PCR+0x10C)
        std::atomic<uint32_t> waitingSince{ 0 }; // ms timestamp when the current wait began, 0 = running
        std::atomic<uint32_t> waitCount{ 0 };
        bool exited = false;
        uint32_t exitCode = 0;
        std::thread host;

        bool IsSignaled(GuestThread*) const override { return exited; }
    };

    // Thrown by ExTerminateThread to unwind back to the thread entry.
    struct ThreadExit { uint32_t code; };

    // ---- handles --------------------------------------------------------------
    constexpr uint32_t kCurrentProcess = 0xFFFFFFFF;
    constexpr uint32_t kCurrentThread = 0xFFFFFFFE;

    uint32_t CreateHandle(std::shared_ptr<Object> obj);
    std::shared_ptr<Object> GetHandle(uint32_t handle);
    template <typename T> std::shared_ptr<T> GetHandleAs(uint32_t handle)
    {
        return std::dynamic_pointer_cast<T>(GetHandle(handle));
    }
    bool CloseHandle(uint32_t handle);
    uint32_t DuplicateHandle(uint32_t handle);

    // ---- guest dispatcher objects (KEVENT etc. living in guest memory) --------
    // Creates the host object on first use from the DISPATCHER_HEADER.
    // File I/O done for the guest (all threads), for the stutter log (gpu.cpp).
    struct IoStats
    {
        std::atomic<uint64_t> opens{ 0 }, openUs{ 0 }, reads{ 0 }, readBytes{ 0 }, readUs{ 0 };
    };
    extern IoStats g_io;

    std::shared_ptr<Object> GetGuestObject(uint32_t addr);
    void RegisterGuestObject(uint32_t addr, std::shared_ptr<Object> obj);
    void ForgetGuestObject(uint32_t addr);
    // Existing host object for a guest address, or null (never creates one).
    std::shared_ptr<Object> FindGuestObject(uint32_t addr);
    template <typename T> std::shared_ptr<T> GetGuestObjectAs(uint32_t addr)
    {
        return std::dynamic_pointer_cast<T>(GetGuestObject(addr));
    }

    // ---- waiting ---------------------------------------------------------------
    // timeoutAddr: guest pointer to a 64-bit LARGE_INTEGER (100 ns units), 0 = infinite.
    std::optional<std::chrono::steady_clock::time_point> DeadlineFromGuest(uint32_t timeoutAddr);
    uint32_t Wait(const std::vector<std::shared_ptr<Object>>& objects, bool waitAll,
        std::optional<std::chrono::steady_clock::time_point> deadline);

    // Signal helpers (take g_lock).
    uint32_t SetEvent(Event& e);
    uint32_t ResetEvent(Event& e);
    uint32_t PulseEvent(Event& e);
    int32_t ReleaseSemaphore(Semaphore& s, int32_t count, uint32_t* status);
    int32_t ReleaseMutant(Mutant& m, GuestThread* t, uint32_t* status);

    // ---- threads ---------------------------------------------------------------
    GuestThread* CurrentThread();
    std::shared_ptr<GuestThread> CreateThread(uint32_t startAddress, uint32_t startContext,
        uint32_t xapiStartup, uint32_t stackSize, uint32_t creationFlags);
    // Moves a thread to guest hardware thread `cpu` (0..5), like Xenia's XThread::SetActiveCpu.
    void SetActiveCpu(GuestThread& t, uint8_t cpu);
    // Picks the guest CPU for a processor mask (0 = inherit the creating thread's CPU).
    uint8_t CpuForAffinity(uint8_t mask);
    void RunMainThread(uint32_t entryPoint, uint32_t stackSize);
    // A guest context driven by a host thread (GPU interrupts, audio callbacks).
    // Call BindCurrentThread() on the host thread before running guest code with it.
    std::shared_ptr<GuestThread> CreateHostContext(const char* name);
    void BindCurrentThread(GuestThread* t);
    void ResumeThread(GuestThread& t);
    void SuspendThread(GuestThread& t);

    // ---- TLS info from the XEX (set by the loader) ----------------------------
    struct TlsInfo { uint32_t slotCount = 64; uint32_t rawDataAddress = 0; uint32_t dataSize = 0; uint32_t rawDataSize = 0; };
    extern TlsInfo g_tls;

    // ---- diagnostics ----------------------------------------------------------
    void StartMonitor();

    // ---- time -----------------------------------------------------------------
    uint64_t SystemTime(); // FILETIME (100 ns since 1601)
    void StartTimeStampThread(uint32_t bundleAddr);
}
