#include "kernel.h"
#include "../memory.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>

using namespace std::chrono;

namespace kernel
{
    std::mutex g_lock;
    std::condition_variable_any g_cv;
    TlsInfo g_tls;

    // ------------------------------------------------------------------------
    // Handles
    // ------------------------------------------------------------------------

    static std::mutex s_handleLock;
    static std::unordered_map<uint32_t, std::shared_ptr<Object>> s_handles;
    static uint32_t s_nextHandle = 0xF8000004;

    uint32_t CreateHandle(std::shared_ptr<Object> obj)
    {
        std::lock_guard lock(s_handleLock);
        uint32_t h = s_nextHandle;
        s_nextHandle += 4;
        s_handles[h] = std::move(obj);
        return h;
    }

    std::shared_ptr<Object> GetHandle(uint32_t handle)
    {
        if (handle == kCurrentThread)
        {
            // Return the current thread object itself.
            auto* t = CurrentThread();
            if (!t) return nullptr;
            std::lock_guard lock(s_handleLock);
            auto it = s_handles.find(t->handle);
            return it == s_handles.end() ? nullptr : it->second;
        }
        std::lock_guard lock(s_handleLock);
        auto it = s_handles.find(handle);
        return it == s_handles.end() ? nullptr : it->second;
    }

    bool CloseHandle(uint32_t handle)
    {
        std::lock_guard lock(s_handleLock);
        return s_handles.erase(handle) != 0;
    }

    uint32_t DuplicateHandle(uint32_t handle)
    {
        auto obj = GetHandle(handle);
        return obj ? CreateHandle(obj) : 0;
    }

    // ------------------------------------------------------------------------
    // Guest dispatcher objects
    // ------------------------------------------------------------------------

    static std::mutex s_guestObjLock;
    static std::unordered_map<uint32_t, std::shared_ptr<Object>> s_guestObjects;

    std::shared_ptr<Object> GetGuestObject(uint32_t addr)
    {
        std::lock_guard lock(s_guestObjLock);
        auto it = s_guestObjects.find(addr);
        if (it != s_guestObjects.end())
            return it->second;

        // DISPATCHER_HEADER: u8 Type, u8 Absolute, u8 Size, u8 Inserted, be32 SignalState, LIST_ENTRY WaitListHead
        uint8_t type = guest::r8(addr);
        int32_t signal = int32_t(guest::r32(addr + 4));
        std::shared_ptr<Object> obj;
        switch (type)
        {
        case 0: obj = std::make_shared<Event>(true, signal != 0); break;   // NotificationEvent
        case 1: obj = std::make_shared<Event>(false, signal != 0); break;  // SynchronizationEvent
        case 2: obj = std::make_shared<Mutant>(); break;                   // MutantObject
        case 5: obj = std::make_shared<Semaphore>(signal, int32_t(guest::r32(addr + 0x10))); break;
        case 6:
            // Thread objects are registered when created; an unknown one is treated as an event.
            obj = std::make_shared<Event>(true, false);
            break;
        case 8: case 9: obj = std::make_shared<Event>(type == 8, signal != 0); break; // timers
        default:
            LOG("GetGuestObject: unknown dispatcher type %u at %08X", type, addr);
            obj = std::make_shared<Event>(false, signal != 0);
            break;
        }
        s_guestObjects[addr] = obj;
        return obj;
    }

    void RegisterGuestObject(uint32_t addr, std::shared_ptr<Object> obj)
    {
        std::lock_guard lock(s_guestObjLock);
        s_guestObjects[addr] = std::move(obj);
    }

    std::shared_ptr<Object> FindGuestObject(uint32_t addr)
    {
        std::lock_guard lock(s_guestObjLock);
        auto it = s_guestObjects.find(addr);
        return it != s_guestObjects.end() ? it->second : nullptr;
    }

    void ForgetGuestObject(uint32_t addr)
    {
        std::lock_guard lock(s_guestObjLock);
        s_guestObjects.erase(addr);
    }

    // ------------------------------------------------------------------------
    // Waiting
    // ------------------------------------------------------------------------

    uint64_t SystemTime()
    {
        // 100 ns intervals since 1601-01-01
        constexpr uint64_t kEpochDiff = 116444736000000000ull;
        auto now = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
        return kEpochDiff + uint64_t(now / 100);
    }

