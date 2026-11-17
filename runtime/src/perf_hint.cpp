// CPU clock boost for the game's threads (Android Performance Hint API, Android 13+).
//
// The game spreads its frame over the main thread and a pool of job threads, none of them busy
// enough on its own for the CPU governor to raise the clock: measured in the city the threads ran
// at ~1.4-1.6 GHz on a Snapdragon 8 Elite that can do 4.3 GHz. A hint session over those threads,
// told each frame how long the frame took against a 60 fps target, lets the system raise the clock
// for them. Other platforms: no-op.
#include "perf_hint.h"

#if defined(__ANDROID__)
#include "guest.h"

#include <android/log.h>
#include <sched.h>
#include <string>
#include <dirent.h>
#include <dlfcn.h>
#include <cerrno>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace perf
{
    namespace
    {
        struct APerformanceHintManager;
        struct APerformanceHintSession;
        using GetManagerFn = APerformanceHintManager* (*)();
        using CreateSessionFn = APerformanceHintSession* (*)(APerformanceHintManager*, const int32_t*, size_t, int64_t);
        using ReportFn = int (*)(APerformanceHintSession*, int64_t);
        using SetThreadsFn = int (*)(APerformanceHintSession*, const int32_t*, size_t);

        constexpr int64_t kTargetNs = 16'666'667; // 60 fps

        std::mutex s_lock;
        std::vector<int32_t> s_tids;
        std::string s_configDir;
        int32_t s_mainTid = 0;
        bool s_pinned = false;

        bool SwitchFile(const char* name)
        {
            return !s_configDir.empty() && access((s_configDir + "/" + name).c_str(), F_OK) == 0;
        }

        // The fastest cores: those with the highest maximum frequency (on the 8 Elite, 6 and 7).
        cpu_set_t FastCores()
        {
            cpu_set_t set;
            CPU_ZERO(&set);
            long best = 0;
            long freq[16] = {};
            for (int c = 0; c < 16; c++)
            {
                char path[96];
                snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
                if (FILE* f = fopen(path, "r"))
                {
                    if (fscanf(f, "%ld", &freq[c]) != 1) freq[c] = 0;
                    fclose(f);
                }
                if (freq[c] > best) best = freq[c];
            }
            for (int c = 0; c < 16; c++)
                if (best && freq[c] == best) CPU_SET(c, &set);
            return set;
        }

        // Keep the game's main thread and the GPU command thread on the fastest cores: they are the
        // frame's critical path. Odin 3 in the city: 55 -> 59 fps average, no dips below 57.
        // Switch file "no_pin_fast_cores" turns it off.
        void PinCriticalThreads(const std::vector<int32_t>& gpuTids)
        {
            if (s_pinned || !s_mainTid || SwitchFile("no_pin_fast_cores")) return;
            s_pinned = true;
            cpu_set_t set = FastCores();
            int ok = sched_setaffinity(s_mainTid, sizeof(set), &set) == 0;
            for (int32_t tid : gpuTids) ok += sched_setaffinity(tid, sizeof(set), &set) == 0;
            LOG("perf: pinned main + GPU threads to %d fast cores (%d ok)", CPU_COUNT(&set), ok);
        }
        bool s_tidsChanged = false;

        bool s_loaded = false;
        CreateSessionFn s_create = nullptr;
        ReportFn s_report = nullptr;
        SetThreadsFn s_setThreads = nullptr;
        APerformanceHintManager* s_manager = nullptr;
        APerformanceHintSession* s_session = nullptr;

        void Load()
        {
            s_loaded = true;
            void* lib = dlopen("libandroid.so", RTLD_NOW);
            if (!lib) return;
            auto getManager = reinterpret_cast<GetManagerFn>(dlsym(lib, "APerformanceHint_getManager"));
            s_create = reinterpret_cast<CreateSessionFn>(dlsym(lib, "APerformanceHint_createSession"));
            s_report = reinterpret_cast<ReportFn>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
            s_setThreads = reinterpret_cast<SetThreadsFn>(dlsym(lib, "APerformanceHint_setThreads"));
            if (getManager && s_create && s_report) s_manager = getManager();
            LOG("perf hint: %s", s_manager ? "available" : "not supported on this device");
        }

        // Host threads doing per-frame work outside the guest: the GPU command processor.
        void AddNamedThreads(std::vector<int32_t>& tids)
        {
            DIR* dir = opendir("/proc/self/task");
            if (!dir) return;
            while (dirent* e = readdir(dir))
            {
                int tid = atoi(e->d_name);
                if (tid <= 0) continue;
                char path[64], name[32] = {};
                snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
                if (FILE* f = fopen(path, "r"))
                {
                    if (fgets(name, sizeof(name), f) && strncmp(name, "GPU Commands", 12) == 0)
                        tids.push_back(tid);
                    fclose(f);
                }
            }
            closedir(dir);
        }
    }

    bool Switch(const char* name)
    {
        std::lock_guard lock(s_lock);
        return SwitchFile(name);
    }

    void SetConfigDir(const char* dir)
    {
        std::lock_guard lock(s_lock);
        s_configDir = dir ? dir : "";
    }

    void RegisterCurrentThread(unsigned guestId)
    {
        std::lock_guard lock(s_lock);
        if (guestId == 1) s_mainTid = int32_t(gettid());
        s_tids.push_back(int32_t(gettid()));
        s_tidsChanged = true;
    }

    void OnFrame()
    {
        using clock = std::chrono::steady_clock;
        static clock::time_point s_last;
        static int s_frames = 0;
        const auto now = clock::now();
        const int64_t frameNs = s_last == clock::time_point{} ? 0
            : std::chrono::duration_cast<std::chrono::nanoseconds>(now - s_last).count();
        s_last = now;

        std::lock_guard lock(s_lock);
        if (!s_loaded) Load();
        if (!s_manager) return;

        // (Re)build the thread list now and then: job threads start late, the GPU thread is found
        // by name.
        if (s_tidsChanged && (++s_frames % 60) == 0)
        {
            // Only threads that still run: a tid that exited makes the call fail.
            std::vector<int32_t> tids;
            for (int32_t tid : s_tids)
            {
                char path[64];
                snprintf(path, sizeof(path), "/proc/self/task/%d", tid);
                if (access(path, F_OK) == 0) tids.push_back(tid);
            }
            s_tids = tids;
            std::vector<int32_t> gpu;
            AddNamedThreads(gpu);
            PinCriticalThreads(gpu);
            tids.insert(tids.end(), gpu.begin(), gpu.end());
            if (!s_session)
            {
                s_session = s_create(s_manager, tids.data(), tids.size(), kTargetNs);
                LOG("perf hint: session %s for %zu threads (errno %d)", s_session ? "created" : "FAILED", tids.size(),
                    s_session ? 0 : errno);
            }
            else if (s_setThreads)
            {
                s_setThreads(s_session, tids.data(), tids.size());
            }
            s_tidsChanged = false;
        }
        // Frame time against the target: longer frames ask for more clock. Pauses (loading screens,
        // a minimised app) are not reported.
        if (s_session && frameNs > 0 && frameNs < 500'000'000) s_report(s_session, frameNs);
    }
}

#else

namespace perf
{
    void SetConfigDir(const char*) {}
    bool Switch(const char*) { return false; }
    void RegisterCurrentThread(unsigned) {}
    void OnFrame() {}
}

#endif
