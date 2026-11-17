// xglue, Android platform half: ANativeWindow-backed xe::ui::Window + VulkanGraphicsSystem.
// The app context / window follow the pattern of Xendroid+'s xendroid_emu.cpp (event-driven UI loop,
// never blocking the GPU present thread).
#include "xglue.h"
#include "xglue_internal.h"

#include <android/log.h>
#include <android/native_window.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/ui/surface_android.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

DECLARE_string(vulkan_lib_path);

extern std::string g_native_lib_dir;

namespace xglue
{
    static std::mutex s_windowLock;
    static ANativeWindow* s_nativeWindow = nullptr;

    // ------------------------------------------------------------------------
    // App context: a single UI thread that runs queued functions and paints.
    // ------------------------------------------------------------------------
    class AppContext final : public xe::ui::WindowedAppContext
    {
    public:
        static constexpr uint32_t kExecute = 1u << 0, kQuit = 1u << 1, kPaint = 1u << 2, kForcePaint = 1u << 3;

        void BindUiThread() { ui_thread_id_ = std::this_thread::get_id(); }

        void NotifyUILoopOfPendingFunctions() override
        {
            // Called from non-UI threads; keeps CallInUIThread synchronous like Xendroid.
            std::unique_lock lock(mutex_);
            uint64_t ticket = ++execRequested_;
            pending_ |= kExecute;
            cv_.wait(lock, [&] { return execCompleted_ >= ticket || exited_; });
        }

        void PlatformQuitFromUIThread() override
        {
            std::lock_guard lock(mutex_);
            pending_ |= kQuit;
        }

        void RequestPaint(bool force)
        {
            // Fire-and-forget: the GPU present thread calls this while holding presenter locks.
            std::lock_guard lock(mutex_);
            pending_ |= kPaint | (force ? kForcePaint : 0);
        }

        void RequestQuitFromAnyThread()
        {
            std::lock_guard lock(mutex_);
            pending_ |= kQuit;
        }

        void Loop(xe::ui::Window* window);

    private:
        std::mutex mutex_;
        std::condition_variable cv_;
        uint32_t pending_ = 0;
        uint64_t execRequested_ = 0, execCompleted_ = 0;
        bool exited_ = false;
    };

    class AndroidWindow final : public xe::ui::Window
    {
    public:
        AndroidWindow(xe::ui::WindowedAppContext& app, std::string_view title, uint32_t w, uint32_t h)
            : Window(app, title, w, h) {}

        void Paint(bool force) { OnPaint(force); }
        void SurfaceChanged(bool attach) { OnSurfaceChanged(attach); }

    protected:
        bool OpenImpl() override { return true; }
        void RequestCloseImpl() override {}
        void RequestPaintImpl() override { static_cast<AppContext&>(app_context()).RequestPaint(false); }

        std::unique_ptr<xe::ui::Surface> CreateSurfaceImpl(xe::ui::Surface::TypeFlags allowed) override
        {
            if (!(allowed & xe::ui::Surface::kTypeFlag_AndroidNativeWindow)) return nullptr;
            std::lock_guard lock(s_windowLock);
            if (!s_nativeWindow) return nullptr;
            return std::make_unique<xe::ui::AndroidNativeWindowSurface>(s_nativeWindow);
        }
    };

    void AppContext::Loop(xe::ui::Window* window)
    {
        auto* win = static_cast<AndroidWindow*>(window);
        while (!HasQuitFromUIThread())
        {
            uint32_t ev;
            uint64_t execTo;
            {
                std::lock_guard lock(mutex_);
                ev = pending_;
                pending_ &= ~(kPaint | kForcePaint | kExecute);
                execTo = execRequested_;
            }
            if (!ev)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (ev & kExecute)
            {
                ExecutePendingFunctionsFromUIThread();
                std::lock_guard lock(mutex_);
                execCompleted_ = execTo;
                cv_.notify_all();
            }
            if ((ev & kPaint) && win) win->Paint((ev & kForcePaint) != 0);
            if (ev & kQuit)
            {
                QuitFromUIThread();
                break;
            }
        }
        std::lock_guard lock(mutex_);
        exited_ = true;
        execCompleted_ = execRequested_;
        cv_.notify_all();
    }