    std::optional<steady_clock::time_point> DeadlineFromGuest(uint32_t timeoutAddr)
    {
        if (!timeoutAddr)
            return std::nullopt;
        int64_t v = int64_t(guest::r64(timeoutAddr));
        if (v < 0)
            return steady_clock::now() + nanoseconds(-v * 100);
        int64_t rel = v - int64_t(SystemTime());
        return steady_clock::now() + nanoseconds(rel > 0 ? rel * 100 : 0);
    }

    uint32_t Wait(const std::vector<std::shared_ptr<Object>>& objects, bool waitAll,
        std::optional<steady_clock::time_point> deadline)
    {
        GuestThread* self = CurrentThread();
        struct WaitMark
        {
            GuestThread* t;
            WaitMark(GuestThread* t) : t(t)
            {
                if (t)
                {
                    t->waitCount++;
                    t->waitingSince = uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()) | 1;
                }
            }
            ~WaitMark() { if (t) t->waitingSince = 0; }
        } mark(self);
        std::unique_lock lock(g_lock);
        for (;;)
        {
            if (waitAll)
            {
                bool all = true;
                for (auto& o : objects)
                    if (!o || !o->IsSignaled(self)) { all = false; break; }
                if (all)
                {
                    for (auto& o : objects) o->OnAcquire(self);
                    return STATUS_WAIT_0;
                }
            }
            else
            {
                for (size_t i = 0; i < objects.size(); i++)
                {
                    if (objects[i] && objects[i]->IsSignaled(self))
                    {
                        objects[i]->OnAcquire(self);
                        return STATUS_WAIT_0 + uint32_t(i);
                    }
                }
            }

            if (deadline)
            {
                if (g_cv.wait_until(lock, *deadline) == std::cv_status::timeout && steady_clock::now() >= *deadline)
                {
                    // one last check happens on the next loop iteration only if we didn't time out
                    return STATUS_TIMEOUT;
                }
            }
            else
            {
                g_cv.wait(lock);
            }
        }
    }

    uint32_t SetEvent(Event& e)
    {
        std::lock_guard lock(g_lock);
        uint32_t prev = e.signaled;
        e.signaled = true;
        g_cv.notify_all();
        return prev;
    }

    uint32_t ResetEvent(Event& e)
    {
        std::lock_guard lock(g_lock);
        uint32_t prev = e.signaled;
        e.signaled = false;
        return prev;
    }

    uint32_t PulseEvent(Event& e)
    {
        // Approximation: set, wake everyone, then reset.
        uint32_t prev;
        {
            std::lock_guard lock(g_lock);
            prev = e.signaled;
            e.signaled = true;
            g_cv.notify_all();
        }
        std::this_thread::yield();
        {
            std::lock_guard lock(g_lock);
            e.signaled = false;
        }
        return prev;
    }

    int32_t ReleaseSemaphore(Semaphore& s, int32_t count, uint32_t* status)
    {
        std::lock_guard lock(g_lock);
        int32_t prev = s.count;
        if (s.count + count > s.limit)
        {
            if (status) *status = STATUS_SEMAPHORE_LIMIT_EXCEEDED;
            return prev;
        }
        s.count += count;
        g_cv.notify_all();
        if (status) *status = STATUS_SUCCESS;
        return prev;
    }

    int32_t ReleaseMutant(Mutant& m, GuestThread* t, uint32_t* status)
    {
        std::lock_guard lock(g_lock);
        if (m.owner != t)
        {
            if (status) *status = STATUS_MUTANT_NOT_OWNED;
            return 0;
        }
        int32_t prev = int32_t(m.recursion);
        if (--m.recursion == 0)
        {
            m.owner = nullptr;
            g_cv.notify_all();
        }
        if (status) *status = STATUS_SUCCESS;
        return prev;
    }

    // ------------------------------------------------------------------------
    // Threads
    // ------------------------------------------------------------------------

    static thread_local GuestThread* t_current = nullptr;
    static std::atomic<uint32_t> s_nextThreadId{ 1 };
    static std::mutex s_threadListLock;
    static std::vector<std::weak_ptr<GuestThread>> s_threads;

    static void TrackThread(const std::shared_ptr<GuestThread>& t)
    {
        std::lock_guard lock(s_threadListLock);
        s_threads.push_back(t);
    }

    void StartMonitor()
    {
        std::thread([] {
            for (;;)
            {
                std::this_thread::sleep_for(seconds(10));
                uint32_t now = uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
                std::string line;
                std::lock_guard lock(s_threadListLock);
                for (auto& w : s_threads)
                {
                    auto t = w.lock();
                    if (!t || t->exited || !t->ctx) continue;
                    char buf[128];
                    uint32_t since = t->waitingSince;
                    if (since)
                        snprintf(buf, sizeof(buf), " t%u:wait%us(lr=%08X)", t->id, (now - since) / 1000, uint32_t(t->ctx->lr));
                    else
                        snprintf(buf, sizeof(buf), " t%u:run(lr=%08X,waits=%u)", t->id, uint32_t(t->ctx->lr), t->waitCount.load());
                    line += buf;
                }
                size_t handles;
                {
                    std::lock_guard hl(s_handleLock);
                    handles = s_handles.size();
                }
                // Saints Row: active player index (signed byte, -1 = none) and the "player signed out" flag.
                rt::Log("monitor: handles=%zu activeUser=%d signedOutFlag=%u%s", handles,
                    int(int8_t(guest::r8(0x827ADEC2))), guest::r8(0x827D8126), line.c_str());
            }
        }).detach();
    }

    GuestThread* CurrentThread() { return t_current; }

    static void SetupGuestThread(GuestThread& t, uint32_t stackSize)
    {
        stackSize = std::max<uint32_t>((stackSize + 0xFFF) & ~0xFFFu, 0x40000);

        // Stack (grows down)
        uint32_t stack = mem::g_virtual.Alloc(stackSize, 0x1000);
        t.stackLimit = stack;
        t.stackBase = stack + stackSize;

        // TLS: [extended (declspec(thread)) data][slots]
        uint32_t slotBytes = g_tls.slotCount * 4;
        uint32_t total = slotBytes + g_tls.dataSize;
        t.tlsStatic = mem::SystemAllocZeroed(total);
        t.tlsDynamic = t.tlsStatic + g_tls.dataSize;
        if (g_tls.rawDataSize)
            std::memcpy(guest::ptr(t.tlsStatic), guest::ptr(g_tls.rawDataAddress), g_tls.rawDataSize);

        // KTHREAD
        t.kthread = mem::SystemAllocZeroed(0xAB0);
        guest::w8(t.kthread + 0x00, 6); // ThreadObject
        guest::w32(t.kthread + 0x5C, t.stackBase);
        guest::w32(t.kthread + 0x60, t.stackLimit);
        guest::w32(t.kthread + 0x68, t.tlsDynamic);
        guest::w8(t.kthread + 0x70, 8); // priority
        guest::w64(t.kthread + 0x130, SystemTime());
        guest::w32(t.kthread + 0x14C, t.id);
        guest::w32(t.kthread + 0x150, t.startAddress);

        // KPCR (r13)
        t.pcr = mem::SystemAllocZeroed(0x2D8, 0x1000);
        guest::w32(t.pcr + 0x00, t.tlsStatic);
        guest::w32(t.pcr + 0x30, 0);
        guest::w32(t.pcr + 0x34, t.pcr);
        guest::w32(t.pcr + 0x70, t.stackBase);
        guest::w32(t.pcr + 0x74, t.stackLimit);
        guest::w32(t.pcr + 0x100, t.kthread);     // prcb.current_thread
        guest::w8(t.pcr + 0x10C, 0);              // prcb.current_cpu
        guest::w32(t.pcr + 0x2A8, t.pcr + 0x100); // prcb pointer

        // Guest context
        t.ctx = static_cast<PPCContext*>(::operator new(sizeof(PPCContext), std::align_val_t(64)));
        new (t.ctx) PPCContext{};
        t.ctx->r1.u64 = t.stackBase - 0x100; // keep a little red zone for the back chain
        t.ctx->r13.u64 = t.pcr;
        t.ctx->fpscr.loadFromHost();
    }

    static void ThreadEntry(GuestThread* t)
    {
        t_current = t;
        PPCContext& ctx = *t->ctx;
        ctx.fpscr.loadFromHost();

        // Honour CREATE_SUSPENDED
        {
            std::unique_lock lock(g_lock);
            g_cv.wait(lock, [t] { return t->suspendCount <= 0; });
        }

        uint32_t code = 0;
        try
        {
            if (t->xapiStartup)
            {
                ctx.r3.u64 = t->startAddress;
                ctx.r4.u64 = t->startContext;
                GuestCall(ctx, t->xapiStartup);
            }
            else
            {
                ctx.r3.u64 = t->startContext;
                GuestCall(ctx, t->startAddress);
            }
            code = ctx.r3.u32;
        }
        catch (const ThreadExit& e)
        {
            code = e.code;
        }

        LOG("thread %u exited (code %u)", t->id, code);
        std::lock_guard lock(g_lock);
        t->exited = true;
        t->exitCode = code;
        g_cv.notify_all();
    }

    void SetActiveCpu(GuestThread& t, uint8_t cpu)
    {
        t.cpu = cpu;
        guest::w8(t.pcr + 0x10C, cpu);     // prcb.current_cpu
        guest::w8(t.kthread + 0xBF, cpu);  // KTHREAD.current_cpu
    }

    uint8_t CpuForAffinity(uint8_t mask)
    {
        mask &= 0x3F;
        if (!mask)
        {
            // Like the 360: threads without an explicit processor stay on the parent's hardware thread.
            if (auto* parent = CurrentThread()) return parent->cpu;
            static std::atomic<uint8_t> s_next{ 0 };
            return uint8_t(s_next++ % 6);
        }
        uint8_t cpu = 0;
        while (!(mask & 1)) { mask >>= 1; ++cpu; }
        return cpu;
    }

    std::shared_ptr<GuestThread> CreateThread(uint32_t startAddress, uint32_t startContext,
        uint32_t xapiStartup, uint32_t stackSize, uint32_t creationFlags)
    {
        bool suspended = (creationFlags & 1) != 0;
        auto t = std::make_shared<GuestThread>();
        t->id = s_nextThreadId++;
        t->startAddress = startAddress;
        t->startContext = startContext;
        t->xapiStartup = xapiStartup;
        t->suspendCount = suspended ? 1 : 0;
        SetupGuestThread(*t, stackSize);
        SetActiveCpu(*t, CpuForAffinity(uint8_t(creationFlags >> 24)));
        t->handle = CreateHandle(t);
        RegisterGuestObject(t->kthread, t);
        TrackThread(t);

        LOG("thread %u (cpu %u) created: start=%08X ctx=%08X xapi=%08X stack=%X%s", t->id, t->cpu, startAddress, startContext,
            xapiStartup, stackSize, suspended ? " (suspended)" : "");

        t->host = std::thread(ThreadEntry, t.get());
        t->host.detach();
        return t;
    }

    void RunMainThread(uint32_t entryPoint, uint32_t stackSize)
    {
        auto t = std::make_shared<GuestThread>();
        t->id = s_nextThreadId++;
        t->startAddress = entryPoint;
        SetupGuestThread(*t, stackSize);
        t->handle = CreateHandle(t);
        RegisterGuestObject(t->kthread, t);
        TrackThread(t);
        StartMonitor();
        LOG("main thread: entry=%08X stack=%08X-%08X pcr=%08X", entryPoint, t->stackLimit, t->stackBase, t->pcr);
        ThreadEntry(t.get());
    }

    std::shared_ptr<GuestThread> CreateHostContext(const char* name)
    {
        auto t = std::make_shared<GuestThread>();
        t->id = s_nextThreadId++;
        SetupGuestThread(*t, 0x40000);
        t->handle = CreateHandle(t);
        RegisterGuestObject(t->kthread, t);
        LOG("host context %u: %s", t->id, name);
        return t;
    }

    void BindCurrentThread(GuestThread* t)
    {
        t_current = t;
        if (t && t->ctx) t->ctx->fpscr.loadFromHost();
    }

    void ResumeThread(GuestThread& t)
    {
        std::lock_guard lock(g_lock);
        if (t.suspendCount > 0) --t.suspendCount;
        g_cv.notify_all();
    }

    void SuspendThread(GuestThread& t)
    {
        // Only supported before the thread starts running (CREATE_SUSPENDED style).
        std::lock_guard lock(g_lock);
        ++t.suspendCount;
    }

    // ------------------------------------------------------------------------
    // KeTimeStampBundle updater
    // ------------------------------------------------------------------------

    void StartTimeStampThread(uint32_t bundleAddr)
    {
        std::thread([bundleAddr] {
            auto start = steady_clock::now();
            for (;;)
            {
                auto elapsed = steady_clock::now() - start;
                uint64_t interrupt = uint64_t(duration_cast<nanoseconds>(elapsed).count() / 100);
                guest::w64(bundleAddr + 0x00, interrupt);           // InterruptTime
                guest::w64(bundleAddr + 0x08, SystemTime());        // SystemTime
                guest::w32(bundleAddr + 0x10, uint32_t(duration_cast<milliseconds>(elapsed).count())); // TickCount
                std::this_thread::sleep_for(milliseconds(1));
            }
        }).detach();
    }
}

namespace rt
{
    uint32_t CurrentThreadId()
    {
        auto* t = kernel::CurrentThread();
        return t ? t->id : 0;
    }
}