    static std::unique_ptr<AppContext> s_app;
    static std::unique_ptr<AndroidWindow> s_window;
    static std::unique_ptr<xe::gpu::vulkan::VulkanGraphicsSystem> s_gpu;

    void SetNativeWindow(void* nativeWindow)
    {
        std::lock_guard lock(s_windowLock);
        if (s_nativeWindow) ANativeWindow_release(s_nativeWindow);
        s_nativeWindow = static_cast<ANativeWindow*>(nativeWindow);
        if (s_nativeWindow) ANativeWindow_acquire(s_nativeWindow);
    }

    // Optional GPU settings file (TOML, `name = value` per line, Xenia cvar names) so settings can
    // be tried without rebuilding: <userdata>/gpu.toml.
    static std::string s_gpuConfigFile;
    void SetGpuConfigFile(const char* path) { s_gpuConfigFile = path ? path : ""; }

    void SetCustomDriver(const char* nativeLibDir, const char* driverLibrary)
    {
        g_native_lib_dir = nativeLibDir ? nativeLibDir : "";
        cvars::vulkan_lib_path = driverLibrary ? driverLibrary : "";
        __android_log_print(ANDROID_LOG_INFO, "SaintsRowGPU", "Vulkan driver: %s",
            cvars::vulkan_lib_path.empty() ? "system" : cvars::vulkan_lib_path.c_str());
    }

    static void ApplyGpuConfig()
    {
        if (s_gpuConfigFile.empty() || !cvar::ConfigVars) return;
        toml::table parsed;
        try { parsed = toml::parse_file(s_gpuConfigFile); }
        catch (const std::exception&)
        {
            __android_log_print(ANDROID_LOG_INFO, "SaintsRowGPU", "no usable %s", s_gpuConfigFile.c_str());
            return;
        }
        for (auto&& [key, value] : parsed)
        {
            std::string name(key.str());
            auto it = cvar::ConfigVars->find(name);
            if (it == cvar::ConfigVars->end())
            {
                __android_log_print(ANDROID_LOG_WARN, "SaintsRowGPU", "gpu.toml: unknown setting %s", name.c_str());
                continue;
            }
            it->second->LoadConfigValue(&value);
            __android_log_print(ANDROID_LOG_INFO, "SaintsRowGPU", "gpu.toml: %s applied", name.c_str());
        }
    }

    bool InitGraphics(InterruptHook hook)
    {
        s_app = std::make_unique<AppContext>();
        s_app->BindUiThread();
        s_window = std::make_unique<AndroidWindow>(*s_app, "Saints Row", 1280, 720);
        if (!s_window->Open())
            return false;

        ApplyGpuConfig();

        s_gpu = std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
        s_gpu->SetHostMode(XMemory(), std::move(hook));
        if (s_gpu->Setup(nullptr, nullptr, s_app.get(), true) != 0)
        {
            XELOGE("xglue: graphics system setup failed");
            return false;
        }
        s_window->SetPresenter(s_gpu->presenter());
        s_window->SurfaceChanged(true);
        g_gpu = s_gpu.get();
        return true;
    }

    int RunUiLoop()
    {
        s_app->Loop(s_window.get());
        return 0;
    }

    void RequestQuit()
    {
        if (s_app) s_app->RequestQuitFromAnyThread();
    }

    void OnSurfaceChanged()
    {
        if (s_app && s_window) s_app->CallInUIThread([] { s_window->SurfaceChanged(true); });
    }

    void OnSurfaceDestroyed()
    {
        if (s_app && s_window) s_app->CallInUIThreadSynchronous([] { s_window->SurfaceChanged(false); });
        SetNativeWindow(nullptr);
    }
}
